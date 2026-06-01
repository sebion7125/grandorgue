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

  const unsigned window = len - max_period;
  float    best_score = -2.f;
  unsigned best_lag   = min_period;

  for (unsigned lag = min_period; lag <= max_period; lag++) {
    float s = NormalizedDotProduct(samples, samples, lag, window);
    if (s > best_score) {
      best_score = s;
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
  float sample_freq_hz,
  unsigned harmonic_number) {
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

  // Mixtures: re-estimate T via autocorrelation only on first call.
  if (first_call && harmonic_number >= CORR_MIXTURE_HARMONIC_THRESHOLD) {
    const unsigned T_formula = m_CorrPeriodSamples;
    const unsigned min_p = std::max(8u, T_formula / 2);
    const unsigned max_p = std::min(T_formula * 4, sample_rate / 20u);
    const unsigned loop_len_full = loop_section.GetLength();

    // Take a stable window from the middle of the loop (8× max_p samples)
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
      m_CorrPeriodFloat = m_CorrPeriodSamples; // autocorr gives integer for now
    }
  }

  if (m_CorrPeriodSamples < 16)
    return;

  // Correlation window: cap at 2 periods — more adds noise, not signal.
  // crossfade_len is a sample-set parameter and is not modified.
  unsigned window_len = std::min(crossfade_len, 2 * m_CorrPeriodSamples);
  if (window_len < 4)
    return;

  unsigned loop_len    = loop_section.GetLength();
  unsigned release_len = release_section.GetLength();
  unsigned loop_ch     = loop_section.GetChannels();
  unsigned rel_ch      = release_section.GetChannels();

  if (loop_len < window_len)
    return;

  // Search range: two periods of the release.
  // Covers all phase alignments plus any recording offset between loop and release.
  // best_r is stored modulo T, so the two-equivalent-solutions problem is avoided
  // at lookup time while the wider search finds the globally best phase match.
  if (release_len < window_len)
    return;
  unsigned r_max
    = std::min(2 * m_CorrPeriodSamples, release_len - window_len);
  if (r_max == 0)
    return;

  // ── Adaptive support-point placement ────────────────────────────────
  // Phase 1 (dense): sample every DENSE_STEP periods until r stabilises
  //   or MAX_DENSE_N periods are sampled.  Stable = last STABLE_WIN
  //   consecutive best_r values all within stable_thresh (circular).
  //   Threshold: T/8 for regular pipes, T/6 for mixtures (minimum 4).
  // Phase 2 (sparse): N_SPARSE points evenly from the end of the dense
  //   phase to n_total-1.
  // Phase 3 (gap fill): insert one midpoint between adjacent pairs whose
  //   best_r values deviate by more than T/4, bounded by MAX_TOTAL.
  // Short loops (n_total ≤ 30): even distribution (unchanged).
  // ─────────────────────────────────────────────────────────────────────

  unsigned n_total = loop_len / m_CorrPeriodSamples;
  if (n_total < 4)
    return;

  // Downsampling factor: for low-pitched pipes take every ds-th sample.
  // Target downsampled period ≈ 500 samples; capped at 4.
  // Reduces ops by ds² with negligible quality loss since both signals
  // are downsampled identically and phase alignment is preserved.
  const unsigned ds = (GOAudioParams::GetCorrLutDownsampling()
                       && m_CorrPeriodSamples >= 500u)
    ? std::min(4u, m_CorrPeriodSamples / 500u)
    : 1u;
  const unsigned window_len_d = std::max(4u, window_len / ds);
  const unsigned r_max_d      = std::max(1u, r_max / ds);

  // loop_mono covers n_total-1 so the sparse phase can reach any position.
  // Same coverage as the previous block-2 approach.
  unsigned loop_needed = std::min(
    loop_len,
    (unsigned)std::round((n_total - 1) * m_CorrPeriodFloat) + window_len);
  unsigned loop_needed_d = loop_needed / ds + 1;
  GOSoundCompressionCache loop_cache;
  loop_cache.Init();
  std::vector<float> loop_mono(loop_needed_d, 0.f);
  for (unsigned i = 0; i < loop_needed_d; i++) {
    unsigned orig = i * ds;
    if (orig >= loop_len) break;
    float s = 0.f;
    for (unsigned c = 0; c < loop_ch; c++)
      s += (float)loop_section.GetSample(orig, c, &loop_cache);
    loop_mono[i] = s / loop_ch;
  }

  // Extract downsampled release mono (stride = ds).
  unsigned release_needed_d = (r_max + window_len) / ds + 1;
  GOSoundCompressionCache rel_cache;
  rel_cache.Init();
  std::vector<float> release_mono(release_needed_d, 0.f);
  for (unsigned i = 0; i < release_needed_d; i++) {
    unsigned orig = i * ds;
    if (orig >= release_len) break;
    float s = 0.f;
    for (unsigned c = 0; c < rel_ch; c++)
      s += (float)release_section.GetSample(orig, c, &rel_cache);
    release_mono[i] = s / rel_ch;
  }

  // Compute CorrPoint at period index n.
  // Forward window starts at crossfade_start so that release[r] and
  // attack[crossfade_start] are compared head-to-head.
  // Returns {crossfade_start, 0} if the window falls outside loop_mono.
  auto corr_at = [&](unsigned n) -> CorrPoint {
    const unsigned cs  = (unsigned)std::round(n * m_CorrPeriodFloat);
    const unsigned p_d = cs / ds;
    if (p_d + window_len_d > loop_needed_d)
      return {cs, 0u};
    const float *lw = loop_mono.data() + p_d;
    float    best_s = -2.f;
    uint16_t best_r = 0;
    for (unsigned r_d = 0; r_d < r_max_d; r_d++) {
      float sc
        = NormalizedDotProduct(lw, release_mono.data(), r_d, window_len_d);
      if (sc > best_s) {
        best_s = sc;
        best_r = (uint16_t)((r_d * ds) % m_CorrPeriodSamples);
      }
    }
    return {cs, best_r};
  };

  // Circular distance between two best_r values, always in [0, T/2].
  auto circ_dist = [&](uint16_t a, uint16_t b) -> int {
    int d = (int)a - (int)b;
    if (d < 0) d = -d;
    if (d > (int)m_CorrPeriodSamples / 2)
      d = (int)m_CorrPeriodSamples - d;
    return d;
  };

  std::vector<CorrPoint> points;

  if (n_total <= 30) {
    // Short loop: distribute evenly from period 3 to n_total-1.
    std::vector<unsigned> sp;
    for (unsigned i = 0; i < 10 && i < n_total; i++) {
      unsigned p = 3 + i * std::max(1u, (n_total - 3) / 9);
      if (p < n_total)
        sp.push_back(p);
    }
    std::sort(sp.begin(), sp.end());
    sp.erase(std::unique(sp.begin(), sp.end()), sp.end());
    for (unsigned p : sp)
      points.push_back(corr_at(p));

  } else {
    // Long loop: adaptive algorithm.
    constexpr unsigned DENSE_STEP  = 6;
    constexpr unsigned MAX_DENSE_N = 100;
    constexpr unsigned STABLE_WIN  = 4;
    constexpr unsigned N_SPARSE    = 5;
    constexpr unsigned MAX_TOTAL   = 30;

    // Stability threshold: T/8 for regular pipes, T/6 for mixtures.
    const int stable_thresh = std::max(
      (int)m_CorrPeriodSamples
        / (harmonic_number >= CORR_MIXTURE_HARMONIC_THRESHOLD ? 6 : 8),
      4);

    // Phase 1: dense sampling until r stabilises.
    for (unsigned n = 5; n < n_total && n <= MAX_DENSE_N; n += DENSE_STEP) {
      points.push_back(corr_at(n));
      if ((unsigned)points.size() >= STABLE_WIN) {
        bool ok = true;
        for (unsigned k = (unsigned)points.size() - STABLE_WIN;
             k + 1 < (unsigned)points.size() && ok; ++k)
          if (circ_dist(points[k].best_r, points[k + 1].best_r)
              > stable_thresh)
            ok = false;
        if (ok)
          break;
      }
    }
    if (points.empty())
      points.push_back(corr_at(std::min(5u, n_total - 1)));

    // Phase 2: sparse from last dense period to n_total-1.
    const unsigned last_n = (unsigned)std::round(
      points.back().loop_pos / m_CorrPeriodFloat);
    if (last_n + 1 < n_total) {
      const unsigned n_rem =
        std::min(N_SPARSE, MAX_TOTAL - (unsigned)points.size());
      for (unsigned i = 1; i <= n_rem; i++) {
        const unsigned n = last_n + i * (n_total - 1 - last_n) / n_rem;
        if (n > last_n && n < n_total)
          points.push_back(corr_at(n));
      }
    }

    // Phase 3: gap fill — one midpoint per deviating adjacent pair.
    const int gap_thresh = (int)m_CorrPeriodSamples / 4;
    unsigned  idx        = 0;
    while (idx + 1 < points.size() && (unsigned)points.size() < MAX_TOTAL) {
      if (circ_dist(points[idx].best_r, points[idx + 1].best_r)
          > gap_thresh) {
        const unsigned na = (unsigned)std::round(
          points[idx].loop_pos     / m_CorrPeriodFloat);
        const unsigned nb = (unsigned)std::round(
          points[idx + 1].loop_pos / m_CorrPeriodFloat);
        const unsigned nm = (na + nb) / 2;
        if (nm > na && nm < nb) {
          points.insert(points.begin() + idx + 1, corr_at(nm));
          continue;  // recheck new left pair (idx, idx+1=midpoint)
        }
      }
      ++idx;
    }
  }

  if (points.empty())
    return;

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
  unsigned phi = (unsigned)std::round(std::fmod((double)loop_pos, T_f));
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

    int r_signed = (int)p0.best_r + (int)std::round(t * diff);

    // Fold into [0, T) — r_signed can go slightly negative when diff < 0
    r_interp = (unsigned)((r_signed % (int)m_CorrPeriodSamples
                           + (int)m_CorrPeriodSamples)
                          % (int)m_CorrPeriodSamples);
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
