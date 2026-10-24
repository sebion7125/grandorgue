/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDSUBBLOCKGRID_H
#define GOSOUNDSUBBLOCKGRID_H

#include <cstdint>
#include <vector>

// The V3 render-subblock bin ladder (2026-07-09 redesign, superseding the
// former fixed SUBBLOCK_SAMPLES=128 grid constant that lived here).
//
// Every voice picks its private decoded-render-block length at note start
// from this ladder, according to its real resampling factor: a block must
// cover at least one full audio callback's source consumption (otherwise
// the voice crosses block boundaries mid-callback and falls back to
// unbatched local rebuilds every period), but should not be much larger
// (never decode more than ~2% beyond what the callback needs - the user's
// 2% rule; large blocks also concentrate rebuild cost into single
// callbacks, causing jitter).
//
// Ladder: s_0 = base (the engine's real samples_per_buffer),
// s_{k+1} = ceil(s_k * 1.02), capped at MAX_FACTOR * base (~2 octaves of
// combined repitch + source/device sample-rate mismatch). Voices needing
// more than the top rung opt out of the subblock path entirely at note
// start (the direct V3 decode path stays correct) - capping them instead
// would just make them miss on every callback.
//
// The grid itself is anchored per voice at its current segment start (see
// GOSoundStream::m_BlockAnchor), NOT at a global position-0 modulo grid -
// so after every loop wrap a block begins exactly at the loop start, where
// a StartSegmentV3 snapshot already exists, and the ladder needs no
// section-side grid-floor seed snapshots at all.
//
// SetBase() is called once from the engine setup (before any voice
// starts); everything else is read-only afterwards, so no locking.
class GOSoundSubblockBins {
private:
  static inline std::vector<unsigned> s_steps;
  static inline unsigned s_base = 0;
  static inline unsigned s_DeviceSampleRate = 0;

public:
  // Top rung = MAX_FACTOR * base: 4x source consumption covers 2 octaves
  // of repitch incl. sample-rate mismatch (e.g. a 96kHz source on a
  // 44.1kHz device pitched up an octave is ~4.4x - already a constructed
  // extreme; anything beyond opts out).
  static constexpr unsigned MAX_FACTOR = 4;

  static void SetBase(unsigned baseSamples, unsigned deviceSampleRate = 0) {
    s_base = baseSamples;
    s_DeviceSampleRate = deviceSampleRate;
    s_steps.clear();
    if (baseSamples == 0)
      return;

    const unsigned maxBin = MAX_FACTOR * baseSamples;

    for (unsigned s = baseSamples; s < maxBin; s = (s * 102 + 99) / 100)
      s_steps.push_back(s);
    s_steps.push_back(maxBin);
  }

  static unsigned GetBase() { return s_base; }
  static unsigned GetMaxBin() { return s_steps.empty() ? 0 : s_steps.back(); }
  static unsigned GetStepCount() { return (unsigned)s_steps.size(); }
  static unsigned GetStep(unsigned index) { return s_steps[index]; }

  /**
   * Smallest ladder rung >= neededSamples; the voice's private block
   * length. Returns 0 if neededSamples exceeds the top rung (the voice
   * must opt out of the subblock path) or the ladder is not set up.
   * outIndex (optional) receives the rung's ladder index - used as the
   * batch-grouping key by GOSoundGroupTask.
   */
  static unsigned Lookup(unsigned neededSamples, unsigned *outIndex = nullptr) {
    if (s_steps.empty() || neededSamples > s_steps.back())
      return 0;

    // Linear scan: ~70 rungs at 2% steps, hit once per note start (not in
    // the per-sample hot path), and the overwhelming majority of voices
    // (ratio near 1.0) match within the first few rungs.
    unsigned i = 0;

    while (s_steps[i] < neededSamples)
      i++;
    if (outIndex)
      *outIndex = i;
    return s_steps[i];
  }

  /**
   * Prospective rung of a section, from its source/device sample-rate
   * ratio alone (per-note tuning/detune aren't known before a voice
   * starts) - the Audio Monitor's "cache bins" histogram classifies the
   * loaded set's sections through this, on demand. Same worst-case-
   * consumption formula as GOSoundStream::ApplySubblockBin. Returns the
   * rung's bin length, or 0 for opt-out (ratio beyond the top rung) /
   * unset ladder; outIndex receives the rung index on success.
   */
  static unsigned ClassifySectionRate(
    unsigned sectionSampleRate, unsigned *outIndex = nullptr) {
    if (s_base == 0 || s_DeviceSampleRate == 0)
      return 0;

    const unsigned needed = (unsigned)(((uint64_t)s_base * sectionSampleRate
                                        + s_DeviceSampleRate - 1)
                                       / s_DeviceSampleRate);

    return Lookup(needed, outIndex);
  }
};

#endif
