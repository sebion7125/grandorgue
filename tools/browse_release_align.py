#!/usr/bin/env python3
"""
Browse go_release_align_verbose.csv — interaktiver Plot.
Zeigt pro Transition: Attack-Tail, Release bei Legacy-Position,
Release bei Corr-Position. Navigation mit Buttons oder Pfeiltasten.

Aufruf:
  python3 browse_release_align.py [pfad/zur/datei.csv]
"""

import os, re, sys
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.widgets import Button, CheckButtons

# ---------------------------------------------------------------------------
# Datei finden
# ---------------------------------------------------------------------------

def default_log_path():
    if sys.platform == "win32":
        tmp = os.environ.get("TEMP") or os.environ.get("TMP") or "C:\\"
        return os.path.join(tmp, "go_release_align_verbose.csv")
    # Shared-folder path (Windows TEMP mapped via VirtualBox)
    sf_path = "/media/sf_Temp/go_release_align_verbose.csv"
    if os.path.isfile(sf_path):
        return sf_path
    return "/tmp/go_release_align_verbose.csv"


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

def parse_log(path):
    """Gibt Liste von (meta_dict, data_dict) zurück.
    meta_dict: loop_pos, phi, T, legacy, corr, circ_diff
    data_dict: {'atk': [(i,v),...], 'rel_leg': [...], 'rel_corr': [...]}
    """
    transitions = []
    meta = None
    data = None

    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip()
            if line.startswith("# vtrans"):
                if meta is not None:
                    transitions.append((meta, data))
                meta = {}
                for m in re.finditer(r"(\w+)=(-?\d+)", line):
                    meta[m.group(1)] = int(m.group(2))
                pm = re.search(r"\bpipe=(.+)$", line)
                if pm:
                    meta["pipe"] = pm.group(1)
                data = {"atk": [], "rel_leg": [], "rel_corr": []}
            elif line and not line.startswith("#"):
                parts = line.split(",")
                if len(parts) == 4:
                    typ, _, i, val = parts[0], parts[1], int(parts[2]), int(parts[3])
                    if typ in data:
                        data[typ].append((i, val))

    if meta is not None:
        transitions.append((meta, data))

    return transitions


def to_arrays(pairs):
    if not pairs:
        return np.array([]), np.array([])
    pairs_sorted = sorted(pairs, key=lambda x: x[0])
    idx = np.array([p[0] for p in pairs_sorted])
    val = np.array([p[1] for p in pairs_sorted])
    return idx, val



# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

class Browser:
    def __init__(self, transitions):
        self.transitions = transitions
        self.idx = 0

        self._normalize  = False
        self._zoom_xlim  = None   # None = autoscale
        self._zoom_ylim  = None
        self._visible    = [True, True, True]  # atk, legacy, corr

        self.fig, self.ax = plt.subplots(figsize=(11, 6))
        plt.subplots_adjust(bottom=0.28, top=0.88, right=0.82)

        # Linien vorab anlegen
        self.line_atk,    = self.ax.plot([], [], color="#2196F3", lw=1.5, label="attack")
        self.line_legacy, = self.ax.plot([], [], color="#F44336", lw=1.5, label="legacy", linestyle="--")
        self.line_corr,   = self.ax.plot([], [], color="#4CAF50", lw=1.5, label="corr")
        self._lines = [self.line_atk, self.line_legacy, self.line_corr]
        self.ax.legend(loc="upper right")
        self.ax.set_xlabel("Sample-Offset  (Scroll=X-Zoom, Shift+Scroll=Y-Zoom)")
        self.ax.grid(True, alpha=0.3)

        # Checkboxen zum Ein-/Ausblenden (rechts neben dem Plot)
        ax_chk = self.fig.add_axes([0.84, 0.55, 0.14, 0.20])
        self.chk = CheckButtons(
            ax_chk,
            ["attack", "legacy", "corr"],
            self._visible,
        )
        # Farben der Häkchen passend zu den Linien
        for rect, color in zip(
            ax_chk.patches, ["#2196F3", "#F44336", "#4CAF50"]
        ):
            rect.set_facecolor(color)
        self.chk.on_clicked(self.toggle_line)

        # Navigations-Buttons (untere Leiste)
        ax_prev  = plt.axes([0.08, 0.06, 0.11, 0.06])
        ax_next  = plt.axes([0.63, 0.06, 0.11, 0.06])
        ax_reset = plt.axes([0.76, 0.06, 0.13, 0.06])
        self.btn_prev  = Button(ax_prev,  "◀ Zurück")
        self.btn_next  = Button(ax_next,  "Weiter ▶")
        self.btn_reset = Button(ax_reset, "Zoom Reset [r]")
        self.btn_prev.on_clicked(self.prev)
        self.btn_next.on_clicked(self.next)
        self.btn_reset.on_clicked(self.reset_zoom)

        # Zähler-Text
        self.counter_ax = plt.axes([0.22, 0.06, 0.22, 0.06])
        self.counter_ax.axis("off")
        self.counter_text = self.counter_ax.text(
            0.5, 0.5, "", ha="center", va="center",
            transform=self.counter_ax.transAxes, fontsize=10
        )

        # Normalize toggle button
        ax_norm = plt.axes([0.22, 0.14, 0.30, 0.05])
        self.btn_norm = Button(ax_norm, "Normalisieren: AUS")
        self.btn_norm.on_clicked(self.toggle_norm)

        self.fig.canvas.mpl_connect("key_press_event", self.on_key)
        self.fig.canvas.mpl_connect("scroll_event",    self.on_scroll)
        self.draw()
        plt.show()

    def draw(self):
        meta, data = self.transitions[self.idx]

        T       = int(meta.get("T", 512))
        xfade   = int(meta.get("xfade", 0))

        def maybe_norm(v):
            if not self._normalize or len(v) == 0:
                return v
            peak = np.max(np.abs(v))
            return v / peak if peak > 0 else v

        xi, xv = to_arrays(data["atk"])
        self.line_atk.set_data(xi, maybe_norm(xv))
        self.line_atk.set_visible(self._visible[0])

        li, lv = to_arrays(data["rel_leg"])
        self.line_legacy.set_data(li, maybe_norm(lv))
        self.line_legacy.set_visible(self._visible[1])

        ci, cv = to_arrays(data["rel_corr"])
        self.line_corr.set_data(ci, maybe_norm(cv))
        self.line_corr.set_visible(self._visible[2])

        if self._zoom_xlim is None:
            self.ax.relim()
            self.ax.autoscale_view()
            # Constrain x to 5 periods; user can scroll/zoom freely after this.
            self.ax.set_xlim(0, 5 * T)
        else:
            self.ax.set_xlim(self._zoom_xlim)
            self.ax.set_ylim(self._zoom_ylim)

        self.ax.set_ylabel("Amplitude (normiert)" if self._normalize else "Amplitude (raw)")

        # NDP score: attack vs. corr-release over crossfade window
        N = min(xfade or (5 * T), len(xv), len(cv))
        if N > 4 and len(xv) > 0 and len(cv) > 0:
            a = xv[:N].astype(float)
            b = cv[:N].astype(float)
            denom = np.linalg.norm(a) * np.linalg.norm(b)
            ndp = np.dot(a, b) / denom if denom > 0.0 else 0.0
            ndp_str = f"NDP={ndp:.4f}"
        else:
            ndp_str = "NDP=n/a"

        T_str   = meta.get("T", "?")
        phi_str = meta.get("phi", "?")
        lp    = meta.get("loop_pos", "?")
        leg   = meta.get("legacy", "?")
        corr  = meta.get("corr", "?")
        cdiff    = meta.get("circ_diff", "?")
        pipe     = meta.get("pipe", "")
        atk_len  = meta.get("atk_len", "")
        atk_sr   = meta.get("atk_sr", "")
        xfade_str = f"  xfade={xfade}" if xfade else ""

        title_line1 = f"{pipe}  " if pipe else ""
        title_line1 += (f"loop_pos={lp}  phi={phi_str}  T={T_str}{xfade_str}  "
                        f"legacy={leg}  corr={corr}  circ_diff={cdiff}  {ndp_str}")
        if atk_len or atk_sr:
            title_line1 += f"  atk:{atk_len}smp@{atk_sr}Hz"
        self.fig.suptitle(title_line1, fontsize=11)
        self.counter_text.set_text(
            f"{self.idx + 1} / {len(self.transitions)}"
        )
        self.fig.canvas.draw_idle()

    def on_scroll(self, event):
        if event.inaxes != self.ax:
            return
        factor = 0.75 if event.button == "up" else 1.33
        xc, yc = event.xdata, event.ydata
        xl, xr = self.ax.get_xlim()
        yb, yt = self.ax.get_ylim()
        if event.key == "shift":          # Shift+Scroll → nur Y
            self._zoom_ylim = (yc - (yc - yb) * factor,
                               yc + (yt - yc) * factor)
            self._zoom_xlim = self._zoom_xlim or (xl, xr)
        else:                             # Scroll → nur X
            self._zoom_xlim = (xc - (xc - xl) * factor,
                               xc + (xr - xc) * factor)
            self._zoom_ylim = self._zoom_ylim or (yb, yt)
        self.ax.set_xlim(self._zoom_xlim)
        self.ax.set_ylim(self._zoom_ylim)
        self.fig.canvas.draw_idle()

    def toggle_line(self, label):
        mapping = {"attack": 0, "legacy": 1, "corr": 2}
        i = mapping[label]
        self._visible[i] = not self._visible[i]
        self._lines[i].set_visible(self._visible[i])
        if self._zoom_xlim is None:
            self.ax.relim()
            self.ax.autoscale_view()
        self.fig.canvas.draw_idle()

    def reset_zoom(self, _event=None):
        self._zoom_xlim = None
        self._zoom_ylim = None
        self.draw()

    def toggle_norm(self, _event=None):
        self._normalize = not self._normalize
        self.btn_norm.label.set_text(
            "Normalisieren: EIN" if self._normalize else "Normalisieren: AUS")
        self.draw()

    def prev(self, _event=None):
        if self.idx > 0:
            self.idx -= 1
            self._zoom_xlim = None
            self._zoom_ylim = None
            self.draw()

    def next(self, _event=None):
        if self.idx < len(self.transitions) - 1:
            self.idx += 1
            self._zoom_xlim = None
            self._zoom_ylim = None
            self.draw()

    def on_key(self, event):
        if event.key in ("right", "n"):
            self.next()
        elif event.key in ("left", "p"):
            self.prev()
        elif event.key == "z":
            self.toggle_norm()
        elif event.key == "r":
            self.reset_zoom()


# ---------------------------------------------------------------------------
# Einstiegspunkt
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else default_log_path()

    if not os.path.isfile(path):
        print(f"Datei nicht gefunden: {path}")
        sys.exit(1)

    transitions = parse_log(path)
    if not transitions:
        print(f"Keine Einträge in {path}")
        sys.exit(1)

    print(f"{len(transitions)} Transition(en) geladen aus {path}")
    Browser(transitions)
