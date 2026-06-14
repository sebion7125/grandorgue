/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundReleaseAlignTable.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <map>
#include <utility>

#include "../GOCrossfadeParam.h"

#if __has_include("GOLogReleaseAlignEnable.h")
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>

static std::atomic<long long> s_TotalLoadUs{0};
static std::atomic<unsigned>  s_LoadCount{0};
static std::mutex             s_LoadLogMutex;

static std::string GetLoadTimingLogPath() {
#ifdef _WIN32
  const char *tmp = std::getenv("TEMP");
  if (!tmp) tmp = std::getenv("TMP");
  if (!tmp) tmp = "C:\\";
  return std::string(tmp) + "\\go_load_timing.log";
#else
  return "/tmp/go_load_timing.log";
#endif
}
#endif

#include "loader/cache/GOCache.h"
#include "loader/cache/GOCacheWriter.h"

#include "GOSoundAudioSection.h"

#ifndef NDEBUG
#ifdef PALIGN_DEBUG
#include <stdio.h>
#endif
#endif

GOSoundReleaseAlignTable::GOSoundReleaseAlignTable() {
  memset(m_PositionEntries, 0, sizeof(m_PositionEntries));
  m_PhaseAlignMaxAmplitude = 0;
  m_PhaseAlignMaxDerivative = 0;
  m_CorrPeriodSamples = 0;
  m_CorrPeriodFloat   = 0.0;
  m_CorrCrossfadeLen  = 0;
}

GOSoundReleaseAlignTable::~GOSoundReleaseAlignTable() {}

bool GOSoundReleaseAlignTable::Load(GOCache &cache) {
  // ── Legacy (unchanged) ──────────────────────────────────────────────────
  if (!cache.Read(&m_PhaseAlignMaxAmplitude, sizeof(m_PhaseAlignMaxAmplitude)))
    return false;
  if (!cache.Read(
        &m_PhaseAlignMaxDerivative, sizeof(m_PhaseAlignMaxDerivative)))
    return false;
  if (!cache.Read(&m_PositionEntries, sizeof(m_PositionEntries)))
    return false;

  // ── Sparse correlation LUTs (optional — graceful on old cache files) ──────
  // Format v3: n_luts (uint32), period+crossfade, per-LUT: count + {loop_pos,best_r}.
  // Format v4: n_luts | 0x80000000 → same layout but each point also has {flags,_pad}.
  //            best_r stored folded [0,T).
  // Format v5: n_luts | 0x80000000 | 0x40000000 → v4 layout, best_r unfolded [0,2T).
  //            Old v4 caches are loaded in legacy mode (flags cleared → T-modulus interp).
  uint32_t n_luts_raw = 0;
  if (!cache.Read(&n_luts_raw, sizeof(n_luts_raw)))
    return true; // EOF on old/v2 cache: not an error
  const bool has_flags    = (n_luts_raw & 0x80000000u) != 0;
  const bool has_unfolded = (n_luts_raw & 0x40000000u) != 0; // v5: best_r in [0,2T)
  const uint32_t n_luts   =  n_luts_raw & 0x3FFFFFFFu;
  if (n_luts > 0) {
    if (!cache.Read(&m_CorrPeriodSamples, sizeof(m_CorrPeriodSamples)))
      return false;
    if (!cache.Read(&m_CorrPeriodFloat, sizeof(m_CorrPeriodFloat)))
      return false;
    if (!cache.Read(&m_CorrCrossfadeLen, sizeof(m_CorrCrossfadeLen)))
      return false;
    m_CorrLuts.resize(n_luts);
    for (AttackLut &lut : m_CorrLuts) {
      lut.p_Attack = nullptr; // restored by AssignAttackPointers after load
      uint32_t n_points = 0;
      if (!cache.Read(&n_points, sizeof(n_points)))
        return false;
      lut.points.resize(n_points);
      for (CorrPoint &cp : lut.points) {
        if (!cache.Read(&cp.loop_pos, sizeof(cp.loop_pos)))
          return false;
        if (!cache.Read(&cp.best_r, sizeof(cp.best_r)))
          return false;
        if (has_flags) {
          if (!cache.Read(&cp.flags, sizeof(cp.flags))) return false;
          if (!cache.Read(&cp._pad,  sizeof(cp._pad)))  return false;
          // v4 cache has folded best_r [0,T); clear flags so GetPositionForCorrelation
          // uses legacy T-modulus path and the old values remain correct.
          if (!has_unfolded)
            cp.flags = 0;
        } else {
          cp.flags = 0;
          cp._pad  = 0;
        }
      }
    }
  }
  return true;
}

bool GOSoundReleaseAlignTable::Save(GOCacheWriter &cache) {
  // ── Legacy (unchanged) ──────────────────────────────────────────────────
  if (!cache.Write(&m_PhaseAlignMaxAmplitude, sizeof(m_PhaseAlignMaxAmplitude)))
    return false;
  if (!cache.Write(
        &m_PhaseAlignMaxDerivative, sizeof(m_PhaseAlignMaxDerivative)))
    return false;
  if (!cache.Write(&m_PositionEntries, sizeof(m_PositionEntries)))
    return false;

  // ── Sparse correlation LUTs (v5 format: 0x80000000=has_flags, 0x40000000=unfolded) ──
  const uint32_t n_luts_raw =
    (uint32_t)m_CorrLuts.size() | 0x80000000u | 0x40000000u; // v5: flags + unfolded
  if (!cache.Write(&n_luts_raw, sizeof(n_luts_raw)))
    return false;
  if (!m_CorrLuts.empty()) {
    if (!cache.Write(&m_CorrPeriodSamples, sizeof(m_CorrPeriodSamples)))
      return false;
    if (!cache.Write(&m_CorrPeriodFloat, sizeof(m_CorrPeriodFloat)))
      return false;
    if (!cache.Write(&m_CorrCrossfadeLen, sizeof(m_CorrCrossfadeLen)))
      return false;
    for (const AttackLut &lut : m_CorrLuts) {
      uint32_t n_points = (uint32_t)lut.points.size();
      if (!cache.Write(&n_points, sizeof(n_points)))
        return false;
      for (const CorrPoint &cp : lut.points) {
        if (!cache.Write(&cp.loop_pos, sizeof(cp.loop_pos))) return false;
        if (!cache.Write(&cp.best_r,   sizeof(cp.best_r)))   return false;
        if (!cache.Write(&cp.flags,    sizeof(cp.flags)))    return false;
        if (!cache.Write(&cp._pad,     sizeof(cp._pad)))     return false;
      }
    }
  }
  return true;
}

void GOSoundReleaseAlignTable::ComputeTable(
  const GOSoundAudioSection &release,
  int phase_align_max_amplitude,
  int phase_align_max_derivative,
  unsigned int sample_rate,
  unsigned start_position) {
  GOSoundCompressionCache cache;

  cache.Init();

  for (unsigned i = 0; i < PHASE_ALIGN_DERIVATIVES; i++)
    for (unsigned j = 0; j < PHASE_ALIGN_AMPLITUDES; j++)
      m_PositionEntries[i][j] = 0;

  unsigned channels = release.GetChannels();
  m_PhaseAlignMaxDerivative = phase_align_max_derivative;
  m_PhaseAlignMaxAmplitude = phase_align_max_amplitude;

  /* We will use a short portion of the release to analyse to get the
   * release offset table. This length is defined by the
   * PHASE_ALIGN_MIN_FREQUENCY macro and should be set to the lowest
   * frequency pipe you would ever expect... if this length is greater
   * than the length of the release, truncate it */
  unsigned required_search_len = sample_rate / PHASE_ALIGN_MIN_FREQUENCY;
  unsigned release_len = release.GetLength();
  if (release_len < required_search_len + BLOCK_HISTORY + start_position)
    return;
  /* If number of samples in the release is not enough to fill the release
   * table, abort - release alignment probably wont help. */
  if (
    required_search_len < PHASE_ALIGN_AMPLITUDES * PHASE_ALIGN_DERIVATIVES * 2)
    return;

  /* Generate the release table using the small portion of the release... */
  bool found[PHASE_ALIGN_DERIVATIVES][PHASE_ALIGN_AMPLITUDES];
  memset(found, 0, sizeof(found));

  int f_p = 0;
  for (unsigned int j = 0; j < channels; j++)
    f_p += release.GetSample(start_position + (BLOCK_HISTORY - 1), j, &cache);

  for (unsigned i = BLOCK_HISTORY; i < required_search_len; i++) {
    /* Store previous values */
    int f = 0;
    for (unsigned int j = 0; j < channels; j++)
      f += release.GetSample(start_position + i, j, &cache);

    /* Bring v into the range -1..2*m_PhaseAlignMaxDerivative-1 */
    int v_mod = (f - f_p) + m_PhaseAlignMaxDerivative - 1;
    int derivIndex
      = (PHASE_ALIGN_DERIVATIVES * v_mod) / (2 * m_PhaseAlignMaxDerivative);

    /* Bring f into the range -1..2*m_PhaseAlignMaxAmplitude-1 */
    int f_mod = f + m_PhaseAlignMaxAmplitude - 1;
    int ampIndex
      = (PHASE_ALIGN_AMPLITUDES * f_mod) / (2 * m_PhaseAlignMaxAmplitude);

    /* Store this release point if it was not already found */
    derivIndex = (derivIndex < 0)
      ? 0
      : (
        (derivIndex >= PHASE_ALIGN_DERIVATIVES) ? PHASE_ALIGN_DERIVATIVES - 1
                                                : derivIndex);
    ampIndex = (ampIndex < 0)
      ? 0
      : (
        (ampIndex >= PHASE_ALIGN_AMPLITUDES) ? PHASE_ALIGN_AMPLITUDES - 1
                                             : ampIndex);
    assert((derivIndex >= 0) && (derivIndex < PHASE_ALIGN_DERIVATIVES));
    assert((ampIndex >= 0) && (ampIndex < PHASE_ALIGN_AMPLITUDES));
    if (!found[derivIndex][ampIndex]) {
      m_PositionEntries[derivIndex][ampIndex] = i + 1 + start_position;
      found[derivIndex][ampIndex] = true;
    }

    f_p = f;
  }

#ifndef NDEBUG
#ifdef PALIGN_DEBUG
  /* print some phase debugging information */
  for (unsigned int i = 0; i < PHASE_ALIGN_DERIVATIVES; i++) {
    printf("deridx: %d\n", i);
    for (unsigned int j = 0; j < PHASE_ALIGN_AMPLITUDES; j++)
      if (found[i][j])
        printf("  idx %d: found\n", j);
      else
        printf("  idx %d: not found\n", j);
  }
#endif
#endif

  /* Phase 2, if there are any entries in the table which were not found,
   * attempt to fill them with the nearest available value. */
  for (int i = 0; i < PHASE_ALIGN_DERIVATIVES; i++)
    for (int j = 0; j < PHASE_ALIGN_AMPLITUDES; j++)
      if (!found[i][j]) {
        bool foundsecond = false;
        for (int l = 0; (l < 2 * PHASE_ALIGN_DERIVATIVES) && (!foundsecond);
             l++)
          for (int k = 0; (k < 2 * PHASE_ALIGN_AMPLITUDES) && (!foundsecond);
               k++) {
            foundsecond = true;
            int sl = (l + 1) / 2;
            if ((l & 1) == 0)
              sl = -sl;
            int sk = (k + 2) / 2;
            if ((k & 1) == 0)
              sk = -sk;
            if (
              (i + sl < PHASE_ALIGN_DERIVATIVES) && (i + sl >= 0)
              && (j + sk < PHASE_ALIGN_AMPLITUDES) && (j + sk >= 0)
              && (found[i + sl][j + sk])) {
              m_PositionEntries[i][j] = m_PositionEntries[i + sl][j + sk];
            } else {
              foundsecond = false;
            }
          }

        assert(foundsecond);
        foundsecond = false;
      }
}

static unsigned PeriodFromFrequency(float freq_hz, unsigned sample_rate) {
  if (freq_hz < 1.f)
    freq_hz = 1.f;
  return (unsigned)std::round((float)sample_rate / freq_hz);
}

// Normalized dot product between loop_window[0..window_len) and
// release[r..r+window_len). Returns value in [-1, +1].
// Used for autocorr (double-precision accumulation, keeps T_float accurate).
static float NormalizedDotProduct(
  const float *loop_window,
  const float *release,
  unsigned r,
  unsigned window_len) {
  double num = 0.0, e_loop = 0.0, e_rel = 0.0;
  for (unsigned i = 0; i < window_len; i++) {
    num    += (double)loop_window[i] * release[r + i];
    e_loop += (double)loop_window[i] * loop_window[i];
    e_rel  += (double)release[r + i]  * release[r + i];
  }
  double denom = std::sqrt(e_loop * e_rel);
  if (denom < 1e-12)
    return 0.f;
  return (float)(num / denom);
}

// Float32 NDP with pre-normalized loop window (||lw_n|| = 1).
// Banker's rounding (round-half-to-even) — matches Python's np.round / round().
// C++ std::round rounds half away from zero, which diverges from Python's reference
// at x == N.5 and causes exp_pos to differ by 1 downsampled unit (= ds original samples).
static int RoundHalfEven(float x) {
  float f = std::floor(x);
  float frac = x - f;
  if (frac < 0.5f) return (int)f;
  if (frac > 0.5f) return (int)f + 1;
  int i = (int)f;
  return (i & 1) ? i + 1 : i;
}

// In logging builds, prevent inlining of scalar float32 helpers so the
// #pragma GCC optimize / #pragma clang loop directives are always honoured.
// Without noinline, GCC may inline NDP_f32 / CalcLwN_f32 into their callers
// and re-optimise the inner loops under the caller's -O3 settings
// (vectorisation, FMA), causing 1-ULP differences vs Numba's scalar output.
#if __has_include("GOLogReleaseAlignEnable.h")
#  if defined(_MSC_VER)
#    define LOG_NOINLINE __declspec(noinline)
#  elif defined(__GNUC__)
#    define LOG_NOINLINE __attribute__((noinline))
#  else
#    define LOG_NOINLINE
#  endif
#else
#  define LOG_NOINLINE
#endif

// Matches Python: dot(rel_window, lw_n) / norm(rel_window)
// Silent release window (denom < 1e-12) → -2.f, matching Python's np.where(ok, ..., -2.0).
// Used in FullScanV2 / TrackStepV2 to align with Python's float32 computation.
//
// In verify/logging builds (GOLogReleaseAlignEnable.h present): auto-vectorization and
// FMA contraction are disabled so the float32 reduction is byte-identical to Numba's
// sequential scalar loop (the Python reference).  Production builds use full -O3.
#if __has_include("GOLogReleaseAlignEnable.h")
#  if defined(_MSC_VER)
#    pragma float_control(push)
#    pragma float_control(precise, on)
#  elif defined(__GNUC__) && !defined(__clang__)
#    pragma GCC push_options
#    pragma GCC optimize("no-fast-math,no-tree-vectorize,fp-contract=off")
#  endif
#endif
static LOG_NOINLINE float NDP_f32(
  const float *lw_n,
  const float *rel,
  unsigned r,
  unsigned W) {
#if __has_include("GOLogReleaseAlignEnable.h")
  // volatile on both the accumulator AND the product prevents FMA:
  // GCC can fuse "acc = acc + a*b" into a single VFMADD even with volatile acc.
  // Making the product volatile forces a float32 store+reload before the add,
  // so mul and add are always separate rounded operations — identical to Numba.
  volatile float num = 0.f, er = 0.f;
  for (unsigned i = 0; i < W; i++) {
    volatile float t_n = lw_n[i] * rel[r + i];
    volatile float t_e = rel[r + i] * rel[r + i];
    num = num + (float)t_n;
    er  = er  + (float)t_e;
  }
  float denom = std::sqrt((float)er);
  return ((float)denom < 1e-12f) ? -2.f : (float)num / denom;
#else
#  if defined(__clang__)
#    pragma clang fp contract(off)
#  endif
  float num = 0.f, er = 0.f;
#  if defined(__clang__)
#    pragma clang loop vectorize(disable) interleave(disable)
#  endif
  for (unsigned i = 0; i < W; i++) {
    num += lw_n[i] * rel[r + i];
    er  += rel[r + i] * rel[r + i];
  }
  float denom = std::sqrt(er);
  return (denom < 1e-12f) ? -2.f : num / denom;
#endif
}
#if __has_include("GOLogReleaseAlignEnable.h")
#  if defined(_MSC_VER)
#    pragma float_control(pop)
#  elif defined(__GNUC__) && !defined(__clang__)
#    pragma GCC pop_options
#  endif
#endif

// Compute normalized loop window in float32. Must carry the same pragma as
// NDP_f32: GCC -O3 vectorizes the e_lw dot-product with a different
// summation tree than Numba's scalar loop, giving a different inv_lw, which
// then propagates into every NDP score at that tracking step.
// Matches Python _normalize_lw_f32_numba (scalar, fastmath=False).
// Returns false when ||lw||^2 < 1e-24 (silent window).
#if __has_include("GOLogReleaseAlignEnable.h")
#  if defined(_MSC_VER)
#    pragma float_control(push)
#    pragma float_control(precise, on)
#  elif defined(__GNUC__) && !defined(__clang__)
#    pragma GCC push_options
#    pragma GCC optimize("no-fast-math,no-tree-vectorize,fp-contract=off")
#  endif
#endif
static LOG_NOINLINE bool CalcLwN_f32(
  const float *lw, unsigned W, float *lw_n) {
#if __has_include("GOLogReleaseAlignEnable.h")
  // volatile on both accumulator and product prevents GCC FMA (see NDP_f32).
  volatile float e_lw = 0.f;
  for (unsigned i = 0; i < W; i++) {
    volatile float sq = lw[i] * lw[i];
    e_lw = e_lw + (float)sq;
  }
  if ((float)e_lw < 1e-24f) return false;
  const float inv_lw = 1.f / std::sqrt((float)e_lw);
  for (unsigned i = 0; i < W; i++) lw_n[i] = lw[i] * inv_lw;
  return true;
#else
#  if defined(__clang__)
#    pragma clang fp contract(off)
#  endif
  float e_lw = 0.f;
#  if defined(__clang__)
#    pragma clang loop vectorize(disable) interleave(disable)
#  endif
  for (unsigned i = 0; i < W; i++) e_lw += lw[i] * lw[i];
  if (e_lw < 1e-24f) return false;
  const float inv_lw = 1.f / std::sqrt(e_lw);
#  if defined(__clang__)
#    pragma clang loop vectorize(disable) interleave(disable)
#  endif
  for (unsigned i = 0; i < W; i++) lw_n[i] = lw[i] * inv_lw;
  return true;
#endif
}
#if __has_include("GOLogReleaseAlignEnable.h")
#  if defined(_MSC_VER)
#    pragma float_control(pop)
#  elif defined(__GNUC__) && !defined(__clang__)
#    pragma GCC pop_options
#  endif
#endif

// Estimates the true fundamental period via autocorrelation on a stable
// Parabolic sub-sample refinement of an NDP peak.
// Matches Python's refine_peak_parabolic: delta = 0.5*(y[i-1]-y[i+1])/(y[i-1]-2y[i]+y[i+1]).
static double RefineNDPPeak(const float *ndp, unsigned idx, unsigned n) {
  if (idx == 0 || idx + 1 >= n)
    return (double)idx;
  float ym1 = ndp[idx - 1], y0 = ndp[idx], yp1 = ndp[idx + 1];
  float denom = ym1 - 2.f * y0 + yp1;
  if (std::abs(denom) < 1e-12f)
    return (double)idx;
  float delta = 0.5f * (ym1 - yp1) / denom;
  if (delta >  1.f) delta =  1.f;
  if (delta < -1.f) delta = -1.f;
  return (double)idx + delta;
}

// excerpt of the loop sample. Returns best lag in [min_period, max_period]
// as a double for sub-sample precision (matches Python refine_peak_parabolic).
//
// Full 1:1 port of analyze_lut.py estimate_period_by_autocorr():
//   1. NDP for all lags 1..max_period (ndp_all[lag], index 0 unused)
//   2. CMNDF cumsum from lag 1 with absolute lag as multiplier (correct YIN)
//   3. Local-minimum valley search in [min_period, max_period]
//   4. Later much-deeper valley may replace first (no early break)
//   5. Submultiple guard using expected_period (T_hn from call site)
//   6. Parabolic refinement on ndp_all
static double EstimatePeriodByAutocorr(
  const float *samples,
  unsigned     len,
  unsigned     min_period,
  unsigned     max_period,
  double       expected_period)  // HN-corrected nominal period (T_hn)
{
  if (len < max_period * 2)
    return min_period;

  // DC removal + Hann window.
  float mean = 0.f;
  for (unsigned i = 0; i < len; i++) mean += samples[i];
  mean /= (float)len;
  std::vector<float> windowed(len);
  for (unsigned i = 0; i < len; i++) {
    const float w = 0.5f * (1.f - std::cos(2.f * (float)M_PI * i / (len - 1)));
    windowed[i] = (samples[i] - mean) * w;
  }
  const float   *s = windowed.data();
  const unsigned W = len - max_period;

  // NDP for all lags 1..max_period.  ndp_all[lag] == NDP at that lag; index 0 unused.
  std::vector<float> ndp_all(max_period + 1, 0.f);
  for (unsigned lag = 1; lag <= max_period; lag++)
    ndp_all[lag] = NormalizedDotProduct(s, s, lag, W);

  // CMNDF from lag 1 with absolute lag multiplier (correct YIN formula).
  std::vector<float> cmndf_all(max_period + 1, 1.f);
  double cumsum = 0.0;
  for (unsigned lag = 1; lag <= max_period; lag++) {
    const double d = 1.0 - (double)ndp_all[lag];
    cumsum += d;
    cmndf_all[lag] = (cumsum > 0.0) ? (float)(d * lag / cumsum) : 1.f;
  }

  static constexpr float YIN_THRESHOLD = 0.15f;

  // Collect local-minimum valleys in (min_period, max_period) — exclusive endpoints.
  struct Valley { float cmndf; unsigned lag; float ndp; };
  std::vector<Valley> valleys;
  for (unsigned lag = min_period + 1; lag < max_period; lag++) {
    const float c = cmndf_all[lag];
    if (c < YIN_THRESHOLD && c <= cmndf_all[lag - 1] && c <= cmndf_all[lag + 1])
      valleys.push_back({c, lag, ndp_all[lag]});
  }

  // Fallback: first continuous valley including endpoints (matches Python).
  if (valleys.empty()) {
    bool in_v = false; float best_c = 2.f; unsigned v_lag = 0;
    for (unsigned lag = min_period; lag <= max_period; lag++) {
      const float c = cmndf_all[lag];
      if (c < YIN_THRESHOLD) {
        if (!in_v || c < best_c) { best_c = c; v_lag = lag; in_v = true; }
        else break;
      } else if (in_v) break;
    }
    if (in_v) valleys.push_back({best_c, v_lag, ndp_all[v_lag]});
  }

  // ndp_all[1..] passed as base so RefineNDPPeak index 0 == lag 1.
  const float *ndp_base = ndp_all.data() + 1;

  double result;
  if (!valleys.empty()) {
    Valley chosen = valleys[0];
    for (size_t k = 1; k < valleys.size(); k++) {
      const Valley &v = valleys[k];
      const bool much_deeper     = v.cmndf <= std::max(0.050f, chosen.cmndf * 0.35f);
      const bool much_better_ndp = v.ndp   >= chosen.ndp + 0.03f;
      bool near_int_mul = false;
      if (chosen.lag > 0) {
        const float ratio   = (float)v.lag / (float)chosen.lag;
        const int   nearest = (int)std::round(ratio);
        near_int_mul = nearest >= 2 && std::abs(ratio - (float)nearest) < 0.18f;
      }
      if (much_deeper && (much_better_ndp || near_int_mul))
        chosen = v;  // no break — iterate all candidates
    }
    result = 1.0 + RefineNDPPeak(ndp_base, chosen.lag - 1, max_period);
  } else {
    // Fallback: penalized NDP over search range.
    float best_score = -2.f; unsigned best_lag = min_period;
    for (unsigned lag = min_period; lag <= max_period; lag++) {
      const float sc = ndp_all[lag] * (1.f - 0.10f * (float)lag / (float)max_period);
      if (sc > best_score) { best_score = sc; best_lag = lag; }
    }
    result = 1.0 + RefineNDPPeak(ndp_base, best_lag - 1, max_period);
  }

  // Submultiple guard: if result ≈ expected_period/k (k=2..8) and expected_period
  // shows good periodicity, override to expected_period (port of Python v75/v76).
  if (expected_period > 0.0) {
    const int exp_i = (int)std::round(expected_period);
    const int res_i = (int)std::round(result);
    if (exp_i >= 1 && exp_i <= (int)max_period && res_i >= 1 && res_i <= (int)max_period) {
      const double ratio = expected_period / result;
      const int    k     = (int)std::round(ratio);
      if (k >= 2 && k <= 8 && std::abs(ratio - (double)k) <= 0.18) {
        const float cm_res  = cmndf_all[res_i];
        const float cm_exp  = cmndf_all[exp_i];
        const float ndp_res = ndp_all[res_i];
        const float ndp_exp = ndp_all[exp_i];

        const bool exp_good           = cm_exp <= 0.03f || ndp_exp >= 0.96f;
        const bool res_suspicious     = result < expected_period * 0.70;
        const bool exp_not_worse      = cm_exp <= cm_res * 1.25f || ndp_exp >= ndp_res - 0.03f;
        const bool exp_clearly_better = cm_exp <  cm_res * 0.50f || ndp_exp >  ndp_res + 0.03f;

        if (exp_good && res_suspicious &&
            (exp_not_worse || exp_clearly_better || (cm_exp <= 0.05f && ndp_exp >= 0.90f)))
          result = 1.0 + RefineNDPPeak(ndp_base, (unsigned)exp_i - 1, max_period);
      }
    }
  }

  return result;
}

// ─── v2 Backward-Tracking Algorithm ──────────────────────────────────────────

static constexpr unsigned V2_TOP_K            = 24;
static constexpr unsigned V2_DN_DENSE         = 2;
static constexpr unsigned V2_DENSE_N_LIMIT    = 35;
static constexpr unsigned V2_DN_SPARSE        = 6;
static constexpr float    V2_RESCAN_RATIO     = 0.90f;
static constexpr float    V2_RESCAN_DRIFT     = 8.0f;  // original samples
// Epsilon for deterministic beam tiebreaking: if two beams differ in cumulative
static constexpr unsigned V2_JUMP_RATE_FACTOR = 16;    // T/16 per period
static constexpr unsigned V2_MAX_PRUNE_GAP_N  = 50;
static constexpr float    V2_PRUNE_TOL_FACTOR = 32.0f; // T/32 Douglas-Peucker

struct BeamState {
  int   r_d;        // current position in downsampled units
  float score;      // NDP score at this step
  int   r_d_prev;   // position at previous step (for slope)
  int   dn_prev;    // step count used at previous step
  float cum_score;  // cumulative sum of scores (used to pick winner)
  int   beam_id;    // stable identifier assigned in Phase 0
};

// Full NDP scan → top-K local maxima as initial beam states.
// Scans [0, r_max_d). Returns at most top_k beams in descending score order.
static std::vector<BeamState> FullScanV2(
  const float *loop_d, unsigned loop_d_len,
  const float *rel_d,  unsigned rel_d_len,
  unsigned cs_d, unsigned window_d,
  unsigned r_max_d, unsigned /*T_int_d*/,
  unsigned top_k) {
  if (cs_d + window_d > loop_d_len || r_max_d == 0)
    return {};
  const float *lw = loop_d + cs_d;
  // Pre-normalize loop window — use CalcLwN_f32 (pragma-protected) so the
  // e_lw dot-product is scalar float32, matching Python _normalize_lw_f32_numba.
  std::vector<float> lw_n(window_d);
  if (!CalcLwN_f32(lw, window_d, lw_n.data()))
    return {};

  // Score for each candidate position (float32, matches Python _compute_corr_scores).
  const unsigned n = std::min(r_max_d, rel_d_len > window_d ? rel_d_len - window_d : 0u);
  std::vector<float> sc(n, -2.f);
  for (unsigned r = 0; r < n; r++)
    sc[r] = NDP_f32(lw_n.data(), rel_d, r, window_d);

  // Local maxima with non-max suppression (min distance = 3 downsampled samples,
  // matching Python BRANCH_MIN_PEAK_DISTANCE=3).
  static constexpr unsigned MIN_PEAK_DIST = 3u;
  std::vector<unsigned> peaks;
  peaks.reserve(64);
  for (unsigned i = 0; i < n; i++) {
    bool left_ok  = (i == 0)     || (sc[i] >= sc[i - 1]);
    bool right_ok = (i + 1 >= n) || (sc[i] >= sc[i + 1]);
    if (left_ok && right_ok)
      peaks.push_back(i);
  }
  std::stable_sort(peaks.begin(), peaks.end(),
                   [&](unsigned a, unsigned b) { return sc[a] > sc[b]; });

  const float best_sc   = peaks.empty() ? -2.f : sc[peaks[0]];
  const float sc_cutoff = best_sc - 0.40f; // BRANCH_SCORE_MARGIN

  std::vector<BeamState> result;
  result.reserve(top_k);
  for (unsigned pk : peaks) {
    if ((unsigned)result.size() >= top_k)
      break;
    if (sc[pk] < sc_cutoff && !result.empty())
      continue;
    bool too_close = false;
    for (const BeamState &b : result) {
      if ((unsigned)std::abs((int)pk - b.r_d) < MIN_PEAK_DIST) {
        too_close = true;
        break;
      }
    }
    if (!too_close) {
      float s = sc[pk];
      result.push_back({(int)pk, s, (int)pk, 1, s, (int)result.size()});
    }
  }
  if (result.empty() && n > 0) {
    // Fallback: global argmax.
    unsigned best = 0;
    for (unsigned r = 1; r < n; r++)
      if (sc[r] > sc[best]) best = r;
    float s = sc[best];
    result.push_back({(int)best, s, (int)best, 1, s, 0});
  }
  return result;
}

struct TrackResult {
  std::vector<BeamState> beams;
  bool needs_rescan;
};

// One backward step of the multi-beam tracker.
// exp_pos per beam = round(r_d + slope * dn) % sp_T_d
// Tests offsets -1, 0, +1; extends to ±2 if best was at edge.
static TrackResult TrackStepV2(
  const float *loop_d, unsigned loop_d_len,
  const float *rel_d,  unsigned rel_d_len,
  unsigned cs_d, unsigned window_d,
  unsigned sp_T_d,
  unsigned T_int_d,
  const std::vector<BeamState> &cands,
  unsigned dn,
  float rescan_ratio, float rescan_drift_orig, unsigned ds,
  unsigned top_k) {
  if (cs_d + window_d > loop_d_len || cands.empty())
    return {cands, false};
  const float *lw = loop_d + cs_d;
  // Pre-normalize loop window — use CalcLwN_f32 (pragma-protected) so the
  // e_lw dot-product is scalar float32, matching Python _normalize_lw_f32_numba.
  std::vector<float> lw_n(window_d);
  if (!CalcLwN_f32(lw, window_d, lw_n.data()))
    return {cands, false};

  const unsigned min_dist_d = std::max(1u, T_int_d / 32u);
  const int      sT         = (int)sp_T_d;
  bool needs_rescan = false;

  std::vector<BeamState> new_beams;
  new_beams.reserve(cands.size());

  auto ndp_at = [&](int r_d) -> float {
    if (r_d < 0 || (unsigned)r_d + window_d > rel_d_len)
      return -2.f;
    return NDP_f32(lw_n.data(), rel_d, (unsigned)r_d, window_d);
  };

  for (const BeamState &b : cands) {
    float slope   = (b.dn_prev > 0)
                    ? (float)(b.r_d - b.r_d_prev) / (float)b.dn_prev
                    : 0.f;
    int exp_pos   = RoundHalfEven((float)b.r_d + slope * (float)dn);
    exp_pos       = ((exp_pos % sT) + sT) % sT;

    // Round 1: offsets -1, 0, +1
    int   best_r  = exp_pos;
    float best_sc = -2.f;
    int   best_off = 0;
    for (int off = -1; off <= 1; off++) {
      int r = ((exp_pos + off) % sT + sT) % sT;
      float s = ndp_at(r);
      if (s > best_sc) { best_sc = s; best_r = r; best_off = off; }
    }

    // Round 2: if at edge, probe one further sample in the same direction.
    if (best_off != 0) {
      int r2 = ((exp_pos + 2 * best_off) % sT + sT) % sT;
      float s2 = ndp_at(r2);
      if (s2 > best_sc) { best_sc = s2; best_r = r2; }
    }

    // Rescan criteria: score drop or large drift.
    int drift_d = std::abs(best_r - exp_pos);
    if (drift_d > sT / 2) drift_d = sT - drift_d;
    const float prev_eff = std::max(0.05f, b.score);
    if (best_sc < rescan_ratio * prev_eff
        || (float)drift_d * (float)ds > rescan_drift_orig)
      needs_rescan = true;

    new_beams.push_back(
      {best_r, best_sc, b.r_d, (int)dn, b.cum_score + best_sc, b.beam_id});
  }

  // Sort by cumulative score descending; exact float32 ties broken by
  // beam_id ascending (lower = higher Phase-0 NDP peak).
  // EPS-based grouping is intentionally avoided: GO and Python accumulate
  // slightly different float32 values (scalar vs. vectorised NDP), so any
  // EPS > 0 creates a boundary that the two implementations straddle
  // differently, introducing new mismatches.
  std::sort(new_beams.begin(), new_beams.end(),
            [](const BeamState &a, const BeamState &b) {
              if (a.cum_score != b.cum_score)
                return a.cum_score > b.cum_score;
              return a.beam_id < b.beam_id;
            });

  // Spatial deduplication: keep at most top_k beams with min distance.
  std::vector<BeamState> filtered;
  filtered.reserve(top_k);
  for (const BeamState &b : new_beams) {
    if (filtered.size() >= top_k)
      break;
    bool too_close = false;
    for (const BeamState &f : filtered) {
      int dist = std::abs(b.r_d - f.r_d);
      if (dist > sT / 2) dist = sT - dist;
      if ((unsigned)dist < min_dist_d) { too_close = true; break; }
    }
    if (!too_close)
      filtered.push_back(b);
  }
  return {std::move(filtered), needs_rescan};
}

// Douglas-Peucker pruning on v2 track points.
// tolerance = max(2, T/V2_PRUNE_TOL_FACTOR).
// Preserves jump points and predecessors of jump points.
// Points are sorted by n ascending; r values are unfolded (may be ≥ T).
struct V2TrackPt {
  unsigned n;         // period index
  unsigned loop_pos;  // absolute sample position (= round(n * T_f))
  int      r;         // unfolded release offset in original samples
  float    score;
  bool     is_jump;
  bool     approach_up;
};

static std::vector<V2TrackPt> PruneV2(
  std::vector<V2TrackPt> pts, float T_int_f) {
  const unsigned sz = (unsigned)pts.size();
  if (sz <= 2)
    return pts;
  const float tol = std::max(2.0f, T_int_f / V2_PRUNE_TOL_FACTOR);

  std::vector<bool> active(sz, true);
  bool changed = true;
  while (changed) {
    changed = false;
    // Build list of currently active indices.
    std::vector<unsigned> idx;
    idx.reserve(sz);
    for (unsigned i = 0; i < sz; i++)
      if (active[i]) idx.push_back(i);

    for (unsigned k = 1; k + 1 < (unsigned)idx.size(); k++) {
      const unsigned cur  = idx[k];
      const unsigned prev = idx[k - 1];
      const unsigned next = idx[k + 1];

      // Never prune jump points or predecessors of jump points.
      if (pts[cur].is_jump)           continue;
      if (pts[next].is_jump)          continue;
      // Skip if neighbour gap is too wide.
      if (pts[next].n - pts[prev].n > V2_MAX_PRUNE_GAP_N) continue;
      // Skip if r-discontinuity > T/4 at either side.
      if (std::abs(pts[cur].r  - pts[prev].r) > (int)(T_int_f / 4.0f)) continue;
      if (std::abs(pts[next].r - pts[cur].r)  > (int)(T_int_f / 4.0f)) continue;

      // Douglas-Peucker: ALL original points in (prev, next) must fit within tol.
      const float n0 = (float)pts[prev].n, r0 = (float)pts[prev].r;
      const float n1 = (float)pts[next].n, r1 = (float)pts[next].r;
      const float dn = std::max(1.0f, n1 - n0);
      bool can_del = true;
      for (unsigned j = prev + 1; j < next; j++) {
        const float t  = (float)(pts[j].n - pts[prev].n) / dn;
        const float er = r0 + t * (r1 - r0);
        if (std::abs((float)pts[j].r - er) > tol) { can_del = false; break; }
      }
      if (can_del) {
        active[cur] = false;
        changed     = true;
      }
    }
  }

  std::vector<V2TrackPt> out;
  out.reserve(sz);
  for (unsigned i = 0; i < sz; i++)
    if (active[i]) out.push_back(pts[i]);
  return out;
}

// Kanonische LUT-Rohinterpolation in [0, round(2*T_f)).
// Nur v2-Logik: direkter linearer Weg, kein Wrap.
// Entspricht Python interp_lut_segment_raw(lut_folded=False).
static unsigned InterpolateCorrPointRaw(
  int loop_pos,
  const std::vector<GOSoundReleaseAlignTable::CorrPoint> &pts,
  double T_f)
{
  if (pts.empty()) return 0;
  const int W = (int)std::round(2.0 * T_f);

  if (loop_pos <= (int)pts.front().loop_pos) return (unsigned)(pts.front().best_r % W);
  if (loop_pos >= (int)pts.back().loop_pos)  return (unsigned)(pts.back().best_r  % W);

  size_t idx = 0;
  while (idx + 1 < pts.size() && (int)pts[idx + 1].loop_pos <= loop_pos)
    ++idx;
  const GOSoundReleaseAlignTable::CorrPoint &p0 = pts[idx];
  const GOSoundReleaseAlignTable::CorrPoint &p1 = pts[idx + 1];
  const double t = (loop_pos - (int)p0.loop_pos)
                 / (double)std::max(1, (int)(p1.loop_pos - p0.loop_pos));

  if (p1.IsJump())
    return (t < 0.5) ? (unsigned)(p0.best_r % W) : (unsigned)(p1.best_r % W);

  const int r0 = (int)p0.best_r % W;
  const int r1 = (int)p1.best_r % W;
  int r_result = (int)std::round(r0 + t * (r1 - r0));
  r_result = std::max(0, std::min(W - 1, r_result));
  return (unsigned)r_result;
}

// Vollständige Interpolation für CSV-Logger und GetPositionForCorrelation:
// InterpolateCorrPointRaw() → r_lut, dann phi addieren und final wrappen.
static unsigned GetPositionForCorrImpl(
  unsigned loop_pos,
  const std::vector<GOSoundReleaseAlignTable::CorrPoint> &pts,
  double   T_float,
  unsigned T_int)
{
  if (pts.empty() || T_int == 0)
    return 0;
  const double T_f   = (T_float > 0.0) ? T_float : (double)T_int;
  const unsigned phi = (unsigned)std::round(std::fmod((double)loop_pos, T_f)) % T_int;
  const unsigned r_lut = InterpolateCorrPointRaw((int)loop_pos, pts, T_f);
  const bool is_v2 = pts.back().IsValid();
  return (unsigned)std::round(
    std::fmod((double)(r_lut + phi), is_v2 ? 2.0 * T_f : T_f));
}

void GOSoundReleaseAlignTable::ComputeCorrelationLut(
  const GOSoundAudioSection &loop_section,
  const GOSoundAudioSection &release_section,
  unsigned crossfade_len,
  unsigned sample_rate,
  float    sample_freq_hz,
  unsigned harmonic_number,
  unsigned min_key_press_ms,
  unsigned max_key_press_ms,
  bool     permissive,
  bool     exhaustive
#if __has_include("GOLogReleaseAlignEnable.h")
  , const char *label
#endif
  ) {
  if (exhaustive) permissive = true; // exhaustive implies permissive
#if __has_include("GOLogReleaseAlignEnable.h")
  const auto t0 = std::chrono::high_resolution_clock::now();
#endif
  // On the first call: initialise shared parameters.
  // On subsequent calls (additional attack variants): T must match.
  const bool first_call = m_CorrLuts.empty();
  if (first_call) {
    m_CorrPeriodSamples = 0;
    m_CorrCrossfadeLen  = crossfade_len;
  }

  if (crossfade_len < 2)
    return;

  if (first_call) {
    m_CorrPeriodFloat   = (double)sample_rate / sample_freq_hz;
    m_CorrPeriodSamples = (unsigned)std::round(m_CorrPeriodFloat);
  }

  // Period estimation via autocorrelation.
  //
  // Search range [T_hn/2, 2*T_hn] centred on the HN-corrected working period
  // T_hn = T_smpl * harmonic_number / 8.  This covers octave ambiguity (T/2
  // and 2T peaks) and moderate detuning, matching the Python analyze_pipe
  // behaviour exactly.  Read from the middle of the sustain loop region for
  // a stable, oscillatory signal.
  if (first_call) {
    const unsigned loop_len_full = loop_section.GetLength();
    const unsigned T_smpl        = m_CorrPeriodSamples;
    // HN-corrected working period (same formula as Python hn_T_float).
    const unsigned T_hn = (unsigned)std::round(
      (double)T_smpl * harmonic_number / 8.0);
    const unsigned min_p = std::max(16u, T_hn / 2u);
    const unsigned max_p = std::min(sample_rate / 20u, T_hn * 2u);

    if (max_p >= min_p * 2 && T_hn >= 16u) {
      // Read from the middle of the sustain loop region (matches Python
      // loop_mid = loop_start + loop_len // 2).
      const unsigned loop_start  = loop_section.GetLoopStart();
      const unsigned loop_len    = loop_len_full - loop_start;
      unsigned ac_offset = loop_start + loop_len / 2;
      unsigned ac_window = 8 * max_p;
      if (ac_offset + ac_window > loop_len_full)
        ac_offset = loop_len_full > ac_window ? loop_len_full - ac_window : 0;
      ac_window = std::min(ac_window, loop_len_full - ac_offset);

      if (ac_window >= max_p * 2) {
        GOSoundCompressionCache ac_cache;
        ac_cache.Init();
        const unsigned loop_ch = loop_section.GetChannels();
        std::vector<float> ac_mono(ac_window);
        for (unsigned i = 0; i < ac_window; i++) {
          double s = 0.0;
          for (unsigned c = 0; c < loop_ch; c++)
            s += loop_section.GetSample(ac_offset + i, c, &ac_cache);
          ac_mono[i] = (float)(s / loop_ch);
        }
        const double refined_T = EstimatePeriodByAutocorr(
          ac_mono.data(), ac_window, min_p, max_p, (double)T_hn);
        m_CorrPeriodSamples = (unsigned)std::round(refined_T);
        m_CorrPeriodFloat   = refined_T;
      }
    }
  }

  // T < 16 samples: correlation window too small in any mode.
  // High-pitched stops (1', 2' upper registers) fall back to legacy alignment;
  // release transients there are inaudible anyway.
  if (m_CorrPeriodSamples < 16)
    return;

  const double T_f = (m_CorrPeriodFloat > 0.0) ? m_CorrPeriodFloat
                                                : (double)m_CorrPeriodSamples;

  unsigned window_len = crossfade_len;
  if (window_len < 4)
    return;

  const unsigned loop_len    = loop_section.GetLength();
  const unsigned release_len = release_section.GetLength();
  const unsigned loop_ch     = loop_section.GetChannels();
  const unsigned rel_ch      = release_section.GetChannels();

  if (loop_len < window_len || release_len < window_len)
    return;

  unsigned r_max = std::min(2 * m_CorrPeriodSamples, release_len - window_len);
  if (r_max == 0)
    return;

  // Latest sustain-loop end: upper bound for the unbounded (final) release.
  // Spec: the final release can be triggered at any point while the attack
  // loops — so its LUT must cover up to the latest loop-end marker.
  // Fallback: if no sustain loops exist, use loop_len - 1.
  const unsigned latest_loop_end = [&]() -> unsigned {
    unsigned v = loop_section.GetLatestLoopEnd();
    return (v > 0) ? v : (loop_len > 0 ? loop_len - 1 : 0u);
  }();

  // Match Python: n_total = max(1, int((loop_len - window_len) / T_float))
  // Ensures n_last = n_total-1 stays within the loop buffer (cs + window <= loop_len).
  unsigned n_total = (loop_len > window_len)
    ? std::max(1u, (unsigned)((double)(loop_len - window_len) / T_f))
    : 1u;
  if (n_total < 4)
    return;

  // n_start / n_end: restrict LUT to the key-press time window of this release.
  // min_key_press_ms > 0  → range starts after previous release's max time.
  // max_key_press_ms == 0 → unbounded final release: use latest_loop_end.
  // max_key_press_ms > 0  → bounded release: use explicit end time.
  const unsigned min_samp       = min_key_press_ms * sample_rate / 1000;
  const unsigned max_samp_limit = (max_key_press_ms > 0)
    ? max_key_press_ms * sample_rate / 1000 : 0u;
  unsigned n_start = (min_samp > 0)
    ? std::max(1u, (unsigned)std::ceil((double)min_samp / T_f)) : 1u;
  unsigned n_end = (max_samp_limit > 0)
    ? std::min(n_total, (unsigned)std::ceil((double)max_samp_limit / T_f) + 2)
    : std::min(n_total,
               std::max(1u, (unsigned)std::ceil((double)latest_loop_end / T_f) + 2));
  if (n_start >= n_end) { n_start = 1; n_end = n_total; }

  // Match Python: ds = min(4, max(1, T // 100)).
  // Python starts downsampling at T=100 (ds=1..4); old C++ threshold was T=500.
  const unsigned ds = GOAudioParams::GetCorrLutDownsampling()
    ? std::min(4u, std::max(1u, m_CorrPeriodSamples / 100u))
    : 1u;
  const unsigned window_len_d = std::max(4u, window_len / ds);
  const unsigned r_max_d      = std::max(1u, r_max / ds);
  // T_d is intentionally integer-based (T_int / ds, not round(T_float / ds)).
  // The tracker operates on a discrete downsampled grid; using a float-derived
  // period would shift sp_T_d by ±1 and flip near-tie beam decisions, breaking
  // parity with Python.  Attack positions (cs_d = round(n*T_float)/ds) are
  // correctly float-derived and unaffected by this choice.
  // A fully float-based tracker (sp_T_d, exp_pos, min_dist_d, circ_dist all
  // derived from T_float/ds) would be more precise but requires a coordinated
  // rework of both C++ and Python — left as a future experiment.
  const unsigned T_d          = std::max(1u, m_CorrPeriodSamples / ds);

  // Build downsampled loop mono up to n_end-1 (saves memory vs. n_total-1).
  const unsigned loop_needed = std::min(
    loop_len,
    (unsigned)std::round((n_end - 1) * T_f) + window_len);
  const unsigned loop_needed_d = (loop_needed + ds - 1) / ds; // ceil(loop_needed/ds) = len(attack[:loop_needed:ds])
  GOSoundCompressionCache loop_cache;
  loop_cache.Init();
  std::vector<float> loop_mono(loop_needed_d, 0.f);
  for (unsigned i = 0; i < loop_needed_d; i++) {
    const unsigned orig = i * ds;
    if (orig >= loop_len) break;
    float s = 0.f;
    for (unsigned c = 0; c < loop_ch; c++)
      s += (float)loop_section.GetSample(orig, c, &loop_cache);
    loop_mono[i] = s / loop_ch;
  }

  // Silent loop guard: if the loop has negligible energy, there is nothing
  // to correlate with.  This catches ranks like traktur noise where the ODF
  // uses BlankLoop.wav (±1 LSB impulses) as the attack placeholder.
  //
  // GetSample() returns raw integers in the native bit-depth range:
  //   24-bit → ±8388608;  16-bit → ±32768.
  // BlankLoop: mean(lw²) ≈ 0.05 (≪ 1).
  // Real pipe: mean(lw²) ≫ 1 000 000 even for quiet samples.
  // Threshold 1.0 provides a generous safety margin on both sides.
  {
    double loop_e = 0.0;
    for (unsigned i = 0; i < loop_needed_d; i++)
      loop_e += (double)loop_mono[i] * loop_mono[i];
    if (loop_needed_d > 0 && loop_e / loop_needed_d < 1.0)
      return; // essentially silent attack → no meaningful correlation
  }

  // Build downsampled release mono.
  const unsigned release_needed_d = (r_max + window_len) / ds + 1;
  GOSoundCompressionCache rel_cache;
  rel_cache.Init();
  std::vector<float> release_mono(release_needed_d, 0.f);
  for (unsigned i = 0; i < release_needed_d; i++) {
    const unsigned orig = i * ds;
    if (orig >= release_len) break;
    float s = 0.f;
    for (unsigned c = 0; c < rel_ch; c++)
      s += (float)release_section.GetSample(orig, c, &rel_cache);
    release_mono[i] = s / rel_ch;
  }

  struct ScoredPoint {
    unsigned loop_pos;
    uint16_t best_r;
    float    score;
  };

  // Fold constants used by the exhaustive corr_at() lambda.
  constexpr float FOLD_ACCEPT_RATIO    = 0.85f;
  constexpr int   FOLD_SEARCH_RADIUS_D = 2;

  // Correlation at period n: full argmax then fold-constrain to nearest branch.
  auto corr_at = [&](unsigned n) -> ScoredPoint {
    if (n < n_start || n >= n_end)
      return {(unsigned)std::round(n * T_f), 0u, -2.f};
    const unsigned cs  = (unsigned)std::round(n * T_f);
    const unsigned p_d = cs / ds;
    if (p_d + window_len_d > loop_needed_d)
      return {cs, 0u, -2.f};
    const float *lw = loop_mono.data() + p_d;

    // Full scan for raw argmax
    float    raw_score = -2.f;
    unsigned raw_r_d   = 0;
    for (unsigned r_d = 0; r_d < r_max_d; r_d++) {
      const float sc = NormalizedDotProduct(lw, release_mono.data(), r_d, window_len_d);
      if (sc > raw_score) { raw_score = sc; raw_r_d = r_d; }
    }
    if (raw_score < -1.5f)
      return {cs, 0u, -2.f};

    // Fold: search window of radius FOLD_SEARCH_RADIUS_D around raw_r_d % T_d
    const unsigned folded_idx_d = raw_r_d % T_d;
    const int lo_d = std::max(0, (int)folded_idx_d - FOLD_SEARCH_RADIUS_D);
    const int hi_d = std::min((int)r_max_d, (int)folded_idx_d + FOLD_SEARCH_RADIUS_D + 1);
    float    folded_score = raw_score;
    unsigned folded_r_d   = raw_r_d;
    if (hi_d > lo_d) {
      folded_score = -2.f;
      for (int r_d2 = lo_d; r_d2 < hi_d; r_d2++) {
        const float sc = NormalizedDotProduct(
          lw, release_mono.data(), (unsigned)r_d2, window_len_d);
        if (sc > folded_score) { folded_score = sc; folded_r_d = (unsigned)r_d2; }
      }
    }

    if (raw_score > 1e-12f && folded_score / raw_score >= FOLD_ACCEPT_RATIO)
      return {cs,
              (uint16_t)(folded_r_d * ds % m_CorrPeriodSamples),
              folded_score};
    return {cs,
            (uint16_t)(raw_r_d * ds % m_CorrPeriodSamples),
            raw_score};
  };

  std::vector<ScoredPoint> spoints;
  std::vector<V2TrackPt>   v2_pts;  // filled by v2 algorithm; empty if exhaustive

  // Phase-0 candidates captured for CSV debug logging (beam_id, r, score).
  std::vector<BeamState> phase0_cands;
  unsigned               phase0_n_last = 0;
  // Diagnostic: final beam states (end of Phase 1) and pre/post-prune points.
  std::vector<BeamState>  final_beam_states;
  std::vector<V2TrackPt>  pre_prune_v2pts;
  // Phase 1.5 diagnostic: interval trigger + per-step log.
  struct Phase15Iv  { unsigned na,nb; int ra,rb,raw_dist,circ_dist; unsigned sp_T_raw; bool triggered; };
  struct Phase15Step{ int nd; unsigned cs_d; int exp_pos,best_r; float best_sc; bool inserted; };
  std::vector<Phase15Iv>   phase15_ivs;
  std::vector<Phase15Step> phase15_steps;
#if __has_include("GOLogReleaseAlignEnable.h")
#endif

  // ── Exhaustive mode: dense scan of [n_start, n_end) ─────────────────────
  // Covers every key-press duration with evenly-spaced support points.
  // For very long loops (n_total > MAX_EXHST) a step is used so the output
  // stays within GOLUT_MAX_POINTS (4096) while still giving comprehensive
  // coverage.  No adaptive sampling, no quality guards.
  if (exhaustive) {
    constexpr unsigned MAX_EXHST = 2000u;
    const unsigned span = (n_end > n_start) ? n_end - n_start : 0u;
    const unsigned step = (span > MAX_EXHST)
                          ? (span + MAX_EXHST - 1) / MAX_EXHST  // ceil divide
                          : 1u;
    for (unsigned n = n_start; n < n_end; n += step) {
      ScoredPoint sp = corr_at(n);
      if (sp.score > -1.5f) spoints.push_back(sp);
    }
    if (spoints.empty()) return;
    goto lut_commit;
  }

  // ── v2 Backward Multi-Beam Tracking ─────────────────────────────────────
  //
  // Phase 0  : full NDP scan at n_last → Top-K initial beams.
  // Phase 1  : backward tracking from n_last to n_start.
  //            dn = V2_DN_DENSE (n ≤ V2_DENSE_N_LIMIT) or V2_DN_SPARSE otherwise.
  //            Each step: ±1 probe; extends to ±2 when best hits the edge.
  //            Rescan trigger: score drops < 90 % of prev or drift > 8 samples.
  // Phase 1.5: dense re-track (dn=1) at jump intervals (dist/dn > T/16).
  // Phase 2  : V2TrackPt list → is_jump / approach_up → Douglas-Peucker prune.
  {
    const unsigned sp_T_d = 2u * T_d;  // search window width (downsampled)

    // ── Phase 0 ───────────────────────────────────────────────────────────
    const unsigned n_last    = n_end - 1;
    const unsigned cs_last_d = (unsigned)std::round(n_last * T_f) / ds;

    std::vector<BeamState> vis_cands = FullScanV2(
      loop_mono.data(), loop_needed_d,
      release_mono.data(), release_needed_d,
      cs_last_d, window_len_d, r_max_d, T_d, V2_TOP_K);
    if (vis_cands.empty()) return;
    phase0_cands = vis_cands;   // capture for CSV logging
    phase0_n_last = n_last;

    // ── Phase 1 ───────────────────────────────────────────────────────────
    using PathMap = std::map<unsigned, std::pair<int, float>>;
    std::vector<PathMap> beam_paths(V2_TOP_K);

    unsigned n_cur = n_last;
    while (n_cur >= n_start) {
      // Record current position for all active beams.
      const unsigned cs_cur_d = (unsigned)std::round(n_cur * T_f) / ds;
      if (cs_cur_d + window_len_d <= loop_needed_d) {
        for (const BeamState &b : vis_cands)
          if (b.beam_id >= 0 && (unsigned)b.beam_id < V2_TOP_K)
            beam_paths[(unsigned)b.beam_id][n_cur] = {b.r_d * (int)ds, b.score};
      }

      // Compute step size and next n.
      const unsigned dn =
        (n_cur <= V2_DENSE_N_LIMIT) ? V2_DN_DENSE : V2_DN_SPARSE;
      const unsigned n_next =
        (n_cur >= n_start + dn) ? n_cur - dn : n_start;
      if (n_next >= n_cur)
        break;  // already at n_start; we recorded it above
      const unsigned dn_actual = n_cur - n_next;

      const unsigned cs_next_d = (unsigned)std::round(n_next * T_f) / ds;
      TrackResult res = TrackStepV2(
        loop_mono.data(), loop_needed_d,
        release_mono.data(), release_needed_d,
        cs_next_d, window_len_d, sp_T_d, T_d,
        vis_cands, dn_actual,
        V2_RESCAN_RATIO, V2_RESCAN_DRIFT, ds, V2_TOP_K);

      if (!res.beams.empty()) {
        vis_cands = res.beams;
        if (res.needs_rescan) {
          std::vector<BeamState> fresh = FullScanV2(
            loop_mono.data(), loop_needed_d,
            release_mono.data(), release_needed_d,
            cs_next_d, window_len_d, r_max_d, T_d, V2_TOP_K);
          if (!fresh.empty()) {
            // Match each fresh peak to the geometrically nearest existing beam.
            // Important: find the nearest beam WITHOUT skipping used IDs first.
            // If two fresh peaks share the same nearest old beam, the branches
            // have locally collapsed — assigning the second peak to the next free
            // beam would create artificial beam crossings and chaotic histories.
            // Instead, discard the second peak (matches Python semantics exactly).
            std::vector<bool> id_used(V2_TOP_K, false);
            std::vector<BeamState> refreshed;
            refreshed.reserve(fresh.size());
            for (const BeamState &fr : fresh) {
              // Step 1: find nearest beam (ignore id_used).
              int best_bid  = -1;
              int best_dist = INT_MAX;
              for (const BeamState &vb : vis_cands) {
                if (vb.beam_id < 0 || (unsigned)vb.beam_id >= V2_TOP_K) continue;
                int d = std::abs(fr.r_d - vb.r_d);
                if (d > (int)sp_T_d / 2) d = (int)sp_T_d - d;
                if (d < best_dist) { best_dist = d; best_bid = vb.beam_id; }
              }
              if (best_bid < 0) continue;
              // Step 2: discard if nearest beam already claimed (no fallback).
              if (id_used[(unsigned)best_bid]) continue;
              id_used[(unsigned)best_bid] = true;
              float prev_cum = 0.f;
              for (const BeamState &vb : vis_cands)
                if (vb.beam_id == best_bid) { prev_cum = vb.cum_score; break; }
              refreshed.push_back(
                {fr.r_d, fr.score, fr.r_d, 1, prev_cum + fr.score, best_bid});
            }
            if (!refreshed.empty()) vis_cands = refreshed;
          }
        }
      }
      n_cur = n_next;
    }

    // Winner beam = vis_cands[0] (sorted by cumulative score descending).
    final_beam_states = vis_cands; // capture for CSV diagnostics (beam state after Phase 1)
    const int   best_id = vis_cands.empty() ? 0 : vis_cands[0].beam_id;
    const auto &primary = ((unsigned)best_id < V2_TOP_K)
                          ? beam_paths[(unsigned)best_id]
                          : beam_paths[0];
    if (primary.empty()) return;

    // Working copy of the winner path (may be extended in Phase 1.5).
    PathMap primary_path(primary);

    // ── Phase 1.5: dense re-tracking at jump intervals ────────────────────
    {
      const unsigned sp_T_raw  = sp_T_d * ds; // = 2*(T_int//ds)*ds; Python uses _sp_T_for_iv = sp_T_d*ds (same), not _sp_T_raw = 2*T_int (different variable, pre-Phase-1.5 only)
      const float jump_thresh = (float)m_CorrPeriodSamples
                                / (float)V2_JUMP_RATE_FACTOR;

      std::vector<std::pair<unsigned, unsigned>> jump_ivs;
      // Python JUMP_DENSE_ABS = 20 original samples: trigger Phase 1.5 for any
      // interval with dn > 2 AND circular distance >= 20 samples.
      static constexpr unsigned JUMP_DENSE_ABS = 20u;
      for (auto it = primary_path.begin(); it != primary_path.end(); ) {
        auto nxt = std::next(it);
        if (nxt == primary_path.end()) break;
        int raw_dist = std::abs(nxt->second.first - it->second.first);
        int circ_dist = raw_dist;
        if ((unsigned)circ_dist > sp_T_raw / 2u) circ_dist = (int)sp_T_raw - circ_dist;
        const unsigned dn_ab = nxt->first - it->first;
        const bool trig = (dn_ab > 2u && (unsigned)circ_dist >= JUMP_DENSE_ABS);
        phase15_ivs.push_back({it->first, nxt->first,
                               it->second.first, nxt->second.first,
                               raw_dist, circ_dist, sp_T_raw, trig});
        if (trig) jump_ivs.push_back({it->first, nxt->first});
        it = nxt;
      }

      for (const auto &iv : jump_ivs) {
        auto it_b = primary_path.find(iv.second);
        if (it_b == primary_path.end()) continue;
        const int   rb  = it_b->second.first;
        const float scb = it_b->second.second;
        std::vector<BeamState> dense_beam = {
          {(int)((unsigned)rb / ds), scb,
           (int)((unsigned)rb / ds), 1, scb, 0}};
        for (int nd = (int)iv.second - 1; nd > (int)iv.first; nd--) {
          if (nd < (int)n_start) break;
          const unsigned cs_d = (unsigned)std::round((double)nd * T_f) / ds;
          if (cs_d + window_len_d > loop_needed_d) {
            phase15_steps.push_back({nd, cs_d, -1, -1, -99.f, false});
            continue;
          }
          // Compute exp_pos from current dense_beam for logging.
          const BeamState &cb = dense_beam[0];
          float slope_log = (cb.dn_prev > 0)
            ? (float)(cb.r_d - cb.r_d_prev) / (float)cb.dn_prev : 0.f;
          int exp_pos_log = RoundHalfEven((float)cb.r_d + slope_log) % (int)sp_T_d;
          if (exp_pos_log < 0) exp_pos_log += (int)sp_T_d;

          TrackResult step = TrackStepV2(
            loop_mono.data(), loop_needed_d,
            release_mono.data(), release_needed_d,
            cs_d, window_len_d, sp_T_d, T_d,
            dense_beam, 1u,
            V2_RESCAN_RATIO, V2_RESCAN_DRIFT, ds, 1u);
          // Python continues with last good beam on failed step; match that.
          if (step.beams.empty()) {
            phase15_steps.push_back({nd, cs_d, exp_pos_log * (int)ds,
                                     dense_beam[0].r_d * (int)ds,
                                     dense_beam[0].score, false});
            continue;
          }
          dense_beam = step.beams;
          primary_path[(unsigned)nd] =
            {dense_beam[0].r_d * (int)ds, dense_beam[0].score};
          phase15_steps.push_back({nd, cs_d, exp_pos_log * (int)ds,
                                   dense_beam[0].r_d * (int)ds,
                                   dense_beam[0].score, true});
        }
      }
    }

    // ── Phase 2: build V2TrackPt list ─────────────────────────────────────
    v2_pts.reserve(primary_path.size());
    for (const auto &kv : primary_path) {
      if (kv.second.second < -1.5f) continue;
      V2TrackPt p;
      p.n           = kv.first;
      p.r           = kv.second.first;
      p.score       = kv.second.second;
      p.loop_pos    = (unsigned)std::round(p.n * T_f);
      p.is_jump     = false;
      p.approach_up = true;
      v2_pts.push_back(p);
    }
    // primary_path is std::map ordered by n ascending — no sort needed.

    if (v2_pts.empty()) return;

    // Compute is_jump and approach_up on a sorted V2TrackPt list.
    const unsigned sp_T_full = 2u * m_CorrPeriodSamples;
    const float jump_rate    =
      (float)m_CorrPeriodSamples / (float)V2_JUMP_RATE_FACTOR;
    auto mark_flags = [&](std::vector<V2TrackPt> &pts) {
      if (pts.empty()) return;
      pts[0].approach_up = true;
      pts[0].is_jump     = false;
      for (unsigned i = 1; i < (unsigned)pts.size(); i++) {
        int dist = std::abs(pts[i].r - pts[i - 1].r);
        if ((unsigned)dist > sp_T_full / 2u) dist = (int)sp_T_full - dist;
        const unsigned dn_ab = std::max(1u, pts[i].n - pts[i - 1].n);
        pts[i].is_jump = ((float)dist / (float)dn_ab > jump_rate);
        const int fwd =
          ((pts[i].r - pts[i - 1].r) % (int)sp_T_full + (int)sp_T_full)
          % (int)sp_T_full;
        pts[i].approach_up = ((unsigned)fwd <= sp_T_full / 2u);
      }
    };
    mark_flags(v2_pts);

    // Douglas-Peucker pruning.
    pre_prune_v2pts = v2_pts; // capture for CSV diagnostics (before PruneV2)
    if (v2_pts.size() > 2)
      v2_pts = PruneV2(std::move(v2_pts), (float)m_CorrPeriodSamples);

    // After pruning: refresh only approach_up — is_jump must not change.
    // PruneV2 protects jump points and their predecessors, so jumps are never
    // added or removed.  Recomputing is_jump here would diverge from Python
    // (reference) which also leaves is_jump unchanged after pruning.
    if (!v2_pts.empty()) {
      v2_pts[0].approach_up = true;
      for (unsigned i = 1; i < (unsigned)v2_pts.size(); i++) {
        const int fwd =
          ((v2_pts[i].r - v2_pts[i - 1].r) % (int)sp_T_full + (int)sp_T_full)
          % (int)sp_T_full;
        v2_pts[i].approach_up = ((unsigned)fwd <= sp_T_full / 2u);
      }
    }
  }

lut_commit:
  // Convert to CorrPoint and append LUT entry.
  std::vector<CorrPoint> points;
  if (!v2_pts.empty()) {
    // v2 path: store r in [0,2T) — NO fold.  Flags encode direction/jump.
    points.reserve(v2_pts.size());
    for (const V2TrackPt &p : v2_pts) {
      uint8_t flags = CorrPoint::kFlagValid;
      if (p.approach_up) flags |= CorrPoint::kFlagApproachUp;
      if (p.is_jump)     flags |= CorrPoint::kFlagIsJump;
      points.push_back({p.loop_pos, (uint16_t)p.r, flags, 0u});
    }
  } else {
    // exhaustive path: no v2 flags (legacy runtime behaviour).
    points.reserve(spoints.size());
    for (const ScoredPoint &sp : spoints)
      points.push_back({sp.loop_pos, sp.best_r, 0u, 0u});
  }

#if __has_include("GOLogReleaseAlignEnable.h")
  {
    const auto t1  = std::chrono::high_resolution_clock::now();
    const long long us
      = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const long long total = (s_TotalLoadUs += us);
    const unsigned  n     = ++s_LoadCount;
    std::lock_guard<std::mutex> lock(s_LoadLogMutex);
    std::ofstream f(GetLoadTimingLogPath(), std::ios::app);
    f << "corr_lut"
      << " freq=" << sample_freq_hz
      << " T=" << m_CorrPeriodSamples
      << " points=" << points.size()
      << " us=" << us
      << " total_ms=" << (total / 1000)
      << " n=" << n
      << "\n";
  }
#endif
  m_CorrLuts.push_back({&loop_section, std::move(points)});

#if __has_include("GOLogReleaseAlignEnable.h")
  // LUT verify log — written when a pipe label is provided so Python's
  // verify_lut.py can independently recompute and compare results.
  if (label) {
    static std::mutex        s_VerifyLogMutex;
    static std::atomic<bool> s_Rotated{false};
    std::lock_guard<std::mutex> vlock(s_VerifyLogMutex);

    auto GetVerifyLogPath = []() -> std::string {
#ifdef _WIN32
      const char *tmp = std::getenv("TEMP");
      if (!tmp) tmp = std::getenv("TMP");
      if (!tmp) tmp = "C:\\";
      return std::string(tmp) + "\\go_lut_verify.csv";
#else
      return "/tmp/go_lut_verify.csv";
#endif
    };

    // One-time rotation per process: rename existing file with timestamp so
    // each GO session starts with a fresh, unambiguous log.
    if (!s_Rotated.exchange(true)) {
      const std::string path = GetVerifyLogPath();
      if (std::ifstream(path).good()) {
        std::time_t t = std::time(nullptr);
        std::tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &lt);
        const std::string dot = path.rfind('.') != std::string::npos
                                ? path.substr(path.rfind('.')) : "";
        const std::string stem = (dot.empty()) ? path
                                : path.substr(0, path.rfind('.'));
        std::rename(path.c_str(), (stem + "_" + ts + dot).c_str());
      }
    }

    std::ofstream vf(GetVerifyLogPath(), std::ios::app);
    if (vf.is_open()) {
      const auto &pts = m_CorrLuts.back().points;
      // NDP_SILENT_SCORE: what NDP_f32 returns when denom < 1e-12.
      // Before fix: 0.f  After fix: -2.f  Used to verify which binary is running.
      static constexpr float NDP_SILENT_SCORE = -2.f;
      vf << std::setprecision(17)
         << "pipe=" << label
         << " align_version=v231a"
         << " ndp_silent_score=" << NDP_SILENT_SCORE
         << " T_float=" << m_CorrPeriodFloat
         << " T_int=" << m_CorrPeriodSamples
         << " crossfade_len=" << crossfade_len
         << " ds=" << ds
         << " harmonic=" << harmonic_number
         << " n_start=" << n_start
         << " n_end=" << n_end
         << " min_ms=" << min_key_press_ms
         << " max_ms=" << max_key_press_ms
         << " sample_rate=" << sample_rate
         << " loop_len=" << loop_section.GetLength()
         << " release_len=" << release_section.GetLength()
         << " r_max=" << r_max
         << " latest_loop_end=" << latest_loop_end
         << " loop_count=" << [&]() {
              unsigned c = 0;
              for (unsigned i = 0; i < loop_section.GetEndSegmentCount(); i++)
                if (loop_section.GetEndSegment(i).next_start_segment_index >= 0) c++;
              return c;
            }()
         << " attack_file=" << loop_section.GetLoaderBasename()
         << "\n";
      // Phase-0 candidates: initial NDP peaks that seeded the tracking.
      // Format: phase0,beam_id,r_samples,score_hex,n_last
      for (const BeamState &b : phase0_cands)
        vf << "phase0," << b.beam_id
           << "," << (b.r_d * (int)ds)
           << "," << std::hexfloat << b.score << std::defaultfloat
           << "," << phase0_n_last
           << "\n";
      // Final beam states after Phase 1 tracking (before winner selection).
      // Format: final_beam,beam_id,r_samples,cum_score_hex,cum_score_dec
      // cum_score_hex: std::hexfloat for exact float32 round-trip (no decimal precision loss).
      for (const BeamState &b : final_beam_states)
        vf << "final_beam," << b.beam_id
           << "," << (b.r_d * (int)ds)
           << "," << std::hexfloat << b.cum_score << std::defaultfloat
           << "," << std::setprecision(9) << b.cum_score
           << "\n";
      // Pre-prune points (after mark_flags, before PruneV2).
      // Format: pre_prune,n,r,score,is_jump,approach_up
      for (const V2TrackPt &p : pre_prune_v2pts)
        vf << "pre_prune," << p.n
           << "," << p.r
           << "," << std::setprecision(6) << p.score
           << "," << (p.is_jump ? 1 : 0)
           << "," << (p.approach_up ? 1 : 0)
           << "\n";
      // Phase 1.5 diagnostics.
      // interval: phase15_iv,na,nb,ra,rb,raw_dist,circ_dist,sp_T_raw,triggered
      for (const Phase15Iv &iv : phase15_ivs)
        vf << "phase15_iv," << iv.na << "," << iv.nb
           << "," << iv.ra << "," << iv.rb
           << "," << iv.raw_dist << "," << iv.circ_dist
           << "," << iv.sp_T_raw << "," << (iv.triggered ? 1 : 0) << "\n";
      // step: phase15_step,nd,cs_d,exp_pos,best_r,best_score,inserted
      for (const Phase15Step &st : phase15_steps)
        vf << "phase15_step," << st.nd << "," << st.cs_d
           << "," << st.exp_pos << "," << st.best_r
           << "," << std::setprecision(6) << st.best_sc
           << "," << (st.inserted ? 1 : 0) << "\n";
      // Post-prune points (after PruneV2 + recomputed mark_flags).
      // Format: post_prune,n,r,is_jump,approach_up
      for (const V2TrackPt &p : v2_pts)
        vf << "post_prune," << p.n
           << "," << p.r
           << "," << (p.is_jump ? 1 : 0)
           << "," << (p.approach_up ? 1 : 0)
           << "\n";
      for (const CorrPoint &p : pts) {
        const unsigned n = (unsigned)std::round((double)p.loop_pos / T_f);
        // best_r is unfolded [0,2T) for v2 path; legacy exhaustive path keeps [0,T).
        vf << "lut," << n << "," << p.loop_pos << "," << p.best_r
           << "," << (p.IsApproachUp() ? 1 : 0)
           << "," << (p.IsJump() ? 1 : 0)
           << "\n";
      }
      // Simulator samples: ~50-point grid + last n so Python can verify
      // GetPositionForCorrelation (interpolation + phi logic).
      const unsigned sim_step = std::max(1u, (n_end - n_start) / 50u);
      unsigned last_lp = (unsigned)-1;
      for (unsigned n = n_start; n < n_end; n += sim_step) {
        const unsigned lp = (unsigned)std::round(n * T_f);
        vf << "sim," << lp << "," << GetPositionForCorrImpl(lp, pts, m_CorrPeriodFloat, m_CorrPeriodSamples) << "\n";
        last_lp = lp;
      }
      const unsigned end_lp = (unsigned)std::round((n_end - 1) * T_f);
      if (end_lp != last_lp)
        vf << "sim," << end_lp << "," << GetPositionForCorrImpl(end_lp, pts, m_CorrPeriodFloat, m_CorrPeriodSamples) << "\n";
    }
  }
#endif
}

void GOSoundReleaseAlignTable::AssignAttackPointers(
  const std::vector<const GOSoundAudioSection *> &attacks) {
  for (size_t i = 0; i < m_CorrLuts.size() && i < attacks.size(); i++)
    m_CorrLuts[i].p_Attack = attacks[i];
}

const std::vector<GOSoundReleaseAlignTable::CorrPoint> *
GOSoundReleaseAlignTable::FindLut(const GOSoundAudioSection *p_Attack) const {
  if (m_CorrLuts.empty())
    return nullptr;
  if (p_Attack)
    for (const auto &lut : m_CorrLuts)
      if (lut.p_Attack == p_Attack)
        return &lut.points;
  return &m_CorrLuts[0].points; // fallback: first LUT
}

unsigned GOSoundReleaseAlignTable::GetPositionForCorrelation(
  unsigned loop_pos, const GOSoundAudioSection *p_Attack) const {
  const std::vector<CorrPoint> *pts = FindLut(p_Attack);
  if (!pts || pts->empty() || m_CorrPeriodSamples == 0)
    return 0;
  // Use float period for accurate phase — avoids drift from integer rounding.
  const double T_f = (m_CorrPeriodFloat > 0.0) ? m_CorrPeriodFloat
                                                : (double)m_CorrPeriodSamples;
  // Guard: round() can produce exactly T_int when fmod result is just below T_f.
  const unsigned phi = (unsigned)std::round(std::fmod((double)loop_pos, T_f))
                       % m_CorrPeriodSamples;
  const unsigned r_lut  = InterpolateCorrPointRaw((int)loop_pos, *pts, T_f);
  const bool     v2_lut = pts->back().IsValid();
  return (unsigned)std::round(
    std::fmod((double)(r_lut + phi), v2_lut ? 2.0 * T_f : T_f));
}

unsigned GOSoundReleaseAlignTable::GetPositionFor(
  int history[BLOCK_HISTORY][MAX_OUTPUT_CHANNELS]) const {
  /* Get combined release f's and v's */
  int f_mod = 0;
  int v_mod = 0;
  for (unsigned i = 0; i < MAX_OUTPUT_CHANNELS; i++) {
    f_mod += history[(BLOCK_HISTORY - 1)][i];
    v_mod += history[(BLOCK_HISTORY - 2)][i];
  }
  v_mod = f_mod - v_mod;

  /* Bring f and v into the range -1..2*m_PhaseAlignMaxDerivative-1 */
  v_mod += (m_PhaseAlignMaxDerivative - 1);
  f_mod += (m_PhaseAlignMaxAmplitude - 1);

  int derivIndex = m_PhaseAlignMaxDerivative
    ? (PHASE_ALIGN_DERIVATIVES * v_mod) / (2 * m_PhaseAlignMaxDerivative)
    : PHASE_ALIGN_DERIVATIVES / 2;

  /* Bring f into the range -1..2*m_PhaseAlignMaxAmplitude-1 */
  int ampIndex = m_PhaseAlignMaxAmplitude
    ? (PHASE_ALIGN_AMPLITUDES * f_mod) / (2 * m_PhaseAlignMaxAmplitude)
    : PHASE_ALIGN_AMPLITUDES / 2;

  /* Store this release point if it was not already found */
  assert((derivIndex >= 0) && (derivIndex < PHASE_ALIGN_DERIVATIVES));
  assert((ampIndex >= 0) && (ampIndex < PHASE_ALIGN_AMPLITUDES));
  derivIndex = (derivIndex < 0)
    ? 0
    : (
      (derivIndex >= PHASE_ALIGN_DERIVATIVES) ? PHASE_ALIGN_DERIVATIVES - 1
                                              : derivIndex);
  ampIndex = (ampIndex < 0)
    ? 0
    : (
      (ampIndex >= PHASE_ALIGN_AMPLITUDES) ? PHASE_ALIGN_AMPLITUDES - 1
                                           : ampIndex);
  return m_PositionEntries[derivIndex][ampIndex];
}

#if __has_include("GOLogReleaseAlignVerbose.h")
void GOSoundReleaseAlignTable::DumpLutPoints(
  const GOSoundAudioSection *p_Attack, std::ostream &out) const {
  const std::vector<CorrPoint> *pts = FindLut(p_Attack);
  if (!pts || pts->empty()) return;
  for (const auto &cp : *pts)
    out << "lut," << cp.loop_pos << "," << (unsigned)cp.best_r << "\n";
}

unsigned GOSoundReleaseAlignTable::CopyLutPoints(
  const GOSoundAudioSection *p_Attack,
  uint32_t *out_pos,
  uint16_t *out_r,
  unsigned  max_points) const {
  const std::vector<CorrPoint> *pts = FindLut(p_Attack);
  if (!pts || pts->empty()) return 0;
  unsigned n = std::min((unsigned)pts->size(), max_points);
  for (unsigned i = 0; i < n; i++) {
    out_pos[i] = (*pts)[i].loop_pos;
    out_r[i]   = (*pts)[i].best_r;
  }
  return n;
}
#endif

void GOSoundReleaseAlignTable::OverrideCorrLutsFromCache(
  std::vector<CorrPoint> points,
  uint32_t               period_samples,
  double                 period_float) {
  if (points.empty())
    return;
  if (period_samples >= 16) {
    m_CorrPeriodSamples = period_samples;
    m_CorrPeriodFloat   = period_float;
  }
  if (m_CorrPeriodSamples == 0)
    return;
  m_CorrLuts.clear();
  m_CorrLuts.push_back({nullptr, std::move(points)});
}

void GOSoundReleaseAlignTable::ClearCachedLut() {
  if (m_CorrLuts.size() == 1 && m_CorrLuts[0].p_Attack == nullptr)
    m_CorrLuts.clear();
}
