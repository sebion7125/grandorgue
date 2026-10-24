/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOTestSoundFader.h"

#include <vector>

#include "sound/GOCrossfadeMode.h"
#include "sound/GOCrossfadeParam.h"
#include "sound/playing/GOSoundFader.h"
#include "sound/playing/GOSoundFilter.h"
#include "sound/playing/GOSoundResampleBatch.h"
#include "sound/playing/GOSoundToneBalanceFilter.h"

#include <cmath>

const std::string GOTestSoundFader::TEST_NAME = "GOTestSoundFader";

void GOTestSoundFader::TestFillFrameGainsMatchesScalar() {
  constexpr unsigned N_FRAMES_PER_BLOCK = 50;
  constexpr unsigned N_BLOCKS = 16;
  constexpr unsigned N_FRAMES = N_FRAMES_PER_BLOCK * N_BLOCKS;

  const GOCrossfadeMode savedMode = GOAudioParams::GetCrossfadeMode();
  const GOCrossfadeMode modes[] = {
    GOCrossfadeMode::Sin2,
    GOCrossfadeMode::SinEqualPower,
    GOCrossfadeMode::SqrtEqualPower,
    GOCrossfadeMode::X2};

  // Deterministic, per-position discriminable stereo input.
  std::vector<float> in(N_FRAMES * 2);

  for (unsigned i = 0; i < N_FRAMES * 2; i++)
    in[i] = 0.25f + 0.5f * float((i * 7) % 41) / 41.0f;

  for (const GOCrossfadeMode mode : modes) {
    GOAudioParams::SetCrossfadeMode(mode);

    GOSoundFader scalarFader, batchFader;

    // Fade-in over 180 frames (crosses block boundaries).
    scalarFader.Setup(0.8f, 1.0f, 180);
    batchFader.Setup(0.8f, 1.0f, 180);

    GOAssert(
      scalarFader.NeedsNonLinearProcessing(),
      "fader should dispatch non-linear under a non-linear mode");

    std::vector<float> outScalar(N_FRAMES * 2, 0.0f);
    std::vector<float> outBatch(N_FRAMES * 2, 0.0f);
    float gains[N_FRAMES_PER_BLOCK];

    for (unsigned blockI = 0; blockI < N_BLOCKS; blockI++) {
      // Block 6: start a 240-frame fade-out (overlaps remaining
      // blocks and COMPLETES before the test ends). Block 9: change the
      // external volume so its smoothing ramp is covered too.
      if (blockI == 6) {
        scalarFader.StartDecreasingVolume(240);
        batchFader.StartDecreasingVolume(240);
      }
      const float extVol = blockI >= 9 ? 0.7f : 1.0f;
      const unsigned off = blockI * N_FRAMES_PER_BLOCK;

      scalarFader.ProcessAndAccumulate(
        N_FRAMES_PER_BLOCK, &in[off * 2], &outScalar[off * 2], extVol);

      GOAssert(
        batchFader.FillFrameGains(N_FRAMES_PER_BLOCK, extVol, gains, 1),
        GOFormat(
          "FillFrameGains should succeed (templates bound; mode={}, "
          "block={})",
          (int)mode,
          blockI));
      // The batch kernel's contract for one lane: out += in * gain(i),
      // in the same per-frame order as the scalar loop.
      for (unsigned i = 0; i < N_FRAMES_PER_BLOCK; i++) {
        outBatch[(off + i) * 2] += in[(off + i) * 2] * gains[i];
        outBatch[(off + i) * 2 + 1] += in[(off + i) * 2 + 1] * gains[i];
      }

      for (unsigned i = 0; i < N_FRAMES_PER_BLOCK * 2; i++) {
        const unsigned k = off * 2 + i;

        GOAssert(
          outScalar[k] == outBatch[k],
          GOFormat(
            "scalar/batch gain divergence (mode={}, block={}, sample={}): "
            "{} != {}",
            (int)mode,
            blockI,
            i,
            outScalar[k],
            outBatch[k]));
      }
      GOAssert(
        scalarFader.IsSilent() == batchFader.IsSilent(),
        GOFormat(
          "IsSilent() diverged (mode={}, block={})", (int)mode, blockI));
    }
    // The 240-frame fade-out started at block 6 (frame 300) ends at frame
    // 540 < 800: both faders must have completed it.
    GOAssert(
      scalarFader.IsSilent() && batchFader.IsSilent(),
      GOFormat("both faders should be silent after the fade-out "
               "(mode={})",
               (int)mode));
  }
  GOAudioParams::SetCrossfadeMode(savedMode);
}

#ifdef GO_RESAMPLE_BATCH_AVX2
// The kernel's fixed summation tree (see HorizontalSum8): (low+high),
// then two hadd steps.
static float refHsum8(const float v[8]) {
  const float s0 = v[0] + v[4];
  const float s1 = v[1] + v[5];
  const float s2 = v[2] + v[6];
  const float s3 = v[3] + v[7];

  return (s0 + s1) + (s2 + s3);
}
#endif

void GOTestSoundFader::TestFilterFadeAccumulateMatchesReference() {
#ifdef GO_RESAMPLE_BATCH_AVX2
  if (!GOSoundResampleBatch::CpuSupported())
    return;

  constexpr unsigned LANES = GOSoundResampleBatch::LANES;
  constexpr unsigned N_FRAMES = 64;
  constexpr unsigned N_BLOCKS = 3;
  constexpr unsigned N_ACTIVE = 3;

  // Three distinct ToneBalance settings (HPF, LPF, HPF), lanes 3..7 inert.
  GOSoundToneBalanceFilter tb[N_ACTIVE];
  GOSoundFilter::FilterState states[N_ACTIVE];
  const int8_t tbValues[N_ACTIVE] = {40, -55, 10};

  double b0[LANES] = {0}, b1[LANES] = {0}, a1[LANES] = {0};
  double st[MAX_OUTPUT_CHANNELS][LANES] = {{0}};
  double refSt[MAX_OUTPUT_CHANNELS][LANES] = {{0}};

  for (unsigned l = 0; l < N_ACTIVE; l++) {
    tb[l].SetFilterSamplerate(48000);
    tb[l].Init(tbValues[l]);
    states[l].Init(tb[l].GetFilter());
    GOAssert(states[l].IsToApply(), "tone balance filter should be active");

    float stF[2];

    states[l].ExportBatch(b0[l], b1[l], a1[l], stF);
    GOAssert(b0[l] != 0.0, "exported coefficients should be non-trivial");
  }

  // Deterministic lane data + per-frame gains; one lane gets a classic
  // scalar shadow for a semantic (epsilon) cross-check.
  std::vector<float> laneStore(
    MAX_OUTPUT_CHANNELS * N_FRAMES * LANES, 0.0f);
  float *const laneOut[MAX_OUTPUT_CHANNELS]
    = {laneStore.data(), laneStore.data() + (size_t)N_FRAMES * LANES};
  std::vector<float> gains(N_FRAMES * LANES, 0.0f);
  std::vector<float> outKernel(N_FRAMES * 2, 0.0f);
  std::vector<float> outRef(N_FRAMES * 2, 0.0f);
  std::vector<float> scalarShadow(N_FRAMES * 2, 0.0f);
  std::vector<float> outScalar(N_FRAMES * 2, 0.0f);
  GOSoundFilter::FilterState shadowState;
  // Lane 0's kernel-op reference chain, carried across blocks like the
  // kernel state.
  double lane0St[2] = {0, 0};

  shadowState.Init(tb[0].GetFilter());

  for (unsigned blockI = 0; blockI < N_BLOCKS; blockI++) {
    for (unsigned j = 0; j < N_FRAMES; j++)
      for (unsigned l = 0; l < LANES; l++) {
        const unsigned k = (blockI * N_FRAMES + j) * 7 + l * 3;

        for (unsigned c = 0; c < 2; c++)
          laneOut[c][(size_t)j * LANES + l]
            = 0.5f + 0.4f * float((k + c * 11) % 53) / 53.0f;
        gains[(size_t)j * LANES + l]
          = l < N_ACTIVE ? 0.3f + 0.5f * float((k * 5) % 31) / 31.0f : 0.0f;
      }

    std::fill(outKernel.begin(), outKernel.end(), 0.0f);
    std::fill(outRef.begin(), outRef.end(), 0.0f);
    GOSoundResampleBatch::FilterFadeAccumulate(
      b0, b1, a1, st, laneOut, gains.data(), 2, N_FRAMES, outKernel.data());

    // Reference: exactly the kernel's op sequence, scalar per lane.
    for (unsigned j = 0; j < N_FRAMES; j++)
      for (unsigned c = 0; c < 2; c++) {
        float contrib[LANES];

        for (unsigned l = 0; l < LANES; l++) {
          const float sF = laneOut[c][(size_t)j * LANES + l]
            * gains[(size_t)j * LANES + l];
          const double s = (double)sF;
          const double out = std::fma(b0[l], s, refSt[c][l]);

          refSt[c][l] = std::fma(b1[l], s, -(a1[l] * out));
          contrib[l] = (float)out;
        }
        outRef[j * 2 + c] += refHsum8(contrib);
      }

    for (unsigned i = 0; i < N_FRAMES * 2; i++)
      GOAssert(
        outKernel[i] == outRef[i],
        GOFormat(
          "filter kernel/reference divergence (block={}, sample={}): "
          "{} != {}",
          blockI,
          i,
          outKernel[i],
          outRef[i]));
    for (unsigned c = 0; c < 2; c++)
      for (unsigned l = 0; l < LANES; l++)
        GOAssert(
          st[c][l] == refSt[c][l],
          GOFormat("filter state divergence (c={}, lane={})", c, l));

    /* Semantic cross-check for lane 0 against the CLASSIC scalar detour
     * (scaled = x * gain in float; FilterState::ProcessBuffer with its
     * per-frame float state truncation). The kernel keeps double state
     * within a pass, so the two may differ by sub-ulp rounding that
     * decays with the filter pole - compare with a small relative
     * epsilon. This anchors the kernel's SEMANTICS (gain before filter,
     * same coefficients) to the path it replaces. */
    for (unsigned j = 0; j < N_FRAMES; j++)
      for (unsigned c = 0; c < 2; c++)
        scalarShadow[j * 2 + c]
          = laneOut[c][(size_t)j * LANES] * gains[(size_t)j * LANES];
    shadowState.ProcessBuffer(N_FRAMES, scalarShadow.data());
    for (unsigned j = 0; j < N_FRAMES; j++)
      for (unsigned c = 0; c < 2; c++) {
        const float sF
          = laneOut[c][(size_t)j * LANES] * gains[(size_t)j * LANES];
        const double sD = (double)sF;
        const double out = std::fma(b0[0], sD, lane0St[c]);

        lane0St[c] = std::fma(b1[0], sD, -(a1[0] * out));

        const float classic = scalarShadow[j * 2 + c];
        const float kernelLane0 = (float)out;
        const float mag = std::max(1.0f, std::fabs(classic));

        GOAssert(
          std::fabs(kernelLane0 - classic) <= 1e-4f * mag,
          GOFormat(
            "kernel/classic filter semantics diverged (block={}, frame={}, "
            "c={}): {} != {}",
            blockI,
            j,
            c,
            kernelLane0,
            classic));
      }
  }
  (void)outScalar;
#endif
}

void GOTestSoundFader::run() {
  TestFillFrameGainsMatchesScalar();
  TestFilterFadeAccumulateMatchesReference();
}
