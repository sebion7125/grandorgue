/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOTESTSOUNDFADER_H
#define GOTESTSOUNDFADER_H

#include <string>

#include "GOTest.h"

class GOTestSoundFader : public GOTest {
private:
  static const std::string TEST_NAME;

  /**
   * FillFrameGains (the batch render path's non-linear gain source) must
   * be BIT-IDENTICAL to the scalar ProcessAndAccumulate non-linear path,
   * block by block, across a fade-in, the steady no-envelope phase, an
   * external-volume ramp, a fade-out and past its completion - two
   * faders driven in lockstep, one per path, must produce the same
   * accumulated output and the same IsSilent() progression. Runs for
   * every non-linear crossfade mode; restores the global mode afterwards.
   */
  void TestFillFrameGainsMatchesScalar();

  /**
   * The vectorized ToneBalance pass (FilterFadeAccumulate) must be
   * bit-identical to a scalar reference reproducing its documented op
   * sequence (float pre-gain, double first-order IIR with explicit
   * fma/fmsub, float contribution, fixed hsum tree), including the state
   * carried across blocks. Skipped without AVX2.
   */
  void TestFilterFadeAccumulateMatchesReference();

public:
  std::string GetName() override { return TEST_NAME; }
  void run() override;
};

#endif /* GOTESTSOUNDFADER_H */
