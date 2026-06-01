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
from matplotlib.widgets import Button

# ---------------------------------------------------------------------------
# Datei finden
# ---------------------------------------------------------------------------

def default_log_path():
    if sys.platform == "win32":
        tmp = os.environ.get("TEMP") or os.environ.get("TMP") or "C:\\"
        return os.path.join(tmp, "go_release_align_verbose.csv")
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


def rotate_attack_to_phase0(atk_vals, phi, T):
    """
    Der verbose-Log zeigt attack ab loop_pos (Phase phi).
    Die Korrelation vergleicht aber bei Phase 0 (crossfade_start = n*T).
    Rotation: Phase-0 beginnt bei Offset (T - phi) im atk-Array.
    Da der Attack periodisch ist, können fehlende Samples am Ende
    aus dem Anfang des Arrays geholt werden (modulo T).
    """
    if T == 0 or len(atk_vals) == 0:
        return atk_vals
    n = len(atk_vals)
    offset = T - phi
    rotated = [atk_vals[(offset + i) % T] if (offset + i) % T < n
               else atk_vals[(offset + i) % T - T] if (offset + i) % T - T >= 0
               else 0
               for i in range(n)]
    return rotated


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

class Browser:
    def __init__(self, transitions):
        self.transitions = transitions
        self.idx = 0

        self._normalize = False

        self.fig, self.ax = plt.subplots(figsize=(11, 6))
        plt.subplots_adjust(bottom=0.25, top=0.88)

        # Linien vorab anlegen
        self.line_atk,     = self.ax.plot([], [], color="#2196F3", lw=1.5, label="attack @ loop_pos")
        self.line_legacy,  = self.ax.plot([], [], color="#F44336", lw=1.5, label="release @ legacy", linestyle="--")
        self.line_corr,    = self.ax.plot([], [], color="#4CAF50", lw=1.5, label="release @ corr")
        self.ax.legend(loc="upper right")
        self.ax.set_xlabel("Sample-Offset ab Startposition")
        self.ax.grid(True, alpha=0.3)

        # Buttons
        ax_prev = plt.axes([0.15, 0.05, 0.12, 0.06])
        ax_next = plt.axes([0.73, 0.05, 0.12, 0.06])
        self.btn_prev = Button(ax_prev, "◀ Zurück")
        self.btn_next = Button(ax_next, "Weiter ▶")
        self.btn_prev.on_clicked(self.prev)
        self.btn_next.on_clicked(self.next)

        # Zähler-Text mittig
        self.counter_ax = plt.axes([0.35, 0.05, 0.30, 0.06])
        self.counter_ax.axis("off")
        self.counter_text = self.counter_ax.text(
            0.5, 0.5, "", ha="center", va="center",
            transform=self.counter_ax.transAxes, fontsize=10
        )

        # Normalize toggle button
        ax_norm = plt.axes([0.35, 0.12, 0.30, 0.05])
        self.btn_norm = Button(ax_norm, "Normalisieren: AUS")
        self.btn_norm.on_clicked(self.toggle_norm)

        self.fig.canvas.mpl_connect("key_press_event", self.on_key)
        self.draw()
        plt.show()

    def draw(self):
        meta, data = self.transitions[self.idx]

        def maybe_norm(v):
            if not self._normalize or len(v) == 0:
                return v
            peak = np.max(np.abs(v))
            return v / peak if peak > 0 else v

        xi, xv = to_arrays(data["atk"])
        self.line_atk.set_data(xi, maybe_norm(xv))

        li, lv = to_arrays(data["rel_leg"])
        self.line_legacy.set_data(li, maybe_norm(lv))

        ci, cv = to_arrays(data["rel_corr"])
        self.line_corr.set_data(ci, maybe_norm(cv))

        self.ax.relim()
        self.ax.autoscale_view()
        self.ax.set_ylabel("Amplitude (normiert)" if self._normalize else "Amplitude (raw)")

        T_str   = meta.get("T", "?")
        phi_str = meta.get("phi", "?")
        lp    = meta.get("loop_pos", "?")
        leg   = meta.get("legacy", "?")
        corr  = meta.get("corr", "?")
        cdiff = meta.get("circ_diff", "?")

        self.fig.suptitle(
            f"loop_pos={lp}  phi={phi_str}  T={T_str}  "
            f"legacy={leg}  corr={corr}  circ_diff={cdiff}",
            fontsize=11
        )
        self.counter_text.set_text(
            f"{self.idx + 1} / {len(self.transitions)}"
        )
        self.fig.canvas.draw_idle()

    def toggle_norm(self, _event=None):
        self._normalize = not self._normalize
        self.btn_norm.label.set_text(
            "Normalisieren: EIN" if self._normalize else "Normalisieren: AUS")
        self.draw()

    def prev(self, _event=None):
        if self.idx > 0:
            self.idx -= 1
            self.draw()

    def next(self, _event=None):
        if self.idx < len(self.transitions) - 1:
            self.idx += 1
            self.draw()

    def on_key(self, event):
        if event.key in ("right", "n"):
            self.next()
        elif event.key in ("left", "p"):
            self.prev()
        elif event.key == "z":
            self.toggle_norm()


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
