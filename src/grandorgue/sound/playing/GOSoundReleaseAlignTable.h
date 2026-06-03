/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2024 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDRELEASEALIGNTABLE_H
#define GOSOUNDRELEASEALIGNTABLE_H

#include <cstdint>
#include <ostream>
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
public:
  // Public so callers (LUT cache loader, generator) can build point lists
  // without going through ComputeCorrelationLut.
  struct CorrPoint {
    uint32_t loop_pos; // absolute sample position in loop (= n * period_samples)
    uint16_t best_r;   // best release offset r* in samples, in [0, T)
  };

private:
  // ── Legacy (unchanged) ──────────────────────────────────────────────────
  int m_PhaseAlignMaxAmplitude;
  int m_PhaseAlignMaxDerivative;
  int m_PositionEntries[PHASE_ALIGN_DERIVATIVES][PHASE_ALIGN_AMPLITUDES];

  // ── Sparse correlation LUT ──────────────────────────────────────────────
  // One LUT per attack joinable; p_Attack is the runtime pointer used for
  // lookup (null = applies to all attacks / loaded from cache).
  struct AttackLut {
    const GOSoundAudioSection *p_Attack = nullptr;
    std::vector<CorrPoint> points;
  };
  std::vector<AttackLut> m_CorrLuts;
  uint32_t m_CorrPeriodSamples; // period length in samples (rounded integer)
  double   m_CorrPeriodFloat;   // exact float period: sample_rate / freq_hz
  uint32_t m_CorrCrossfadeLen;  // crossfade window length in samples

  const std::vector<CorrPoint> *FindLut(
    const GOSoundAudioSection *p_Attack) const;

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

  // Append one LUT for the given attack joinable. Call once per joinable.
  // min_key_press_ms / max_key_press_ms restrict the LUT to the time window
  // in which this release is actually triggered (0 = no restriction).
  void ComputeCorrelationLut(
    const GOSoundAudioSection &loop,
    const GOSoundAudioSection &release,
    unsigned crossfade_len,
    unsigned sample_rate,
    float    sample_freq_hz,
    unsigned harmonic_number,
    unsigned min_key_press_ms = 0,
    unsigned max_key_press_ms = 0);

  // Restore runtime attack pointers after cache load (in joinable order).
  void AssignAttackPointers(
    const std::vector<const GOSoundAudioSection *> &attacks);

  unsigned GetPositionForCorrelation(
    unsigned loop_pos, const GOSoundAudioSection *p_Attack = nullptr) const;

  unsigned GetPeriodSamples() const { return m_CorrPeriodSamples; }

  // Returns true if at least one correlation LUT is present.
  bool HasCorrLut() const { return !m_CorrLuts.empty(); }

  // Returns the points of the first LUT, or nullptr if none.
  // Used by the cache generator to extract quality-filtered LUT data.
  const std::vector<CorrPoint> *GetFirstLutPoints() const {
    return m_CorrLuts.empty() ? nullptr : &m_CorrLuts[0].points;
  }

  // Replace any existing correlation LUTs with a single cached LUT that
  // applies to all attacks (p_Attack = nullptr → FindLut() fallback).
  // No-op if m_CorrPeriodSamples is not yet set or points is empty.
  void OverrideCorrLutsFromCache(std::vector<CorrPoint> points);

#if __has_include("GOLogReleaseAlignVerbose.h")
  // Write LUT support points for the given attack to out (for debug logging).
  void DumpLutPoints(
    const GOSoundAudioSection *p_Attack, std::ostream &out) const;
  // Copy LUT points into caller-supplied arrays (no allocation, safe in audio
  // thread). Returns number of points actually copied (<= max_points).
  unsigned CopyLutPoints(
    const GOSoundAudioSection *p_Attack,
    uint32_t *out_pos,
    uint16_t *out_r,
    unsigned  max_points) const;
#endif
};

#endif /* GOSOUNDRELEASEALIGNTABLE_H */
