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

import os, sys, re, math, argparse, types, json, threading
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

# Patch analyze_lut's pruning globally with C++ style (once, before any threads).
# Will be applied after prune_cpp_style is defined below.


# ── Log parser ────────────────────────────────────────────────────────────────

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
                current["T_float"]       = float(kv.get("T_float", "0"))
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
                current["latest_loop_end"] = int(kv.get("latest_loop_end", "0"))
                current["loop_count"]      = int(kv.get("loop_count", "0"))
                current["attack_file"]     = kv.get("attack_file", "")

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
                # phase0,beam_id,r_samples,score,n_last
                if len(parts) >= 5:
                    current.setdefault("phase0", []).append({
                        "beam_id": int(parts[1]),
                        "r":       int(parts[2]),
                        "score":   float(parts[3]),
                        "n_last":  int(parts[4]),
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
                pipes.append({
                    "rank_name":        rank_name,
                    "midi_note":        midi_note,
                    "attack_path":      resolve(atk_rel),
                    "attack_paths":     all_attacks,
                    "release_path":     resolve(rel_path),
                    "harmonic_number":  harmonic,
                    "crossfade_len_ms": int(sec.get(
                        f"{pk}ReleaseCrossfadeLength", str(xfade_ms))),
                    "min_key_press_ms": prev_max_ms,
                    "max_key_press_ms": max_key_ms,
                    "organ_base":       organ_dir,
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


def get_position_for_correlation_cpp(loop_pos: int, pts: list,
                                     T_int: int, T_float: float) -> int:
    """C++-compatible replica of GetPositionForCorrelation.

    v2 LUT points store r in [0,2T) — no fold.  All modular arithmetic uses
    T2 = 2*T for v2 points, T for legacy points (flags==0).
    phi is still computed modulo T (loop phase within one period).
    """
    if not pts:
        return 0
    T = max(1, T_int)
    T_f = T_float if T_float > 0.0 else float(T)
    phi = int(round(math.fmod(loop_pos, T_f))) % T

    sorted_pts = sorted(pts, key=lambda p: p.loop_pos)
    # Detect v2 LUT (any point has is_jump attribute set)
    is_v2 = getattr(sorted_pts[-1], 'is_jump', None) is not None
    T2 = 2 * T if is_v2 else T

    if len(sorted_pts) == 1 or loop_pos <= sorted_pts[0].loop_pos:
        r_interp = int(sorted_pts[0].best_r)
    elif loop_pos >= sorted_pts[-1].loop_pos:
        r_interp = int(sorted_pts[-1].best_r)
    else:
        idx = 0
        while idx + 1 < len(sorted_pts) and sorted_pts[idx + 1].loop_pos <= loop_pos:
            idx += 1
        p0 = sorted_pts[idx]
        p1 = sorted_pts[idx + 1]
        t = (loop_pos - p0.loop_pos) / max(1, p1.loop_pos - p0.loop_pos)
        r0, r1 = int(p0.best_r), int(p1.best_r)

        if is_v2:
            is_jump = bool(getattr(p1, 'is_jump', False))
            if not is_jump:
                if getattr(p1, 'approach_up', True):
                    diff = (r1 - r0 + T2) % T2
                    if diff > T2 // 2: diff -= T2
                else:
                    bwd = (r0 - r1 + T2) % T2
                    diff = -bwd
                    if diff < -(T2 // 2): diff += T2
        else:
            # Legacy: shortest arc, T/4 jump heuristic
            diff = r1 - r0
            if diff >  T // 2: diff -= T
            if diff < -(T // 2): diff += T
            is_jump = abs(diff) > T // 4

        if is_jump:
            r_interp = r0 if t < 0.5 else r1
        else:
            r_signed = r0 + int(round(t * diff))
            r_interp = (r_signed % T2 + T2) % T2

    return (r_interp + phi) % T2


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


# ── Per-entry comparison ──────────────────────────────────────────────────────

def compare_entry(entry: dict, organ_path: str,
                  sim_only: bool = False, verbose: bool = False) -> dict:
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

    # Load WAVs (cached — same file shared by all release-time variants)
    try:
        atk_mono, sr = _load_wav_cached(chosen_attack_path)
        rel_mono, _  = _load_wav_cached(pipe_desc["release_path"])
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
        # C++-compatible sim with Python LUT
        py_r  = get_position_for_correlation_cpp(lp, py_lut,     T_int, T_float)
        # C++-compatible sim with GO LUT (tests only interpolation, not LUT)
        py_r2 = get_position_for_correlation_cpp(lp, go_pts_list, T_int, T_float)

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
        "diag_atk_len": _diag_atk_len,
        "diag_rel_len": _diag_rel_len,
        "diag_log_atk": _diag_log_atk,
        "diag_log_rel": _diag_log_rel,
        "diag_atk_path": chosen_attack_path,
        "diag_rel_path": pipe_desc["release_path"],
        "diag_py_loop_end": max((l[1] for l in loops), default=0) if loops else 0,
        "diag_go_loop_end": entry.get("latest_loop_end", 0),
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
                             sim_only=sim_only, verbose=verbose)

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

    for res in results:
        st  = res["status"]
        sev = res.get("severity", "none")
        rel = _rel_tag(res)
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
            # Phase-0 comparison: show for major diffs that have LUT divergences
            go_p0 = res.get("go_phase0", [])
            py_p0 = res.get("py_phase0", [])
            if sev == "major" and (go_p0 or py_p0) and res["lut_diffs"]:
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
            for s in res["simsrc_diffs"][:5]:
                emit(f"         SIM_LOGIC lp={s['loop_pos']:7d}  go={s['go_r']:4d} py={s['py_r']:4d}  Δ={s['dist']}")
            if len(res["simsrc_diffs"]) > 5:
                emit(f"         ... and {len(res['simsrc_diffs'])-5} more sim-logic diffs")

    minor_hint = f"  (minor diffs hidden — use --verbose to show)" if minor_mis > 0 and not verbose else ""
    emit(f"\nResult: {total} entries — "
         f"OK={ok}  MISMATCH={mis} (major={major_mis} minor={minor_mis})  "
         f"SKIP={skip}  NOT_FOUND={notfound}  ERR={errs}{minor_hint}")
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
    var_cpp_num     = _tk.BooleanVar(value=settings.get("cpp_numerics", False))
    var_verbose     = _tk.BooleanVar(value=settings.get("verbose",      False))

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
    )
    if result is None:
        sys.exit(2)
    ok, mis, skip, notfound, errs = result
    sys.exit(0 if mis == 0 and errs == 0 else 1)


if __name__ == "__main__":
    main()
