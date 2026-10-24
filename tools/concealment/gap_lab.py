#!/usr/bin/env python3
"""Gap lab: replace a stretch of real material with a Burg continuation,
then crossfade back into the original - hear and measure it.

Scenario (GrandOrgue "emergency buffer"): a multithreaded renderer
misses one buffer deadline.  The first worker that finished in time has
already extrapolated the recent OUTPUT history (the finished mix sum)
one buffer ahead with a Burg predictor; that emergency buffer is played
instead of an xrun.  The buffer is a CONTINUATION, not a fade-out - no
fade happens during the gap.  When the real stream re-enters one buffer
later, its start does not exactly match the predicted end, so a short
crossfade blends prediction into reality.

This tool simulates exactly that on a WAV file (or a built-in test
signal): pick a gap position, a gap length in samples (= the audio
buffer size), a re-entry crossfade length in ms, and a strategy.  The
gap samples are replaced by the strategy's continuation of the history
BEFORE the gap; over the following crossfade window the continuation
fades out while the ORIGINAL fades in; before and after, the signal is
bit-identical to the original.

The open design question this lab decides ("phase-correct fade"): the
prediction drifts in phase over the gap, so the blunt crossfade of two
nearly identical but slightly shifted signals partially cancels
(combing).  The lab therefore
- measures the residual lag k at the junction by normalised
  cross-correlation (prediction vs. true material, sub-sample refined),
  and
- can optionally WARP the prediction tail (gentle resampling ramp
  inside the gap, reaching shift k at the junction) so the crossfade
  runs phase-aligned - the "warp the ephemeral tail" idea.  The real
  stream is never touched.
The decisive number is delta-NMR = NMR(blunt) - NMR(aligned).  If it
is ~0, phase alignment is unnecessary and the boring blunt crossfade
wins; the GUI checkbox and the self-test print both.

Metrics: same NMR audibility estimate as ramp_lab.py (masked threshold
from Bark spreading + absolute threshold + post-masking + temporal
integration; >0 dB = audible), but the reference here is simply THE
UNTOUCHED ORIGINAL - the ideal concealment is indistinguishable from
never having missed the deadline.  Caveats inherited from ramp_lab: the
comparison is phase-blind and one-sided (missing energy does not count,
only excess), so a continuation that merely LOSES partials during the
gap scores well - the ABX test is the honest arbiter for that.
Stochastic material (chiff, wind noise) shows pessimistic per-bin
excess; settle by ABX.

Playback via paplay (not ffplay/Chrome - see ramp_lab.py header).

Usage:
  python3 experiments/gap_lab.py                 # GUI
  python3 experiments/gap_lab.py --selftest      # headless number table
  python3 experiments/gap_lab.py --selftest x.wav [y.wav ...]
"""
import argparse
import math
import random
import subprocess
import sys
import time
import traceback
import wave as wavemod
from pathlib import Path

import numpy as np

WAV_OUT = Path.home() / "pwdsp-test" / "rampen-demo" / "gap-lab.wav"

K_MAX = 32          # lag search range, samples (junction alignment)
LEAD_S, TAIL_S = 4.0, 4.0       # playback excerpt around the gap
RING_HIST = 32768               # GO's per-channel history ring (HIST in
                                # xorcist.cpp = 4 * NFFT_MAX).  Doubled
                                # when the ring turned out to be what
                                # capped the higher orders: at 16384 a
                                # hop-512 fit gets 24 frames, too few to
                                # identify six roots, which is why order 6
                                # once looked like overfitting.
                                # (GOSoundConcealment::HIST = 2*NFFT_MAX),
                                # zero-initialised - so "not enough history
                                # yet" is simply SILENCE in the ring, not a
                                # special case.  The lab pads the same way,
                                # otherwise early gap positions would run a
                                # different code path than the engine.
SCATTER_S = 20.0                # stretch covered by the scatter mode
SCATTER_MAX = 3000              # compute guard (~3 ms per gap); reported
                                # in the readout whenever it actually bites
MARK_DETAIL_MAX = 24            # above this, draw bands only (no lines):
                                # 150 gaps x 5 items would crawl, and the
                                # individual lines are visual noise anyway


# ---------------------------------------------------------------------------
# Continuation strategies.  Each takes (history, n) and returns n samples
# of RAW continuation - no fade curve applied (the gap is a continuation;
# only the re-entry crossfade fades, and that happens in conceal()).
# ---------------------------------------------------------------------------

def fade_curve(n):
    """Cosine from 1 to 0 (same flank as ramp_lab / PipeWire's ramp)."""
    return 0.5 + 0.5 * np.cos(np.pi * np.arange(n) / n)


def hold_cont(hist, n):
    return np.full(n, float(hist[-1]) if len(hist) else 0.0)


def burg_cont(hist, n, order):
    """Burg continuation, verbatim DSP from ramp_lab.burg() minus the
    fade multiplication: windowless fit (covariance-class accuracy),
    reflection coefficients |k| <= 1 by construction -> stable poles,
    no artificial damping (the continuation inherits the material's own
    decay rate)."""
    v = np.asarray(hist, float)
    if len(v) < 2 * order + 8:
        return hold_cont(v, n)
    f = v[1:].copy()
    b = v[:-1].copy()
    a = np.zeros(order)
    for m in range(order):
        den = float(np.dot(f, f) + np.dot(b, b))
        if den <= 0:
            return hold_cont(v, n)
        k = 2.0 * float(np.dot(f, b)) / den
        a_new = a.copy()
        a_new[m] = k
        a_new[:m] = a[:m] - k * a[m - 1::-1][:m]
        a = a_new
        f, b = f[1:] - k * b[1:], b[:-1] - k * f[:-1]
    state = list(v[-order:][::-1])
    out = np.empty(n)
    for i in range(n):
        new = float(np.dot(a, state))
        out[i] = new
        state = [new] + state[:-1]
    return out


def _safety_cap(out, lead):
    """The continuation must never get louder than the recent material
    (+20 % headroom); scale, don't clip (see ramp_lab)."""
    limit = 1.2 * float(np.abs(lead[-2000:]).max()) if len(lead) else 1.0
    peak = float(np.abs(out).max()) if len(out) else 0.0
    if peak <= limit or peak == 0.0:
        return out
    return out * (limit / peak)


def history_match_cont(hist, n, search=16384, tmpl=192):
    """Packet-loss concealment the boring classic way (G.711 App. I
    family): find the stretch of RECENT HISTORY most similar to the
    signal just before the gap, then replay what FOLLOWED it.  No
    model, no fit: the continuation is real past signal, so it carries
    all partials, the noise floor and the room sound alike - by
    construction no spectral holes and no detuned satellites (the two
    audible defects of an under-modelled AR fit on real music).  Entry
    is phase-aligned by construction (that is what the template match
    optimises).  Failure mode: the replayed beat/reverb pattern is
    o/sr seconds stale, and non-stationary material (transients) can
    produce a brief echo-like repeat."""
    v = np.asarray(hist, float)
    if len(v) < n + tmpl + 1024:
        return hold_cont(v, n)
    search = int(min(search, len(v) - n - tmpl - 1))
    t = v[-tmpl:]
    tn = float(np.dot(t, t))
    if tn <= 0:
        return hold_cont(v, n)
    L = len(v)
    # windows a[i:i+tmpl] of region a end at L-n-search+i in v-coords,
    # i = 0..search; replay offset o = n + search - i (o >= n so the
    # continuation exists entirely in the past).
    a = v[L - tmpl - n - search: L - n]
    corr = np.correlate(a, t, mode="valid")
    sq = np.concatenate([[0.0], np.cumsum(a * a)])
    en = sq[tmpl:] - sq[:-tmpl]
    ncc = corr / (np.sqrt(en * tn) + 1e-12)
    i = int(np.argmax(ncc))
    q = L - n - search + i                   # window end = replay start
    cont = v[q: q + n].copy()
    if en[i] > 0:                            # match the entry level
        cont *= float(np.clip(math.sqrt(tn / en[i]), 0.25, 4.0))
    return cont


def specfreeze_cont(hist, n, nfft=2048, ana_hop=None, mag_trend=0.0,
                    trend_lo=0.5, trend_hi=1.10, trend_bin_w=1.0, chirp=0.0,
                    chirp_max=math.pi / 2):
    """Phase-vocoder freeze: continue EVERY STFT bin with the frequency
    implied by its phase advance over the last two history frames.
    Carries hundreds of partials at once with no AR order limit, and
    keeps the noise floor's spectral envelope alive (with frozen-
    magnitude character) - the complement to Burg's order starvation.
    Failure mode: 'freeze' quality on transients and a slightly
    metallic tail on long horizons (irrelevant at 1 buffer).

    The rotator is normalize(F1 * conj(F0)) - algebraically identical to
    the textbook omega + princarg(...) formulation (the 2*pi wraps cancel
    inside the exponential), but without any transcendentals.

    ana_hop decouples the ANALYSIS hop from the synthesis hop (which
    stays nfft/4, where Hann satisfies COLA and the frame count is
    minimal).  Motivation: in the realtime engine the analysis frames sit
    at the end of the history ring, so if ana_hop equals the audio period
    size, THIS period's frame is exactly the frame the NEXT period needs
    as its older one - one FFT per period instead of two.  The synthesis
    rotator is then the integer power rot**(syn_hop/ana_hop), which
    cancels the 2*pi wraps just as exactly.  The open question this
    option is here to answer: does the shorter measurement baseline
    (fewer samples between the two frames) degrade the frequency
    estimate on dense material?"""
    syn_hop = nfft // 4
    if ana_hop is None:
        ana_hop = syn_hop
    if syn_hop % ana_hop:
        raise ValueError("syn_hop must be an integer multiple of ana_hop")
    v = np.asarray(hist, float)
    if len(v) < nfft + ana_hop * (2 if chirp > 0.0 else 1):
        return hold_cont(v, n)
    w = np.hanning(nfft)
    F1 = np.fft.rfft(v[-nfft:] * w)
    F0 = np.fft.rfft(v[-nfft - ana_hop:-ana_hop] * w)

    z = F1 * np.conj(F0)                     # cross spectrum per bin
    mag = np.abs(z)
    rot_raw = np.where(mag > 1e-30, z / np.where(mag > 0, mag, 1.0), 1.0)
    m_hop = syn_hop // ana_hop
    rot = rot_raw ** m_hop                   # integer power: wraps cancel

    if mag_trend > 0.0:
        # Per-bin MAGNITUDE trend, the modulus twin of the phase rotator:
        # |F1|/|F0| is what normalising z threw away, and z/|F0|^2 is the
        # full complex ratio F1/F0 that advances phase AND level at once.
        # Applied geometrically like the rotator, so a bin decaying by
        # x dB per hop keeps decaying by x dB per hop.
        #
        # Bounded, not one-sided: a swell box opening is a real
        # crescendo.  The bound exists only because the extrapolation
        # COMPOUNDS over the ~5 synthesis frames of one shadow.  1.10 per
        # hop is +0.83 dB / 21 ms = 39 dB/s (beyond any swell pedal) and
        # at most +4.1 dB over the whole horizon.  Sweep: fast swell
        # -6.5 -> -10.3 dB excess, held organ tone -16.7 -> -18.7, dense
        # material 0.6 dB worse.
        e0 = F0.real ** 2 + F0.imag ** 2
        r = np.where(e0 > 1e-30, mag / np.maximum(e0, 1e-30), 1.0)
        # GLOBAL vs PER-BIN.  A per-bin ratio does not distinguish "this
        # partial got louder" from "this partial moved into the next
        # bin": on a glissando 36 % of all bins exceed the clamp while
        # the global level does not move at all (measured), and beating
        # does the same on a detuned chord (11 %).  The global ratio -
        # the RMS level change between the two frames - is immune to
        # both, because energy that only migrates between bins cancels
        # in the sum.  trend_bin_w blends: 0 = global only, 1 = per-bin.
        e1 = F1.real ** 2 + F1.imag ** 2
        g = math.sqrt(e1.sum() / max(e0.sum(), 1e-30))
        if trend_bin_w < 1.0:
            r = g * (np.maximum(r, 1e-6) / g) ** trend_bin_w
        # The clamp is specified per SYNTHESIS hop, so a shorter analysis
        # baseline has to clamp proportionally tighter - otherwise
        # ana 128 would permit 1.10 per 2.7 ms = eight times the growth.
        lo = trend_lo ** (1.0 / m_hop)
        hi = trend_hi ** (1.0 / m_hop)
        rot = rot * (np.clip(r, lo, hi) ** (mag_trend * m_hop))

    # Per-bin CHIRP: the next term of the same Taylor series.  The
    # rotator freezes each bin's frequency; a glissando changes it, and
    # the change is measurable from a THIRD frame - q = rot_new /
    # rot_old is the per-hop change of the phase advance, i.e. the
    # frequency slope.  Applying rot*q^k turns the linear phase ramp
    # into a quadratic one, which is exactly a linear chirp.
    #
    # Clamped in ANGLE: pi/2 of q equals one FFT bin of frequency change
    # per hop (a bin-wide deviation advances phase by 2*pi*(sr/nfft)*hop
    # = pi/2 at hop = nfft/4).  Noisy bins would otherwise wander off.
    q = None
    if chirp > 0.0:
        z_old = F0 * np.conj(np.fft.rfft(
            v[-nfft - 2 * ana_hop:-2 * ana_hop] * w))
        a_old = np.abs(z_old)
        rot_old = np.where(a_old > 1e-30,
                           z_old / np.where(a_old > 0, a_old, 1.0), 1.0)
        # Per-ANALYSIS-hop change of the per-analysis-hop phase advance.
        dq = np.angle(rot_raw * np.conj(rot_old))
        # Per SYNTHESIS hop the advance is m_hop times larger and changes
        # m_hop times as often -> m_hop**2.
        dq = np.clip(dq, -chirp_max, chirp_max) * chirp * m_hop ** 2
        q = np.exp(1j * dq)

    out = np.zeros(n + nfft)
    wsum = np.zeros(n + nfft) + 1e-12
    k_end = int(math.ceil((n + nfft) / syn_hop)) + 1
    spec = F1.copy()
    cur = rot if q is None else rot.copy()
    for k in range(1, k_end):            # frame k spans [k*hop-nfft, k*hop)
        spec = spec * cur
        if q is not None:                # frequency itself moves on
            cur = cur * q
        start = k * syn_hop - nfft
        frame = np.fft.irfft(spec, nfft) * w
        a0, a1 = max(0, start), min(n + nfft, start + nfft)
        if a0 >= a1:
            continue
        out[a0:a1] += frame[a0 - start:a1 - start]
        wsum[a0:a1] += w[a0 - start:a1 - start] ** 2
    return out[:n] / wsum[:n]


def prony_cont(hist, n, nfft=4096, ana_hop=128, nfit=16, order=2,
               ridge=0.0):
    """Per-bin AR(order) continuation - Prony's method, one fit per bin.

    The phase vocoder's rotator IS an order-1 recursion X(n) = z X(n-1)
    with |z| = 1, which is EXACT for one sinusoid per bin and structurally
    wrong for two.  M sinusoids in a bin satisfy an order-M recursion
    exactly, its roots being their per-hop rotations, so the honest
    generalisation is to fit that recursion instead of assuming M = 1.

    `nfit` is the number of equations, i.e. how far back the fit looks:
    nfit * ana_hop samples on top of the frame.

    HISTORY OF THIS FUNCTION'S RIDGE (see
    tools/concealment/notes/mathevorlesung.md for the full derivation):
    the first version shipped ridge=1e-3 and measured as barely better
    than order 1 outside the dense chord.  That was the ridge, not the
    model: for two close frequencies the solution is two LARGE nearly
    cancelling coefficients (a ~ 2z, b ~ -z^2) whose difference carries
    all the information, and shrinking them pulls the fit exactly back
    onto AR(1).  ρ must sit below the reciprocal condition number, and at
    6 Hz separation that is ~1e-4, so 1e-3 regularised the signal away.
    The real safeguard is the root handling below; ridge stays available
    for float32 ports (1e-6 keeps ~95 % of the gain).

    Measured at 600/min in the full chain (anchor corrector, safety cap,
    self-feeding ring):

                                2 sines  1 tone  chord  attacks  Vierne
      AR(1) rotator (engine)      -2.1    -14.5   -5.6    -13.7    -3.0
      AR(2) h128 n16 ridge 1e-3   -1.7    -14.2  -12.3    -13.8    -2.3
      AR(2) h128 n16 ridge 0     -66.1    -21.6  -14.5    -20.8    -3.2
      AR(2) h512 n14 ridge 0     -95.8    -22.6  -17.4    -22.0    -3.8
      AR(2) h1024 n8 ridge 0    -104.1    -21.8  -19.0    -21.3    -3.4

    The 147/153 Hz pair inside one bin is SOLVED, not improved - the
    earlier claim that a 6 Hz beat needs ~167 ms of observation was the
    ridge bias, not an information limit.  A COARSER hop helps AR(2)
    because the two roots sit Δω·hop apart on the unit circle: the hop is
    the lever arm of the separation, not the sampling of the envelope.
    (For the order-1 rotator the opposite held - short hop wins - which
    is why the engine's AR(1) uses hop 128.)

    The remaining true limit is identifiability at ONSETS: a tone that
    enters ε before the gap is observation-equivalent to an amplitude
    ramp, with B·Δω degenerate, so no algorithm can predict that beat.
    The consequence is an ORDER GATE per bin (fall back to AR(1) when the
    2x2 system is ill-conditioned), still to be built.

    Cost here is dominated by recomputing nfit+order frames per call.  In
    the engine the fit would slide as exponentially forgotten sums (RLS,
    five complex accumulators per bin, one MAC round per period); the
    solve runs only when a prediction is actually needed.  float32 needs
    either double accumulators or periodic rescaling.
    """
    syn_hop = nfft // 4
    m_hop = syn_hop // ana_hop
    v = np.asarray(hist, float)
    if len(v) < nfft + (nfit + order) * ana_hop:
        return hold_cont(v, n)
    w = np.hanning(nfft)
    X = []
    for j in range(nfit + order):
        e = len(v) - j * ana_hop
        X.append(np.fft.rfft(v[e - nfft:e] * w).astype(np.complex128))

    if order == 1:
        num = sum(X[j] * np.conj(X[j + 1]) for j in range(nfit))
        mag = np.abs(num)
        a = np.where(mag > 1e-30, num / np.where(mag > 0, mag, 1.0), 1.0)
        b = np.zeros_like(a)
    else:
        # Complex least squares on  X[j] = a X[j+1] + b X[j+2].
        S11 = sum(X[j + 1] * np.conj(X[j + 1]) for j in range(nfit))
        S22 = sum(X[j + 2] * np.conj(X[j + 2]) for j in range(nfit))
        S12 = sum(X[j + 1] * np.conj(X[j + 2]) for j in range(nfit))
        S21 = np.conj(S12)
        r1 = sum(X[j] * np.conj(X[j + 1]) for j in range(nfit))
        r2 = sum(X[j] * np.conj(X[j + 2]) for j in range(nfit))
        # Ridge: wherever a bin really holds ONE partial the two columns
        # are collinear and the system is singular - which is most bins.
        tr = np.real(S11 + S22) * ridge + 1e-30
        d = (S11 + tr) * (S22 + tr) - S12 * S21
        d = np.where(np.abs(d) < 1e-30, 1e-30, d)
        a = (r1 * (S22 + tr) - r2 * S21) / d
        b = (r2 * (S11 + tr) - r1 * S12) / d
        # Roots of z^2 - a z - b.  ONLY roots already near the unit circle
        # are snapped onto it.  Worked through for a single sinusoid the
        # fit returns exactly {z, -z/2}: the second is the regulariser's
        # shadow and must be left to die.  Forcing it to |z| = 1 installs
        # a permanent parasitic oscillation - measured +4.1 dB on a pure
        # sine where the plain rotator reaches -75.
        disc = np.sqrt(a * a + 4.0 * b)
        z1, z2 = 0.5 * (a + disc), 0.5 * (a - disc)
        for z in (z1, z2):
            m = np.abs(z)
            inv = 1.0 / np.maximum(m, 1e-30)
            z *= np.where(m > 0.9, inv, np.minimum(1.0, inv))
        a = z1 + z2
        b = -(z1 * z2)

    out = np.zeros(n + nfft)
    wsum = np.zeros(n + nfft) + 1e-12
    k_end = int(math.ceil((n + nfft) / syn_hop)) + 1
    cur, prev = X[0], X[1]
    for k in range(1, k_end):
        for _ in range(m_hop):          # advance one ANALYSIS hop a time
            cur, prev = a * cur + b * prev, cur
        start = k * syn_hop - nfft
        fr = np.fft.irfft(cur, nfft) * w
        a0, a1 = max(0, start), min(n + nfft, start + nfft)
        if a0 >= a1:
            continue
        out[a0:a1] += fr[a0 - start:a1 - start]
        wsum[a0:a1] += w[a0 - start:a1 - start] ** 2
    return out[:n] / wsum[:n]


def prony_p(hist, n, nfft=4096, ana_hop=None, nfit=20, order=4, ridge=0.0,
            ring=RING_HIST, clamp=0.9, stab="roots"):
    """Per-bin AR(p) continuation for ARBITRARY order - Prony generalised.

    Order 2 was an assumption, not a derivation: a 4096-bin of a triad
    holds three or four partials (D, F# and A plus their overtones
    collide more than once), not two.  Measured in the full chain at
    600/min, hop 512, nfit 20 - within GO's 16384-sample history ring,
    which allows 24 frames at that hop:

                      order 1  order 2  order 3  order 4  order 6
      D 1 Ton           -14.4    -22.8    -24.6    -24.2    -17.3
      D Aufbau Attacks  -14.0    -22.2    -24.5    -24.7    -12.6
      D voller Akkord   -11.0    -18.1    -22.4    -23.3    -20.8
      Vierne dicht       -2.8     -4.4     -4.2     -3.8     -3.2

    Order 6 collapses (overfitting), and the higher orders need the
    longer span: order 4 at nfit 8 is 3.7 dB worse than at nfit 20.

    `stab` selects how the fitted polynomial is kept from exploding over
    the horizon.  "roots" clamps only the roots that are ALREADY near the
    unit circle onto it and lets the fit's shadow roots decay; it is what
    the engine does.  "mag" (a per-bin level ceiling, which would have
    been nearly free in C) measured 7-9 dB worse at order 4 and is kept
    only as the record of that.  "none" is the unstabilised control.
    """
    if ana_hop is None:
        ana_hop = nfft // 8
    syn_hop = nfft // 4
    steps = max(1, syn_hop // ana_hop)
    v = np.asarray(hist, float)
    if ring:
        v = v[-ring:]
    avail = (len(v) - nfft) // ana_hop - order
    if avail < 3:
        return np.zeros(n)
    J = min(nfit, avail)
    if order < 1 or J < order + 1:
        return np.zeros(n)

    w = np.hanning(nfft)
    # index 0 = newest, spaced ana_hop
    X = np.empty((J + order, nfft // 2 + 1), np.complex128)
    for j in range(J + order):
        e = len(v) - j * ana_hop
        X[j] = np.fft.rfft(v[e - nfft:e] * w)

    # Normal equations of  X[j] = sum_k a_k X[j+k],  k = 1..order
    # P[k,l] = sum_j X[j+k] conj(X[j+l]),  q[k] = sum_j X[j] conj(X[j+k])
    nb = X.shape[1]
    P = np.empty((nb, order, order), np.complex128)
    q = np.empty((nb, order), np.complex128)
    for k in range(order):
        Xk = X[1 + k:1 + k + J]
        q[:, k] = np.einsum("jb,jb->b", X[:J], np.conj(Xk))
        for l in range(order):
            Xl = X[1 + l:1 + l + J]
            P[:, k, l] = np.einsum("jb,jb->b", Xk, np.conj(Xl))
    if ridge:
        tr = np.real(np.einsum("bkk->b", P)) * ridge + 1e-30
        P[:, np.arange(order), np.arange(order)] += tr[:, None]
    else:
        P[:, np.arange(order), np.arange(order)] += 1e-30

    # P is indexed [k,l] = <X_k, X_l>; the system is  sum_l a_l P[l,k] = q[k]
    try:
        # q[..., None] on purpose: since NumPy 2.0 a right-hand side of
        # shape (batch, m) is read as a MATRIX (m, n), not as a stack of
        # vectors, so passing q directly raises a core-dimension mismatch
        # on any NumPy >= 2 while working on older ones.
        a = np.linalg.solve(np.swapaxes(P, 1, 2), q[..., None])[..., 0]
    except np.linalg.LinAlgError:
        return np.zeros(n)
    a = np.nan_to_num(a, nan=0.0, posinf=0.0, neginf=0.0)

    # Roots of  z^order - a1 z^(order-1) - ... - a_order
    comp = np.zeros((nb, order, order), np.complex128)
    comp[:, 0, :] = a
    if order > 1:
        idx = np.arange(order - 1)
        comp[:, idx + 1, idx] = 1.0
    if stab == "roots":
        if order == 2:
            # Closed form; batched eigvals would be 2049 LAPACK calls.
            disc = np.sqrt(a[:, 0] ** 2 + 4.0 * a[:, 1])
            z = np.stack([0.5 * (a[:, 0] + disc), 0.5 * (a[:, 0] - disc)], 1)
        else:
            # Durand-Kerner, vectorised over ALL bins at once - the same
            # iteration the engine runs.  np.linalg.eigvals on a batch of
            # companion matrices calls LAPACK once PER BIN and made these
            # entries 30x slower than the reference strategy (20.5 ms per
            # gap at order 4), which looks like a frozen window rather
            # than a slow one.
            ang = 2 * np.pi * np.arange(order) / order + 0.35
            z = 0.9 * np.exp(1j * ang)[None, :] * np.ones((nb, 1))
            for _ in range(40):
                v = np.ones(z.shape, np.complex128)
                for j in range(order):
                    v = v * z - a[:, j:j + 1]
                d = np.ones(z.shape, np.complex128)
                for j in range(order):
                    d = d * np.where(
                        np.arange(order)[None, :] == j, 1.0,
                        z - z[:, j:j + 1])
                step = v / np.where(np.abs(d) < 1e-300, 1e-300, d)
                z = z - step
                if np.max(np.abs(step)) < 1e-10:
                    break
        m = np.abs(z)
        # Only roots ALREADY near the circle are snapped onto it; the
        # fit's shadow roots must stay inside and die over the horizon.
        z = z * np.where(m > clamp, 1.0 / np.maximum(m, 1e-30), 1.0)
        # Coefficients back from the clamped roots.
        c = np.zeros((nb, order + 1), np.complex128)
        c[:, 0] = 1.0
        for i in range(order):
            c[:, 1:] = c[:, 1:] - z[:, i:i + 1] * c[:, :-1]
        a = -c[:, 1:]

    out = np.zeros(n + nfft)
    wsum = np.zeros(n + nfft) + 1e-12
    k_end = int(math.ceil((n + nfft) / syn_hop)) + 1
    state = X[:order].copy()          # state[k] = X(n-k), k = 0..order-1
    lim = np.abs(X[0]) ** 2           # no bin may exceed its start level
    for kf in range(1, k_end):
        for _ in range(steps):
            nxt = np.einsum("bk,kb->b", a, state)
            if stab == "mag":
                e = nxt.real ** 2 + nxt.imag ** 2
                over = e > lim
                if over.any():
                    nxt = np.where(
                        over, nxt * np.sqrt(lim / np.maximum(e, 1e-30)), nxt)
            state[1:] = state[:-1]
            state[0] = nxt
        st = kf * syn_hop - nfft
        fr = np.fft.irfft(state[0], nfft) * w
        a0, a1 = max(0, st), min(n + nfft, st + nfft)
        if a0 >= a1:
            continue
        out[a0:a1] += fr[a0 - st:a1 - st]
        wsum[a0:a1] += w[a0 - st:a1 - st] ** 2
    return out[:n] / wsum[:n]

# The engine's Burg constants (GOSoundConcealment::BURG_ORDER / _WINDOW).
# The lab MUST use the same ones or it cannot predict what GO sounds like:
# with order 64 on 2048 samples the lab's bridge was markedly better than
# the engine's, so lab measurements systematically UNDERSTATED the
# bridge's artifacts - found on 2026-08-03 by a sample-by-sample
# cross-check (scratchpad/cross_check.cpp) that agreed to 57-69 dB
# wherever the bridge stayed out and fell to 31 dB as soon as it
# contributed.
GO_BURG_ORDER = 32
GO_BURG_WINDOW = 512


def with_entry_bridge(fn, bridge_len=96):
    """Fix the entry discontinuity of non-AR strategies.

    A Burg recursion is continuous at the gap entry by construction
    (seeded with the actual last samples - continuous up to the
    order-th derivative); replay and spectral freeze are NOT, and the
    residual step showed up in listening as a broadband impulse
    (vertical spectrogram stripe) at the entry.  Remedy: crossfade the
    first ~2 ms of the strategy's output against a short Burg bridge,
    which starts perfectly continuous.  Legitimate in the real
    scenario too - the worker computes both, the bridge is ephemeral."""
    def g(h, n):
        raw = np.asarray(fn(h, n), float).copy()
        m = min(bridge_len, n)
        bridge = burg_cont(h[-GO_BURG_WINDOW:], m, GO_BURG_ORDER)
        w = 0.5 - 0.5 * np.cos(np.pi * np.arange(m) / m)      # 0 -> 1
        raw[:m] = bridge * (1 - w) + raw[:m] * w
        return raw
    return g


def with_anchor_corrector(fn, tau=16, order=GO_BURG_ORDER,
                          N=GO_BURG_WINDOW):
    """Fix the entry discontinuity WITHOUT a Burg crossfade.

    Band-split analysis on a real aliquot-rich organ tone (2026-08-02)
    showed the 2 ms Burg bridge is itself the dominant seam error: the
    order-starved recursion injects a low/mid-band error burst ~17 dB
    ABOVE the raw freeze error ('dull, quiet thump', independent of
    gap width).  Remedy: keep the freeze untouched and subtract the
    entry jump as a decaying exponential,

        out[i] = raw[i] - (raw[0] - v0) * exp(-i/tau)

    where the anchor v0 is Burg's ONE-step prediction.  Key insight:
    order starving only degrades the free-running recursion; the
    one-step prediction is the very residual the fit minimizes and
    stays exact even on dense material.  A naive linear-extrapolation
    anchor fails on curved waveforms (injects -20 dB errors on clean
    tones where the freeze was perfect at -100 dB).
    Real tone g128: -1.9 -> -17 dB NMR+; clean signals unchanged."""
    def g(h, n):
        raw = np.asarray(fn(h, n), float).copy()
        v0 = burg_cont(h[-N:], 1, order)[0]
        raw -= (raw[0] - v0) * np.exp(-np.arange(n) / tau)
        return raw
    return g


def with_slope_anchor(fn, tau=16, order=32, N=1024):
    """Anchor that makes VALUE AND SLOPE continuous.

    The "pale high flame" over the gap is spectral splatter: the error
    jumps from zero to its full value at the seam, and that discontinuity
    scatters broadband.  Zeroing the value (see with_anchor_corrector)
    leaves a kink, whose spectrum still falls only 12 dB/oct; matching
    the slope as well adds another 6 dB/oct.  Measured on the synthetic
    chord: HF error -42 -> -56 dBFS.
    The catch: the slope needs Burg's TWO-step prediction, which is
    already free-running by one sample and unreliable on dense real
    material - the held organ tone degrades from -17.2 to +0.7 dB.  Use
    only where the spectrum is sparse."""
    def g(h, n):
        raw = np.asarray(fn(h, n), float).copy()
        p = burg_cont(h[-N:], 2, order)
        a = raw[0] - p[0]
        b = (raw[1] - p[1]) * math.exp(1.0 / tau) - a
        i = np.arange(n)
        raw -= (a + b * i) * np.exp(-i / tau)
        return raw
    return g


def with_adaptive_seam(nfft=4096, ana_hop=None, **freeze_kw):
    """Adaptive Naht: Anker auf gehaltenem Material, Burg-Bruecke auf
    Transienten - stufenlos gemischt.

    Befund (Vierne-Finale, 02.08.): an Transienten-Naehten ist der Freeze
    inhaltlich falsch; dort rettet die 2-ms-Bruecke, weil sie den falschen
    Inhalt ERSETZT (AR sagt Transienten kurzfristig gut voraus) - exakte
    Stetigkeit war nicht der Grund (Mikro-Bruecke 16sa fiel durch, besserer
    Anker-Fit half nicht).  Auf gehaltenen Toenen ist es genau umgekehrt
    (Bruecke = tieffrequenter 2-ms-Bums).  Diskriminator: Kosinus-
    Aehnlichkeit s der Betragsspektren der beiden PV-Analyserahmen
    (gehaltener Ton s >= 0.9995, Transienten <= 0.97; in C++ gratis, die
    Rotator-Normierung berechnet Sum|F1||F0| ohnehin).
    w = clamp((0.999 - s)/0.009): s>=0.999 -> reiner Anker, <=0.990 ->
    reine Bruecke.  Misst auf BEIDEN Testdateien das jeweilige Optimum."""
    # The level trend is DENSITY GATED exactly as the engine does it
    # (exponent 1 - wgt): on dense material the level fluctuation is
    # beating, not an envelope.  Building raw_fn per call is what makes
    # that possible - a fixed raw_fn applied the full trend even at
    # wgt = 1, which was a silent lab-vs-engine mismatch.
    trend_base = freeze_kw.pop("mag_trend", 0.0)

    def make_raw(wgt):
        return lambda h, n: specfreeze_cont(
            h, n, nfft, ana_hop=ana_hop,
            mag_trend=trend_base * (1.0 - wgt), **freeze_kw)

    syn_hop = nfft // 4
    hop = syn_hop if ana_hop is None else ana_hop
    # The discriminator MUST use the same frame pair as the rotator -
    # measuring it over a different distance would force the second FFT
    # that a short analysis hop exists to avoid.  But (1 - s) shrinks
    # roughly with hop**2, so the thresholds have to be scaled back to
    # the syn_hop reference or a short hop would call everything
    # "sustained" (measured at ana 128: the dense Vierne still reads
    # s = 0.997..0.9996, i.e. above the unscaled 0.999 threshold).
    # Normalised to THIS frame size's own classic hop, NOT to a global
    # time base.  Across frame sizes (1 - s) legitimately differs: at a
    # fixed hop it still falls 52x from 2k to 8k on a dense chord while
    # staying flat on a single sustained tone.  That is frequency
    # RESOLUTION - wide 2k bins hold several beating partials, which is
    # exactly when the phase vocoder's model breaks - so more bridge at
    # 2k is correct, not a miscalibration.
    d_scale = (syn_hop / hop) ** 2
    def g(h, n):
        if len(h) < nfft + hop:
            # Defensive only - reachable when a strategy is called with a
            # raw history directly.  Via conceal() the history is padded
            # to RING_HIST with silence (the engine's zero-initialised
            # ring), and the engine itself skips ComputeShadow entirely
            # while m_HistFill is short.
            wgt = 1.0
        else:
            w = np.hanning(nfft + 1)[:-1]
            F1 = np.abs(np.fft.rfft(h[-nfft:] * w))
            F0 = np.abs(np.fft.rfft(h[-nfft - hop:-hop] * w))
            s = float((F1 * F0).sum()
                      / max(np.sqrt((F1**2).sum() * (F0**2).sum()), 1e-18))
            d = (1.0 - s) * d_scale        # normalised to the syn_hop base
            wgt = min(1.0, max(0.0, (d - 0.001) / 0.009))
        raw_fn = make_raw(wgt)
        if wgt <= 0.0:
            return with_anchor_corrector(raw_fn)(h, n)
        if wgt >= 1.0:
            return with_entry_bridge(raw_fn)(h, n)
        return (wgt * np.asarray(with_entry_bridge(raw_fn)(h, n), float)
                + (1 - wgt) * with_anchor_corrector(raw_fn)(h, n))
    return g


def burg_sustain_cont(hist, n, order, N):
    """Burg continuation with the 'sustain assumption'.

    On dense chords, partial pairs closer than sr/N are unresolvable;
    the fit models the pair as ONE line whose beat envelope it reads
    as DAMPING - the pole radii shrink and the whole continuation sags
    by a few dB over 10 ms (the audible 'dent', immune to model
    order).  An organ tone, however, does not decay within 10 ms, so
    flatten the continuation's own envelope: measure the RMS decay
    between the first and last third and compensate with the inverse
    exponential, capped at +8 dB.  (Half the time the true beat is
    even rising - holding the level is the unbiased guess.)"""
    out = burg_cont(hist[-N:], n, order)
    third = max(8, n // 3)
    a = float(np.sqrt(np.mean(out[:third] ** 2)))
    b = float(np.sqrt(np.mean(out[-third:] ** 2)))
    if b > 1e-9 and a > 1e-9 and b < a:
        c1, c2 = third / 2, n - third / 2
        slope = math.log(min(a / b, 2.51)) / (c2 - c1)
        # Anchor the envelope at 1.0 AT THE ENTRY - anchoring it at c1
        # put a 1-2 dB level step right on the seam (audible as a
        # broadband impulse; found by listening).
        out = out * np.minimum(np.exp(slope * np.arange(n)), 2.51)
    return out


# "xrun" is the baseline: silence in the gap, HARD edges on both sides -
# what an actual deadline miss without concealment sounds like.  It
# ignores the crossfade setting on purpose.
#
try:
    import xorcist as _xo
except OSError as _e:
    import sys
    print(f"WARNUNG: libxorcist nicht ladbar ({_e}) - GO-Strategien laufen "
          "als numpy-NACHBAU (kann vom Engine-Verhalten abweichen!)",
          file=sys.stderr)
    _xo = None


def _build_stamp():
    """Which libxorcist am I hearing?

    On 2026-08-06 the deployed .so turned out to be a plain -O3 build
    while the numbers quoted in chat came from a -march=native one - the
    same source, but 12 dB apart at order 20, because the normal
    equations are that ill-conditioned up there.  Nobody could tell by
    looking, so now the title bar says it.  The stamp file is written by
    tools/concealment/deploy_lab.sh; a missing one means the .so was
    copied by hand and its provenance is unknown.
    """
    if _xo is None:
        return "numpy-NACHBAU - libxorcist fehlt!"
    try:
        so = Path(_xo._lib._name)
        age = time.strftime("%d.%m. %H:%M", time.localtime(so.stat().st_mtime))
        stamp = so.with_suffix(".stamp")
        if stamp.exists():
            return f"lib {stamp.read_text().splitlines()[0]}"
        return f"lib {so.name} ({age}) - OHNE Stempel, Herkunft unbekannt"
    except Exception as e:                                  # never fatal
        return f"lib ? ({e})"


# Buffer size the GO strategies mirror (GOSoundConcealment uses the audio
# period as its analysis hop).  The GUI keeps this in step with the gap
# spin, because in the engine one gap IS one period.
GO_BUFFER = 128


def scatter_positions(samples, sr, gap_n, fade_n, pos, rate, regular=False,
                      seed=0xC0FFEE):
    """The gap positions the GUI would use - THE one implementation.

    Added 06.08.2026 because measuring scripts had been re-deriving this
    by hand and quietly disagreeing with the lab they were supposed to
    explain (different start, different end, different gap count, and
    therefore numbers that could not be compared with what the window
    displayed).  Same reason the numpy reimplementation of the engine was
    retired in favour of binding the library: one implementation cannot
    drift from itself.
    """
    span = gap_n + fade_n
    pos = min(max(pos, 2048), max(2048, len(samples) - gap_n - fade_n - 2048))
    if rate <= 0:
        return [pos], False
    # IMMER die ganze Datei bestreuen (Nutzer 08.08.): der Regler
    # positioniert nur noch die EINZEL-Luecke (rate 0).  Vorher startete
    # die Streuung an der Reglerposition und endete nach SCATTER_S
    # Sekunden - "ab wann gewuerfelt wird" war nie die gewuenschte
    # Semantik.
    last = max(2048, len(samples) - span - 2048)
    end = last
    positions = []
    if regular:
        step = max(gap_n, int(round(60.0 / rate * sr)))
        t = 2048
        while t < end and len(positions) < SCATTER_MAX:
            positions.append(t)
            t += step
    else:
        p = rate / 60.0 * gap_n / sr
        # Die Saat ist ein REGLER (Nutzer 10.08.): ob ein Transient sauber
        # klingt, haengt stark davon ab, ob zufaellig ein Lueckenklumpen
        # genau darauf faellt.  Mit fester Saat hoert man immer dieselbe
        # Wuerfelung und haelt deren Eigenheiten fuer Eigenschaften des
        # Verfahrens.  Saat durchzaehlen = dieselbe Dichte, andere Lage.
        rng = random.Random(seed)
        t = 2048
        while t < end and len(positions) < SCATTER_MAX:
            if rng.random() < p:
                positions.append(t)
            t += gap_n
    return (positions or [pos]), t < end


def metric_positions(positions):
    """The gaps the GUI actually evaluates - an even sample of at most
    150, because the NMR metric costs ~14 ms each."""
    return (positions if len(positions) <= 150
            else positions[::-(-len(positions) // 150)])


def go_strategy(nfft, **tuning):
    """EXACTLY what GrandOrgue runs for this mode.

    `tuning` forwards single XorcistTuning fields (seam, noTrend, anaHop,
    freqDamping) to the library, so an entry can differ from the stock
    engine in exactly ONE knob.  That is the whole point: the older
    "Labor:" entries reimplemented the engine in numpy AND changed two
    things at once (forced seam plus trend off), so a listening
    comparison against "GO:" never isolated the seam.

    Calls libxorcist - the very code the engine links - instead of
    reimplementing it.  On 2026-08-03 a sample-by-sample cross-check
    showed the numpy reimplementation had silently drifted apart
    (different Burg order, an ungated level trend, a different refit
    schedule), so listening conclusions drawn here did not transfer to
    the product.  One implementation cannot drift, and it is ~11x
    faster.

    Falls back to the numpy path if the library is not built, so the lab
    still runs - the fallback is marked in the strategy list at import.
    """
    mode = {1024: 4, 2048: 1, 4096: 2, 8192: 3}[nfft]

    def g(h, n):
        if _xo is None:
            return with_adaptive_seam(
                nfft=nfft, ana_hop=_go_ana_hop(nfft), mag_trend=1.0,
                trend_bin_w=0.0)(h, n)
        # The library thinks in PERIODS, the lab places gaps at arbitrary
        # samples.  Pad the FRONT so the history ends exactly on a period
        # boundary - otherwise up to buf-1 samples are dropped and the
        # prediction starts in the wrong place (seen as ncc ~0.3 and a
        # junction lag of a third of a period).
        hh = np.asarray(h, np.float32)
        pad = (-len(hh)) % GO_BUFFER
        if pad:
            hh = np.concatenate((np.zeros(pad, np.float32), hh))
        # warm = periods whose predict() actually runs.  2 is the cheapest
        # FAITHFUL setting for the rotator - but the AR fit needs P + 2
        # grid points (one every 4 periods) before useAr even arms, so
        # with warm=2 every PRONY entry silently fell back to ORDER 1 in
        # this per-gap path.  That is why the dropdown's PRONY entries
        # never sounded like the numpy prototypes: they were order 1
        # wearing a PRONY label.  Engine-faithful chains were never
        # affected (they call predict() every period).
        order = int((tuning or {}).get("order", 1))
        warm = 2 if order < 2 else 4 * (order + 2) + 12
        pred = _xo.predict_at(hh, len(hh), n, buf=GO_BUFFER, mode=mode,
                              warm=warm, tuning=tuning or None)[:, 0]
        if len(pred) < n:      # horizon is shorter than the request
            pred = np.concatenate((pred, np.zeros(n - len(pred))))
        return pred.astype(float)
    # Marks this as an ENGINE strategy: _build() then runs the whole
    # signal through one instance (engine_run) instead of calling g()
    # per gap, which is the only way the streak continuity and the
    # clean-seed gate are in play.  g() stays as the fallback for
    # callers that only want a single continuation.
    g.engine = None if _xo is None else (mode, dict(tuning))
    return g


def split_strategy(nfft_lo, nfft_hi, fc):
    """Band-split entry: two engine instances, see split_engine_run()."""
    m = {1024: 4, 2048: 1, 4096: 2, 8192: 3}

    def g(h, n):
        # Single-continuation fallback (metric harnesses that call the
        # strategy directly): the low instance alone, which is the half
        # that carries the energy.
        return go_strategy(nfft_lo)(h, n)
    g.engine_split = (
        None if _xo is None else (m[nfft_lo], m[nfft_hi], float(fc)))
    return g


def _go_ana_hop(nfft):
    syn = nfft // 4
    return GO_BUFFER if syn % GO_BUFFER == 0 and GO_BUFFER <= syn else syn


# NAMING.  Every entry spells out all four knobs of the model, because
# leaving any of them implicit is what caused the lab and the engine to
# disagree:
#
#   frame <n>   FFT frame size (the "PV 2k/4k/8k" the Audio Monitor shows)
#   ana <n>     ANALYSIS hop - the distance between the two frames whose
#               phase difference measures each bin's frequency.  Short =
#               local and unambiguous, long = averaged and wrap-prone.
#               The engine always uses one audio period (128).  It is the
#               DOMINANT knob for seam splatter: one step 128 -> 256 costs
#               ~6 dB of fill accuracy and 1.5-3 dB of extra broadband at
#               the re-entry, at every frame size.
#   seam        how the first sample joins the real signal: anchor (bend
#               the shadow onto the Burg one-step value), bridge (let a
#               free Burg run replace the seam) or ADAPTIVE (the engine's
#               density discriminator picks per gap).
#   trend       global level trend on/off - continues a swell or decay
#               through the gap, gated off on dense material.
#
# The "GO:" entries are libxorcist, the very code the engine links.  The
# "GO-1knob:" entries are the SAME library with exactly one field of
# XorcistTuning changed, so an A/B against "GO:" isolates that one knob.
# The "Labor:" entries are numpy probes that do NOT run in the engine.
_GO_TAG = "GO: " if _xo is not None else "GO (numpy-FALLBACK!): "
_K_TAG = "GO-1knob: " if _xo is not None else "GO-1knob (numpy-FALLBACK!): "

STRATEGIES = {
    # ===== GrandOrgue, wie es HEUTE ausgeliefert wird ==================
    # Ganz oben, weil jeder Vergleich gegen diesen Zustand laeuft.
    # Ordnung 1 (Rotator), Trend AN - dort ist der Trend richtig, weil
    # ein einzelner Rotator die Huellkurve nicht selbst tragen kann.
    _GO_TAG + "off (hard xrun)  [die unverdeckte Grundlinie]": "xrun",
    _GO_TAG + "frame 2k, ana 128, seam ADAPTIVE, trend on":
        go_strategy(2048),
    _GO_TAG + "frame 4k, ana 128, seam ADAPTIVE, trend on  [GO-Vorgabe]":
        go_strategy(4096),
    _GO_TAG + "frame 8k, ana 128, seam ADAPTIVE, trend on":
        go_strategy(8192),

    # ===== ANKER-MODI: der Arbeitsbereich ==============================
    # Ganz nach vorne, weil hier gehoert und entschieden wird.  ANKER =
    # der Naht-Zweig, der die Fortsetzung auf den Burg-Einschrittwert
    # biegt, statt die Naht durch einen freien Burg-Lauf zu ERSETZEN.
    #
    # KEIN TREND ab Ordnung 2 (Nutzer, 07.08.): der globale Pegeltrend
    # war eine Ordnung-1-Kalibrierung - er schiebt den Pegel linear durch
    # die Luecke, weil ein einzelner Rotator die Huellkurve nicht selbst
    # tragen kann.  Ab Ordnung 2 traegt der AR-Fit die Amplitude ueber
    # die Wurzelradien, und beides zusammen ist eine doppelte Buchung
    # (auf dichtem Akkord gemessen 7 dB).  Die Trend-Varianten sind
    # deshalb ersatzlos raus; "adaptiv" unten heisst ab jetzt IMMER
    # adaptive NAHT ohne Trend.
    _K_TAG + "frame 4k, PRONY order 4, hop 512, Gedaechtnis 22 Hops, ANKER"
    "  [alte Vorgabe]":
        go_strategy(4096, order=4, seam=1, noTrend=1),
    _K_TAG + "frame 4k, PRONY order 1, hop 512, Gedaechtnis 10 Hops, ANKER"
    "  [reiner Rotator]":
        go_strategy(4096, order=1, seam=1, noTrend=1),
    _K_TAG + "frame 4k, PRONY order 2, hop 512, Gedaechtnis 14 Hops, ANKER":
        go_strategy(4096, order=2, seam=1, noTrend=1),
    _K_TAG + "frame 4k, PRONY order 3, hop 512, Gedaechtnis 18 Hops, ANKER":
        go_strategy(4096, order=3, seam=1, noTrend=1),
    _K_TAG + "frame 4k, PRONY order 5, hop 512, Gedaechtnis 26 Hops, ANKER":
        go_strategy(4096, order=5, seam=1, noTrend=1),
    _K_TAG + "frame 4k, PRONY order 6, hop 512, Gedaechtnis 30 Hops, ANKER":
        go_strategy(4096, order=6, seam=1, noTrend=1),
    _K_TAG + "frame 4k, PRONY order 8, hop 512, Gedaechtnis 38 Hops, ANKER":
        go_strategy(4096, order=8, seam=1, noTrend=1),
    _K_TAG + "frame 4k, PRONY order 12, hop 512, Gedaechtnis 54 Hops, ANKER":
        go_strategy(4096, order=12, seam=1, noTrend=1),
    _K_TAG + "frame 4k, PRONY order 16, hop 512, Gedaechtnis 70 Hops, ANKER":
        go_strategy(4096, order=16, seam=1, noTrend=1),
    _K_TAG + "frame 4k, PRONY order 20, hop 512, Gedaechtnis 86 Hops, ANKER":
        go_strategy(4096, order=20, seam=1, noTrend=1),
    # ---- KURZE RAHMEN, volle Ordnungsreihe ----------------------------
    # Die Frage vom 07.08.: holt eine hoehere Prony-Ordnung zurueck, was
    # ein kurzes Fenster an Frequenzaufloesung verliert?  1k = 21 ms
    # Fenster, 47 Hz Bins, Hop 128 - vierfache Zeitaufloesung gegenueber
    # 4k.  Wenn ja, braucht es keine Umschaltung, dann steuert die
    # Ordnung allein die Gedaechtnislaenge.
    _K_TAG + "frame 1k, PRONY order 5, hop 128, Gedaechtnis 26 Hops, ANKER"
    "  [bester Schlimmstfall aller Messungen: 6,95 dB]":
        go_strategy(1024, order=5, seam=1, noTrend=1),
    _K_TAG + "frame 1k, PRONY order 1, hop 128, Gedaechtnis 10 Hops, ANKER":
        go_strategy(1024, order=1, seam=1, noTrend=1),
    _K_TAG + "frame 1k, PRONY order 2, hop 128, Gedaechtnis 14 Hops, ANKER":
        go_strategy(1024, order=2, seam=1, noTrend=1),
    _K_TAG + "frame 1k, PRONY order 3, hop 128, Gedaechtnis 18 Hops, ANKER":
        go_strategy(1024, order=3, seam=1, noTrend=1),
    _K_TAG + "frame 1k, PRONY order 4, hop 128, Gedaechtnis 22 Hops, ANKER":
        go_strategy(1024, order=4, seam=1, noTrend=1),
    _K_TAG + "frame 1k, PRONY order 6, hop 128, Gedaechtnis 30 Hops, ANKER":
        go_strategy(1024, order=6, seam=1, noTrend=1),
    _K_TAG + "frame 1k, PRONY order 8, hop 128, Gedaechtnis 38 Hops, ANKER":
        go_strategy(1024, order=8, seam=1, noTrend=1),
    _K_TAG + "frame 1k, PRONY order 12, hop 128, Gedaechtnis 54 Hops, ANKER":
        go_strategy(1024, order=12, seam=1, noTrend=1),
    _K_TAG + "frame 1k, PRONY order 16, hop 128, Gedaechtnis 70 Hops, ANKER":
        go_strategy(1024, order=16, seam=1, noTrend=1),
    _K_TAG + "frame 1k, PRONY order 20, hop 128, Gedaechtnis 86 Hops, ANKER":
        go_strategy(1024, order=20, seam=1, noTrend=1),
    _K_TAG + "frame 2k, PRONY order 3, hop 256, Gedaechtnis 18 Hops, ANKER":
        go_strategy(2048, order=3, seam=1, noTrend=1),
    _K_TAG + "frame 2k, PRONY order 5, hop 256, Gedaechtnis 26 Hops, ANKER":
        go_strategy(2048, order=5, seam=1, noTrend=1),
    _K_TAG + "frame 2k, PRONY order 6, hop 256, Gedaechtnis 30 Hops, ANKER":
        go_strategy(2048, order=6, seam=1, noTrend=1),
    _K_TAG + "frame 2k, PRONY order 12, hop 256, Gedaechtnis 54 Hops, ANKER":
        go_strategy(2048, order=12, seam=1, noTrend=1),
    _K_TAG + "frame 2k, PRONY order 16, hop 256, Gedaechtnis 70 Hops, ANKER":
        go_strategy(2048, order=16, seam=1, noTrend=1),
    _K_TAG + "frame 2k, PRONY order 20, hop 256, Gedaechtnis 86 Hops, ANKER":
        go_strategy(2048, order=20, seam=1, noTrend=1),
    _K_TAG + "frame 1k, PRONY order 4, hop 128, Gedaechtnis 22 Hops, adaptiv":
        go_strategy(1024, order=4, noTrend=1),

    # ---- Anker auf den anderen Rahmen ---------------------------------
    # Der KURZE Rahmen ist seit dem 07.08. kein Nebenschauplatz mehr:
    # gemessen bei 600 Luecken/min, Ordnung 4, gegen frame 4k
    #
    #             Einsaetze 0-6k   6-12k      Schwebung 0-6k   18-24k
    #   frame 2k     -32.1 (+4.2)  -28.4 (+7.5)   -24.9 (-12.8)  +0.1
    #   frame 4k     -27.9         -20.9          -37.7          -6.7
    #
    # - also genau die Komplementaritaet, die der Nutzer gehoert hat
    # (2k: sauberer Transient, schrebbeliges Plateau).  Diese Eintraege
    # sind die Endpunkte fuer den geplanten Hybrid.
    _K_TAG + "frame 2k, PRONY order 4, hop 256, Gedaechtnis 22 Hops, ANKER"
    "  [kurz: Einsatz +4..7 dB, Plateau -13 dB]":
        go_strategy(2048, order=4, seam=1, noTrend=1),
    _K_TAG + "frame 8k, PRONY order 4, hop 1024, Gedaechtnis 22 Hops, ANKER"
    "  [lang]":
        go_strategy(8192, order=4, seam=1, noTrend=1),
    _K_TAG + "frame 2k, PRONY order 2, hop 256, Gedaechtnis 14 Hops, ANKER":
        go_strategy(2048, order=2, seam=1, noTrend=1),
    _K_TAG + "frame 2k, PRONY order 8, hop 256, Gedaechtnis 38 Hops, ANKER"
    "  [Vorgabe - Hybrid-Basis]":
        go_strategy(2048, order=8, seam=1, noTrend=1),
    _K_TAG + "frame 8k, PRONY order 8, hop 1024, Gedaechtnis 38 Hops, ANKER":
        go_strategy(8192, order=8, seam=1, noTrend=1),
    _K_TAG + "frame 8k, PRONY order 12, hop 1024, Gedaechtnis 54 Hops, ANKER":
        go_strategy(8192, order=12, seam=1, noTrend=1),


    # ===== ADAPTIVE NAHT ab Ordnung 2 (kein Trend) =====================
    # Die adaptive Naht bleibt als Achse erhalten: fuer sehr bewegtes
    # Material (Vierne) kann der Bruecken-Zweig noch gebraucht werden.
    _K_TAG + "frame 4k, PRONY order 4, hop 512, Gedaechtnis 22 Hops, adaptiv":
        go_strategy(4096, order=4, noTrend=1),
    _K_TAG + "frame 4k, PRONY order 8, hop 512, Gedaechtnis 38 Hops, adaptiv":
        go_strategy(4096, order=8, noTrend=1),
    _K_TAG + "frame 4k, PRONY order 12, hop 512, Gedaechtnis 54 Hops, adaptiv":
        go_strategy(4096, order=12, noTrend=1),
    _K_TAG + "frame 4k, PRONY order 4, hop 512, Gedaechtnis 22 Hops, BRUECKE":
        go_strategy(4096, order=4, seam=2, noTrend=1),

    # ===== Ordnung 1: die Naht-Achse isoliert ==========================
    _K_TAG + "frame 4k, ana 128, seam ANCHOR, trend on":
        go_strategy(4096, seam=1),
    _K_TAG + "frame 4k, ana 128, seam BRIDGE, trend on":
        go_strategy(4096, seam=2),
    _K_TAG + "frame 4k, ana 128, seam ADAPTIVE, trend OFF":
        go_strategy(4096, noTrend=1),

    # ===== Grenzwerte des Modells ======================================
    # Der Analyse-Hop an seinen Extremen.  128 ist der der Engine;
    # frame/4 ist die klassische Phasenvocoder-Wahl und die obere Grenze
    # der Bibliothek (er muss den Synthese-Hop teilen).
    _K_TAG + "frame 4k, ana 256, seam ADAPTIVE, trend on  [hop x2]":
        go_strategy(4096, anaHop=256),
    _K_TAG + "frame 4k, ana 1024, seam ADAPTIVE, trend on  [hop max = frame/4]":
        go_strategy(4096, anaHop=1024),
    # Frequenzdaempfung an beiden Enden: 0 ersetzt den Rotator jedes Bins
    # rundheraus, 1 friert ihn vollstaendig ein.  Die Engine liefert 0.
    _K_TAG + "frame 4k, ana 128, freq damping 0 %  [classic replace]":
        go_strategy(4096, freqDamping=0.0),
    _K_TAG + "frame 4k, ana 128, freq damping 100 %  [fully frozen]":
        go_strategy(4096, freqDamping=1.0),
    _K_TAG + "frame 4k, PRONY order 8, hop 512, Gedaechtnis 38 Hops, SCHUR-Kappung  [verliert immer: bei rho=1.0 die Singularitaet, sonst ~1 dB gegen die Wurzelkappung]":
        go_strategy(4096, order=8, schur=1),

    # ---- NOT in the engine yet: per-bin AR instead of one rotator -----
    # Prony per bin, ridge 0 since the maths lecture (see prony_cont's
    # docstring and tools/concealment/notes/mathevorlesung.md).  "hop" is
    # the ANALYSIS hop = the lever arm separating the two roots; "span N"
    # = N hops of fitting history on top of the frame.
    # Higher orders: a 4k bin of a triad holds three or four partials,
    # so order 2 was an assumption rather than a derivation.  See
    # prony_p() for the table; order 6 overfits and collapses.
    "Versuch: Prony AR(4), 4k, hop 512, span 20"
    "  [Akkord -23.3 dB - bester Gesamtwert]":
        with_anchor_corrector(lambda h, n: prony_p(h, n, order=4, nfit=20)),
    "Versuch: Prony AR(3), 4k, hop 512, span 20"
    "  [1 Ton -24.6, Vierne -4.2]":
        with_anchor_corrector(lambda h, n: prony_p(h, n, order=3, nfit=20)),
    "Versuch: Prony AR(2), 4k, hop 512, span 20  [zum Vergleich]":
        with_anchor_corrector(lambda h, n: prony_p(h, n, order=2, nfit=20)),
    "Versuch: Prony AR(6), 4k, hop 512, span 20  [ueberangepasst, kippt]":
        with_anchor_corrector(lambda h, n: prony_p(h, n, order=6, nfit=20)),
    "Versuch: Prony AR(2), 4k, hop 512, span 14"
    "  [BESTER: Akkord +11.8, 1 Ton +8.1, Schwebung geloest]":
        with_anchor_corrector(
            lambda h, n: prony_cont(h, n, ana_hop=512, nfit=14)),
    "Versuch: Prony AR(2), 4k, hop 1024, span 8"
    "  [Akkord-Spitze -19.0, sonst knapp hinter hop 512]":
        with_anchor_corrector(
            lambda h, n: prony_cont(h, n, ana_hop=1024, nfit=8)),
    "Versuch: Prony AR(2), 4k, hop 128, span 16"
    "  [Engine-Hop, zeigt was der Hebelarm bringt]":
        with_anchor_corrector(lambda h, n: prony_cont(h, n, nfit=16)),
    "Versuch: Prony AR(2), 4k, hop 512, span 14, ridge 1e-3"
    "  [das alte Wuergeband, nur zum Vergleich]":
        with_anchor_corrector(
            lambda h, n: prony_cont(h, n, ana_hop=512, nfit=14,
                                    ridge=1e-3)),
    "Versuch: Prony AR(1), 4k, hop 128, span 16"
    "  [nur die lange Spanne - in der Engine fast gratis]":
        with_anchor_corrector(
            lambda h, n: prony_cont(h, n, nfit=16, order=1)),
    # ---- NOT in the engine yet: two frames at once --------------------
    # Heisenberg per band instead of one compromise frame.  See
    # split_engine_run() for the measurements and the controls.
    "Versuch: Bandsplit 8k unten / 4k oben, Trennung 1600 Hz  [+2.0 dB vs 4k]":
        split_strategy(8192, 4096, 1600),
    "Versuch: Bandsplit 8k unten / 4k oben, Trennung 400 Hz":
        split_strategy(8192, 4096, 400),
    "Versuch: Bandsplit 8k unten / 2k oben, Trennung 1600 Hz":
        split_strategy(8192, 2048, 1600),
    # ---- lab-only probes (NOT what the engine runs) -------------------
    "Labor: hold last value  [degenerate reference, always audible]":
        lambda h, n: hold_cont(h, n),
    "Labor: Burg(12) N=128  [too few partials - satellite tones]":
        lambda h, n: burg_cont(h[-128:], n, 12),
    "Labor: Burg(24) N=256  [short-window bias still audible]":
        lambda h, n: burg_cont(h[-256:], n, 24),
    "Labor: Burg(32) N=512  [what GO uses for the seam bridge]":
        lambda h, n: burg_cont(h[-512:], n, 32),
    "Labor: Burg(48) N=1024  [single stops, few partials]":
        lambda h, n: burg_cont(h[-1024:], n, 48),
    "Labor: Burg(64) N=2048  [small ensembles]":
        lambda h, n: burg_cont(h[-2048:], n, 64),
    # High-order probes: order carries ~order/2 partials, so a real
    # organ mix wants far more than 64.  Sweep result (synth chord):
    # the excess (satellites) falls monotonically with order and is
    # inaudible from ~96 up, but the missing-energy plateau at long
    # gaps (~+4.5 dB at 512) does NOT yield to order - unresolvable
    # near-coincident partials pull the fitted pole radii inward and
    # the whole continuation decays a few dB over 10 ms.
    "Labor: Burg(96) N=2048  [full organ, but costly]":
        lambda h, n: burg_cont(h[-2048:], n, 96),
    "Labor: Burg(128) N=4096  [full organ, RT-borderline]":
        lambda h, n: burg_cont(h[-4096:], n, 128),
    "Labor: Burg(192) N=8192  [reference for 'what order buys']":
        lambda h, n: burg_cont(h[-8192:], n, 192),
    "Labor: Burg(96)+sustain  [FAILED - level dip is not a decay]":
        lambda h, n: burg_sustain_cont(h, n, 96, 2048),
    "Labor: Burg(192)+sustain  [FAILED, same reason]":
        lambda h, n: burg_sustain_cont(h, n, 192, 8192),
    "Labor: history match: replay + 2 ms bridge  [no holes, but combs at the seam]":
        with_entry_bridge(lambda h, n: history_match_cont(h, n)),
    # PV frame-size sweep (chord, gap 512): 2048 = 23 Hz bins mix
    # several chord partials into one bin frequency; 4096 was the
    # sweet spot on BOTH metrics (-3.2/-5.1), 8192 trades missing
    # energy for excess (170 ms frame goes stale).
    "Labor: PV 2k, ana 512, bridge  [bins too wide, partials merge]":
        with_entry_bridge(lambda h, n: specfreeze_cont(h, n, 2048)),
    "Labor: PV 4k, ana 1024, bridge  [the old default]":
        with_entry_bridge(lambda h, n: specfreeze_cont(h, n, 4096)),
    "Labor: PV 8k, ana 2048, bridge  [170 ms frame goes stale]":
        with_entry_bridge(lambda h, n: specfreeze_cont(h, n, 8192)),
    # Anchor corrector instead of the 2 ms Burg crossfade - the bridge
    # was the residual dull artifact on real aliquot-rich tones.
    "Labor: PV 2k, ana 512, anchor  [cheapest of the good ones]":
        with_anchor_corrector(lambda h, n: specfreeze_cont(h, n, 2048)),
    "Labor: PV 4k, ana 1024, anchor  [sustained tones, sparse spectrum]":
        with_anchor_corrector(lambda h, n: specfreeze_cont(h, n, 4096)),
    "Labor: PV 4k, ana 1024, anchor value+slope  [least HF splatter, sparse only]":
        with_slope_anchor(lambda h, n: specfreeze_cont(h, n, 4096)),
    # The three entries above tie the analysis hop to frame/4, so raising
    # the frame ALSO lengthens the hop - and the hop is the dominant term
    # for seam splatter, not the frame.  Measured at 600/min regular on
    # the D major file: one step from hop 128 to 256 costs ~6 dB of fill
    # accuracy and 1.5-3 dB of extra broadband at the re-entry, at EVERY
    # frame size.  Within the frame/4 family the SMALL frame therefore
    # wins (2k/512 fill +0.4 dB, entry -1.8 dB vs 4k/1024 +1.4 / -1.2);
    # with the hop held at 128 the order reverses (8k -9.9, 4k -5.2,
    # 2k -1.6).  These three hold the hop so the frame has its own axis.
    "Labor: PV 2k, ana 128, anchor  [pure frame axis, hop held at GO's]":
        with_anchor_corrector(
            lambda h, n: specfreeze_cont(h, n, 2048, ana_hop=128)),
    "Labor: PV 8k, ana 128, anchor  [pure frame axis, hop held at GO's]":
        with_anchor_corrector(
            lambda h, n: specfreeze_cont(h, n, 8192, ana_hop=128)),
    "Labor: PV 4k, ana 1024, ADAPTIVE seam  [anchor <-> bridge by density]":
        with_adaptive_seam(),
    "Labor: PV 4k, ana 1024, ADAPTIVE + trend  [engine seam, long hop]":
        with_adaptive_seam(mag_trend=1.0, trend_bin_w=0.0),
    # Analysis hop = one audio period: the engine can then reuse the
    # previous period's frame instead of computing a second FFT.
    # Measured better on EVERY material, not just cheaper.
    "Labor: PV 4k, ana 128, ADAPTIVE + trend  [= GO PV 4k, fresh Burg fit]":
        with_adaptive_seam(ana_hop=128, mag_trend=1.0, trend_bin_w=0.0),
    "Labor: PV 2k, ana 128, ADAPTIVE + trend  [cheapest, merges chord partials]":
        with_adaptive_seam(nfft=2048, ana_hop=128, mag_trend=1.0,
                           trend_bin_w=0.0),
    "Labor: PV 8k, ana 128, ADAPTIVE + trend  [rotator^16, still fine]":
        with_adaptive_seam(nfft=8192, ana_hop=128, mag_trend=1.0,
                           trend_bin_w=0.0),
    "Labor: PV 4k, ana 1024, ADAPTIVE + per-bin trend  [BAD: follows beating]":
        with_adaptive_seam(mag_trend=1.0, trend_bin_w=1.0),
    # Chirp = next Taylor term (frequency SLOPE from a third frame).
    # LAB ONLY: wins big on a true sustained glissando but costs 3 dB on
    # a held organ tone, where the measured "slope" is beating noise.
    "Labor: PV 4k, ana 1024, ADAPTIVE + trend + chirp  [glissando only]":
        with_adaptive_seam(mag_trend=1.0, trend_bin_w=0.0, chirp=1.0),
    "Labor: PV 4k, ana 1024, freeze + chirp only  [glissando, isolated]":
        with_anchor_corrector(
            lambda h, n: specfreeze_cont(h, n, 4096, chirp=1.0)),
    # Analysis-hop sweep: shorter = more local frequency estimate; 128
    # equals one audio period, which lets the engine reuse the previous
    # period's frame instead of computing a second FFT.
    "Labor: PV 4k, ana 128, anchor  [one audio period - local, saves an FFT]":
        with_anchor_corrector(
            lambda h, n: specfreeze_cont(h, n, 4096, ana_hop=128)),
    "Labor: PV 4k, ana 256, bridge  [hop sweep]":
        with_entry_bridge(
            lambda h, n: specfreeze_cont(h, n, 4096, ana_hop=256)),
    "Labor: PV 4k, ana 512, bridge  [hop sweep]":
        with_entry_bridge(
            lambda h, n: specfreeze_cont(h, n, 4096, ana_hop=512)),
}


# ---------------------------------------------------------------------------
# Junction alignment: measure the lag, optionally warp the tail.
# ---------------------------------------------------------------------------

def measure_lag(pred, orig, pos, gap_n, fade_n, sr):
    """Residual lag of the prediction at the junction, in samples.

    Normalised cross-correlation of pred[gap_n+k ...] against the true
    material orig[pos+gap_n ...] over a window of about the crossfade
    length; integer peak plus parabolic sub-sample refinement.  k > 0
    means the prediction runs LATE (its junction content appears k
    samples later than the truth).  Returns (k, peak_ncc).  ncc near 1
    means the mismatch really is a common time shift; low ncc means the
    partials have drifted apart individually and NO global lag can fix
    the junction (then warping cannot help either - that is exactly
    the delta-NMR question this lab answers)."""
    win = int(np.clip(fade_n, 128, 512))
    kmax = min(K_MAX, gap_n - 1)
    tgt = orig[pos + gap_n: pos + gap_n + win]
    win = len(tgt)
    if win < 32:
        return 0.0, 0.0
    tn = float(np.dot(tgt, tgt))
    if tn <= 0:
        return 0.0, 0.0
    ks = np.arange(-kmax, kmax + 1)
    ncc = np.empty(len(ks))
    for i, k in enumerate(ks):
        seg = pred[gap_n + k: gap_n + k + win]
        d = float(np.dot(seg, seg))
        ncc[i] = float(np.dot(seg, tgt)) / math.sqrt(d * tn) if d > 0 else 0.0
    j = int(np.argmax(ncc))
    k = float(ks[j])
    if 0 < j < len(ks) - 1:                       # parabolic refinement
        c0, c1, c2 = ncc[j - 1], ncc[j], ncc[j + 1]
        den = c0 - 2 * c1 + c2
        if den < 0:
            k += 0.5 * (c0 - c2) / den
    return k, float(ncc[j])


def warp_tail(pred, junction, k, warp_len):
    """Resample the prediction so it arrives at the junction shifted by
    k samples - the phase-correct re-entry idea.

    The shift is absorbed BEFORE the junction, inside the gap, where
    the prediction plays alone at full weight: a cosine-smooth ramp
    from shift 0 to shift k over warp_len samples.  There the warp is
    only a tiny momentary pitch bend (k/warp_len fractional); from the
    junction on the shift is constant, so the whole crossfade window
    runs phase-aligned.  The prediction is ephemeral - nothing
    downstream consumes it - so this cascades into nothing."""
    idx = np.arange(len(pred), dtype=float)
    s = np.ones(len(pred))
    z0 = junction - warp_len
    ramp = (idx >= z0) & (idx < junction)
    s[idx < z0] = 0.0
    s[ramp] = 0.5 - 0.5 * np.cos(np.pi * (idx[ramp] - z0) / warp_len)
    src = np.clip(idx + k * s, 0, len(pred) - 1)
    return np.interp(src, np.arange(len(pred)), pred)


def split_engine_run(x, positions, gap_n, sr, mode_lo, mode_hi, fc, taps=257):
    """Two engine instances, split by frequency instead of switched.

    The uncertainty trade is not the same in every band.  Low down, a
    chord's partials stand close together in Hz and move slowly, so
    resolution wins; up top they are sparse and attack-dominated, so time
    accuracy wins.  Running ONE long and ONE short instance and taking
    each where it is strong beats either alone - and beats switching the
    frame size per gap, which loses the spectrum cache and the rotator
    keep at every switch and therefore gains nothing at all (measured:
    -11.9 dB switching vs -12.0 for fixed 8k).

    Measured on the D major buildup at 600/min, 4k default = -10.7 dB:

      8k low / 4k high, fc 400..3200 Hz      -12.4 .. -12.7
      fixed 8k                               -12.0
      fixed 4k                               -10.7
      4k low / 8k high (swapped)             -10.1   <- direction matters
      both instances 4k                      -10.6   <- mechanism alone
                                                        contributes nothing

    Costs two FFT chains instead of one; the crossover itself is a
    linear-phase FIR and only runs during concealment.
    """
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * fc / sr * n) * np.hanning(taps)
    h = (h / h.sum()).astype(np.float32)
    d = (taps - 1) // 2
    buf = GO_BUFFER
    n_gap = max(1, gap_n // buf)
    miss = set()
    for p in positions:
        k = p // buf
        for j in range(n_gap):
            miss.add(k + j)
    ch = x.shape[1]
    out = x.copy()
    lo = _xo.Xorcist(ch, buf, sr, mode_lo, _xo.default_wisdom())
    hi = _xo.Xorcist(ch, buf, sr, mode_hi, _xo.default_wisdom())
    tail = np.zeros((taps - 1, ch), np.float32)
    try:
        for i in range(len(x) // buf):
            blk = np.ascontiguousarray(x[i * buf:(i + 1) * buf], np.float32)
            if i in miss:
                a, _ = lo.conceal()
                b, _ = hi.conceal()
                o = np.empty((buf, ch), np.float32)
                for c in range(ch):
                    xa = np.concatenate((tail[:, c], a[:, c]))
                    xb = np.concatenate((tail[:, c], b[:, c]))
                    ya = np.convolve(xa, h)[d:d + len(xa)][-buf:]
                    yb = (xb - np.convolve(xb, h)[d:d + len(xb)])[-buf:]
                    o[:, c] = ya + yb
                # Both instances self-feed what was actually played, not
                # their own half of it.
                lo.replace_last(o)
                hi.replace_last(o)
                out[i * buf:(i + 1) * buf] = o
                tail = np.concatenate((tail, o))[-(taps - 1):]
            else:
                out[i * buf:(i + 1) * buf] = lo.passthrough(blk)
                hi.passthrough(blk)
                lo.push(blk)
                hi.push(blk)
                tail = np.concatenate((tail, blk.reshape(-1, ch)))[-(taps - 1):]
            lo.predict()
            hi.predict()
    finally:
        lo.close()
        hi.close()
    engine_run.patches = _xo.patch_log() if _xo is not None else []
    # Lief das Geraet wirklich?  Ein gesetztes Haekchen beweist nichts.
    engine_run.gpu = _xo.gpu_stats() if _xo is not None else (0, 0)
    engine_run.hyb = _xo.hyb_stats() if _xo is not None else (0, 0, 0)
    engine_run.hyb_marks = _xo.hyb_log() if _xo is not None else []
    return out, [(0.0, 1.0)] * ch


def engine_run(x, positions, gap_n, sr, mode, tuning, lazy=0, progress=None):
    """Run the WHOLE signal through one library instance, as GO does.

    conceal() below builds each gap independently: fresh instance, the
    history pushed in again, and the already-concealed samples pushed as
    if they were real.  That throws away everything the engine carries
    across periods, and it measured 7.4 dB worse than this path on
    identical miss positions (6000/min, single, first D of the D major
    recording).  Two mechanisms account for it:

      - a RUN of consecutive misses is one continuous prediction in the
        engine; the per-gap path re-predicts every 128 samples from a
        history whose tail is its own previous output, which is the
        self-feeding loop at its worst.  Runs of 3-4 are common at 17 %
        duty.
      - the clean-seed gate never fires, because pushing a concealed
        period as real leaves m_RecentPredMask zero - so the Burg bridge
        is allowed to run from a predicted seed, the very thing the gate
        exists to prevent.

    The re-entry crossfade is the library's own (~3 ms), so the fade spin
    does not apply to the GO strategies - that is the point: they play
    exactly what the engine plays.
    """
    buf = GO_BUFFER
    n_gap = max(1, gap_n // buf)
    miss = set()
    for p in positions:
        k = p // buf
        for j in range(n_gap):
            miss.add(k + j)
    ch = x.shape[1]
    n_per = len(x) // buf
    out = x.copy()
    inst = _xo.Xorcist(ch, buf, sr, mode, _xo.default_wisdom())
    if lazy:
        tuning = dict(tuning or {})
        tuning["solveEvery"] = 1
    if _xo is not None:
        _xo.patch_log_clear()   # nur DIESER Lauf zaehlt
        _xo.hyb_log_clear()
        _xo.solve_tap_clear()
    if tuning:
        inst.set_tuning(**tuning)
    # lazy > 0: volle Vorhersage (Synthese) nur im Fenster [m-lazy, m+2]
    # um jede Luecke m; sonst absorb() - Analyse und Fit laufen weiter,
    # nur der ungehoert verfallende Schatten entfaellt.  Der Zustand
    # bleibt exakt der der Engine; erzwungen wird solveEvery=1, weil die
    # gestreifte Aufloesung waehrend absorb nicht laeuft und die
    # Koeffizienten sonst beliebig alt ans Lueckenfenster kaemen.
    need = None
    if lazy:
        need = bytearray(n_per)
        for m in miss:
            for j in range(max(0, m - lazy), min(n_per, m + 3)):
                need[j] = 1
    try:
        for i in range(n_per):
            blk = np.ascontiguousarray(x[i * buf:(i + 1) * buf], np.float32)
            if i in miss:
                o, _ = inst.conceal()
                out[i * buf:(i + 1) * buf] = o
            else:
                out[i * buf:(i + 1) * buf] = inst.passthrough(blk)
                inst.push(blk)
            if need is None or need[i]:
                inst.predict()
            else:
                inst.absorb()
            # Zuschauen WAEHREND der Rechnung: der Ring der Bibliothek
            # fuellt sich ja laufend, also kann ein Beobachter jederzeit
            # hineinsehen.  Alle 64 Perioden anzuklopfen kostet fast
            # nichts (der Rueckrufer bremst sich selbst nach Uhrzeit);
            # was Zeit kostet, ist erst das Zeichnen - und wer die
            # Mitschrift einschaltet, hat die Zeitmessung ohnehin
            # aufgegeben.
            if progress is not None and (i & 63) == 0:
                # Der Beobachter will nicht nur wissen WIE WEIT, sondern
                # auch WAS: den Ausgang, das ankommende Signal MIT seinen
                # Loechern, und ueber inst.peek() den Schatten, der noch
                # gar nicht gespielt wurde.  Ein Woerterbuch statt immer
                # neuer Parameter - die naechste Anzeige will wieder
                # etwas anderes.
                progress({"period": i, "n_periods": n_per, "out": out,
                          "inst": inst, "x": x, "miss": miss, "buf": buf})
    finally:
        inst.close()
    engine_run.patches = _xo.patch_log() if _xo is not None else []
    # Lief das Geraet wirklich?  Ein gesetztes Haekchen beweist nichts.
    engine_run.gpu = _xo.gpu_stats() if _xo is not None else (0, 0)
    engine_run.hyb = _xo.hyb_stats() if _xo is not None else (0, 0, 0)
    engine_run.hyb_marks = _xo.hyb_log() if _xo is not None else []
    # Solve-Mitschrift des LAUFS, nicht der Sitzung: der Ring wurde oben
    # geleert, hier steht also nur, was dieser Lauf gerechnet hat.  Die
    # Pole kommen nur mit, wenn sie auch angefordert wurden - sie sind
    # das Vielfache der Zusammenfassung.
    engine_run.tap = (_xo.solve_tap(roots=True) if _xo is not None
                      else (None, None, None, None))
    return out, [(0.0, 1.0)] * ch


def conceal(x, pos, gap_n, fade_n, fn, align, sr):
    """Build the concealed signal for all channels of x[n, ch].

    Returns (candidate, info) where candidate is x with
    [pos, pos+gap_n) replaced by the strategy's continuation and
    [pos+gap_n, pos+gap_n+fade_n) crossfaded back into the original;
    info carries per-channel (lag, ncc).  fn == "xrun" bypasses
    everything: zeros in the gap, hard edges."""
    cand = x.copy()
    info = []
    if fn == "xrun":
        cand[pos:pos + gap_n, :] = 0.0
        return cand, [(0.0, 0.0)] * x.shape[1]
    need = gap_n + max(fade_n, 512) + K_MAX + 8
    fo = fade_curve(fade_n)
    for ch in range(x.shape[1]):
        hist = x[:pos, ch]
        if len(hist) < RING_HIST:   # pad with silence, exactly like the
            hist = np.concatenate(  # engine's zero-initialised ring
                (np.zeros(RING_HIST - len(hist)), hist))
        try:
            pred = _safety_cap(fn(hist, need), hist)
        except Exception:
            # A strategy that throws must not take the whole run with it -
            # the scatter runs are minutes of work, and losing them to one
            # bad gap hides which gap it was.  Report once, leave this gap
            # unfilled, carry on.
            import traceback
            if not getattr(conceal, "_warned", False):
                conceal._warned = True
                print("STRATEGIE-FEHLER bei pos=%d ch=%d, Luecke bleibt leer:"
                      % (pos, ch))
                traceback.print_exc()
            pred = np.zeros(need)
        k, ncc = measure_lag(pred, x[:, ch], pos, gap_n, fade_n, sr)
        if align and abs(k) >= 0.05:
            warp_len = int(np.clip(gap_n - K_MAX, 16, 0.005 * sr))
            pred = warp_tail(pred, gap_n, k, warp_len)
        cand[pos:pos + gap_n, ch] = pred[:gap_n]
        b = min(pos + gap_n + fade_n, len(x))
        m = b - (pos + gap_n)
        cand[pos + gap_n:b, ch] = (pred[gap_n:gap_n + m] * fo[:m]
                                   + x[pos + gap_n:b, ch] * (1 - fo[:m]))
        info.append((k, ncc))
    return cand, info


# ---------------------------------------------------------------------------
# Metrics - verbatim from ramp_lab.py (see the docstrings there for the
# psychoacoustic rationale and the calibration anchors).
# ---------------------------------------------------------------------------

def a_weight(f):
    f = np.asarray(f, float)
    f2 = f * f
    ra = (12194.0 ** 2 * f2 * f2) / (
        (f2 + 20.6 ** 2)
        * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2))
        * (f2 + 12194.0 ** 2) + 1e-30)
    return ra * 10 ** (2.0 / 20)


def _stft_mag(s, nfft, hop):
    n = (len(s) - nfft) // hop + 1
    if n <= 0:
        return np.zeros((0, nfft // 2 + 1))
    idx = np.arange(nfft)[None, :] + hop * np.arange(n)[:, None]
    return np.abs(np.fft.rfft(s[idx] * np.hanning(nfft), axis=1))


_SLICE_CAL = {}


def _full_scale_slice(sr, nfft, hop):
    key = (sr, nfft, hop)
    if key not in _SLICE_CAL:
        t = np.arange(8 * nfft) / sr
        M = _stft_mag(np.sin(2 * np.pi * 1000.0 * t), nfft, hop)
        w = a_weight(np.fft.rfftfreq(nfft, 1 / sr))
        _SLICE_CAL[key] = float(np.median(np.sqrt(((M * w) ** 2).sum(axis=1))))
    return _SLICE_CAL[key]


def _bark(f):
    f = np.asarray(f, float)
    return 13.0 * np.arctan(0.00076 * f) + 3.5 * np.arctan((f / 7500.0) ** 2)


_SPREAD = {}


def _spreading_matrix(sr, nfft):
    key = (sr, nfft)
    if key not in _SPREAD:
        z = _bark(np.fft.rfftfreq(nfft, 1 / sr))
        dz = z[:, None] - z[None, :]
        s = np.where(dz >= 0, -12.0 * dz, 27.0 * dz)
        _SPREAD[key] = 10 ** (s / 10)
    return _SPREAD[key]


_BIN_CAL = {}


def _full_scale_bin(sr, nfft, hop):
    key = (sr, nfft, hop)
    if key not in _BIN_CAL:
        t = np.arange(8 * nfft) / sr
        M = _stft_mag(np.sin(2 * np.pi * 1000.0 * t), nfft, hop)
        _BIN_CAL[key] = float(M.max())
    return _BIN_CAL[key]


def _quiet_threshold(fr, sr, nfft, hop, full_scale_spl=95.0):
    f = np.maximum(np.asarray(fr, float), 10.0) / 1000.0
    tq = (3.64 * f ** -0.8
          - 6.5 * np.exp(-0.6 * (f - 3.3) ** 2)
          + 1e-3 * f ** 4)
    mag = _full_scale_bin(sr, nfft, hop) * 10 ** ((tq - full_scale_spl) / 20)
    return mag ** 2


def audibility_metric(cand, ref, pos, n_eval, sr, nfft=2048, hop=512):
    """NMR of cand vs. ref around the concealment (see ramp_lab.py).
    Here ref is the untouched original; n_eval = gap + crossfade."""
    a = max(0, int(pos) - int(0.5 * sr))
    b = min(len(cand), int(pos) + n_eval + int(0.3 * sr))
    K = _stft_mag(cand[a:b], nfft, hop)
    R = _stft_mag(ref[a:b], nfft, hop)
    m = min(len(K), len(R))
    if m == 0:
        return float("nan"), float("nan")
    K, R = K[:m], R[:m]
    ex2 = np.maximum(K - R, 0.0) ** 2
    fr = np.fft.rfftfreq(nfft, 1 / sr)
    thresh = (R ** 2) @ _spreading_matrix(sr, nfft).T * 10 ** (-14 / 10)
    decay = 10 ** (-2.3 * (hop / sr) * 10)
    for i in range(1, len(thresh)):
        np.maximum(thresh[i], thresh[i - 1] * decay, out=thresh[i])
    floor = _quiet_threshold(fr, sr, nfft, hop)
    q = ex2 / (thresh + floor[None, :])
    win = max(1, int(0.150 * sr / hop))
    if len(q) > win:
        c = np.cumsum(q, axis=0)
        q = (c[win:] - c[:-win]) / win
    nmr = 10 * np.log10(float(q.max()) + 1e-30)
    w = a_weight(np.fft.rfftfreq(nfft, 1 / sr))
    slices = np.sqrt(((np.maximum(K - R, 0.0) * w) ** 2).sum(axis=1))
    absolute = 20 * np.log10(float(slices.max()) /
                             _full_scale_slice(sr, nfft, hop) + 1e-12)
    return nmr, absolute


def concealment_metrics(cand, ref, pos, n_eval, sr):
    """(NMR_excess, NMR_missing, absolute) - both defect directions.

    The inherited NMR is ONE-SIDED by design (fade-out heritage:
    'getting quiet early is timing, not a click') and therefore blind
    to the second audible concealment defect: SPECTRAL HOLES - partials
    the predictor could not model simply die during the gap.  Listening
    on real music confirmed those holes as clearly audible, so they get
    their own number: the roles are swapped, and the ORIGINAL's excess
    over the concealment - i.e. what is missing - is judged against
    the masking of what actually plays (the concealment).  Post-masking
    of the identical pre-gap signal is included, which is also the
    mechanism that makes SHORT holes inaudible in reality (continuity
    illusion); for stochastic content the per-bin caveat applies as
    always - pessimistic, settle by ABX."""
    nmr_exc, absolute = audibility_metric(cand, ref, pos, n_eval, sr)
    nmr_mis, _ = audibility_metric(ref, cand, pos, n_eval, sr)
    return nmr_exc, nmr_mis, absolute


# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

def read_wav(path):
    w = wavemod.open(str(path), "rb")
    sr, ch, sw, nf = (w.getframerate(), w.getnchannels(),
                      w.getsampwidth(), w.getnframes())
    raw = w.readframes(nf)
    w.close()
    if sw == 2:
        x = np.frombuffer(raw, "<i2").astype(float) / 32768.0
    elif sw == 4:
        x = np.frombuffer(raw, "<i4").astype(float) / 2 ** 31
    elif sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3)
        x = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8)
             | (b[:, 2].astype(np.int32) << 16))
        x = (x - (x >> 23 << 24)).astype(float) / 2 ** 23
    else:
        raise ValueError(f"sample width {sw} not supported")
    return x.reshape(-1, ch), sr


def organ_chord(sr, dur=10.0, seed=7):
    """Dense deterministic test chord: C-major-ish, 4 notes x 10
    harmonics with 1/h rolloff and a few cents of per-note detune -
    the partial spacing that forces the higher Burg orders."""
    rng = np.random.default_rng(seed)
    t = np.arange(int(dur * sr)) / sr
    sig = np.zeros(len(t))
    for f0 in (130.81, 164.81, 196.00, 261.63):
        f0 = f0 * (1 + rng.uniform(-3e-4, 3e-4))
        for h in range(1, 11):
            f = f0 * h
            if f > sr / 2 - 2000:
                break
            sig += (0.12 / h) * np.sin(2 * np.pi * f * t
                                       + rng.uniform(0, 2 * np.pi))
    sig *= 0.5 / np.abs(sig).max()
    m = int(0.010 * sr)
    sig[:m] *= fade_curve(m)[::-1]
    return sig


def glissando(sr, dur=10.0, f_start=196.0, octaves=1.0, n_harm=10):
    """Continuous pitch slide - a harmonic tone whose f0 rises
    exponentially (constant semitones per second).

    In the spectrogram this is a FAN of diagonals: partial h slides with
    h times the absolute slope, so the higher the partial the steeper
    the line.  Every one of them crosses FFT bins continuously, which is
    precisely the assumption the phase-vocoder freeze breaks - it holds
    each bin's frequency, so the shadow keeps playing the pitch of the
    moment the gap started.  Not an organ problem (no continuous
    glissando on a pipe organ), but the clean test case for the chirp
    term: it is the only signal where a frequency SLOPE really exists."""
    t = np.arange(int(dur * sr)) / sr
    rate = octaves / dur                        # octaves per second
    # phase = integral of f0(t) = f_start * 2**(rate*t)
    ph0 = 2 * np.pi * f_start * (2 ** (rate * t) - 1) / (rate * math.log(2))
    sig = sum((0.12 / h) * np.sin(h * ph0 + 0.7 * h) for h in range(1, n_harm + 1))
    sig *= 0.5 / np.abs(sig).max()
    m = int(0.010 * sr)
    sig[:m] *= fade_curve(m)[::-1]
    return sig


def sine_mixture(sr, dur=10.0, f0=261.63):
    """Pure sines at the ranks of a real Mixtur: 8', 4', 2 2/3', 2',
    1 1/3', 1'.  No detune, no beating - so any level movement in the
    measurement comes from the ENVELOPE, not from partials drifting
    through each other."""
    t = np.arange(int(dur * sr)) / sr
    sig = sum((0.10 / (i + 1)) * np.sin(2 * np.pi * f0 * r * t + 0.6 * i)
              for i, r in enumerate((1, 2, 3, 4, 6, 8)))
    sig *= 0.5 / np.abs(sig).max()
    m = int(0.010 * sr)
    sig[:m] *= fade_curve(m)[::-1]
    return sig


def swell_env(n, sr, kind, span_db=30.0, period_s=2.5):
    """Level movement as a gain curve.

    'cresc'/'decresc' ramp linearly in dB across the whole signal (span
    30 dB over 10 s = 3 dB/s, a slow registration swell); 'swell' is the
    periodic pedal gesture, a raised cosine in dB whose steepest flank
    reaches pi*span/period = 25 dB/s at the default 20 dB / 2.5 s.  Both
    directions occur in one signal there, so any gap position lands on a
    rising or a falling flank."""
    t = np.arange(n) / sr
    total = n / sr
    if kind == "cresc":
        db = -span_db * (1.0 - t / total)
    elif kind == "decresc":
        db = -span_db * (t / total)
    else:
        db = -0.5 * span_db * (1.0 - np.cos(2 * np.pi * t / period_s))
    return 10.0 ** (db / 20.0)


def _moved(gen, kind, **kw):
    """Signal generator with a level movement applied."""
    def f(sr):
        sig = gen(sr)
        return sig * swell_env(len(sig), sr, kind, **kw)
    return f


SIGNALS = {
    "Organ chord (synth)": organ_chord,
    "Sine mixture (8'..1')": sine_mixture,
    # Level movements - the material the magnitude trend is made for.
    "Mixture, crescendo 3 dB/s": _moved(sine_mixture, "cresc"),
    "Mixture, decrescendo 3 dB/s": _moved(sine_mixture, "decresc"),
    "Mixture, swell 20 dB / 2.5 s": _moved(sine_mixture, "swell",
                                           span_db=20.0),
    "Organ chord, crescendo 3 dB/s": _moved(organ_chord, "cresc"),
    "Organ chord, decrescendo 3 dB/s": _moved(organ_chord, "decresc"),
    "Organ chord, swell 20 dB / 2.5 s": _moved(organ_chord, "swell",
                                               span_db=20.0),
    "Glissando 1 oct / 10 s": glissando,
    "Glissando 1 oct / 2 s (fast)": lambda sr: glissando(sr, dur=10.0,
                                                         octaves=5.0),
    "Sine 1 kHz @ 0.2": lambda sr: 0.2 * np.sin(
        2 * np.pi * 1000.0 * np.arange(int(10 * sr)) / sr),
    "3 tones 220/880/3520": lambda sr: sum(
        a * np.sin(2 * np.pi * f * np.arange(int(10 * sr)) / sr + p)
        for f, a, p in ((220, .13, 0), (880, .09, 1.1), (3520, .06, 2.3))),
}


def good_positions(x, sr, count):
    """Spread positions over the loud half of the file so the gap does
    not land in silence (a concealed gap in silence measures nothing)."""
    hop = int(0.100 * sr)
    n = (len(x) - hop) // hop
    rms = np.array([np.sqrt(np.mean(x[i * hop:(i + 1) * hop] ** 2))
                    for i in range(n)])
    ok = np.nonzero(rms >= np.median(rms))[0]
    lo, hi = int(0.1 * len(x)), int(0.85 * len(x))
    cand = [i * hop + hop // 2 for i in ok]
    cand = [p for p in cand if lo <= p <= hi] or [len(x) // 2]
    step = max(1, len(cand) // count)
    return cand[::step][:count]


# ---------------------------------------------------------------------------
# Self-test: headless number table (runs without Qt / audio)
# ---------------------------------------------------------------------------

def selftest(paths):
    sr = 48000
    sources = [("organ chord (synth)", organ_chord(sr)[:, None], sr)]
    for p in paths:
        smp, fsr = read_wav(p)
        sources.append((Path(p).name, smp, fsr))
    gaps = (128, 256, 512)
    fade_ms = 3.0
    # Looked up by prefix so relabelling the menu entries cannot break
    # the self-test again.
    # Matched by content, not exact prefix - the GO entries are renamed
    # to "GO (numpy-FALLBACK!)" when the library is missing.
    def _find(sub):
        return next(k for k in STRATEGIES if sub in k)
    strategies = (_find("off (hard xrun)"), _find("Burg(32)"),
                  _find("Burg(64)"), _find("history match:"),
                  _find("PV 4k  [engine default]"))
    print(f"gap concealment self-test - fade {fade_ms} ms, blunt "
          f"crossfade.  NMR+ = excess (satellites/clicks), NMR- = "
          f"missing energy (spectral holes); > 0 dB = audible.")
    for name, x, fsr in sources:
        positions = good_positions(x[:, 0], fsr, 5)
        print(f"\n=== {name} (sr {fsr}, {len(x)/fsr:.1f} s, "
              f"{len(positions)} positions) ===")
        print(f"{'strategy':26s} {'gap':>5s} {'ms':>5s} "
              f"{'NMR+':>7s} {'NMR-':>7s} {'k':>6s} {'ncc':>5s}")
        for sname in strategies:
            fn = STRATEGIES[sname]
            for gap_n in gaps:
                fade_n = int(fade_ms / 1000 * fsr)
                exc, mis, lags, nccs = [], [], [], []
                for pos in positions:
                    c, info = conceal(x, pos, gap_n, fade_n, fn, False, fsr)
                    ne, nm, _ = concealment_metrics(
                        c[:, 0], x[:, 0], pos, gap_n + fade_n, fsr)
                    exc.append(ne)
                    mis.append(nm)
                    lags.append(info[0][0])
                    nccs.append(info[0][1])
                if fn == "xrun":
                    print(f"{sname:26s} {gap_n:5d} {gap_n/fsr*1e3:5.1f} "
                          f"{np.mean(exc):+7.1f} {np.mean(mis):+7.1f} "
                          f"{'-':>6s} {'-':>5s}")
                else:
                    print(f"{sname:26s} {gap_n:5d} {gap_n/fsr*1e3:5.1f} "
                          f"{np.mean(exc):+7.1f} {np.mean(mis):+7.1f} "
                          f"{np.mean(np.abs(lags)):6.2f} "
                          f"{np.mean(nccs):5.2f}")


# ---------------------------------------------------------------------------
# GUI (only when Qt is available - the self-test runs without it)
# ---------------------------------------------------------------------------

try:
    import pyqtgraph as pg
    from PySide6.QtCore import QPoint, QRect, QRectF, QSize, Qt, QTimer
    from PySide6.QtGui import QIcon
    from PySide6.QtWidgets import (
        QApplication, QCheckBox, QComboBox, QDoubleSpinBox, QFileDialog,
        QHBoxLayout, QLabel, QLayout, QMainWindow, QPushButton, QSizePolicy,
        QScrollArea, QSlider, QSpinBox, QSplitter, QStackedWidget,
        QVBoxLayout,
        QWidget,
    )
    from PySide6 import __version__ as QT_VERSION
    HAVE_QT = True
except ImportError:
    HAVE_QT = False
    QT_VERSION = "?"

# 3D ist OPTIONAL und wird spaet und weich geladen: pyqtgraph.opengl
# braucht PyOpenGL und einen echten GL-Kontext.  Fehlt eins von beidem,
# soll die Ansicht fehlen - nicht das Lab.
try:
    import pyqtgraph.opengl as glw
    HAVE_GL = True
except Exception:
    HAVE_GL = False


if HAVE_QT and HAVE_GL:

    class SafeGLView(glw.GLViewWidget):
        """GLViewWidget, dessen Fehler die Ereignisschleife nie erreichen.

        Ein fehlender oder zu alter GL-Kontext wirft nicht im
        Konstruktor, sondern erst in initializeGL - also MITTEN in Qts
        Zeichenpfad, wo ein try um die Erzeugung nichts nuetzt (gemessen:
        die Ausnahme kam aus processEvents heraus und riss den Aufrufer
        mit).  Hier abgefangen, gemerkt, und der Rest des Labs laeuft
        weiter."""

        failed = False

        def initializeGL(self):
            try:
                super().initializeGL()
            except Exception:
                traceback.print_exc()
                SafeGLView.failed = True

        def paintGL(self, *a, **kw):
            if SafeGLView.failed:
                return
            try:
                super().paintGL(*a, **kw)
            except Exception:
                traceback.print_exc()
                SafeGLView.failed = True


if HAVE_QT:

    class FlowLayout(QLayout):
        """Zeile, die auf die vorhandene Breite umbricht.

        Existiert, weil die Schalterreihe die Mindestbreite des Fensters
        bestimmte: 2428 px allein fuer die Schalter, und jeder neue Knopf
        machte das Fenster breiter als der Bildschirm.  Mit Umbruch haengt
        die Mindestbreite nur noch am breitesten EINZELNEN Schalter, und
        die Reihe darf beliebig weiter wachsen.

        Standardmuster aus der Qt-Dokumentation (heightForWidth), nur mit
        deutschem Kommentar - hier ist nichts zu erfinden."""

        def __init__(self, parent=None, margin=0, spacing=6):
            super().__init__(parent)
            self._items = []
            self._space = spacing
            self.setContentsMargins(margin, margin, margin, margin)

        def addItem(self, item):
            self._items.append(item)

        def count(self):
            return len(self._items)

        def itemAt(self, i):
            return self._items[i] if 0 <= i < len(self._items) else None

        def takeAt(self, i):
            return self._items.pop(i) if 0 <= i < len(self._items) else None

        def expandingDirections(self):
            return Qt.Orientations(Qt.Orientation(0))

        def hasHeightForWidth(self):
            return True

        def heightForWidth(self, width):
            return self._do(QRect(0, 0, width, 0), test=True)

        def setGeometry(self, rect):
            super().setGeometry(rect)
            self._do(rect, test=False)

        def sizeHint(self):
            return self.minimumSize()

        def minimumSize(self):
            s = QSize()
            for it in self._items:
                s = s.expandedTo(it.minimumSize())
            m = self.contentsMargins()
            return s + QSize(m.left() + m.right(), m.top() + m.bottom())

        def _do(self, rect, test):
            m = self.contentsMargins()
            eff = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom())
            x, y, line = eff.x(), eff.y(), 0
            for it in self._items:
                w = it.sizeHint().width()
                h = it.sizeHint().height()
                if line > 0 and x + w > eff.right() + 1:
                    x = eff.x()
                    y += line + self._space
                    line = 0
                if not test:
                    it.setGeometry(QRect(QPoint(x, y), it.sizeHint()))
                x += w + self._space
                line = max(line, h)
            return y + line - rect.y() + m.bottom()
    class GapLab(QMainWindow):
        def __init__(self):
            super().__init__()
            # The build identity belongs where it cannot be overlooked:
            # a lab silently running yesterday's .so looks exactly like
            # one running today's.
            # Auch der ANZEIGE-Stack gehoert in die Titelzeile.  Das Lab
            # laeuft auf dem Host, entwickelt und geprueft wird es in
            # einer venv der VM - Versionsunterschiede sind der
            # Normalfall, nicht die Ausnahme, und sie erklaeren
            # Verhalten, das sonst raetselhaft bleibt (die
            # Farb-Array-Form von GLSurfacePlotItem etwa hat zwischen
            # pyqtgraph-Fassungen gewechselt).
            gl_txt = ("GL" if HAVE_GL else "GL FEHLT (pip install PyOpenGL)")
            self.setWindowTitle(
                f"Gap lab   -   {_build_stamp()}   -   "
                f"pyqtgraph {pg.__version__} / Qt {QT_VERSION} / {gl_txt}")
            icon = QIcon.fromTheme("gap-lab")
            if icon.isNull():
                icon = QIcon(str(Path.home() / ".local/share/icons/hicolor"
                                 "/scalable/apps/gap-lab.svg"))
            self.setWindowIcon(icon)
            self.resize(1300, 840)
            # Hoehe, die sich die Parameterzeilen hoechstens nehmen
            # duerfen.  Alles darueber wird gerollt - der Rest der
            # Fensterhoehe gehoert dem Bild.
            self.CTL_MAX_H = 60
            w = QWidget()
            self.setCentralWidget(w)
            lay = QVBoxLayout(w)

            # Auch die obere Zeile bricht um: sie verlangte 1767 px und
            # war damit nach dem Schrumpfen der Aufklappfelder der
            # naechste Engpass.  Umbruch statt Abschneiden - alle
            # Knoepfe bleiben sichtbar, egal wie schmal das Fenster ist.
            row_w = self.row_w = QWidget()
            row = FlowLayout(row_w, spacing=6)
            self.sig_combo = QComboBox()
            self.sig_combo.setSizeAdjustPolicy(
                QComboBox.AdjustToMinimumContentsLengthWithIcon)
            self.sig_combo.setMinimumContentsLength(18)
            self.sig_combo.setStyleSheet("QComboBox { combobox-popup: 0; }")
            self.sig_combo.setMaxVisibleItems(18)
            self.sig_combo.addItems(SIGNALS)
            self.sig_combo.currentIndexChanged.connect(self._source_changed)
            self.load_btn = QPushButton("Load WAV…")
            self.load_btn.clicked.connect(self.load_wav)
            self.strat_combo = QComboBox()
            self.strat_combo.addItems(STRATEGIES)
            # Without combobox-popup:0 Qt renders the list as a NATIVE MENU,
            # which does not scroll - with 60 entries the top ones end up
            # off-screen and unreachable.  The stylesheet forces the
            # scrollable list view, and maxVisibleItems then caps its
            # height.
            # Der laengste Strategiename hat 164 Zeichen und zwang das
            # Aufklappfeld auf 1073 px - damit verlangte die obere Zeile
            # 2602 px und das FENSTER 2620, unabhaengig von allem
            # anderen.  Das Feld darf jetzt schrumpfen; die aufgeklappte
            # Liste bleibt unverkuerzt, und der volle Name steht im
            # Tooltip, den _strat_changed nachzieht.
            self.strat_combo.setSizeAdjustPolicy(
                QComboBox.AdjustToMinimumContentsLengthWithIcon)
            self.strat_combo.setMinimumContentsLength(28)
            self.strat_combo.setStyleSheet("QComboBox { combobox-popup: 0; }")
            self.strat_combo.setMaxVisibleItems(18)
            self.strat_combo.view().setVerticalScrollBarPolicy(
                Qt.ScrollBarPolicy.ScrollBarAsNeeded)
            # Vorgabe: der Arbeitspunkt, nicht der Auslieferungszustand -
            # 4k, Prony Ordnung 4, ANKER, kein Trend.  (Die alte Suche
            # nach "PV 4k  [engine default]" traf seit der Umbenennung
            # KEINEN Schluessel mehr und landete still auf Eintrag 0.)
            self.strat_combo.setCurrentIndex(next(
                (i for i, k in enumerate(STRATEGIES)
                 if "order 8, hop 256, Gedaechtnis 38 Hops, ANKER" in k), 0))
            self.gap_spin = QSpinBox()
            self.gap_spin.setRange(48, 4096)
            self.gap_spin.setValue(128)   # eine Engine-Periode
            self.gap_spin.setSuffix(" smp gap")
            self.fade_spin = QDoubleSpinBox()
            self.fade_spin.setRange(0.2, 100.0)
            self.fade_spin.setValue(3.0)
            self.fade_spin.setSuffix(" ms fade")
            self.rate_spin = QSpinBox()
            # No 600/min ceiling like GO's spin: saturation (every single
            # period missed) is 60*sr/gap - 22500/min at 128 smp / 48 kHz -
            # and everything above that is simply saturation.
            self.rate_spin.setRange(0, 30000)
            self.rate_spin.setValue(6000)
            self.rate_spin.setSingleStep(60)   # one arrow click = 1 gap/s
            self.rate_spin.setSuffix(" gaps/min")
            # Wuerfelsaat als Regler: gleiche Dichte, andere LAGE der
            # Luecken.  Ob ein Transient sauber klingt, haengt stark daran,
            # ob zufaellig ein Lueckenklumpen genau auf ihn faellt - mit
            # einer festen Saat hoert man diesen Einfluss nie und haelt die
            # Eigenheiten EINER Wuerfelung fuer Eigenschaften des
            # Verfahrens (Nutzerbeobachtung 10.08.2026).
            self.seed_spin = QSpinBox()
            self.seed_spin.setRange(0, 9999)
            self.seed_spin.setValue(0)
            self.seed_spin.setPrefix("Saat ")
            self.seed_spin.setToolTip(
                "Wuerfelsaat der Lueckenstreuung.  Dieselbe Dichte, andere "
                "Lage: durchzaehlen zeigt, wie stark das Urteil ueber einen "
                "Transienten davon abhaengt, ob ein Lueckenklumpen gerade "
                "auf ihm liegt.  0 = die alte feste Saat, damit frueher "
                "gemessene Laeufe reproduzierbar bleiben.")
            self.rate_spin.setToolTip(
                "0 = one gap at the slider position.  Above 0, gaps are "
                "scattered over a "
                f"{SCATTER_S:.0f} s stretch exactly the way GrandOrgue "
                "rolls them: every period of one gap length is missed with "
                "probability rate/60 * gap duration, so consecutive misses "
                "occur on their own.  Saturation (nothing but shadow) is "
                "60*sr/gap per minute.")
            self.spacing_combo = QComboBox()
            self.spacing_combo.addItems(["random spacing", "regular spacing"])
            self.spacing_combo.setToolTip(
                "How the scattered gaps are placed.  RANDOM is the engine's "
                "own dice (Bernoulli per period).  REGULAR puts them at a "
                "fixed distance - which makes any per-gap artifact add up "
                "coherently at that repetition rate, so a defect that is "
                "inaudible once becomes obvious.  A single artifact at "
                "e.g. 37 Hz repetition is heard as roughness, not as a "
                "sequence of separate events.")
            # Safety cap as a CONTROL, not as three more rows in a list of
            # 76 strategies - it applies to whichever GO strategy is
            # selected, so it can be moved without losing the place.
            self.cap_spin = QDoubleSpinBox()
            self.cap_spin.setRange(0.0, 8.0)
            self.cap_spin.setSingleStep(0.1)
            self.cap_spin.setValue(1.2)
            self.cap_spin.setPrefix("Cap ")
            self.cap_spin.setSpecialValueText("Cap AUS")
            self.cap_spin.setToolTip(
                "The shadow may not peak above this many times the loudest "
                "of the last 2048 history samples; it SCALES, it does not "
                "clip.  0 turns it off.  Engine default 1.2.  Measured on "
                "the D major chord at 600/min it fires on ~14 % of shadow "
                "builds at every order, and switching it off moves nothing "
                "- but that was at a tenth of the density this lab can "
                "reach, so it is a knob and not a settled question.  "
                "Applies to the GO/library strategies only.")
            # Der Nachbau fehlender AR-Stuetzstellen.  Als Haekchen und
            # nicht als Listeneintrag, weil er ueber ALLEN GO-Strategien
            # liegt - und weil das Umschalten waehrend des Hoerens die
            # ganze Frage ist.
            self.backfill_box = QCheckBox("Backfill")
            self.backfill_box.setChecked(True)
            self.backfill_box.setToolTip(
                "Fehlt eine AR-Stuetzstelle im Spektrenring, rechne sie aus "
                "der Historie nach - statt den ganzen AR-Pfad abzuschalten "
                "und mit dem Ordnung-1-Rotator zu rechnen.\n\n"
                "Der Ring wird nur beim Schattenbau gefuellt, und waehrend "
                "einer Lueckenkette wird kein Schatten gebaut - ein Cluster "
                "reisst also ein Loch hinein.  OHNE Backfill lief die "
                "Bibliothek bei 600/min in 11 % (Ordnung 4) bzw. 31 % "
                "(Ordnung 12) aller Schattenbauten als Ordnung 1, bei "
                "6000/min in 71 % bzw. 97 %.\n\n"
                "Gemessen auf dem D-Dur-Akkord, Ordnung 4, 600/min: "
                "Ausreisser ueber 0.20 von 1.6 % auf 0.0 %, mittleres NMR "
                "von -8.55 auf -12.36 dB.  Ordnung 12 gewinnt 8.2 dB und "
                "liegt danach gleichauf mit Ordnung 4.\n\n"
                "Gilt nur fuer die GO/Bibliotheks-Strategien.")
            # Die Frische-Knoepfe.  Gemessen, aber vom Nutzer noch nicht
            # gehoert - und sein Urteil hat heute jedes Mal getroffen.
            self.solve_box = QCheckBox("frisch loesen")
            self.solve_box.setToolTip(
                "Loest die AR-Koeffizienten JEDES Bins in JEDER Periode neu, "
                "statt gestreift alle 16 Perioden (43 ms).\n\n"
                "Der Offline-Prototyp tut das bei jeder Luecke.  Gemessen an "
                "den Toneinsaetzen: 6-12 kHz von -36.74 auf -37.99 dB, damit "
                "0.17 dB hinter dem Prototyp.  Oberhalb 12 kHz bleiben 3 bis "
                "5 dB Abstand, deren Ursache noch nicht gefunden ist.\n\n"
                "Kostet Rechenzeit: das volle Loesen wurde mit 10.8 ms "
                "seriell bei Ordnung 4 und sechs Kanaelen gemessen.")
            self.burg_box = QCheckBox("Burg frisch")
            self.burg_box.setToolTip(
                "Fittet das Burg-Modell der Nahtkorrektur in jeder Periode "
                "neu statt alle 8.  Gemessen wirkungslos (0.04 dB) - drin, "
                "weil das Messergebnis und das Ohr heute mehrfach "
                "auseinandergingen.")
            self.ring_box = QCheckBox("Fade im Ring")
            self.ring_box.setChecked(True)
            self.ring_box.setToolTip(
                "Schreibt die Wiedereinstiegs-Ueberblendung AUCH in die "
                "Historie, nicht nur in den Ausgang.\n\n"
                "Ohne das steht im Ring an jedem Kettenende ein harter "
                "Spleiss Vorhersage -> Wahrheit.  Der Sprung ist "
                "breitbandig und verseucht die naechsten 32 "
                "Analysefenster bis 24 kHz - das waren die vertikalen "
                "Striche.  Der Ausgang aendert sich NICHT.\n\n"
                "Gemessen an den Einsaetzen: 12-18 kHz -9.13 -> -10.61, "
                "18-24 kHz +0.05 -> -1.77 dB.")
            self.capspan_box = QCheckBox("Cap nur Gap+Fade")
            self.capspan_box.setChecked(True)
            self.capspan_box.setToolTip(
                "Der Safety-Cap misst die Schattenspitze nur ueber eine "
                "Luecke plus Wiedereinstieg statt ueber den ganzen "
                "32-ms-Schatten.  Was danach divergiert, wird von einem "
                "frischen Schatten ersetzt, bevor es je gespielt wird - "
                "es mitzumessen laesst den Cap an Transienten ausloesen "
                "und zieht die GESPIELTEN Samples mit herunter.")
            self.tau_spin = QSpinBox()
            self.tau_spin.setRange(0, 512)
            self.tau_spin.setValue(0)
            self.tau_spin.setSuffix(" smp Anker-tau")
            self.tau_spin.setSpecialValueText("Anker-tau auto (16)")
            self.tau_spin.setToolTip(
                "Zeitkonstante der Anker-Exponentiale in Samples; die "
                "Korrektur laeuft ueber 10 tau.  0 = eingebaute 16 "
                "(1/3 ms).\n\nDer Anker zieht (shadow[0] - v0) * "
                "exp(-i/tau) ab - breitbandig.  Kuerzeres tau: gleiche "
                "Sprunghoehe in weniger Zeit, also mehr Hochtonanteil.  "
                "Laengeres tau: leiser verteilt, dafuer laenger "
                "hoerbar.  Wirkt auf die GO-Strategien.")
            self.det_spin = QDoubleSpinBox()
            self.det_spin.setRange(0.0, 1.2)
            self.det_spin.setSingleStep(0.01)
            self.det_spin.setDecimals(2)
            # 0.99: ueber 1.0 schaukelt sich ab halber Lueckendichte ein
            # einzelner Bin auf (Nutzergehoer 10.08., nachgemessen im
            # 1:1-Muster: +72 dB schmalbandig bei 1.02 gegen +58 bei 0.99).
            self.det_spin.setValue(0.99)
            self.det_spin.setPrefix("Detektor rho ")
            self.det_spin.setSpecialValueText("Detektor AUS (klassisch)")
            self.det_spin.setToolTip(
                "rho-Schur-Detektor statt Kappung fuer alle (Entwurf des "
                "Nutzers, 06.08.):\n\nBestehen die rohen Koeffizienten "
                "den Alle-Wurzeln-innerhalb-rho-Test (96 % der Bins bei "
                "1.02), laufen sie UNVERAENDERT - kein Durand-Kerner, "
                "die gefittete Huellkurve jeder Mode bleibt.  Fallen sie "
                "durch, laeuft warmer DK und |z| wird auf rho GEDECKELT "
                "statt auf 1.0 gehoben: Wachstum aufs Budget gestutzt, "
                "Abklingen bleibt, nichts wird verewigt.\n\n"
                "Sinnvoller Start: 1.02 (Wachstum <= x1.7 ueber den "
                "Horizont).  Ersetzt z-Kappe und deaktiviert den "
                "GPU-Solve, bis der Kernel den Modus kennt.")
            self.gpu_box = QCheckBox("GPU")
            self.gpu_box.setChecked(True)
            self.gpu_box.setToolTip(
                "Rechnet den per-Bin-Solve auf dem ersten OpenCL-Geraet "
                "(NVIDIA/AMD/Intel/pocl - was der ICD-Loader findet).  "
                "Der RLS-Fit bleibt auf der CPU; bei Geraetefehlern "
                "faellt die Bibliothek nahtlos auf die CPU zurueck.  "
                "Kernels bit-identisch gegen SolveBin verifiziert.\n\n"
                "Ohne OpenCL-Laufzeit: stiller CPU-Betrieb.")
            self.tap_box = QCheckBox("Solve mitschreiben")
            self.tap_box.setToolTip(
                "Zeichnet auf, was der Kernel je Bin GETAN hat: groesster "
                "Polradius vor dem Deckeln, genommener Zweig und ALLE Pole "
                "(ebenfalls vor dem Deckeln).  Ein Bild je vollstaendigem "
                "Bin-Durchlauf von Kanal 0.\n\n"
                "Zwei Ansichten: 'Solve-Teppich' zeigt den ganzen Lauf auf "
                "einmal, 'Solve z-Ebene' ein Bild je Durchlauf - dort sieht "
                "man die Pole wandern.  Beide wachsen WAEHREND der Rechnung "
                "mit, wenn die Ansicht schon offen ist.\n\n"
                "Braucht GPU - der CPU-Solve hat keinen Tap; Ordnung 1 "
                "bleibt leer, weil der Rotator am AR-Solve vorbeilaeuft.\n\n"
                "KOSTET ZEIT: je Kernelstart ein zweiter blockierender "
                "Rueckweg (gemessen auf pocl: +7 % bei Ordnung 4, +1 % bei "
                "Ordnung 12 - der Solve selbst waechst schneller als der "
                "Transfer).  Das Ergebnis ist bitgleich (geprueft), die "
                "Zeitmessung mit gesetztem Haken aber wertlos - erst recht "
                "mit offener Live-Ansicht, die zusaetzlich zeichnet.")
            self.patch_box = QCheckBox("Transienten-Patch")
            self.patch_box.setToolTip(
                "Nach jedem stabilisierten Toneinsatz wird die Zone um den "
                "Einsatz NUR IN DER ANALYSE-SICHT durch die rueckwaerts "
                "rotierte Fortsetzung des neuen Klangs ersetzt - der "
                "breitbandige Knall verschwindet aus allen kuenftigen "
                "Analysefenstern, der Ring bleibt Wahrheit.\n\n"
                "Phasenexakt an der rechten Kante (Transparenz auf "
                "stationaerem Material -26 bis -39 dB, Schwebung -38.9); "
                "statisch, waechst von selbst zur Wahrheit zurueck.  "
                "Waechter: keine Verdeckung in den letzten 16 Perioden, "
                "0,5-s-Entprellung gegen periodische Modulation (Schwebung wie Tremulant).")
            self.hybrid_box = QCheckBox("Hybrid 1k")
            self.hybrid_box.setChecked(True)
            self.hybrid_box.setToolTip(
                "Zwei-Ketten-Hybrid: eine kurze 1k-Kette laeuft dauerhaft "
                "warm mit.  Feuert der Einsatz-Detektor, bekommen die "
                "betroffenen 94-Hz-Baender in BEIDEN Ketten frische "
                "Normalgleichungen (Gedaechtnis reicht nie hinter den "
                "Anschlag zurueck), die kurze Kette spielt sie mit "
                "kletternder Ordnung, und sobald der frische Fit der "
                "langen Kette die Uebergabe-Ordnung traegt, gibt sie "
                "zurueck (weich, ~3 Perioden).\n\nBasis-Strategie muss "
                "ein GO-Eintrag mit Rahmen > 1k sein; Detektor rho "
                "spielt keine Rolle (der Breiten-Trigger laeuft immer).")
            self.hybcap_spin = QSpinBox()
            self.hybcap_spin.setRange(1, 8)
            self.hybcap_spin.setValue(6)
            self.hybcap_spin.setPrefix("1k-Deckel ")
            self.hybcap_spin.setToolTip(
                "Deckel der 1k-Ordnungsleiter.  Ordnung p rechnet, sobald "
                "p+2 Perioden seit dem Anschlag vergangen sind - eine "
                "Sprosse alle 2,7 ms, Gedaechtnis waechst stetig mit.")
            self.hybhand_spin = QSpinBox()
            self.hybhand_spin.setRange(1, 80)
            self.hybhand_spin.setValue(8)
            self.hybhand_spin.setPrefix("Uebergabe @Ord ")
            self.hybhand_spin.setToolTip(
                "Zurueck zur langen Kette (X+2) Rasterpunkte nach dem "
                "Anschlag.  Bei 2k/gap 128 ist ein Rasterpunkt 5,33 ms:\n"
                "  X=6  -> 42,7 ms (kein 2k-Rahmen sieht mehr Vor-Anschlag)\n"
                "  X=13 -> 80 ms   (Gedaechtnislaenge der kurzen Kette)\n"
                "  X=36 -> 203 ms  (Haltetest: 2k-Fit hat den Anschlag\n"
                "                   weitgehend vergessen)")
            self.hybpre_box = QCheckBox("512er-Vorstufe")
            self.hybpre_box.setChecked(False)
            self.hybpre_box.setToolTip(
                "Zweistufiges Schalten: die 512er-Breite schaltet 6-11 ms "
                "frueher (Augen-Paritaet) und nur VORLAEUFIG.  Der Check "
                "nach 11 ms urteilt PRO BAND: es bleibt nur, wer selbst "
                "persistent gestiegen ist UND wessen eigenes Residuum "
                "gesprungen ist (XO_PRE_RESB 0.10) - Schwebungsbaender "
                "sind persistent, aber vorhersagbar, und fliegen einzeln "
                "raus.\n\nBei 6000/min: Einsatz +0.90 statt +1.13, "
                "Schlimmstfall 7.70 statt 8.36 - beide besser als ohne "
                "Vorstufe; Schwebung zahlt noch 1.4 dB, global -1.94 vs "
                "-2.46.  Ohr entscheidet.  XO_PRE_ON/TTL/VETO/RESB.")
            self.hybsimple_box = QCheckBox("Detektor simpel")
            # Vorgabe AN seit 08.08. abends: Hoerurteil des Nutzers -
            # Transienten besser als pures 1k/Ordnung 5, Einsaetze auch
            # im Spektrogramm sauberer.
            self.hybsimple_box.setChecked(True)
            self.hybsimple_box.setToolTip(
                "Exakt die ERSTE Hybrid-Erkennung, ohne allen spaeteren "
                "Schnickschnack: 2k-Breite mit Kandidat, Schaltlatte und "
                "Spleiss-Veto, Wiedereintritts-Sperre 48 Perioden.  "
                "Keine 512er-Vorstufe (Haken wird ignoriert), kein "
                "Wechselwunsch, kein Per-Band-Urteil, kein "
                "Lautstaerke-Tor, kein Rueckzug.  Zum A/B-Hoeren gegen "
                "den ausgebauten Pfad.")
            self.hybres_box = QCheckBox("Residuen-Waehler")
            self.hybres_box.setChecked(False)
            self.hybres_box.setToolTip(
                "EXPERIMENT, gemessen gescheitert (08.08.): jedes Band "
                "spielt die Kette mit kleinerem Ein-Schritt-"
                "Vorhersagefehler.  Der misst aber NACHFUEHRUNG, nicht "
                "Extrapolation ueber den Verdeckungshorizont - die 1k-"
                "Kette gewinnt ihn auch auf Plateaus (sie folgt der "
                "Schwebung schneller), der Waehler schaltete 3935x fern "
                "der Einsaetze (+6,4 dB NMR statt -2,4).  Bleibt zum "
                "Nachmessen; Vorgabe ist die Detektor-Umschaltung.")
            self.hyball_box = QCheckBox("alle Baender")
            self.hyball_box.setChecked(True)
            self.hyball_box.setToolTip(
                "Beim Einsatz schalten ALLE Baender auf die kurze Kette, "
                "nicht nur die steigenden.  Gemessen 08.08.: die "
                "Teilauswahl reisst Teiltoene zwischen den Ketten "
                "auseinander (200-ms-Haltetest max +16,7 dB NMR mit "
                "Auswahl, +7,05 mit allen).  Abhaken = die selektive "
                "Variante zum Vergleich.")
            self.hybwarm_spin = QSpinBox()
            self.hybwarm_spin.setRange(0, 8)
            self.hybwarm_spin.setValue(0)
            self.hybwarm_spin.setPrefix("1k warm@Ord ")
            self.hybwarm_spin.setToolTip(
                "Sparbetrieb: warm laeuft nur diese Startordnung; ab "
                "der Schaltung akkumuliert ein frischer Satz bei voller "
                "Deckelordnung, die Leiter klettert mit echten Tupeln "
                "(Ordnung p ab p+2).  0 = Vollbetrieb.\n\nGemessen bei "
                "6000/min (Mittel / max / Einsatz | CPU 2 Kanaele):\n"
                "  voll  -2.70 / 7.10 / +4.75 | 326 us  <- Vorgabe\n"
                "  @5    -2.02 / 6.97 / +4.29 | 307 us\n"
                "  @4    -1.94 / 6.93 / +5.21 | 283 us\n"
                "  @3    -1.53 / 7.77 / +5.30 | 260 us\n"
                "  @2    -1.36 / 9.88 / +7.70 | 240 us\n"
                "Im Normalmodus gibt es sonst KEINE Leiter: die kurze "
                "Kette spielt ab dem Wechsel sofort die Deckelordnung "
                "aus ihrem warmen Akkumulator.  Der Sparbetrieb baut "
                "eine ein - und verliert, weil ein frischer Fit 20 ms "
                "nach dem Anschlag nur Ordnung 2-3 traegt, waehrend der "
                "warme halb kontaminiert, aber voll bestimmt ist.  "
                "Knopf fuer Rechenzeit-Not.")
            self.lazy_box = QCheckBox("Lazy")
            self.lazy_box.setToolTip(
                "Volle Schattenberechnung nur im Fenster von 8 Perioden "
                "vor jeder Luecke; dazwischen laeuft nur Analyse und Fit "
                "(absorb).  Der Zustand bleibt exakt der der Engine, nur "
                "die ungehoert verfallenden Schatten entfallen - bei 60 "
                "Luecken/min sind das 99 %.\n\nErzwingt frisches "
                "Loesen am Lueckenfenster (solveEvery=1), weil die "
                "gestreifte Aufloesung waehrend absorb nicht laeuft.\n\n"
                "Fuer Hoertests bei niedrigen Raten; fuer exakte "
                "Engine-Treue abschalten.")
            self.zclamp_spin = QDoubleSpinBox()
            self.zclamp_spin.setRange(0.0, 1.2)
            self.zclamp_spin.setSingleStep(0.01)
            self.zclamp_spin.setDecimals(2)
            # AUS als Vorgabe (Nutzer, 07.08.): die selektive Kappung
            # beisst sich mit dem Detektor-Modus.  1.00 heisst "nur echte
            # Divergenz wird zurueckgeholt" - Transienten-Moden bei 0.93
            # bleiben sterblich.  Im Detektor-Modus fragt ohnehin nur noch
            # Ordnung 1 diese Schwelle ab; ab Ordnung 2 deckelt rho.
            self.zclamp_spin.setValue(1.0)
            self.zclamp_spin.setPrefix("z-Kappe ")
            self.zclamp_spin.setSpecialValueText("z-Kappe auto (0.90)")
            self.zclamp_spin.setToolTip(
                "Ausloeseschwelle der Wurzelkappung.  Wurzeln mit |z| "
                "UEBER der Schwelle werden auf den Einheitskreis "
                "geschnappt (Ziel immer 1.0); darunter behalten sie ihr "
                "gefittetes Abklingen.\n\n0.90 (auto) friert auch "
                "Transienten-Moden bei z.B. |z|=0.93 zu ewigen "
                "Oszillatoren ein - das Klingeln nach Anschlaegen.  "
                "Naeher an 1.0: Transienten duerfen aussterben, "
                "Haltetoene verlieren kaum.  Ueber 1.0: nur noch echte "
                "Divergenz wird zurueckgeholt (Safety-Cap sichert).\n\n"
                "Wirkt auf die GO-Strategien.")
            self.align_box = QCheckBox("phase-align re-entry (warp)")
            self.gen_btn = QPushButton("Generate")
            self.gen_btn.clicked.connect(self.generate)
            self.start_btn = QPushButton("Generate + play")
            self.start_btn.clicked.connect(self.start)
            self.stop_btn = QPushButton("Stop")
            self.stop_btn.clicked.connect(self.stop)
            for x in (QLabel("Signal:"), self.sig_combo, self.load_btn,
                      QLabel("Strategy:"), self.strat_combo, self.gap_spin,
                      self.fade_spin, self.rate_spin, self.seed_spin,
                      self.spacing_combo, self.align_box,
                      self.gen_btn, self.start_btn, self.stop_btn):
                row.addWidget(x)
            # Gedeckelter Rollbereich statt frei wachsender Zeilen.
            # Die beiden Parameterzeilen brauchten umgebrochen zusammen
            # ueber 140 px und liessen dem BILD nur 46 % der
            # Fensterhoehe - bei einem Werkzeug, dessen Ergebnis das
            # Bild IST, ist das die falsche Verteilung.  Gedeckelt statt
            # versteckt: alles bleibt erreichbar, notfalls durch Rollen.
            self.ctl_box = QWidget()
            ctl_lay = QVBoxLayout(self.ctl_box)
            ctl_lay.setContentsMargins(0, 0, 0, 0)
            ctl_lay.setSpacing(2)
            ctl_lay.addWidget(row_w)

            # Eigene Zeile fuer die Verdeckungs-Knoepfe.  In einer Reihe
            # mit dem breiten Strategie-Aufklapper wurden sie aus dem
            # sichtbaren Bereich gedraengt - der Nutzer sah die neuen
            # Haekchen nach einem Neustart schlicht nicht.
            # UMBRECHENDE Zeile statt einer starren: die Schalterreihe
            # bestimmte bisher die Mindestbreite des Fensters (gemessen
            # 2428 px allein fuer die Schalter, 2654 fuer das Fenster -
            # mehr als der Bildschirm), und jeder neue Knopf machte es
            # schlimmer.  Jetzt bricht sie auf die vorhandene Breite um
            # und die Mindestbreite haengt nur noch am breitesten
            # EINZELNEN Schalter.
            row2_w = self.row2_w = QWidget()
            row2 = FlowLayout(row2_w, spacing=6)
            row2.addWidget(QLabel("Verdeckung:"))
            # EINE Liste fuer Layout UND Cache-Schluessel.  Der
            # Uebergabe-Spinner ohne Cache-Eintrag (08.08.) war der
            # vierte Vorfall der Klasse "neuer Knopf, alte
            # Registrierstelle vergessen" - jetzt gibt es nur noch DIESE
            # Stelle: was hier steht, liegt in der Leiste und macht den
            # Lauf ungueltig.
            # Residuen-Tor des Transientendetektors.  Ein Band zaehlt nur
            # als steigend, wenn sein EIGENER Vorhersagefehler darueber
            # liegt; die Breite bildet sich aus den bestaetigten Baendern.
            # Trennt, was der Pegel nicht trennt - echte Anschlagsbaender
            # liegen im selben Pegelbereich wie der Vorhersagefehler.
            self.resgate_spin = QDoubleSpinBox()
            self.resgate_spin.setRange(0.0, 1.0)
            self.resgate_spin.setSingleStep(0.01)
            self.resgate_spin.setDecimals(2)
            self.resgate_spin.setValue(0.0)
            self.resgate_spin.setPrefix("Res-Tor ")
            self.resgate_spin.setSpecialValueText("Res-Tor AUS")
            self.resgate_spin.setToolTip(
                "Transientendetektor, Residuen-Tor.  0 = aus.\n\n"
                "Gemessen am D-Dur-Material bei 6000 Luecken/min: 0.02 "
                "raeumt alle drei Plateau-Fehlzuendungen ab und kostet "
                "zwei von sechs Einsaetzen, verbessert aber jedes Mass "
                "(Mittel -18.08 -> -18.28 dB, schlimmster Fall 15.61 -> "
                "15.05, an den Einsaetzen -13.66 -> -13.81) - eine "
                "Fehlzuendung im Plateau kostet weit mehr als ein "
                "verpasster Anschlag.\n\n"
                "Eine PEGEL-Untergrenze scheitert dagegen: schon 0.001 "
                "der mittleren Bandenergie loescht alle sechs Einsaetze, "
                "weil echte Anschlagsbaender im selben Pegelbereich "
                "liegen wie der Vorhersagefehler.")

            self.conceal_widgets = (
                self.cap_spin, self.backfill_box, self.ring_box,
                self.capspan_box, self.solve_box, self.burg_box,
                self.tau_spin, self.zclamp_spin, self.det_spin,
                self.patch_box, self.hybrid_box, self.hybcap_spin,
                self.hybhand_spin, self.hybwarm_spin, self.hybpre_box,
                self.hybsimple_box, self.hybres_box,
                self.hyball_box,
                self.resgate_spin,
                self.lazy_box, self.gpu_box, self.tap_box)
            for x in self.conceal_widgets:
                row2.addWidget(x)
            ctl_lay.addWidget(row2_w)
            self.ctl_scroll = QScrollArea()
            self.ctl_scroll.setWidget(self.ctl_box)
            self.ctl_scroll.setWidgetResizable(True)
            self.ctl_scroll.setFrameShape(QScrollArea.NoFrame)
            self.ctl_scroll.setHorizontalScrollBarPolicy(
                Qt.ScrollBarAlwaysOff)
            self.ctl_scroll.setMaximumHeight(self.CTL_MAX_H)
            lay.addWidget(self.ctl_scroll)

            self.slider_row_w = QWidget()
            slider_row = QHBoxLayout(self.slider_row_w)
            slider_row.setContentsMargins(0, 0, 0, 0)
            self.pos_text = QLabel("Gap position: -")
            self.slider = QSlider(Qt.Horizontal)
            self.slider.setRange(0, 10000)
            self.slider.setValue(0)   # Einzel-Luecke; Streuung ist eh ganze Datei
            self.slider.valueChanged.connect(self._show_pos)
            slider_row.addWidget(self.pos_text)
            slider_row.addWidget(self.slider, 1)
            lay.addWidget(self.slider_row_w)

            self.numbers = QLabel("-")
            self.numbers.setTextInteractionFlags(Qt.TextSelectableByMouse)
            lay.addWidget(self.numbers)

            # ABX: candidate (concealed) vs. the untouched original -
            # "would anyone notice the deadline miss?"
            self.abx_row_w = QWidget()
            abx_row = QHBoxLayout(self.abx_row_w)
            abx_row.setContentsMargins(0, 0, 0, 0)
            abx_row.addWidget(QLabel("ABX blind test:"))
            self.abx_a_btn = QPushButton("Play A (concealed)")
            self.abx_b_btn = QPushButton("Play B (original)")
            self.abx_x_btn = QPushButton("Play X")
            self.abx_isa_btn = QPushButton("X = A")
            self.abx_isb_btn = QPushButton("X = B")
            self.abx_reset_btn = QPushButton("Reset")
            self.abx_a_btn.clicked.connect(lambda: self._abx_play("A"))
            self.abx_b_btn.clicked.connect(lambda: self._abx_play("B"))
            self.abx_x_btn.clicked.connect(lambda: self._abx_play("X"))
            self.abx_isa_btn.clicked.connect(lambda: self._abx_answer("A"))
            self.abx_isb_btn.clicked.connect(lambda: self._abx_answer("B"))
            self.abx_reset_btn.clicked.connect(self._abx_reset)
            self.abx_label = QLabel("no trials yet")
            for x in (self.abx_a_btn, self.abx_b_btn, self.abx_x_btn,
                      self.abx_isa_btn, self.abx_isb_btn,
                      self.abx_reset_btn, self.abx_label):
                abx_row.addWidget(x)
            abx_row.addStretch(1)
            lay.addWidget(self.abx_row_w)

            pg.setConfigOptions(antialias=True, background="w",
                                foreground="k")
            self.wave = pg.PlotWidget(title="Waveform (left channel)")
            self.wave.setLabel("bottom", "time [s]")
            self.wave_curve = self.wave.plot(pen=pg.mkPen("#1f77b4", width=1))
            self.cursor = pg.InfiniteLine(0, pen=pg.mkPen("#d62728", width=2))
            self.wave.addItem(self.cursor)
            # Die Waveform hatte dasselbe Gewicht wie der GESAMTE
            # Plotbereich darunter - damit blieb den Ansichten nur
            # die halbe Fensterhoehe, und im 3D war das Gebirge
            # entsprechend klein.
            # KEIN fester Deckel mehr: Waveform und Bildbereich haengen
            # jetzt an einem ziehbaren Trenner (weiter unten), damit sich
            # jeder die Aufteilung so zieht, wie er sie braucht.  Nur
            # eine Untergrenze, damit die Waveform nicht unbedienbar
            # duenn wird - ganz zuschieben geht trotzdem, der Trenner
            # laesst sie einklappen.
            self.wave.setMinimumHeight(28)

            bottom = QHBoxLayout()
            left_box = QVBoxLayout()
            # Dritte umbrechende Zeile - nach den beiden oberen war
            # diese der letzte Posten ueber 1300 px.
            view_row_w = QWidget()
            view_row = FlowLayout(view_row_w, spacing=6)
            self.view_combo = QComboBox()
            self.view_combo.addItems(
                ["Spectrogram", "Live FFT at cursor", "Solve-Teppich",
                 "Solve z-Ebene", "Scroll-Spektro",
                 "Phasen-Gebirge 3D"])
            self.view_combo.currentIndexChanged.connect(
                lambda i: self.stack.setCurrentIndex(i))
            view_row.addWidget(self.view_combo)
            self.big_box = QCheckBox("Bild gross")
            self.big_box.setToolTip(
                "Klappt die beiden Parameterzeilen und die Waveform weg, "
                "damit das Bild die Fensterhoehe bekommt.  Aendert nichts "
                "an der Rechnung - die Einstellungen bleiben, sie sind "
                "nur nicht zu sehen.")
            self.big_box.stateChanged.connect(self._big_toggle)
            view_row.addWidget(self.big_box)
            self.allctl_box = QCheckBox("alle Regler")
            self.allctl_box.setToolTip(
                "Zeigt ALLE Ansichts-Regler gleichzeitig, statt nur die "
                "der aktiven Ansicht.\n\n"
                "Die Filterung spart Hoehe fuers Bild, macht aber "
                "Einstellungen unerreichbar, die man von einer anderen "
                "Ansicht aus setzen will - dafuer ist dieser Haken da.")
            self.allctl_box.stateChanged.connect(
                lambda *_: self._tap_ui_visible(
                    self.view_combo.currentIndex()))
            view_row.addWidget(self.allctl_box)
            self._lbl_fft = QLabel("FFT:"); view_row.addWidget(self._lbl_fft)
            self.spec_combo = QComboBox()
            # 128/256: Perioden-Aufloesung fuer die Detektor-Forschung -
            # zeigt, was ein Kriterium auf Engine-Puffer-Ebene sehen
            # wuerde (2,7 ms Frames, 375-Hz-Bins).
            for nfft in (128, 256, 512, 1024, 2048, 4096, 8192):
                self.spec_combo.addItem(
                    f"{nfft}  ({48000 / nfft:.0f} Hz / {nfft / 48:.1f} ms)",
                    nfft)
            self.spec_combo.setCurrentIndex(3)   # 1024 wie bisher
            self.spec_combo.currentIndexChanged.connect(self._spec_changed)
            view_row.addWidget(self.spec_combo)
            self._lbl_floor = QLabel("floor:"); view_row.addWidget(self._lbl_floor)
            self.spec_floor = QSpinBox()
            self.spec_floor.setRange(-160, -30)
            self.spec_floor.setSingleStep(5)
            self.spec_floor.setValue(-120)
            self.spec_floor.setSuffix(" dB")
            self.spec_floor.valueChanged.connect(self._spec_levels_changed)
            view_row.addWidget(self.spec_floor)
            self._lbl_ceil = QLabel("ceil:"); view_row.addWidget(self._lbl_ceil)
            self.spec_ceil = QSpinBox()
            self.spec_ceil.setRange(-90, 0)
            self.spec_ceil.setSingleStep(5)
            self.spec_ceil.setValue(0)
            self.spec_ceil.setSuffix(" dB")
            self.spec_ceil.valueChanged.connect(self._spec_levels_changed)
            view_row.addWidget(self.spec_ceil)
            # Gap/fade markers.  Colour choice is not cosmetic: "inferno"
            # is a purely warm ramp (black-purple-red-orange-yellow) with
            # essentially no green content at ANY level, so spring green
            # and teal are the two hues that cannot be confused with
            # signal energy - orange/red/yellow markers would vanish in
            # loud passages, which is exactly where one looks.
            self.spec_patch_box = QCheckBox("patch marks")
            self.spec_patch_box.setChecked(True)
            self.spec_patch_box.setToolTip(
                "Strichpunktierte Linie = vom Breiten-Detektor erkannter "
                "Einsatz (laeuft IMMER mit, auch ohne Patch-Modus).  "
                "Gruenes Band = die 16 ms, die zusaetzlich in der "
                "ANALYSE-Sicht durch die rueckwaerts rotierte "
                "Fortsetzung ersetzt wurden - nur mit Transienten-Patch "
                "und bestandenen Waechtern.")
            self.spec_marks_box = QCheckBox("gap/fade marks")
            self.spec_marks_box.setChecked(True)
            self.spec_marks_box.stateChanged.connect(self._update_spec_marks)
            self.spec_patch_box.stateChanged.connect(self._update_spec_marks)
            self.spec_hyb_box = QCheckBox("hybrid marks")
            self.spec_hyb_box.setChecked(True)
            self.spec_hyb_box.setToolTip(
                "Blaue Rechtecke: WELCHE Baender WANN von der kurzen "
                "1k-Kette gerechnet wurden (Zeit x Frequenz, aus dem "
                "Episoden-Log der Engine).  Beschriftung im Tooltip des "
                "Rechtecks: Rahmen, Ordnung, Bandbereich.")
            self.spec_hyb_box.stateChanged.connect(self._update_spec_marks)
            # Kausal-Sicht: alles nach "jetzt" wird abgedeckt, und
            # "jetzt" laesst sich in ENGINE-Schritten (1 Periode =
            # Puffergroesse) durchsteppen - der faire Wettkampf Auge
            # gegen Detektor: beide sehen nur die Vergangenheit.
            self.causal_box = QCheckBox("Zukunft ausblenden")
            self.causal_box.setToolTip(
                "Deckt alles rechts von 'jetzt' ab.  Sichtbar sind nur "
                "Analyse-Frames, die zum Zeitpunkt 'jetzt' VOLLSTAENDIG "
                "in der Vergangenheit liegen - exakt das Wissen eines "
                "kausalen Detektors.  Mit den Pfeilen des Zaehlers "
                "steppst du in Engine-Perioden (= Puffergroesse) durchs "
                "Signal.")
            self.causal_box.stateChanged.connect(self._causal_toggled)
            self.causal_spin = QSpinBox()
            self.causal_spin.setRange(0, 10 ** 7)
            self.causal_spin.setPrefix("jetzt @P ")
            self.causal_spin.setToolTip(
                "Aktuelle Periode (1 Schritt = 1 Engine-Puffer).  "
                "Pfeiltasten/Mausrad = 1 Periode vor/zurueck.")
            self.causal_spin.valueChanged.connect(self._update_causal)
            view_row.addWidget(self.spec_marks_box)
            view_row.addWidget(self.spec_patch_box)
            view_row.addWidget(self.spec_hyb_box)
            view_row.addWidget(self.causal_box)
            view_row.addWidget(self.causal_spin)
            self.spec_alpha = QSlider(Qt.Horizontal)
            self.spec_alpha.setRange(0, 100)
            self.spec_alpha.setValue(70)
            self.spec_alpha.setFixedWidth(90)
            self.spec_alpha.setToolTip(
                "marker opacity - fills stay far more transparent than the "
                "boundary lines")
            self.spec_alpha.valueChanged.connect(self._update_spec_marks)
            view_row.addWidget(self.spec_alpha)
            left_box.addWidget(view_row_w)

            self.stack = QStackedWidget()
            self.spec = pg.PlotWidget(title="Spectrogram")
            # Klick ins Spektrogramm setzt 'jetzt' (nur bei aktiver
            # Kausal-Sicht).  MUSS nach der Erzeugung von self.spec
            # stehen - die erste Fassung verdrahtete es in der
            # view_row-Sektion davor, und das Lab startete gar nicht.
            self.spec.scene().sigMouseClicked.connect(self._causal_click)
            self.spec.setLabel("bottom", "time [s]")
            self.spec.setLabel("left", "frequency [Hz]")
            self.spec_img = pg.ImageItem()
            self.spec_img.setColorMap(pg.colormap.get("inferno"))
            self.spec.addItem(self.spec_img)
            self.spec_cursor = pg.InfiniteLine(
                0, pen=pg.mkPen("#00d0ff", width=2))
            self.spec.addItem(self.spec_cursor)
            # Gap = spring green, re-entry fade = teal; the shaded bands
            # make the two intervals readable at a glance, the lines mark
            # the sample-exact boundaries.  Items are rebuilt per run
            # because the scatter mode produces any number of gaps.
            self.spec_mark_items = []
            self.stack.addWidget(self.spec)

            self.live = pg.PlotWidget(title="Live FFT at cursor (4096)")
            self.live.setLogMode(x=True, y=False)
            self.live.setLabel("bottom", "frequency [Hz]")
            self.live.setLabel("left", "level [dB]")
            self.live.setYRange(-140, 0)
            self.live_curve = self.live.plot(
                pen=pg.mkPen("#2ca02c", width=1.5))
            self.stack.addWidget(self.live)

            # ---- Solve-Teppich ---------------------------------------
            # Der Teppich ist die eigentliche Anzeige: eine Spalte je
            # vollstaendigem Bin-Durchlauf, also der ganze Lauf auf
            # einen Blick.  Der Laufstrich mit Abspielknopf ist das
            # 'Zuschauen' - er wandert durch dieselbe Flaeche, statt
            # dass man Bild fuer Bild ein Fenster neu zeichnen laesst.
            self.tap_plot = pg.PlotWidget(title="Solve-Teppich")
            self.tap_plot.setLabel("bottom", "time [s]")
            self.tap_plot.setLabel("left", "frequency [Hz]")
            self.tap_img = pg.ImageItem()
            self.tap_plot.addItem(self.tap_img)
            self.tap_cursor = pg.InfiniteLine(
                0, pen=pg.mkPen("#ffffff", width=2))
            self.tap_plot.addItem(self.tap_cursor)
            self.stack.addWidget(self.tap_plot)

            # ---- z-Ebene ---------------------------------------------
            # Der eigentliche Film: je Solve-Durchlauf EIN Bild, das sich
            # umbaut.  Die zweite Achse kommt aus den Polen selbst - man
            # sieht sie nach aussen wandern und von der Kappe
            # zurueckgeholt werden, statt nur eine Linie ueber ein
            # fertiges Bild laufen zu lassen.
            self.zpl = pg.PlotWidget(title="Solve z-Ebene")
            self.zpl.setLabel("bottom", "Re z")
            self.zpl.setLabel("left", "Im z")
            self.zpl.setAspectLocked(True)
            self.zpl.showGrid(x=True, y=True, alpha=0.15)
            phi = np.linspace(0, 2 * np.pi, 361)
            # Einheitskreis: die Grenze zwischen Abklingen und Wachsen.
            self.zpl.plot(np.cos(phi), np.sin(phi),
                          pen=pg.mkPen("#8899aa", width=1.5))
            # Schwellenkreis - dort greift der Kernel wirklich ein.
            self.z_thr_circle = self.zpl.plot(
                pen=pg.mkPen("#ff6600", width=1.2, style=Qt.DashLine))
            self.z_pts = pg.ScatterPlotItem(
                size=2.5, pen=None, brush=pg.mkBrush(255, 255, 255, 90))
            self.zpl.addItem(self.z_pts)
            self.zpl.setXRange(-1.6, 1.6)
            self.zpl.setYRange(-1.6, 1.6)
            self.stack.addWidget(self.zpl)

            # ---- Scroll-Spektrogramm ---------------------------------
            # Waechst waehrend der Rechnung nach rechts, und VOR der
            # Jetzt-Linie steht, was der Praediktor spielen WUERDE
            # (inst.peek()) - Spekulation, noch nicht gehoert.  Der
            # Schatten wird bei jedem Blick frisch geholt, also aendert
            # er sich vor den Augen, waehrend neue Evidenz eintrifft.
            self.sg = pg.PlotWidget(title="Scroll-Spektrogramm")
            self.sg.setLabel("bottom", "time [s]")
            self.sg.setLabel("left", "frequency [Hz]")
            self.sg_img = pg.ImageItem()
            # Drei klar getrennte FARBFAMILIEN, nicht drei huebsche
            # Rampen: inferno lief von violett ueber rot nach gelb und
            # kollidierte damit mit dem Rot des Schattens - man sah die
            # Grenze nicht.  Jetzt Bernstein / Magenta / Tuerkis, jede
            # Familie fuer sich von schwarz nach hell.
            self.sg_img.setColorMap(pg.ColorMap(
                [0.0, 0.5, 1.0],
                [(0, 0, 0), (170, 95, 10), (255, 230, 170)]))
            self.sg.addItem(self.sg_img)
            # Eigenes Bild fuer die Spekulation, eigene Farbkarte: was
            # noch nicht gespielt wurde, darf nicht aussehen wie das,
            # was schon gespielt wurde.
            self.sg_spec_img = pg.ImageItem()
            self.sg_spec_img.setColorMap(
                pg.ColorMap([0.0, 0.5, 1.0],
                            [(0, 0, 0), (160, 20, 140), (255, 180, 245)]))
            self.sg.addItem(self.sg_spec_img)
            # Drittes Bild: die WIRKLICHE Zukunft aus der Datei, rechts
            # vom Schatten.  Kuehl gefaerbt, weil sie noch nicht durch
            # die Engine gelaufen ist - sie darf nicht aussehen wie
            # Gerechnetes.
            self.sg_future_img = pg.ImageItem()
            self.sg_future_img.setColorMap(
                pg.ColorMap([0.0, 0.5, 1.0],
                            [(0, 0, 0), (0, 110, 120), (160, 250, 255)]))
            self.sg.addItem(self.sg_future_img)
            self.sg_now = pg.InfiniteLine(
                0, pen=pg.mkPen("#ffffff", width=2))
            self.sg.addItem(self.sg_now)
            # ZWEITE Grenze: ohne sie war nicht zu sehen, wo der Schatten
            # aufhoert und die echte Zukunft anfaengt.
            self.sg_specend = pg.InfiniteLine(
                0, pen=pg.mkPen("#ff70e0", width=1, style=Qt.DashLine))
            self.sg.addItem(self.sg_specend)
            # Und Namen dran - Farbe allein muss man sich merken, ein
            # Wort nicht.
            self.sg_tags = []
            for txt, colr in (("aufbereitet", "#ffcc66"),
                              ("SCHATTEN", "#ff8ae8"),
                              ("Zukunft", "#66e6f0")):
                ti = pg.TextItem(txt, color=colr, anchor=(0.5, 0))
                self.sg.addItem(ti)
                self.sg_tags.append(ti)
            self.stack.addWidget(self.sg)
            self.sg_src = QComboBox()
            self.sg_src.addItems(self.SG_SOURCES)
            self.sg_src.setToolTip(
                "Was RECHTS vom Schatten steht - die wirkliche Zukunft "
                "aus der Datei.\n\n"
                "mit Luecken: so kommt sie bei der Engine an, "
                "ausgefallene Perioden sind Loecher.  Der Massstab, an "
                "dem der Schatten links daneben zu messen ist.\n"
                "Original: dieselbe Zukunft ungestoert.\n\n"
                "Links vom Schatten steht immer das aufbereitete Signal, "
                "also was tatsaechlich gespielt wurde."
                "\n\nWirkt beim naechsten Blick.")
            view_row.addWidget(self.sg_src)
            # Fenstergroesse: Vorgabe ist die der Engine.  Ein Umschalter
            # muss trotzdem sein, weil beide Wuensche sich widersprechen -
            # gemessen liegt eine 128-Sample-Luecke in einem
            # 4096er-Fenster nur 1,2 dB tief (unsichtbar), in einem
            # 128er-Fenster 207 dB (schwarze Spalte).  Wer die LOECHER
            # sehen will, braucht ein Fenster in Lueckengroesse; wer
            # sehen will, was die ENGINE sieht, braucht ihres.
            self.sg_nfft = QComboBox()
            self.sg_nfft.addItem("Fenster: Engine", 0)
            for n in (128, 256, 512, 1024, 2048, 4096):
                self.sg_nfft.addItem(f"Fenster: {n}", n)
            self.sg_nfft.setToolTip(
                "Fensterlaenge des Scroll-Spektrogramms.\n\n"
                "'Engine' nimmt genau das, was die Instanz rechnet "
                "(2k/4k/8k je Modus, Sprung = eine Audio-Periode) - dann "
                "zeigt das Bild die Aufloesung, in der der Algorithmus "
                "wirklich denkt.\n\n"
                "Kleiner als die Luecke gewaehlt, werden die AUSFAELLE "
                "sichtbar: gemessen liegt eine 128er-Luecke bei Fenster "
                "4096 nur 1,2 dB tief, bei Fenster 128 dagegen 207 dB.")
            self.sg_nfft.currentIndexChanged.connect(self._sg_scale_changed)
            view_row.addWidget(self.sg_nfft)
            # Bis 8 kHz war fest verdrahtet - bei 48 kHz Abtastrate
            # blieb die obere Haelfte des Bildes ungenutzt und die
            # Zischlaute unsichtbar.  Vorgabe jetzt VOLL.
            self.sg_top = QComboBox()
            self.sg_top.addItem("bis Nyquist", 0)
            for khz in (4, 8, 12, 16):
                self.sg_top.addItem(f"bis {khz} kHz", khz * 1000)
            self.sg_top.setToolTip(
                "Obere Frequenzgrenze der Anzeige.  'bis Nyquist' zeigt "
                "den ganzen Bereich (24 kHz bei 48 kHz Abtastrate); "
                "hineinzoomen geht mit dem Mausrad ohnehin.")
            self.sg_top.currentIndexChanged.connect(self._g3_redraw)
            view_row.addWidget(self.sg_top)
            self.sg_future = QSpinBox()
            self.sg_future.setRange(0, 5000)
            self.sg_future.setValue(300)
            self.sg_future.setSingleStep(50)
            self.sg_future.setSuffix(" ms voraus")
            self.sg_future.setToolTip(
                "Wie weit rechts vom Schatten die wirkliche Zukunft "
                "gezeigt wird.\n\n"
                "Der SCHATTEN selbst laesst sich nicht verlaengern: die "
                "Bibliothek rechnet 32 ms voraus (m_HorizonPeriods, im "
                "Konstruktor festgelegt), und das ist genau die Menge "
                "Verdeckung, die sie bereithaelt.  Was hier weiter nach "
                "rechts wandert, ist deshalb kein laengerer Schatten, "
                "sondern das, was wirklich kommt - im Labor kennen wir "
                "die Datei ja.\n\n0 blendet die Zukunft aus.")
            view_row.addWidget(self.sg_future)
            self.sg_back = QSpinBox()
            self.sg_back.setRange(50, 5000)
            self.sg_back.setValue(250)
            self.sg_back.setSingleStep(50)
            self.sg_back.setSuffix(" ms zurueck")
            self.sg_back.setToolTip(
                "Wie weit die Anzeige zurueckblickt.  Zusammen mit dem "
                "Ausblick bestimmt das die ZEITACHSE: je kleiner beide, "
                "desto breiter faechert sich jede Spalte auf.\n\n"
                "War bis 12.08. fest auf 600 ms verdrahtet und damit fuer "
                "die kurzen Vorgaenge um eine Luecke herum zu gestaucht.")
            view_row.addWidget(self.sg_back)

            # ---- Phasen-Gebirge (3D) ---------------------------------
            # Hoehe = Pegel, Farbe = Phase.  Erst der Aufbau, dann die
            # Farbwahl - beides kann fehlschlagen, ohne dass das Lab
            # daran stirbt.
            self.g3 = None
            self.g3_surf = None
            self.g3_fallback = QLabel(
                "3D nicht verfuegbar - der GL-Kontext hat sich beim "
                "Zeichnen verweigert.\nDie Meldung steht auf der Konsole; "
                "alle anderen Ansichten laufen unberuehrt weiter.")
            self.g3_fallback.setAlignment(Qt.AlignCenter)
            self.g3_fallback.setVisible(False)
            if HAVE_GL:
                try:
                    self.g3 = SafeGLView()
                    self.g3.setBackgroundColor(pg.mkColor(8, 8, 14))
                    self.g3.setCameraPosition(distance=430, elevation=24,
                                              azimuth=-38)
                    # KEIN Beleuchtungs-Shader: 'shaded' multipliziert die
                    # Farbe mit dem Lichteinfall und macht damit genau die
                    # Information kaputt, derentwegen gefaerbt wird.  Die
                    # Form liest man hier aus der Silhouette, nicht aus
                    # der Helligkeit.
                    self.g3_surf = glw.GLSurfacePlotItem(
                        shader=None, smooth=False, computeNormals=False,
                        glOptions="translucent")   # fuer 'Boden aus'

                    self.g3.addItem(self.g3_surf)
                    # Zonengrenzen als aufrechte Rahmen quer durchs
                    # Gitter, plus Namen daneben.  Ohne die war im 3D
                    # ueberhaupt nicht zu sehen, wo das Gespielte
                    # aufhoert und die Spekulation anfaengt.
                    self.g3_marks = []
                    for colr in ("#ffffff", "#ff70e0"):
                        ln = glw.GLLinePlotItem(
                            color=pg.glColor(colr), width=2,
                            mode="line_strip", antialias=True)
                        self.g3.addItem(ln)
                        self.g3_marks.append(ln)
                except Exception:
                    traceback.print_exc()
                    self.g3 = self.g3_surf = None
            # Seite 5 traegt BEIDES: das GL-Fenster und den Ersatztext.
            # Welches sichtbar ist, entscheidet sich erst beim Zeichnen -
            # vorher weiss niemand, ob der Kontext taugt.
            g3_page = QWidget()
            g3_lay = QVBoxLayout(g3_page)
            g3_lay.setContentsMargins(0, 0, 0, 0)
            if self.g3 is not None:
                # Der ganze uebrige Platz gehoert der Zeichenflaeche.
                self.g3.setSizePolicy(QSizePolicy.Policy.Expanding,
                                      QSizePolicy.Policy.Expanding)
                g3_lay.addWidget(self.g3, 1)
            else:
                self.g3_fallback.setText(
                    "3D nicht verfuegbar - pyqtgraph.opengl braucht "
                    "PyOpenGL:\n    pip install PyOpenGL\n"
                    "Die 2D-Ansichten laufen davon unberuehrt weiter.")
                self.g3_fallback.setVisible(True)
            g3_lay.addWidget(self.g3_fallback)
            self.g3_legend = QLabel("Phasen-Gebirge: noch kein Lauf")
            self.g3_legend.setTextInteractionFlags(Qt.TextSelectableByMouse)
            # FEST in der Hoehe.  Ohne das teilt sich die eine Textzeile
            # den Platz gleichberechtigt mit dem GL-Fenster - gemessen
            # 237 px fuer ein Etikett, das 18 braucht, und die
            # Zeichenflaeche schrumpfte auf die Haelfte.
            self.g3_legend.setSizePolicy(QSizePolicy.Policy.Preferred,
                                         QSizePolicy.Policy.Fixed)
            g3_lay.addWidget(self.g3_legend, 0)
            self.stack.addWidget(g3_page)
            self.g3_color = QComboBox()
            self.g3_color.addItems(
                ["Phase: Bin-Takt", "Phase: Fehler vs. Original",
                 "Amplitude (dB)", "Sandstein (nur Form)"])
            self.g3_color.setToolTip(
                "Faerbung des Gebirges.  Hoehe ist immer der Pegel.\n\n"
                "Bin-Takt: Abweichung der Phasendrehung von der, die ein "
                "Ton genau auf der Bin-Mitte haette.  Jeder Partialton "
                "bekommt dadurch seine eigene, ueber die Zeit KONSTANTE "
                "Farbe - sie codiert seine Verstimmung.  Kippt die Farbe "
                "eines Gipfels, wandert seine Frequenz.\n\n"
                "Fehler vs. Original: Phasendifferenz zur ungestoerten "
                "Wahrheit.  Cyan = null.  Das ist die Diagnose - hier "
                "faerbt sich, was die Verdeckung falsch macht, oft bevor "
                "man es im Pegelbild sieht.\n\n"
                "Rohe Phase gibt es absichtlich nicht: sie dreht je "
                "Sprung um omega*H weiter und zeigt nur die Bin-Nummer.")
            self.g3_color.currentIndexChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_color)
            self.g3_depth = QSpinBox()
            self.g3_depth.setRange(50, 3000)
            self.g3_depth.setValue(400)
            self.g3_depth.setSingleStep(50)
            self.g3_depth.setPrefix("Tiefe ")
            self.g3_depth.setSuffix(" ms")
            self.g3_depth.setToolTip(
                "Wie viel ZEIT das Gebirge in die Tiefe zeigt.\n\n"
                "Hiess bis 12.08. 'Rahmen' und zaehlte Analyserahmen - "
                "eine Zahl, die niemand im Kopf in Zeit umrechnet, und "
                "die bei Sprung 128 nur 213 ms ergab: viel zu fein, um "
                "einen Verlauf zu erkennen, dafuer voller Zappeln.\n\n"
                "Die Rahmenzahl bleibt jetzt bei rund 100 und der "
                "SCHRITT waechst mit der Tiefe - laenger heisst groeber, "
                "nicht unuebersichtlicher.")
            self.g3_depth.valueChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_depth)
            self.g3_range = QSpinBox()
            self.g3_range.setRange(20, 120)
            self.g3_range.setValue(60)
            self.g3_range.setSingleStep(10)
            self.g3_range.setPrefix("Dynamik ")
            self.g3_range.setSuffix(" dB")
            self.g3_range.setToolTip(
                "Hoehenmassstab: so viele dB unter dem lautesten Punkt "
                "des GEZEIGTEN Ausschnitts liegt der Boden.\n\n"
                "Vorher war der Boden fest auf -90 dBFS genagelt.  "
                "Gemessen lagen damit 89,6 % der Flaeche unter 5 % Hoehe "
                "und der hoechste Gipfel erreichte 70 % - das Gebirge "
                "sah aus, als fiele kein Licht darauf.  Jetzt spannt "
                "sich der Massstab ueber das, was wirklich da ist.")
            self.g3_range.valueChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_range)
            self.g3_mask = QCheckBox("Boden neutral")
            self.g3_mask.setChecked(True)
            self.g3_mask.setToolTip(
                "Gitterpunkte UNTER dem Hoehenmassstab grau faerben.\n\n"
                "Dort liegt kein Signal, also auch keine sinnvolle Phase - "
                "ohne diesen Schnitt wuerde der flache Boden zum "
                "Konfettifeld aus Zufallsfarben und uebertoente die "
                "Gipfel.  Es ist eine Ja/Nein-Entscheidung, keine "
                "Helligkeit nach Amplitude: die traegt allein die Hoehe.")
            self.g3_mask.stateChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_mask)
            self.g3_light = QSpinBox()
            self.g3_light.setRange(0, 100)
            self.g3_light.setValue(65)
            self.g3_light.setSingleStep(5)
            self.g3_light.setPrefix("Licht ")
            self.g3_light.setSuffix(" %")
            self.g3_light.setToolTip(
                "Streiflicht auf die Flaeche, damit die Hoehenstruktur "
                "plastisch wird.\n\n"
                "Die Helligkeit kommt aus der NEIGUNG (Normalenvektor), "
                "nicht aus dem Pegel - ein hoher flacher Ruecken und der "
                "flache Boden werden gleich hell, nur die Form "
                "unterscheidet sie.  Der Farbton bleibt die Phase.\n\n"
                "Das Licht steht fest im Datenraum, nicht an der Kamera: "
                "beim Drehen bleibt die Beleuchtung, sonst wanderte der "
                "Formeindruck mit dem Blickwinkel.\n\n"
                "0 % = flach wie vorher.")
            self.g3_light.valueChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_light)
            self.g3_fscale = QComboBox()
            self.g3_fscale.addItems(["f linear", "f logarithmisch"])
            self.g3_fscale.setToolTip(
                "Frequenzachse.\n\n"
                "Logarithmisch heisst: gleiche Strecke je OKTAVE.  Die "
                "Bins werden dann auch logarithmisch ausgewaehlt, sonst "
                "haette der Bass drei Stuetzstellen und die Hoehen "
                "zweihundert.  Fuer Orgelklang meist die richtige Wahl - "
                "die Partialtoene stehen dann in gleichmaessigen "
                "Abstaenden statt sich unten zu draengen.\n\n"
                "Linear zeigt die Bins, wie die FFT sie liefert - "
                "richtig, wenn es um Bin-Nachbarschaft geht (Leckage, "
                "Detektor, Rotator).")
            self.g3_fscale.currentIndexChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_fscale)
            self.g3_ascale = QComboBox()
            self.g3_ascale.addItems(["A in dB", "A linear"])
            self.g3_ascale.setToolTip(
                "Hoehenachse.\n\n"
                "dB (Vorgabe) ist bereits logarithmisch - der Regler "
                "'Dynamik' spannt den Massstab.\n\n"
                "Linear zeigt den Betrag selbst, auf das Maximum "
                "normiert.  Dann verschwindet fast alles im Boden und nur "
                "die staerksten Partialtoene ragen heraus - ehrlich, aber "
                "fuer das Auge meist nutzlos; 'Dynamik' wirkt dabei "
                "nicht.")
            self.g3_ascale.currentIndexChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_ascale)
            self.g3_len = QSpinBox()
            self.g3_len.setRange(40, 4000)
            self.g3_len.setValue(260)
            self.g3_len.setSingleStep(40)
            self.g3_len.setPrefix("Laenge ")
            self.g3_len.setToolTip(
                "Wie lang das Gebirge in der Tiefe GEZEICHNET wird, in "
                "denselben Einheiten wie die Frequenzachse (die ist 256 "
                "breit).\n\n"
                "Das ist reine Geometrie, unabhaengig davon, wie viel "
                "ZEIT die Tiefe umfasst: gross gewaehlt wird daraus ein "
                "langer Teppich, den man entlangschaut.")
            self.g3_len.valueChanged.connect(self._g3_fit)
            self.g3_len.valueChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_len)
            self.g3_hpd = QDoubleSpinBox()
            self.g3_hpd.setRange(0.05, 20.0)
            self.g3_hpd.setValue(0.7)
            self.g3_hpd.setSingleStep(0.1)
            self.g3_hpd.setDecimals(2)
            self.g3_hpd.setPrefix("Hoehe ")
            self.g3_hpd.setSuffix(" /dB")
            self.g3_hpd.setToolTip(
                "Ueberhoehung: Welteinheiten je dB.  Bei 0.7 und 60 dB "
                "Dynamik ist das Gebirge 42 Einheiten hoch, die "
                "Frequenzachse 256 breit.\n\n"
                "Bei 'A linear' zaehlt der Wert je 1/60 des Maximums - "
                "die Hoehe bleibt so beim Umschalten vergleichbar.")
            self.g3_hpd.valueChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_hpd)
            self.g3_floor = QCheckBox("Boden zeigen")
            self.g3_floor.setChecked(True)
            self.g3_floor.setToolTip(
                "Die flache Ebene unter dem Massstab zeichnen.\n\n"
                "Aus: die Gitterpunkte dort werden durchsichtig, es "
                "bleiben nur die Gebirgszuege stehen - gut, um an den "
                "Flanken entlangzusehen.  An: die Ebene traegt das Bild "
                "und macht die Zonenwaende lesbar.\n\n"
                "Nicht zu verwechseln mit 'Boden neutral', das nur die "
                "FARBE dort abschaltet.")
            self.g3_floor.stateChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_floor)
            self.g3_draw = QComboBox()
            self.g3_draw.addItems(["massiv", "Gitter auf massiv",
                                   "nur Gitter"])
            self.g3_draw.setToolTip(
                "Darstellungsart der Flaeche.\n\n"
                "massiv: geschlossene Oberflaeche.\n"
                "Gitter auf massiv: zusaetzlich die Maschenlinien - man "
                "sieht, wo die Stuetzstellen liegen, und die Woelbung "
                "wird besser lesbar.\n"
                "nur Gitter: die Flaechen werden durchsichtig, es bleibt "
                "das Drahtmodell.  Gut, um durch das Gebirge hindurch auf "
                "dahinterliegende Zuege zu sehen.")
            self.g3_draw.currentIndexChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_draw)
            self.g3_tex = QSpinBox()
            self.g3_tex.setRange(0, 100)
            self.g3_tex.setValue(0)
            self.g3_tex.setSingleStep(10)
            self.g3_tex.setPrefix("Koernung ")
            self.g3_tex.setSuffix(" %")
            self.g3_tex.setToolTip(
                "Sandstein-Koernung auf die Oberflaeche legen: feines "
                "Korn plus waagerechte Schichtung, wie bei geschichtetem "
                "Gestein.\n\n"
                "Rein dekorativ - sie traegt KEINE Messgroesse und ist "
                "deshalb auch nicht nach Daten, sondern fest verwuerfelt "
                "(gleiches Gitter = gleiches Korn, es flimmert nicht "
                "zwischen den Bildern).  Sie modelliert nur die "
                "Helligkeit, der Farbton bleibt die Phase.\n\n"
                "0 % = glatt.")
            self.g3_tex.valueChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_tex)
            self.g3_bins = QSpinBox()
            self.g3_bins.setRange(0, 4096)
            self.g3_bins.setValue(0)
            self.g3_bins.setSingleStep(64)
            self.g3_bins.setPrefix("Bins ")
            self.g3_bins.setSpecialValueText("Bins: alle")
            self.g3_bins.setToolTip(
                "Hoechstzahl der Stuetzstellen ueber die Frequenz; "
                "'alle' nimmt jeden vorhandenen Bin.\n\n"
                "War fest auf 256 gedeckelt - deshalb aenderte ein "
                "groesseres Analysefenster die Aufloesung des Gebirges "
                "NICHT: 2k wie 8k ergaben dieselben 256 Stuetzstellen.  "
                "Mit 'alle' bestimmt allein das Fenster die "
                "Frequenzaufloesung, so wie man es erwartet.\n\n"
                "Mehr Stuetzstellen heisst mehr Dreiecke; ab einigen "
                "hundert wird das Drehen zaeh, und im Drahtmodell wird "
                "es ohnehin wieder zu Nebel (dort gilt zusaetzlich die "
                "eigene, groebere Masche).")
            self.g3_bins.valueChanged.connect(self._g3_redraw)
            view_row.addWidget(self.g3_bins)

            # BEDIENELEMENTE NACH ANSICHT.  Alle gleichzeitig sichtbar
            # brauchten in der umbrechenden Zeile drei bis vier Zeilen
            # Hoehe - genau die Hoehe, die dann dem Bild fehlte.  Jede
            # Ansicht zeigt nur noch ihre eigenen.
            self._view_ctrls = {
                "spec": (self._lbl_fft, self.spec_combo, self._lbl_floor,
                         self.spec_floor, self._lbl_ceil, self.spec_ceil,
                         self.spec_marks_box, self.spec_patch_box,
                         self.spec_hyb_box, self.causal_box,
                         self.causal_spin, self.spec_alpha),
                "sg": (self.sg_src, self.sg_future, self.sg_back),
                # Die Fenstergroesse bestimmt im Gebirge die Zahl der
                # verfuegbaren Bins - sie gehoert dort genauso hin.
                "nfft": (self.sg_nfft,),
                "g3": (self.g3_color, self.g3_depth, self.g3_range,
                       self.g3_mask, self.g3_light, self.g3_fscale,
                       self.g3_ascale, self.g3_len, self.g3_hpd,
                       self.g3_floor, self.g3_draw, self.g3_tex,
                       self.g3_bins),
                # Die Frequenzgrenze teilen sich Scroll und Gebirge.
                "top": (self.sg_top,),
            }
            self._g3_src = None
            self._sg_reset()

            # Steuerzeile UNTER dem Stapel, von Teppich und z-Ebene
            # gemeinsam benutzt: ein Bildindex, nicht zwei, die
            # auseinanderlaufen koennen.
            tap_row = QHBoxLayout()
            self.tap_mode = QComboBox()
            self.tap_mode.addItems(["Polradius |z|", "Zweig"])
            self.tap_mode.setToolTip(
                "Faerbung des Teppichs.\n\n"
                "Polradius: |z| des groessten Pols VOR dem Deckeln, mit "
                "hartem Farbbruch an der z-Kappe - kalt = klingt ab, "
                "heiss = wollte wachsen und wurde gedeckelt.\n\n"
                "Zweig: welchen Weg der Kernel je Bin nahm.  Zeigt vor "
                "allem, welche Bins der Detektor durchgewunken hat und "
                "welche durch Durand-Kerner mussten.")
            self.tap_mode.currentIndexChanged.connect(self._draw_tap)
            tap_row.addWidget(self.tap_mode)
            self.tap_slider = QSlider(Qt.Horizontal)
            self.tap_slider.setRange(0, 0)
            self.tap_slider.valueChanged.connect(self._tap_frame_changed)
            tap_row.addWidget(self.tap_slider, 1)
            self.tap_play_btn = QPushButton("▶")
            self.tap_play_btn.setFixedWidth(36)
            self.tap_play_btn.setToolTip(
                "Abspielen: Laufstrich durch den Teppich bzw. Bild fuer "
                "Bild durch die z-Ebene (25 Bilder/s)")
            self.tap_play_btn.clicked.connect(self._tap_play_toggle)
            tap_row.addWidget(self.tap_play_btn)
            self.tap_row_w = QWidget()
            self.tap_row_w.setLayout(tap_row)
            self.tap_label = QLabel("Solve-Mitschrift: noch kein Lauf")
            self.tap_label.setTextInteractionFlags(Qt.TextSelectableByMouse)

            self.tap_timer = QTimer(self)
            self.tap_timer.setInterval(40)
            self.tap_timer.timeout.connect(self._tap_tick)
            self.tap_data = None    # (z, path, pos, thr, roots)
            self._z_brush_lut = None
            self._tap_seen = 0      # Bilder, die der Live-Blick schon sah
            self._tap_last_draw = 0.0

            # Dasselbe eine Ebene hoeher: der Stapel bekommt den Platz,
            # Steuerzeile und Etikett nur ihre eigene Hoehe.
            self.stack.setSizePolicy(QSizePolicy.Policy.Expanding,
                                     QSizePolicy.Policy.Expanding)
            left_box.addWidget(self.stack, 1)
            self.tap_row_w.setSizePolicy(QSizePolicy.Policy.Preferred,
                                         QSizePolicy.Policy.Fixed)
            left_box.addWidget(self.tap_row_w, 0)
            self.tap_label.setSizePolicy(QSizePolicy.Policy.Preferred,
                                         QSizePolicy.Policy.Fixed)
            left_box.addWidget(self.tap_label, 0)
            self.view_combo.currentIndexChanged.connect(self._tap_ui_visible)
            self._tap_ui_visible(0)
            bottom.addLayout(left_box, 1)

            self.compare = pg.PlotWidget(
                title="Spectrum: before the gap (grey) vs. at re-entry (red)")
            self.compare.setLogMode(x=True, y=False)
            self.compare.setLabel("bottom", "frequency [Hz]")
            self.compare.setYRange(-140, 0)
            self.quiet_curve = self.compare.plot(
                pen=pg.mkPen("#999999", width=1.5))
            self.bang_curve = self.compare.plot(
                pen=pg.mkPen("#d62728", width=1.5))
            # Kausal-Sicht: Momentanspektrum des NEUESTEN vollstaendigen
            # Fensters (endet bei 'jetzt'), scrollt beim Durchsteppen mit.
            self.causal_curve = self.compare.plot(
                pen=pg.mkPen("#4faaff", width=2.0))
            bottom.addWidget(self.compare)
            bottom_w = QWidget()
            bottom_w.setLayout(bottom)
            bottom_w.setMinimumHeight(120)
            # Ziehbarer Trenner zwischen Wellenform und Bildbereich.
            self.vsplit = QSplitter(Qt.Vertical)
            self.vsplit.setHandleWidth(6)
            self.vsplit.addWidget(self.wave)
            self.vsplit.addWidget(bottom_w)
            # Beim Vergroessern des Fensters soll das BILD wachsen, nicht
            # die Wellenform; und die Wellenform darf ganz zugeschoben
            # werden, der Bildbereich nicht.
            self.vsplit.setStretchFactor(0, 0)
            self.vsplit.setStretchFactor(1, 1)
            self.vsplit.setCollapsible(0, True)
            self.vsplit.setCollapsible(1, False)
            self.vsplit.setSizes([70, 600])
            lay.addWidget(self.vsplit, 1)

            # ALL loaded files, keyed by their combo label - the combo
            # lists every one of them, so a single "last loaded" slot
            # made the older entries silently play the newest file.
            self.files = {}
            self.x = np.zeros((48000, 1))
            self.sr = 48000
            self.marks = []
            self.patch_marks = []
            self.player = None
            self.t0 = None
            self.clock = QTimer(self)
            self.clock.setInterval(33)
            self.clock.timeout.connect(self.tick)

            self.abx_a = None
            self.abx_b = None
            self.abx_sr = 48000
            self.abx_x_is = None
            self.abx_n = 0
            self.abx_k = 0
            self.abx_player = None
            self._show_pos()

        # --------------------------------------------------------------
        def load_wav(self):
            path, _ = QFileDialog.getOpenFileName(
                self, "Load WAV file", str(Path.home()), "WAV (*.wav)")
            if not path:
                return
            try:
                samples, sr = read_wav(path)
            except (OSError, ValueError, wavemod.Error) as e:
                self.numbers.setText(f"loading failed: {e}")
                return
            label = f"File: {Path(path).name}"
            self.files[label] = (samples, sr, Path(path).name)
            i = self.sig_combo.findText(label)
            if i < 0:
                self.sig_combo.addItem(label)
                i = self.sig_combo.count() - 1
            self.sig_combo.setCurrentIndex(i)
            self._show_pos()

        def _source(self):
            """(samples[n, ch], sr, name) of the current source."""
            name = self.sig_combo.currentText()
            if name in self.files:
                return self.files[name]
            gen = SIGNALS.get(name, SIGNALS["Organ chord (synth)"])
            return gen(48000)[:, None], 48000, name

        def _source_changed(self, *_):
            self._show_pos()

        def _show_pos(self, *_):
            samples, sr, _ = self._source()
            pos = int(self.slider.value() / 10000 * len(samples))
            self.pos_text.setText(
                f"Gap position: {pos / sr:7.2f} s of {len(samples)/sr:.2f} s"
                f"  ({self.gap_spin.value() / sr * 1000:.1f} ms gap)")

        # --------------------------------------------------------------
        def _build(self):
            """Concealed excerpt + original excerpt around the gap.

            Metric runs on the FULL signals (masking needs real
            lead-in); the excerpt is presentation only - identical
            fades on both so ABX differences live in the gap alone."""
            # Der Live-Blick der Mitschrift ruft processEvents, damit das
            # Bild waehrend der Rechnung waechst - damit kann aber auch
            # ein KLICK durchkommen und einen zweiten Lauf in den ersten
            # hinein starten.  Ein Riegel, kein Warteschlangen-Kunstwerk:
            # waehrend gerechnet wird, wird nicht neu gestartet.
            #
            # Das ERGEBNIS muss durchgereicht werden - _build_cached legt
            # es ab und der Zeichner lebt davon.  Die erste Fassung des
            # Riegels verschluckte es, und damit verschwand die ganze
            # Anzeige (Waveform, Zahlen, Spektrogramm): ein None unter
            # gueltigem Schluessel im Zwischenspeicher.  Ein abgewiesener
            # Bau gibt darum None zurueck und wird DORT als "nichts
            # gebaut" behandelt, nicht als Ergebnis.
            if getattr(self, "_building", False):
                return None
            self._building = True
            try:
                return self._build_inner()
            finally:
                self._building = False

        def _build_inner(self):
            samples, sr, _ = self._source()
            fn = STRATEGIES[self.strat_combo.currentText()]
            gap_n = self.gap_spin.value()
            # In the engine one gap IS one audio period, and the period
            # size sets the analysis hop - so keep the GO strategies in
            # step with the gap spin instead of pinning them to 128.
            globals()["GO_BUFFER"] = gap_n
            fade_n = max(4, int(self.fade_spin.value() / 1000 * sr))
            pos = int(self.slider.value() / 10000 * len(samples))
            pos = min(max(pos, 2048), max(2048, len(samples) - gap_n
                                          - fade_n - 2048))
            span = gap_n + fade_n
            rate = self.rate_spin.value()

            sd = int(self.seed_spin.value())
            positions, capped = scatter_positions(
                samples, sr, gap_n, fade_n, pos, rate,
                regular=self.spacing_combo.currentIndex() == 1,
                seed=0xC0FFEE if sd == 0 else 0xC0FFEE + sd)

            # Chained on the accumulating array: a gap that follows within
            # the predictor's history window sees the CONCEALED output as
            # its history, exactly like GO's ring self-feed.
            eng = getattr(fn, "engine", None)
            spl = getattr(fn, "engine_split", None)
            per_gap = []
            # The NMR metric costs ~14 ms PER GAP - at 6000 gaps that is
            # 80 s of "hard xrun computes for half a minute" while the
            # audio itself is done in under a second.  Evaluate an even
            # SAMPLE of the gaps (audio is always built for all of them);
            # 150 gaps give a stable mean, and worst-of-sample is still a
            # worst indicator.
            mpos = metric_positions(positions)
            if spl is not None:
                cand, info = split_engine_run(samples, positions, gap_n, sr,
                                              *spl)
                for p_i in mpos:
                    per_gap.append(concealment_metrics(
                        cand[:, 0], samples[:, 0], p_i, span, sr))
            elif eng is not None:
                # GO strategies: one instance over the whole signal, so
                # miss streaks stay one prediction and the clean-seed
                # gate sees the truth.  See engine_run().
                # Der Cap-Regler ueberschreibt das Feld der Strategie:
                # ein Knopf, der ueber allen GO-Eintraegen liegt, statt
                # je drei Listeneintraege pro Ordnung.
                tun = dict(eng[1])
                tun["safetyCap"] = float(self.cap_spin.value())
                tun["backfill"] = int(self.backfill_box.isChecked())
                tun["solveEvery"] = 1 if self.solve_box.isChecked() else 0
                tun["burgEvery"] = 1 if self.burg_box.isChecked() else 0
                tun["noFadeInRing"] = 0 if self.ring_box.isChecked() else 1
                # Der Cap-Bereich in Samples: eine Luecke plus Fade.
                tun["capSpan"] = (gap_n + fade_n
                                  if self.capspan_box.isChecked() else 0)
                # Die Fade-Einstellung des Labs erreicht die Bibliothek
                # jetzt wirklich - bis 06.08.2026 wirkte sie NUR auf die
                # numpy-Strategien, waehrend die GO-Eintraege stur ihre
                # eingebauten 128 Samples benutzten.
                tun["reentryFade"] = fade_n
                tun["anchorTau"] = int(self.tau_spin.value())
                tun["rootClamp"] = float(self.zclamp_spin.value())
                tun["gpu"] = 1 if self.gpu_box.isChecked() else 0
                tun["solveTap"] = 1 if self.tap_box.isChecked() else 0
                tun["detectRho"] = float(self.det_spin.value())
                tun["detResGate"] = float(self.resgate_spin.value())
                tun["patchTransients"] = (1 if self.patch_box.isChecked()
                                          else 0)
                tun["hybrid"] = 1 if self.hybrid_box.isChecked() else 0
                tun["hybShortCap"] = int(self.hybcap_spin.value())
                tun["hybHandOrder"] = int(self.hybhand_spin.value())
                tun["hybAllBands"] = (1 if self.hyball_box.isChecked()
                                      else 0)
                tun["hybChooser"] = (1 if self.hybres_box.isChecked()
                                     else 0)
                tun["hybPre"] = 1 if self.hybpre_box.isChecked() else 0
                tun["hybSimple"] = (1 if self.hybsimple_box.isChecked()
                                    else 0)
                tun["hybWarmOrder"] = int(self.hybwarm_spin.value())
                # Live-Blick nur, wenn auch mitgeschrieben wird -
                # sonst klopft der Lauf 1000-mal an ein leeres Fenster.
                self._tap_seen = 0
                self._tap_last_draw = 0.0
                self._sg_reset()
                cand, info = engine_run(samples, positions, gap_n, sr,
                                        eng[0], tun,
                                        lazy=8 if self.lazy_box.isChecked()
                                        else 0,
                                        progress=self._live_tick)
                for p_i in mpos:
                    per_gap.append(concealment_metrics(
                        cand[:, 0], samples[:, 0], p_i, span, sr))
            elif fn == "xrun":
                # Marks des VORIGEN Engine-Laufs loeschen - sonst zeigt
                # das Spektrogramm bei xrun/numpy-Strategien fremde
                # Episoden (Altlast, durch das schnelle xrun sichtbar).
                engine_run.patches = []
                engine_run.hyb_marks = []
                engine_run.hyb = (0, 0, 0)
                engine_run.gpu = (0, 0)
                engine_run.tap = (None, None, None, None)
                # EINE Kopie, dann alle Luecken nullen.  Der allgemeine
                # Pfad rief conceal() pro Luecke auf, und das kopiert
                # JEDES MAL das ganze Signal: bei 1768 Luecken x 3,4 MB
                # = 6 GB Speicherverkehr fuer eine Handvoll Nullen.
                cand = samples.copy()
                for p_i in positions:
                    cand[p_i:p_i + gap_n, :] = 0.0
                info = [(0.0, 0.0)] * samples.shape[1]
                for p_i in mpos:
                    per_gap.append(concealment_metrics(
                        cand[:, 0], samples[:, 0], p_i, span, sr))
            else:
                engine_run.patches = []
                engine_run.hyb_marks = []
                engine_run.hyb = (0, 0, 0)
                engine_run.gpu = (0, 0)
                engine_run.tap = (None, None, None, None)
                cand = samples
                for p_i in positions:
                    cand, info = conceal(cand, p_i, gap_n, fade_n, fn,
                                         self.align_box.isChecked(), sr)
                # MEASURE AFTER the loop, exactly as the engine path does.
                # Measuring inside it (as this did until 06.08.2026) reads
                # the metric window - gap_n + fade_n = 272 samples, more
                # than TWICE a 128-sample gap - at a moment when the NEXT
                # gap in a run is still untouched original.  The error is
                # then zero over that stretch, and the per-gap strategies
                # score better the more runs there are.  It flattered them
                # by 3.9 dB at 600/min against the same library the "GO:"
                # entries measured honestly.
                for p_i in mpos:
                    per_gap.append(concealment_metrics(
                        cand[:, 0], samples[:, 0], p_i, span, sr))
            nmr_exc = float(np.mean([m[0] for m in per_gap]))
            nmr_mis = float(np.mean([m[1] for m in per_gap]))
            absolute = float(np.max([m[2] for m in per_gap]))
            worst = float(np.max([m[0] for m in per_gap]))
            a = max(0, positions[0] - int(LEAD_S * sr))
            b = min(len(samples),
                    positions[-1] + span + int(TAIL_S * sr))
            exc = cand[a:b].copy()
            ref = samples[a:b].copy()
            m = int(0.010 * sr)
            for arr in (exc, ref):
                arr[:m] *= fade_curve(m)[::-1, None]
                e = min(int(0.100 * sr), len(arr))
                arr[-e:] *= fade_curve(e)[:, None]
            marks = [(p_i - a, p_i - a + gap_n, p_i - a + span)
                     for p_i in positions]
            # Einsatz-Ereignisse relativ zum Ausschnitt.  Laenge 0 =
            # erkannt, aber nicht gepatcht (Linie ohne Band) - der
            # Detektor laeuft immer, das Patchen nur mit Modus+Waechtern.
            seen = {}
            for o, z, n in getattr(engine_run, "patches", []):
                if a <= o <= b and (n > 0 or o not in seen):
                    seen[o] = (z - a, z - a + n, o - a) if n > 0 \
                        else (None, None, o - a)
            self.patch_marks = list(seen.values())
            # Hybrid-Episoden: zusammenhaengende Baender mit gleichem
            # Zeitfenster zu EINEM Rechteck verschmelzen (bei 'alle
            # Baender' sonst 256 Items pro Einsatz).
            eps = {}
            for t0, t1, band, order, nfft, retr in getattr(
                    engine_run, "hyb_marks", []):
                if t1 <= a or t0 >= b:
                    continue
                eps.setdefault((t0, t1, order, nfft, retr), []).append(band)
            self.hyb_marks = []
            for (t0, t1, order, nfft, retr), bands in eps.items():
                bands.sort()
                lo = prev = bands[0]
                for bb in bands[1:] + [None]:
                    if bb is not None and bb == prev + 1:
                        prev = bb
                        continue
                    self.hyb_marks.append(
                        (t0 - a, t1 - a, lo, prev, order, nfft, retr))
                    if bb is not None:
                        lo = prev = bb
            return (exc, ref, sr, marks, nmr_exc, nmr_mis, absolute,
                    info, gap_n, fade_n, worst, capped)

        def _build_key(self):
            """Everything _build() reads.  Display-only settings (FFT
            size, dB levels, marker opacity) are deliberately absent -
            they have their own handlers and must not force a rebuild."""
            return (self.sig_combo.currentText(),
                    self.strat_combo.currentText(),
                    self.gap_spin.value(), self.fade_spin.value(),
                    self.rate_spin.value(), self.seed_spin.value(),
                    self.spacing_combo.currentIndex(),
                    self.align_box.isChecked(), self.slider.value()) + tuple(
                        w.isChecked() if hasattr(w, "isChecked")
                        else w.value()
                        for w in self.conceal_widgets)

        def _build_cached(self):
            """_build() is deterministic (the scatter RNG is seeded), so
            an unchanged parameter set can reuse the previous result -
            scatter runs are seconds of work."""
            key = self._build_key()
            if getattr(self, "_cache_key", None) != key:
                built = self._build()
                if built is None:
                    # Bau abgewiesen (Wiedereintritt) - NICHTS merken,
                    # sonst haelt der Zwischenspeicher unter gueltigem
                    # Schluessel ein Nichts fest und die Anzeige bleibt
                    # dauerhaft leer, bis jemand einen Parameter dreht.
                    self._cache_key = None
                    return getattr(self, "_cache", None)
                self._cache = built
                self._cache_key = key
            return self._cache

        def generate(self):
            """Rechnen, Anzeigen und WAV schreiben - ohne abzuspielen
            (fuer reine Spektrum-/Metrik-Arbeit)."""
            self.stop()
            if (self._build_key() == getattr(self, "_rendered_key", None)
                    and WAV_OUT.exists()):
                return          # nothing changed - plots and WAV still valid
            (exc, ref, sr, marks, nmr_exc, nmr_mis, absolute, info,
             gap_n, fade_n, worst, capped) = self._build_cached()
            self.x, self.sr, self.marks = exc, sr, marks

            fn = STRATEGIES[self.strat_combo.currentText()]
            lag_txt = ("-" if fn == "xrun" else "  ".join(
                f"ch{c}: k={k:+.2f} ncc={n:.2f}"
                for c, (k, n) in enumerate(info)))
            if len(marks) > 1:
                runs = len(self._merged_marks())
                pm = getattr(self, "patch_marks", [])
                nev = len(pm)
                npat = sum(1 for z0, _, _ in pm if z0 is not None)
                scope = ((f"{nev} Einsaetze ({npat} gepatcht), " if nev
                          else "")
                         + f"{len(marks)} gaps"
                         f"{' (CAPPED)' if capped else ''} in {runs} runs"
                         f" over {(marks[-1][2] - marks[0][0]) / sr:.1f} s"
                         f", mean / worst excess {nmr_exc:+.1f} /"
                         f" {worst:+.1f} dB, missing {nmr_mis:+.1f} dB")
            else:
                scope = (f"excess {nmr_exc:+.1f} dB,"
                         f" missing {nmr_mis:+.1f} dB")
            # WIRKUNG der GPU-Einstellung, nicht ihr Zustand: replica
            # und Schur bleiben CPU, ein gescheitertes Geraet faellt
            # dauerhaft zurueck.  "an, 0 Streifen" ist der Fall, der
            # diese Anzeige noetig gemacht hat.
            dev, cpu = getattr(engine_run, "gpu", (0, 0))
            if not self.gpu_box.isChecked():
                gpu_txt = "aus"
            elif dev and not cpu:
                gpu_txt = f"{dev} Streifen"
            elif dev:
                gpu_txt = f"{dev} Streifen, {cpu} auf CPU"
            else:
                gpu_txt = "AN, aber 0 Streifen (ungenutzt!)"
            # WIRKUNG des Hybrid-Hakens.  Er faellt still aus, wenn der
            # Basisrahmen 1k IST oder der Puffer 256 nicht teilt - und
            # der Puffer haengt an der LUECKEN-BREITE (gap 512 = aus).
            hact, hinert, hband = getattr(engine_run, "hyb", (0, 0, 0))
            if not self.hybrid_box.isChecked():
                hyb_txt = "aus"
            elif hinert and not hact:
                hyb_txt = ("AN, aber WIRKUNGSLOS (Basisrahmen 1k? "
                           "gap teilt 256 nicht?)")
            elif hact:
                hyb_txt = (f"{hband} Band-Perioden kurz"
                           if hband else "aktiv, 0 Baender umgeschaltet")
            else:
                hyb_txt = "AN, aber Strategie ohne Bibliothek"
            # Ebenso die WIRKUNG der Mitschrift: der Haken allein beweist
            # nichts, es gibt drei stille Wege ins Leere (CPU-Solve,
            # Ordnung 1, Strategie ohne Bibliothek).
            #
            # Eingepackt, weil die Waveform WEITER UNTEN gezeichnet wird:
            # eine Ausnahme in der Zusatzanzeige haette sonst die ganze
            # Analyse mitgerissen.  Zusatzanzeigen duerfen ausfallen,
            # Messung und Waveform nicht.
            try:
                tap_txt = self._tap_capture()
            except Exception as e:
                traceback.print_exc()
                self.tap_data = None
                tap_txt = f"FEHLER in der Anzeige: {e}"
            self.numbers.setText(
                f"NMR vs. original (>0 dB = audible): {scope}"
                f"   |   absolute excess {absolute:.1f} dBFS"
                f"   |   gap {gap_n} smp = {gap_n/sr*1000:.1f} ms,"
                f" fade {fade_n/sr*1000:.1f} ms"
                f"   |   junction lag {lag_txt}"
                f"   |   GPU {gpu_txt}"
                f"   |   Hybrid {hyb_txt}"
                f"   |   Film {tap_txt}"
                f"   |   align={'ON' if self.align_box.isChecked() else 'off'}")

            t = np.arange(len(exc)) / sr
            step = max(1, len(exc) // 400000)
            self.wave_curve.setData(t[::step], exc[::step, 0])
            for m in getattr(self, "_mark_lines", []):
                self.wave.removeItem(m)
            self._mark_lines = []
            # Many scattered gaps: only the entry/re-entry boundaries, or
            # the trace disappears behind its own annotation.
            merged = self._merged_marks()
            keep = 3 if len(merged) <= MARK_DETAIL_MAX else 2
            for triple in merged:
                for p, colour in zip(triple[:keep],
                                     ("#ff7f0e", "#2ca02c", "#9467bd")):
                    l = pg.InfiniteLine(
                        p / sr, pen=pg.mkPen(colour, style=Qt.DashLine))
                    self.wave.addItem(l)
                    self._mark_lines.append(l)

            self.spectrum(self.quiet_curve, exc[:, 0],
                          marks[0][0] - int(0.5 * sr))
            self.spectrum(self.bang_curve, exc[:, 0], marks[0][1])
            self.spectrogram(exc[:, 0], sr)
            self._update_spec_marks()

            WAV_OUT.parent.mkdir(parents=True, exist_ok=True)
            self._write_wav(WAV_OUT, exc, sr)
            self._rendered_key = self._build_key()

        def start(self):
            self.generate()
            self.player = subprocess.Popen(["paplay", str(WAV_OUT)])
            self.t0 = time.monotonic()
            self.clock.start()

        @staticmethod
        def _write_wav(path, x, sr):
            s = (np.clip(x, -1, 1) * 32767).astype("<i2")
            if s.shape[1] == 1:
                s = np.repeat(s, 2, axis=1)
            w = wavemod.open(str(path), "wb")
            w.setnchannels(s.shape[1])
            w.setsampwidth(2)
            w.setframerate(sr)
            w.writeframes(s.tobytes())
            w.close()

        def _spec_changed(self, *_):
            if len(self.x) > 1:
                self.spectrogram(self.x[:, 0], self.sr)

        def _spec_levels(self):
            lo = self.spec_floor.value()
            hi = max(self.spec_ceil.value(), lo + 5)
            return lo, hi

        def _spec_levels_changed(self, *_):
            self.spec_img.setLevels(self._spec_levels())

        GAP_RGB = (0, 255, 106)    # spring green - absent from "inferno"
        FADE_RGB = (0, 255, 200)   # teal, likewise

        def _merged_marks(self):
            """Runs of consecutive misses drawn as ONE band.

            At saturation every period is missed, which is a single
            continuously concealed stretch - not 2000 separate gaps.
            Merging is both honest and what keeps the plot drawable."""
            out = []
            for t0, t1, t2 in self.marks:
                if out and t0 <= out[-1][2]:
                    out[-1] = (out[-1][0], t1, t2)
                else:
                    out.append((t0, t1, t2))
            return out

        def _update_spec_marks(self, *_):
            """Place the gap/fade markers and apply the opacity slider."""
            for it in self.spec_mark_items:
                self.spec.removeItem(it)
            self.spec_mark_items = []
            want_gaps = self.spec_marks_box.isChecked() and self.marks
            want_patch = (self.spec_patch_box.isChecked()
                          and getattr(self, "patch_marks", []))
            want_hyb = (self.spec_hyb_box.isChecked()
                        and getattr(self, "hyb_marks", []))
            if not want_gaps and not want_patch and not want_hyb:
                return

            a = self.spec_alpha.value() / 100.0
            line_a = int(255 * a)
            fill_a = int(90 * a)     # fills stay subtle so the spectrum
            edge_a = int(120 * a)    # keeps reading through them

            merged = self._merged_marks() if want_gaps else []
            detail = len(merged) <= MARK_DETAIL_MAX

            for t0, t1, t2 in ((m[0] / self.sr, m[1] / self.sr,
                                m[2] / self.sr) for m in merged):
                for (lo, hi), rgb in (((t0, t1), self.GAP_RGB),
                                      ((t1, t2), self.FADE_RGB)):
                    rgn = pg.LinearRegionItem(values=(lo, hi), movable=False,
                                              brush=pg.mkBrush((*rgb, fill_a)))
                    if hasattr(rgn, "setHoverBrush"):
                        rgn.setHoverBrush(pg.mkBrush((*rgb, fill_a)))
                    for ln in rgn.lines:
                        ln.setPen(pg.mkPen((*rgb, edge_a), width=1))
                    self.spec_mark_items.append(rgn)
                if not detail:
                    continue
                for t, rgb, style in ((t0, self.GAP_RGB, Qt.SolidLine),
                                      (t1, self.FADE_RGB, Qt.SolidLine),
                                      (t2, self.FADE_RGB, Qt.DashLine)):
                    self.spec_mark_items.append(pg.InfiniteLine(
                        t, pen=pg.mkPen((*rgb, line_a), width=2,
                                        style=style)))
            # Hybrid-Episoden: Zeit-x-Frequenz-Rechtecke in Blau (kommt
            # in "inferno" nicht vor).  y-Achse des Spektrogramms ist Hz,
            # ein Band ist (sr/2)/256 breit.
            if want_hyb:
                df = self.sr / 2.0 / 256.0
                for t0, t1, b0, b1, order, nfft, retr in self.hyb_marks:
                    # Orange = ZURUECKGEZOGEN: die 512er-Vorstufe hat
                    # geschaltet, der 2k-Detektor nie bestaetigt.
                    rgb = (255, 150, 60) if retr else (80, 170, 255)
                    r = pg.QtWidgets.QGraphicsRectItem(
                        t0 / self.sr, b0 * df,
                        (t1 - t0) / self.sr, (b1 - b0 + 1) * df)
                    r.setPen(pg.mkPen((*rgb, edge_a), width=1))
                    r.setBrush(pg.mkBrush((*rgb, fill_a)))
                    r.setToolTip(
                        f"kurze Kette {nfft//1024}k Ordnung {order}"
                        + ("  -  ZURUECKGEZOGEN (Vorstufe unbestaetigt)"
                           if retr else "") + "\n"
                        f"{t0/self.sr:.2f}-{t1/self.sr:.2f} s, "
                        f"Baender {b0}-{b1} "
                        f"({b0*df/1000:.1f}-{(b1+1)*df/1000:.1f} kHz)")
                    self.spec_mark_items.append(r)

            # Transienten-Patches: eigenes Grün, damit sie sich von
            # Lücken (rot) und Fades klar unterscheiden.
            for z0, z1, on in (getattr(self, "patch_marks", [])
                               if want_patch else []):
                if z0 is not None:   # Band nur fuer wirklich Gepatchte
                    rgn = pg.LinearRegionItem(
                        values=(z0 / self.sr, z1 / self.sr), movable=False,
                        brush=pg.mkBrush((60, 220, 120, fill_a)))
                    if hasattr(rgn, "setHoverBrush"):
                        rgn.setHoverBrush(pg.mkBrush((60, 220, 120, fill_a)))
                    for ln in rgn.lines:
                        ln.setPen(pg.mkPen((60, 220, 120, edge_a), width=1))
                    self.spec_mark_items.append(rgn)
                self.spec_mark_items.append(pg.InfiniteLine(
                    on / self.sr,
                    pen=pg.mkPen((60, 220, 120, line_a), width=2,
                                 style=Qt.DashDotLine)))
            for it in self.spec_mark_items:
                it.setZValue(20)
                self.spec.addItem(it)

        def _causal_toggled(self, *_):
            # Beim Einschalten an der Slider-Position starten - dort
            # wird gerade untersucht.
            if self.causal_box.isChecked() and self.x is not None:
                buf = max(1, self.gap_spin.value())
                pos = int(self.slider.value() / 10000 * len(self.x))
                self.causal_spin.blockSignals(True)
                self.causal_spin.setValue(pos // buf)
                self.causal_spin.blockSignals(False)
            self._update_causal()

        def _causal_click(self, ev):
            if not self.causal_box.isChecked() \
                    or getattr(self, "x", None) is None:
                return

            vb = self.spec.getViewBox()

            if not vb.sceneBoundingRect().contains(ev.scenePos()):
                return

            t_click = vb.mapSceneToView(ev.scenePos()).x()
            buf = max(1, self.gap_spin.value())

            self.causal_spin.setValue(
                max(0, int(round(t_click * self.sr / buf))))

        def _update_causal(self, *_):
            for it in getattr(self, "_causal_items", []):
                self.spec.removeItem(it)
            self._causal_items = []
            if not self.causal_box.isChecked() or getattr(self, "x", None) \
                    is None:
                self.causal_curve.setData([], [])
                return

            sr = self.sr
            buf = max(1, self.gap_spin.value())
            n = len(self.x)

            self.causal_spin.setMaximum(n // buf)

            T = min(self.causal_spin.value() * buf, n)
            nfft = int(self.spec_combo.currentData())
            # Seit der Fenster-ENDE-Konvention traegt jede Spalte ihre
            # Daten bis zu ihrer x-Position: bekannt bei T ist alles
            # links von T - die Kante liegt exakt auf 'jetzt'.
            x0 = max(0.0, T / sr)
            r = pg.QtWidgets.QGraphicsRectItem(x0, 0, n / sr - x0, sr / 2)

            r.setPen(pg.mkPen(None))
            r.setBrush(pg.mkBrush(10, 10, 14, 255))
            r.setZValue(50)
            self.spec.addItem(r)
            self._causal_items.append(r)
            self.causal_spin.setSuffix(f"  = {T / sr:7.3f} s")

            # Momentanspektrum (rechtes Bild) folgt: das neueste Fenster,
            # das bei 'jetzt' vollstaendig bekannt ist - Breite wie die
            # Spektrogramm-FFT, Ende exakt bei T (kein Blick voraus).
            if T >= nfft:
                self.spectrum(self.causal_curve, self.x[:, 0],
                              T - nfft // 2, width=nfft)

        # ---- Solve-Teppich -------------------------------------------
        # Zweigfarben, gleiche Schluessel wie XO_TAP_* im Kernel.
        TAP_COLORS = {-1: (40, 40, 40), 0: (255, 0, 255),
                      1: (60, 135, 125), 2: (120, 120, 255),
                      3: (60, 170, 230), 4: (240, 170, 40),
                      5: (150, 150, 150), 6: (255, 255, 255)}
        # z-Ebene: Punktdeckel fuers Abspielen, Farbstufen fuer den
        # Pinsel-Zwischenspeicher von pyqtgraph.
        Z_MAX_POINTS = 20000
        Z_COLORS = 32

        def _z_brushes(self):
            """Pinsel je Farbstufe - EINMAL, nicht je Bild."""
            if getattr(self, "_z_brush_lut", None) is not None:
                return
            u = np.arange(self.Z_COLORS) / max(self.Z_COLORS - 1, 1)
            col = self._tap_ramp(
                u, [(0.0, (68, 1, 84)), (0.35, (59, 82, 139)),
                    (0.65, (33, 145, 140)), (0.85, (94, 201, 98)),
                    (1.0, (253, 231, 37))])
            lut = np.empty(self.Z_COLORS, dtype=object)
            for k, c in enumerate(col):
                lut[k] = pg.mkBrush(int(c[0]), int(c[1]), int(c[2]), 110)
            self._z_brush_lut = lut

        def _tap_thresh(self):
            """Die Schwelle, an der der Kernel wirklich eingreift - im
            Detektor-Modus rho, sonst die z-Kappe (0 = eingebaute 0.9).
            Der Farbbruch muss auf DIESER Zahl sitzen, sonst zeigt das
            Bild eine Grenze, die die Bibliothek gar nicht kennt."""
            det = float(self.det_spin.value())
            if det > 0.0:
                return det
            z = float(self.zclamp_spin.value())
            return z if z > 0.0 else 0.9

        # ---- Scroll-Spektrogramm -------------------------------------
        # Fenster 512 bei Sprung = eine Audio-Periode: so entspricht
        # EINE Spalte genau EINEM Engine-Schritt, und die Spekulation
        # setzt luecklos an der Jetzt-Linie an.
        # KEINE erfundene Fenstergroesse mehr: die Anzeige nimmt die
        # Geometrie, die die Instanz wirklich rechnet
        # (Xorcist_analysisGeometry).  Ein 512er-Fenster zeigte eine
        # Aufloesung, die es in der Engine gar nicht gibt - bei 4k sind
        # es 4096 Punkte, also 11,7 Hz statt 94.
        SG_FALLBACK_NFFT = 1024   # nur bis die erste Geometrie da ist
        # Rueckblick bewusst KURZ: der Horizont ist nur 1664 Samples
        # (35 ms), und bei zwei Sekunden Rueckblick waere die
        # Spekulation ein 1,7-%-Splitter am Rand.
        SG_WINDOW_S = 0.6          # nur noch Startwert des Reglers
        SG_FLOOR_DB = -110.0
        # Was links der Jetzt-Linie steht.  Vorgabe ist das ANKOMMENDE
        # Signal mit seinen Loechern: dann erzaehlt das Bild die
        # wirkliche Geschichte - hier fehlt etwas, und rechts steht, was
        # der Praediktor dagegen zu setzen haette.  Der Ausgang zeigt
        # stattdessen das Ergebnis (Loecher gefuellt), das Original die
        # ungestoerte Wahrheit als Vergleich.
        SG_SOURCES = ("mit Luecken", "Original (ungestoert)")
        # Deckel fuer die Zukunftsspalten: bei 4k und langem Ausblick
        # waeren es sonst Tausende FFTs je Blick.  Wird er erreicht,
        # waechst der Sprung - der Ausblick bleibt so lang wie bestellt,
        # nur groeber.
        SG_FUTURE_MAX_COLS = 400

        def _sg_top_hz(self):
            """Obere Grenze der Anzeige; 0 = voller Bereich."""
            v = int(self.sg_top.currentData() or 0)
            return min(self.sr / 2, v) if v else self.sr / 2

        def _sg_reset(self):
            self._sg_cols = []       # fertige Spalten (dB)
            self._sg_done = 0        # bis hierhin transformiert [Samples]
            self._sg_nfft = self.SG_FALLBACK_NFFT
            self._sg_engine_nfft = 0
            self._sg_engine_hop = 0
            self._sg_hop = 128
            self._sg_arhop = 0
            self._sg_win = None

        def _sg_geometry(self, inst):
            """Rahmen und Sprung von der Instanz holen, nicht raten."""
            try:
                nfft, anahop, arhop = inst.analysis_geometry()
            except Exception:
                return
            if not nfft or not anahop:
                return
            self._sg_engine_nfft = nfft
            self._sg_engine_hop = anahop
            self._sg_arhop = arhop
            self._sg_apply_scale()

        def _sg_apply_scale(self, *_):
            """Fenster-Ueberschreiber anwenden - OHNE neuen Lauf.

            Muss getrennt vom Abfragen der Instanz sein: die gibt es nur
            waehrend eines Laufs, das Aufklappfeld dagegen dreht der
            Nutzer jederzeit.  Solange das hier nur im Lauf passierte,
            wirkte die Fenstergroesse im Gebirge scheinbar gar nicht -
            generate() ueberspringt den Neuaufbau ja, wenn sich kein
            Rechen-Parameter geaendert hat, und Anzeige-Einstellungen
            stehen absichtlich nicht im Cache-Schluessel."""
            nfft = self._sg_engine_nfft or self.SG_FALLBACK_NFFT
            anahop = getattr(self, "_sg_engine_hop", 0) or 128
            ovr = int(self.sg_nfft.currentData() or 0)
            if ovr:
                # Eigene Laenge: Sprung mitziehen, damit kurze Fenster
                # nicht zu duenn abgetastet werden - sonst faellt eine
                # Luecke zwischen zwei Spalten und bleibt trotz
                # passender Laenge unsichtbar.
                nfft = ovr
                anahop = max(16, min(anahop, nfft // 4))
            if nfft != self._sg_nfft or anahop != self._sg_hop:
                # Geometrie gewechselt: alte Spalten haben eine andere
                # Breite und liessen sich nicht anstueckeln.
                self._sg_cols = []
                self._sg_done = 0
                self._sg_nfft, self._sg_hop = nfft, anahop
                self._sg_win = np.hanning(nfft).astype(np.float32)

        def _sg_scale_changed(self, *_):
            """Aufklappfeld gedreht: Geometrie neu anwenden und das
            Gebirge aus den GESPEICHERTEN Signalen neu rechnen."""
            self._sg_apply_scale()
            self._g3_redraw()

        def _sg_columns(self, sig, first, last, hop):
            """dB-Spalten fuer Fenster, die bei first..last ENDEN.

            Fenster-Ende-Konvention wie im grossen Spektrogramm: eine
            Spalte gehoert an das Ende ihres Fensters, nicht an den
            Anfang - sonst erscheint ein Anschlag vor seiner Zeit."""
            n = self._sg_nfft
            if self._sg_win is None or len(self._sg_win) != n:
                self._sg_win = np.hanning(n).astype(np.float32)
            win = self._sg_win
            out = []
            end = first
            while end <= last:
                a = end - n
                seg = (sig[a:end] if a >= 0
                       else np.concatenate((np.zeros(-a, np.float32),
                                            sig[0:end])))
                S = np.abs(np.fft.rfft(seg * win))
                out.append(20 * np.log10(S / (n / 4) + 1e-12))
                end += hop
            return out, end - hop

        # ---- Phasen-Gebirge ------------------------------------------
        G3_MAX_BINS = 256        # Gitterbreite; darueber wird ausgeduennt
        G3_ROWS = 110            # Gittertiefe; die Zeit wird ausgeduennt
        G3_WIRE_X, G3_WIRE_Y = 90, 60   # Maschen in den Gittermodi

        @staticmethod
        def _hsv_rgba(h, s, v):
            """HSV -> RGBA als (N,4) float32, ohne Zusatzpaket."""
            i = np.floor(h * 6).astype(int) % 6
            f = h * 6 - np.floor(h * 6)
            p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
            r = np.choose(i, [v, q, p, p, t, v])
            g = np.choose(i, [t, v, v, q, p, p])
            b = np.choose(i, [p, p, t, v, v, q])
            out = np.empty(h.shape + (4,), np.float32)
            out[..., 0], out[..., 1], out[..., 2] = r, g, b
            out[..., 3] = 1.0
            return out

        def _g3_stft(self, sig, end, nfr, nfft, hop):
            """nfr komplexe Rahmen, die bei end ENDEN (juengster zuletzt)."""
            if self._sg_win is None or len(self._sg_win) != nfft:
                self._sg_win = np.hanning(nfft).astype(np.float32)
            first = end - (nfr - 1) * hop
            if first - nfft < 0:
                nfr = max(2, (end - nfft) // hop + 1)
                first = end - (nfr - 1) * hop
            if nfr < 2 or first - nfft < 0:
                return None
            F = np.empty((nfr, nfft // 2 + 1), np.complex64)
            for i in range(nfr):
                e = first + i * hop
                F[i] = np.fft.rfft(sig[e - nfft:e] * self._sg_win)
            return F

        def _g3_draw(self, src):
            """Ein Gebirge ueber ALLE DREI Zonen: aufbereitet, Schatten,
            wirkliche Zukunft - auf einer durchgehenden Zeitachse.

            Der Zeitstrahl wird dazu zusammengesetzt: gespielte Ausgabe
            bis jetzt, dann der mitgeschnittene Schatten, dann das
            ankommende Signal mit seinen Loechern.  Die WAHRHEIT dazu ist
            in allen drei Zonen dasselbe Original, deshalb kann der
            Phasenfehler durchgehend gebildet werden - und genau im
            Schattenbereich ist er die interessante Zahl."""
            if not self._g3_check():
                return
            nfft, hop = self._sg_nfft, self._sg_hop
            out, x = src["out"], src["x"]
            end = int(src["end"])
            shadow = src["shadow"]
            ns = 0 if shadow is None else len(shadow)
            nfut = int(self.sg_future.value() / 1000.0 * self.sr)

            # Tiefe in Zeit -> Rahmenzahl und Schrittweite.  Die Zahl der
            # Zeilen bleibt beschraenkt, damit ein langer Blick nicht in
            # Zappeln erstickt; gezeigt wird trotzdem die volle Spanne.
            span = int(self.g3_depth.value() / 1000.0 * self.sr)
            ghop = max(hop, (span // max(self.G3_ROWS, 1)) // hop * hop or hop)
            if end - span - nfft < 0:
                span = max(hop * 4, end - nfft)
            if span < hop * 4:
                return

            # Zeitstrahl und Wahrheit, gleiche Indizierung.
            fut = self._g3_gapped(src, end + ns, end + ns + nfut)
            parts = [out[:end]]
            if ns:
                parts.append(shadow)
            if len(fut):
                parts.append(fut)
            tl = np.concatenate(parts)
            ref = x[:len(tl)]
            if len(ref) < len(tl):
                ref = np.concatenate(
                    (ref, np.zeros(len(tl) - len(ref), np.float32)))

            # Fenster-Endpunkte ueber alle drei Zonen.
            ends = np.arange(end - span, len(tl) + 1, ghop)
            ends = ends[(ends - nfft >= 0) & (ends <= len(tl))]
            if len(ends) < 4:
                return
            if self._sg_win is None or len(self._sg_win) != nfft:
                self._sg_win = np.hanning(nfft).astype(np.float32)
            win = self._sg_win
            F = np.stack([np.fft.rfft(tl[e - nfft:e] * win) for e in ends])

            kmax = max(8, int(self._sg_top_hz() / (self.sr / nfft)))
            kmax = min(kmax, F.shape[1] - 1)
            log_f = self.g3_fscale.currentIndex() == 1
            # 0 = alle vorhandenen Bins.  Nur so schlaegt die
            # Fenstergroesse ueberhaupt auf die Zahl der Stuetzstellen
            # durch: mit festem Deckel liefert 2k wie 8k dieselben 256.
            nbins_max = int(self.g3_bins.value()) or (kmax + 1)
            if log_f:
                # Bins ebenfalls logarithmisch waehlen, nicht nur
                # logarithmisch HINLEGEN: sonst stuenden unten drei
                # Stuetzstellen und oben zweihundert.
                kmin = max(1, int(round(30.0 / (self.sr / nfft))))
                k = np.unique(np.round(np.geomspace(
                    kmin, max(kmin + 1, kmax), nbins_max)).astype(int))
            else:
                step = max(1, kmax // nbins_max)
                k = np.arange(0, kmax, step)
            mag = np.abs(F[:, k])
            db = 20 * np.log10(mag / (nfft / 4) + 1e-12)
            top = float(db.max())
            rng = float(self.g3_range.value())
            if self.g3_ascale.currentIndex() == 1:
                # Linear: Betrag auf das Maximum normiert.  Der
                # Dynamikregler hat hier keinen Sinn und wirkt nicht.
                z = np.clip(mag / (float(mag.max()) or 1.0), 0, 1)
            else:
                z = np.clip((db - (top - rng)) / rng, 0, 1)

            ph = np.angle(F[:, k])
            if self.g3_color.currentIndex() == 1:
                R = np.stack([np.fft.rfft(ref[e - nfft:e] * win) for e in ends])
                dev = ph - np.angle(R[:, k])
            else:
                exp_adv = 2 * np.pi * k * ghop / nfft
                dev = np.zeros_like(ph)
                dev[1:] = ph[1:] - ph[:-1] - exp_adv[None, :]
            dev = (dev + np.pi) % (2 * np.pi) - np.pi

            # AMPLITUDE IST HOEHE, sonst nichts.  Farbe traegt
            # ausschliesslich die Phase, bei voller Saettigung und
            # Helligkeit; die Form liest man aus der Geometrie.
            if self.g3_color.currentIndex() == 2:
                # AMPLITUDE als Farbe - dieselbe Groesse wie die Hoehe,
                # also bewusst doppelt gezeigt.  Mit Streiflicht liest
                # sich der Pegel dadurch auch dort, wo die Form flach
                # ist, und Gipfel gleicher Hoehe bekommen dieselbe Farbe.
                col = self._tap_ramp(
                    z, [(0.0, (10, 4, 30)), (0.3, (86, 24, 122)),
                        (0.58, (190, 60, 84)), (0.8, (245, 150, 30)),
                        (1.0, (252, 252, 190))]
                ).astype(np.float32) / 255.0
                col = np.concatenate(
                    (col, np.ones(col.shape[:-1] + (1,), np.float32)), axis=-1)
            elif self.g3_color.currentIndex() == 3:
                # SANDSTEIN: keine Messgroesse, sondern Gestein - fuer
                # den Blick auf die reine Form.  Ocker nach Hoehe
                # geschichtet, wie Sedimentbaender.
                shade_t = np.clip(z * 0.85
                                  + 0.15 * np.sin(z * 34.0), 0.0, 1.0)
                col = self._tap_ramp(
                    shade_t,
                    [(0.0, (92, 62, 42)), (0.35, (150, 106, 66)),
                     (0.7, (198, 156, 104)), (1.0, (236, 214, 172))]
                ).astype(np.float32) / 255.0
                col = np.concatenate(
                    (col, np.ones(col.shape[:-1] + (1,), np.float32)), axis=-1)
            else:
                col = self._hsv_rgba((dev + np.pi) / (2 * np.pi),
                                     np.ones_like(z), np.ones_like(z))
            floor = z <= 0.002
            if self.g3_mask.isChecked():
                # Wo nichts ist, ist auch keine Phase - sonst wuerde der
                # flache Boden zum Konfettifeld.  Ja/Nein, keine
                # Helligkeit nach Amplitude.
                col[floor] = (0.22, 0.22, 0.25, 1.0)
            if not self.g3_floor.isChecked():
                col[floor, 3] = 0.0        # Ebene ausblenden

            # Hoehe aus dem Regler: Welteinheiten je dB, mal Dynamik.
            # Bei linearer Amplitude wird derselbe Massstab auf 60
            # gedachte dB bezogen, damit das Umschalten die Hoehe nicht
            # springen laesst.
            hspan = float(self.g3_hpd.value()) * (
                rng if self.g3_ascale.currentIndex() == 0 else 60.0)
            tex = float(self.g3_tex.value()) / 100.0
            if tex > 0.0:
                col[..., :3] *= self._g3_grain(z.shape)[..., None] ** tex
            zz = np.ascontiguousarray(z.T.astype(np.float32)) * hspan
            nx, ny = zz.shape
            ysp = max(0.01, float(self.g3_len.value()) / max(ny - 1, 1))
            # Frequenzachse: die Stuetzstellen werden PLATZIERT, nicht
            # umgerechnet - GLSurfacePlotItem nimmt beliebige x.  Beide
            # Skalen auf dieselbe Weltbreite gespannt, damit sich beim
            # Umschalten nur die Verteilung aendert, nicht die Groesse.
            fpos = (np.log2(np.maximum(k, 1) * self.sr / nfft) if log_f
                    else k.astype(np.float64))
            fspan = float(fpos[-1] - fpos[0]) or 1.0
            xs = ((fpos - fpos[0]) / fspan * 256.0 - 128.0).astype(np.float32)

            # ---- SCHATTIERUNG aus der NEIGUNG ------------------------
            # Das ist NICHT die Rueckkehr der Amplitude in die Helligkeit:
            # gerechnet wird der Normalenvektor der Flaeche, also ihre
            # Neigung, nicht ihr Pegel.  Ein hoher flacher Ruecken und
            # der flache Boden bekommen dieselbe Helligkeit - was sich
            # aendert, ist allein die FORM.  Der Farbton bleibt die
            # Phase, unangetastet.
            lit = float(self.g3_light.value()) / 100.0
            if lit > 0.0:
                # Bei logarithmischer Achse sind die Abstaende ungleich -
                # der Gradient muss die KOORDINATEN kennen, sonst waeren
                # die gedraengten Hoehen scheinbar viel steiler.
                gx = np.gradient(zz, xs.astype(np.float64), axis=0)
                gy = np.gradient(zz, axis=1) / ysp
                inv = 1.0 / np.sqrt(gx * gx + gy * gy + 1.0)
                # Licht fest im DATEN-Raum, nicht an der Kamera: beim
                # Drehen soll die Form gleich beleuchtet bleiben, sonst
                # wandert der Eindruck mit dem Blickwinkel.
                lx, ly, lz = -0.35, -0.40, 0.85     # bereits Einheitslaenge
                # Lambert: Skalarprodukt aus Flaechennormale (-gx,-gy,1)*inv
                # und Lichtrichtung.  Die z-Komponente MUSS lz sein, nicht
                # 1.0 - sonst ist der Vektor zu steil und zu lang, ebene
                # Flaechen laufen ueber und kippen beim Deckeln nach
                # Schwarz.  Mit lz = 0.85 steht die Ebene bei 85 % Licht,
                # dunkel werden nur die steilen Flanken.
                lam = (-gx * lx - gy * ly + lz) * inv
                shade = (1.0 - lit) + lit * np.clip(lam, 0.0, 1.0)
                # zz ist [Bin][Zeile], col ist [Zeile][Bin] - erst drehen.
                col[..., :3] *= shade.T[..., None].astype(np.float32)

            cc = np.ascontiguousarray(col.transpose(1, 0, 2).reshape(-1, 4))
            ys = (np.arange(ny, dtype=np.float32) - ny / 2.0) * ysp
            mode = self.g3_draw.currentIndex()
            cc_lines = cc
            if mode:
                # Bei 341 x 225 Maschen ist ein Drahtmodell kein Modell,
                # sondern Nebel - die Linien decken die Flaeche
                # vollstaendig zu.  Fuer die Gittermodi wird das Netz
                # deshalb ausgeduennt, bis man hindurchsieht.
                sx = max(1, nx // self.G3_WIRE_X)
                sy = max(1, ny // self.G3_WIRE_Y)
                if sx > 1 or sy > 1:
                    zz = np.ascontiguousarray(zz[::sx, ::sy])
                    cc = np.ascontiguousarray(
                        cc.reshape(nx, ny, 4)[::sx, ::sy].reshape(-1, 4))
                    cc_lines = cc
                    xs, ys = xs[::sx], ys[::sy]
                    # ends MUSS mit ausgeduennt werden: die Zonenwaende
                    # suchen ihre Zeile per searchsorted in ends und
                    # greifen dann in ys.  Blieb ends voll besetzt, lag
                    # der Index jenseits des verkuerzten ys und die Wand
                    # wurde als "nicht vorhanden" geloescht - genau
                    # deshalb fehlte die Schattenmarkierung in den
                    # Gittermodi.
                    ends = ends[::sy]
                    nx, ny = zz.shape
            if mode == 2:
                # Drahtmodell: die Flaechen ganz durchsichtig, nur die
                # Maschenlinien bleiben - die tragen dann die Farbe.
                cc_lines = cc
                cc = cc.copy()
                cc[:, 3] = 0.0
            # Linienfarbe SCHON HIER bestimmen und mitgeben.  In zwei
            # Schritten (erst setData, dann lineplot.setData(color=...))
            # gibt es einen Zwischenzustand, in dem Positionen und Farben
            # aus verschiedenen Gittergroessen stammen - faellt ein
            # Neuzeichnen hinein, meldet pyqtgraph "Error while drawing
            # item".  _update_grid reicht lineColor unveraendert an den
            # Linienzug weiter, ein Feld je Knoten geht also genauso.
            # IMMER ein Feld je Knoten, nie ein Tupel - auch fuer die
            # einfarbige Variante.  GLLinePlotItem setzt sein
            # COLOR-Schmutzbit nur, wenn eine Farbe als Feld kommt, und
            # loescht es beim Wechsel zurueck auf ein Tupel NICHT: beim
            # naechsten Zeichnen versucht es dann tuple.nbytes und wirft
            # "Error while drawing item".  Bleibt der Typ gleich, kann
            # der Fall nicht eintreten.
            line_col = None
            if mode:
                rows, cols = zz.shape
                idx = np.arange(zz.size, dtype=np.int32).reshape(rows, cols)
                edges = np.vstack((
                    np.column_stack((idx[:, :-1].ravel(), idx[:, 1:].ravel())),
                    np.column_stack((idx[:-1, :].ravel(), idx[1:, :].ravel()))))
                line_col = np.tile(
                    np.array([0.75, 0.78, 0.82, 0.55], np.float32),
                    (edges.size, 1))
            if mode == 2:
                # Ohne Flaechen traegt das Gitter die ganze Aussage, also
                # bekommt es auch die Farbe.  _update_grid legt die
                # Linien als Punktpaare in EINER festen Reihenfolge ab
                # (erst waagerecht, dann senkrecht, je Knotenindex) - die
                # Farben muessen genau dieser Reihenfolge folgen.
                line_col = cc_lines[edges].reshape(-1, 4).copy()
                line_col[:, 3] = 0.95
            self.g3_surf.setData(
                x=xs, y=ys, z=zz, colors=cc,
                showGrid=mode in (1, 2), lineWidth=1.0,
                **({"lineColor": line_col} if line_col is not None else {}))
            if not mode:
                # _update_grid der Bibliothek kehrt bei showGrid=False
                # sofort zurueck und raeumt die ALTEN Linien nie weg -
                # das Gitter blieb sonst nach dem Umschalten stehen.
                self.g3_surf.lineplot.setData(pos=np.zeros((0, 3), np.float32))


            # ---- Zonengrenzen ----------------------------------------
            # Ein Rahmen quer durchs Gitter dort, wo 'jetzt' bzw. das
            # Schattenende liegt.  Die Zeile ist die erste, deren
            # FENSTER hinter der Grenze endet.
            x0, x1 = float(xs[0]), float(xs[-1])
            ztop = float(zz.max()) * 1.05 + 2.0
            for ln, bound in zip(self.g3_marks, (end, end + ns)):
                i = int(np.searchsorted(ends, bound))
                if i >= len(ys) or (bound == end + ns and not ns):
                    ln.setData(pos=np.zeros((0, 3), np.float32))
                    continue
                y = float(ys[i])
                # WAND statt Rahmen: ein duenner Umriss verschwindet im
                # Gebirge, eine Lattenwand aus senkrechten Strichen liest
                # sich auch von schraeg oben als Grenze.
                xg = np.linspace(x0, x1, 26, dtype=np.float32)
                seg = np.empty((len(xg) * 2 + 2, 3), np.float32)
                seg[:-2:2, 0] = xg
                seg[1:-2:2, 0] = xg
                seg[:-2:2, 2] = 0.0
                seg[1:-2:2, 2] = ztop
                seg[-2] = (x0, y, ztop)
                seg[-1] = (x1, y, ztop)
                seg[:, 1] = y
                ln.setData(pos=seg, mode="lines")
            # Beschriftung NEBEN dem GL-Fenster, nicht darin: GLTextItem
            # zeichnet unter Qt 6.11 ueberhaupt nicht (geprueft, null
            # Pixel in beiden Abzugsarten), und ein unsichtbares Etikett
            # ist schlimmer als gar keins.
            self.g3_legend.setText(
                "<span style='color:#ffcc66'>&#9608; aufbereitet</span> "
                f"({1000 * span / self.sr:.0f} ms)"
                " &nbsp;|&nbsp; <span style='color:#ffffff'>&#9553; jetzt"
                "</span> &nbsp;|&nbsp; "
                "<span style='color:#ff8ae8'>&#9608; SCHATTEN</span> "
                f"({1000 * ns / self.sr:.0f} ms)"
                " &nbsp;|&nbsp; <span style='color:#ff70e0'>&#9553; "
                "Schattenende</span> &nbsp;|&nbsp; "
                "<span style='color:#66e6f0'>&#9608; Zukunft</span> "
                f"({1000 * len(fut) / self.sr:.0f} ms)"
                f" &nbsp;&nbsp;-&nbsp;&nbsp; Zeile = {1000 * ghop / self.sr:.1f} ms, "
                f"f {'log' if log_f else 'linear'} "
                f"({self.sr * k[0] / nfft:.0f}-{self.sr * k[-1] / nfft:.0f} Hz, "
                f"{len(k)} von {kmax} Bins), "
                + (f"A linear" if self.g3_ascale.currentIndex() == 1
                   else f"A {rng:.0f} dB unter {top:.0f} dBFS"))

        def _g3_grain(self, shape):
            """Sandstein-Korn fuer ein Gitter dieser Groesse.

            FEST verwuerfelt und gemerkt: ein je Bild neu gezogenes Korn
            wuerde zwischen den Live-Bildern flimmern und dabei aussehen,
            als bewege sich die Flaeche.  Feines Korn mal waagerechte
            Schichtung - Sediment, nicht Rauschen."""
            if getattr(self, "_g3_grain_shape", None) != shape:
                rng = np.random.default_rng(20260812)
                g = rng.normal(1.0, 0.09, shape)
                # Schichten laufen quer zur Hoehe, also entlang der Zeit.
                bands = 1.0 + 0.06 * np.sin(
                    np.arange(shape[1])[None, :] * 0.9
                    + rng.normal(0, 0.3, (shape[0], 1)))
                self._g3_grain_cache = np.clip(
                    g * bands, 0.55, 1.35).astype(np.float32)
                self._g3_grain_shape = shape
            return self._g3_grain_cache

        def _g3_gapped(self, src, a, b):
            """Ankommendes Signal in [a,b): Original mit Loechern."""
            x = src["x"]
            a, b = max(0, a), min(b, len(x))
            if b <= a:
                return np.zeros(0, np.float32)
            sig = np.array(x[a:b], np.float32)
            buf = src["buf"]
            for kk in src["miss"]:
                k0 = kk * buf
                if k0 + buf <= a or k0 >= b:
                    continue
                sig[max(0, k0 - a):min(b, k0 + buf) - a] = 0.0
            return sig

        def _g3_check(self):
            """Hat sich der GL-Kontext verweigert?  Dann Ersatztext."""
            bad = HAVE_GL and SafeGLView.failed
            if bad and self.g3 is not None:
                self.g3.setVisible(False)
                self.g3_fallback.setVisible(True)
            return not bad and self.g3_surf is not None

        def _g3_live(self, ctx):
            """Waehrend des Laufs: Gebirge am aktuellen Rand aufbauen.

            Der SCHATTEN wird hier mitgeschnitten, nicht spaeter geholt:
            nach dem Lauf ist die Instanz geschlossen und peek() tot."""
            inst = ctx["inst"]
            self._sg_geometry(inst)
            end = (ctx["period"] + 1) * ctx["buf"]
            try:
                h = inst.horizon_frames()
                shadow = inst.peek(h)[:, 0].astype(np.float32) if h else None
            except Exception:
                shadow = None
            self._g3_src = {
                "out": np.asarray(ctx["out"][:, 0], np.float32),
                "x": np.asarray(ctx["x"][:, 0], np.float32),
                "miss": ctx["miss"], "buf": ctx["buf"],
                "shadow": shadow, "end": end}
            self._g3_draw(self._g3_src)

        def _g3_fit(self, *_):
            """Kamera an die Geometrie anpassen - NUR beim Aendern der
            Laenge, nicht bei jedem Neuzeichnen: sonst wuerde jede
            Mausdrehung des Nutzers gleich wieder zurueckgesetzt."""
            if self.g3 is None:
                return
            reach = max(256.0, float(self.g3_len.value()))
            self.g3.setCameraPosition(distance=reach * 1.45)

        def _g3_redraw(self, *_):
            """Nach Umschalten neu zeichnen, ohne neuen Lauf."""
            if self.g3_surf is None or not self._g3_src:
                return
            try:
                self._g3_draw(self._g3_src)
            except Exception:
                traceback.print_exc()

        def _sg_gapped(self, ctx, a, b):
            """Das ANKOMMENDE Signal im Bereich [a,b): Original, aber die
            ausgefallenen Perioden als Loecher.  Das ist, was die Engine
            bekommt - und rechts vom Schatten das, wogegen sich seine
            Vorhersage gleich bewaehren muss."""
            a, b = max(0, a), min(b, len(ctx["x"]))
            if b <= a:
                return np.zeros(0, np.float32)
            src = np.array(ctx["x"][a:b, 0], np.float32)
            if self.sg_src.currentIndex() == 0:
                buf = ctx["buf"]
                for k in ctx["miss"]:
                    k0 = k * buf
                    if k0 + buf <= a or k0 >= b:
                        continue
                    src[max(0, k0 - a):min(b, k0 + buf) - a] = 0.0
            return src

        def _sg_live(self, ctx):
            inst, out = ctx["inst"], ctx["out"]
            self._sg_geometry(inst)
            hop, nfft = self._sg_hop, self._sg_nfft
            played = (ctx["period"] + 1) * ctx["buf"]
            sr = self.sr

            # ---- LINKS: das aufbereitete Signal, also was gespielt wurde
            if self._sg_done == 0:
                self._sg_done = nfft
            cols, self._sg_done = self._sg_columns(
                out[:, 0], self._sg_done, played, hop)
            self._sg_cols.extend(cols)
            if not self._sg_cols:
                return
            past = np.asarray(self._sg_cols, dtype=np.float32)
            t_now = played / sr
            self.sg_img.setImage(past, autoLevels=False,
                                 levels=(self.SG_FLOOR_DB, 0.0))
            self.sg_img.setRect(QRectF(
                nfft / sr, 0.0, max(len(past) * hop / sr, 1e-6), sr / 2))

            # ---- MITTE: der Schatten.  Was der Praediktor JETZT spielen
            # wuerde - bei jedem Blick frisch geholt, er darf sich also
            # aendern.  Laenger als 32 ms geht nicht: der Horizont steht
            # im Konstruktor der Bibliothek (m_HorizonPeriods) und ist
            # genau die Menge Verdeckung, die sie bereithaelt.
            try:
                horizon = inst.horizon_frames()
            except Exception:
                horizon = 0
            spec = inst.peek(horizon) if horizon else None
            shop = max(16, hop // 4)   # feiner, sonst zu wenige Spalten
            spec_end = played
            if spec is not None and len(spec):
                tail = out[max(0, played - nfft):played, 0]
                joined = np.concatenate((tail, spec[:, 0]))
                scols, _ = self._sg_columns(
                    joined, len(tail) + shop, len(joined), shop)
                if scols:
                    self.sg_spec_img.setImage(
                        np.asarray(scols, dtype=np.float32), autoLevels=False,
                        levels=(self.SG_FLOOR_DB, 0.0))
                    self.sg_spec_img.setRect(QRectF(
                        t_now, 0.0, len(scols) * shop / sr, sr / 2))
                    spec_end = played + len(scols) * shop
            else:
                self.sg_spec_img.clear()

            # ---- RECHTS: die echte Zukunft aus der Datei, mit ihren
            # Loechern.  Nicht der Praediktor - das ist, was WIRKLICH
            # kommt, und damit der Massstab, an dem der Schatten links
            # daneben zu messen ist.
            fut_n = int(self.sg_future.value() / 1000.0 * sr)
            fcols = []
            if fut_n > 0:
                # Vorlauf von einem Fenster, damit die erste Spalte nicht
                # aus dem Nichts kommt; Spalten deckeln, sonst kostet ein
                # langer Ausblick bei 4k spuerbar Zeit.
                fhop = hop
                want = fut_n // fhop
                if want > self.SG_FUTURE_MAX_COLS:
                    fhop = max(hop, fut_n // self.SG_FUTURE_MAX_COLS)
                sig = self._sg_gapped(ctx, spec_end - nfft, spec_end + fut_n)
                if len(sig) > nfft:
                    fcols, _ = self._sg_columns(sig, nfft + fhop, len(sig),
                                                fhop)
                if fcols:
                    self.sg_future_img.setImage(
                        np.asarray(fcols, dtype=np.float32), autoLevels=False,
                        levels=(self.SG_FLOOR_DB, 0.0))
                    self.sg_future_img.setRect(QRectF(
                        spec_end / sr, 0.0, len(fcols) * fhop / sr, sr / 2))
            if not fcols:
                self.sg_future_img.clear()

            self.sg_now.setValue(t_now)
            self.sg_specend.setValue(spec_end / sr)
            top = self._sg_top_hz()
            self.sg.setLimits(yMin=0, yMax=sr / 2)
            self.sg.setYRange(0, top)
            back = self.sg_back.value() / 1000.0
            # Rechter Rand = so weit WIRKLICH gezeichnet wurde.  Am
            # Dateiende ist die Zukunft kuerzer als bestellt, und die
            # Achse bis zur Bestellung zu ziehen liesse dort einen
            # leeren Streifen stehen.
            x0 = max(0.0, t_now - back)
            x1 = spec_end / sr
            if fcols:
                r = self.sg_future_img.boundingRect()
                x1 = self.sg_future_img.mapRectToView(r).right()
            self.sg.setXRange(x0, x1)
            # Namen an die Mitte ihrer Zone, knapp unter den oberen Rand.
            ty = top * 0.94
            for ti, tx in zip(self.sg_tags,
                              ((x0 + t_now) / 2,
                               (t_now + spec_end / sr) / 2,
                               (spec_end / sr + x1) / 2)):
                ti.setPos(tx, ty)
            self.sg.setTitle(
                f"links aufbereitet | Schatten {1000 * horizon / sr:.0f} ms "
                f"| rechts {self.sg_src.currentText()}"
                f"   -   Fenster {nfft} @ Sprung {hop} "
                f"({sr / nfft:.1f} Hz / {1000 * hop / sr:.1f} ms)"
                + ("" if nfft == self._sg_engine_nfft
                   else f" [Engine {self._sg_engine_nfft}]")
                + f", AR-Raster {self._sg_arhop}   -   jetzt {t_now:.2f} s")

        def _live_tick(self, ctx):
            """Ein Einstieg fuer alle mitlaufenden Anzeigen.

            Laeuft im Rechen-Thread (es gibt keinen zweiten) und darf
            deshalb NICHTS werfen: der Lauf gehoert der Messung."""
            view = self.stack.currentIndex()
            if view not in (2, 3, 4, 5):
                return
            now = time.perf_counter()
            if now - self._tap_last_draw < 0.12:      # ~8 Bilder/s
                return
            self._tap_last_draw = now
            try:
                if view == 4:
                    self._sg_live(ctx)
                elif view == 5:
                    self._g3_live(ctx)
                elif self.tap_box.isChecked():
                    self._tap_live_inner(ctx["period"], ctx["n_periods"])
            except Exception:
                traceback.print_exc()
                self.tap_box.setChecked(False)
                self.tap_label.setText(
                    "Live-Anzeige nach Fehler abgeschaltet - Rechnung "
                    "laeuft weiter, Meldung steht auf der Konsole")
                return
            QApplication.processEvents()

        def _big_toggle(self, *_):
            """Parameterzeilen und Waveform weg - Platz fuers Bild."""
            big = self.big_box.isChecked()
            for name in ("ctl_scroll", "wave", "slider_row_w",
                         "abx_row_w", "numbers"):
                widget = getattr(self, name, None)
                if widget is not None:
                    widget.setVisible(not big)

        def _tap_ui_visible(self, view_idx):
            """Steuerzeile nur zeigen, wo sie etwas steuert."""
            if view_idx == 5:
                self._g3_check()
            # Das Vergleichsspektrum gehoert zu den Luecken-Ansichten.
            # Bei Teppich, z-Ebene, Scroll und Gebirge kostet es nur die
            # halbe Breite, ohne etwas zu diesen Bildern beizutragen.
            # getattr: _tap_ui_visible laeuft schon im Konstruktor,
            # bevor das Vergleichsfeld gebaut ist.
            cmp_w = getattr(self, "compare", None)
            if cmp_w is not None:
                cmp_w.setVisible(view_idx in (0, 1))
            groups = getattr(self, "_view_ctrls", None)
            if groups and self.allctl_box.isChecked():
                for widgets in groups.values():
                    for widget in widgets:
                        widget.setVisible(True)
            elif groups:
                want = {"spec": view_idx in (0, 1),
                        "sg": view_idx == 4,
                        "g3": view_idx == 5,
                        "nfft": view_idx in (4, 5),
                        "top": view_idx in (4, 5)}
                for name, widgets in groups.items():
                    for widget in widgets:
                        widget.setVisible(want[name])
            on = view_idx in (2, 3)
            self.tap_row_w.setVisible(on)
            self.tap_label.setVisible(on)
            if not on and self.tap_timer.isActive():
                self._tap_play_toggle()
            elif on and self.tap_data:
                # Beim Umschalten neu zeichnen: die z-Ebene haengt am
                # gerade gewaehlten Bild, nicht am Lauf.
                self._tap_frame_changed(self.tap_slider.value())

        def _tap_capture(self):
            """Mitschrift des letzten Laufs uebernehmen; gibt den Text
            fuer die Zahlenzeile zurueck (WIRKUNG, nicht Hakenzustand)."""
            tap = getattr(engine_run, "tap", (None, None, None))
            z, path, pos = tap[:3]
            roots = tap[3] if len(tap) > 3 else None
            if z is None or not len(z):
                self.tap_data = None
                self.tap_slider.setRange(0, 0)
                self._draw_tap()
                if not self.tap_box.isChecked():
                    return "aus"
                if not self.gpu_box.isChecked():
                    return "AN, aber CPU-Solve (kein Tap)"
                return "AN, aber 0 Bilder (Ordnung 1? Strategie ohne Bibliothek?)"
            self.tap_data = (z, path, pos, self._tap_thresh(), roots)
            self.tap_slider.blockSignals(True)
            self.tap_slider.setRange(0, len(pos) - 1)
            self.tap_slider.setValue(len(pos) - 1)
            self.tap_slider.blockSignals(False)
            self._draw_tap()
            order = roots.shape[2] if roots is not None and roots.size else 0
            return f"{len(pos)} Bilder x {z.shape[1]} Bins x {order} Pole"

        def _tap_live_inner(self, period, n_periods):
            """Neue Bilder der Mitschrift holen und zeichnen.

            Gerufen aus _live_tick, der schon nach Uhrzeit bremst und
            Fehler abfaengt.  Die ZUSAMMENFASSUNG (z/path) ist klein
            genug, um den ganzen Ring je Blick zu holen; die Pole sind
            es nicht - fuer die z-Ebene reicht ohnehin das NEUESTE Bild,
            denn genau eines ist dort zu sehen."""
            if _xo is None:
                return
            nf, nb, order, total = _xo.solve_tap_info()
            if not nf or total == self._tap_seen:
                return
            self._tap_seen = total
            if self.stack.currentIndex() == 3:
                z, path, pos, roots = _xo.solve_tap(max_frames=1, roots=True)
            else:
                z, path, pos = _xo.solve_tap()
                roots = None
            if not len(pos):
                return
            self.tap_data = (z, path, pos, self._tap_thresh(), roots)
            self.tap_slider.blockSignals(True)
            self.tap_slider.setRange(0, len(pos) - 1)
            self.tap_slider.setValue(len(pos) - 1)
            self.tap_slider.blockSignals(False)
            self._draw_tap()
            self.tap_label.setText(
                self.tap_label.text()
                + f"   [laeuft: Periode {period}/{n_periods}]")
            QApplication.processEvents()

        @staticmethod
        def _tap_ramp(t, stops):
            """Farbverlauf ueber Stuetzstellen [(Lage, (r, g, b)), ...]."""
            xs = np.array([s[0] for s in stops], float)
            out = np.empty(t.shape + (3,), np.uint8)
            for c in range(3):
                ys = np.array([s[1][c] for s in stops], float)
                out[..., c] = np.interp(t, xs, ys).astype(np.uint8)
            return out

        def _tap_rgb(self, z, path, thr):
            if self.tap_mode.currentIndex() == 1:
                rgb = np.zeros(path.shape + (3,), np.uint8)
                for k, col in self.TAP_COLORS.items():
                    rgb[path == k] = col
                return rgb
            # Harter Bruch an der Schwelle: darunter kalt (klingt ab),
            # darueber heiss (wollte wachsen, wurde gedeckelt).  Zwei
            # getrennte Verlaeufe statt eines durchgehenden - ein
            # stetiger Verlauf wuerde genau die Grenze verstecken,
            # derentwegen man hinsieht.
            cold = self._tap_ramp(
                np.clip(z / thr, 0.0, 1.0),
                [(0.0, (12, 12, 45)), (0.55, (26, 90, 190)),
                 (0.85, (0, 200, 205)), (1.0, (150, 240, 140))])
            hot = self._tap_ramp(
                np.clip((z - thr) / 0.6, 0.0, 1.0),
                [(0.0, (255, 235, 120)), (0.4, (255, 150, 0)),
                 (0.75, (220, 45, 25)), (1.0, (140, 0, 80))])
            rgb = np.where((z > thr)[..., None], hot, cold).astype(np.uint8)
            # Bins ohne berechnete Wurzel kriegen KEINE Radiusfarbe -
            # sonst laese man aus der -1 eine Aussage heraus.
            for k in (-1, 0, 1, 5, 6):
                rgb[path == k] = self.TAP_COLORS[k]
            return rgb

        def _draw_tap(self):
            if not self.tap_data:
                self.tap_img.clear()
                self.z_pts.setData([], [])
                self.tap_label.setText(
                    "Solve-Mitschrift: keine Bilder - Haken 'Solve "
                    "mitschreiben' UND 'GPU' setzen, Ordnung >= 2, "
                    "GO-Strategie waehlen")
                return
            z, path, pos, thr, _roots = self.tap_data
            self.tap_img.setImage(
                self._tap_rgb(z, path, thr), autoLevels=False)
            t0, t1 = pos[0] / self.sr, pos[-1] / self.sr
            nyq = self.sr / 2
            self.tap_img.setRect(
                QRectF(t0, 0.0, max(t1 - t0, 1e-6), nyq))
            self.tap_plot.setLimits(yMin=0, yMax=nyq)
            self.tap_plot.setYRange(0, min(nyq, 16000))
            what = ("Zweig" if self.tap_mode.currentIndex()
                    else f"Polradius, Bruch bei {thr:.2f}")
            self.tap_plot.setTitle(f"Solve-Teppich - {what}")
            phi = np.linspace(0, 2 * np.pi, 361)
            self.z_thr_circle.setData(thr * np.cos(phi), thr * np.sin(phi))
            self._tap_frame_changed(self.tap_slider.value())

        def _draw_zplane(self, i):
            """Ein Bild: alle Pole dieses Solve-Durchlaufs.

            Farbe nach Frequenz, damit erkennbar bleibt, WELCHE Baender
            nach aussen laufen - ohne das waere es eine anonyme Wolke.
            Pole ohne Wert (Detektor bestanden, singulaer) sind NAN und
            fallen von selbst heraus."""
            z, path, pos, thr, roots = self.tap_data
            if roots is None or not roots.size:
                self.z_pts.setData([], [])
                self.zpl.setTitle(
                    "Solve z-Ebene - keine Pole mitgeschrieben")
                return
            # Beim Live-Blick enthaelt roots nur das neueste Bild.
            r = roots[i] if roots.shape[0] > 1 else roots[0]
            nb, order = r.shape[0], r.shape[1]
            # Bei hohen Ordnungen sind es zu viele Punkte fuer fluessiges
            # Abspielen; dann jeden k-ten BIN nehmen (nicht jeden k-ten
            # Pol - sonst faehrt man einem Bin die Wurzelmenge auseinander).
            step = max(1, (nb * order) // self.Z_MAX_POINTS)
            r = r[::step]
            nb_s = r.shape[0]
            re, im = r[..., 0].ravel(), r[..., 1].ravel()
            ok = np.isfinite(re) & np.isfinite(im)
            re, im = re[ok], im[ok]
            if not len(re):
                self.z_pts.setData([], [])
                self.zpl.setTitle("Solve z-Ebene - dieses Bild ohne Pole")
                return
            # Frequenz -> Farbe, aber nur in Z_COLORS Stufen: pyqtgraph
            # legt seinen Zeichen-Zwischenspeicher je PINSEL an, und
            # 2049 verschiedene Pinsel je Bild machen das Abspielen
            # zaeh.  Die Stufung ist im Bild nicht zu sehen.
            klass = np.repeat(
                (np.arange(nb_s) * self.Z_COLORS) // max(nb_s, 1), order)[ok]
            self._z_brushes()
            self.z_pts.setData(x=re, y=im, brush=self._z_brush_lut[klass])
            out = np.hypot(re, im) > thr
            self.zpl.setTitle(
                f"Solve z-Ebene - t = {pos[i] / self.sr:.3f} s, "
                f"{len(re)} Pole, {100 * out.mean():.1f} % jenseits "
                f"{thr:.2f}")

        def _tap_frame_changed(self, i):
            if not self.tap_data:
                return
            z, path, pos, thr, roots = self.tap_data
            i = min(max(0, i), len(pos) - 1)
            self.tap_cursor.setValue(pos[i] / self.sr)
            if self.stack.currentIndex() == 3:
                self._draw_zplane(i)
            zi, pi = z[i], path[i]
            real = pi >= 2            # nur Bins mit echter Wurzel
            parts = []
            for k, cnt in zip(*np.unique(pi, return_counts=True)):
                parts.append(f"{_xo.TAP_PATHS[int(k)]} {100 * cnt / len(pi):.0f} %")
            radius = (f"med |z| {np.median(zi[real]):.3f}, "
                      f"ueber {thr:.2f}: {100 * (zi[real] > thr).mean():.1f} %"
                      if real.any() else "keine gerechneten Wurzeln")
            self.tap_label.setText(
                f"Bild {i + 1}/{len(pos)}   t = {pos[i] / self.sr:.3f} s   "
                f"({pos[i]} smp)   |   {radius}   |   " + ", ".join(parts))

        def _tap_play_toggle(self):
            if self.tap_timer.isActive():
                self.tap_timer.stop()
                self.tap_play_btn.setText("▶")
            elif self.tap_data:
                if self.tap_slider.value() >= self.tap_slider.maximum():
                    self.tap_slider.setValue(0)
                self.tap_timer.start()
                self.tap_play_btn.setText("■")

        def _tap_tick(self):
            if self.tap_slider.value() >= self.tap_slider.maximum():
                self._tap_play_toggle()
                return
            self.tap_slider.setValue(self.tap_slider.value() + 1)

        def spectrogram(self, x, sr):
            nfft = int(self.spec_combo.currentData())
            hop = nfft // 4
            win = np.hanning(nfft)
            n_frames = max(1, (len(x) - nfft) // hop)
            S = np.empty((n_frames, nfft // 2 + 1))
            for i in range(n_frames):
                seg = x[i * hop:i * hop + nfft] * win
                S[i] = np.abs(np.fft.rfft(seg))
            db = 20 * np.log10(S / (nfft / 4) + 1e-12)
            self.spec_img.setImage(db, autoLevels=False,
                                   levels=self._spec_levels())
            # FENSTER-ENDE-Konvention (08.08.): jede Spalte wird an das
            # ENDE ihres Fensters gezeichnet - wie die Engine analysiert
            # (Rahmen enden an ihrer Zeit).  Vorher stand die Spalte am
            # ANFANG: eine Anschlagslinie erschien bis zu eine
            # Fensterlaenge VOR ihrer wahren Zeit (85 ms bei 4k!), die
            # Marks an wahren Zeiten wirkten dagegen 'zu spaet' -
            # Anzeige-Parallaxe.  Jetzt liegen Bild, Marks und
            # Engine-Wissen auf derselben Achse.
            self.spec_img.setRect(
                QRectF(nfft / sr, 0, n_frames * hop / sr, sr / 2))
            self.spec.setLimits(xMin=0, xMax=len(x) / sr, yMin=0, yMax=sr / 2)
            self.spec.setYRange(0, min(sr / 2, 16000))
            self._update_causal()

        def spectrum(self, curve, x, centre, width=4096):
            a = max(0, int(centre) - width // 2)
            seg = x[a:a + width]
            if len(seg) < width:
                seg = np.pad(seg, (0, width - len(seg)))
            S = np.fft.rfft(seg * np.hanning(width))
            db = 20 * np.log10(np.abs(S) / (width / 4) + 1e-12)
            f = np.fft.rfftfreq(width, 1 / self.sr)
            curve.setData(f[1:], db[1:])

        def tick(self):
            if self.player is None:
                return
            pos = (time.monotonic() - self.t0 - 0.08) * self.sr
            if pos >= len(self.x) or self.player.poll() is not None:
                self.clock.stop()
                self.cursor.setValue(0)
                return
            self.cursor.setValue(max(0.0, pos / self.sr))
            self.spec_cursor.setValue(max(0.0, pos / self.sr))
            if self.stack.currentIndex() == 1:
                self.spectrum(self.live_curve, self.x[:, 0], pos)

        def stop(self):
            self.clock.stop()
            if self.player is not None:
                self.player.terminate()
                self.player = None
            self.cursor.setValue(0)

        # --------------------------------------------------------------
        # ABX
        # --------------------------------------------------------------
        def _abx_new_trial(self):
            cond = (self.sig_combo.currentText(),
                    self.strat_combo.currentText(),
                    self.gap_spin.value(), self.fade_spin.value(),
                    self.rate_spin.value(),
                    self.align_box.isChecked(), self.slider.value())
            if getattr(self, "abx_cond", None) not in (None, cond) \
                    and self.abx_n:
                self.abx_n = self.abx_k = 0
                self.abx_label.setText("condition changed - counter reset")
            self.abx_cond = cond
            exc, ref, sr, *_ = self._build_cached()
            self.abx_a, self.abx_b, self.abx_sr = exc, ref, sr
            self.abx_x_is = random.choice("AB")
            return True

        def _abx_play(self, which):
            if self.abx_a is None or self.abx_x_is is None:
                if not self._abx_new_trial():
                    return
            sig = {"A": self.abx_a, "B": self.abx_b,
                   "X": self.abx_a if self.abx_x_is == "A"
                   else self.abx_b}[which]
            if self.abx_player is not None:
                self.abx_player.terminate()
            path = WAV_OUT.parent / "abx.wav"
            path.parent.mkdir(parents=True, exist_ok=True)
            self._write_wav(path, sig, self.abx_sr)
            self.abx_player = subprocess.Popen(["paplay", str(path)])

        def _abx_answer(self, guess):
            if self.abx_x_is is None:
                self.abx_label.setText("play X first")
                return
            self.abx_n += 1
            if guess == self.abx_x_is:
                self.abx_k += 1
            n, k = self.abx_n, self.abx_k
            p = sum(math.comb(n, i) for i in range(k, n + 1)) / 2 ** n
            verdict = ("distinguishable" if p < 0.05 else
                       "not distinguishable so far")
            self.abx_label.setText(
                f"{k}/{n} correct, p = {p:.3f} ({verdict}; new X drawn)")
            self._abx_new_trial()

        def _abx_reset(self):
            self.abx_n = self.abx_k = 0
            self.abx_a = self.abx_b = self.abx_x_is = None
            if self.abx_player is not None:
                self.abx_player.terminate()
                self.abx_player = None
            self.abx_label.setText("no trials yet")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selftest", action="store_true",
                    help="headless number table instead of the GUI")
    ap.add_argument("wavs", nargs="*", help="WAV files for --selftest")
    args = ap.parse_args()
    if args.selftest:
        selftest(args.wavs)
        return
    if not HAVE_QT:
        sys.exit("PySide6/pyqtgraph not available - GUI needs them "
                 "(--selftest runs without)")
    app = QApplication(sys.argv)
    win = GapLab()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
