#!/usr/bin/env python3
"""Cross-correlate L/R to find the sample delay that maximizes correlation,
then check whether L - R_shifted has meaningfully smaller magnitude/energy
than the naive L - R (delay=0) - i.e. whether phase-aligning before
differencing would actually help compression, before building a full
encode/decode pipeline for it.

Status: written but never run - retracted before use. Even if phase
alignment had helped, it would only improve compression RATIO, not
decode RUNTIME, which was the actual priority for this investigation
(see simd_multivoice_check.cpp for where the real win was). Kept here
only as a record of an idea considered and consciously not pursued.
"""
import sys
import wave
import numpy as np


def load_wav(path):
    w = wave.open(path, "rb")
    n, ch, sw = w.getnframes(), w.getnchannels(), w.getsampwidth()
    raw = w.readframes(n)
    assert sw == 3 and ch == 2
    arr = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
    v = arr[:, 0].astype(np.int32) | (arr[:, 1].astype(np.int32) << 8) | (
        arr[:, 2].astype(np.int32) << 16
    )
    v = np.where(v & 0x800000, v - (1 << 24), v)
    L = v[0::2].astype(np.float64)
    R = v[1::2].astype(np.float64)
    return L, R


def best_delay(L, R, max_delay=500):
    """Find integer delay d (R shifted by d relative to L) maximizing
    normalized cross-correlation, searching [-max_delay, max_delay]."""
    n = min(len(L), len(R))
    # Use a representative middle chunk for speed on long files.
    chunk = min(n, 200000)
    start = (n - chunk) // 2
    Lc = L[start : start + chunk]
    Rc = R[start : start + chunk]

    best_d, best_corr = 0, -1.0
    for d in range(-max_delay, max_delay + 1):
        if d >= 0:
            a, b = Lc[d:], Rc[: len(Rc) - d]
        else:
            a, b = Lc[: len(Lc) + d], Rc[-d:]
        if len(a) < 1000:
            continue
        denom = np.sqrt(np.sum(a * a) * np.sum(b * b))
        if denom == 0:
            continue
        corr = np.sum(a * b) / denom
        if corr > best_corr:
            best_corr, best_d = corr, d
    return best_d, best_corr


def main():
    for path in sys.argv[1:]:
        L, R = load_wav(path)
        d, corr0 = best_delay(L, R, max_delay=0)  # correlation AT delay=0
        dbest, corrbest = best_delay(L, R, max_delay=500)

        n = min(len(L), len(R))
        if dbest >= 0:
            diff0 = L[:n] - R[:n]
            diffbest = L[dbest:n] - R[: n - dbest]
        else:
            diff0 = L[:n] - R[:n]
            diffbest = L[: n + dbest] - R[-dbest:n]

        rms0 = np.sqrt(np.mean(diff0**2))
        rmsbest = np.sqrt(np.mean(diffbest**2))
        rmsL = np.sqrt(np.mean(L**2))

        name = path.split("/")[-1]
        print(
            f"{name:14s} corr@0={corr0:+.4f}  best_delay={dbest:+4d} "
            f"corr@best={corrbest:+.4f} | "
            f"RMS(L-R)={rms0:12.1f}  RMS(L-R_shifted)={rmsbest:12.1f}  "
            f"reduction={100*(1-rmsbest/max(rms0,1e-9)):5.1f}%  "
            f"(RMS(L)={rmsL:12.1f} for scale)"
        )


if __name__ == "__main__":
    main()
