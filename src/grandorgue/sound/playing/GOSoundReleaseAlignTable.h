/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2024 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDRELEASEALIGNTABLE_H
#define GOSOUNDRELEASEALIGNTABLE_H

#include <cstdint>
#include <vector>

#include "GOSoundAudioSection.h"

class GOCache;
class GOCacheWriter;

#define PHASE_ALIGN_DERIVATIVES 2
#define PHASE_ALIGN_AMPLITUDES 32
#define PHASE_ALIGN_MIN_FREQUENCY 20 /* Hertz */

// Ranks with HarmonicNumber >= this are treated as potential mixtures and use
// autocorrelation for period estimation. Quint 1⅓' = HarmonicNumber 48 is the
// lowest rank where repetition can occur.
static constexpr unsigned CORR_MIXTURE_HARMONIC_THRESHOLD = 48;

class GOSoundReleaseAlignTable {
private:
  // ── Legacy (unchanged) ──────────────────────────────────────────────────
  int m_PhaseAlignMaxAmplitude;
  int m_PhaseAlignMaxDerivative;
  int m_PositionEntries[PHASE_ALIGN_DERIVATIVES][PHASE_ALIGN_AMPLITUDES];

  // ── Sparse correlation LUT ──────────────────────────────────────────────
  // Up to 10 support points; GetPositionForCorrelation() interpolates between
  // them on the circle [0, T) using the shortest-arc path.
  struct CorrPoint {
    uint32_t loop_pos; // absolute sample position in loop (= n * period_samples)
    uint16_t best_r;   // best release offset r* in samples, in [0, T)
  };
  std::vector<CorrPoint> m_CorrPoints;
  uint32_t m_CorrPeriodSamples; // period length in samples
  uint32_t m_CorrCrossfadeLen;  // crossfade window length in samples

public:
  GOSoundReleaseAlignTable();
  ~GOSoundReleaseAlignTable();

  bool Load(GOCache &cache);
  bool Save(GOCacheWriter &cache);

  void ComputeTable(
    const GOSoundAudioSection &m_release,
    int phase_align_max_amplitude,
    int phase_align_max_derivative,
    unsigned int sample_rate,
    unsigned start_position);

  unsigned GetPositionFor(
    int history[BLOCK_HISTORY][MAX_OUTPUT_CHANNELS]) const;

  void ComputeCorrelationLut(
    const GOSoundAudioSection &loop,
    const GOSoundAudioSection &release,
    unsigned crossfade_len,
    unsigned sample_rate,
    float sample_freq_hz,
    unsigned harmonic_number);

  unsigned GetPositionForCorrelation(unsigned loop_pos) const;

  unsigned GetPeriodSamples() const { return m_CorrPeriodSamples; }
};

#endif /* GOSOUNDRELEASEALIGNTABLE_H */
