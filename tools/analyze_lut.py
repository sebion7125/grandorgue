#!/usr/bin/env python3
"""
GrandOrgue LUT Analysis Tool
Analysiert die Korrelations-LUT-Qualität für alle Pfeifen eines Sample-Sets.

Erfordert: Python 3.8+, tkinter, matplotlib, numpy
Aufruf:    python3 analyze_lut.py [pfad/zur/orgel.organ]
"""

import os
import re
import sys
import struct
import threading
import concurrent.futures
import os as _os
import queue
import math
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from dataclasses import dataclass, field
from typing import Optional
import numpy as np

try:
    import matplotlib
    matplotlib.use("TkAgg")
    import matplotlib.pyplot as plt
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
    from matplotlib.figure import Figure
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False


# ─── Konstanten (aus GOSoundReleaseAlignTable.cpp) ────────────────────────────
DENSE_STEP  = 6
MAX_DENSE_N = 100
STABLE_WIN  = 4
N_SPARSE    = 5
MAX_TOTAL   = 30
# Removed: CORR_MIXTURE_HARMONIC_THRESHOLD = 48
# Now using CorrIsOctaveStop logic: only power-of-2 HarmonicNumbers are pure
# octave stops (8=8', 16=4', 32=2', 4=16', ...) and skip autocorrelation.
# Non-power-of-2 (24=2⅔', 40=1⅗', 48=1⅓', mixtures) always use autocorr.
def corr_is_octave_stop(harmonic_number: int) -> bool:
    return harmonic_number > 0 and (harmonic_number & (harmonic_number - 1)) == 0
SCORE_WARN  = 0.5   # Korrelationsscore unter dem eine Warnung erscheint
SCORE_BAD   = 0.2

# v64: YIN-CMNDF-Refinement nutzt globales ndp_all, vermeidet Integer-Rundung am Suchrand.
# v63: YIN-CMNDF normalisiert ab Lag 1, verhindert 2T-Schätzung bei Oktav-Stops.
# v57: DC-Entfernung vor Hann-Fenster in estimate_period_by_autocorr.
#      Ohne DC-Removal blaehte der Gleichanteil alle NDP-Werte Richtung 1
#      auf → YIN fand T/2 statt T. Entspricht ChatGPT-Referenzskript.
#      Diagnose-Felder cmndf_at_T_half/T/2T im Detail-Panel angezeigt.
# v56: YIN-CMNDF Periodendetektion: findet erste starke Periodizitaet statt
#      globalem NDP-Maximum — loest T/2-Problem fuer Mixturen mit gerader Harmonik.
#      Fallback auf Lag-Penalty-NDP wenn kein YIN-Valley gefunden.
# v55: Hann-Fenster in estimate_period_by_autocorr (Seitenkeulendaempfung).
#      Score-Worker crossfade window = crossfade_len_samples (wie compute_lut).
# v54: Lag-Strafterm (alpha=0.10) statt Sub-Harmonischen-Check: NDP(lag) *= (1 - 0.10*lag/max_p).
#      Bevorzugt kuerze Perioden wenn NDP(T) ≈ NDP(2T), ohne Mixturen faelschlich zu halbieren.
# v52: NDP-Fenster = crossfade_len (statt min(crossfade,2*T)).
#      estimate_period_by_autocorr: Sub-Harmonischen-Check (div 2/3/4, 94%-Schwelle)
#      verhindert 2*T-Fehlbestimmung bei gerader Harmonik.
# v51: Aliquote/Mixturen: Autokorrelations-Suchbereich unabhängig vom smpl-Chunk.
#      max_p = sr//20 (deckt bis 20 Hz ab), kein HarmonicNumber/smpl-Pitch-Einfluss.
# v50: Bugfix: Loop-Punkte werden jetzt VOR der Autokorrelation aus dem smpl-Chunk
#      gelesen. Vorher: loop_start=0 (Default) → Autokorrelation lief im Anblase-
#      Transient → falsches T bei Aliquoten/Mixturen (z.B. T=96 statt ~177).
# v49: Downsampling-Checkbox in der Toolbar (entspricht GOSettingsOptions::CorrLutDownsampling,
#      Default ON). Toter Code nach return in compute_lut() entfernt.
# v46: Zusätzliche Qualitätskriterien für Legacy-Fallback.
# Eine formal stabilisierte LUT kann trotzdem unbrauchbar sein, wenn
# sie nur durch viele Gap-Fill-Punkte zusammengeflickt wird, schlechte
# Scores hat oder die best_r-Werte kreisstatistisch breit streuen.
LEGACY_SCORE_MIN_THRESHOLD = 0.35
LEGACY_COHERENCE_THRESHOLD = 0.75
LEGACY_PHASE3_MAX          = 8

# v13: Drift-Erkennung für Mixturen/Sesquialtera
DRIFT_FIT_WIN = 12
DRIFT_MAX_RESID_FACTOR = 1.0 # Residuum <= stable_thresh * Faktor

# v10: Fold-Akzeptanz-Parameter
FOLD_ACCEPT_RATIO    = 0.85
FOLD_SEARCH_RADIUS_D = 2


# ─── Datenklassen ─────────────────────────────────────────────────────────────

@dataclass
class LutPoint:
    n:          int     # Periodenindex
    loop_pos:   int     # Sample-Position im Attack
    best_r:     int     # Effektiver Release-Offset / Phase
    best_score: float   # Korrelationsscore beim besten r
    phase:      str     # "dense", "sparse", "gap", "drift"
    raw_r:      int   = 0
    raw_score:  float = 0.0
    folded:     bool  = False
    fold_ratio: float = 1.0


@dataclass
class PipeAnalysis:
    organ_base:    str
    rank_name:     str
    midi_note:     int
    perspective:   str
    release_type:  str
    attack_path:   str
    release_path:  str
    harmonic_number: int = 8

    # Ergebnisse
    T_float:       float = 0.0
    T_int:         int   = 0
    sample_rate:   int   = 0
    loop_start:    int   = 0
    loop_end:      int   = 0
    loop_len:      int   = 0
    release_len:   int   = 0
    n_total:       int   = 0
    crossfade_len_samples: int = 0

    min_key_press_ms: Optional[int] = None
    max_key_press_ms: Optional[int] = None
    is_shortest_release: bool = False  # True für das Release mit kleinster max_key_press_ms
    min_sample:   int   = 0
    max_sample:   Optional[int] = None

    # YIN diagnostics: CMNDF values at T/2, T, 2T (NaN if not computed)
    cmndf_at_T_half: float = float('nan')
    cmndf_at_T:      float = float('nan')
    cmndf_at_2T:     float = float('nan')

    drift_mode: bool = False
    drift_per_period: float = 0.0
    drift_residual: float = 0.0
    max_interp_gap_n: int = 0
    dense_step_used: int = DENSE_STEP

    lut_points:    list = field(default_factory=list)   # List[LutPoint]
    stable_at_n:   Optional[int] = None
    stabilized:    bool = False
    phase3_count:  int  = 0
    atk_rms:       float = 0.0
    rel_rms:       float = 0.0
    amplitude_ratio: float = 0.0
    error:         Optional[str] = None
    legacy_fallback: bool = False  # GO nutzt Legacy statt Korrelations-LUT
    legacy_reason: str = ""          # "short_period", "drift", "instabil", "bad_lut"

    # Erweiterte Metriken fuer Datenanalyse
    n_dense:       int   = 0    # Anzahl Phase-1-Punkte
    n_sparse:      int   = 0    # Anzahl Phase-2-Punkte
    n_gap:         int   = 0    # Anzahl Phase-3-Punkte
    score_mean:    float = 0.0  # Durchschnittsscore steady-state
    score_min:     float = 0.0  # Schlechtester Score steady-state
    score_max:     float = 0.0  # Bester Score steady-state
    bestr_coherence: float = 0.0  # Kreisstatistik R ∈ [0,1]: 1=stabil ein Ast, 0=instabil/mehrere Äste
    bestr_mean:    float = 0.0  # Mittelwert best_r (steady-state)
    transient_end_n: int = 0    # n ab dem steady-state beginnt
    freq_hz:       float = 0.0  # berechnete Pfeifenfrequenz
    midi_note_str: str   = ""   # z.B. "GIS3"
    atk_frames:    int   = 0    # Laenge Attack-WAV in Samples
    rel_frames:    int   = 0    # Laenge Release-WAV in Samples

    @property
    def severity(self) -> int:
        """0=ok, 1=warn, 2=bad"""
        if self.legacy_fallback:
            return 0
        if self.error:
            return 2
        if not self.stabilized:
            return 2
        scores = [p.best_score for p in self.lut_points if p.phase in ("sparse","gap")]
        if scores and min(scores) < SCORE_BAD:
            return 2
        # stable_at_n > 60 nur für das kürzeste Release prüfen —
        # das ist das Release mit min_key_press_ms=0.
        # Längere Releases fangen erst spät an, stable_at_n ist dann normal.
        if self.is_shortest_release and self.stable_at_n is not None and self.stable_at_n > 60:
            return 1
        if scores and min(scores) < SCORE_WARN:
            return 1
        if self.phase3_count > 3:
            return 1
        return 0

    @property
    def severity_label(self) -> str:
        if self.legacy_fallback and self.legacy_reason == "drift":
            return "Legacy Drift"
        if self.legacy_fallback and self.legacy_reason == "instabil":
            return "Legacy instabil"
        if self.legacy_fallback and self.legacy_reason.startswith("bad_lut"):
            return "Legacy schlechte LUT"
        if self.legacy_fallback and self.legacy_reason == "short_period":
            return "Legacy T<16"
        if self.legacy_fallback:
            return "Legacy"
        return ["OK", "Warn", "Fehler"][self.severity]

    @property
    def severity_tag(self) -> str:
        """Tag-Name für tkinter Treeview Zeilenfarbe."""
        if self.error:
            return "sev_error"
        if self.legacy_fallback and self.legacy_reason == "drift":
            return "sev_drift"
        if self.legacy_fallback and self.legacy_reason == "instabil":
            return "sev_legacy_warn"
        if self.legacy_fallback and self.legacy_reason.startswith("bad_lut"):
            return "sev_legacy_badlut"
        if self.legacy_fallback:
            return "sev_legacy"
        return ["sev_ok", "sev_warn", "sev_bad"][self.severity]


# ─── WAV-Hilfsfunktionen ──────────────────────────────────────────────────────

def _parse_wav_chunks(path: str) -> dict:
    """
    Parst WAV-Datei direkt (ohne wave.open), toleriert fmt_size > 16.
    Gibt dict mit: nch, sr, sw (bytes/sample), data_bytes, n_frames zurück.
    Unterstützt PCM (format=1) und IEEE Float (format=3).
    """
    with open(path, "rb") as f:
        raw = f.read()

    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"Keine gültige WAV-Datei: {path}")

    result = {}
    pos = 12
    while pos < len(raw) - 8:
        cid  = raw[pos:pos+4]
        size = struct.unpack_from("<I", raw, pos+4)[0]
        chunk_data = raw[pos+8 : pos+8+size]

        if cid == b"fmt ":
            fmt_code = struct.unpack_from("<H", chunk_data, 0)[0]
            result["fmt_code"] = fmt_code          # 1=PCM, 3=float, 65534=extensible
            result["nch"] = struct.unpack_from("<H", chunk_data, 2)[0]
            result["sr"]  = struct.unpack_from("<I", chunk_data, 4)[0]
            result["bps"] = struct.unpack_from("<H", chunk_data, 14)[0]  # bits per sample
            result["sw"]  = result["bps"] // 8                           # bytes per sample
            # Extensible: echter Sub-Format steckt ab Offset 24
            if fmt_code == 65534 and size >= 40:
                sub = struct.unpack_from("<H", chunk_data, 24)[0]
                result["fmt_code"] = sub

        elif cid == b"data":
            result["data_offset"] = pos + 8
            result["data_size"]   = size

        pos += 8 + size + (size % 2)

    if "nch" not in result or "data_offset" not in result:
        raise ValueError(f"fmt oder data chunk fehlt: {path}")

    nch   = result["nch"]
    sw    = result["sw"]
    nf    = result["data_size"] // (nch * sw)
    result["n_frames"]  = nf
    result["raw_bytes"] = raw  # komplettes File im Speicher
    return result


def read_wav_mono_float(path: str, max_samples: int = None) -> tuple:
    """Liest WAV, gibt (mono_float32_array, sample_rate, n_frames, n_channels) zurück.
    Toleriert fmt_size != 16 (z.B. 18 oder 20 bei manchen 24-bit WAVs)."""
    r   = _parse_wav_chunks(path)
    nch = r["nch"]
    sr  = r["sr"]
    sw  = r["sw"]
    nf  = r["n_frames"]
    fmt = r["fmt_code"]

    if max_samples:
        nf = min(nf, max_samples)

    byte_start = r["data_offset"]
    byte_end   = byte_start + nf * nch * sw
    raw        = r["raw_bytes"][byte_start:byte_end]

    if fmt == 3:
        # IEEE Float
        if sw == 4:
            s = np.frombuffer(raw, dtype=np.float32).copy()
        elif sw == 8:
            s = np.frombuffer(raw, dtype=np.float64).astype(np.float32)
        else:
            raise ValueError(f"Unbekannte Float-Breite {sw*8} bit: {path}")
        norm = 1.0
    elif sw == 3:
        # 24-bit PCM
        arr = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        s = (arr[:,0].astype(np.int32)
             | (arr[:,1].astype(np.int32) << 8)
             | (arr[:,2].astype(np.int32) << 16))
        s = np.where(s >= 2**23, s - 2**24, s).astype(np.float32)
        norm = float(2**23)
    elif sw == 2:
        s = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
        norm = float(2**15)
    elif sw == 4:
        s = np.frombuffer(raw, dtype=np.int32).astype(np.float32)
        norm = float(2**31)
    elif sw == 1:
        s = np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0
        norm = 128.0
    else:
        raise ValueError(f"Unbekannte Sample-Breite {sw*8} bit: {path}")

    # Mono-Mix aller Kanäle
    s    = s.reshape(-1, nch)
    mono = s.mean(axis=1) / norm
    return mono, sr, nf, nch


def parse_smpl_chunk(path: str) -> dict:
    """Liest smpl-Chunk aus WAV: midi_note, pitch_frac, loops."""
    result = {"midi_note": None, "pitch_frac": 0, "loops": []}
    try:
        with open(path, "rb") as f:
            data = f.read()
        pos = 12
        while pos < len(data) - 8:
            cid  = data[pos:pos+4]
            size = struct.unpack_from("<I", data, pos+4)[0]
            if cid == b"smpl" and size >= 36:
                result["midi_note"]  = struct.unpack_from("<I", data, pos+20)[0]
                result["pitch_frac"] = struct.unpack_from("<I", data, pos+24)[0]
                n_loops = struct.unpack_from("<I", data, pos+36)[0]
                for i in range(min(n_loops, 8)):
                    off = pos + 44 + i * 24
                    if off + 16 <= len(data):
                        s = struct.unpack_from("<I", data, off+8)[0]
                        e = struct.unpack_from("<I", data, off+12)[0]
                        result["loops"].append((s, e))
                break
            pos += 8 + size + (size % 2)
    except Exception:
        pass
    return result


def normalized_dot_product(a: np.ndarray, b: np.ndarray,
                            offset: int, window: int) -> float:
    """Pearson-Korrelation zwischen a[0:window] und b[offset:offset+window]."""
    if offset + window > len(b):
        return -2.0
    aw = a[:window]
    bw = b[offset:offset+window]
    na = np.linalg.norm(aw)
    nb = np.linalg.norm(bw)
    if na < 1e-12 or nb < 1e-12:
        return 0.0
    return float(np.dot(aw / na, bw / nb))


def best_corr_vectorized(lw: np.ndarray, release_mono: np.ndarray,
                          r_max: int, window_len: int,
                          T_int: int) -> tuple:
    """
    Vektorisierte Suche nach dem besten Korrelationsoffset in [0, r_max).
    Gibt (best_r, best_score, raw_r, raw_score, folded, fold_ratio) zurück.
    """
    if r_max + window_len > len(release_mono):
        r_max = max(1, len(release_mono) - window_len)

    na = np.linalg.norm(lw)
    if na < 1e-12:
        return 0, 0.0, 0, 0.0, False, 1.0
    lw_n = lw / na

    from numpy.lib.stride_tricks import as_strided
    s = release_mono.strides[0]
    rel_mat = as_strided(release_mono,
                          shape=(r_max, window_len),
                          strides=(s, s))

    norms = np.linalg.norm(rel_mat, axis=1)
    norms = np.where(norms < 1e-12, 1.0, norms)
    scores = rel_mat.dot(lw_n) / norms

    raw_idx = int(np.argmax(scores))
    raw_score = float(scores[raw_idx])

    folded_idx = raw_idx % T_int
    lo = max(0, folded_idx - FOLD_SEARCH_RADIUS_D)
    hi = min(r_max, folded_idx + FOLD_SEARCH_RADIUS_D + 1)
    folded_score = float(np.max(scores[lo:hi])) if hi > lo else raw_score
    fold_ratio = folded_score / (raw_score + 1e-12)

    if fold_ratio >= FOLD_ACCEPT_RATIO:
        best_r = int(lo + np.argmax(scores[lo:hi]))
        return best_r, folded_score, raw_idx, raw_score, True, fold_ratio
    return raw_idx, raw_score, raw_idx, raw_score, False, fold_ratio

def circ_dist(a: int, b: int, T: int) -> int:
    d = abs(int(a) - int(b))
    if d > T // 2:
        d = T - d
    return d


def shortest_circular_delta(a: float, b: float, T: int) -> float:
    """Kürzeste gerichtete Differenz b-a auf dem Kreis [0,T)."""
    d = float(b) - float(a)
    half = T / 2.0
    while d > half:
        d -= T
    while d < -half:
        d += T
    return d


def unwrap_phase_points(points, T: int):
    """Gibt (ns, unwrapped best_r mod T) für LUT-Punkte zurück."""
    if not points:
        return np.array([], dtype=float), np.array([], dtype=float)
    ns = np.array([p.n for p in points], dtype=float)
    phases = [float(p.best_r % T) for p in points]
    vals = [phases[0]]
    for prev, cur in zip(phases, phases[1:]):
        vals.append(vals[-1] + shortest_circular_delta(prev, cur, T))
    return ns, np.array(vals, dtype=float)


def fit_linear_drift(points, T: int):
    """Lineare Regression auf unwrapped Phase. Rückgabe: slope, intercept, max_resid."""
    if len(points) < 2:
        return 0.0, 0.0, float('inf')
    ns, vals = unwrap_phase_points(points, T)
    a, b = np.polyfit(ns, vals, 1)
    pred = a * ns + b
    resid = float(np.max(np.abs(vals - pred))) if len(vals) else float('inf')
    return float(a), float(b), resid


def refine_peak_parabolic(y: np.ndarray, i: int) -> float:
    """Subsample-Position eines Maximums per Parabelinterpolation."""
    if i <= 0 or i >= len(y) - 1:
        return float(i)
    ym1 = float(y[i - 1])
    y0  = float(y[i])
    yp1 = float(y[i + 1])
    denom = ym1 - 2.0 * y0 + yp1
    if abs(denom) < 1e-12:
        return float(i)
    delta = 0.5 * (ym1 - yp1) / denom
    if abs(delta) > 1.0:
        return float(i)
    return float(i) + delta


def estimate_period_by_autocorr(samples: np.ndarray,
                                  min_period: int,
                                  max_period: int,
                                  yin_threshold: float = 0.15) -> tuple:
    """YIN-style CMNDF period estimator (Nachbau EstimatePeriodByAutocorr).

    Finds the FIRST lag where the cumulative-mean normalised difference dips
    below yin_threshold — the true fundamental, not T/2 or 2T.
    Falls back to penalised NDP if no YIN valley is found.
    """
    N = len(samples)
    if N < max_period * 2:
        return float(min_period), {'cmndf_half': float('nan'), 'cmndf_T': float('nan'), 'cmndf_2T': float('nan')}

    # DC removal then Hann window (matches ChatGPT reference: seg -= mean before FFT).
    # Removing DC prevents the mean offset from inflating sub-harmonic ACF peaks.
    s    = samples.astype(np.float32) - float(np.mean(samples))
    hann = 0.5 * (1.0 - np.cos(2.0 * np.pi * np.arange(N) / (N - 1)))
    x    = s * hann.astype(np.float32)
    W    = N - max_period
    ref_n = x[:W] / (np.linalg.norm(x[:W]) + 1e-12)

    lags_all = np.arange(1, max_period + 1)
    ndp_all  = np.empty(len(lags_all), dtype=np.float64)
    for i, lag in enumerate(lags_all):
        sh = x[lag:lag + W]
        nrm = np.linalg.norm(sh)
        ndp_all[i] = float(np.dot(ref_n, sh / nrm)) if nrm > 1e-12 else 0.0

    # YIN CMNDF: d'[i]=1-NDP[i]; cmndf[i]=d'[i]/mean(d'[0..i]) ab Lag 1
    d_prime_all = 1.0 - ndp_all
    cumsum_all  = np.cumsum(d_prime_all)
    cmndf_all   = np.where(
        cumsum_all > 0,
        d_prime_all * np.arange(1, len(lags_all) + 1) / cumsum_all,
        1.0,
    )

    search_start = max(1, min_period)
    search_stop  = max_period
    lags    = np.arange(search_start, search_stop + 1)
    ndp_arr = ndp_all[search_start - 1:search_stop]
    cmndf   = cmndf_all[search_start - 1:search_stop]

    # First local minimum below threshold
    in_valley  = False
    best_cmndf = 2.0
    yin_lag    = 0
    for i in range(len(cmndf)):
        c = cmndf[i]
        if c < yin_threshold:
            if not in_valley or c < best_cmndf:
                best_cmndf = c;  yin_lag = int(lags[i]);  in_valley = True
            else:
                break
        elif in_valley:
            break
    if in_valley:
        abs_idx = int(yin_lag - 1)
        refined_abs_idx = refine_peak_parabolic(ndp_all, abs_idx)
        result = 1.0 + refined_abs_idx
    else:
        # Fallback: penalised NDP fuer Auswahl, echtes NDP fuer Subsample-Refinement
        weighted = ndp_arr * (1.0 - 0.10 * lags / max_period)
        best_idx = int(np.argmax(weighted))
        abs_idx = int((search_start - 1) + best_idx)
        refined_abs_idx = refine_peak_parabolic(ndp_all, abs_idx)
        result = 1.0 + refined_abs_idx

    # Diagnostic: CMNDF values at T/2, T, 2T for the RETURNED period.
    def _cmndf_at(lag: int) -> float:
        if lag < 1 or lag > max_period:
            return float('nan')
        i = lag - 1
        return float(cmndf_all[i]) if 0 <= i < len(cmndf_all) else float('nan')

    result_i = int(round(result))
    diag = {
        'cmndf_half': _cmndf_at(result_i // 2),
        'cmndf_T':    _cmndf_at(result_i),
        'cmndf_2T':   _cmndf_at(result_i * 2),
    }
    return result, diag


# ─── LUT-Algorithmus (Python-Nachbau) ────────────────────────────────────────

def compute_lut(attack_mono: np.ndarray, release_mono: np.ndarray,
                T_float: float, T_int: int,
                crossfade_len_samples: int,
                harmonic_number: int,
                loop_start: int, loop_end: int,
                min_sample: int = 0,
                max_sample: Optional[int] = None,
                downsampling: bool = True) -> tuple:
    """
    Nachbau von ComputeCorrelationLut. Rückgabe: (points, meta)
    v24: Klassischer Dense-Loop für alle Pfeifen.
         Drift-Erkennung nur als Diagnostik — bei Drift: leere LUT, Legacy-Fallback.
    """
    loop_len    = loop_end - loop_start + 1
    release_len = len(release_mono)
    window_len  = crossfade_len_samples

    meta = {
        "stabilized": False, "stable_at_n": None,
        "drift_mode": False, "drift_per_period": 0.0,
        "drift_residual": 0.0, "max_interp_gap_n": 0,
        "dense_step_used": DENSE_STEP,
    }

    if window_len < 4 or loop_len < window_len or release_len < window_len:
        return [], meta
    r_max = min(2 * T_int, release_len - window_len)
    if r_max == 0 or int(loop_len / T_float) < 4:
        return [], meta

    ds           = (min(4, T_int // 500) if T_int >= 500 else 1) if downsampling else 1
    atk_full_len = len(attack_mono)
    n_total      = max(1, int((atk_full_len - window_len) / T_float))

    min_sample = max(0, min_sample)
    if max_sample is not None:
        max_sample = max(min_sample + 1, max_sample)
    n_start = max(1, int(math.ceil(min_sample / T_float)))
    n_end   = n_total if max_sample is None else min(n_total, int(math.ceil(max_sample / T_float)) + 2)
    if n_start >= n_end:
        n_start, n_end = 1, n_total

    loop_needed  = min(int(round((n_end - 1) * T_float)) + window_len, atk_full_len)
    loop_seg     = attack_mono[:loop_needed:ds].astype(np.float32)
    release_ds   = release_mono[:r_max + window_len + 1 : ds].astype(np.float32)
    window_len_d = max(4, window_len // ds)
    r_max_d      = max(1, r_max // ds)

    def corr_at(n: int, phase: str) -> LutPoint:
        if n < n_start or n >= n_end:
            return LutPoint(n=n, loop_pos=0, best_r=0, best_score=-2.0, phase=phase)
        cs   = int(round(n * T_float))
        cs_d = cs // ds
        if cs_d + window_len_d > len(loop_seg):
            return LutPoint(n=n, loop_pos=cs, best_r=0, best_score=-2.0, phase=phase)
        lw = loop_seg[cs_d:cs_d + window_len_d]
        best_r_d, best_s, raw_r_d, raw_s, folded, fold_ratio = best_corr_vectorized(
            lw, release_ds, r_max_d, window_len_d, max(1, T_int // ds))
        best_r = best_r_d * ds
        raw_r  = raw_r_d  * ds
        if folded:
            best_r = best_r % T_int
        return LutPoint(n=n, loop_pos=cs, best_r=best_r, best_score=best_s,
                        phase=phase, raw_r=raw_r, raw_score=raw_s,
                        folded=folded, fold_ratio=fold_ratio)

    # Tighter tolerance for non-octave stops (aliquots/mixtures), matching GO.
    stable_thresh = max(T_int // (8 if corr_is_octave_stop(harmonic_number) else 6), 4)
    points: list  = []

    # ── Kurzer Loop ───────────────────────────────────────────────────────────
    if n_total <= 30:
        span = max(0, n_end - n_start)
        for i in range(min(10, span)):
            p = n_start + i * max(1, (span - 1) // 9)
            if p < n_end:
                points.append(corr_at(p, "dense"))
        points = [p for p in points if p.best_score > -1.5]
        meta["stabilized"] = True
        return points, meta

    # ── Phase 1: Dense ────────────────────────────────────────────────────────
    dense_start = max(n_start, 5)
    # dense_step dynamisch: so dass mindestens STABLE_WIN+1 Punkte in den
    # verfügbaren Bereich passen. Bei kurzen Releases (rel00220 mit wenig n)
    # kann DENSE_STEP=6 zu wenige Punkte liefern.
    available_n = max(1, n_end - dense_start)
    dense_step  = min(DENSE_STEP, max(1, available_n // (STABLE_WIN + 1)))
    meta["dense_step_used"] = dense_step

    dense_stop  = min(n_end, dense_start + dense_step * (STABLE_WIN + 2))
    stable_at_n = None

    for n in range(dense_start, dense_stop, dense_step):
        pt = corr_at(n, "dense")
        if pt.best_score > -1.5:
            points.append(pt)
        if len(points) >= STABLE_WIN:
            ok = all(
                circ_dist(points[k].best_r % T_int, points[k+1].best_r % T_int, T_int) <= stable_thresh
                for k in range(len(points) - STABLE_WIN, len(points) - 1)
            )
            if ok:
                stable_at_n = n
                break

    if not points and n_start < n_end:
        points.append(corr_at(min(n_end - 1, dense_start), "dense"))

    # ── Drift-Diagnose (nur Diagnostik, kein Einfluss auf LUT-Berechnung) ────
    # Für die ersten DRIFT_FIT_WIN Dense-Punkte: linearer Fit auf unwrapped best_r
    drift_slope, drift_resid = 0.0, 0.0
    diag_pts = [p for p in points if p.best_score > -1.5][:DRIFT_FIT_WIN]
    if len(diag_pts) >= 4:
        a, b, resid = fit_linear_drift(diag_pts, T_int)
        drift_slope = a
        drift_resid = resid

    # Drift ist problematisch wenn r sich innerhalb von STABLE_WIN * DENSE_STEP
    # Perioden um mehr als T/4 bewegt — dann kann die normale LUT nicht folgen.
    # slope_thresh = T / (4 * STABLE_WIN * DENSE_STEP)
    drift_slope_thresh = T_int / (4.0 * STABLE_WIN * DENSE_STEP)
    drift_detected = (abs(drift_slope) >= drift_slope_thresh and
                      drift_resid <= stable_thresh * DRIFT_MAX_RESID_FACTOR)

    if drift_detected:
        # Drift erkannt → LUT-Punkte trotzdem behalten für Visualisierung,
        # aber drift_mode=True signalisiert GO/analyze_pipe: Legacy verwenden.
        meta["drift_mode"]       = True
        meta["drift_per_period"] = drift_slope
        meta["drift_residual"]   = drift_resid
        meta["stabilized"]       = False
        meta["stable_at_n"]      = None
        # Weiter mit Phase 2+3 für vollständige Visualisierung

    meta["stabilized"]       = stable_at_n is not None
    meta["stable_at_n"]      = stable_at_n
    meta["drift_per_period"] = drift_slope
    meta["drift_residual"]   = drift_resid

    # ── Phase 2: Sparse ───────────────────────────────────────────────────────
    last_n = int(round(points[-1].loop_pos / T_float)) if points else dense_start
    if last_n + 1 < n_end:
        n_rem = min(N_SPARSE, MAX_TOTAL - len(points))
        for i in range(1, n_rem + 1):
            n = last_n + i * (n_end - 1 - last_n) // n_rem
            if n > last_n and n < n_end:
                pt = corr_at(n, "sparse")
                if pt.best_score > -1.5:
                    points.append(pt)

    # ── Phase 3: Gap-Fill ─────────────────────────────────────────────────────
    gap_thresh = T_int // 4
    idx = 0
    while idx + 1 < len(points) and len(points) < MAX_TOTAL:
        gap = circ_dist(points[idx].best_r % T_int, points[idx+1].best_r % T_int, T_int)
        if gap > gap_thresh:
            na = int(round(points[idx].loop_pos / T_float))
            nb = int(round(points[idx+1].loop_pos / T_float))
            nm = (na + nb) // 2
            if nm > na and nm < nb:
                pt = corr_at(nm, "gap")
                if pt.best_score > -1.5:
                    points.insert(idx + 1, pt)
                continue
        idx += 1

    points = [p for p in points if p.best_score > -1.5]
    return points, meta


# ─── ODF-Parser ──────────────────────────────────────────────────────────────

def parse_organ_file(organ_path: str) -> list:
    """
    Parst .organ-Datei und gibt Liste von Pipe-Deskriptoren zurück.
    Jeder Eintrag: dict mit rank_name, midi_note, attack_path, releases,
                   harmonic_number, crossfade_len_ms, perspective
    """
    organ_dir = os.path.dirname(os.path.abspath(organ_path))
    pipes = []

    try:
        with open(organ_path, encoding="utf-8", errors="replace") as f:
            content = f.read()
    except Exception as e:
        return []

    # Finde alle Rank-Sektionen
    sections = {}
    current_section = None
    for line in content.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            current_section = line[1:-1]
            sections[current_section] = {}
        elif "=" in line and current_section:
            k, _, v = line.partition("=")
            sections[current_section][k.strip()] = v.strip()

    # Finde Rank-Sektionen
    rank_pattern = re.compile(r"^Rank\d+$")
    for sec_name, sec_data in sections.items():
        if not rank_pattern.match(sec_name):
            continue

        rank_name = sec_data.get("Name", sec_name)
        # Globaler HarmonicNumber-Default (kann per Pipe ueberschrieben werden)
        global_harmonic = int(sec_data.get("HarmonicNumber", "8"))
        xfade_ms  = int(sec_data.get("ReleaseCrossfadeLength", "0"))
        first_key = int(sec_data.get("FirstMidiNoteNumber",
                        sec_data.get("FirstAccessibleKeyMIDINoteNumber", "36")))
        pipe_count = int(sec_data.get("NumberOfLogicalPipes", "0"))
        if pipe_count == 0:
            pipe_count = sum(1 for k in sec_data if re.match(r"^Pipe\d+$", k))

        for pipe_idx in range(1, pipe_count + 1):
            pipe_key = f"Pipe{pipe_idx:03d}"
            attack_rel = sec_data.get(pipe_key)
            if not attack_rel:
                continue
            midi_note = first_key + pipe_idx - 1
            # Per-Pipe HarmonicNumber hat Vorrang vor globalem Wert
            harmonic = int(sec_data.get(f"{pipe_key}HarmonicNumber",
                                        str(global_harmonic)))

            # Releases sammeln
            releases = {}
            for k, v in sec_data.items():
                m = re.match(rf"^{pipe_key}Release(\d+)$", k)
                if m:
                    rel_idx = int(m.group(1))
                    releases[rel_idx] = v

            # Pfad auflösen
            def resolve(rel_path):
                # Prüfe ob rel_path direkt existiert
                full = os.path.join(organ_dir, rel_path.replace("\\", os.sep).replace("/", os.sep))
                if os.path.isfile(full):
                    return full
                # Suche in übergeordneten Verzeichnissen (OrganInstallationPackages)
                parts = rel_path.replace("\\", "/").split("/")
                for root, dirs, files in os.walk(organ_dir):
                    candidate = os.path.join(root, parts[-1])
                    if os.path.isfile(candidate):
                        # Prüfe ob der Pfad stimmt
                        tail = os.path.join(*parts[-3:]) if len(parts) >= 3 else parts[-1]
                        if candidate.replace("\\", "/").endswith(tail.replace("\\", "/")):
                            return candidate
                return None

            atk_path = resolve(attack_rel)
            if not atk_path:
                continue

            # Perspektive aus Pfad ableiten
            parts = attack_rel.replace("\\", "/").split("/")
            perspective = "default"
            for p in parts:
                if re.match(r"^\d{2}-", p):
                    perspective = p
                    break

            release_infos = []
            for rel_idx, rel_path in releases.items():
                rel_full = resolve(rel_path)
                if not rel_full:
                    continue
                rel_parent = os.path.basename(os.path.dirname(rel_full))
                mkpt_entry = sec_data.get(
                    f"{pipe_key}Release{rel_idx:03d}MaxKeyPressTime", "")
                if mkpt_entry:
                    max_key_ms = int(mkpt_entry)
                else:
                    m = re.match(r"rel(\d+)", rel_parent)
                    max_key_ms = int(m.group(1)) if m else None
                if max_key_ms is not None and max_key_ms >= 99999:
                    max_key_ms = None
                release_infos.append({
                    "rel_idx": rel_idx,
                    "rel_path": rel_full,
                    "rel_type": rel_parent,
                    "max_key_ms": max_key_ms,
                })

            release_infos.sort(key=lambda info: info["max_key_ms"]
                               if info["max_key_ms"] is not None else float("inf"))
            prev_max_ms = 0
            for rel_idx_loop, info in enumerate(release_infos):
                rel_full = info["rel_path"]
                rel_parent = info["rel_type"]
                max_key_ms = info["max_key_ms"]
                min_key_ms = prev_max_ms
                if max_key_ms is not None:
                    prev_max_ms = max_key_ms

                pipes.append({
                    "rank_name":   rank_name,
                    "midi_note":   midi_note,
                    "perspective": perspective,
                    "release_type": rel_parent,
                    "attack_path": atk_path,
                    "release_path": rel_full,
                    "harmonic_number": harmonic,
                    "crossfade_len_ms": int(sec_data.get(
                        f"{pipe_key}ReleaseCrossfadeLength",
                        str(xfade_ms))),
                    "min_key_press_ms": min_key_ms,
                    "max_key_press_ms": max_key_ms,
                    "is_shortest_release": rel_idx_loop == 0,
                    "organ_base":  organ_dir,
                })
                if max_key_ms is None:
                    break

    return pipes


# ─── Analyse-Worker ───────────────────────────────────────────────────────────

def analyze_pipe(desc: dict) -> PipeAnalysis:
    pa = PipeAnalysis(
        organ_base=desc["organ_base"],
        rank_name=desc["rank_name"],
        midi_note=desc["midi_note"],
        perspective=desc["perspective"],
        release_type=desc["release_type"],
        attack_path=desc["attack_path"],
        release_path=desc["release_path"],
        harmonic_number=desc["harmonic_number"],
    )

    try:
        # smpl-Chunk lesen
        smpl = parse_smpl_chunk(pa.attack_path)
        if smpl["midi_note"] is None:
            pa.error = "Kein smpl-Chunk"
            return pa

        midi_note  = smpl["midi_note"]
        pitch_frac = smpl["pitch_frac"] / 2**32  # in Semitones
        # Frequenz direkt aus smpl-Chunk: MIDI-Note + Pitch-Fraction.
        # Der smpl-Chunk enthaelt die physikalische Schwingungsfrequenz
        # der aufgenommenen Pfeife — KEINE HarmonicNumber-Umrechnung noetig.
        # HN/8 braucht man nur wenn man von der ODF-Tastennote auf die
        # Frequenz schliesst (wie GO intern). Wir lesen die WAV direkt.
        # HarmonicNumber wird nur fuer den Autocorr-Schwellwert verwendet.
        freq_hz    = 440.0 * 2**((midi_note + pitch_frac - 69) / 12)

        # WAV laden
        atk_mono, sr, atk_frames, atk_ch = read_wav_mono_float(pa.attack_path)
        rel_mono, _,  rel_frames, rel_ch  = read_wav_mono_float(pa.release_path)

        pa.sample_rate = sr
        pa.T_float     = sr / freq_hz
        pa.T_int       = int(round(pa.T_float))
        pa.release_len = len(rel_mono)

        if pa.T_int < 16:
            pa.legacy_fallback = True
            pa.legacy_reason = "short_period"
            pa.error = None
            return pa

        # Loop-Punkte (muss vor Autokorrelation stehen — loop_mid braucht loop_start)
        loops = smpl["loops"]
        if loops:
            pa.loop_start = loops[0][0]
            pa.loop_end   = loops[0][1]
        else:
            pa.loop_start = 0
            pa.loop_end   = len(atk_mono) - 1
        pa.loop_len = pa.loop_end - pa.loop_start + 1

        # Periodenbestimmung per Autokorrelation fuer ALLE nicht-trivialen Stops.
        #
        # Nicht-Oktav (Aliquote/Mixturen): smpl-unabhaengiger Bereich [16, sr/20].
        #
        # Oktav-Stops (Zweierpotenz-HN): smpl gibt normalerweise die richtige Periode,
        # aber ein Mixtur-Rank mit geradem HN kann ungerade Obertöne enthalten, die
        # die echte Wellenformperiode auf 2*T_smpl verdoppeln. Pruefen mit Bereich
        # [T_smpl, 3*T_smpl] — YIN erkennt ob T_smpl oder 2*T_smpl korrekt ist.
        T_smpl = int(round(pa.T_float))
        if corr_is_octave_stop(pa.harmonic_number):
            min_p = T_smpl
            max_p = min(T_smpl * 3, sr // 20)
        else:
            min_p = 16
            max_p = sr // 20

        if max_p >= min_p * 2 and T_smpl >= 16:
            loop_mid = pa.loop_start + pa.loop_len // 2
            ac_window = max_p * 8
            autocorr_region = atk_mono[loop_mid:loop_mid + ac_window]
            if len(autocorr_region) >= max_p * 2:
                t_est, diag = estimate_period_by_autocorr(
                    autocorr_region, min_p, max_p)
                pa.T_float         = float(t_est)
                pa.T_int           = int(round(pa.T_float))
                pa.cmndf_at_T_half = diag['cmndf_half']
                pa.cmndf_at_T      = diag['cmndf_T']
                pa.cmndf_at_2T     = diag['cmndf_2T']

        pa.n_total  = int(pa.loop_len / pa.T_float)

        # Crossfade-Länge
        xfade_ms = desc.get("crossfade_len_ms", 0)
        pa.crossfade_len_samples = int(xfade_ms * sr / 1000) if xfade_ms > 0 else 2 * pa.T_int
        if pa.crossfade_len_samples == 0:
            pa.crossfade_len_samples = 2 * pa.T_int

        # Amplitudenverhältnis (Steady-State)
        ss_start = pa.loop_start
        ss_end   = min(ss_start + 50 * pa.T_int, len(atk_mono))
        pa.atk_rms = float(np.sqrt(np.mean(atk_mono[ss_start:ss_end]**2))) if ss_end > ss_start else 0.0
        rel_early  = rel_mono[:min(2000, len(rel_mono))]
        pa.rel_rms = float(np.sqrt(np.mean(rel_early**2))) if len(rel_early) > 0 else 0.0
        pa.amplitude_ratio = (pa.atk_rms / pa.rel_rms) if pa.rel_rms > 1e-9 else 0.0

        # LUT berechnen
        pa.min_key_press_ms = desc.get("min_key_press_ms")
        pa.max_key_press_ms = desc.get("max_key_press_ms")
        pa.is_shortest_release = bool(desc.get("is_shortest_release", False))
        pa.min_sample = int(pa.min_key_press_ms * sr / 1000) if pa.min_key_press_ms is not None else 0
        pa.max_sample = (int(pa.max_key_press_ms * sr / 1000)
                         if pa.max_key_press_ms is not None else None)

        pa.lut_points, lut_meta = compute_lut(
            attack_mono=atk_mono,
            release_mono=rel_mono,
            T_float=pa.T_float,
            T_int=pa.T_int,
            crossfade_len_samples=pa.crossfade_len_samples,
            harmonic_number=pa.harmonic_number,
            loop_start=pa.loop_start,
            loop_end=pa.loop_end,
            min_sample=pa.min_sample,
            max_sample=pa.max_sample,
            downsampling=desc.get("downsampling", True),
        )
        pa.stabilized = bool(lut_meta.get("stabilized", False))
        pa.stable_at_n = lut_meta.get("stable_at_n")
        pa.drift_mode = bool(lut_meta.get("drift_mode", False))
        pa.drift_per_period = float(lut_meta.get("drift_per_period", 0.0))
        pa.drift_residual = float(lut_meta.get("drift_residual", 0.0))
        pa.max_interp_gap_n = int(lut_meta.get("max_interp_gap_n", 0) or 0)
        pa.dense_step_used = int(lut_meta.get("dense_step_used", DENSE_STEP) or DENSE_STEP)

        if not pa.lut_points:
            pa.error = "Keine LUT-Punkte"
            return pa

        # Stabilisierung/Drift wurde bereits in compute_lut ausgewertet.
        dense_pts = [p for p in pa.lut_points if p.phase == "dense"]
        pa.phase3_count = sum(1 for p in pa.lut_points if p.phase == "gap")

        # ── Erweiterte Metriken ────────────────────────────────────────────
        pa.n_dense  = sum(1 for p in pa.lut_points if p.phase == "dense")
        pa.n_sparse = sum(1 for p in pa.lut_points if p.phase == "sparse")
        pa.n_gap    = pa.phase3_count
        pa.freq_hz  = freq_hz
        pa.atk_frames = atk_frames
        pa.rel_frames = rel_frames
        pa.midi_note_str = midi_to_name(pa.midi_note)

        # Steady-state Punkte: sparse + gap (nach Stabilisierung)
        ss_pts = [p for p in pa.lut_points if p.phase in ("sparse", "gap")]
        if ss_pts:
            scores_ss = [p.best_score for p in ss_pts]
            pa.score_mean = float(np.mean(scores_ss))
            pa.score_min  = float(np.min(scores_ss))
            pa.score_max  = float(np.max(scores_ss))

            # Standardabweichung von best_r (Zwei-Ast-Indikator)
            # Kreisstatistik: circ_mean dann circ_std
            rs = np.array([p.best_r for p in ss_pts], dtype=float)
            T  = pa.T_int
            # Kreismittelwert via sin/cos
            angles = rs / T * 2 * np.pi
            mean_sin = np.mean(np.sin(angles))
            mean_cos = np.mean(np.cos(angles))
            pa.bestr_mean = float(np.arctan2(mean_sin, mean_cos) % (2*np.pi) / (2*np.pi) * T)
            # Kreisstreuung: 0 = perfekt konzentriert, T/2 = maximal gestreut
            R = float(np.clip(np.sqrt(mean_sin**2 + mean_cos**2), 0.0, 1.0))
            pa.bestr_coherence = R  # 1=stabil, 0=instabil/mehrere Äste

        # Transient-Ende: n des letzten dense-Punkts
        if dense_pts:
            pa.transient_end_n = dense_pts[-1].n

        # v46: Legacy-Fallback nicht nur bei Drift/Instabilität, sondern auch
        # bei formal stabilisierter, aber qualitativ schlechter LUT.
        # Wichtig: erst NACH Berechnung von score_min, R und phase3_count.
        if pa.drift_mode:
            pa.legacy_fallback = True
            pa.legacy_reason = "drift"
        elif not pa.stabilized:
            pa.legacy_fallback = True
            pa.legacy_reason = "instabil"
        else:
            bad_reasons = []
            if pa.score_min and pa.score_min < LEGACY_SCORE_MIN_THRESHOLD:
                bad_reasons.append(f"score_min<{LEGACY_SCORE_MIN_THRESHOLD:.2f}")
            if pa.bestr_coherence and pa.bestr_coherence < LEGACY_COHERENCE_THRESHOLD:
                bad_reasons.append(f"R<{LEGACY_COHERENCE_THRESHOLD:.2f}")
            if pa.phase3_count > LEGACY_PHASE3_MAX:
                bad_reasons.append(f"gaps>{LEGACY_PHASE3_MAX}")
            if bad_reasons:
                pa.legacy_fallback = True
                pa.legacy_reason = "bad_lut:" + ",".join(bad_reasons)

    except Exception as e:
        pa.error = str(e)

    return pa


def circ_interp(r_a: int, r_b: int, t: float, T: int) -> int:
    """Kreislineare Interpolation zwischen r_a und r_b auf [0,T). Kürzester Weg."""
    delta = shortest_circular_delta(float(r_a), float(r_b), T)
    return int(round(r_a + delta * t)) % T


def get_position_for_correlation(loop_pos: int, lut_points: list, T: int) -> int:
    """Simuliert GOs GetPositionForCorrelation mit circ_interp und Step-Funktion.
    v47: Step-Funktion für |diff| > T/4 (kein Interpolieren über Branch-Grenze),
         entspricht dem GO-Runtime-Verhalten seit Branch-Consensus-Entfernung."""
    if not lut_points:
        return 0
    if loop_pos <= lut_points[0].loop_pos:
        return lut_points[0].best_r % T
    if loop_pos >= lut_points[-1].loop_pos:
        return lut_points[-1].best_r % T
    lo, hi = 0, len(lut_points) - 1
    while lo + 1 < hi:
        mid = (lo + hi) // 2
        if lut_points[mid].loop_pos <= loop_pos:
            lo = mid
        else:
            hi = mid
    pa_pt = lut_points[lo]
    pb_pt = lut_points[hi]
    span  = pb_pt.loop_pos - pa_pt.loop_pos
    if span <= 0:
        return pa_pt.best_r % T
    t = (loop_pos - pa_pt.loop_pos) / span
    r_a = pa_pt.best_r % T
    r_b = pb_pt.best_r % T
    # v47: Step-Funktion bei Branch-Abstand > T/4 (wie GO-Runtime).
    # Interpolation über Branch-Grenzen erzeugt Werte auf keinem Branch —
    # ein harter Sprung bei t=0.5 ist immer besser als ein sinnloser Mittelwert.
    diff = shortest_circular_delta(float(r_a), float(r_b), T)
    if abs(diff) > T / 4.0:
        return r_a if t < 0.5 else r_b
    return circ_interp(r_a, r_b, t, T)


# ─── Legacy Release Alignment (Nachbau GOSoundReleaseAlignTable) ─────────────

PHASE_ALIGN_DERIVATIVES  = 32
PHASE_ALIGN_AMPLITUDES   = 32
PHASE_ALIGN_MIN_FREQUENCY = 64   # Hz — bestimmt Fensterlänge


class LegacyAlignTable:
    """
    Nachbau von GOSoundReleaseAlignTable.
    Baut eine 32×32-Tabelle aus dem Release-WAV auf (Amplitude × Ableitung).
    Lookup: letzte 2 Attack-Samples → Release-Startposition.
    """

    def __init__(self, release_mono: np.ndarray, sample_rate: int):
        self._table = {}   # (deriv_bin, amp_bin) → release_start_sample
        self._max_val = 0.0
        self._build(release_mono, sample_rate)

    def _build(self, rel: np.ndarray, sr: int):
        search_len = sr // PHASE_ALIGN_MIN_FREQUENCY
        n = min(search_len, len(rel))
        if n < 2:
            return

        # Finde Max-Amplitude für Normalisierung
        max_val = float(np.max(np.abs(rel[:n]))) if n > 0 else 1.0
        if max_val < 1e-12:
            max_val = 1.0
        self._max_val = max_val

        table = {}
        for i in range(1, n):
            f_cur  = float(rel[i])
            f_prev = float(rel[i-1])
            deriv  = f_cur - f_prev
            amp    = f_cur

            # Quantisierung wie in GO: shift in [0..2*max-1], dann bin
            amp_shifted   = amp   + max_val
            deriv_shifted = deriv + max_val

            amp_bin   = int(amp_shifted   / (2.0 * max_val) * PHASE_ALIGN_AMPLITUDES)
            deriv_bin = int(deriv_shifted / (2.0 * max_val) * PHASE_ALIGN_DERIVATIVES)

            amp_bin   = max(0, min(PHASE_ALIGN_AMPLITUDES   - 1, amp_bin))
            deriv_bin = max(0, min(PHASE_ALIGN_DERIVATIVES  - 1, deriv_bin))

            key = (deriv_bin, amp_bin)
            if key not in table:
                table[key] = i + 1   # frühester Index (wie GO: i+1+start_position)

        # Leere Bins per Nachbarsuche füllen
        all_keys = set((d, a) for d in range(PHASE_ALIGN_DERIVATIVES)
                               for a in range(PHASE_ALIGN_AMPLITUDES))
        empty    = all_keys - set(table.keys())
        radius   = 1
        while empty and radius <= max(PHASE_ALIGN_DERIVATIVES, PHASE_ALIGN_AMPLITUDES):
            filled = set()
            for (d, a) in list(empty):
                for dd in range(-radius, radius+1):
                    for da in range(-radius, radius+1):
                        if abs(dd) != radius and abs(da) != radius:
                            continue
                        nd, na = d+dd, a+da
                        if (nd, na) in table:
                            table[(d, a)] = table[(nd, na)]
                            filled.add((d, a))
                            break
                    if (d, a) in filled:
                        break
            empty -= filled
            radius += 1

        self._table = table

    def get_position_for(self, attack_mono: np.ndarray, t_attack: int) -> int:
        """Gibt Release-Startposition zurück basierend auf letzten 2 Attack-Samples."""
        if t_attack < 2 or len(attack_mono) < 2:
            return 0
        t = min(t_attack, len(attack_mono) - 1)
        f_cur  = float(attack_mono[t])
        f_prev = float(attack_mono[t-1])
        f_mod  = f_cur  + f_prev   # Summe wie in GO (2 Kanäle summiert, hier mono)
        v_mod  = f_cur  - f_prev

        max_val = self._max_val if self._max_val > 1e-12 else 1.0
        amp_shifted   = f_mod + max_val
        deriv_shifted = v_mod + max_val

        amp_bin   = int(amp_shifted   / (2.0 * max_val) * PHASE_ALIGN_AMPLITUDES)
        deriv_bin = int(deriv_shifted / (2.0 * max_val) * PHASE_ALIGN_DERIVATIVES)
        amp_bin   = max(0, min(PHASE_ALIGN_AMPLITUDES   - 1, amp_bin))
        deriv_bin = max(0, min(PHASE_ALIGN_DERIVATIVES  - 1, deriv_bin))

        return self._table.get((deriv_bin, amp_bin), 0)


# ─── Crossfade-Kurven ─────────────────────────────────────────────────────────

def _crossfade_weights(mode: str, n: int) -> tuple:
    """Gibt (a_weights, b_weights) für n Samples zurück. a=Attack, b=Release."""
    t = np.linspace(0.0, 1.0, n, dtype=np.float32)
    half_pi = np.pi / 2.0
    if mode == "Linear":
        return 1.0 - t, t
    elif mode == "SinEqualPower":
        return np.cos(half_pi * t), np.sin(half_pi * t)
    elif mode == "Sin2":
        return np.cos(half_pi * t)**2, np.sin(half_pi * t)**2
    elif mode == "SqrtEqualPower":
        return np.sqrt(1.0 - t), np.sqrt(t)
    elif mode == "X2":
        return 1.0 - t**2, t**2
    else:  # Custom / fallback
        return np.zeros(n), np.ones(n)



class CrossfadeSimWindow:
    """
    Simulationsfenster: zeigt wie GO den Crossfade bei einer bestimmten
    Attack-Dauer durchführen würde.

    Modi:
    - Sofort: r aus LUT-Interpolation + Legacy-Tabelle
    - Nach "Berechnen": zusätzlich r direkt per argmax aus Score-Matrix
    """

    XFADE_MODES = ["Linear", "SinEqualPower", "Sin2", "SqrtEqualPower", "X2"]

    def __init__(self, parent, pa: "PipeAnalysis"):
        self.pa          = pa
        self._atk_mono   = None
        self._rel_mono   = None
        self._legacy_tbl = None
        self._score_mat  = None   # (N, r_max_d) — nach "Berechnen"
        self._score_ns   = None   # n-Werte zur Score-Matrix
        self._computing  = False
        self._stop_event = threading.Event()

        self.win = tk.Toplevel(parent)
        self.win.title(f"Crossfade-Simulation — {pa.rank_name} "
                       f"{midi_to_name(pa.midi_note)} / {pa.release_type}")
        self.win.geometry("1200x750")
        self.win.configure(bg=C_BG)
        self.win.protocol("WM_DELETE_WINDOW", self._on_close)

        self._build_ui()
        # WAVs und Legacy-Tabelle im Hintergrund laden
        threading.Thread(target=self._load_data, daemon=True).start()
        # Warnung bei Legacy-Fallback
        if pa.legacy_fallback:
            self.win.after(100, lambda: self._status.config(
                text="⚠ Legacy-Pfeife — NDP-Kurve zeigt LUT-Visualisierung, kein GO-Korrelations-Alignment",
                fg=C_WARN))

    def _on_close(self):
        self._stop_event.set()
        self.win.destroy()

    def _build_ui(self):
        pa = self.pa

        # ── Toolbar ───────────────────────────────────────────────────────────
        bar = tk.Frame(self.win, bg=C_BG3, pady=5)
        bar.pack(fill=tk.X)

        btn_kw = dict(bg=C_BTN, fg=C_TEXT, relief=tk.RAISED,
                      font=("Consolas", 9), padx=8, pady=3,
                      activebackground=C_BTN_ACT, cursor="hand2")

        # ── Attack-Dauer Controls ──────────────────────────────────────────────
        pa = self.pa
        sr = pa.sample_rate or 48000
        min_ms  = pa.min_key_press_ms or 0
        max_ms  = pa.max_key_press_ms or 2000
        min_smp = int(min_ms * sr / 1000)
        max_smp = int(max_ms * sr / 1000)
        self._sr = sr
        self._min_smp = min_smp
        self._max_smp = max_smp

        # interner Zustand: Sample-genau
        self._cur_smp = tk.IntVar(value=min_smp)

        # Grob-Slider (ms)
        tk.Label(bar, text="Attack:", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=(8,2))

        self._slider = tk.Scale(bar, from_=min_ms, to=max_ms,
                                resolution=1, orient=tk.HORIZONTAL, length=250,
                                bg=C_BG3, fg=C_TEXT, highlightthickness=0,
                                font=("Consolas", 8), showvalue=False)
        self._slider.set(min_ms)
        self._slider.pack(side=tk.LEFT, padx=2)
        self._slider.bind("<Motion>",        self._on_slider_move)
        self._slider.bind("<ButtonRelease>", self._on_slider_move)

        # ms-Feld
        tk.Label(bar, text="ms:", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=(6,1))
        self._ms_var = tk.StringVar(value=f"{min_ms:.3f}")
        ms_entry = tk.Entry(bar, textvariable=self._ms_var, width=9,
                            font=("Consolas", 9))
        ms_entry.pack(side=tk.LEFT, padx=1)
        ms_entry.bind("<Return>", self._on_ms_entry)
        ms_entry.bind("<FocusOut>", self._on_ms_entry)

        # Samples-Feld
        tk.Label(bar, text="smp:", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=(6,1))
        self._smp_var = tk.StringVar(value=str(min_smp))
        smp_entry = tk.Entry(bar, textvariable=self._smp_var, width=8,
                             font=("Consolas", 9))
        smp_entry.pack(side=tk.LEFT, padx=1)
        smp_entry.bind("<Return>", self._on_smp_entry)
        smp_entry.bind("<FocusOut>", self._on_smp_entry)

        # Step-Buttons: ±1 sample, ±10 samples, ±1ms
        btn_kw_step = dict(font=("Consolas", 9), bg=C_BTN, fg=C_TEXT,
                           relief=tk.RAISED, padx=3, pady=2,
                           activebackground=C_BTN_ACT)

        tk.Label(bar, text="Step:", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=(6,2))

        sr_ = pa.sample_rate or 48000
        ms_step = int(round(sr_ / 1000))  # samples per ms

        tk.Button(bar, text="«ms",  command=lambda: self._step_sample(-ms_step),
                  **btn_kw_step).pack(side=tk.LEFT, padx=1)
        tk.Button(bar, text="«10",  command=lambda: self._step_sample(-10),
                  **btn_kw_step).pack(side=tk.LEFT, padx=1)
        tk.Button(bar, text="◀1",   command=lambda: self._step_sample(-1),
                  **btn_kw_step).pack(side=tk.LEFT, padx=1)
        tk.Button(bar, text="▶1",   command=lambda: self._step_sample(+1),
                  **btn_kw_step).pack(side=tk.LEFT, padx=1)
        tk.Button(bar, text="10»",  command=lambda: self._step_sample(+10),
                  **btn_kw_step).pack(side=tk.LEFT, padx=1)
        tk.Button(bar, text="ms»",  command=lambda: self._step_sample(+ms_step),
                  **btn_kw_step).pack(side=tk.LEFT, padx=(1,4))

        # Pfeiltasten binden mit eigenem Key-Repeat (wie Texteditor)
        #
        # Wichtig:
        # Das Betriebssystem erzeugt beim Halten einer Taste selbst wiederholte
        # KeyPress-Events. Die alte Implementierung hat bei jedem solchen
        # KeyPress den after()-Timer gecancelt und wieder auf 600 ms gesetzt.
        # Ergebnis: der eigene Repeat konnte praktisch nie starten.
        #
        # Deshalb ignorieren wir wiederholte KeyPress-Events derselben Taste,
        # solange unser eigener Repeat aktiv ist.
        self._repeat_job = None
        self._repeat_release_job = None
        self._repeat_delta = 0
        self._repeat_key = None
        self._plotting = False
        self._plot_pending = False

        def _cancel_repeat_job():
            if self._repeat_job is not None:
                try:
                    self.win.after_cancel(self._repeat_job)
                except tk.TclError:
                    pass
                self._repeat_job = None

        def _cancel_release_job():
            if self._repeat_release_job is not None:
                try:
                    self.win.after_cancel(self._repeat_release_job)
                except tk.TclError:
                    pass
                self._repeat_release_job = None

        def _repeat_tick():
            # Während matplotlib zeichnet, blockiert der Tk-Mainloop ohnehin.
            # Nach dem Zeichnen läuft dieser Callback weiter. _plot() selbst
            # coalesced Zwischenwerte über _plot_pending.
            if self._repeat_key is None:
                self._repeat_job = None
                return
            self._step_sample(self._repeat_delta)
            self._repeat_job = self.win.after(100, _repeat_tick)

        def _key_press(key, delta):
            _cancel_release_job()

            # OS-Autorepeat: gleicher KeyPress kommt mehrfach. Nicht erneut
            # initialisieren, sonst wird der 600-ms-Delay immer wieder neu
            # gestartet.
            if self._repeat_key == key:
                return "break"

            # Wechsel zwischen Links/Rechts bzw. Modifier-Variante sauber
            # übernehmen.
            _cancel_repeat_job()
            self._repeat_key = key
            self._repeat_delta = delta

            # Erster Tastendruck wirkt sofort.
            self._step_sample(delta)

            # Danach verzögerter Repeat.
            self._repeat_job = self.win.after(600, _repeat_tick)
            return "break"

        def _finish_key_release(key):
            self._repeat_release_job = None
            if self._repeat_key != key:
                return
            _cancel_repeat_job()
            self._repeat_key = None
            self._repeat_delta = 0

        def _key_release(key):
            # Kleiner Delay macht das auch unter X11 robust, wo Autorepeat
            # teils als KeyRelease+KeyPress-Paare sichtbar wird. Kommt direkt
            # wieder ein KeyPress, cancelt _key_press diesen Release-Job.
            _cancel_release_job()
            self._repeat_release_job = self.win.after(
                40, lambda key=key: _finish_key_release(key))
            return "break"

        self.win.bind("<KeyPress-Left>",  lambda e: _key_press("Left", -1))
        self.win.bind("<KeyPress-Right>", lambda e: _key_press("Right", +1))
        self.win.bind("<KeyRelease-Left>",  lambda e: _key_release("Left"))
        self.win.bind("<KeyRelease-Right>", lambda e: _key_release("Right"))
        self.win.bind("<Control-KeyPress-Left>",  lambda e: _key_press("CtrlLeft", -10))
        self.win.bind("<Control-KeyPress-Right>", lambda e: _key_press("CtrlRight", +10))
        self.win.bind("<Control-KeyRelease-Left>",  lambda e: _key_release("CtrlLeft"))
        self.win.bind("<Control-KeyRelease-Right>", lambda e: _key_release("CtrlRight"))
        self.win.bind("<Shift-KeyPress-Left>",  lambda e: _key_press("ShiftLeft", -ms_step))
        self.win.bind("<Shift-KeyPress-Right>", lambda e: _key_press("ShiftRight", +ms_step))
        self.win.bind("<Shift-KeyRelease-Left>",  lambda e: _key_release("ShiftLeft"))
        self.win.bind("<Shift-KeyRelease-Right>", lambda e: _key_release("ShiftRight"))

        # Crossfade-Modus
        tk.Label(bar, text="Xfade:", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=(8,2))
        self._xfade_var = tk.StringVar(value="SinEqualPower")
        xfade_menu = ttk.Combobox(bar, textvariable=self._xfade_var,
                                   values=self.XFADE_MODES, width=14,
                                   state="readonly", font=("Consolas", 9))
        xfade_menu.pack(side=tk.LEFT, padx=2)
        xfade_menu.bind("<<ComboboxSelected>>", lambda _: self._plot())

        # Berechnen-Button
        self._btn_compute = tk.Button(bar, text="▶ Alle N berechnen",
                                       command=self._compute_scores, **btn_kw)
        self._btn_compute.pack(side=tk.LEFT, padx=12)

        self._progress = ttk.Progressbar(bar, length=150, mode="determinate")
        self._progress.pack(side=tk.LEFT, padx=4)
        self._status = tk.Label(bar, text="Lade WAV...", bg=C_BG3,
                                 fg=C_TEXT2, font=("Consolas", 9))
        self._status.pack(side=tk.LEFT, padx=4)

        # ── Kurven-Checkboxen ─────────────────────────────────────────────────
        cb_frame = tk.Frame(self.win, bg=C_BG2, pady=4)
        cb_frame.pack(fill=tk.X, padx=6)

        self._show = {}
        curves = [
            ("attack",       "Attack",                  C_DENSE),
            ("ndp_interp",   "Release NDP (interpoliert)", C_SPARSE),
            ("xfade_interp", "Crossfade NDP (interpoliert)", C_SPARSE),
            ("ndp_direct",   "Release NDP (direkt, alle N)", "#9c27b0"),
            ("xfade_direct", "Crossfade NDP (direkt)",   "#9c27b0"),
            ("legacy",       "Release Legacy",           C_WARN),
            ("xfade_legacy", "Crossfade Legacy",         C_WARN),
        ]
        for key, label, color in curves:
            var = tk.BooleanVar(value=True)
            self._show[key] = var
            cb = tk.Checkbutton(cb_frame, text=label, variable=var,
                                 bg=C_BG2, fg=color, selectcolor=C_BG2,
                                 font=("Consolas", 9),
                                 command=self._plot)
            cb.pack(side=tk.LEFT, padx=6)

        # ── Plot ──────────────────────────────────────────────────────────────
        if HAS_MATPLOTLIB:
            plot_frame = tk.Frame(self.win, bg=C_BG)
            plot_frame.pack(fill=tk.BOTH, expand=True, padx=6, pady=4)
            self._fig = Figure(figsize=(11, 5), facecolor=C_BG)
            from matplotlib.backends.backend_tkagg import NavigationToolbar2Tk
            self._canvas = FigureCanvasTkAgg(self._fig, master=plot_frame)
            toolbar_frame = tk.Frame(plot_frame, bg=C_BG)
            toolbar_frame.pack(fill=tk.X)
            NavigationToolbar2Tk(self._canvas, toolbar_frame)
            self._canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    # ── Datenladen ────────────────────────────────────────────────────────────

    def _load_data(self):
        try:
            atk, sr, _, _ = read_wav_mono_float(self.pa.attack_path)
            rel, _,  _, _ = read_wav_mono_float(self.pa.release_path)
            self._atk_mono = atk
            self._rel_mono = rel
            # Legacy-Tabelle aufbauen
            legacy = LegacyAlignTable(rel, sr)
            self._legacy_tbl = legacy
            self.win.after(0, lambda: self._status.config(
                text="Bereit", fg=C_OK))
            self.win.after(0, self._plot)
        except Exception as e:
            self.win.after(0, lambda: self._status.config(
                text=f"Fehler: {e}", fg=C_BAD))

    # ── Score-Matrix berechnen (Modus 2) ──────────────────────────────────────

    def _compute_scores(self):
        if self._computing or self._atk_mono is None:
            return
        self._computing = True
        self._stop_event.clear()
        self._btn_compute.config(state=tk.DISABLED)
        self._progress["value"] = 0
        self._status.config(text="Berechne Score-Matrix...")
        threading.Thread(target=self._score_worker, daemon=True).start()

    def _score_worker(self):
        pa  = self.pa
        try:
            atk = self._atk_mono
            rel = self._rel_mono
            T   = pa.T_int
            ds  = min(4, T // 500) if T >= 500 else 1
            window_len = pa.crossfade_len_samples if pa.crossfade_len_samples >= 4 else 2 * T
            r_max = min(2 * T, len(rel) - window_len)
            if r_max <= 0:
                return

            window_len_d = max(4, window_len // ds)
            r_max_d      = max(1, r_max // ds)
            rel_ds       = rel[:r_max + window_len + 1 : ds].astype(np.float32)

            # n-Bereich: nur gültige LUT-Perioden
            n_start = max(1, int(pa.min_sample / pa.T_float)) if pa.min_sample else 1
            n_end   = min(int(len(atk) / pa.T_float),
                          int(pa.max_sample / pa.T_float) + 2) \
                      if pa.max_sample else int(len(atk) / pa.T_float)
            ns = list(range(n_start, n_end))
            if not ns:
                return

            from numpy.lib.stride_tricks import as_strided as _ast
            score_mat = np.zeros((len(ns), r_max_d), dtype=np.float32)

            for col_i, n in enumerate(ns):
                if self._stop_event.is_set():
                    return
                cs   = int(round(n * pa.T_float))
                cs_d = cs // ds
                lw   = atk[cs_d*ds : (cs_d+window_len_d)*ds : ds].astype(np.float32)
                if len(lw) < window_len_d:
                    continue
                na = np.linalg.norm(lw)
                if na < 1e-12:
                    continue
                lw_n = lw / na
                s0      = rel_ds.strides[0]
                if r_max_d + window_len_d > len(rel_ds):
                    continue
                rel_mat = _ast(rel_ds, shape=(r_max_d, window_len_d),
                               strides=(s0, s0))
                norms   = np.linalg.norm(rel_mat, axis=1)
                norms   = np.where(norms < 1e-12, 1.0, norms)
                score_mat[col_i] = rel_mat.dot(lw_n) / norms

                pct = 100 * (col_i+1) / len(ns)
                self.win.after(0, lambda p=pct: setattr(self._progress, 'value', p)
                               or self._progress.configure(value=p))

            self._score_mat = score_mat
            self._score_ns  = ns
            self._score_ds  = ds
            self._score_r_max_d = r_max_d
            self.win.after(0, self._on_scores_done)
        except Exception as e:
            self.win.after(0, lambda: self._status.config(
                text=f"Fehler: {e}", fg=C_BAD))
        finally:
            self._computing = False

    def _on_scores_done(self):
        self._btn_compute.config(state=tk.NORMAL)
        self._status.config(text="Score-Matrix fertig — Slider live", fg=C_OK)
        self._progress.configure(value=100)
        self._plot()

    def _set_sample(self, smp: int, update_slider: bool = True):
        """Zentrale Update-Funktion — alle Controls synchronisieren."""
        smp = max(self._min_smp, min(self._max_smp, smp))
        sr  = self._sr
        ms  = smp * 1000.0 / sr
        self._cur_smp.set(smp)
        self._ms_var.set(f"{ms:.3f}")
        self._smp_var.set(str(smp))
        if update_slider:
            self._slider.set(int(round(ms)))
        self._plot()

    def _on_slider_move(self, event=None):
        """Slider-Bewegung → Sample berechnen, aber Slider NICHT zurücksetzen."""
        try:
            ms  = float(self._slider.get())
            smp = int(round(ms * self._sr / 1000))
            smp = max(self._min_smp, min(self._max_smp, smp))
            if smp != self._cur_smp.get():
                self._cur_smp.set(smp)
                self._ms_var.set(f"{ms:.3f}")
                self._smp_var.set(str(smp))
                self._plot()
        except ValueError:
            pass

    def _on_slider(self, val):
        self._on_slider_move()

    def _on_ms_entry(self, event=None):
        try:
            ms  = float(self._ms_var.get().replace(",", "."))
            smp = int(round(ms * self._sr / 1000))
            self._set_sample(smp)
        except ValueError:
            pass

    def _on_smp_entry(self, event=None):
        try:
            smp = int(self._smp_var.get())
            self._set_sample(smp)
        except ValueError:
            pass

    def _step_sample(self, delta: int):
        self._set_sample(self._cur_smp.get() + delta)

    def _on_atk_change(self):
        self._plot()

    def _get_r_interp(self, t_attack_samples: int) -> int:
        """r aus LUT-Interpolation + Offset innerhalb Periode.
        n = floor(t/T) — immer die Periode VOR dem aktuellen Sample."""
        pa = self.pa
        T_float = pa.T_float if pa.T_float > 0 else float(max(1, pa.T_int))
        if not pa.lut_points or T_float <= 0:
            return 0
        n_int  = int(t_attack_samples / T_float)   # floor, kein round
        t_n    = int(round(n_int * T_float))
        offset = t_attack_samples - t_n             # immer >= 0
        r_base = get_position_for_correlation(t_n, pa.lut_points, pa.T_int)
        return (r_base + offset) % max(1, pa.T_int)

    def _get_r_direct(self, t_attack_samples: int) -> int:
        """r direkt per argmax aus Score-Matrix, kein mod-Fold."""
        if self._score_mat is None:
            return None
        pa  = self.pa
        ns  = self._score_ns
        ds  = self._score_ds
        T_float = pa.T_float if pa.T_float > 0 else float(max(1, pa.T_int))
        n_int  = int(t_attack_samples / T_float)   # floor
        t_n    = int(round(n_int * T_float))
        offset = t_attack_samples - t_n             # >= 0

        # Nächstes n in Score-Matrix
        if n_int not in ns:
            diffs = [abs(ni - n_int) for ni in ns]
            n_int = ns[diffs.index(min(diffs))]
        col_i = ns.index(n_int) if n_int in ns else 0
        r_d   = int(np.argmax(self._score_mat[col_i]))
        # Kein % T_int — roher Wert aus Score-Matrix + Offset
        return r_d * ds + offset

    def _get_r_legacy(self, t_attack_samples: int) -> int:
        if self._legacy_tbl is None or self._atk_mono is None:
            return 0
        return self._legacy_tbl.get_position_for(self._atk_mono, t_attack_samples)

    # ── Plot ─────────────────────────────────────────────────────────────────

    def _on_atk_change(self):
        self._plot()

    def _plot(self):
        if not HAS_MATPLOTLIB or self._atk_mono is None or self._rel_mono is None:
            return
        if getattr(self, '_plotting', False):
            # Plot läuft noch — Zielwert merken, nach Fertigstellung neu zeichnen
            self._plot_pending = True
            return
        self._plotting = True
        self._plot_pending = False
        try:
            self._do_plot()
        finally:
            self._plotting = False
            # Wenn während des Plots ein neuer Wert ankam: nochmal zeichnen
            if getattr(self, '_plot_pending', False):
                self.win.after(0, self._plot)

    def _do_plot(self):
        pa          = self.pa
        sr          = pa.sample_rate or 48000
        T           = max(16, pa.T_int)
        T_float     = pa.T_float if pa.T_float > 0 else float(T)
        t_smp       = self._cur_smp.get()
        t_ms        = t_smp * 1000.0 / sr
        xfade_len   = pa.crossfade_len_samples if pa.crossfade_len_samples > 0 else 2 * T
        context     = 2 * T

        if len(self._rel_mono) < 4 or len(self._atk_mono) < 4:
            return
        t_smp     = max(0, min(t_smp, len(self._atk_mono) - 1))
        xfade_len = min(xfade_len, len(self._rel_mono) - 1,
                        max(1, len(self._atk_mono) - t_smp))

        r_interp = self._get_r_interp(t_smp)
        r_legacy = self._get_r_legacy(t_smp)
        r_direct = self._get_r_direct(t_smp)  # None wenn Score-Matrix fehlt

        # Gemeinsame Zeitachse: [-context, xfade_len + context]
        total = context + xfade_len + context
        x     = np.arange(-context, xfade_len + context)

        def safe_slice(arr, start, length):
            s = max(0, start)
            e = min(len(arr), start + length)
            out = np.zeros(length, dtype=np.float32)
            src_start = s - start
            out[src_start:src_start+(e-s)] = arr[s:e]
            return out

        # Attack-Kurve: zentriert um t_smp
        atk_seg = safe_slice(self._atk_mono, t_smp - context, total)

        # Release-Segmente
        def rel_seg(r):
            return safe_slice(self._rel_mono, r - context, total)

        # Crossfade-Kurven
        a_w, b_w = _crossfade_weights(self._xfade_var.get(), xfade_len)

        def make_xfade(r):
            atk_part = safe_slice(self._atk_mono, t_smp, xfade_len)
            rel_part = safe_slice(self._rel_mono,  r,     xfade_len)
            xf = atk_part * a_w + rel_part * b_w
            out = np.zeros(total, dtype=np.float32)
            out[context:context+xfade_len] = xf
            # Attack vor Crossfade
            out[:context] = safe_slice(self._atk_mono, t_smp - context, context)
            # Release nach Crossfade
            out[context+xfade_len:] = safe_slice(
                self._rel_mono, r + xfade_len, context)
            return out

        self._fig.clear()
        ax = self._fig.add_subplot(111, facecolor=C_BG2)
        self._fig.patch.set_facecolor(C_BG)
        ax.tick_params(colors=C_TEXT2)
        for sp in ax.spines.values():
            sp.set_edgecolor(C_BORDER)

        show = self._show

        if show["attack"].get():
            ax.plot(x, atk_seg, color=C_DENSE, lw=1.5, label="Attack")

        if show["ndp_interp"].get():
            ax.plot(x, rel_seg(r_interp), color=C_SPARSE, lw=1.2,
                    label=f"Release NDP interp (r={r_interp})")
        if show["xfade_interp"].get():
            ax.plot(x, make_xfade(r_interp), color=C_SPARSE, lw=1.5,
                    linestyle="--", label="Crossfade NDP interp")

        if r_direct is not None:
            if show["ndp_direct"].get():
                ax.plot(x, rel_seg(r_direct), color="#9c27b0", lw=1.2,
                        label=f"Release NDP direkt (r={r_direct})")
            if show["xfade_direct"].get():
                ax.plot(x, make_xfade(r_direct), color="#9c27b0", lw=1.5,
                        linestyle="--", label="Crossfade NDP direkt")

        if show["legacy"].get():
            ax.plot(x, rel_seg(r_legacy), color=C_WARN, lw=1.2,
                    label=f"Release Legacy (r={r_legacy})")
        if show["xfade_legacy"].get():
            ax.plot(x, make_xfade(r_legacy), color=C_WARN, lw=1.5,
                    linestyle="--", label="Crossfade Legacy")

        # Vertikale Marker
        ax.axvline(0,          color=C_TEXT2,  lw=0.8, linestyle=":",
                   label="Crossfade Start")
        ax.axvline(xfade_len,  color=C_BORDER, lw=0.8, linestyle=":",
                   label="Crossfade Ende")

        ax.set_xlabel("Samples (0 = Crossfade-Start)", color=C_TEXT2)
        ax.set_ylabel("Amplitude", color=C_TEXT2)
        ax.set_title(
            f"{pa.rank_name} {midi_to_name(pa.midi_note)} / {pa.release_type}"
            f"  —  Attack {t_ms:.3f} ms ({t_smp} smp)  |  T={T}",
            color=C_TEXT, fontsize=10)
        self._fig.tight_layout(rect=[0, 0, 0.78, 1])
        ax.legend(facecolor=C_BG2, edgecolor=C_BORDER, labelcolor=C_TEXT,
                  fontsize=8, loc="upper left",
                  bbox_to_anchor=(1.02, 1), borderaxespad=0)
        self._canvas.draw()


class CorrLandscapeWindow:
    """
    Separates Fenster: NormalizedDotProduct(attack@n, release@r) als 2D-Heatmap.
    X-Achse: Periodenindex n (gesamter Attack)
    Y-Achse: r in [0, 2*T)
    Farbe:   NDP-Wert in [-1, 1]

    Optionen:
    - Fensterfunktion: keine, Hann, Hamming, Blackman
    - CSV-Export der berechneten Matrix
    """

    WINDOWS = {
        "Keine":    None,
        "Hann":     "hanning",
        "Hamming":  "hamming",
        "Blackman": "blackman",
    }

    def __init__(self, parent, pa: "PipeAnalysis"):
        self.pa     = pa
        self.matrix = None   # np.ndarray (n_cols x r_max)
        self.ns     = None   # x-Achse: n-Werte
        self._computing = False
        self._stop_event = threading.Event()

        self.win = tk.Toplevel(parent)
        self.win.title(f"Korrelationslandschaft — {pa.rank_name} {midi_to_name(pa.midi_note)} "
                       f"{pa.perspective} / {pa.release_type}")
        self.win.geometry("1100x700")
        self.win.configure(bg=C_BG)
        self.win.protocol("WM_DELETE_WINDOW", self._on_close)

        self._build_ui()

    def _on_close(self):
        self._stop_event.set()
        self.win.destroy()

    def _build_ui(self):
        # Toolbar
        bar = tk.Frame(self.win, bg=C_BG3, pady=5)
        bar.pack(fill=tk.X)

        btn_kw = dict(bg=C_BTN, fg=C_TEXT, relief=tk.RAISED,
                      font=("Consolas", 9), padx=8, pady=3,
                      activebackground=C_BTN_ACT, cursor="hand2")

        # Fensterfunktion
        tk.Label(bar, text="Fensterfunktion:", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=(8,2))
        self._win_var = tk.StringVar(value="Keine")
        for name in self.WINDOWS:
            tk.Radiobutton(bar, text=name, variable=self._win_var, value=name,
                           bg=C_BG3, fg=C_TEXT, selectcolor=C_SEL,
                           font=("Consolas", 9),
                           command=self._on_window_change).pack(side=tk.LEFT, padx=4)

        tk.Button(bar, text="▶  Berechnen",
                  command=self._compute, **btn_kw).pack(side=tk.LEFT, padx=12)

        self._btn_csv = tk.Button(bar, text="💾  CSV exportieren",
                                   command=self._export_csv,
                                   state=tk.DISABLED, **btn_kw)
        self._btn_csv.pack(side=tk.LEFT, padx=4)

        self._progress = ttk.Progressbar(bar, length=180, mode="determinate")
        self._progress.pack(side=tk.LEFT, padx=8)
        self._status = tk.Label(bar, text="", bg=C_BG3, fg=C_TEXT2,
                                 font=("Consolas", 9))
        self._status.pack(side=tk.LEFT, padx=4)

        # Info
        pa = self.pa
        info = (f"T={pa.T_int}  SR={pa.sample_rate}  "
                f"r in [0, {2*pa.T_int})  "
                f"n_total={len(pa.attack_path) and pa.n_total or '?'}")
        tk.Label(self.win, text=info, bg=C_BG, fg=C_TEXT2,
                 font=("Consolas", 9)).pack(anchor=tk.W, padx=8, pady=(4,0))

        # Plot
        if HAS_MATPLOTLIB:
            self._fig = Figure(figsize=(10, 5), facecolor=C_BG)
            self._canvas = FigureCanvasTkAgg(self._fig, master=self.win)
            # Matplotlib NavigationToolbar fuer Zoom/Pan
            from matplotlib.backends.backend_tkagg import NavigationToolbar2Tk
            toolbar_frame = tk.Frame(self.win, bg=C_BG)
            toolbar_frame.pack(fill=tk.X, padx=6)
            NavigationToolbar2Tk(self._canvas, toolbar_frame)
            self._canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True, padx=6, pady=4)
        else:
            tk.Label(self.win, text="matplotlib nicht verfügbar",
                     bg=C_BG, fg=C_BAD, font=("Consolas", 10)).pack(pady=20)

    def _on_window_change(self):
        # Wenn Matrix schon berechnet: neu plotten mit neuer Fensterfunktion
        if self.matrix is not None:
            self._plot()

    def _compute(self):
        if self._computing:
            return
        self._stop_event.clear()
        self._computing = True
        self._btn_csv.config(state=tk.DISABLED)
        self._progress["value"] = 0
        self._status.config(text="Lade WAV...")
        # win_name hier lesen (Hauptthread) — nicht im Worker-Thread
        win_name = self._win_var.get()
        threading.Thread(target=self._compute_worker, args=(win_name,), daemon=True).start()

    def _compute_worker(self, win_name: str):
        pa = self.pa
        try:
            atk_mono, sr, _, _ = read_wav_mono_float(pa.attack_path)
            rel_mono, _,  _, _ = read_wav_mono_float(pa.release_path)

            T       = pa.T_int
            T_f     = pa.T_float
            ds      = min(4, T // 500) if T >= 500 else 1
            # window_len: Crossfade-Laenge (wie in GO: min(crossfade_len, 2T))
            # r_max:      Suchbereich [0, 2T)
            window_len = min(pa.crossfade_len_samples, 2 * T)
            if window_len < 4:
                window_len = 2 * T
            r_max      = 2 * T
            # Kuerzen wenn Release zu kurz
            available  = len(rel_mono) - window_len
            if available <= 0:
                window_len = len(rel_mono) // 2
                r_max      = len(rel_mono) - window_len
            elif available < r_max:
                r_max = available
            window_len_d = max(4, window_len // ds)
            r_max_d      = max(1, r_max // ds)

            # Release downgesamplet (muss r_max + window_len abdecken)
            rel_ds = rel_mono[:r_max + window_len + 1 : ds].astype(np.float32)

            # Alle n-Positionen: jede Periode (delta_n=1), damit Feinstrukturen sichtbar werden.
            atk_len = len(atk_mono)
            n_max   = atk_len // T
            ns      = list(range(1, n_max))
            if not ns:
                ns = [1]

            matrix = np.zeros((len(ns), r_max_d), dtype=np.float32)

            for col_idx, n in enumerate(ns):
                if self._stop_event.is_set():
                    return
                cs   = int(round(n * T_f))
                cs_d = cs // ds
                if cs_d + window_len_d > len(atk_mono) // ds:
                    continue
                lw = atk_mono[cs_d * ds : (cs_d + window_len_d) * ds : ds].astype(np.float32)
                if len(lw) < window_len_d:
                    continue

                # Fensterfunktion anwenden — win_name wurde vor Thread-Start gelesen
                win_fn = self.WINDOWS.get(win_name)
                if win_fn:
                    w = getattr(np, win_fn)(len(lw)).astype(np.float32)
                    lw = lw * w
                else:
                    w = None

                na = np.linalg.norm(lw)
                if na < 1e-12:
                    continue
                lw_n = lw / na

                # Alle r auf einmal
                from numpy.lib.stride_tricks import as_strided
                if r_max_d + window_len_d <= len(rel_ds):
                    s0      = rel_ds.strides[0]
                    rel_mat = as_strided(rel_ds,
                                         shape=(r_max_d, window_len_d),
                                         strides=(s0, s0))
                    if w is not None:
                        rel_mat_w = rel_mat * w[np.newaxis, :]
                    else:
                        rel_mat_w = rel_mat
                    norms   = np.linalg.norm(rel_mat_w, axis=1)
                    norms   = np.where(norms < 1e-12, 1.0, norms)
                    scores  = rel_mat_w.dot(lw_n) / norms
                    matrix[col_idx, :len(scores)] = scores

                # Fortschritt ans GUI melden
                pct = 100 * (col_idx + 1) / len(ns)
                self.win.after(0, lambda p=pct, i=col_idx+1, tot=len(ns):
                    self._update_progress(p, i, tot))

            self.matrix = matrix
            self.ns     = ns
            self.r_max_d = r_max_d
            self.ds      = ds
            self.T       = T

            self.win.after(0, self._on_done)

        except Exception as e:
            msg = str(e)
            self.win.after(0, lambda msg=msg: self._status.config(
                text=f"Fehler: {msg}", fg=C_BAD))
        finally:
            self._computing = False

    def _update_progress(self, pct, done, total):
        self._progress["value"] = pct
        self._status.config(text=f"{done}/{total} Positionen")

    def _on_done(self):
        self._status.config(text="Fertig — zoom mit Mausrad/Toolbar")
        self._btn_csv.config(state=tk.NORMAL)
        self._plot()

    def _plot(self):
        if self.matrix is None or not HAS_MATPLOTLIB:
            return
        self._fig.clear()
        ax = self._fig.add_subplot(111, facecolor=C_BG2)
        self._fig.patch.set_facecolor(C_BG)
        ax.tick_params(colors=C_TEXT2)
        for spine in ax.spines.values():
            spine.set_edgecolor(C_BORDER)

        T   = self.T
        ds  = self.ds
        ns  = self.ns
        mat = self.matrix   # shape: (n_cols, r_max_d)

        # Y-Achse in echten Sample-Offsets (0 .. 2T)
        r_ticks = np.arange(mat.shape[1]) * ds

        im = ax.imshow(
            mat.T,
            aspect="auto",
            origin="lower",
            extent=[ns[0], ns[-1], 0, r_ticks[-1] if len(r_ticks) else 2*T],
            vmin=-1.0, vmax=1.0,
            cmap="RdYlGn",
            interpolation="nearest",
        )
        self._fig.colorbar(im, ax=ax, label="NDP-Score", fraction=0.03)

        # LUT-Punkte einzeichnen — getrennt nach Fold-Status
        if self.pa.lut_points:
            pts_folded   = [p for p in self.pa.lut_points if p.folded]
            pts_unfolded = [p for p in self.pa.lut_points if not p.folded]
            if pts_folded:
                ax.scatter([p.n for p in pts_folded], [p.raw_r for p in pts_folded],
                           c="white", marker="o", s=25, zorder=5,
                           edgecolors="black", linewidths=0.5,
                           label="LUT gefaltet")
            if pts_unfolded:
                ax.scatter([p.n for p in pts_unfolded], [p.raw_r for p in pts_unfolded],
                           c="orange", marker="D", s=25, zorder=5,
                           edgecolors="black", linewidths=0.5,
                           label="LUT raw")

        # T-Linie
        ax.axhline(T, color="cyan", linewidth=0.8, linestyle="--",
                    alpha=0.7, label=f"T={T}")

        win_name = self._win_var.get()
        ax.set_xlabel("Periodenindex n", color=C_TEXT2)
        ax.set_ylabel("r (Sample-Offset in Release)", color=C_TEXT2)
        ax.set_title(
            f"Korrelationslandschaft  [{win_name}]  —  "
            f"{self.pa.rank_name} {midi_to_name(self.pa.midi_note)}",
            color=C_TEXT, fontsize=10)
        ax.legend(facecolor=C_BG2, edgecolor=C_BORDER,
                   labelcolor=C_TEXT, fontsize=8,
                   loc="upper left", bbox_to_anchor=(1.02, 1), borderaxespad=0)

        self._fig.tight_layout(rect=[0, 0, 0.82, 1])
        self._canvas.draw()

    def _export_csv(self):
        if self.matrix is None:
            return
        from tkinter import filedialog
        path = filedialog.asksaveasfilename(
            parent=self.win,
            defaultextension=".csv",
            filetypes=[("CSV", "*.csv"), ("Alle", "*.*")],
            initialfile=(f"corr_landscape_{self.pa.rank_name}_{midi_to_name(self.pa.midi_note)}"
                         f"_{self.pa.perspective}_{self.pa.release_type}.csv").replace(" ", "_"),
        )
        if not path:
            return

        import csv
        ds = self.ds
        with open(path, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            # Header: Metadaten
            w.writerow(["# GrandOrgue Korrelationslandschaft"])
            w.writerow(["# Register", self.pa.rank_name])
            w.writerow(["# Note", midi_to_name(self.pa.midi_note)])
            w.writerow(["# Perspektive", self.pa.perspective])
            w.writerow(["# Release", self.pa.release_type])
            w.writerow(["# T_int", self.T])
            w.writerow(["# ds", ds])
            w.writerow(["# Fensterfunktion", self._win_var.get()])
            w.writerow([])
            # Spaltenheader: n-Werte
            w.writerow(["r\n"] + self.ns)
            # Daten: eine Zeile pro r_d
            for r_d in range(self.matrix.shape[1]):
                r_full = r_d * ds
                w.writerow([r_full] + [f"{v:.4f}" for v in self.matrix[:, r_d]])

        self._status.config(text=f"Exportiert: {path}")


# ─── GUI ──────────────────────────────────────────────────────────────────────

SEVERITY_COLOR = ["#1a6b1a", "#8a5c00", "#8b0000"]
SEVERITY_BG    = ["#d4edda", "#fff3cd", "#f8d7da"]
SEVERITY_FG    = ["#155724", "#856404", "#721c24"]

# Helles Theme
C_BG     = "#f5f5f5"
C_BG2    = "#ffffff"
C_BG3    = "#e8e8e8"

# Zeilenfarben für Severity-Tags im Report (Hintergrund / Vordergrund)
SEV_COLORS = {
    "sev_ok":          ("#e8f5e9", "#1b5e20"),   # grün
    "sev_warn":        ("#fff8e1", "#e65100"),   # gelb
    "sev_bad":         ("#ffebee", "#b71c1c"),   # rot
    "sev_error":       ("#fce4ec", "#880e4f"),   # dunkelrot
    "sev_legacy":      ("#f5f5f5", "#555555"),   # grau
    "sev_legacy_warn": ("#fff3e0", "#bf360c"),   # orange
    "sev_legacy_badlut": ("#ffe0b2", "#e65100"), # kräftiges orange
    "sev_drift":       ("#ede7f6", "#4527a0"),   # lila
}
C_BORDER = "#cccccc"
C_TEXT   = "#1a1a1a"
C_TEXT2  = "#555555"
C_ACCENT = "#1565c0"
C_SEL    = "#bbdefb"
C_BTN    = "#e0e0e0"
C_BTN_ACT= "#bdbdbd"
C_RANK   = "#1565c0"
C_NOTE   = "#1a1a1a"
C_OK     = "#1b5e20"
C_WARN   = "#e65100"
C_BAD    = "#b71c1c"
C_DENSE  = "#1565c0"
C_SPARSE = "#2e7d32"
C_GAP    = "#e65100"
C_DRIFT  = "#6a1b9a"

MIDI_NOTE_NAMES = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"]

def midi_to_name(n: int) -> str:
    return f"{MIDI_NOTE_NAMES[n%12]}{n//12-1}"


class LUTAnalyzerApp(tk.Tk):
    def __init__(self, initial_organ: str = None):
        super().__init__()
        self.title("GrandOrgue LUT Analyzer")
        self.geometry("1400x900")
        self.configure(bg=C_BG)

        self._analyses: dict = {}   # key -> PipeAnalysis
        self._key_to_item: dict = {}  # key -> tree item_id (O(1) lookup)
        self._organ_path: str = ""
        self._work_queue  = queue.Queue()
        self._result_queue = queue.Queue()
        self._total_work  = 0
        self._done_work   = 0

        self._build_ui()
        self.after(100, self._poll_results)

        if initial_organ:
            self.after(200, lambda: self._load_organ(initial_organ))

    # ── UI-Aufbau ──────────────────────────────────────────────────────────────

    def _build_ui(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("Treeview",
                         background=C_BG2, foreground=C_TEXT,
                         fieldbackground=C_BG2, rowheight=22,
                         font=("Consolas", 10))
        style.configure("Treeview.Heading",
                         background=C_BG3, foreground=C_TEXT,
                         font=("Consolas", 10, "bold"))
        style.map("Treeview", background=[("selected", C_SEL)],
                              foreground=[("selected", C_TEXT)])
        style.configure("TNotebook", background=C_BG, tabmargins=[2,5,0,0])
        style.configure("TNotebook.Tab", background=C_BG3, foreground=C_TEXT,
                         padding=[10,4], font=("Consolas", 10))
        style.map("TNotebook.Tab", background=[("selected", C_BG2)])
        style.configure("TProgressbar", troughcolor=C_BG3, background=C_ACCENT)

        # Toolbar
        toolbar = tk.Frame(self, bg=C_BG3, pady=6)
        toolbar.pack(fill=tk.X)

        btn_kw = dict(bg=C_BTN, fg=C_TEXT, relief=tk.RAISED,
                       font=("Consolas",10), padx=12, pady=4,
                       activebackground=C_BTN_ACT, activeforeground=C_TEXT,
                       cursor="hand2")

        tk.Button(toolbar, text="📂  Organ laden",
                  command=self._open_dialog, **btn_kw).pack(side=tk.LEFT, padx=6)

        self._btn_all = tk.Button(toolbar, text="🔍  Alles analysieren",
                  command=self._analyze_all, state=tk.DISABLED, **btn_kw)
        self._btn_all.pack(side=tk.LEFT, padx=4)

        self._btn_report = tk.Button(toolbar, text="📄  Report speichern",
                  command=self._save_report, state=tk.DISABLED, **btn_kw)
        self._btn_report.pack(side=tk.LEFT, padx=4)

        self._organ_label = tk.Label(toolbar, text="Keine Datei geladen",
                                      bg=C_BG3, fg=C_TEXT2,
                                      font=("Consolas",10))
        self._organ_label.pack(side=tk.LEFT, padx=12)

        # Downsampling-Einstellung — entspricht GOSettingsOptions::CorrLutDownsampling
        self._ds_var = tk.BooleanVar(value=True)
        tk.Checkbutton(toolbar, text="Downsampling",
                       variable=self._ds_var,
                       bg=C_BG3, fg=C_TEXT,
                       selectcolor=C_BG2, activebackground=C_BG3,
                       activeforeground=C_TEXT,
                       font=("Consolas", 10)
                       ).pack(side=tk.LEFT, padx=(12, 4))

        # Fortschrittsbalken
        self._progress_var = tk.DoubleVar()
        self._progress = ttk.Progressbar(toolbar, variable=self._progress_var,
                                          length=200, mode="determinate")
        self._progress.pack(side=tk.RIGHT, padx=12)
        self._progress_label = tk.Label(toolbar, text="", bg=C_BG3,
                                         fg=C_TEXT2, font=("Consolas", 9))
        self._progress_label.pack(side=tk.RIGHT, padx=4)

        # Haupt-Split: Baum links / Detail rechts
        paned = tk.PanedWindow(self, orient=tk.HORIZONTAL,
                                bg=C_BG3, sashwidth=6,
                                sashrelief=tk.RAISED)
        paned.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

        # Linkes Panel — Baum
        left = tk.Frame(paned, bg=C_BG)
        paned.add(left, minsize=280)

        tk.Label(left, text="Register  /  Pfeife  /  Release",
                  bg=C_BG, fg=C_TEXT2,
                  font=("Consolas",9)).pack(anchor=tk.W, padx=4, pady=(4,2))

        tree_frame = tk.Frame(left, bg=C_BG2)
        tree_frame.pack(fill=tk.BOTH, expand=True)

        self._tree = ttk.Treeview(tree_frame, columns=("info",),
                                   show="tree headings", selectmode="browse")
        self._tree.heading("#0",    text="Eintrag")
        self._tree.heading("info",  text="Status")
        self._tree.column("#0",     width=220)
        self._tree.column("info",   width=120, anchor=tk.CENTER)
        vsb = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL,
                             command=self._tree.yview)
        self._tree.configure(yscrollcommand=vsb.set)
        vsb.pack(side=tk.RIGHT, fill=tk.Y)
        self._tree.pack(fill=tk.BOTH, expand=True)
        self._tree.bind("<<TreeviewSelect>>", self._on_select)

        # Rechtes Panel — Tabs
        right = tk.Frame(paned, bg=C_BG)
        paned.add(right, minsize=600)

        self._tabs = ttk.Notebook(right)
        self._tabs.pack(fill=tk.BOTH, expand=True)

        # Tab 1: Detail
        self._tab_detail = tk.Frame(self._tabs, bg=C_BG)
        self._tabs.add(self._tab_detail, text="  Detail  ")
        self._build_detail_tab()

        # Tab 2: Report
        self._tab_report = tk.Frame(self._tabs, bg=C_BG)
        self._tabs.add(self._tab_report, text="  Report  ")
        self._build_report_tab()

    def _build_detail_tab(self):
        f = self._tab_detail

        # Info-Bereich oben
        info_frame = tk.Frame(f, bg=C_BG2, pady=8, padx=12, relief=tk.GROOVE, bd=1)
        info_frame.pack(fill=tk.X, padx=6, pady=6)

        self._detail_title = tk.Label(info_frame, text="Keine Pfeife ausgewählt",
                                       bg=C_BG2, fg=C_ACCENT,
                                       font=("Consolas", 13, "bold"), anchor=tk.W)
        self._detail_title.pack(fill=tk.X)

        self._detail_info = tk.Label(info_frame, text="",
                                      bg=C_BG2, fg=C_TEXT,
                                      font=("Consolas", 10), anchor=tk.W,
                                      justify=tk.LEFT)
        self._detail_info.pack(fill=tk.X, pady=(4,0))

        # LUT-Tabelle
        tbl_frame = tk.Frame(f, bg=C_BG)
        tbl_frame.pack(fill=tk.X, padx=6, pady=(0,4))

        tk.Label(tbl_frame, text="LUT-Stützpunkte",
                  bg=C_BG, fg=C_TEXT2,
                  font=("Consolas",9)).pack(anchor=tk.W, padx=4)

        cols = ("n","loop_pos","best_r","score","phase","raw_r","folded","fold_ratio")
        self._lut_table = ttk.Treeview(tbl_frame, columns=cols,
                                        show="headings", height=8)
        headers = {"n":"n","loop_pos":"Loop-Pos","best_r":"best_r",
                   "score":"Score","phase":"Phase",
                   "raw_r":"raw_r","folded":"Fold?","fold_ratio":"Fold%"}
        widths   = {"n":50,"loop_pos":100,"best_r":80,"score":80,"phase":70,
                    "raw_r":80,"folded":50,"fold_ratio":60}
        for c in cols:
            self._lut_table.heading(c, text=headers[c])
            self._lut_table.column(c, width=widths[c], anchor=tk.CENTER)
        self._lut_table.pack(fill=tk.X, padx=4)
        self._lut_table.bind("<<TreeviewSelect>>", self._on_lut_select)

        # Button: Korrelationslandschaft
        btn_frame = tk.Frame(tbl_frame, bg=C_BG)
        btn_frame.pack(fill=tk.X, padx=4, pady=(4,0))
        btn_kw2 = dict(bg=C_BTN, fg=C_TEXT, relief=tk.RAISED,
                       font=("Consolas",9), padx=8, pady=3,
                       activebackground=C_BTN_ACT, cursor="hand2")
        tk.Button(btn_frame, text="📊  Korrelationslandschaft öffnen",
                  command=self._open_corr_landscape, **btn_kw2).pack(side=tk.LEFT, padx=2)
        tk.Button(btn_frame, text="🎵  Crossfade-Simulation",
                  command=self._open_crossfade_sim, **btn_kw2).pack(side=tk.LEFT, padx=2)

        # Plot-Bereich
        if HAS_MATPLOTLIB:
            plot_frame = tk.Frame(f, bg=C_BG)
            plot_frame.pack(fill=tk.BOTH, expand=True, padx=6, pady=4)

            self._fig = Figure(figsize=(8, 4), facecolor=C_BG)
            self._canvas = FigureCanvasTkAgg(self._fig, master=plot_frame)
            self._canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        else:
            tk.Label(f, text="matplotlib nicht verfügbar — Plots deaktiviert",
                      bg=C_BG, fg=C_BAD,
                      font=("Consolas",10)).pack(pady=20)

    def _build_report_tab(self):
        f = self._tab_report

        tk.Label(f, text="Problematischste Pfeifen (sortiert nach Schwere)",
                  bg=C_BG, fg=C_TEXT2,
                  font=("Consolas",9)).pack(anchor=tk.W, padx=10, pady=(8,2))

        cols = ("sev","rank","note","persp","rel","freq","T","stable_n",
                "transient_n","n_pts","score_mean","score_min","score_max",
                "bestr_coherence","amp_ratio","phase3","atk_dur","rel_dur","error")
        self._report_table = ttk.Treeview(f, columns=cols,
                                           show="headings", height=30)
        headers = {
            "sev":"", "rank":"Register", "note":"Note",
            "persp":"Perspektive", "rel":"Release",
            "freq":"Freq Hz", "T":"T",
            "stable_n":"Stabil n", "transient_n":"Trans.Ende n",
            "n_pts":"LUT d+s+g", "score_mean":"Score Mean",
            "score_min":"Score Min", "score_max":"Score Max",
            "bestr_coherence":"R Streu.", "amp_ratio":"Amp-Ratio",
            "phase3":"Gaps", "atk_dur":"Atk ms", "rel_dur":"Rel ms",
            "error":"Fehler"
        }
        widths = {
            "sev":110, "rank":200, "note":55, "persp":130, "rel":80,
            "freq":70, "T":45, "stable_n":70, "transient_n":95,
            "n_pts":80, "score_mean":85, "score_min":75, "score_max":75,
            "bestr_coherence":80, "amp_ratio":75,
            "phase3":45, "atk_dur":65, "rel_dur":65, "error":200
        }
        for c in cols:
            self._report_table.heading(c, text=headers[c])
            stretch = c in ("rank", "error", "persp")
            self._report_table.column(c, width=widths[c],
                                       minwidth=widths[c]//2,
                                       anchor=tk.W if c in ("rank","error","sev") else tk.CENTER,
                                       stretch=stretch)

        # Zeilenfarben für Severity
        for tag, (bg, fg) in SEV_COLORS.items():
            self._report_table.tag_configure(tag, background=bg, foreground=fg)

        vsb2 = ttk.Scrollbar(f, orient=tk.VERTICAL,
                              command=self._report_table.yview)
        self._report_table.configure(yscrollcommand=vsb2.set)
        vsb2.pack(side=tk.RIGHT, fill=tk.Y, padx=(0,6), pady=6)
        self._report_table.pack(fill=tk.BOTH, expand=True, padx=(6,0), pady=6)
        self._report_table.bind("<<TreeviewSelect>>", self._on_report_select)

    # ── Datei-Dialog ───────────────────────────────────────────────────────────

    def _open_dialog(self):
        path = filedialog.askopenfilename(
            title="Organ-Datei öffnen",
            filetypes=[("Organ files","*.organ"),("Alle Dateien","*.*")])
        if path:
            self._load_organ(path)

    def _load_organ(self, path: str):
        self._organ_path = path
        self._organ_label.config(text=os.path.basename(path), fg=C_TEXT)
        self._analyses.clear()
        self._key_to_item.clear()
        self._tree.delete(*self._tree.get_children())
        self._btn_all.config(state=tk.NORMAL)
        self._btn_report.config(state=tk.DISABLED)

        # Baum aufbauen (ohne Analyse)
        self._pipe_descs = parse_organ_file(path)
        if not self._pipe_descs:
            messagebox.showerror("Fehler",
                "Keine Pfeifen gefunden. Bitte prüfe den Organ-Dateipfad.")
            return

        # Baum: Register → MIDI-Note → Perspektive/Release
        ranks = {}
        for d in self._pipe_descs:
            r = d["rank_name"]
            n = d["midi_note"]
            p = d["perspective"]
            t = d["release_type"]
            ranks.setdefault(r, {}).setdefault(n, []).append(d)

        for rank_name in sorted(ranks):
            rank_id = self._tree.insert("", tk.END,
                                         text=f"♩ {rank_name}",
                                         values=("–",), tags=("rank",))
            self._tree.tag_configure("rank", foreground=C_RANK,
                                      font=("Consolas",10,"bold"))
            for midi in sorted(ranks[rank_name]):
                note_id = self._tree.insert(rank_id, tk.END,
                                             text=f"  {midi_to_name(midi)} ({midi})",
                                             values=("–",), tags=("note",))
                self._tree.tag_configure("note", foreground=C_NOTE)
                for d in ranks[rank_name][midi]:
                    key = f"{rank_name}|{midi}|{d['perspective']}|{d['release_type']}"
                    label = f"    {d['perspective']} / {d['release_type']}"
                    item_id = self._tree.insert(note_id, tk.END,
                                                 text=label,
                                                 values=("⬜ ausstehend",),
                                                 tags=("pipe",))
                    self._tree.tag_configure("pipe", foreground=C_OK)
                    # item_id → key Mapping
                    self._tree.item(item_id, tags=("pipe", key))
                    self._tree.tag_configure(key, foreground=C_OK)
                    self._key_to_item[key] = item_id   # O(1) lookup
                    # Speichere Descriptor im Analyse-Dict (noch ohne Ergebnis)
                    self._analyses[key] = d  # temporär Descriptor

        self._title(f"GrandOrgue LUT Analyzer — {os.path.basename(path)}")

    def _title(self, t):
        self.title(t)

    # ── Analyse ────────────────────────────────────────────────────────────────

    def _analyze_all(self):
        self._btn_all.config(state=tk.DISABLED)
        self._done_work = 0
        self._total_work = len(self._pipe_descs)
        self._progress_var.set(0)
        self._progress_label.config(text=f"0/{self._total_work}")

        def worker():
            # ProcessPoolExecutor umgeht Pythons GIL — echte CPU-Parallelität.
            # analyze_pipe ist eine reine Funktion ohne GUI-Referenzen → picklebar.
            n_workers = min(20, max(2, (_os.cpu_count() or 4)))
            self._progress_label.config(text=f"0/{self._total_work}  ({n_workers} Prozesse)")
            use_ds = self._ds_var.get()
            descs = [{**d, "downsampling": use_ds} for d in self._pipe_descs]
            with concurrent.futures.ProcessPoolExecutor(max_workers=n_workers) as pool:
                futures = {pool.submit(analyze_pipe, d): d
                           for d in descs}
                for fut in concurrent.futures.as_completed(futures):
                    try:
                        result = fut.result()
                    except Exception as e:
                        d = futures[fut]
                        result = PipeAnalysis(
                            organ_base=d.get("organ_base",""),
                            rank_name=d.get("rank_name","?"),
                            midi_note=d.get("midi_note",0),
                            perspective=d.get("perspective","?"),
                            release_type=d.get("release_type","?"),
                            attack_path=d.get("attack_path",""),
                            release_path=d.get("release_path",""),
                            error=str(e),
                        )
                    self._result_queue.put(result)

        threading.Thread(target=worker, daemon=True).start()

    def _poll_results(self):
        # Max 30 Ergebnisse pro Poll-Zyklus — GUI bleibt responsiv
        for _ in range(30):
            try:
                pa: PipeAnalysis = self._result_queue.get_nowait()
            except queue.Empty:
                break
            key = f"{pa.rank_name}|{pa.midi_note}|{pa.perspective}|{pa.release_type}"
            self._analyses[key] = pa
            self._done_work += 1
            self._update_tree_item(key, pa)

            if self._done_work >= self._total_work and self._total_work > 0:
                pct = 100.0
                self._progress_var.set(pct)
                self._progress_label.config(
                    text=f"{self._done_work}/{self._total_work}  ✓")
                self._btn_all.config(state=tk.NORMAL)
                self._btn_report.config(state=tk.NORMAL)
                self._build_report()
                self._tabs.select(self._tab_report)
                self.after(50, self._poll_results)
                return

        # Fortschritt nach dem Batch aktualisieren (ein Aufruf statt N)
        if self._total_work > 0:
            pct = 100 * self._done_work / self._total_work
            self._progress_var.set(pct)
            self._progress_label.config(
                text=f"{self._done_work}/{self._total_work}")
        self.after(50, self._poll_results)

    def _update_tree_item(self, key: str, pa: PipeAnalysis):
        """Aktualisiert Farbe und Status eines Baum-Eintrags (O(1) via dict)."""
        pipe_item = self._key_to_item.get(key)
        if pipe_item is None:
            return
        colors = [C_OK, C_WARN, C_BAD]
        status = pa.error or (f"n={pa.stable_at_n}" if pa.stabilized else "nicht stabil")
        self._tree.item(pipe_item,
                        values=(pa.severity_label + " " + status,),
                        tags=("pipe", key))
        self._tree.tag_configure(key, foreground=colors[pa.severity])

    # ── Selektion ──────────────────────────────────────────────────────────────

    def _on_select(self, event):
        sel = self._tree.selection()
        if not sel:
            return
        tags = self._tree.item(sel[0], "tags")
        # Finde key-Tag (enthält "|")
        key = next((t for t in tags if "|" in t), None)
        if not key:
            return
        pa = self._analyses.get(key)
        if pa is None:
            return

        if isinstance(pa, dict):
            # Noch nicht analysiert — on-demand
            pa = analyze_pipe({**pa, "downsampling": self._ds_var.get()})
            self._analyses[key] = pa
            self._update_tree_item(key, pa)

        self._show_detail(pa)

    def _open_crossfade_sim(self):
        if not self._current_pa:
            return
        pa = self._current_pa
        if pa.error:
            return
        CrossfadeSimWindow(self, pa)
    def _open_corr_landscape(self):
        """Oeffnet separates Fenster mit vollstaendiger Korrelationslandschaft."""
        if not self._current_pa:
            return
        pa = self._current_pa
        if pa.error:
            tk.messagebox.showinfo("Korrelationslandschaft",
                                   f"Keine Daten: {pa.error}", parent=self)
            return
        CorrLandscapeWindow(self, pa)

    def _on_report_select(self, event):
        sel = self._report_table.selection()
        if not sel:
            return
        tags = self._report_table.item(sel[0], "tags")
        key = next((t for t in tags if "|" in t), None)
        if not key:
            return
        pa = self._analyses.get(key)
        if pa and isinstance(pa, PipeAnalysis):
            self._tabs.select(self._tab_detail)
            self._show_detail(pa)

    def _on_lut_select(self, event):
        """Klick auf LUT-Punkt → Waveform-Plot aktualisieren."""
        sel = self._lut_table.selection()
        if not sel or not self._current_pa:
            return
        vals = self._lut_table.item(sel[0], "values")
        try:
            n = int(vals[0])
        except (ValueError, IndexError):
            return
        pt = next((p for p in self._current_pa.lut_points if p.n == n), None)
        if pt:
            self._plot_waveform(self._current_pa, pt)

    # ── Detail-Anzeige ─────────────────────────────────────────────────────────

    _current_pa: Optional[PipeAnalysis] = None

    def _show_detail(self, pa: PipeAnalysis):
        self._current_pa = pa

        # Titel
        note_str = midi_to_name(pa.midi_note)
        self._detail_title.config(
            text=f"{pa.rank_name}  —  {note_str}  —  {pa.perspective} / {pa.release_type}",
            fg=[C_OK, C_WARN, C_BAD][pa.severity])

        # Info-Text
        if pa.error:
            info = f"❌ Fehler: {pa.error}"
        else:
            info_parts = [
                f"T_float={pa.T_float:.2f}  T_int={pa.T_int}  SR={pa.sample_rate}Hz"
                + (f"  CMNDF: T/2={pa.cmndf_at_T_half:.3f}  T={pa.cmndf_at_T:.3f}  2T={pa.cmndf_at_2T:.3f}"
                   if not (pa.cmndf_at_T_half != pa.cmndf_at_T_half) else ""),  # nan check
                f"Loop: {pa.loop_start}–{pa.loop_end}  ({pa.loop_len} Samples, {pa.n_total} Perioden)",
                f"Stabilisiert: {'Drift bei n=' + str(pa.stable_at_n) if pa.drift_mode else ('ja bei n=' + str(pa.stable_at_n) if pa.stabilized else '⚠ nein')}",
                f"Drift: {pa.drift_per_period:.3f} Samples/Periode  Residuum={pa.drift_residual:.2f}  max_gap_n={pa.max_interp_gap_n}",
                f"Amplitude Attack/Release: {pa.amplitude_ratio:.1f}×",
                f"LUT-Punkte: {len(pa.lut_points)}  (Phase-3-Gaps: {pa.phase3_count}, dense_step={pa.dense_step_used})",
                f"Legacy-Fallback: {'ja (' + pa.legacy_reason + ')' if pa.legacy_fallback else 'nein'}",
            ]
            info = "\n".join(info_parts)
        self._detail_info.config(text=info)

        # LUT-Tabelle
        self._lut_table.delete(*self._lut_table.get_children())
        phase_colors = {"dense":C_DENSE,"sparse":C_SPARSE,"gap":C_GAP,"drift":C_DRIFT}
        for pt in pa.lut_points:
            score_str = f"{pt.best_score:.3f}" if pt.best_score > -2 else "n/a"
            fold_str  = "✓" if pt.folded else "✗"
            ratio_str = f"{pt.fold_ratio*100:.0f}%" if pt.fold_ratio < 1.5 else "–"
            iid = self._lut_table.insert("", tk.END,
                                          values=(pt.n, pt.loop_pos, pt.best_r,
                                                  score_str, pt.phase,
                                                  pt.raw_r, fold_str, ratio_str),
                                          tags=(pt.phase,))
            self._lut_table.tag_configure(pt.phase,
                                           foreground=phase_colors.get(pt.phase, C_TEXT))

        # Plot
        if HAS_MATPLOTLIB:
            self._plot_lut(pa)

    def _plot_lut(self, pa: PipeAnalysis):
        """Zeigt LUT-Punkte und die Kreis-Interpolation, die GO später verwenden soll."""
        self._fig.clear()
        if not pa.lut_points:
            self._canvas.draw()
            return

        ax = self._fig.add_subplot(111, facecolor=C_BG2)
        self._fig.patch.set_facecolor(C_BG)
        ax.tick_params(colors=C_TEXT2)
        for spine in ax.spines.values():
            spine.set_edgecolor(C_BORDER)

        pts_sorted = sorted(pa.lut_points, key=lambda p: p.n)
        T = pa.T_int

        # Interpolationslinie: nicht naive Gerade, sondern shortest-circular-delta.
        # Das entspricht der geplanten GO-Interpolation auf best_r mod T.
        interp_x = []
        interp_y = []
        for p0, p1 in zip(pts_sorted, pts_sorted[1:]):
            n0, n1 = p0.n, p1.n
            if n1 <= n0:
                continue
            r0 = float(p0.best_r % T)
            r1 = float(p1.best_r % T)
            d = shortest_circular_delta(r0, r1, T)
            steps = max(2, min(200, n1 - n0 + 1))
            last_y = None
            for i in range(steps):
                t = i / (steps - 1)
                x = n0 + t * (n1 - n0)
                y = (r0 + t * d) % T
                # Bei sichtbarem Wrap Linie unterbrechen, damit keine falsche Senkrechte entsteht.
                if last_y is not None and abs(y - last_y) > T / 2:
                    interp_x.append(float('nan'))
                    interp_y.append(float('nan'))
                interp_x.append(x)
                interp_y.append(y)
                last_y = y
        if interp_x:
            ax.plot(interp_x, interp_y, color=C_TEXT, linewidth=1.2,
                    alpha=0.75, zorder=1, label="GO-Kreisinterpolation")

        # Rohpunkte nach Phase/Fold-Status getrennt zeichnen.
        phase_color = {"dense": C_DENSE, "sparse": C_SPARSE, "gap": C_GAP, "drift": C_DRIFT}
        for phase in ["dense", "sparse", "gap", "drift"]:
            pts_fold   = [p for p in pts_sorted if p.phase == phase and p.folded]
            pts_unfold = [p for p in pts_sorted if p.phase == phase and not p.folded]
            col = phase_color[phase]
            if pts_fold:
                ax.scatter([p.n for p in pts_fold], [p.best_r % T for p in pts_fold],
                           color=col, marker="o", s=36, zorder=3,
                           label=f"{phase} (gefaltet)")
            if pts_unfold:
                ax.scatter([p.n for p in pts_unfold], [p.best_r % T for p in pts_unfold],
                           color=col, marker="D", s=36, zorder=3,
                           label=f"{phase} (raw)")

        ax.axhline(T, color=C_BAD, linewidth=0.7,
                    linestyle="--", alpha=0.6, label=f"T={T}")
        ax.axhline(0, color=C_BORDER, linewidth=0.5)

        if pa.stable_at_n:
            label = f"Drift n={pa.stable_at_n}" if pa.drift_mode else f"stabil n={pa.stable_at_n}"
            ax.axvline(pa.stable_at_n, color=C_ACCENT, linewidth=1,
                        linestyle=":", alpha=0.8, label=label)

        ax.set_xlabel("Periodenindex n", color=C_TEXT2)
        ax.set_ylabel("best_r mod T", color=C_TEXT2)
        title = "LUT-Stützpunkte + GO-Kreisinterpolation"
        if pa.drift_mode:
            title += f"  (Drift {pa.drift_per_period:.3f} smp/Periode)"
        ax.set_title(title, color=C_TEXT, fontsize=10)
        ax.legend(facecolor=C_BG2, edgecolor=C_BORDER,
                   labelcolor=C_TEXT, fontsize=8,
                   loc="upper left", bbox_to_anchor=(1.02, 1), borderaxespad=0)
        ax.set_ylim(-5, T + 5)
        self._fig.tight_layout(rect=[0, 0, 0.82, 1])
        self._canvas.draw()

    def _plot_waveform(self, pa: PipeAnalysis, pt: LutPoint):
        """Zeigt Attack-Loop und Release an einem LUT-Punkt.
        Drei Kurven:
          - Attack @ loop_pos (blau):   was GO am Übergang abspielt
          - Release @ best_r (grün):    korrelationsbasierter Startpunkt
          - Release @ 0 (grau):         Release-Anfang zum Vergleich
        """
        if not HAS_MATPLOTLIB:
            return
        try:
            # Vollständige Dateien laden (nur bis zum benötigten Bereich)
            needed_atk = pa.loop_start + pt.loop_pos + 3 * pa.T_int + 10
            needed_rel = pt.best_r + 3 * pa.T_int + 10
            atk_mono, sr, _, _ = read_wav_mono_float(pa.attack_path,
                                                      max_samples=needed_atk)
            rel_mono, _,  _, _ = read_wav_mono_float(pa.release_path,
                                                      max_samples=needed_rel)
        except Exception as e:
            return

        self._fig.clear()
        ax = self._fig.add_subplot(111, facecolor=C_BG2)
        self._fig.patch.set_facecolor(C_BG)
        ax.tick_params(colors=C_TEXT2)
        for spine in ax.spines.values():
            spine.set_edgecolor(C_BORDER)

        # Fensterlänge: 2 Perioden, max 512 Samples
        win = min(2 * pa.T_int, 512)
        t   = list(range(win))

        # Attack bei loop_pos:
        # pt.loop_pos ist absolute Sample-Position in der Attack-Datei (wie in GO)
        abs_cs = pt.loop_pos
        atk_w  = atk_mono[abs_cs : abs_cs + win]

        # Release ab best_r (korrelierter Startpunkt)
        rel_corr = rel_mono[pt.best_r : pt.best_r + win]

        # Release ab 0 (unkompensiert, zum Vergleich)
        rel_raw  = rel_mono[0 : win]

        if len(atk_w) == win:
            ax.plot(t, atk_w,   color=C_DENSE,  linewidth=1.5,
                    label=f"Attack @ loop_pos={pt.loop_pos}", zorder=3)
        if len(rel_corr) == win:
            ax.plot(t, rel_corr, color=C_SPARSE, linewidth=1.5,
                    label=f"Release @ best_r={pt.best_r}  (score={pt.best_score:.3f})",
                    zorder=2)
        if len(rel_raw) == win:
            ax.plot(t, rel_raw,  color=C_BORDER, linewidth=1.0,
                    linestyle="--", label="Release @ 0 (Anfang)", zorder=1)

        # Vertikale Linie bei T (Ende der ersten Periode)
        ax.axvline(pa.T_int, color=C_TEXT2, linewidth=0.8,
                   linestyle=":", alpha=0.6, label=f"T={pa.T_int}")

        ax.set_xlabel("Sample-Offset ab Startposition", color=C_TEXT2)
        ax.set_ylabel("Amplitude (normiert)", color=C_TEXT2)
        ax.set_title(
            f"Waveform-Vergleich  —  n={pt.n}  loop_pos={pt.loop_pos}  "
            f"best_r={pt.best_r}  score={pt.best_score:.3f}",
            color=C_TEXT, fontsize=10)
        ax.legend(facecolor=C_BG2, edgecolor=C_BORDER,
                   labelcolor=C_TEXT, fontsize=8)
        self._canvas.draw()

    # ── Report ─────────────────────────────────────────────────────────────────

    def _build_report(self):
        self._report_table.delete(*self._report_table.get_children())
        pas = [v for v in self._analyses.values() if isinstance(v, PipeAnalysis)]

        def sort_key(p):
            # 1. Fehler zuerst
            # 2. Rot (Fehler in LUT)
            # 3. Gelb (Warn in LUT)
            # 4. Grün (OK in LUT)
            # 5. Legacy instabil
            # 6. Legacy Drift
            # 7. Legacy (T<16)
            tag = p.severity_tag
            order = {
                "sev_error":       0,
                "sev_bad":         1,
                "sev_warn":        2,
                "sev_ok":          3,
                "sev_legacy_warn": 4,
                "sev_legacy_badlut": 5,
                "sev_drift":       6,
                "sev_legacy":      7,
            }.get(tag, 9)
            return (order, p.rank_name, p.midi_note)

        pas.sort(key=sort_key)

        for pa in pas:
            key = f"{pa.rank_name}|{pa.midi_note}|{pa.perspective}|{pa.release_type}"
            sr_ = pa.sample_rate or 48000
            self._report_table.insert("", tk.END,
                values=(
                    pa.severity_label,
                    pa.rank_name,
                    midi_to_name(pa.midi_note),
                    pa.perspective,
                    pa.release_type,
                    f"{pa.freq_hz:.1f}"    if pa.freq_hz         else "-",
                    str(pa.T_int)           if pa.T_int           else "-",
                    str(pa.stable_at_n)     if pa.stabilized      else "nein",
                    str(pa.transient_end_n) if pa.transient_end_n else "-",
                    f"{pa.n_dense}+{pa.n_sparse}+{pa.n_gap}",
                    f"{pa.score_mean:.3f}"  if pa.score_mean      else "-",
                    f"{pa.score_min:.3f}"   if pa.score_min       else "-",
                    f"{pa.score_max:.3f}"   if pa.score_max       else "-",
                    f"{pa.bestr_coherence:.1f}"   if pa.bestr_coherence       else "-",
                    f"{pa.amplitude_ratio:.1f}x" if pa.amplitude_ratio else "-",
                    str(pa.phase3_count),
                    f"{pa.atk_frames*1000//sr_}" if pa.atk_frames else "-",
                    f"{pa.rel_frames*1000//sr_}" if pa.rel_frames else "-",
                    pa.error or ""
                ),
                tags=(pa.severity_tag, key))

    def _save_report(self):
        path = filedialog.asksaveasfilename(
            defaultextension=".html",
            filetypes=[("HTML","*.html"),("CSV","*.csv"),("Alle","*.*")])
        if not path:
            return

        pas = [v for v in self._analyses.values() if isinstance(v, PipeAnalysis)]
        pas.sort(key=lambda p: (-p.severity, p.rank_name, p.midi_note))

        if path.endswith(".csv"):
            import csv
            with open(path, "w", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                w.writerow(["Severity","Register","MIDI","Note","Perspektive","Release",
                             "Freq_Hz","T_int","T_float",
                             "Stabil_n","Transient_Ende_n","N_Dense","N_Sparse","N_Gap",
                             "Score_Mean","Score_Min","Score_Max","BestR_Std","BestR_Mean",
                             "Amp_Ratio","Atk_ms","Rel_ms","Loop_Start","Loop_End",
                             "HarmonicNumber","Fehler"])
                for pa in pas:
                    sr_ = pa.sample_rate or 48000
                    w.writerow([
                        pa.severity, pa.rank_name, pa.midi_note,
                        midi_to_name(pa.midi_note), pa.perspective, pa.release_type,
                        f"{pa.freq_hz:.3f}", pa.T_int, f"{pa.T_float:.4f}",
                        pa.stable_at_n if pa.stabilized else "nein",
                        pa.transient_end_n, pa.n_dense, pa.n_sparse, pa.n_gap,
                        f"{pa.score_mean:.4f}", f"{pa.score_min:.4f}", f"{pa.score_max:.4f}",
                        f"{pa.bestr_coherence:.2f}", f"{pa.bestr_mean:.2f}",
                        f"{pa.amplitude_ratio:.3f}",
                        pa.atk_frames * 1000 // sr_, pa.rel_frames * 1000 // sr_,
                        pa.loop_start, pa.loop_end,
                        pa.harmonic_number, pa.error or ""
                    ])
        else:
            # HTML
            rows = ""
            for pa in pas:
                scores = [p.best_score for p in pa.lut_points if p.phase in ("sparse","gap")]
                bg = ["#e8f5e8","#fff8e0","#fde8e8"][pa.severity]
                sr_ = pa.sample_rate or 48000
                rows += f"""<tr style="background:{bg}">
                  <td>{pa.severity_label}</td>
                  <td>{pa.rank_name}</td>
                  <td>{midi_to_name(pa.midi_note)}</td>
                  <td>{pa.perspective}</td>
                  <td>{pa.release_type}</td>
                  <td>{pa.freq_hz:.1f}</td>
                  <td>{pa.T_int}</td>
                  <td>{"n="+str(pa.stable_at_n) if pa.stabilized else "<b>nein</b>"}</td>
                  <td>{pa.transient_end_n}</td>
                  <td>{pa.n_dense}+{pa.n_sparse}+{pa.n_gap}</td>
                  <td>{pa.score_mean:.3f}</td>
                  <td>{pa.score_min:.3f}</td>
                  <td>{pa.score_max:.3f}</td>
                  <td>{pa.bestr_coherence:.1f}</td>
                  <td>{pa.amplitude_ratio:.1f}x</td>
                  <td>{pa.phase3_count}</td>
                  <td>{pa.atk_frames*1000//sr_}</td>
                  <td>{pa.rel_frames*1000//sr_}</td>
                  <td>{pa.error or ""}</td>
                </tr>\n"""

            html = f"""<!DOCTYPE html><html><head><meta charset="utf-8">
<title>GrandOrgue LUT Report</title>
<style>
  body {{ font-family: monospace; font-size:13px; background:#f8f8f0; margin:20px; }}
  h1 {{ color:#333; }}
  table {{ border-collapse:collapse; width:100%; }}
  th {{ background:#333; color:#fff; padding:6px 10px; text-align:left; }}
  td {{ padding:5px 10px; border-bottom:1px solid #ddd; }}
</style></head><body>
<h1>GrandOrgue LUT Analyzer — Report</h1>
<p>Organ: {os.path.basename(self._organ_path)}</p>
<table>
<tr><th></th><th>Register</th><th>Note</th><th>Perspektive</th><th>Release</th>
<th>Freq Hz</th><th>T</th><th>Stabil n</th><th>Trans.Ende n</th>
<th>LUT-Pts<br>(d+s+g)</th><th>Score Mean</th><th>Score Min</th><th>Score Max</th>
<th>R Streuung</th><th>Amp-Ratio</th><th>Gaps</th>
<th>Atk ms</th><th>Rel ms</th><th>Fehler</th></tr>
{rows}
</table></body></html>"""

            with open(path, "w", encoding="utf-8") as f:
                f.write(html)

        messagebox.showinfo("Gespeichert", f"Report gespeichert:\n{path}")


# ─── Einstiegspunkt ───────────────────────────────────────────────────────────

def export_csv_batch(organ_path: str, output_path: str = None):
    """
    Batch mode for Phase 8 GO comparison.
    Analyses all releases of an organ file and exports LUT points as CSV:
      release_idx, point_idx, loop_pos, best_r
    Releases with legacy_fallback (drift, instability, bad quality) are omitted —
    matching GO's behaviour (releaseMap[i] = -1 for those).

    Compare with GO output:
      python3 analyze_lut_v57.py organ.organ --export-csv py.csv
      python3 read_golut.py organ.release-align.golut --csv > go.csv
      diff py.csv go.csv
    """
    import concurrent.futures

    print(f"Parsing {organ_path} …", file=sys.stderr)
    pipes = parse_organ_file(organ_path)
    print(f"  {len(pipes)} release descriptors found", file=sys.stderr)

    print("Analysing …", file=sys.stderr)
    max_workers = max(1, (os.cpu_count() or 2) - 1)
    with concurrent.futures.ProcessPoolExecutor(max_workers=max_workers) as ex:
        results = list(ex.map(analyze_pipe, pipes))

    out = open(output_path, 'w', newline='') if output_path else sys.stdout
    try:
        out.write("release_idx,point_idx,loop_pos,best_r\n")
        n_cached = 0
        for release_idx, pa in enumerate(results):
            # Skip releases that GO would mark as no-LUT
            if pa.legacy_fallback or not pa.lut_points:
                continue
            n_cached += 1
            for pt_idx, pt in enumerate(pa.lut_points):
                out.write(f"{release_idx},{pt_idx},{pt.loop_pos},{pt.best_r}\n")
    finally:
        if output_path:
            out.close()

    print(
        f"Exported {n_cached} cached / {len(results)} total releases.",
        file=sys.stderr)
    if output_path:
        print(f"Written to {output_path}", file=sys.stderr)


def main():
    # CLI batch mode: analyze_lut_v57.py <organ> --export-csv [output.csv]
    if len(sys.argv) >= 3 and sys.argv[2] == '--export-csv':
        out = sys.argv[3] if len(sys.argv) > 3 else None
        export_csv_batch(sys.argv[1], out)
        return

    initial = sys.argv[1] if len(sys.argv) > 1 else None
    app = LUTAnalyzerApp(initial_organ=initial)
    app.mainloop()


if __name__ == "__main__":
    # Nötig für ProcessPoolExecutor unter Windows (spawn-Methode)
    import multiprocessing
    multiprocessing.freeze_support()
    main()
