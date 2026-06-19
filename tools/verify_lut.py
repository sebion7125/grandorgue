#!/usr/bin/env python3
"""
verify_lut.py  —  Compare GO's LUT output against Python v2 algorithm.

Usage:
  python3 verify_lut.py [go_lut_verify.csv] organ.organ [options]

  go_lut_verify.csv is written by GO during organ load when #define
  GO_LOG_RELEASE_ALIGN_VERIFY is active (or always, when label is non-null).
  Default path: /tmp/go_lut_verify.csv

Options:
  --filter PATTERN   Only process entries whose label contains PATTERN
  --max-pipes N      Stop after N pipes
  --verbose          Print all LUT points, not just diffs
  --sim-only         Skip LUT comparison, only compare simulator outputs

Exit code: 0 if no mismatches, 1 otherwise.
"""

import os, sys, re, math, argparse, types, json, threading, struct
import numpy as _np
import queue as _queue_mod

# ── Real tkinter for GUI (imported BEFORE the mock replaces sys.modules) ─────
_GUI_AVAILABLE = False
try:
    import tkinter as _tk
    import tkinter.ttk as _ttk
    import tkinter.filedialog as _tkfd
    import tkinter.scrolledtext as _tkst
    import tkinter.messagebox as _tkmb
    _GUI_AVAILABLE = True
except ImportError:
    pass

# ── Mock tkinter BEFORE importing analyze_lut ────────────────────────────────
# Two requirements:
# 1. tk.Frame / tk.Tk etc. must be real types (used as base classes in analyze_lut)
# 2. tkinter.mainloop and tkinter.Misc.mainloop must be real functions with
#    .__code__ (matplotlib/cbook inspects them to detect running event loops)

def _noop(*a, **kw): pass   # one real function covers both requirements

class _TkBase:
    """Shared base for all dummy tk widget stubs."""
    def __init__(self, *a, **kw): pass
    def __getattr__(self, n):     return _noop
    mainloop = _noop

class _TkMockModule(types.ModuleType):
    """Module that returns usable types for widget names and _noop for the rest."""
    def __getattr__(self, name):
        if name.startswith('__'):
            raise AttributeError(name)
        obj = type(name, (_TkBase,), {})   # default: a class (for base-class use)
        object.__setattr__(self, name, obj)
        return obj

def _make_tk_mock(name: str) -> _TkMockModule:
    m = _TkMockModule(name)
    # Functions that matplotlib/other code inspects for .__code__
    for fn in ('mainloop', 'quit', 'getboolean', 'getdouble', 'getint',
               'wantobjects', 'NoDefaultRoot'):
        object.__setattr__(m, fn, _noop)
    # Misc class — matplotlib checks Misc.mainloop.__code__ specifically
    object.__setattr__(m, 'Misc', type('Misc', (_TkBase,), {'mainloop': _noop}))
    # String constants
    for attr in ('END','BOTH','X','Y','TOP','BOTTOM','LEFT','RIGHT','FLAT',
                 'RIDGE','SUNKEN','RAISED','GROOVE','NORMAL','DISABLED','ACTIVE',
                 'WORD','CHAR','NONE','CENTER','HORIZONTAL','VERTICAL',
                 'N','S','E','W','NE','NW','SE','SW','TRUE','FALSE',
                 'BROWSE','EXTENDED','MULTIPLE','LAST','CURRENT','SEP'):
        object.__setattr__(m, attr, attr.lower())
    return m

for _mn in ('tkinter', 'tkinter.ttk', 'tkinter.filedialog',
            'tkinter.messagebox', 'tkinter.font', 'tkinter.simpledialog',
            'tkinter.colorchooser'):
    if _mn not in sys.modules:
        sys.modules[_mn] = _make_tk_mock(_mn)

# Prevent analyze_lut from switching matplotlib to TkAgg
try:
    import matplotlib as _mpl
    _mpl.use = lambda *a, **kw: None
except ImportError:
    pass

# ── Import analyze_lut from the same directory ───────────────────────────────
_tools_dir = os.path.dirname(os.path.abspath(__file__))
if _tools_dir not in sys.path:
    sys.path.insert(0, _tools_dir)

try:
    import analyze_lut as _al
except Exception as e:
    print(f"ERROR: cannot import analyze_lut: {e}", file=sys.stderr)
    sys.exit(2)

get_position_for_correlation_cpp = _al.get_position_for_correlation_runtime

# Patch analyze_lut's pruning globally with C++ style (once, before any threads).
# Will be applied after prune_cpp_style is defined below.


# ── Log parser ────────────────────────────────────────────────────────────────

def _parse_float_flex(s: str) -> float:
    """Parse a float in hexfloat (0x…) or decimal format.  Returns 0.0 on error.
    Tolerates leading garbage bytes (e.g. encoding artifacts like Ä before 0x)."""
    try:
        s = s.strip()
        while s and s[0] not in '0123456789+-':
            s = s[1:]
        if s.startswith(("0x", "-0x", "+0x", "0X", "-0X", "+0X")):
            return float.fromhex(s)
        return float(s)
    except (ValueError, OverflowError):
        return 0.0


def parse_verify_log(path: str) -> list:
    """Parse go_lut_verify.csv; return list of pipe-entry dicts."""
    entries  = []
    current  = None

    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line:
                continue

            if line.startswith("pipe="):
                if current:
                    entries.append(current)
                current = {"lut": [], "sim": []}
                # Label may contain spaces; it always ends with |midi=<digits>.
                # Extract it first, then parse the remaining key=value pairs.
                lm = re.match(r'^pipe=(.+\|midi=\d+)\s+(.*)', line)
                label_str = lm.group(1).strip() if lm else ""
                rest      = lm.group(2)         if lm else line[5:]
                kv = {}
                for tok in rest.split():
                    if "=" in tok:
                        k, _, v = tok.partition("=")
                        kv[k] = v
                current["label"]         = label_str
                current["T_float"]       = _parse_float_flex(kv.get("T_float", "0"))
                current["T_int"]         = int(kv.get("T_int", "0"))
                current["crossfade_len"] = int(kv.get("crossfade_len", "0"))
                current["ds"]            = int(kv.get("ds", "1"))
                current["harmonic"]      = int(kv.get("harmonic", "8"))
                current["n_start"]       = int(kv.get("n_start", "1"))
                current["n_end"]         = int(kv.get("n_end", "0"))
                current["min_ms"]        = int(kv.get("min_ms", "0"))
                current["max_ms"]        = int(kv.get("max_ms", "0"))
                current["sample_rate"]   = int(kv.get("sample_rate", "44100"))
                current["loop_len"]      = int(kv.get("loop_len", "0"))
                current["release_len"]   = int(kv.get("release_len", "0"))
                # r_max added in v214: unfolded search range for 2T comparison
                current["r_max"]         = int(kv.get("r_max", "0"))
                # latest_loop_end/loop_count added in v227
                current["latest_loop_end"]  = int(kv.get("latest_loop_end", "0"))
                current["loop_count"]       = int(kv.get("loop_count", "0"))
                current["attack_file"]      = kv.get("attack_file", "")
                # v230j diagnostics
                current["align_version"]    = kv.get("align_version", "")
                current["ndp_silent_score"] = kv.get("ndp_silent_score", "?")
                # v231b+ autocorr diagnostics (hexfloat for exact round-trip)
                current["initial_T"]      = _parse_float_flex(kv.get("initial_T", "0"))
                current["ac_loop_start"]  = int(kv.get("ac_loop_start",  "0"))
                current["loop_end_used"]  = int(kv.get("loop_end_used",  "0"))
                current["ac_offset"]      = int(kv.get("ac_offset",      "0"))
                current["ac_window"]      = int(kv.get("ac_window",      "0"))
                current["ac_min_p"]       = int(kv.get("ac_min_p",       "0"))
                current["ac_max_p"]       = int(kv.get("ac_max_p",       "0"))

            elif line.startswith("lut,") and current is not None:
                parts = line.split(",")
                # lut,n,loop_pos,best_r,approach_up,is_jump
                # best_r: unfolded [0,2T) for v2 (v215+); [0,T) for legacy/exhaustive
                if len(parts) >= 6:
                    current["lut"].append({
                        "n":           int(parts[1]),
                        "loop_pos":    int(parts[2]),
                        "best_r":      int(parts[3]),
                        "approach_up": parts[4].strip() != "0",
                        "is_jump":     parts[5].strip() != "0",
                    })

            elif line.startswith("sim,") and current is not None:
                parts = line.split(",")
                # sim,loop_pos,r_interp
                if len(parts) >= 3:
                    current["sim"].append({
                        "loop_pos": int(parts[1]),
                        "r_interp": int(parts[2]),
                    })

            elif line.startswith("phase0,") and current is not None:
                parts = line.split(",")
                # phase0,beam_id,r_samples,score_hex,n_last
                if len(parts) >= 5:
                    sc_raw = parts[3]
                    sc = float.fromhex(sc_raw) if sc_raw.startswith("0x") or sc_raw.startswith("-0x") else float(sc_raw)
                    current.setdefault("phase0", []).append({
                        "beam_id": int(parts[1]),
                        "r":       int(parts[2]),
                        "score":   sc,
                        "n_last":  int(parts[4]),
                    })

            elif line.startswith("final_beam,") and current is not None:
                parts = line.split(",")
                # Format v230t+: final_beam,beam_id,r_samples,cum_score_hex,cum_score_dec
                # Format older:  final_beam,beam_id,r_samples,cum_score_dec
                if len(parts) >= 4:
                    if len(parts) >= 5:
                        # hex field present → exact float32 round-trip
                        import struct as _struct
                        cum = float.fromhex(parts[3])  # parse hexfloat as float64
                        cum_f32 = _np.float32(cum)
                    else:
                        cum_f32 = _np.float32(float(parts[3]))
                    current.setdefault("final_beams", []).append({
                        "beam_id":   int(parts[1]),
                        "r":         int(parts[2]),
                        "cum_score": float(cum_f32),  # store as exact float32 value
                    })

            elif line.startswith("pre_prune,") and current is not None:
                parts = line.split(",")
                # old: pre_prune,n,r,is_jump,approach_up (5 fields)
                # new: pre_prune,n,r,score,is_jump,approach_up (6 fields)
                if len(parts) >= 6:
                    current.setdefault("pre_prune", []).append({
                        "n":           int(parts[1]),
                        "r":           int(parts[2]),
                        "score":       float(parts[3]),
                        "is_jump":     parts[4].strip() != "0",
                        "approach_up": parts[5].strip() != "0",
                    })
                elif len(parts) >= 5:
                    current.setdefault("pre_prune", []).append({
                        "n":           int(parts[1]),
                        "r":           int(parts[2]),
                        "score":       float("nan"),
                        "is_jump":     parts[3].strip() != "0",
                        "approach_up": parts[4].strip() != "0",
                    })

            elif line.startswith("post_prune,") and current is not None:
                parts = line.split(",")
                # post_prune,n,r,is_jump,approach_up
                if len(parts) >= 5:
                    current.setdefault("post_prune", []).append({
                        "n":          int(parts[1]),
                        "r":          int(parts[2]),
                        "is_jump":    parts[3].strip() != "0",
                        "approach_up": parts[4].strip() != "0",
                    })

            elif line.startswith("phase15_iv,") and current is not None:
                parts = line.split(",")
                # phase15_iv,na,nb,ra,rb,raw_dist,circ_dist,sp_T_raw,triggered
                if len(parts) >= 9:
                    current.setdefault("phase15_ivs", []).append({
                        "na": int(parts[1]), "nb": int(parts[2]),
                        "ra": int(parts[3]), "rb": int(parts[4]),
                        "raw_dist": int(parts[5]), "circ_dist": int(parts[6]),
                        "sp_T_raw": int(parts[7]), "triggered": parts[8].strip() != "0",
                    })

            elif line.startswith("phase15_step,") and current is not None:
                parts = line.split(",")
                # phase15_step,nd,cs_d,exp_pos,best_r,best_score,inserted
                if len(parts) >= 7:
                    current.setdefault("phase15_steps", []).append({
                        "nd": int(parts[1]), "cs_d": int(parts[2]),
                        "exp_pos": int(parts[3]), "best_r": int(parts[4]),
                        "best_sc": float(parts[5]), "inserted": parts[6].strip() != "0",
                    })


    if current:
        entries.append(current)
    return entries


# ── Organ ODF lookup ──────────────────────────────────────────────────────────

# Cache so the ODF is parsed only once per organ path.
_organ_cache: dict = {}

# WAV cache: path → (mono_array, sample_rate).  Avoids re-reading the same
# attack/release file for each of the N release-time variants of a pipe.
import threading
_wav_cache: dict = {}
_wav_cpp_cache: dict = {}  # integer-scale cache for --cpp-numerics numba path
_smpl_cache: dict = {}
_wav_cache_lock = threading.Lock()

def _load_wav_cached(path: str):
    with _wav_cache_lock:
        if path in _wav_cache:
            return _wav_cache[path]
    mono, sr, _, _ = _al.read_wav_mono_float(path)
    result = (mono, sr)
    with _wav_cache_lock:
        _wav_cache[path] = result
    return result

def _load_wav_cpp_cached(path: str):
    """Like _load_wav_cached but returns C++ integer-scale samples (no norm division).
    Used when --cpp-numerics + numba to match C++ float32 NDP arithmetic exactly."""
    with _wav_cache_lock:
        if path in _wav_cpp_cache:
            return _wav_cpp_cache[path]
    mono, sr, _, _ = _al.read_wav_mono_float(path, _cpp_int_scale=True)
    result = (mono, sr)
    with _wav_cache_lock:
        _wav_cpp_cache[path] = result
    return result

def _parse_smpl_cached(path: str):
    with _wav_cache_lock:
        if path in _smpl_cache:
            return _smpl_cache[path]
    smpl = _al.parse_smpl_chunk(path)
    with _wav_cache_lock:
        _smpl_cache[path] = smpl
    return smpl

def _parse_organ_fast(organ_path: str) -> list:
    """Fast ODF parser — no os.path.isfile checks (avoids slow shared-folder I/O).
    Returns same dict structure as analyze_lut.parse_organ_file.
    """
    if organ_path in _organ_cache:
        return _organ_cache[organ_path]

    organ_dir = os.path.dirname(os.path.abspath(organ_path))

    def resolve(rel: str) -> str:
        return os.path.normpath(
            os.path.join(organ_dir, rel.replace("\\", os.sep).lstrip("/\\")))

    with open(organ_path, encoding="utf-8", errors="replace") as f:
        content = f.read()

    # Parse all sections into {section_name: {key: value}}
    sections: dict = {}
    cur_sec = None
    for line in content.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            cur_sec = line[1:-1]
            sections[cur_sec] = {}
        elif "=" in line and cur_sec is not None:
            k, _, v = line.partition("=")
            sections[cur_sec][k.strip()] = v.strip()

    pipes = []
    rank_pat = re.compile(r"^Rank\d+$")
    for sec_name, sec in sections.items():
        if not rank_pat.match(sec_name):
            continue
        rank_name  = sec.get("Name", sec_name)
        global_hn  = int(sec.get("HarmonicNumber", "8"))
        xfade_ms   = int(sec.get("ReleaseCrossfadeLength", "0"))
        first_key  = int(sec.get("FirstMidiNoteNumber",
                         sec.get("FirstAccessibleKeyMIDINoteNumber", "36")))
        pipe_count = int(sec.get("NumberOfLogicalPipes", "0"))
        if pipe_count == 0:
            pipe_count = sum(1 for k in sec if re.match(r"^Pipe\d+$", k))

        for pi in range(1, pipe_count + 1):
            pk = f"Pipe{pi:03d}"
            atk_rel = sec.get(pk)
            if not atk_rel:
                continue
            midi_note = first_key + pi - 1
            harmonic  = int(sec.get(f"{pk}HarmonicNumber", str(global_hn)))

            # Collect all attack variants (main + PipeXXXAttackNNN extras)
            all_attacks = [resolve(atk_rel)]
            for k, v in sec.items():
                if re.match(rf"^{pk}Attack\d+$", k):
                    all_attacks.append(resolve(v))

            # Collect releases
            releases = {}
            for k, v in sec.items():
                m2 = re.match(rf"^{pk}Release(\d+)$", k)
                if m2:
                    releases[int(m2.group(1))] = v

            release_infos = []
            for rel_idx, rel_path in releases.items():
                mkpt = sec.get(f"{pk}Release{rel_idx:03d}MaxKeyPressTime", "")
                if mkpt:
                    max_key_ms: int | None = int(mkpt)
                else:
                    m3 = re.match(r"rel(\d+)", os.path.basename(
                                  os.path.dirname(rel_path.replace("\\","/"))))
                    max_key_ms = int(m3.group(1)) if m3 else None
                if max_key_ms is not None and (max_key_ms < 0 or max_key_ms >= 99999):
                    max_key_ms = None
                release_infos.append((rel_idx, rel_path, max_key_ms))
            release_infos.sort(key=lambda x: x[2] if x[2] is not None else float("inf"))

            prev_max_ms = 0
            for ri, (rel_idx, rel_path, max_key_ms) in enumerate(release_infos):
                # Crossfade priority: release-specific > pipe-level > rank-level > 0
                _rel_xf_key  = f"{pk}Release{rel_idx:03d}ReleaseCrossfadeLength"
                _pipe_xf_key = f"{pk}ReleaseCrossfadeLength"
                if _rel_xf_key in sec:
                    _crossfade_ms  = int(sec[_rel_xf_key])
                    _crossfade_src = _rel_xf_key
                elif _pipe_xf_key in sec:
                    _crossfade_ms  = int(sec[_pipe_xf_key])
                    _crossfade_src = _pipe_xf_key
                else:
                    _crossfade_ms  = xfade_ms
                    _crossfade_src = "ReleaseCrossfadeLength" if xfade_ms else "default"
                pipes.append({
                    "rank_name":          rank_name,
                    "midi_note":          midi_note,
                    "attack_path":        resolve(atk_rel),
                    "attack_paths":       all_attacks,
                    "release_path":       resolve(rel_path),
                    "harmonic_number":    harmonic,
                    "crossfade_len_ms":   _crossfade_ms,
                    "crossfade_len_source": _crossfade_src,
                    "min_key_press_ms":   prev_max_ms,
                    "max_key_press_ms":   max_key_ms,
                    "organ_base":         organ_dir,
                })
                if max_key_ms is not None:
                    prev_max_ms = max_key_ms
                else:
                    break

    _organ_cache[organ_path] = pipes
    return pipes


def find_pipes_for_label(organ_path: str, label: str) -> list:
    """Return matching pipe descs for label (rank_name_lower|midi=key)."""
    return _find_pipes_for_label_cached(organ_path, label,
                                        _parse_organ_fast(organ_path))

def _find_pipes_for_label_cached(organ_path: str, label: str,
                                  pipes: list) -> list:
    """Same as find_pipes_for_label but reuses an already-parsed pipes list."""
    m = re.match(r"^(.+)\|midi=(\d+)$", label)
    if not m:
        return []
    rank_part = m.group(1).strip().lower()
    midi_key  = int(m.group(2))
    return [p for p in pipes
            if p["rank_name"].strip().lower() == rank_part
            and p["midi_note"] == midi_key]


# ── Helpers ───────────────────────────────────────────────────────────────────

def circ_dist(a: int, b: int, T: int) -> int:
    d = abs(int(a) - int(b)) % max(1, T)
    return min(d, max(1, T) - d)




def prune_cpp_style(pts: list, T_int: int) -> list:
    """Douglas-Peucker pruning matching C++ PruneV2 semantics.

    Unlike Python's _prune_lut_points, this collects all deletions in one full
    pass before rebuilding the active set — matching C++ behavior exactly.
    Tolerance: max(2, T/32).  Gap guard: MAX_PRUNE_GAP_N=50 (total span).
    Uses track_r (unfolded) for distance computation.
    """
    if len(pts) <= 2:
        return list(pts)
    tol     = max(2.0, T_int / 32.0)
    MAX_GAP = 50
    ns       = [p.n                               for p in pts]
    track_rs = [float(getattr(p, "track_r", p.best_r)) for p in pts]
    sz      = len(pts)
    active  = [True] * sz

    changed = True
    while changed:
        changed = False
        idx = [i for i in range(sz) if active[i]]
        for k in range(1, len(idx) - 1):
            cur  = idx[k]
            prev = idx[k - 1]
            nxt  = idx[k + 1]
            if pts[cur].is_jump:                              continue
            if pts[nxt].is_jump:                              continue
            if ns[nxt] - ns[prev] > MAX_GAP:                 continue
            if abs(track_rs[cur]  - track_rs[prev]) > T_int / 4.0: continue
            if abs(track_rs[nxt]  - track_rs[cur])  > T_int / 4.0: continue
            n0, r0 = ns[prev], track_rs[prev]
            n1, r1 = ns[nxt],  track_rs[nxt]
            dn = max(1, n1 - n0)
            if all(abs(track_rs[j] - (r0 + (ns[j] - n0) / dn * (r1 - r0))) <= tol
                   for j in range(prev + 1, nxt)):
                active[cur] = False
                changed     = True

    return [pts[i] for i in range(sz) if active[i]]


# prune_cpp_style is kept for reference but NOT applied globally.
# Python uses its own _prune_lut_points (the authoritative reference).
# C++ should be fixed to match Python, not the other way around.


def go_lut_to_lutpoints(go_lut: dict) -> list:
    """Convert parsed GO LUT dict {n: entry} to list of LutPoint for simulator."""
    pts = []
    for n, gp in go_lut.items():
        pt = _al.LutPoint(
            n=n, loop_pos=gp["loop_pos"], best_r=gp["best_r"],
            best_score=1.0, phase="go",
            approach_up=gp["approach_up"], is_jump=gp["is_jump"],
        )
        pts.append(pt)
    pts.sort(key=lambda p: p.n)
    return pts


# ── Free-analyzer T_float path (analyze_pipe replica) ────────────────────────

def _compute_free_analyzer_T(entry: dict, atk_mono, loops: list) -> dict:
    """Reproduce analyze_pipe's T_float path using the first SMPL loop.

    initial_T from log is smpl_T (pre-HN).  We derive hn_T = initial_T * hn/8,
    replicate the window formula, and run estimate_period_by_autocorr().

    Returns dict with 'available' key.  When True, also contains:
      hn_T_float, py_T_float, py_ac_offset, py_ac_window, py_min_p, py_max_p,
      py_loop_start, py_loop_end.
    """
    initial_T   = entry.get("initial_T", 0.0)  # smpl_T from log (hexfloat)
    if initial_T <= 0.0 or not loops:
        return {"available": False}

    harmonic    = entry.get("harmonic", 8)
    sample_rate = entry.get("sample_rate", 44100)

    # Exact replica of analyze_pipe lines 2857–2904:
    hn_T_float = initial_T * harmonic / 8.0
    T_int_init = int(round(hn_T_float))
    min_p      = max(16, int(round(0.5 * T_int_init)))
    max_p      = min(sample_rate // 20, int(round(2.0 * T_int_init)))

    loop_start = loops[0][0]
    loop_end   = loops[0][1]
    loop_len   = loop_end - loop_start + 1
    loop_mid   = loop_start + loop_len // 2
    ac_region  = atk_mono[loop_mid : loop_mid + max_p * 8]
    ac_window  = len(ac_region)

    py_T_float = hn_T_float  # default if autocorr is skipped
    if max_p >= min_p * 2 and T_int_init >= 16 and ac_window >= max_p * 2:
        t_est, _ = _al.estimate_period_by_autocorr(
            ac_region, min_p, max_p, expected_period=hn_T_float)
        py_T_float = float(t_est)

    return {
        "available":    True,
        "hn_T_float":   hn_T_float,
        "py_T_float":   py_T_float,
        "py_ac_offset": loop_mid,
        "py_ac_window": ac_window,
        "py_min_p":     min_p,
        "py_max_p":     max_p,
        "py_loop_start":loop_start,
        "py_loop_end":  loop_end,
    }


# ── Free-analyzer full run (analyze_pipe replica without GO overrides) ────────

def _run_free_analysis(chosen_attack_path: str, pipe_desc: dict,
                       entry: dict, _use_cpp_scale: bool) -> dict:
    """Run the full analyze_pipe-style LUT computation from WAV smpl chunk.

    Unlike compare_entry (which feeds GO's T_float/n_start/n_end back into
    compute_lut_v2), this derives every input independently — same as analyze_lut
    does when run standalone.  The resulting LUT is then compared to GO's LUT to
    reveal whether GO's *input computation* (T_float, crossfade, loop bounds)
    diverges from what the Python analyzer would compute.

    Returns dict with 'available' key; when True also contains:
      free_lut, free_T_float, free_T_int, free_crossfade, smpl_T, free_pts.
    """
    smpl = _parse_smpl_cached(chosen_attack_path)
    if smpl["midi_note"] is None:
        return {"available": False, "reason": "no_smpl"}

    _load_fn = _load_wav_cpp_cached if _use_cpp_scale else _load_wav_cached
    try:
        atk_mono_full, sr = _load_fn(chosen_attack_path)
        rel_mono, _       = _load_fn(pipe_desc["release_path"])
    except Exception as e:
        return {"available": False, "reason": f"wav_error:{e}"}

    # T_float from smpl chunk — exact replica of analyze_pipe
    midi_note  = smpl["midi_note"]
    pitch_frac = smpl["pitch_frac"] / 2**32
    freq_hz    = 440.0 * 2**((midi_note + pitch_frac - 69) / 12)
    smpl_T     = sr / freq_hz
    harmonic   = entry.get("harmonic", 8)
    hn_T       = smpl_T * harmonic / 8.0
    T_float    = hn_T
    T_int      = int(round(T_float))

    loops = smpl["loops"]
    if loops:
        loop_start      = loops[0][0]
        loop_end        = loops[0][1]
        latest_loop_end = max(l[1] for l in loops)
    else:
        loop_start      = 0
        loop_end        = len(atk_mono_full) - 1
        latest_loop_end = loop_end

    # Autocorrelation — exact replica of analyze_pipe
    T_hn_int = T_int
    min_p = max(16, int(round(0.5 * T_hn_int)))
    max_p = min(sr // 20, int(round(2.0 * T_hn_int)))
    if max_p >= min_p * 2 and T_hn_int >= 16:
        loop_len_ac = loop_end - loop_start + 1
        loop_mid    = loop_start + loop_len_ac // 2
        ac_region   = atk_mono_full[loop_mid : loop_mid + max_p * 8]
        if len(ac_region) >= max_p * 2:
            t_est, _ = _al.estimate_period_by_autocorr(
                ac_region, min_p, max_p, expected_period=hn_T)
            T_float = float(t_est)
            T_int   = int(round(T_float))

    # Crossfade — from ODF (already resolved per-release) or GO default formula.
    # Use midi_note from smpl chunk (matches C++ m_MidiKeyNumber), not ODF midi_note.
    xfade_ms         = pipe_desc.get("crossfade_len_ms", 0)
    xfade_src        = pipe_desc.get("crossfade_len_source", "default")
    if xfade_ms > 0:
        crossfade_samples = int(xfade_ms * sr / 1000)
    else:
        auto_ms = _al._go_default_crossfade_ms(midi_note)
        crossfade_samples = int(auto_ms * sr / 1000)
        xfade_src = "go_default"
    if crossfade_samples < 4:
        crossfade_samples = 2 * T_int

    # min/max sample from ODF pipe descriptor
    min_key_ms = pipe_desc.get("min_key_press_ms") or 0
    max_key_ms = pipe_desc.get("max_key_press_ms")
    min_sample = int(min_key_ms * sr / 1000) if min_key_ms else 0
    max_sample = int(max_key_ms * sr / 1000) if max_key_ms is not None else None

    # Truncate attack to latest_loop_end — same as analyze_pipe
    atk_for_lut = atk_mono_full[:latest_loop_end + 1]

    try:
        free_lut, free_meta = _al.compute_lut_v2(
            attack_mono=atk_for_lut,
            release_mono=rel_mono,
            T_float=T_float,
            T_int=T_int,
            crossfade_len_samples=crossfade_samples,
            harmonic_number=harmonic,
            loop_start=loop_start,
            loop_end=loop_end,
            min_sample=min_sample,
            max_sample=max_sample,
            latest_loop_end_sample=latest_loop_end if latest_loop_end > 0 else None,
            # No _n_start_override / _n_end_override — free run
        )
    except Exception as e:
        return {"available": False, "reason": f"compute_error:{e}"}

    return {
        "available":        True,
        "free_lut":         free_lut,
        "free_meta":        free_meta,
        "free_T_float":     T_float,
        "free_T_int":       T_int,
        "free_crossfade":     crossfade_samples,
        "free_xfade_source":  xfade_src,
        "free_min_sample":    min_sample,
        "free_max_sample":    max_sample,
        "free_latest_le":     latest_loop_end,
        "free_loop_start":  loop_start,
        "free_loop_end":    loop_end,
        "smpl_T":           smpl_T,
        "free_pts":         len(free_lut),
    }


# ── Per-entry comparison ──────────────────────────────────────────────────────

def compare_entry(entry: dict, organ_path: str,
                  sim_only: bool = False, verbose: bool = False,
                  free_analysis: bool = False) -> dict:
    label   = entry["label"]
    T_float = entry["T_float"]
    T_int   = entry["T_int"]

    min_ms = entry["min_ms"]
    max_ms = entry["max_ms"]

    def _skip(status):
        return {"label": label, "min_ms": min_ms, "max_ms": max_ms,
                "status": status, "lut_diffs": [], "sim_diffs": [], "simsrc_diffs": []}

    if T_int < 16:
        return _skip("skip_short_T")

    # Find matching releases in ODF
    pipes = find_pipes_for_label(organ_path, label)
    if not pipes:
        return _skip("not_found")

    # Pick the pipe desc matching max_ms
    log_max_ms = entry["max_ms"]
    pipe_desc  = None
    for p in pipes:
        odf_max = p.get("max_key_press_ms")
        if log_max_ms == 0 and odf_max is None:
            pipe_desc = p; break
        if odf_max is not None and odf_max == log_max_ms:
            pipe_desc = p; break
    if pipe_desc is None:
        pipe_desc = pipes[0]

    # Select the correct attack variant by exact filename match (requires GO
    # rebuild that logs attack_file= in the CSV header), falling back to the
    # loop_len+loop_count heuristic for older CSV files without attack_file=.
    log_attack_file = entry.get("attack_file", "")
    chosen_attack_path = pipe_desc["attack_path"]
    if pipe_desc.get("attack_paths") and len(pipe_desc["attack_paths"]) > 1:
        if log_attack_file:
            # Exact basename match — deterministic, no collision risk.
            for ap in pipe_desc["attack_paths"]:
                if os.path.basename(ap) == log_attack_file:
                    chosen_attack_path = ap
                    break
        else:
            # Fallback heuristic for CSV files predating attack_file= logging.
            log_loop_len   = entry["loop_len"]
            log_loop_count = entry.get("loop_count", 0)
            best_ap = None
            for ap in pipe_desc["attack_paths"]:
                try:
                    smpl_ap  = _parse_smpl_cached(ap)
                    loops_ap = smpl_ap.get("loops", [])
                    if not loops_ap:
                        continue
                    le_ap = max(l[1] for l in loops_ap) + 1
                    lc_ap = len(loops_ap)
                    if le_ap == log_loop_len:
                        if lc_ap == log_loop_count or best_ap is None:
                            best_ap = ap
                        if lc_ap == log_loop_count:
                            break
                except Exception:
                    pass
            if best_ap is not None:
                chosen_attack_path = best_ap

    # Load WAVs (cached — same file shared by all release-time variants).
    # When --cpp-numerics + numba: use integer-scale samples to match C++
    # float32 NDP arithmetic exactly (C++ keeps raw integer values, Python
    # normally divides by 2^(bit_depth-1) → different float32 rounding).
    _use_cpp_scale = (_al.get_cpp_numerics()
                      and _al._CPP_NUMERICS_BACKEND == "numba")
    _load_fn = _load_wav_cpp_cached if _use_cpp_scale else _load_wav_cached
    try:
        atk_mono, sr = _load_fn(chosen_attack_path)
        rel_mono, _  = _load_fn(pipe_desc["release_path"])
    except Exception as e:
        return _skip(f"wav_error:{e}")

    # Diagnostic: log WAV lengths vs what C++ used.
    # Stored so compare_entry result can include them for diff output.
    _diag_atk_len = len(atk_mono)
    _diag_rel_len = len(rel_mono)
    _diag_log_atk = entry["loop_len"]
    _diag_log_rel = entry.get("release_len", 0)

    # smpl loop points from attack WAV (cached header read)
    smpl   = _parse_smpl_cached(pipe_desc["attack_path"])
    loops  = smpl.get("loops", [])
    loop_start, loop_end = (loops[0][0], loops[0][1]) if loops else (0, len(atk_mono) - 1)

    # C++ uses loop_section.GetLength() = latest_loop_end + 1 as the attack buffer length.
    # Truncate Python's array to the same length so guards in compute_lut_v2 agree.
    log_loop_len = entry["loop_len"]
    if log_loop_len > 0 and len(atk_mono) != log_loop_len:
        atk_mono = atk_mono[:log_loop_len]
    # After truncation the relevant span is always [0, len-1] regardless of SMPL loop_start.
    loop_start = 0
    loop_end   = len(atk_mono) - 1

    # Time-window constraints
    log_sr     = entry["sample_rate"]
    min_sample = int(entry["min_ms"] * log_sr / 1000) if entry["min_ms"] > 0 else 0
    max_sample = int(entry["max_ms"] * log_sr / 1000) if entry["max_ms"] > 0 else None

    # Run Python v2 with T_float/T_int from GO log.
    # Pass C++ n_start/n_end directly to avoid ±1 off in n_total from
    # float-truncation, which shifts the sparse tracking grid by one period.
    try:
        # latest_loop_end from CSV (v227+) or derived from SMPL loops.
        latest_loop_end = entry.get("latest_loop_end") or 0
        if latest_loop_end == 0 and loops:
            latest_loop_end = max(l[1] for l in loops)

        py_lut, py_meta = _al.compute_lut_v2(
            attack_mono=atk_mono,
            release_mono=rel_mono,
            T_float=T_float,
            T_int=T_int,
            crossfade_len_samples=entry["crossfade_len"],
            harmonic_number=entry["harmonic"],
            loop_start=loop_start,
            loop_end=loop_end,
            min_sample=min_sample,
            max_sample=max_sample,
            _n_start_override=entry["n_start"],
            _n_end_override=entry["n_end"],
            latest_loop_end_sample=latest_loop_end if latest_loop_end > 0 else None,
        )
    except Exception as e:
        return _skip(f"compute_error:{e}")

    # ── Phase-0 candidate comparison (diagnostic) ─────────────────────────────
    go_phase0 = entry.get("phase0", [])  # list of {beam_id, r, score, n_last}
    py_phase0 = py_meta.get("phase0_candidates", [])  # list of (r_samples, score)

    # ── LUT comparison ─────────────────────────────────────────────────────────
    # Compare in [0, 2T) space so cross-period divergences are detected correctly.
    # go r_raw: unfolded [0,2T) from new CSV (v214+); falls back to folded best_r
    # for old CSV files (both values then in [0,T), circ_dist still works fine).
    # py best_r: already in [0, 2T) from compute_lut_v2 (never folded in Python).
    r_mod = entry.get("r_max") or (2 * T_int)  # comparison modulus

    lut_diffs = []
    if not sim_only:
        tol = max(1, int(T_float / 2))

        go_pts_sorted = sorted(entry["lut"], key=lambda p: p["loop_pos"])
        py_pts_sorted = sorted(py_lut,       key=lambda p: p.loop_pos)

        # Greedy nearest-neighbour match (both lists are sorted by loop_pos)
        matched_py = set()
        go_matched = {}   # go loop_pos → py LutPoint
        j = 0
        for gp in go_pts_sorted:
            glp = gp["loop_pos"]
            best_j, best_d = -1, tol + 1
            k = j
            while k < len(py_pts_sorted):
                plp = py_pts_sorted[k].loop_pos
                if plp > glp + tol:
                    break
                d = abs(plp - glp)
                if plp >= glp - tol and d < best_d and k not in matched_py:
                    best_d, best_j = d, k
                k += 1
            if best_j >= 0:
                matched_py.add(best_j)
                go_matched[glp] = (gp, py_pts_sorted[best_j])
                # advance j to not re-scan already-passed entries
                while j < best_j and py_pts_sorted[j].loop_pos < glp - tol:
                    j += 1
            else:
                go_matched[glp] = (gp, None)

        unmatched_py = [py_pts_sorted[k] for k in range(len(py_pts_sorted))
                        if k not in matched_py]

        for glp, (gp, pp) in sorted(go_matched.items()):
            n_go  = gp["n"]
            go_r  = gp["best_r"]  # unfolded [0,2T) from v215+ CSV
            if pp is None:
                lut_diffs.append({"n": n_go, "issue": "PY_MISSING",
                                  "go_r": go_r, "py_r": None})
            else:
                py_r = int(pp.best_r)  # [0,2T) from compute_lut_v2
                dist = circ_dist(go_r, py_r, r_mod)
                flags_ok = (gp["approach_up"] == pp.approach_up
                            and gp["is_jump"]  == pp.is_jump)
                if dist > 1 or not flags_ok:
                    lut_diffs.append({
                        "n": n_go,
                        "issue": ("MISMATCH_r" if dist > 1 else "") +
                                 ("_flags" if not flags_ok else ""),
                        "go_r":  go_r,  "py_r":  py_r,
                        "dist":  dist,
                        "go_up": gp["approach_up"], "py_up": pp.approach_up,
                        "go_jmp":gp["is_jump"],     "py_jmp":pp.is_jump,
                    })

        for pp in unmatched_py:
            lut_diffs.append({"n": pp.n, "issue": "GO_MISSING",
                              "go_r": None, "py_r": int(pp.best_r)})

    # ── Simulator comparison ──────────────────────────────────────────────────
    # sim_diffs:    C++ sim (GO LUT)  vs  Python C++-compat sim (Python LUT)
    # simsrc_diffs: C++ sim (GO LUT)  vs  Python C++-compat sim (GO LUT)
    #   → simsrc_diffs isolates interpolation-logic differences only
    sim_diffs    = []
    simsrc_diffs = []
    go_lut_dict  = {p["n"]: p for p in entry["lut"]}
    py_lut_dict  = {p.n:   p for p in py_lut}
    go_pts_list  = go_lut_to_lutpoints(go_lut_dict)

    sim_r_mod = entry.get("r_max") or (2 * T_int)  # sim outputs in [0,2T) for v2
    for s in entry["sim"]:
        lp   = s["loop_pos"]
        go_r = s["r_interp"]
        W = int(round(2.0 * T_float))
        # C++-compatible sim with Python LUT
        py_r  = get_position_for_correlation_cpp(lp, py_lut,      T_float, W, False)
        # C++-compatible sim with GO LUT (tests only interpolation, not LUT)
        py_r2 = get_position_for_correlation_cpp(lp, go_pts_list, T_float, W, False)

        if circ_dist(go_r, py_r, sim_r_mod) > 1:
            sim_diffs.append({"loop_pos": lp, "go_r": go_r, "py_r": py_r,
                              "dist": circ_dist(go_r, py_r, sim_r_mod)})
        if circ_dist(go_r, py_r2, sim_r_mod) > 1:
            simsrc_diffs.append({"loop_pos": lp, "go_r": go_r, "py_r": py_r2,
                                 "dist": circ_dist(go_r, py_r2, sim_r_mod)})

    # Compute max circular distance across sim and simsrc diffs.
    # Only large Δr (> T/8) is acoustically relevant.
    max_sim_dr    = max((d["dist"] for d in sim_diffs),    default=0)
    max_simsrc_dr = max((d["dist"] for d in simsrc_diffs), default=0)
    max_lut_dr    = max((d.get("dist", 0) for d in lut_diffs), default=0)
    # Structural LUT difference: different number of points
    lut_count_diff = abs(len(go_lut_dict) - len(py_lut_dict))

    has_any_diff = lut_diffs or sim_diffs or simsrc_diffs
    if not has_any_diff:
        status = "OK"
        severity = "none"
    else:
        status = "MISMATCH"
        # MINOR: only small Δr (≤ T/8) and point count similar
        minor = (max_sim_dr    <= T_int // 8 and
                 max_simsrc_dr <= T_int // 8 and
                 lut_count_diff <= 2)
        severity = "minor" if minor else "major"

    # ── Free-analyzer T_float (reproduce analyze_pipe path exactly) ─────────────
    # Re-read SMPL from the chosen attack file for correct loops list.
    _smpl_chosen  = _parse_smpl_cached(chosen_attack_path)
    _loops_chosen = _smpl_chosen.get("loops", [])
    free_T = _compute_free_analyzer_T(entry, atk_mono, _loops_chosen)

    # ── Autocorr window comparison (logged GO values vs Python-style expected) ──
    diag_ac = {}
    go_ac_max_p = entry.get("ac_max_p", 0)
    if _loops_chosen and go_ac_max_p > 0:
        buf_len          = entry["loop_len"]           # = GetLength()
        go_loop_start    = _loops_chosen[0][0]         # = GetLoopStart()
        py_loop_end      = _loops_chosen[0][1]         # first loop end (Python style)
        go_loop_end_used = entry.get("loop_end_used", 0)  # what C++ actually used

        # Python-style window (what analyze_pipe computes):
        # No backshift — just clip at EOF like Python's array slice.
        py_loop_len  = py_loop_end - go_loop_start + 1
        py_ac_offset = go_loop_start + py_loop_len // 2
        py_ac_window = min(8 * go_ac_max_p, buf_len - py_ac_offset) if py_ac_offset < buf_len else 0

        # C++ values (from log)
        go_ac_offset = entry.get("ac_offset", 0)
        go_ac_window = entry.get("ac_window", 0)

        diag_ac = {
            "go_loop_start":    entry.get("ac_loop_start", 0),
            "go_loop_end_used": go_loop_end_used,
            "py_loop_start":    go_loop_start,
            "py_loop_end":      py_loop_end,
            "go_ac_offset":     go_ac_offset,
            "py_ac_offset":     py_ac_offset,
            "go_ac_window":     go_ac_window,
            "py_ac_window":     py_ac_window,
            "window_mismatch":  (go_ac_offset != py_ac_offset
                                 or go_ac_window != py_ac_window),
        }

    # ── Free analysis: full analyze_pipe path, no GO overrides ──────────────────
    free_result    = {"available": False}
    free_lut_diffs = []
    if free_analysis and not sim_only:
        _use_cpp_scale = (_al.get_cpp_numerics()
                          and _al._CPP_NUMERICS_BACKEND == "numba")
        free_result = _run_free_analysis(
            chosen_attack_path, pipe_desc, entry, _use_cpp_scale)
        if free_result["available"]:
            _free_lut     = free_result["free_lut"]
            _free_T_int   = free_result["free_T_int"]
            _free_T_float = free_result["free_T_float"]
            _r_mod_free   = 2 * _free_T_int
            _tol_free     = max(1, int(_free_T_float / 2))

            _free_sorted = sorted(_free_lut, key=lambda p: p.loop_pos)
            _go_sorted_f = sorted(entry["lut"], key=lambda p: p["loop_pos"])
            _matched_f   = set()
            _go_mat_f    = {}
            j2 = 0
            for gp in _go_sorted_f:
                glp = gp["loop_pos"]
                best_j2, best_d2 = -1, _tol_free + 1
                k2 = j2
                while k2 < len(_free_sorted):
                    plp = _free_sorted[k2].loop_pos
                    if plp > glp + _tol_free:
                        break
                    d2 = abs(plp - glp)
                    if plp >= glp - _tol_free and d2 < best_d2 and k2 not in _matched_f:
                        best_d2, best_j2 = d2, k2
                    k2 += 1
                if best_j2 >= 0:
                    _matched_f.add(best_j2)
                    _go_mat_f[glp] = (gp, _free_sorted[best_j2])
                    while j2 < best_j2 and _free_sorted[j2].loop_pos < glp - _tol_free:
                        j2 += 1
                else:
                    _go_mat_f[glp] = (gp, None)

            for _fp in (_free_sorted[k2] for k2 in range(len(_free_sorted))
                        if k2 not in _matched_f):
                free_lut_diffs.append({"n": _fp.n, "issue": "GO_MISSING",
                                       "go_r": None, "free_r": int(_fp.best_r)})

            for glp, (gp, fp) in sorted(_go_mat_f.items()):
                go_r = gp["best_r"]
                if fp is None:
                    free_lut_diffs.append({"n": gp["n"], "issue": "FREE_MISSING",
                                           "go_r": go_r, "free_r": None})
                else:
                    free_r = int(fp.best_r)
                    dist   = circ_dist(go_r, free_r, _r_mod_free)
                    if dist > 1:
                        free_lut_diffs.append({
                            "n": gp["n"], "issue": "MISMATCH_r",
                            "go_r": go_r, "free_r": free_r, "dist": dist,
                        })

    return {
        "label":       label,
        "min_ms":      entry["min_ms"],
        "max_ms":      entry["max_ms"],
        "status":      status,
        "severity":    severity,
        "max_sim_dr":  max_sim_dr,
        "max_lut_dr":  max_lut_dr,
        "go_pts":      len(go_lut_dict),
        "py_pts":      len(py_lut_dict),
        "lut_diffs":   lut_diffs,
        "sim_diffs":   sim_diffs,
        "simsrc_diffs":simsrc_diffs,
        "go_phase0":   go_phase0,
        "py_phase0":   py_phase0,
        "phase0_n_last": py_meta.get("phase0_n_last"),
        "go_final_beams":  entry.get("final_beams", []),
        "py_final_beams":  py_meta.get("final_beams", []),
        "go_pre_prune":    entry.get("pre_prune",  []),
        "py_pre_prune":    py_meta.get("pre_prune_points",  []),
        "go_post_prune":   entry.get("post_prune", []),
        "py_post_prune":   py_meta.get("post_prune_points", []),
        "go_phase15_ivs":  entry.get("phase15_ivs",   []),
        "go_phase15_steps":entry.get("phase15_steps",  []),
        "py_phase15_ivs":  py_meta.get("phase15_ivs",   []),
        "py_phase15_steps":py_meta.get("phase15_steps",  []),
        "go_ndp_silent":   entry.get("ndp_silent_score", "?"),
        "go_align_ver":    entry.get("align_version", ""),
        "diag_atk_len": _diag_atk_len,
        "diag_rel_len": _diag_rel_len,
        "diag_log_atk": _diag_log_atk,
        "diag_log_rel": _diag_log_rel,
        "diag_atk_path": chosen_attack_path,
        "diag_rel_path": pipe_desc["release_path"],
        "diag_py_loop_end": max((l[1] for l in loops), default=0) if loops else 0,
        "diag_go_loop_end": entry.get("latest_loop_end", 0),
        # T_float diagnostics (v231b+)
        "diag_initial_T":    entry.get("initial_T", 0.0),
        "diag_T_float_go":   entry["T_float"],
        "diag_ac":           diag_ac,
        "diag_free_T":       free_T,
        # Free-analysis results
        "free_result":    free_result,
        "free_lut_diffs": free_lut_diffs,
        # GO CSV params needed for input comparison in run_analysis FREE block
        "go_params": {
            "sample_rate":    entry.get("sample_rate", 44100),
            "crossfade_len":  entry.get("crossfade_len", 0),
            "n_start":        entry.get("n_start", 0),
            "n_end":          entry.get("n_end", 0),
            "ds":             entry.get("ds", 1),
            "r_max":          entry.get("r_max", 0),
            "loop_len":       entry.get("loop_len", 0),
            "latest_loop_end":entry.get("latest_loop_end", 0),
            "T_float":        entry.get("T_float", 0.0),
            # ODF crossfade source for comparison with free-analysis source
            "crossfade_len_source": pipe_desc.get("crossfade_len_source", "unknown"),
            # Derived diagnostics for n_end and cs_last_d comparison
            "n_total": max(1, int((entry.get("loop_len", 0) - entry.get("crossfade_len", 0))
                                  / max(1e-9, entry.get("T_float", 1.0)))),
            "max_sample": (int(entry.get("max_ms", 0) * entry.get("sample_rate", 44100) / 1000)
                           if entry.get("max_ms", 0) > 0 else None),
            "cs_last_full": int(round((entry.get("n_end", 1) - 1) * entry.get("T_float", 0.0))),
        },
    }


# ── Persistent settings ───────────────────────────────────────────────────────

_SETTINGS_FILE = os.path.join(os.path.expanduser("~"), ".verify_lut_settings.json")

def _load_settings() -> dict:
    try:
        with open(_SETTINGS_FILE, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}

def _save_settings(s: dict):
    try:
        with open(_SETTINGS_FILE, "w", encoding="utf-8") as f:
            json.dump(s, f, indent=2)
    except Exception:
        pass


# Module-level initializer for ProcessPoolExecutor workers (must be picklable).
def _worker_init(cpp_num: bool) -> None:
    _al.set_cpp_numerics(cpp_num)


# ── Core analysis (shared by CLI and GUI) ────────────────────────────────────

def run_analysis(log_path, organ_path, filter_str="", max_pipes=0,
                 workers=None, verbose=False, sim_only=False,
                 compare_inputs=False, free_analysis=False,
                 line_cb=None, progress_cb=None, cancel_event=None):
    """Run the full comparison.
    line_cb(str)          — called for each output line (None → print).
    progress_cb(done,tot) — optional progress updates.
    cancel_event          — threading.Event; set it to abort early.
    Returns (ok, mis, skip, notfound, errs) or None if cancelled.
    """
    import concurrent.futures

    if workers is None:
        workers = os.cpu_count() or 4

    def emit(s):
        if line_cb:
            line_cb(s)
        else:
            print(s)

    def cancelled():
        return cancel_event is not None and cancel_event.is_set()

    entries = parse_verify_log(log_path)
    emit(f"Parsed {len(entries)} pipe-release entries from {log_path}")
    if cancelled(): return None

    if filter_str:
        entries = [e for e in entries if filter_str.lower() in e["label"].lower()]
        emit(f"After filter '{filter_str}': {len(entries)} entries")

    if max_pipes:
        entries = entries[:max_pipes]

    total = len(entries)

    import platform
    # ProcessPoolExecutor: each process has its own GIL → true CPU parallelism.
    # Good on local disk (Windows) where workers re-read WAVs cheaply.
    # ThreadPoolExecutor: shares memory (WAV cache) → better on slow shared
    # folders where pre-loading matters.
    n_workers     = max(1, workers)
    use_processes = (platform.system() == "Windows" and n_workers > 1)

    # Pre-warm WAV cache sequentially only when using threads (shared memory).
    # With ProcessPoolExecutor (Windows) each worker reads its own WAVs in
    # parallel — sequential pre-loading would just waste time.
    if not use_processes:
        all_pipes = _parse_organ_fast(organ_path)
        needed_wavs: set = set()
        for e in entries:
            for p in _find_pipes_for_label_cached(organ_path, e["label"], all_pipes):
                needed_wavs.add(p["attack_path"])
                needed_wavs.add(p["release_path"])
        emit(f"Preloading {len(needed_wavs)} WAV files...")
        for path in sorted(needed_wavs):
            if cancelled(): return None
            try:
                _load_wav_cached(path)
                _parse_smpl_cached(path)
            except Exception:
                pass
        emit("WAV cache ready.")
    else:
        emit("Local disk detected — workers load WAVs in parallel.")

    ok = mis = skip = notfound = errs = 0
    # ProcessPoolExecutor spawns fresh interpreter processes — module-level globals
    # like _al._cpp_numerics_enabled are NOT inherited. Pass them via initializer.
    # _worker_init is defined at module level (required for pickling).
    _pool_kwargs: dict = {}
    if use_processes:
        _pool_kwargs["initializer"] = _worker_init
        _pool_kwargs["initargs"]    = (_al.get_cpp_numerics(),)

    PoolClass = (concurrent.futures.ProcessPoolExecutor if use_processes
                 else concurrent.futures.ThreadPoolExecutor)

    # compare_entry is a module-level function → picklable for ProcessPool.
    import functools
    _run = functools.partial(compare_entry, organ_path=organ_path,
                             sim_only=sim_only, verbose=verbose,
                             free_analysis=free_analysis)

    if n_workers == 1:
        results = []
        for i, e in enumerate(entries):
            if cancelled(): return None
            results.append(_run(e))
            if progress_cb: progress_cb(i + 1, total)
    else:
        # Do NOT use "with pool:" — the context manager calls shutdown(wait=True)
        # on __exit__, blocking until all workers finish even after cancel.
        # We manage the pool manually so cancel returns immediately.
        pool = PoolClass(max_workers=n_workers, **_pool_kwargs)
        futures_to_idx: dict = {}
        ordered = [None] * total
        completed = 0
        try:
            for i, e in enumerate(entries):
                futures_to_idx[pool.submit(_run, e)] = i
            for fut in concurrent.futures.as_completed(futures_to_idx):
                if cancelled():
                    pool.shutdown(wait=False, cancel_futures=True)
                    return None
                ordered[futures_to_idx[fut]] = fut.result()
                completed += 1
                if progress_cb: progress_cb(completed, total)
        finally:
            pool.shutdown(wait=False, cancel_futures=True)
        results = ordered

    def _show_prune_ctx(go_raw, py_raw, n_go, n_py, idx):
        """Emit ±2 context lines around pre/post-prune divergence index."""
        ctx_lo = max(0, idx - 2)
        ctx_hi = min(max(n_go, n_py) - 1, idx + 2)
        for i in range(ctx_lo, ctx_hi + 1):
            mark = ">>>" if i == idx else "   "
            if i < n_go and go_raw:
                gp = go_raw[i]
                sc = gp.get("score", float("nan"))
                sc_str = f" sc={sc:+.4f}" if not (sc != sc) else ""  # nan check
                go_str = (f"n={gp['n']:5d} r={gp['r']:5d}{sc_str} "
                          f"jmp={int(gp['is_jump'])} up={int(gp['approach_up'])}")
            else:
                go_str = "---"
            if i < n_py and py_raw:
                t = py_raw[i]
                # t is (n, r, score, is_jump) or (n, r, is_jump) — handle both
                if len(t) >= 4:
                    py_sc_str = f" sc={t[2]:+.4f}"
                    py_jmp = f" jmp={t[3]}"
                elif len(t) == 3:
                    py_sc_str = ""
                    py_jmp = f" jmp={t[2]}"
                else:
                    py_sc_str = ""
                    py_jmp = ""
                py_str = f"n={t[0]:5d} r={t[1]:5d}{py_sc_str}{py_jmp}"
            else:
                py_str = "---"
            emit(f"           {mark} [{i:3d}] GO: {go_str}")
            emit(f"                      PY: {py_str}")

    def _show_phase15_diag(go_ivs, py_ivs, go_steps, py_steps):
        """Show Phase 1.5 intervals and steps where GO and Python diverge."""
        if not go_ivs and not py_ivs:
            return
        # Find triggered intervals in GO
        go_trig = [(d["na"], d["nb"]) for d in go_ivs if d.get("triggered")]
        py_trig = {(d[0], d[1]) for d in py_ivs if d[7]}  # (na,nb) triggered in PY
        if not go_trig:
            return
        emit(f"         phase15 triggered intervals:")
        for na, nb in go_trig[:5]:
            go_iv = next((d for d in go_ivs if d["na"]==na and d["nb"]==nb), None)
            py_iv = next((d for d in py_ivs if d[0]==na  and d[1]==nb), None)
            in_py = (na, nb) in py_trig
            go_str = (f"na={na} nb={nb} ra={go_iv['ra']} rb={go_iv['rb']} "
                      f"raw={go_iv['raw_dist']} circ={go_iv['circ_dist']} "
                      f"sp_T_raw={go_iv['sp_T_raw']}")
            if py_iv:
                py_str = (f"circ={py_iv[5]} sp_T={py_iv[6]} "
                          f"trig={'YES' if py_iv[7] else 'NO'}")
            else:
                py_str = "NOT IN PY IVLIST"
            emit(f"           GO iv: {go_str}")
            emit(f"           PY iv: {py_str}")
        # Show first few steps for first triggered interval
        first_iv = go_trig[0]
        go_st = [s for s in go_steps
                 if s.get("inserted") is not None
                 and any(iv["na"]==first_iv[0] and iv["nb"]==first_iv[1]
                         for iv in go_ivs if iv["triggered"])]
        # Simpler: show phase15_steps where nd in (na+1..nb-1)
        na0, nb0 = first_iv
        go_st2 = [s for s in go_steps if na0 < s["nd"] < nb0][:6]
        py_st2 = [s for s in py_steps if na0 < s[2] < nb0][:6]
        if go_st2 or py_st2:
            emit(f"         phase15 steps for iv [{na0},{nb0}]:")
            for s in go_st2:
                emit(f"           GO step n={s['nd']} exp={s['exp_pos']} "
                     f"r={s['best_r']} sc={s['best_sc']:+.4f} ins={int(s['inserted'])}")
            for s in py_st2:
                emit(f"           PY step n={s[2]} exp={s[4]} "
                     f"r={s[5]} sc={s[6]:+.4f} ins={s[7]}")

    def _rel_tag(res):
        """Short release-window tag, e.g. '[≥778ms]' or '[0..256ms]'."""
        mn, mx = res.get("min_ms", 0), res.get("max_ms", 0)
        if mn and mx:
            return f"[{mn}..{mx}ms]"
        elif mn:
            return f"[≥{mn}ms]"
        elif mx:
            return f"[≤{mx}ms]"
        return "[always]"

    minor_mis = 0
    major_mis = 0
    free_mis  = 0

    def _emit_inputs(res):
        """Emit one-line T_float + autocorr-window comparison for a single pipe."""
        ft = res.get("diag_free_T", {})
        if not ft.get("available"):
            return
        go_T   = res.get("diag_T_float_go", 0.0)
        py_T   = ft["py_T_float"]
        T_diff = go_T - py_T
        if abs(T_diff) < 1e-9:
            T_tag = "T✓"
        else:
            T_tag = f"T⚠  GO={go_T:.6f}  PY={py_T:.6f}  Δ={T_diff:+.6f}"
        da = res.get("diag_ac", {})
        if da:
            loop_end_go = da.get("go_loop_end_used", 0)
            loop_end_py = da.get("py_loop_end",      0)
            le_tag = "le✓" if loop_end_go == loop_end_py else (
                f"le⚠ GO={loop_end_go} PY={loop_end_py}")
            if da.get("window_mismatch"):
                win_tag = (f"win⚠  GO off={da['go_ac_offset']} win={da['go_ac_window']}"
                           f"  PY off={da['py_ac_offset']} win={da['py_ac_window']}")
            else:
                win_tag = "win✓"
            win_str = f"  {le_tag}  {win_tag}"
        else:
            win_str = ""
        emit(f"  INP  {res['label']:50s} {_rel_tag(res):20s} {T_tag}{win_str}")

    for res in results:
        st  = res["status"]
        sev = res.get("severity", "none")
        rel = _rel_tag(res)
        if compare_inputs:
            _emit_inputs(res)

        # ── FREE_DIFF: free-analyzer LUT vs GO (independent of REPLAY status) ──
        # Processed first so that `continue` inside the REPLAY-DIFF block cannot
        # suppress FREE_DIFF output.
        free_diffs = res.get("free_lut_diffs", [])
        fr         = res.get("free_result", {})

        def _emit_free_inputs(fr, res):
            """Emit GO vs FREE input parameter comparison with ⚠ on mismatches."""
            fm = fr.get("free_meta", {})
            if not fm:
                return
            gp = res.get("go_params", {})

            go_xfade   = gp.get("crossfade_len", 0)
            free_xfade = fm.get("window_len", fr.get("free_crossfade", 0))
            go_ll      = gp.get("loop_len", 0)
            free_ll    = fm.get("atk_full_len", 0)
            go_le      = gp.get("latest_loop_end", 0)
            free_le    = fr.get("free_latest_le", 0)
            go_nst     = gp.get("n_start", 0)
            free_nst   = fm.get("n_start", 0)
            go_nend    = gp.get("n_end", 0)
            free_nend  = fm.get("n_end", 0)
            go_ds      = gp.get("ds", 1)
            free_ds    = fm.get("ds", 1)
            go_rmax    = gp.get("r_max", 0)
            free_rmax  = fm.get("r_max", 0)
            go_nlast   = go_nend - 1
            free_nlast = fm.get("n_last", free_nend - 1)
            go_csld    = int(round(go_nlast * gp.get("T_float", 0.0))) // max(1, go_ds)
            free_csld  = fm.get("cs_last_d", 0)

            def _w(gv, fv): return "⚠" if gv != fv else ""

            go_xf_src   = gp.get("crossfade_len_source", "")
            free_xf_src = fr.get("free_xfade_source", "")
            xfade_diff  = go_xfade != free_xfade
            xfade_src_note = f" [{go_xf_src}→{free_xf_src}]" if xfade_diff else ""
            emit(f"         INP GO:   xfade={go_xfade} nst={go_nst} nend={go_nend} "
                 f"ds={go_ds} rmax={go_rmax} n_last={go_nlast} "
                 f"cs_last_d={go_csld} loop_len={go_ll} latest_le={go_le}")
            emit(f"         INP FREE: xfade={free_xfade}{_w(go_xfade,free_xfade)}{xfade_src_note} "
                 f"nst={free_nst}{_w(go_nst,free_nst)} "
                 f"nend={free_nend}{_w(go_nend,free_nend)} "
                 f"ds={free_ds}{_w(go_ds,free_ds)} "
                 f"rmax={free_rmax}{_w(go_rmax,free_rmax)} "
                 f"n_last={free_nlast}{_w(go_nlast,free_nlast)} "
                 f"cs_last_d={free_csld}{_w(go_csld,free_csld)} "
                 f"loop_len={free_ll}{_w(go_ll,free_ll)} "
                 f"latest_le={free_le}{_w(go_le,free_le)}")

            # ── Detail: cs_last_d ±1 (downsampling rounding) ──────────────────
            if go_csld != free_csld:
                go_T      = gp.get("T_float", 0.0)
                free_T    = fr.get("free_T_float", 0.0)
                go_cs_f   = gp.get("cs_last_full", int(round(go_nlast * go_T)))
                free_cs_f = fm.get("cs_last", 0)
                go_prod   = go_nlast   * go_T
                free_prod = free_nlast * free_T
                emit(f"           cs_last(full): GO={go_cs_f} FREE={free_cs_f}"
                     f"{'⚠' if go_cs_f != free_cs_f else ''}"
                     f"  (÷ds={go_ds}→{go_csld}, ÷ds={free_ds}→{free_csld})")
                emit(f"           n_last×T: GO={go_prod:.15g} ({float.hex(go_prod)})")
                emit(f"           n_last×T: FR={free_prod:.15g} ({float.hex(free_prod)})")

            # ── Detail: nend/nst divergence ────────────────────────────────────
            if go_nend != free_nend or go_nst != free_nst:
                import math as _math
                go_T      = gp.get("T_float", 0.0)
                free_T    = fr.get("free_T_float", 0.0)
                go_ntot   = gp.get("n_total", 0)
                free_ntot = fm.get("n_total", 0)
                go_max_s  = gp.get("max_sample")   # None for unbounded
                free_max_s = fr.get("free_max_sample")
                emit(f"           n_total: GO={go_ntot} FREE={free_ntot}"
                     f"{'⚠' if go_ntot != free_ntot else ''}")
                if go_max_s is not None and free_max_s is not None and go_T > 0 and free_T > 0:
                    go_ratio   = go_max_s   / go_T
                    free_ratio = free_max_s / free_T
                    go_ceil    = _math.ceil(go_ratio)
                    free_ceil  = _math.ceil(free_ratio)
                    go_ne_calc  = min(go_ntot,   go_ceil   + 2)
                    free_ne_calc = min(free_ntot, free_ceil + 2)
                    emit(f"           max_sample: GO={go_max_s} FREE={free_max_s}"
                         f"{'⚠' if go_max_s != free_max_s else ''}")
                    emit(f"           max_s/T: GO={go_ratio:.10g} FREE={free_ratio:.10g}"
                         f"{'⚠' if go_ratio != free_ratio else ''}")
                    emit(f"           ceil(max_s/T): GO={go_ceil} FREE={free_ceil}"
                         f"{'⚠' if go_ceil != free_ceil else ''}"
                         f"  → n_end=min(ntot,ceil+2): GO={go_ne_calc} FREE={free_ne_calc}"
                         f"{'⚠' if go_ne_calc != free_ne_calc else ''}")
                elif go_max_s is None and free_max_s is None and go_T > 0 and free_T > 0:
                    # Unbounded release: n_end from latest_loop_end
                    go_le_r   = go_le   / go_T   if go_T   > 0 else 0.0
                    free_le_r = free_le / free_T if free_T > 0 else 0.0
                    go_ne_u   = min(go_ntot,   _math.ceil(go_le_r)   + 2)
                    free_ne_u = min(free_ntot, _math.ceil(free_le_r) + 2)
                    emit(f"           (unbounded) latest_le/T: GO={go_le_r:.10g} FREE={free_le_r:.10g}"
                         f"{'⚠' if _math.ceil(go_le_r) != _math.ceil(free_le_r) else ''}")
                    emit(f"           → n_end=min(ntot,ceil+2): GO={go_ne_u} FREE={free_ne_u}"
                         f"{'⚠' if go_ne_u != free_ne_u else ''}")

        if free_diffs:
            free_mis += 1
            go_T     = res.get("diag_T_float_go", 0.0)
            free_T_f = fr.get("free_T_float", 0.0)
            T_delta  = free_T_f - go_T
            if abs(T_delta) > 0.001:
                T_tag = f"freeT={free_T_f:.3f}(GOT={go_T:.3f},Δ={T_delta:+.3f})"
            else:
                T_tag = "freeT=T✓"
            go_pts_n    = res.get("go_pts", "?")
            free_pts_n  = fr.get("free_pts", "?")
            max_free_dr = max((d.get("dist", 0) for d in free_diffs), default=0)
            emit(f"  FREE   {res['label']:50s} {rel:20s} "
                 f"go={go_pts_n:3} free={free_pts_n:3}  "
                 f"[DIFF:{len(free_diffs)}]  maxΔr={max_free_dr}  {T_tag}")
            # Input comparison before LUT points
            _emit_free_inputs(fr, res)
            # Phase0 candidates (GO vs FREE)
            go_p0_raw = res.get("go_phase0", [])
            free_p0   = fr.get("free_meta", {}).get("phase0_candidates", [])
            if go_p0_raw or free_p0:
                def _fmt_p0c(cands, n=3):
                    return "  ".join(f"r={r:5d} sc={sc:.6f}" for r, sc in cands[:n])
                go_p0_top  = [(c["r"], c["score"]) for c in go_p0_raw[:3]]
                free_p0_top = free_p0[:3]
                emit(f"         Phase0  GO: {_fmt_p0c(go_p0_top)}")
                emit(f"         Phase0 FR: {_fmt_p0c(free_p0_top)}")
            # LUT point diffs
            for d in free_diffs[:6]:
                issue = d.get("issue", "")
                if issue == "MISMATCH_r":
                    emit(f"         FREE n={d['n']:4d}  Δr={d['dist']:3d}  "
                         f"go_r={d['go_r']:4d} free_r={d['free_r']:4d}")
                elif d["go_r"] is None:
                    emit(f"         FREE n={d['n']:4d}  GO_MISSING  free_r={d['free_r']}")
                else:
                    emit(f"         FREE n={d['n']:4d}  FREE_MISSING  go_r={d['go_r']}")
            if len(free_diffs) > 6:
                emit(f"         ... and {len(free_diffs)-6} more FREE diffs")
        elif free_analysis and fr.get("available") and compare_inputs:
            # Show FREE✓ line when compare_inputs is active (like INP lines)
            go_T     = res.get("diag_T_float_go", 0.0)
            free_T_f = fr.get("free_T_float", 0.0)
            T_tag = "freeT=T✓" if abs(free_T_f - go_T) < 0.001 else f"freeT={free_T_f:.3f}"
            emit(f"  FREE✓  {res['label']:50s} {rel:20s} "
                 f"go={res.get('go_pts','?'):3} free={fr.get('free_pts','?'):3}  {T_tag}")

        if st == "OK":
            ok += 1
        elif st.startswith("skip"):
            skip += 1
        elif st == "not_found":
            notfound += 1
            emit(f"  ???    {res['label']}  (not found in ODF)")
        elif "error" in st.lower():
            errs += 1
            emit(f"  ERR    {res['label']} {rel}: {st}")
        else:
            mis += 1
            if sev == "minor":
                minor_mis += 1
            else:
                major_mis += 1

            # Only print minor diffs in verbose mode
            if sev == "minor" and not verbose:
                continue

            has_lut  = bool(res["lut_diffs"])
            has_sim  = bool(res["sim_diffs"])
            has_ssrc = bool(res["simsrc_diffs"])
            tags = []
            if has_lut:  tags.append(f"LUT:{len(res['lut_diffs'])}")
            if has_ssrc: tags.append(f"SIM_LOGIC:{len(res['simsrc_diffs'])}")
            if has_sim and not has_ssrc: tags.append(f"SIM_FROM_LUT:{len(res['sim_diffs'])}")
            sev_tag = "" if sev == "major" else f" [{sev}]"
            max_dr = max(res.get("max_lut_dr", 0), res.get("max_sim_dr", 0))
            emit(f"  DIFF{sev_tag} {res['label']:50s} {rel:20s} go={res.get('go_pts','?'):3} py={res.get('py_pts','?'):3}  [{', '.join(tags)}]  maxΔr={max_dr}")
            for d in res["lut_diffs"][:10]:
                if d.get("issue","").startswith("MISMATCH") or d.get("issue","") in ("GO_MISSING","PY_MISSING"):
                    if d["go_r"] is None or d["py_r"] is None:
                        emit(f"         LUT n={d['n']:4d}  {d['issue']}  r={d['go_r'] if d['go_r'] is not None else d['py_r']}")
                    else:
                        emit(f"         LUT n={d['n']:4d}  Δr={d.get('dist',0):3d}  "
                             f"go_r={d['go_r']:4d} py_r={d['py_r']:4d}  "
                             f"up={d.get('go_up')}/{d.get('py_up')}  "
                             f"jmp={d.get('go_jmp')}/{d.get('py_jmp')}")
            if len(res["lut_diffs"]) > 10:
                emit(f"         ... and {len(res['lut_diffs'])-10} more LUT diffs")
            # Phase-0 comparison: show for major diffs
            go_p0 = res.get("go_phase0", [])
            py_p0 = res.get("py_phase0", [])
            _show_p0 = (sev == "major" and res["lut_diffs"])
            if _show_p0 and (go_p0 or py_p0):
                n_last = res.get("phase0_n_last", "?")
                def _fmt_p0(cands, n=4):
                    return "  ".join(f"r={r:5d} sc={sc:.6f}" for r, sc in cands[:n])
                go_top = [(c["r"], c["score"]) for c in go_p0[:4]]
                py_top = py_p0[:4]
                emit(f"         Phase0 n_last={n_last}:")
                emit(f"           GO: {_fmt_p0(go_top)}")
                emit(f"           PY: {_fmt_p0(py_top)}")
                # Flag near-tie: top GO and top PY score differ by < 5e-4
                if go_top and py_top:
                    score_gap = abs(go_top[0][1] - py_top[0][1])
                    r_gap = abs(go_top[0][0] - py_top[0][0])
                    if r_gap > 1:
                        if score_gap < 5e-4:
                            emit(f"           → NEAR-TIE: top score gap={score_gap:.2e}  r_gap={r_gap}")
                        else:
                            emit(f"           → DIVERGED: score_gap={score_gap:.4f}  r_gap={r_gap}")
                            # Show WAV lengths and paths to diagnose wrong file
                            da, dl_a = res.get("diag_atk_len",0), res.get("diag_log_atk",0)
                            dr, dl_r = res.get("diag_rel_len",0), res.get("diag_log_rel",0)
                            atk_ok = dl_a == 0 or da >= dl_a
                            rel_ok = dl_r == 0 or dr >= dl_r
                            emit(f"           WAV: atk={da}(GO={dl_a}){'✓' if atk_ok else '⚠MISMATCH'}  rel={dr}(GO={dl_r}){'✓' if rel_ok else '⚠MISMATCH'}")
                            py_le = res.get("diag_py_loop_end", 0)
                            go_le = res.get("diag_go_loop_end", 0)
                            le_match = "✓" if py_le == go_le else f"⚠PY={py_le} GO={go_le}"
                            emit(f"           latest_loop_end: {le_match}")
                            ap = res.get("diag_atk_path","?")
                            emit(f"           atk_path: ...{ap[-60:]}" if len(ap)>60 else f"           atk_path: {ap}")
            # T_float and autocorr-window diagnostics (v231b+ logs only).
            # Show for any diff when initial_T or ac_offset data is present.
            diag_ac = res.get("diag_ac", {})
            _init_T  = res.get("diag_initial_T", 0.0)
            _T_go    = res.get("diag_T_float_go", 0.0)
            if _init_T > 0.0:
                _T_delta = _T_go - _init_T
                _T_tag   = f"Δ={_T_delta:+.6f}" if abs(_T_delta) > 1e-9 else "Δ=0 (no autocorr change)"
                emit(f"         T_float: initial_T={_init_T:.6f}  →  T_float={_T_go:.6f}  {_T_tag}")
            if diag_ac:
                go_off = diag_ac["go_ac_offset"]
                py_off = diag_ac["py_ac_offset"]
                go_win = diag_ac["go_ac_window"]
                py_win = diag_ac["py_ac_window"]
                win_tag = "⚠MISMATCH" if diag_ac["window_mismatch"] else "✓"
                emit(f"         autocorr window {win_tag}: "
                     f"GO offset={go_off} win={go_win}  "
                     f"PY offset={py_off} win={py_win}  "
                     f"(loop_start={diag_ac['go_loop_start']} "
                     f"py_loop_end={diag_ac['py_loop_end']})")
            # Pre/post-prune and final-beam diagnostics.
            # Major diffs always; minor diffs in verbose mode.
            if (sev == "major" or verbose) and res["lut_diffs"]:
                go_fb  = res.get("go_final_beams", [])
                py_fb  = res.get("py_final_beams", [])
                go_pre = res.get("go_pre_prune",  [])
                py_pre = res.get("py_pre_prune",  [])
                go_post = res.get("go_post_prune", [])
                py_post = res.get("py_post_prune", [])
                if go_fb or py_fb:
                    def _f32hex(v):
                        f32 = _np.float32(v)
                        return struct.pack('f', float(f32)).hex()
                    def _fmt_fb(lst, n=4):
                        return "  ".join(
                            f"[{b['beam_id']}]r={b['r']} c={_np.float32(b['cum_score']):.10f}({_f32hex(b['cum_score'])})"
                            for b in lst[:n])
                    def _fmt_py_fb(lst, n=4):
                        return "  ".join(
                            f"[{r_c_b[2]}]r={r_c_b[0]} c={_np.float32(r_c_b[1]):.10f}({_f32hex(r_c_b[1])})"
                            for r_c_b in lst[:n])
                    emit(f"         final_beams GO: {_fmt_fb(go_fb)}")
                    emit(f"         final_beams PY: {_fmt_py_fb(py_fb)}")
                if go_pre is not None or py_pre is not None:
                    n_go_pre  = len(go_pre)  if go_pre  else 0
                    n_py_pre  = len(py_pre)  if py_pre  else 0
                    n_go_post = len(go_post) if go_post else 0
                    n_py_post = len(py_post) if py_post else 0
                    go_pre_pts = [(p["n"], p["r"]) for p in go_pre]
                    # py_pre may be (n,r) or (n,r,is_jump) — compare on (n,r) only
                    py_pre_pts = [(t[0], t[1]) for t in py_pre] if py_pre else []
                    pre_match = (go_pre_pts == py_pre_pts)
                    emit(f"         pre_prune:  GO={n_go_pre}pts  PY={n_py_pre}pts  "
                         f"{'SAME' if pre_match else 'DIFFER'}")
                    if pre_match:
                        go_post_pts = [(p["n"], p["r"]) for p in go_post] if go_post else []
                        py_post_pts = [(t[0], t[1]) for t in py_post] if py_post else []
                        post_match  = (go_post_pts == py_post_pts)
                        emit(f"         post_prune: GO={n_go_post}pts  PY={n_py_post}pts  "
                             f"{'SAME' if post_match else 'DIFFER'}")
                        if post_match:
                            emit(f"           → Tracking+Pruning OK (unexpected LUT diff)")
                        else:
                            emit(f"           → Tracking OK; PruneV2 diverges "
                                 f"(GO pruned {n_go_pre-n_go_post}, PY pruned {n_py_pre-n_py_post})")
                            first_post_idx = next(
                                (i for i, (a, b) in enumerate(zip(go_post_pts, py_post_pts)) if a != b),
                                min(n_go_post, n_py_post))
                            emit(f"           → post_prune DIVERGED at idx={first_post_idx}")
                            _show_prune_ctx(go_post, py_post, n_go_post, n_py_post, first_post_idx)
                    else:
                        emit(f"         post_prune: GO={n_go_post}pts  PY={n_py_post}pts")
                        first_diff_idx = next(
                            (i for i, (a, b) in enumerate(zip(go_pre_pts, py_pre_pts)) if a != b),
                            min(n_go_pre, n_py_pre))
                        res["pre_prune_div_idx"] = first_diff_idx
                        emit(f"           → pre_prune DIVERGED at idx={first_diff_idx}")
                        _show_prune_ctx(go_pre, py_pre, n_go_pre, n_py_pre, first_diff_idx)
                        # Phase-0 comparison when tracking diverges from the very first step
                        if first_diff_idx == 0 and (go_p0 or py_p0):
                            n_last = res.get("phase0_n_last", "?")
                            def _fmt_p0(cands, n=4):
                                return "  ".join(f"r={r:5d} sc={sc:.6f}" for r, sc in cands[:n])
                            go_top = [(c["r"], c["score"]) for c in go_p0[:4]]
                            py_top = py_p0[:4]
                            emit(f"         Phase0 n_last={n_last}:")
                            emit(f"           GO: {_fmt_p0(go_top)}")
                            emit(f"           PY: {_fmt_p0(py_top)}")
                            if go_top and py_top:
                                score_gap = abs(go_top[0][1] - py_top[0][1])
                                r_gap = abs(go_top[0][0] - py_top[0][0])
                                if r_gap > 1:
                                    if score_gap < 5e-4:
                                        emit(f"           → NEAR-TIE: top score gap={score_gap:.2e}  r_gap={r_gap}")
                                    else:
                                        emit(f"           → DIVERGED: score_gap={score_gap:.4f}  r_gap={r_gap}")
                        # Show GO binary marker and Phase 1.5 diagnostic.
                        go_av  = res.get("go_align_ver",  "")
                        go_nss = res.get("go_ndp_silent", "?")
                        if go_av or go_nss != "?":
                            emit(f"         GO build: align_version={go_av}  ndp_silent_score={go_nss}")
                        cnt_diff = n_go_pre - n_py_pre
                        if 0 < cnt_diff <= 10:
                            go_ivs   = res.get("go_phase15_ivs",   [])
                            py_ivs   = res.get("py_phase15_ivs",   [])
                            go_steps = res.get("go_phase15_steps",  [])
                            py_steps = res.get("py_phase15_steps",  [])
                            _show_phase15_diag(go_ivs, py_ivs, go_steps, py_steps)
            for s in res["simsrc_diffs"][:5]:
                emit(f"         SIM_LOGIC lp={s['loop_pos']:7d}  go={s['go_r']:4d} py={s['py_r']:4d}  Δ={s['dist']}")
            if len(res["simsrc_diffs"]) > 5:
                emit(f"         ... and {len(res['simsrc_diffs'])-5} more sim-logic diffs")

    minor_hint = f"  (minor diffs hidden — use --verbose to show)" if minor_mis > 0 and not verbose else ""
    free_hint  = f"  FREE_DIFF={free_mis}" if free_analysis else ""
    emit(f"\nResult: {total} entries — "
         f"OK={ok}  MISMATCH={mis} (major={major_mis} minor={minor_mis})  "
         f"SKIP={skip}  NOT_FOUND={notfound}  ERR={errs}{free_hint}{minor_hint}")
    return ok, mis, skip, notfound, errs


# ── GUI ───────────────────────────────────────────────────────────────────────

def run_gui():
    root = _tk.Tk()
    root.title("verify_lut — GO vs Python v2")
    root.minsize(720, 560)

    settings = _load_settings()

    var_log     = _tk.StringVar(value=settings.get("log",     ""))
    var_organ   = _tk.StringVar(value=settings.get("organ",   ""))
    var_filter  = _tk.StringVar(value=settings.get("filter",  ""))
    var_workers = _tk.IntVar(   value=settings.get("workers", os.cpu_count() or 4))
    var_status      = _tk.StringVar(value="Ready")
    var_prog        = _tk.DoubleVar(value=0.0)
    var_cpp_num        = _tk.BooleanVar(value=settings.get("cpp_numerics",    False))
    var_verbose        = _tk.BooleanVar(value=settings.get("verbose",         False))
    var_compare_inputs = _tk.BooleanVar(value=settings.get("compare_inputs",  False))
    var_free_analysis  = _tk.BooleanVar(value=settings.get("free_analysis",   False))

    def _apply_cpp_numerics(*_):
        _al.set_cpp_numerics(var_cpp_num.get())
    var_cpp_num.trace_add("write", _apply_cpp_numerics)
    _al.set_cpp_numerics(var_cpp_num.get())  # apply on startup

    # ── File pickers ──────────────────────────────────────────────────────────
    def browse_log():
        p = _tkfd.askopenfilename(
            title="Select go_lut_verify.csv",
            filetypes=[("CSV", "*.csv"), ("All files", "*.*")],
            initialfile=var_log.get() or None)
        if p:
            var_log.set(p)

    def browse_organ():
        p = _tkfd.askopenfilename(
            title="Select .organ file",
            filetypes=[("Organ", "*.organ"), ("All files", "*.*")],
            initialfile=var_organ.get() or None)
        if p:
            var_organ.set(p)

    # ── Layout ────────────────────────────────────────────────────────────────
    pad = {"padx": 6, "pady": 3}

    top = _ttk.Frame(root, padding=8)
    top.grid(row=0, column=0, sticky="ew")
    root.columnconfigure(0, weight=1)

    _ttk.Label(top, text="Log CSV:").grid(row=0, column=0, sticky="w", **pad)
    _ttk.Entry(top, textvariable=var_log, width=62).grid(row=0, column=1, sticky="ew", **pad)
    _ttk.Button(top, text="Browse…", command=browse_log).grid(row=0, column=2, **pad)

    _ttk.Label(top, text="Organ ODF:").grid(row=1, column=0, sticky="w", **pad)
    _ttk.Entry(top, textvariable=var_organ, width=62).grid(row=1, column=1, sticky="ew", **pad)
    _ttk.Button(top, text="Browse…", command=browse_organ).grid(row=1, column=2, **pad)

    _ttk.Label(top, text="Filter:").grid(row=2, column=0, sticky="w", **pad)
    filter_row = _ttk.Frame(top)
    filter_row.grid(row=2, column=1, sticky="w")
    _ttk.Entry(filter_row, textvariable=var_filter, width=30).pack(side="left")
    _ttk.Label(filter_row, text="  Workers:").pack(side="left")
    _ttk.Spinbox(filter_row, from_=1, to=64, textvariable=var_workers, width=5).pack(side="left")
    _backend_hint = " (numba)" if _al._CPP_NUMERICS_BACKEND == "numba" else " (einsum)"
    _ttk.Checkbutton(filter_row, text=f"  C++ Numerik{_backend_hint}",
                     variable=var_cpp_num).pack(side="left", padx=(12, 0))
    _ttk.Checkbutton(filter_row, text="  Verbose",
                     variable=var_verbose).pack(side="left", padx=(8, 0))
    _ttk.Checkbutton(filter_row, text="  Compare Inputs",
                     variable=var_compare_inputs).pack(side="left", padx=(8, 0))
    _ttk.Checkbutton(filter_row, text="  Free Analysis",
                     variable=var_free_analysis).pack(side="left", padx=(8, 0))
    top.columnconfigure(1, weight=1)

    # Buttons
    btn_row = _ttk.Frame(root, padding=(8, 0, 8, 4))
    btn_row.grid(row=1, column=0, sticky="w")
    cancel_ev = threading.Event()
    btn_start  = _ttk.Button(btn_row, text="▶ Start")
    btn_cancel = _ttk.Button(btn_row, text="■ Cancel", state="disabled")
    btn_start.pack(side="left", padx=4)
    btn_cancel.pack(side="left")

    # Progress
    _ttk.Progressbar(root, variable=var_prog, maximum=100).grid(
        row=2, column=0, sticky="ew", padx=8, pady=(2, 0))
    _ttk.Label(root, textvariable=var_status, anchor="w").grid(
        row=3, column=0, sticky="ew", padx=8)

    # Output
    out = _tkst.ScrolledText(root, font=("Courier New", 9), state="disabled",
                              wrap="none", height=22)
    out.grid(row=4, column=0, sticky="nsew", padx=8, pady=(2, 8))
    root.rowconfigure(4, weight=1)

    # ── Thread → GUI queue ────────────────────────────────────────────────────
    q = _queue_mod.Queue()

    def append_text(s):
        out.config(state="normal")
        out.insert("end", s + "\n")
        out.see("end")
        out.config(state="disabled")

    def poll():
        try:
            while True:
                msg = q.get_nowait()
                kind = msg[0]
                if kind == "line":
                    append_text(msg[1])
                elif kind == "progress":
                    done, tot = msg[1], msg[2]
                    var_prog.set(100.0 * done / max(1, tot))
                    var_status.set(f"{done} / {tot}")
                elif kind == "done":
                    result = msg[1]
                    btn_start.config(state="normal")
                    btn_cancel.config(state="disabled")
                    if result:
                        ok, mis, skip, nf, errs = result
                        var_status.set(
                            f"Done — OK={ok}  MISMATCH={mis}  SKIP={skip}  NOT_FOUND={nf}  ERR={errs}")
                    else:
                        var_status.set("Cancelled.")
                    var_prog.set(100.0)
        except _queue_mod.Empty:
            pass
        root.after(100, poll)

    # ── Start / Cancel ────────────────────────────────────────────────────────
    def start():
        log_p   = var_log.get().strip()
        organ_p = var_organ.get().strip()
        if not os.path.isfile(log_p):
            _tkmb.showerror("File not found", f"Log CSV not found:\n{log_p}")
            return
        if not os.path.isfile(organ_p):
            _tkmb.showerror("File not found", f"Organ file not found:\n{organ_p}")
            return

        _save_settings({
            "log": log_p, "organ": organ_p,
            "filter": var_filter.get(),
            "workers": var_workers.get(),
            "cpp_numerics": var_cpp_num.get(),
            "verbose": var_verbose.get(),
            "compare_inputs": var_compare_inputs.get(),
            "free_analysis":  var_free_analysis.get(),
        })

        out.config(state="normal")
        out.delete("1.0", "end")
        out.config(state="disabled")
        var_prog.set(0.0)
        var_status.set("Running…")
        cancel_ev.clear()
        btn_start.config(state="disabled")
        btn_cancel.config(state="normal")

        def worker():
            result = run_analysis(
                log_p, organ_p,
                filter_str=var_filter.get(),
                workers=var_workers.get(),
                verbose=var_verbose.get(),
                compare_inputs=var_compare_inputs.get(),
                free_analysis=var_free_analysis.get(),
                line_cb=lambda s: q.put(("line", s)),
                progress_cb=lambda d, t: q.put(("progress", d, t)),
                cancel_event=cancel_ev,
            )
            q.put(("done", result))

        threading.Thread(target=worker, daemon=True).start()

    def cancel():
        cancel_ev.set()
        var_status.set("Cancelling…")
        btn_cancel.config(state="disabled")

    btn_start.config(command=start)
    btn_cancel.config(command=cancel)

    root.after(100, poll)
    root.mainloop()


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    # GUI mode: no arguments → open GUI (if tkinter available)
    if len(sys.argv) == 1 and _GUI_AVAILABLE:
        run_gui()
        return

    ap = argparse.ArgumentParser(
        description="Compare GO LUT output vs Python v2 algorithm.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("log",   nargs="?", default="/tmp/go_lut_verify.csv",
                    help="Path to go_lut_verify.csv (default: /tmp/go_lut_verify.csv)")
    ap.add_argument("organ", nargs="?", default=None,
                    help="Path to .organ ODF file (omit to launch GUI)")
    ap.add_argument("--filter",    "-f", metavar="PATTERN",
                    help="Only process entries whose label contains PATTERN")
    ap.add_argument("--max-pipes", "-n", type=int, default=0, metavar="N",
                    help="Stop after N pipes (0 = all)")
    ap.add_argument("--verbose",   "-v", action="store_true",
                    help="Print all LUT points including matching ones")
    ap.add_argument("--sim-only",  "-s", action="store_true",
                    help="Skip LUT comparison, only test simulator outputs")
    ap.add_argument("--cpp-numerics", "-c", action="store_true",
                    help="Use C++-style scalar float32 NDP (pre-normalized, "
                         "no BLAS) for Python comparison")
    ap.add_argument("--workers",   "-j", type=int, default=os.cpu_count() or 4, metavar="N",
                    help=f"Parallel worker threads (default: {os.cpu_count() or 4})")
    ap.add_argument("--compare-inputs", "-i", action="store_true",
                    help="Show free-analyzer vs GO input comparison (T_float, "
                         "autocorr window) for every pipe, independent of LUT diffs")
    ap.add_argument("--free-analysis", "-F", action="store_true",
                    help="Run full analyze_pipe path from WAV smpl chunk (no GO "
                         "overrides) and compare resulting LUT against GO — shows "
                         "whether GO's input computation (T_float, crossfade, loop "
                         "bounds) diverges from the Python analyzer")
    args = ap.parse_args()

    # If organ still missing after parsing, try GUI
    if args.organ is None:
        if _GUI_AVAILABLE:
            run_gui()
            return
        print("Usage: verify_lut.py [csv] organ.organ [options]", file=sys.stderr)
        sys.exit(1)

    if not os.path.isfile(args.log):
        print(f"Log file not found: {args.log}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(args.organ):
        print(f"Organ file not found: {args.organ}", file=sys.stderr)
        sys.exit(1)

    if args.cpp_numerics:
        _al.set_cpp_numerics(True)

    result = run_analysis(
        args.log, args.organ,
        filter_str=args.filter or "",
        max_pipes=args.max_pipes,
        workers=args.workers,
        verbose=args.verbose,
        sim_only=args.sim_only,
        compare_inputs=args.compare_inputs,
        free_analysis=args.free_analysis,
    )
    if result is None:
        sys.exit(2)
    ok, mis, skip, notfound, errs = result
    sys.exit(0 if mis == 0 and errs == 0 else 1)


if __name__ == "__main__":
    main()
