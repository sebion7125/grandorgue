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

import os, sys, re, math, argparse, types

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

            elif line.startswith("lut,") and current is not None:
                parts = line.split(",")
                # lut,n,loop_pos,best_r,approach_up,is_jump
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

    if current:
        entries.append(current)
    return entries


# ── Organ ODF lookup ──────────────────────────────────────────────────────────

# Cache so the ODF is parsed only once per organ path.
_organ_cache: dict = {}

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
    m = re.match(r"^(.+)\|midi=(\d+)$", label)
    if not m:
        return []
    rank_part = m.group(1).strip().lower()
    midi_key  = int(m.group(2))

    pipes = _parse_organ_fast(organ_path)
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

    Two differences vs Python's get_position_for_correlation:
    1. phi = round(loop_pos % T_float) % T  is ADDED to r_interp.
    2. is_jump → step function (snap to nearest endpoint), not linear interp.
    """
    if not pts:
        return 0
    T = max(1, T_int)
    T_f = T_float if T_float > 0.0 else float(T)
    phi = int(round(math.fmod(loop_pos, T_f))) % T

    sorted_pts = sorted(pts, key=lambda p: p.loop_pos)

    if len(sorted_pts) == 1 or loop_pos <= sorted_pts[0].loop_pos:
        r_interp = int(sorted_pts[0].best_r) % T
    elif loop_pos >= sorted_pts[-1].loop_pos:
        r_interp = int(sorted_pts[-1].best_r) % T
    else:
        # Binary search for segment
        idx = 0
        while idx + 1 < len(sorted_pts) and sorted_pts[idx + 1].loop_pos <= loop_pos:
            idx += 1
        p0 = sorted_pts[idx]
        p1 = sorted_pts[idx + 1]
        t = (loop_pos - p0.loop_pos) / max(1, p1.loop_pos - p0.loop_pos)
        r0, r1 = int(p0.best_r) % T, int(p1.best_r) % T

        is_valid = getattr(p1, 'is_jump', None) is not None  # v2 flags present
        if is_valid:
            is_jump = bool(getattr(p1, 'is_jump', False))
            if not is_jump:
                if getattr(p1, 'approach_up', True):
                    diff = (r1 - r0 + T) % T
                    if diff > T // 2: diff -= T
                else:
                    bwd = (r0 - r1 + T) % T
                    diff = -bwd
                    if diff < -(T // 2): diff += T
            # else diff unused
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
            r_interp = (r_signed % T + T) % T

    return (r_interp + phi) % T


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


# Apply global patch now that prune_cpp_style is defined.
_al._prune_lut_points = lambda pts, T, tol=None: prune_cpp_style(pts, T)


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

    if T_int < 16:
        return {"label": label, "status": "skip_short_T",
                "lut_diffs": [], "sim_diffs": [], "simsrc_diffs": []}

    # Find matching releases in ODF
    pipes = find_pipes_for_label(organ_path, label)
    if not pipes:
        return {"label": label, "status": "not_found",
                "lut_diffs": [], "sim_diffs": [], "simsrc_diffs": []}

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

    # Load WAVs
    try:
        atk_mono, sr, _, _ = _al.read_wav_mono_float(pipe_desc["attack_path"])
        rel_mono, _,  _, _ = _al.read_wav_mono_float(pipe_desc["release_path"])
    except Exception as e:
        return {"label": label, "status": f"wav_error:{e}",
                "lut_diffs": [], "sim_diffs": [], "simsrc_diffs": []}

    # smpl loop points from attack WAV
    smpl   = _al.parse_smpl_chunk(pipe_desc["attack_path"])
    loops  = smpl.get("loops", [])
    loop_start, loop_end = (loops[0][0], loops[0][1]) if loops else (0, len(atk_mono) - 1)

    # C++ uses loop_section.GetLength() as the attack buffer length.
    # This matches loop_end - loop_start + 1 (or the full WAV if no smpl loops).
    # Truncate Python's array to the same length so n_total agrees.
    log_loop_len = entry["loop_len"]
    if log_loop_len > 0 and len(atk_mono) != log_loop_len:
        atk_mono = atk_mono[:log_loop_len]

    # Time-window constraints
    log_sr     = entry["sample_rate"]
    min_sample = int(entry["min_ms"] * log_sr / 1000) if entry["min_ms"] > 0 else 0
    max_sample = int(entry["max_ms"] * log_sr / 1000) if entry["max_ms"] > 0 else None

    # Run Python v2 with T_float/T_int from GO log.
    # _prune_lut_points is already globally patched to prune_cpp_style.
    try:
        py_lut, _ = _al.compute_lut_v2(
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
        )
    except Exception as e:
        return {"label": label, "status": f"compute_error:{e}",
                "lut_diffs": [], "sim_diffs": [], "simsrc_diffs": []}

    go_lut_dict = {p["n"]: p for p in entry["lut"]}
    py_lut_dict = {p.n: p    for p in py_lut}

    def _fold(r: int) -> int:
        """Fold release offset into [0, T_int) — matches C++ lut_commit folding."""
        return int(r) % T_int if T_int > 0 else int(r)

    # ── LUT comparison ────────────────────────────────────────────────────────
    # C++ stores best_r folded in [0, T).
    # Python LutPoint.best_r is unfolded in [0, 2*T) — fold before comparing.
    lut_diffs = []
    if not sim_only:
        for n in sorted(set(go_lut_dict) | set(py_lut_dict)):
            go_pt = go_lut_dict.get(n)
            py_pt = py_lut_dict.get(n)
            if go_pt is None:
                lut_diffs.append({"n": n, "issue": "GO_MISSING",
                                  "go_r": None, "py_r": _fold(py_pt.best_r)})
            elif py_pt is None:
                lut_diffs.append({"n": n, "issue": "PY_MISSING",
                                  "go_r": go_pt["best_r"], "py_r": None})
            else:
                py_r_folded = _fold(py_pt.best_r)
                dist = circ_dist(go_pt["best_r"], py_r_folded, T_int)
                flags_ok = (go_pt["approach_up"] == py_pt.approach_up
                            and go_pt["is_jump"]  == py_pt.is_jump)
                if dist > 1 or not flags_ok:
                    lut_diffs.append({
                        "n": n,
                        "issue": ("MISMATCH_r" if dist > 1 else "") +
                                 ("_flags" if not flags_ok else ""),
                        "go_r":  go_pt["best_r"],  "py_r":  py_r_folded,
                        "dist":  dist,
                        "go_up": go_pt["approach_up"], "py_up": py_pt.approach_up,
                        "go_jmp":go_pt["is_jump"],     "py_jmp":py_pt.is_jump,
                    })

    # ── Simulator comparison ──────────────────────────────────────────────────
    # sim_diffs:    C++ sim (GO LUT)  vs  Python C++-compat sim (Python LUT)
    # simsrc_diffs: C++ sim (GO LUT)  vs  Python C++-compat sim (GO LUT)
    #   → simsrc_diffs isolates interpolation-logic differences only
    sim_diffs    = []
    simsrc_diffs = []
    go_pts_list  = go_lut_to_lutpoints(go_lut_dict)

    for s in entry["sim"]:
        lp   = s["loop_pos"]
        go_r = s["r_interp"]
        # C++-compatible sim with Python LUT
        py_r  = get_position_for_correlation_cpp(lp, py_lut,     T_int, T_float)
        # C++-compatible sim with GO LUT (tests only interpolation, not LUT)
        py_r2 = get_position_for_correlation_cpp(lp, go_pts_list, T_int, T_float)

        if circ_dist(go_r, py_r, T_int) > 1:
            sim_diffs.append({"loop_pos": lp, "go_r": go_r, "py_r": py_r,
                              "dist": circ_dist(go_r, py_r, T_int)})
        if circ_dist(go_r, py_r2, T_int) > 1:
            simsrc_diffs.append({"loop_pos": lp, "go_r": go_r, "py_r": py_r2,
                                 "dist": circ_dist(go_r, py_r2, T_int)})

    status = "OK" if not lut_diffs and not sim_diffs and not simsrc_diffs else "MISMATCH"
    return {
        "label":       label,
        "status":      status,
        "go_pts":      len(go_lut_dict),
        "py_pts":      len(py_lut_dict),
        "lut_diffs":   lut_diffs,
        "sim_diffs":   sim_diffs,
        "simsrc_diffs":simsrc_diffs,
    }


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description="Compare GO LUT output vs Python v2 algorithm.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("log",   nargs="?", default="/tmp/go_lut_verify.csv",
                    help="Path to go_lut_verify.csv (default: /tmp/go_lut_verify.csv)")
    ap.add_argument("organ", help="Path to .organ ODF file")
    ap.add_argument("--filter",    "-f", metavar="PATTERN",
                    help="Only process entries whose label contains PATTERN")
    ap.add_argument("--max-pipes", "-n", type=int, default=0, metavar="N",
                    help="Stop after N pipes (0 = all)")
    ap.add_argument("--verbose",   "-v", action="store_true",
                    help="Print all LUT points including matching ones")
    ap.add_argument("--sim-only",  "-s", action="store_true",
                    help="Skip LUT comparison, only test simulator outputs")
    ap.add_argument("--workers",   "-j", type=int, default=os.cpu_count() or 4, metavar="N",
                    help=f"Parallel worker threads (default: {os.cpu_count() or 4})")
    args = ap.parse_args()

    if not os.path.isfile(args.log):
        print(f"Log file not found: {args.log}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(args.organ):
        print(f"Organ file not found: {args.organ}", file=sys.stderr)
        sys.exit(1)

    entries = parse_verify_log(args.log)
    print(f"Parsed {len(entries)} pipe-release entries from {args.log}")

    if args.filter:
        entries = [e for e in entries if args.filter.lower() in e["label"].lower()]
        print(f"After filter '{args.filter}': {len(entries)} entries")

    if args.max_pipes:
        entries = entries[:args.max_pipes]

    import concurrent.futures

    organ_path = args.organ
    sim_only   = args.sim_only
    verbose    = args.verbose

    def _run(entry):
        return compare_entry(entry, organ_path, sim_only=sim_only, verbose=verbose)

    ok = mis = skip = notfound = errs = 0
    results = []

    n_workers = max(1, args.workers)
    if n_workers == 1:
        results = [_run(e) for e in entries]
    else:
        futures_to_idx = {}
        ordered = [None] * len(entries)
        with concurrent.futures.ThreadPoolExecutor(max_workers=n_workers) as pool:
            for i, e in enumerate(entries):
                futures_to_idx[pool.submit(_run, e)] = i
            for fut in concurrent.futures.as_completed(futures_to_idx):
                ordered[futures_to_idx[fut]] = fut.result()
        results = ordered

    for res in results:
        st = res["status"]

        if st == "OK":
            ok += 1
            if args.verbose:
                print(f"  OK     {res['label']:50s} go={res['go_pts']:3d} py={res['py_pts']:3d} pts")
        elif st.startswith("skip"):
            skip += 1
        elif st == "not_found":
            notfound += 1
            print(f"  ???    {res['label']}  (not found in ODF)")
        elif "error" in st.lower():
            errs += 1
            print(f"  ERR    {res['label']}: {st}")
        else:
            mis += 1
            has_lut  = bool(res["lut_diffs"])
            has_sim  = bool(res["sim_diffs"])
            has_ssrc = bool(res["simsrc_diffs"])
            tags = []
            if has_lut:  tags.append(f"LUT:{len(res['lut_diffs'])}")
            if has_ssrc: tags.append(f"SIM_LOGIC:{len(res['simsrc_diffs'])}")
            if has_sim and not has_ssrc: tags.append(f"SIM_FROM_LUT:{len(res['sim_diffs'])}")
            print(f"  DIFF   {res['label']:50s} go={res.get('go_pts','?'):3} py={res.get('py_pts','?'):3}  [{', '.join(tags)}]")
            for d in res["lut_diffs"][:10]:
                if d.get("issue","").startswith("MISMATCH") or d.get("issue","") in ("GO_MISSING","PY_MISSING"):
                    if d["go_r"] is None or d["py_r"] is None:
                        print(f"         LUT n={d['n']:4d}  {d['issue']}  r={d['go_r'] if d['go_r'] is not None else d['py_r']}")
                    else:
                        print(f"         LUT n={d['n']:4d}  Δr={d.get('dist',0):3d}  "
                              f"go_r={d['go_r']:4d} py_r={d['py_r']:4d}  "
                              f"up={d.get('go_up')}/{d.get('py_up')}  "
                              f"jmp={d.get('go_jmp')}/{d.get('py_jmp')}")
            if len(res["lut_diffs"]) > 10:
                print(f"         ... and {len(res['lut_diffs'])-10} more LUT diffs")
            for s in res["simsrc_diffs"][:5]:
                print(f"         SIM_LOGIC lp={s['loop_pos']:7d}  go={s['go_r']:4d} py={s['py_r']:4d}  Δ={s['dist']}")
            if len(res["simsrc_diffs"]) > 5:
                print(f"         ... and {len(res['simsrc_diffs'])-5} more sim-logic diffs")

    total = ok + mis + skip + notfound + errs
    print(f"\nResult: {total} entries — "
          f"OK={ok}  MISMATCH={mis}  SKIP={skip}  NOT_FOUND={notfound}  ERR={errs}")
    sys.exit(0 if mis == 0 and errs == 0 else 1)


if __name__ == "__main__":
    main()
