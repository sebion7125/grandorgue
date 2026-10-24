"""ctypes binding for libxorcist - the concealment the engine actually runs.

Why this exists: the lab used to reimplement the algorithm in numpy.  On
2026-08-03 a sample-by-sample cross-check showed the two had silently
drifted apart (different Burg order, an ungated level trend, a different
refit schedule), so lab conclusions about the seam did not transfer to
the product.  Binding the library removes that failure mode by
construction - and it happens to be ~11x faster than the numpy path.

Do not build this by hand - use the deploy script, which builds with
GrandOrgue's OWN flags and leaves a stamp file next to the .so:

    <grandorgue>/tools/concealment/deploy_lab.sh

This docstring used to prescribe `-O3 -march=native -ffast-math`, and
that was wrong twice over: GO ships generic x86-64, so -march=native is
not the code the product runs, and the copy that actually ended up
deployed had neither flag.  Same source, but up to 12 dB apart at AR
order 20 - the normal equations are ill-conditioned enough up there that
the rounding order decides.  Orders up to 12 are flag-independent.
"""

import ctypes
import os
from pathlib import Path

import numpy as np

OFF, PV_2K, PV_4K, PV_8K = 0, 1, 2, 3

_SEARCH = [
    os.environ.get("XORCIST_LIB"),
    # Next to this module - the deploy target for the HOST, where the VM
    # paths below do not exist.  (They silently did not, for a while: the
    # lab then fell back to numpy without anyone noticing, which is
    # exactly the drift this binding exists to prevent.  The GUI now
    # labels the fallback, see gap_lab.)
    Path(__file__).with_name("libxorcist.so"),
    Path.home() / "grandorgue/src/xorcist/libxorcist.so",
    Path("libxorcist.so"),
]


def _load():
    last = None
    for cand in _SEARCH:
        if not cand:
            continue
        try:
            return ctypes.CDLL(str(cand))
        except OSError as e:
            last = e
            # Missing libfftw3f on the host: borrow the one the GrandOrgue
            # AppImage ships, then retry once.
            if "fftw" in str(e):
                for fftw in (Path.home() / "squashfs-root/usr/lib/libfftw3f.so.3",
                             Path("/media/sf_sebion/squashfs-root/usr/lib/libfftw3f.so.3")):
                    if fftw.exists():
                        try:
                            ctypes.CDLL(str(fftw), mode=ctypes.RTLD_GLOBAL)
                            return ctypes.CDLL(str(cand))
                        except OSError as e2:
                            last = e2
    raise OSError(
        f"libxorcist.so not found/loadable ({last}) - build it (see this "
        "module's docstring) or point XORCIST_LIB at it")


_lib = _load()

_lib.Xorcist_new.restype = ctypes.c_void_p
_lib.Xorcist_new.argtypes = [ctypes.c_uint] * 3 + [ctypes.c_char_p]
_lib.Xorcist_free.argtypes = [ctypes.c_void_p]
_lib.Xorcist_setMode.argtypes = [ctypes.c_void_p, ctypes.c_int]
_F32 = np.ctypeslib.ndpointer(np.float32, flags="C_CONTIGUOUS")
_lib.Xorcist_push.argtypes = [ctypes.c_void_p, _F32]
_lib.Xorcist_pushMany.argtypes = [ctypes.c_void_p, _F32, ctypes.c_uint]
_lib.Xorcist_absorb.argtypes = [ctypes.c_void_p]
_lib.Xorcist_patchForce.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                    ctypes.c_uint64, ctypes.c_uint]
_lib.Xorcist_patchForce.restype = ctypes.c_int
_lib.Xorcist_patchRead.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                   ctypes.POINTER(ctypes.c_uint64), _F32,
                                   ctypes.c_uint]
_lib.Xorcist_patchRead.restype = ctypes.c_uint
_lib.Xorcist_patchClear.argtypes = [ctypes.c_void_p]
_lib.Xorcist_predict.argtypes = [ctypes.c_void_p]
_lib.Xorcist_predict.restype = ctypes.c_int
_lib.Xorcist_conceal.argtypes = [ctypes.c_void_p, _F32]
_lib.Xorcist_conceal.restype = ctypes.c_int
_lib.Xorcist_pass.argtypes = [ctypes.c_void_p, _F32, _F32]
_lib.Xorcist_replaceLast.argtypes = [ctypes.c_void_p, _F32]
_lib.Xorcist_peek.argtypes = [ctypes.c_void_p, _F32, ctypes.c_uint]
_lib.Xorcist_peek.restype = ctypes.c_uint
_lib.Xorcist_horizonFrames.argtypes = [ctypes.c_void_p]
_lib.Xorcist_horizonFrames.restype = ctypes.c_uint
_lib.Xorcist_version.restype = ctypes.c_char_p


class Tuning(ctypes.Structure):
    """XorcistTuning - the knobs the engine exposes in the Audio Monitor.

    seam: 0 adaptive (engine default), 1 force anchor, 2 force bridge.
    noTrend: 1 disables the level trend.
    anaHop: 0 keeps the mode's own hop (one audio period).
    freqDamping: 0 classic replace ... 1 fully frozen.
    order: 1 = the classic rotator, 2..N = per-bin Prony (own hop nfft/8).
    schur: 1 = stabilise via reflection coefficients instead of root
        clamping - finite O(p^2), no iteration, float-safe; NOT the same
        result as the selective clamp (moves every mode on a violated k).
    safetyCap: the shadow may not peak above this many times the loudest
        of the last 2048 history samples (default 1.2, scales rather than
        clips).  0 turns it off, < 0 keeps the default.
    """

    _fields_ = [("seam", ctypes.c_int), ("noTrend", ctypes.c_int),
                ("anaHop", ctypes.c_int), ("freqDamping", ctypes.c_float),
                ("order", ctypes.c_int), ("schur", ctypes.c_int),
                ("safetyCap", ctypes.c_float), ("gateFit", ctypes.c_int),
                ("staleWindow", ctypes.c_int),
                ("backfill", ctypes.c_int),
                ("fitWindow", ctypes.c_int),
                ("solveEvery", ctypes.c_int),
                ("burgEvery", ctypes.c_int),
                ("replica", ctypes.c_int), ("noFadeInRing", ctypes.c_int),
                ("capSpan", ctypes.c_int), ("reentryFade", ctypes.c_int),
                ("anchorTau", ctypes.c_int), ("rootClamp", ctypes.c_float),
                ("gpu", ctypes.c_int), ("detectRho", ctypes.c_float),
                ("patchTransients", ctypes.c_int),
                ("hybrid", ctypes.c_int),
                ("hybShortCap", ctypes.c_int),
                ("hybHandOrder", ctypes.c_int),
                ("hybReset", ctypes.c_int),
                ("hybAllBands", ctypes.c_int),
                ("hybChooser", ctypes.c_int),
                ("hybPre", ctypes.c_int),
                ("hybSimple", ctypes.c_int),
                ("hybWarmOrder", ctypes.c_int),
                # ANGEHAENGT - Reihenfolge muss Feld fuer Feld zu
                # struct XorcistTuning passen, sonst setzt setTuning still
                # die falschen Parameter.
                ("detResGate", ctypes.c_float),
                ("detResDecay", ctypes.c_float),
                # 1 = Naht-Anker aus Schatten[-1] (Hindcast) statt Burg-v0;
                # 0 = Vorgabe (Burg).  XO_V0_SHADOW setzt nur den Startwert.
                ("v0Hindcast", ctypes.c_int),
                # 1 = SOLVE-FILM (braucht gpu=1): der Kernel schreibt je
                # Bin Polradius vor dem Deckeln und genommenen Zweig mit,
                # je vollem Bin-Durchlauf ein Bild.  Kostet einen zweiten
                # Rueckweg je Kernelstart - Zeitmessungen damit sind
                # wertlos, das Ergebnis bleibt unveraendert.
                ("solveTap", ctypes.c_int)]


_lib.Xorcist_setTuning.argtypes = [ctypes.c_void_p, ctypes.POINTER(Tuning)]
_lib.Xorcist_getTuning.argtypes = [ctypes.c_void_p, ctypes.POINTER(Tuning)]

_lib.Xorcist_capStats.argtypes = [ctypes.POINTER(ctypes.c_uint64)] * 2
_lib.Xorcist_gateStats.argtypes = [ctypes.POINTER(ctypes.c_uint64)] * 2
_lib.Xorcist_arDropStats.argtypes = [ctypes.POINTER(ctypes.c_uint64)] * 2
_lib.Xorcist_backfillCount.restype = ctypes.c_uint64
_lib.Xorcist_hybLog.argtypes = [
    ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint]
_lib.Xorcist_hybLog.restype = ctypes.c_uint
_lib.Xorcist_hybLogClear.argtypes = []
_lib.Xorcist_hybLogClear.restype = None
_lib.Xorcist_hybStats.argtypes = [ctypes.POINTER(ctypes.c_uint64)] * 3
_lib.Xorcist_hybStats.restype = None
_lib.Xorcist_gpuStats.argtypes = [
    ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
_lib.Xorcist_gpuStats.restype = None
_lib.Xorcist_rhoStats.argtypes = [ctypes.POINTER(ctypes.c_uint64)] * 2
_lib.Xorcist_patchCount.restype = ctypes.c_uint64
_lib.Xorcist_patchLog.argtypes = [ctypes.POINTER(ctypes.c_uint64),
                                  ctypes.c_uint]
_lib.Xorcist_patchLog.restype = ctypes.c_uint
_lib.Xorcist_patchLogClear.argtypes = []
_lib.Xorcist_safetyCap.argtypes = [ctypes.c_void_p]
_lib.Xorcist_safetyCap.restype = ctypes.c_float

_lib.Xorcist_gpuDevice.restype = ctypes.c_char_p

_lib.Xorcist_solveTapInfo.argtypes = [ctypes.POINTER(ctypes.c_uint)] * 3 + [
    ctypes.POINTER(ctypes.c_uint64)]
_lib.Xorcist_solveTapInfo.restype = None
_lib.Xorcist_solveTapRead.argtypes = [
    ctypes.POINTER(ctypes.c_float)] * 3 + [
    ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint]
_lib.Xorcist_solveTapRead.restype = ctypes.c_uint
_lib.Xorcist_solveTapClear.argtypes = []
_lib.Xorcist_solveTapClear.restype = None
_lib.Xorcist_analysisGeometry.argtypes = [ctypes.c_void_p] + [
    ctypes.POINTER(ctypes.c_uint)] * 3
_lib.Xorcist_analysisGeometry.restype = None

version = _lib.Xorcist_version().decode()


def gpu_device():
    """Device name the solve would run on with gpu=1, or None."""
    d = _lib.Xorcist_gpuDevice()
    return d.decode() if d else None


def ar_drop_stats():
    """(drops, tries) - how often the AR path fell back to the order-1
    rotator because a seed was missing from the spectrum ring.  The ring
    is written only when a shadow is built, and no shadow is built during
    a miss streak, so a cluster tears a hole that later grid points fall
    into.  Reading RESETS."""
    d, t = ctypes.c_uint64(), ctypes.c_uint64()
    _lib.Xorcist_arDropStats(ctypes.byref(d), ctypes.byref(t))
    return d.value, t.value


def patch_log(max_events=512):
    """[(onset, zone_start, zone_len), ...] of recent transient patches,
    newest last - so a host can SHOW where the detector fired."""
    buf = (ctypes.c_uint64 * (3 * max_events))()
    n = _lib.Xorcist_patchLog(buf, max_events)
    return [(int(buf[3*i]), int(buf[3*i+1]), int(buf[3*i+2]))
            for i in range(n)]


def patch_log_clear():
    _lib.Xorcist_patchLogClear()


def patch_count():
    """Transient patches built since the last call (reading RESETS)."""
    return int(_lib.Xorcist_patchCount())


def hyb_log(max_events=16384):
    """Hybrid-Episoden als Liste (startSample, endSample, band, ordnung,
    nfft) - welche Baender wann mit der kurzen Kette gerechnet wurden."""
    buf = (ctypes.c_uint64 * (3 * max_events))()
    n = _lib.Xorcist_hybLog(buf, max_events)
    out = []
    for i in range(n):
        meta = buf[3 * i + 2]
        out.append((buf[3 * i], buf[3 * i + 1], meta & 0xFF,
                    (meta >> 16) & 0xFFFF, (meta >> 32) & 0xFFFFFFFF,
                    (meta >> 15) & 1))   # 1 = zurueckgezogen (Vorstufe)
    return out


def hyb_log_clear():
    _lib.Xorcist_hybLogClear()


def hyb_stats():
    """(aktiv, wirkungslos, Band-Perioden) des Hybrids seit dem letzten
    Aufruf.  Lesen SETZT ZURUECK.  wirkungslos > 0 heisst: Haken
    gesetzt, aber Basisrahmen 1k oder Puffer teilt 256 nicht."""
    a, b, c = (ctypes.c_uint64(), ctypes.c_uint64(), ctypes.c_uint64())
    _lib.Xorcist_hybStats(
        ctypes.byref(a), ctypes.byref(b), ctypes.byref(c))
    return a.value, b.value, c.value


def gpu_stats():
    """(auf dem Geraet, auf der CPU) geloeste Solve-Streifen seit dem
    letzten Aufruf.  Lesen SETZT ZURUECK.  0/N heisst: Haekchen gesetzt,
    Geraet ungenutzt."""
    a, b = ctypes.c_uint64(), ctypes.c_uint64()
    _lib.Xorcist_gpuStats(ctypes.byref(a), ctypes.byref(b))
    return a.value, b.value


# Zweig-Schluessel des Solve-Films, gleiche Zahlen wie die XO_TAP_*
# im Kernel.  -1 = der Bin wurde in diesem Durchlauf nie besucht.
TAP_PATHS = {-1: "nicht gerechnet", 0: "Gauss singulaer",
             1: "Detektor bestanden", 2: "Rotator (O1)",
             3: "Quadratisch (O2)", 4: "Durand-Kerner",
             5: "Schur", 6: "Wurzel nicht endlich"}


def solve_tap_info():
    """(abrufbar, Bins, Ordnung, insgesamt geschrieben).

    Das letzte Feld zaehlt ueber den Ring hinaus weiter - die Differenz
    zum vorigen Blick sagt WAEHREND eines Laufs, wie viele Bilder neu
    dazugekommen sind."""
    nf, nb, order = (ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint())
    total = ctypes.c_uint64()
    _lib.Xorcist_solveTapInfo(
        ctypes.byref(nf), ctypes.byref(nb), ctypes.byref(order),
        ctypes.byref(total))
    return nf.value, nb.value, order.value, total.value


def solve_tap(max_frames=None, roots=False):
    """Solve-Mitschrift: (z, path, pos) bzw. (z, path, pos, roots).

    z = groesster Polradius je Bin VOR dem Deckeln, -1 wo keine Wurzeln
    entstanden (Detektor bestanden, Gauss singulaer, Bin nie besucht) -
    deshalb nie blind mitteln, sondern gegen path maskieren.
    path = welchen Zweig der Kernel nahm, siehe TAP_PATHS.
    pos = Sampleposition des Bildes.
    roots = [Bilder, Bins, Ordnung, 2] Re/Im der Pole vor dem Deckeln,
    NAN wo der Zweig keine berechnet hat.  Nur auf Verlangen, weil es
    das Vielfache der Zusammenfassung ist.

    max_frames holt nur die JUENGSTEN n Bilder - so kostet ein Blick
    waehrend des Laufs nur die neu dazugekommenen, nicht den ganzen Ring.

    Leer, solange kein Lauf mit solveTap=1 UND gpu=1 gelaufen ist.
    """
    nf, nb, order, _total = solve_tap_info()
    if max_frames is not None:
        nf = min(nf, max(0, int(max_frames)))
    if not nf or not nb:
        empty = (np.zeros((0, 0), np.float32), np.zeros((0, 0), np.float32),
                 np.zeros(0, np.uint64))
        return empty + (np.zeros((0, 0, 0, 2), np.float32),) if roots else empty
    z = np.empty((nf, nb), np.float32)
    path = np.empty((nf, nb), np.float32)
    pos = np.empty(nf, np.uint64)
    rt = (np.empty((nf, nb, order, 2), np.float32) if roots else None)
    _f = ctypes.POINTER(ctypes.c_float)
    n = _lib.Xorcist_solveTapRead(
        z.ctypes.data_as(_f), path.ctypes.data_as(_f),
        rt.ctypes.data_as(_f) if roots else None,
        pos.ctypes.data_as(ctypes.POINTER(ctypes.c_uint64)), nf)
    if roots:
        return z[:n], path[:n], pos[:n], rt[:n]
    return z[:n], path[:n], pos[:n]


def solve_tap_clear():
    _lib.Xorcist_solveTapClear()


def rho_stats():
    """(fails, tests) of the rho-Schur growth detector since the last
    call - armed by XO_COUNT_RHO=<rho> in the environment.  Reading
    RESETS."""
    a, b = ctypes.c_uint64(), ctypes.c_uint64()
    _lib.Xorcist_rhoStats(ctypes.byref(a), ctypes.byref(b))
    return a.value, b.value


def gate_stats():
    """(skips, tests) of the AR fit gate since the last call - reading
    RESETS.  A skip rate near 100 % means the gate starved the fit
    rather than cleaned it, which is the failure mode to watch for at
    high concealment density."""
    sk, t = ctypes.c_uint64(), ctypes.c_uint64()
    _lib.Xorcist_gateStats(ctypes.byref(sk), ctypes.byref(t))
    return sk.value, t.value


def cap_stats():
    """(hits, calls) of the safety cap since the last call - reading
    RESETS, so a caller can bracket exactly one run.

    Under suspicion since 2026-08-06: the cap takes its peak over the
    WHOLE 32 ms shadow while only the played 128 samples are heard, and
    it ratchets (scaled samples enter the ring, so the next histPeak is
    smaller).  XO_SAFETY_CAP overrides the factor, 0 turns it off."""
    h, c = ctypes.c_uint64(), ctypes.c_uint64()
    _lib.Xorcist_capStats(ctypes.byref(h), ctypes.byref(c))
    return h.value, c.value





class Xorcist:
    """One instance = one output stream, exactly as in the engine."""

    def __init__(self, channels, frames_per_period, sample_rate,
                 mode=PV_4K, wisdom=None):
        self.h = _lib.Xorcist_new(channels, frames_per_period, sample_rate,
                                  wisdom.encode() if wisdom else None)
        if not self.h:
            raise RuntimeError("Xorcist_new failed")
        self.ch = channels
        self.buf = frames_per_period
        _lib.Xorcist_setMode(self.h, mode)

    def close(self):
        if getattr(self, "h", None):
            _lib.Xorcist_free(self.h)
            self.h = None

    __del__ = close

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def get_tuning(self):
        t = Tuning()
        _lib.Xorcist_getTuning(self.h, ctypes.byref(t))
        return t

    def set_tuning(self, **kw):
        """Change single knobs, leaving the rest at the engine default."""
        t = self.get_tuning()
        for k, v in kw.items():
            if not hasattr(t, k):
                raise AttributeError("no such tuning field: " + k)
            setattr(t, k, v)
        _lib.Xorcist_setTuning(self.h, ctypes.byref(t))
        return t

    def push(self, period):
        _lib.Xorcist_push(self.h, np.ascontiguousarray(period, np.float32))

    def push_many(self, periods):
        """periods: (n, buf*channels) or flat, contiguous."""
        a = np.ascontiguousarray(periods, np.float32).ravel()
        _lib.Xorcist_pushMany(self.h, a, a.size // (self.buf * self.ch))

    def predict(self):
        return bool(_lib.Xorcist_predict(self.h))

    def patch_force(self, ch, zone_start, length):
        """Build the backward-rotation patch over [zone_start,
        zone_start+length) absolute samples.  Bite-test entry."""
        return bool(_lib.Xorcist_patchForce(self.h, ch, zone_start, length))

    def patch_read(self, ch, max_len=4096):
        st = ctypes.c_uint64()
        out = np.zeros(max_len, np.float32)
        n = _lib.Xorcist_patchRead(self.h, ch, ctypes.byref(st), out, max_len)
        return int(st.value), out[:n]

    def patch_clear(self):
        _lib.Xorcist_patchClear(self.h)

    def absorb(self):
        """Analysis without synthesis: identical state to predict(),
        no shadow.  For offline runs where no miss can be near."""
        _lib.Xorcist_absorb(self.h)

    def analysis_geometry(self):
        """(nfft, anaHop, arHop) - was die Instanz WIRKLICH rechnet.

        anaHop ist der STFT-Sprung (meist eine Audio-Periode), arHop der
        Abstand der AR-Stuetzstellen - Letzterer heisst in den
        Strategienamen "hop"."""
        a, b, c = (ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint())
        _lib.Xorcist_analysisGeometry(
            self.h, ctypes.byref(a), ctypes.byref(b), ctypes.byref(c))
        return a.value, b.value, c.value

    def horizon_frames(self):
        """Wie weit der fertige Schatten reicht - so viele Frames darf
        peek() hoechstens liefern."""
        return _lib.Xorcist_horizonFrames(self.h)

    def peek(self, frames):
        """Der SCHATTEN: was der Praediktor spielen wuerde, wenn ab
        jetzt nichts mehr ankaeme.  Liest nur, veraendert den Zustand
        nicht - gedacht zum Zuschauen, nicht zum Abspielen."""
        out = np.zeros(frames * self.ch, np.float32)
        n = _lib.Xorcist_peek(self.h, out, frames)
        return out.reshape(-1, self.ch)[:n]

    def passthrough(self, real):
        """Real period arrived: apply the re-entry crossfade if the
        previous one was concealed.  Returns the audio to play."""
        r = np.ascontiguousarray(real, np.float32).ravel()
        out = np.zeros_like(r)
        _lib.Xorcist_pass(self.h, out, r)
        return out.reshape(-1, self.ch)

    def replace_last(self, real):
        """Overwrite the last ring append with this audio.

        The engine uses it for straggler truth; the lab uses it to
        feed a MULTI-INSTANCE prediction its own combined output,
        so each instance self-feeds what was actually played."""
        _lib.Xorcist_replaceLast(
            self.h, np.ascontiguousarray(real, np.float32).ravel())

    def conceal(self):
        out = np.zeros(self.buf * self.ch, np.float32)
        played = _lib.Xorcist_conceal(self.h, out)
        return out.reshape(-1, self.ch), bool(played)


def default_wisdom():
    """Where to cache the measured FFTW plans.

    Creating an instance plans three frame sizes with FFTW_MEASURE, which
    times real codelets and costs ~0.9 s the first time.  Persisting the
    result makes every later instance essentially free - without this, a
    tool that creates instances in a loop spends all its time planning
    (measured: 145 ms per call instead of 1)."""
    d = Path.home() / ".cache" / "xorcist"
    d.mkdir(parents=True, exist_ok=True)
    return str(d / "fftw-wisdomf.dat")


_pool = {}


def _instance(ch, buf, rate, mode, tuning=None):
    """Reuse instances: planning is per instance, the history ring is
    fully overwritten by the pushes anyway.

    Pooled instances keep their tuning, so every call restores the stock
    settings first and then applies `tuning` - otherwise one strategy's
    forced seam would silently leak into the next one's run.
    """
    key = (ch, buf, rate)
    x = _pool.get(key)
    if x is None:
        x = _pool[key] = Xorcist(ch, buf, rate, mode, default_wisdom())
        x.stock = x.get_tuning()
    _lib.Xorcist_setMode(x.h, mode)
    t = Tuning(x.stock.seam, x.stock.noTrend, x.stock.anaHop,
               x.stock.freqDamping, x.stock.order, x.stock.schur,
               x.stock.safetyCap, x.stock.gateFit,
               x.stock.staleWindow, x.stock.backfill,
               x.stock.fitWindow, x.stock.solveEvery,
               x.stock.burgEvery, x.stock.replica,
               x.stock.noFadeInRing, x.stock.capSpan,
               x.stock.reentryFade, x.stock.anchorTau,
               x.stock.rootClamp, x.stock.gpu,
               x.stock.detectRho, x.stock.patchTransients)
    for k, v in (tuning or {}).items():
        if not hasattr(t, k):
            raise AttributeError("no such tuning field: " + k)
        setattr(t, k, v)
    _lib.Xorcist_setTuning(x.h, ctypes.byref(t))
    return x


def predict_at(signal, pos, frames, buf=128, mode=PV_4K, sample_rate=48000,
               warm=2, history=None, tuning=None):
    """Return the prediction the engine would have played at `pos`.

    `signal` may be mono (1-D) or (n, channels).

    Only the last `warm` periods are actually predicted; the earlier ones
    are pushed into the history ring, which is a memcpy.  warm = 2 is the
    cheapest faithful setting: the engine's analysis hop is one period, so
    the second-to-last prediction is what leaves the previous frame in the
    spectrum cache.  The Burg coefficients come out FRESH this way, while
    a long-running engine refits only every 8 periods - deterministic and
    therefore better for A/B work, but see the note in gap_lab about
    bridge-dominated material.

    `history` caps how many periods are pushed (default: enough to fill
    the ring).  Pushing more changes nothing - the ring is finite.
    """
    sig = np.asarray(signal, np.float32)
    if sig.ndim == 1:
        sig = sig[:, None]
    ch = sig.shape[1]
    n_periods = pos // buf
    if history is None:
        history = 16384 // buf + warm + 2   # ring size + what predict needs
    first = max(0, n_periods - history)
    x = _instance(ch, buf, sample_rate, mode, tuning)
    bulk_end = n_periods - warm
    if bulk_end > first:                    # one call instead of hundreds
        x.push_many(sig[first * buf:bulk_end * buf])
    for p in range(max(first, bulk_end), n_periods):
        x.push(sig[p * buf:(p + 1) * buf].ravel())
        x.predict()
    return x.peek(frames)
