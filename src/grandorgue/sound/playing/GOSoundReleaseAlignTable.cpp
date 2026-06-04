/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundReleaseAlignTable.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "../GOCrossfadeParam.h"

#if __has_include("GOLogReleaseAlignEnable.h")
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
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
  // Format v3: n_luts (uint32), then period+crossfade, then each LUT's points.
  uint32_t n_luts = 0;
  if (!cache.Read(&n_luts, sizeof(n_luts)))
    return true; // EOF on old/v2 cache: not an error
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

  // ── Sparse correlation LUTs (v3 format) ─────────────────────────────────
  uint32_t n_luts = (uint32_t)m_CorrLuts.size();
  if (!cache.Write(&n_luts, sizeof(n_luts)))
    return false;
  if (n_luts > 0) {
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
        if (!cache.Write(&cp.loop_pos, sizeof(cp.loop_pos)))
          return false;
        if (!cache.Write(&cp.best_r, sizeof(cp.best_r)))
          return false;
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

// Estimates the true fundamental period via autocorrelation on a stable
// excerpt of the loop sample. Returns best lag in [min_period, max_period].
static unsigned EstimatePeriodByAutocorr(
  const float *samples,
  unsigned len,
  unsigned min_period,
  unsigned max_period) {
  if (len < max_period * 2)
    return min_period;

  // Hann window: tapers the analysis segment to zero at both ends.
  // This suppresses spectral side lobes that make NDP(T/2) and NDP(2T)
  // appear nearly as strong as NDP(T) for harmonically rich signals.
  std::vector<float> windowed(len);
  for (unsigned i = 0; i < len; i++) {
    const float w = 0.5f * (1.f - std::cos(2.f * (float)M_PI * i / (len - 1)));
    windowed[i] = samples[i] * w;
  }
  const float *s = windowed.data();

  const unsigned window = len - max_period;
  float    best_score = -2.f;
  unsigned best_lag   = min_period;

  // Lag penalty (alpha=0.10): breaks the NDP(T)=NDP(2T) tie for perfect
  // loops, preferring the shorter period.  Combines with Hann for robustness.
  const float alpha = 0.10f;
  for (unsigned lag = min_period; lag <= max_period; lag++) {
    const float raw = NormalizedDotProduct(s, s, lag, window);
    const float sc  = raw * (1.f - alpha * (float)lag / (float)max_period);
    if (sc > best_score) {
      best_score = sc;
      best_lag   = lag;
    }
  }
  return best_lag;
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
  bool     exhaustive) {
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

  // Non-octave stops (aliquots, mixtures): determine T purely from the audio
  // via autocorrelation.  The smpl-chunk pitch (encoded in sample_freq_hz)
  // is NOT used as the search-range centre — it can reflect the harmonic
  // pitch, the key pitch, or be influenced by HarmonicNumber scaling, all
  // of which would bias the range away from the true waveform period.
  // Instead we derive the range from the loop length directly, which is
  // the only reliable bound we have without prior assumptions about pitch.
  if (first_call && !CorrIsOctaveStop(harmonic_number)) {
    const unsigned loop_len_full = loop_section.GetLength();

    // Search the full range down to 20 Hz — no smpl-pitch bias.
    // If the loop is too short to support this window the inner check handles it.
    const unsigned max_p = sample_rate / 20u;
    const unsigned min_p = 16u;

    {
      unsigned ac_window = 8 * max_p;
      unsigned ac_offset = loop_len_full / 2;
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
        m_CorrPeriodSamples = EstimatePeriodByAutocorr(
          ac_mono.data(), ac_window, min_p, max_p);
        m_CorrPeriodFloat = m_CorrPeriodSamples;
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

  unsigned n_total = loop_len / m_CorrPeriodSamples;
  if (n_total < 4)
    return;

  // n_start / n_end: restrict LUT to the key-press time window of this release.
  // min_key_press_ms=0 means start from the beginning;
  // max_key_press_ms=0 means cover to the end of the attack.
  const unsigned min_samp       = min_key_press_ms * sample_rate / 1000;
  const unsigned max_samp_limit = (max_key_press_ms > 0)
    ? max_key_press_ms * sample_rate / 1000 : 0u;
  unsigned n_start = (min_samp > 0)
    ? std::max(1u, (unsigned)std::ceil((double)min_samp / T_f)) : 1u;
  unsigned n_end = (max_samp_limit > 0)
    ? std::min(n_total, (unsigned)std::ceil((double)max_samp_limit / T_f) + 2)
    : n_total;
  if (n_start >= n_end) { n_start = 1; n_end = n_total; }

  const unsigned ds = (GOAudioParams::GetCorrLutDownsampling()
                       && m_CorrPeriodSamples >= 500u)
    ? std::min(4u, m_CorrPeriodSamples / 500u)
    : 1u;
  const unsigned window_len_d = std::max(4u, window_len / ds);
  const unsigned r_max_d      = std::max(1u, r_max / ds);
  const unsigned T_d          = std::max(1u, m_CorrPeriodSamples / ds);

  // Build downsampled loop mono up to n_end-1 (saves memory vs. n_total-1).
  const unsigned loop_needed = std::min(
    loop_len,
    (unsigned)std::round((n_end - 1) * T_f) + window_len);
  const unsigned loop_needed_d = loop_needed / ds + 1;
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

  // Fold constants matching Python v47:
  // If the globally best r falls near a branch multiple (r % T ≈ folded),
  // restrict to that branch so oscillation between branches is suppressed.
  constexpr float FOLD_ACCEPT_RATIO    = 0.85f;
  constexpr int   FOLD_SEARCH_RADIUS_D = 2;

  // Circular distance between two best_r values, always in [0, T/2].
  auto circ_dist = [&](uint16_t a, uint16_t b) -> int {
    int d = (int)a - (int)b;
    if (d < 0) d = -d;
    if (d > (int)m_CorrPeriodSamples / 2)
      d = (int)m_CorrPeriodSamples - d;
    return d;
  };

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

  // ── Short loop (n_total ≤ 30) ─────────────────────────────────────────────
  if (n_total <= 30) {
    const unsigned span = (n_end > n_start) ? n_end - n_start : 0u;
    const unsigned step = (span > 1) ? std::max(1u, (span - 1) / 9u) : 1u;
    for (unsigned i = 0; i < std::min(10u, span); i++) {
      const unsigned p = n_start + i * step;
      if (p < n_end) {
        ScoredPoint sp = corr_at(p);
        if (sp.score > -1.5f) spoints.push_back(sp);
      }
    }
    // Short loops: stabilized by definition, no drift/quality check.
    if (spoints.empty()) return;

  } else {
    // ── Long loop ─────────────────────────────────────────────────────────────
    constexpr unsigned DENSE_STEP_MAX        = 6;
    constexpr unsigned STABLE_WIN            = 4;
    constexpr unsigned N_SPARSE              = 5;
    constexpr unsigned MAX_TOTAL             = 30;
    constexpr unsigned DRIFT_FIT_WIN         = 12;
    constexpr float    DRIFT_MAX_RESID_FACTOR = 1.0f;
    constexpr float    SCORE_MIN_THRESHOLD   = 0.35f;
    constexpr float    COHERENCE_THRESHOLD   = 0.75f;
    constexpr unsigned PHASE3_MAX            = 8;

    const int stable_thresh = std::max(
      (int)m_CorrPeriodSamples
        / (!CorrIsOctaveStop(harmonic_number) ? 6 : 8),
      4);

    // Dynamic dense_step: enough steps to fit STABLE_WIN+1 points in the range.
    const unsigned dense_start  = std::max(n_start, 5u);
    const unsigned available_n  = (n_end > dense_start) ? n_end - dense_start : 1u;
    const unsigned dense_step
      = std::min(DENSE_STEP_MAX, std::max(1u, available_n / (STABLE_WIN + 1)));
    const unsigned dense_stop
      = std::min(n_end, dense_start + dense_step * (STABLE_WIN + 2));

    // Phase 1: dense — sample until stable or dense_stop reached.
    bool     stabilized = false;
    unsigned n_dense    = 0;
    for (unsigned n = dense_start; n < dense_stop; n += dense_step) {
      ScoredPoint sp = corr_at(n);
      if (sp.score > -1.5f) {
        spoints.push_back(sp);
        n_dense++;
        if ((unsigned)spoints.size() >= STABLE_WIN) {
          bool ok = true;
          for (unsigned k = (unsigned)spoints.size() - STABLE_WIN;
               k + 1 < (unsigned)spoints.size() && ok; ++k)
            if (circ_dist(spoints[k].best_r, spoints[k + 1].best_r) > stable_thresh)
              ok = false;
          if (ok) { stabilized = true; break; }
        }
      }
    }
    if (!stabilized) {
      // permissive: continue with partial spoints if at least one was computed
      if (!permissive || spoints.empty()) return;
    }

    // Drift detection: linear phase shift over DRIFT_FIT_WIN dense points.
    // Slope ≥ T/(4·STABLE_WIN·DENSE_STEP) with low residual → drift → Legacy.
    {
      const unsigned drift_n = std::min((unsigned)spoints.size(), DRIFT_FIT_WIN);
      if (drift_n >= 4) {
        const double half_T = m_CorrPeriodSamples / 2.0;
        double prev_phase   = (double)(spoints[0].best_r % m_CorrPeriodSamples);
        double sx = 0, sy = 0, sxy = 0, sxx = 0;
        double unwrapped = prev_phase;
        double x0        = std::round(spoints[0].loop_pos / T_f);
        sx += x0; sy += unwrapped; sxy += x0 * unwrapped; sxx += x0 * x0;
        for (unsigned i = 1; i < drift_n; i++) {
          const double cur = (double)(spoints[i].best_r % m_CorrPeriodSamples);
          double d = cur - prev_phase;
          while (d >  half_T) d -= m_CorrPeriodSamples;
          while (d < -half_T) d += m_CorrPeriodSamples;
          unwrapped  += d;
          prev_phase  = cur;
          const double xi = std::round(spoints[i].loop_pos / T_f);
          sx += xi; sy += unwrapped; sxy += xi * unwrapped; sxx += xi * xi;
        }
        const double N     = (double)drift_n;
        const double denom = N * sxx - sx * sx;
        const double slope = (denom != 0.0) ? (N * sxy - sx * sy) / denom : 0.0;
        const double icept = (sy - slope * sx) / N;

        double max_resid = 0.0;
        prev_phase = (double)(spoints[0].best_r % m_CorrPeriodSamples);
        unwrapped  = prev_phase;
        double xi0  = std::round(spoints[0].loop_pos / T_f);
        max_resid  = std::abs(unwrapped - (slope * xi0 + icept));
        for (unsigned i = 1; i < drift_n; i++) {
          const double cur = (double)(spoints[i].best_r % m_CorrPeriodSamples);
          double d = cur - prev_phase;
          while (d >  half_T) d -= m_CorrPeriodSamples;
          while (d < -half_T) d += m_CorrPeriodSamples;
          unwrapped  += d;
          prev_phase  = cur;
          const double xi = std::round(spoints[i].loop_pos / T_f);
          const double r  = std::abs(unwrapped - (slope * xi + icept));
          if (r > max_resid) max_resid = r;
        }

        const double drift_slope_thresh
          = m_CorrPeriodSamples / (4.0 * STABLE_WIN * DENSE_STEP_MAX);
        if (std::abs(slope) >= drift_slope_thresh
            && max_resid <= (double)stable_thresh * DRIFT_MAX_RESID_FACTOR)
          if (!permissive) return;  // drift detected → Legacy path
      }
    }

    // Phase 2: sparse — N_SPARSE points from last dense n to n_end-1.
    const unsigned last_n
      = (unsigned)std::round(spoints.back().loop_pos / T_f);
    if (last_n + 1 < n_end) {
      const unsigned n_rem
        = std::min(N_SPARSE, MAX_TOTAL - (unsigned)spoints.size());
      for (unsigned i = 1; i <= n_rem; i++) {
        const unsigned n = last_n + i * (n_end - 1 - last_n) / n_rem;
        if (n > last_n && n < n_end) {
          ScoredPoint sp = corr_at(n);
          if (sp.score > -1.5f) spoints.push_back(sp);
        }
      }
    }

    // Phase 3: gap fill — insert midpoint between pairs with circ_dist > T/4.
    const int gap_thresh = (int)m_CorrPeriodSamples / 4;
    unsigned  gap_count  = 0;
    unsigned  idx        = 0;
    while (idx + 1 < spoints.size() && (unsigned)spoints.size() < MAX_TOTAL) {
      if (circ_dist(spoints[idx].best_r, spoints[idx + 1].best_r) > gap_thresh) {
        const unsigned na = (unsigned)std::round(spoints[idx].loop_pos     / T_f);
        const unsigned nb = (unsigned)std::round(spoints[idx + 1].loop_pos / T_f);
        const unsigned nm = (na + nb) / 2;
        if (nm > na && nm < nb) {
          ScoredPoint sp = corr_at(nm);
          if (sp.score > -1.5f) {
            spoints.insert(spoints.begin() + idx + 1, sp);
            gap_count++;
          }
          continue;  // recheck this pair (or advance if midpoint was invalid)
        }
      }
      ++idx;
    }

    if (spoints.empty()) return;

    // Quality check on steady-state (sparse + gap) points.
    // Short loops and unstabilised long loops never reach here.
    if (n_dense < (unsigned)spoints.size()) {
      const unsigned n_ss = (unsigned)spoints.size() - n_dense;

      float score_min = spoints[n_dense].score;
      for (unsigned i = n_dense + 1; i < (unsigned)spoints.size(); i++)
        score_min = std::min(score_min, spoints[i].score);

      // Circular coherence R ∈ [0,1]: 1 = all best_r on one branch, 0 = scattered.
      const double two_pi = 2.0 * std::acos(-1.0);
      double sum_sin = 0.0, sum_cos = 0.0;
      for (unsigned i = n_dense; i < (unsigned)spoints.size(); i++) {
        const double angle = two_pi * spoints[i].best_r / m_CorrPeriodSamples;
        sum_sin += std::sin(angle);
        sum_cos += std::cos(angle);
      }
      const double R = std::sqrt(sum_sin * sum_sin + sum_cos * sum_cos) / n_ss;

      if (score_min < SCORE_MIN_THRESHOLD
          || R < COHERENCE_THRESHOLD
          || gap_count > PHASE3_MAX)
        if (!permissive) return;  // bad LUT quality → Legacy path
    }
  }

lut_commit:
  // Convert to CorrPoint and append LUT entry.
  std::vector<CorrPoint> points;
  points.reserve(spoints.size());
  for (const ScoredPoint &sp : spoints)
    points.push_back({sp.loop_pos, sp.best_r});

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

  const std::vector<CorrPoint> &m_CorrPoints = *pts;
  // Use float period for accurate phase — avoids drift from integer rounding.
  const double T_f = (m_CorrPeriodFloat > 0.0) ? m_CorrPeriodFloat
                                                : (double)m_CorrPeriodSamples;
  // Guard: round() can produce exactly T_int when fmod result is just below T_f.
  unsigned phi = (unsigned)std::round(std::fmod((double)loop_pos, T_f))
                 % m_CorrPeriodSamples;
  unsigned r_interp;

  if (m_CorrPoints.size() == 1 || loop_pos <= m_CorrPoints.front().loop_pos) {
    r_interp = m_CorrPoints.front().best_r;

  } else if (loop_pos >= m_CorrPoints.back().loop_pos) {
    r_interp = m_CorrPoints.back().best_r;

  } else {
    unsigned idx = 0;
    while (idx + 1 < m_CorrPoints.size()
           && m_CorrPoints[idx + 1].loop_pos <= loop_pos)
      idx++;

    const CorrPoint &p0 = m_CorrPoints[idx];
    const CorrPoint &p1 = m_CorrPoints[idx + 1];

    // Circular interpolation on [0, T): take the shortest arc.
    // best_r values are stored modulo T, so both are in [0, T).
    int diff    = (int)p1.best_r - (int)p0.best_r;
    int half_T  = (int)m_CorrPeriodSamples / 2;
    if (diff >  half_T) diff -= (int)m_CorrPeriodSamples;
    if (diff < -half_T) diff += (int)m_CorrPeriodSamples;

    double t = (double)(loop_pos - p0.loop_pos)
             / (double)(p1.loop_pos - p0.loop_pos);

    // Residual branch-jump > T/4: step function instead of interpolation.
    // Interpolating between branches produces values on neither branch —
    // a hard midpoint-switch is always better than a meaningless average.
    if (std::abs(diff) > (int)m_CorrPeriodSamples / 4) {
      r_interp = (t < 0.5) ? p0.best_r : p1.best_r;
    } else {
      int r_signed = (int)p0.best_r + (int)std::round(t * diff);
      // Fold into [0, T) — r_signed can go slightly negative when diff < 0
      r_interp = (unsigned)((r_signed % (int)m_CorrPeriodSamples
                             + (int)m_CorrPeriodSamples)
                            % (int)m_CorrPeriodSamples);
    }
  }

  // Fold into [0, T): r_interp and phi are both in [0, T), sum in [0, 2T).
  // Modulo ensures the earliest phase-correct release entry point is used.
  return (r_interp + phi) % m_CorrPeriodSamples;
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
