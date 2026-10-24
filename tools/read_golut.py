#!/usr/bin/env python3
"""
read_golut.py — GrandOrgue Release Alignment LUT cache inspector.

Reads a .release-align.golut file and prints its content in human-readable
form or as CSV.  The CSV output is compatible with the --export-csv flag of
analyze_lut_v48.py so that you can diff GO's output against Python's.

Usage:
    python3 read_golut.py  <file.release-align.golut>            # human-readable
    python3 read_golut.py  <file.release-align.golut>  --csv     # CSV comparison

Phase 8 test cases that can be run with this tool:
    1. Normal pipe:        Release present in CSV, plausible {loop_pos, r} values.
    2. Mixture/Sesquialtera with drift:
                           Release absent from CSV (releaseMap = -1).
    3. Unstable deep pedal: Release absent (not stabilized).
    4. Bad-quality LUT:    Release absent (score_min < 0.35 or R < 0.75 or
                           gap_count > 8).
    5. ODF changed:        Run with wrong organ → GOLutCacheReader::Load returns
                           false silently; GO falls back to live computation.
    6. Algorithm mismatch: Change GOLUT_ALGORITHM_VERSION and check that the
                           Reader rejects the old file silently.
    7. Corrupted file:     Truncate the file by 1 byte; Reader returns false.
"""

import struct
import sys
import os

# Must match constants in GOLutCacheFile.h
GOLUT_MAGIC             = b'GORALC1\x00'
GOLUT_FORMAT_VERSION    = 1
GOLUT_ALGORITHM_VERSION = 1


def read_golut(path: str) -> dict:
    """
    Parse a .golut file.  Returns a dict with keys:
      fmt_ver, alg_ver, odf_hash, release_count, lut_count,
      criteria (dict), release_map (list[int]), luts (list[list[(int,int)]])
    Raises ValueError on magic/structural errors.
    """
    with open(path, 'rb') as f:
        raw = f.read()

    pos = 0

    def read(n):
        nonlocal pos
        chunk = raw[pos:pos + n]
        if len(chunk) != n:
            raise ValueError(
                f"Unexpected EOF at offset {pos}: wanted {n} bytes, got {len(chunk)}")
        pos += n
        return chunk

    # ── Magic ────────────────────────────────────────────────────────────────
    magic = read(8)
    if magic != GOLUT_MAGIC:
        raise ValueError(f"Bad magic: {magic!r}  (expected {GOLUT_MAGIC!r})")

    # ── Versions ─────────────────────────────────────────────────────────────
    fmt_ver, alg_ver = struct.unpack_from('<II', read(8))
    if fmt_ver != GOLUT_FORMAT_VERSION:
        raise ValueError(
            f"Format version mismatch: file={fmt_ver}, reader={GOLUT_FORMAT_VERSION}")
    if alg_ver != GOLUT_ALGORITHM_VERSION:
        raise ValueError(
            f"Algorithm version mismatch: file={alg_ver}, reader={GOLUT_ALGORITHM_VERSION}")

    # ── ODF hash ─────────────────────────────────────────────────────────────
    odf_hash = read(64).rstrip(b'\x00').decode('utf-8', errors='replace')

    # ── Counts ───────────────────────────────────────────────────────────────
    release_count, lut_count = struct.unpack_from('<II', read(8))

    # ── Generator criteria ───────────────────────────────────────────────────
    min_score, min_coh  = struct.unpack_from('<ff', read(8))
    max_gaps,           = struct.unpack_from('<I',  read(4))
    req_stab, use_drift = struct.unpack_from('<BB', read(2))
    read(2)  # padding

    criteria = {
        'min_score':             min_score,
        'min_coherence':         min_coh,
        'max_gap_fills':         max_gaps,
        'require_stabilization': bool(req_stab),
        'use_drift_fallback':    bool(use_drift),
    }

    # ── ReleaseMap ───────────────────────────────────────────────────────────
    release_map = list(struct.unpack_from(f'<{release_count}i',
                                          read(4 * release_count)))

    # ── LUT entries ──────────────────────────────────────────────────────────
    luts = []
    for li in range(lut_count):
        n_pts, = struct.unpack_from('<I', read(4))
        pts = []
        for _ in range(n_pts):
            loop_pos, = struct.unpack_from('<I', read(4))
            best_r,   = struct.unpack_from('<H', read(2))
            pts.append((loop_pos, best_r))
        luts.append(pts)

    # Validate releaseMap indices
    for i, idx in enumerate(release_map):
        if idx != -1 and (idx < 0 or idx >= lut_count):
            raise ValueError(
                f"ReleaseMap[{i}] = {idx} out of range [0, {lut_count})")

    return {
        'fmt_ver':       fmt_ver,
        'alg_ver':       alg_ver,
        'odf_hash':      odf_hash,
        'release_count': release_count,
        'lut_count':     lut_count,
        'criteria':      criteria,
        'release_map':   release_map,
        'luts':          luts,
    }


def print_human(data: dict):
    c = data['criteria']
    cached = sum(1 for x in data['release_map'] if x >= 0)

    print(f"Format version    : {data['fmt_ver']}")
    print(f"Algorithm version : {data['alg_ver']}")
    print(f"ODF hash          : {data['odf_hash']}")
    print(f"Release count     : {data['release_count']}")
    print(f"LUT count         : {data['lut_count']}")
    print(f"Cached releases   : {cached} / {data['release_count']}")
    print(f"Criteria          : min_score={c['min_score']:.2f}  "
          f"min_coherence={c['min_coherence']:.2f}  "
          f"max_gap_fills={c['max_gap_fills']}")
    print()

    for i, lut_idx in enumerate(data['release_map']):
        if lut_idx < 0:
            continue
        pts = data['luts'][lut_idx]
        print(f"  Release {i:5d}  LUT {lut_idx:4d}  ({len(pts)} points)")
        for loop_pos, best_r in pts:
            print(f"    loop_pos={loop_pos:9d}  best_r={best_r:5d}")


def print_csv(data: dict):
    """
    CSV format matching --export-csv in analyze_lut_v48.py:
      release_idx, point_idx, loop_pos, best_r
    """
    print("release_idx,point_idx,loop_pos,best_r")
    for i, lut_idx in enumerate(data['release_map']):
        if lut_idx < 0:
            continue
        for j, (loop_pos, best_r) in enumerate(data['luts'][lut_idx]):
            print(f"{i},{j},{loop_pos},{best_r}")


# ── Robustness test helpers (Phase 8) ─────────────────────────────────────────

def test_corrupted(path: str):
    """Verify that truncating the file makes read_golut() raise an error."""
    with open(path, 'rb') as f:
        original = f.read()
    truncated = path + '.truncated'
    with open(truncated, 'wb') as f:
        f.write(original[:-1])  # remove last byte
    try:
        read_golut(truncated)
        print(f"FAIL: truncated file should have raised ValueError")
    except (ValueError, struct.error) as e:
        print(f"OK: truncated file correctly rejected: {e}")
    finally:
        os.remove(truncated)


def test_wrong_magic(path: str):
    """Verify that a wrong magic causes rejection."""
    with open(path, 'rb') as f:
        original = f.read()
    bad = path + '.bad_magic'
    with open(bad, 'wb') as f:
        f.write(b'BADMAGIC' + original[8:])
    try:
        read_golut(bad)
        print(f"FAIL: bad-magic file should have raised ValueError")
    except ValueError as e:
        print(f"OK: bad-magic file correctly rejected: {e}")
    finally:
        os.remove(bad)


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]
    if not args or args[0] in ('-h', '--help'):
        print(__doc__)
        sys.exit(0)

    path = args[0]
    csv_mode  = '--csv'  in args
    test_mode = '--test' in args

    try:
        data = read_golut(path)
    except (ValueError, struct.error, OSError) as e:
        print(f"Error reading {path}: {e}", file=sys.stderr)
        sys.exit(1)

    if test_mode:
        test_corrupted(path)
        test_wrong_magic(path)
    elif csv_mode:
        print_csv(data)
    else:
        print_human(data)


if __name__ == '__main__':
    main()
