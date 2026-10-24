// fir_tile_bench.cpp
//
// Does tiling help variant D at large buffers? At 512-frame callbacks the
// group's working set (interleaved scratch ~34 KB + planes ~34 KB stereo)
// exceeds the 32 KB L1d: the full-span transpose evicts its own output to
// L2 before the render reads it back. The user's proposal: tile the
// transpose JUST IN TIME into the (full-size, contiguous) plane, render
// the output frames covered by that tile, then transpose the next slice -
// the freshly flipped plane region stays L1-hot for its render.
//
//   T1: producer (decoder store model) -> FULL-span transpose ->
//       per-lane classic render of all frames            (variant D as is)
//   T2: producer -> per 128-output-frame tile: transpose the source rows
//       this tile consumes -> per-lane classic render of the tile
//       (positions persist across tiles in ResamplingPosition)
//
// Both run at BLOCK_FRAMES=512 stereo, 8 lanes. The producer pass models
// the decoder's stores so the scratch starts in the same eviction state
// as production (early lines already in L2).
//
// Build:
//   g++ -O3 -ffast-math -funroll-loops -mavx2 -mfma -std=gnu++20 \
//       -I ../../src/grandorgue -I ../../src/grandorgue/sound/playing \
//       fir_tile_bench.cpp ../../src/grandorgue/sound/playing/GOSoundResample.cpp \
//       -o /tmp/claude-1000/firtile

#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "sound/playing/GOSoundResample.h"
#include "sound/playing/GOSoundResampleBatch.h"

#define GR_UP GOSoundResample::UPSAMPLE_FACTOR

namespace {

constexpr unsigned BLOCK_FRAMES = 512;
constexpr unsigned TILE_FRAMES = 128;
constexpr unsigned LANES = GOSoundResampleBatch::LANES;
constexpr unsigned REPS = 11;
constexpr unsigned ITERS = 24;

using Clock = std::chrono::steady_clock;

double bestSeconds(unsigned reps, auto &&fn) {
  double best = 1e30;
  for (unsigned r = 0; r < reps; r++) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
  }
  return best;
}

volatile float g_sinkF;

struct PlanarVec : GOSoundResample::FloatingSampleVector<2> {
  static constexpr bool kPlanarInt32Taps = true;
  const int32_t *plane[2];
  const int32_t *p = nullptr;

  inline void Seek(unsigned index, uint8_t ch) { p = plane[ch] + index; }
  inline const int32_t *GetPtr() const { return p; }
  inline float NextSample() { return (float)*p++; }
};

void transposeTile(
  const int32_t *src, int32_t *dst, unsigned stride, unsigned s8) {
  const int32_t *rows = src + (size_t)s8 * LANES;
  const __m256i r0 = _mm256_loadu_si256((const __m256i *)(rows + 0 * LANES));
  const __m256i r1 = _mm256_loadu_si256((const __m256i *)(rows + 1 * LANES));
  const __m256i r2 = _mm256_loadu_si256((const __m256i *)(rows + 2 * LANES));
  const __m256i r3 = _mm256_loadu_si256((const __m256i *)(rows + 3 * LANES));
  const __m256i r4 = _mm256_loadu_si256((const __m256i *)(rows + 4 * LANES));
  const __m256i r5 = _mm256_loadu_si256((const __m256i *)(rows + 5 * LANES));
  const __m256i r6 = _mm256_loadu_si256((const __m256i *)(rows + 6 * LANES));
  const __m256i r7 = _mm256_loadu_si256((const __m256i *)(rows + 7 * LANES));
  const __m256i t0 = _mm256_unpacklo_epi32(r0, r1);
  const __m256i t1 = _mm256_unpackhi_epi32(r0, r1);
  const __m256i t2 = _mm256_unpacklo_epi32(r2, r3);
  const __m256i t3 = _mm256_unpackhi_epi32(r2, r3);
  const __m256i t4 = _mm256_unpacklo_epi32(r4, r5);
  const __m256i t5 = _mm256_unpackhi_epi32(r4, r5);
  const __m256i t6 = _mm256_unpacklo_epi32(r6, r7);
  const __m256i t7 = _mm256_unpackhi_epi32(r6, r7);
  const __m256i u0 = _mm256_unpacklo_epi64(t0, t2);
  const __m256i u1 = _mm256_unpackhi_epi64(t0, t2);
  const __m256i u2 = _mm256_unpacklo_epi64(t1, t3);
  const __m256i u3 = _mm256_unpackhi_epi64(t1, t3);
  const __m256i u4 = _mm256_unpacklo_epi64(t4, t6);
  const __m256i u5 = _mm256_unpackhi_epi64(t4, t6);
  const __m256i u6 = _mm256_unpacklo_epi64(t5, t7);
  const __m256i u7 = _mm256_unpackhi_epi64(t5, t7);
  _mm256_storeu_si256((__m256i *)(dst + 0 * (size_t)stride + s8),
    _mm256_permute2x128_si256(u0, u4, 0x20));
  _mm256_storeu_si256((__m256i *)(dst + 1 * (size_t)stride + s8),
    _mm256_permute2x128_si256(u1, u5, 0x20));
  _mm256_storeu_si256((__m256i *)(dst + 2 * (size_t)stride + s8),
    _mm256_permute2x128_si256(u2, u6, 0x20));
  _mm256_storeu_si256((__m256i *)(dst + 3 * (size_t)stride + s8),
    _mm256_permute2x128_si256(u3, u7, 0x20));
  _mm256_storeu_si256((__m256i *)(dst + 4 * (size_t)stride + s8),
    _mm256_permute2x128_si256(u0, u4, 0x31));
  _mm256_storeu_si256((__m256i *)(dst + 5 * (size_t)stride + s8),
    _mm256_permute2x128_si256(u1, u5, 0x31));
  _mm256_storeu_si256((__m256i *)(dst + 6 * (size_t)stride + s8),
    _mm256_permute2x128_si256(u2, u6, 0x31));
  _mm256_storeu_si256((__m256i *)(dst + 7 * (size_t)stride + s8),
    _mm256_permute2x128_si256(u3, u7, 0x31));
}

} // namespace

int main() {
  const GOSoundResample resample;
  const double factors[LANES]
    = {0.97, 0.985, 1.0, 1.01, 1.02, 1.035, 1.05, 1.06};
  const double maxFactor = 1.06;

  const unsigned span
    = ((unsigned)(BLOCK_FRAMES * maxFactor) + 8 + 8 + 7) & ~7u;
  std::vector<int32_t> resid[2], il[2], pl[2];
  for (unsigned c = 0; c < 2; c++) {
    resid[c].resize((size_t)span * LANES);
    il[c].resize((size_t)span * LANES);
    pl[c].resize((size_t)span * LANES);
    for (size_t i = 0; i < resid[c].size(); i++)
      resid[c][i] = (int32_t)((i * 2654435761u) >> 24) - 128;
  }
  std::vector<float> out(BLOCK_FRAMES * 2);

  auto produce = [&](unsigned c) {
    // Lockstep predictor, models the decoder's interleaved stores.
    __m256i v = _mm256_set1_epi32(1000), pr = _mm256_set1_epi32(990),
            la = _mm256_set1_epi32(985);
    const int32_t *rs = resid[c].data();
    int32_t *dst = il[c].data();

    for (unsigned smp = 0; smp < span; smp++) {
      la = pr;
      pr = v;
      const __m256i pred = _mm256_add_epi32(
        pr, _mm256_srai_epi32(_mm256_sub_epi32(pr, la), 1));
      v = _mm256_add_epi32(pred,
        _mm256_loadu_si256((const __m256i *)(rs + (size_t)smp * LANES)));
      _mm256_storeu_si256((__m256i *)(dst + (size_t)smp * LANES), v);
    }
  };

  auto renderLane = [&](
    unsigned l, GOSoundResample::ResamplingPosition &pos, float *dst,
    unsigned nFrames) {
    const GOSoundResample::PolyphaseResampler poly(resample);
    PlanarVec vec;

    vec.plane[0] = pl[0].data() + (size_t)l * span;
    vec.plane[1] = pl[1].data() + (size_t)l * span;
    poly.ResampleBlock<PlanarVec, 2>(pos, vec, dst, nFrames);
  };

  // --- T1: full transpose, then full render ---------------------------------
  const double secFull = bestSeconds(REPS, [&] {
    for (unsigned it = 0; it < ITERS; it++) {
      for (unsigned c = 0; c < 2; c++) {
        produce(c);
        for (unsigned s8 = 0; s8 < span; s8 += 8)
          transposeTile(
            il[c].data(), pl[c].data() + 0, span, s8); // lane stride = span
      }
      // note: transposeTile writes all 8 lanes (dst + l*stride internally)
      for (unsigned l = 0; l < LANES; l++) {
        GOSoundResample::ResamplingPosition pos;

        pos.Init((float)factors[l], 0);
        renderLane(l, pos, out.data(), BLOCK_FRAMES);
      }
      g_sinkF = out[0];
    }
  });

  // --- T2: just-in-time tiles into the same full-size plane -----------------
  const double secTiled = bestSeconds(REPS, [&] {
    for (unsigned it = 0; it < ITERS; it++) {
      for (unsigned c = 0; c < 2; c++)
        produce(c);

      GOSoundResample::ResamplingPosition pos[LANES];

      for (unsigned l = 0; l < LANES; l++)
        pos[l].Init((float)factors[l], 0);

      unsigned rowsDone = 0;

      for (unsigned f = 0; f < BLOCK_FRAMES; f += TILE_FRAMES) {
        // Source rows this tile can touch (fastest lane + taps), rounded up.
        const unsigned rowsNeeded = std::min(
          span,
          ((unsigned)((f + TILE_FRAMES) * maxFactor) + 8 + 8 + 7) & ~7u);

        for (unsigned c = 0; c < 2; c++)
          for (unsigned s8 = rowsDone; s8 < rowsNeeded; s8 += 8)
            transposeTile(il[c].data(), pl[c].data(), span, s8);
        rowsDone = rowsNeeded;
        for (unsigned l = 0; l < LANES; l++)
          renderLane(l, pos[l], out.data() + (size_t)f * 2, TILE_FRAMES);
      }
      g_sinkF = out[0];
    }
  });

  // --- H: FIR+Gain+Accumulate FUSIONIERT (ein Pass, kein temp) --------------
  // Modelliert die Frage vom 13.7.: lohnt es, fuer Steady-Voices den
  // temp-Roundtrip (FIR->1KB->Fader->out) durch direkte Akkumulation im
  // FIR-Loop zu ersetzen? Referenz T1 nutzt renderLane->temp + separatem
  // Accumulate-Pass (wie Produktion Variante D).
  std::vector<float> acc(BLOCK_FRAMES * 2, 0.f), temp(BLOCK_FRAMES * 2);
  const double secTwoPass = bestSeconds(REPS, [&] {
    for (unsigned it = 0; it < ITERS; it++) {
      for (unsigned c = 0; c < 2; c++) produce(c);
      for (unsigned s8 = 0; s8 < span; s8 += 8)
        for (unsigned c = 0; c < 2; c++)
          transposeTile(il[c].data(), pl[c].data(), span, s8);
      for (unsigned l = 0; l < LANES; l++) {
        GOSoundResample::ResamplingPosition pos;
        pos.Init((float)factors[l], 0);
        renderLane(l, pos, temp.data(), BLOCK_FRAMES);
        const float g = 0.7f;
        for (unsigned j = 0; j < BLOCK_FRAMES * 2; j++)
          acc[j] += temp[j] * g;   // (Produktions-Pass ist 8-wide; gcc
                                    // vektorisiert diese Form ebenfalls)
      }
      g_sinkF = acc[0];
    }
  });
  const double secFused = bestSeconds(REPS, [&] {
    for (unsigned it = 0; it < ITERS; it++) {
      for (unsigned c = 0; c < 2; c++) produce(c);
      for (unsigned s8 = 0; s8 < span; s8 += 8)
        for (unsigned c = 0; c < 2; c++)
          transposeTile(il[c].data(), pl[c].data(), span, s8);
      for (unsigned l = 0; l < LANES; l++) {
        // Fusion: FIR skalarprodukt + gain + accumulate in einem Loop,
        // direkt aus den Planes (kein temp).
        const int32_t *p0 = pl[0].data() + (size_t)l * span;
        const int32_t *p1 = pl[1].data() + (size_t)l * span;
        const float g = 0.7f;
        unsigned idx = 0, frac = 0;
        const unsigned incr
          = (unsigned)(factors[l] * GR_UP + 0.5);
        for (unsigned j = 0; j < BLOCK_FRAMES; j++) {
          const float *co = resample.m_PolyphaseCoefs[frac];
          float a0 = 0, a1 = 0;
          for (int k = 0; k < 8; k++) {
            a0 += (float)p0[idx + k] * co[k];
            a1 += (float)p1[idx + k] * co[k];
          }
          acc[(size_t)j * 2] += a0 * g;
          acc[(size_t)j * 2 + 1] += a1 * g;
          frac += incr; idx += frac >> 13; frac &= GR_UP - 1;
        }
      }
      g_sinkF = acc[0];
    }
  });

  const double vf = (double)ITERS * BLOCK_FRAMES * LANES;

  printf(
    "512-frame callbacks, 8 stereo voices, incl. decoder-store producer\n");
  printf("  T1 full-span transpose + render        %7.2f ns/voice-frame\n",
    secFull / vf * 1e9);
  printf("  T2 just-in-time tiles (%u out-frames)  %7.2f ns/voice-frame\n",
    TILE_FRAMES, secTiled / vf * 1e9);
  printf("  T2/T1: %.3fx\n", secTiled / secFull);
  printf("\n  Fusion-Frage (FIR->temp->Accumulate vs ein Pass):\n");
  printf("  H1 zwei Paesse (Produktion D)          %7.2f ns/voice-frame\n",
    secTwoPass / vf * 1e9);
  printf("  H2 fusioniert (skalarer Prototyp!)     %7.2f ns/voice-frame\n",
    secFused / vf * 1e9);
  printf("  H2/H1: %.3fx (H2 ist SKALAR - AVX-Fusion waere nochmal besser)\n",
    secFused / secTwoPass);
  return 0;
}
