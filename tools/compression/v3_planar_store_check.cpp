// v3_planar_store_check.cpp
//
// Bit-parity check for the fused planar store (bench F2 in production):
// DecodeBatchPlanarMidCapture must produce, for every lane/channel/sample,
// exactly the value DecodeBatchInterleavedMidCapture produces - same
// mid-capture states, same final lane caches. Synthetic organ-ish signals,
// 8 lanes, phase-misaligned starts, odd (non-multiple-of-8) capture points
// and sample counts to exercise the scalar tail path.
//
// Build:
//   g++ -O3 -mavx2 -std=gnu++17 -I ../../src/grandorgue  \
//     -I ../../src/v3codec \
//       v3_planar_store_check.cpp -o /tmp/claude-1000/v3planarcheck

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "GOSoundCompressionCacheV3.h"
#include "GOSoundCompressionCacheV3Batch.h"

constexpr unsigned LANES = GOSoundCompressionCacheV3Batch::LANES;

int main() {
  if (!GOSoundCompressionCacheV3Batch::CpuSupported()) {
    printf("no AVX2 - skipped\n");
    return 0;
  }

  const unsigned totalFrames = 40000;
  const unsigned channels = 2;

  // Synthetic per-lane signals + full encode (as AudioSection::Compress).
  std::vector<std::vector<int32_t>> pcm(LANES);
  std::vector<std::vector<unsigned char>> encoded(LANES);
  for (unsigned l = 0; l < LANES; l++) {
    pcm[l].resize((size_t)totalFrames * channels);
    for (unsigned i = 0; i < totalFrames; i++)
      for (unsigned c = 0; c < channels; c++) {
        const double ph = i * (0.013 + 0.0011 * l) + c * 0.4;
        pcm[l][(size_t)i * channels + c] = (int32_t)(3000000.0
          * (std::sin(ph) + 0.4 * std::sin(3.01 * ph)
             + 0.2 * std::sin(9.02 * ph)));
      }
    encoded[l].resize(
      GOSoundCompressionCacheV3::WorstCaseSize(totalFrames, channels));
    GOSoundCompressionCacheV3 enc;
    const unsigned written = enc.Encode(
      encoded[l].data(), totalFrames, channels, nullptr, 0,
      [&](unsigned idx, unsigned ch) {
        return (int)pcm[l][(size_t)idx * channels + ch];
      },
      nullptr);
    encoded[l].resize(written);
  }

  unsigned failures = 0;

  // Several scenarios: varying lane counts, spans, capture points.
  const struct {
    unsigned numLanes, nSamples, captureAt;
  } cases[] = {
    {8, 136, 128}, {8, 531, 523}, {5, 136, 129}, {2, 45, 41},
    {8, 2100, 2093}, {3, 8, 5}, {8, 137, 130},
  };

  for (const auto &tc : cases) {
    // Phase-misaligned starts per lane.
    GOSoundCompressionCacheV3 cA[LANES], cB[LANES];
    GOSoundCompressionCacheV3Batch::Lane lA[LANES], lB[LANES];
    for (unsigned l = 0; l < tc.numLanes; l++) {
      const unsigned start = (l * 7919u + 1234u) % (totalFrames / 2);
      cA[l].Init();
      cA[l].DecompressTo(start, encoded[l].data(), channels);
      cB[l] = cA[l];
      lA[l].cache = &cA[l];
      lA[l].channels = channels;
      lB[l].cache = &cB[l];
      lB[l].channels = channels;
    }

    const unsigned stride = ((tc.nSamples + 15) & ~7u); // 8-aligned + Reserve
    std::vector<int32_t> il[2], pl[2];
    for (unsigned c = 0; c < 2; c++) {
      il[c].assign((size_t)tc.nSamples * LANES, 0x7abcdef0);
      pl[c].assign((size_t)stride * LANES, 0x7abcdef0);
    }
    int32_t *ilPtr[MAX_OUTPUT_CHANNELS] = {il[0].data(), il[1].data()};
    int32_t *plPtr[MAX_OUTPUT_CHANNELS] = {pl[0].data(), pl[1].data()};
    GOSoundCompressionCacheV3 midA[LANES], midB[LANES];

    GOSoundCompressionCacheV3Batch::DecodeBatchInterleavedMidCapture(
      lA, tc.numLanes, tc.nSamples, tc.captureAt, midA, ilPtr);
    GOSoundCompressionCacheV3Batch::DecodeBatchPlanarMidCapture(
      lB, tc.numLanes, tc.nSamples, tc.captureAt, midB, plPtr, stride);

    unsigned bad = 0;
    for (unsigned c = 0; c < channels; c++)
      for (unsigned l = 0; l < tc.numLanes; l++)
        for (unsigned s = 0; s < tc.nSamples; s++) {
          const int32_t a = il[c][(size_t)s * LANES + l];
          const int32_t b = pl[c][(size_t)l * stride + s];
          if (a != b && bad++ < 3)
            printf(
              "  MISMATCH lanes=%u n=%u cap=%u: c%u l%u s%u  %d != %d\n",
              tc.numLanes, tc.nSamples, tc.captureAt, c, l, s, a, b);
        }
    for (unsigned l = 0; l < tc.numLanes; l++) {
      // Mid-capture states and final caches must match field-for-field.
      if (
        memcmp(&midA[l], &midB[l], sizeof(midA[l])) != 0
        || memcmp(&cA[l], &cB[l], sizeof(cA[l])) != 0) {
        printf(
          "  STATE MISMATCH lanes=%u n=%u cap=%u lane %u\n",
          tc.numLanes, tc.nSamples, tc.captureAt, l);
        bad++;
      }
    }
    printf(
      "case lanes=%u n=%-5u cap=%-5u : %s\n",
      tc.numLanes, tc.nSamples, tc.captureAt, bad ? "FAIL" : "ok");
    failures += bad ? 1 : 0;
  }

  printf(failures ? "\nFAILURES: %u\n" : "\nall cases bit-identical\n",
    failures);
  return failures ? 1 : 0;
}
