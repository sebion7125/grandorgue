// fir_batch_vs_planar_bench.cpp
//
// Head-to-head of the two production FIR resample kernels on identical,
// cache-warm data - the suspected V3-vs-legacy gap of 2026-07-11:
//
//   A  classic per-voice planar AVX2 polyphase
//      (GOSoundResample::PolyphaseResampler, contiguous 32B tap loads -
//      what every LEGACY voice runs since the planar ReadAhead redesign)
//   B  fused 8-lane batch kernel (GOSoundResampleBatch::RenderAccumulate,
//      per-tap GATHERS from the lane-interleaved decode scratch - what
//      every fused V3 voice runs)
//   C  like B, plus the decode kernel's planar->interleaved store pass
//      (GOSoundCompressionCacheV3Batch writes planar[ch][lane][s] first,
//      then interleaves into the scratch - B's input layout is not free)
//
// All three produce 8 voices x BLOCK_FRAMES stereo output per call over the
// same synthetic material, small enough to stay L1/L2-resident (mirrors
// production: the decode scratch is warm when the FIR runs). Reported as
// ns per voice-frame.
//
// Build:
//   g++ -O3 -ffast-math -funroll-loops -std=gnu++20 \
//       -I ../../src/grandorgue -I ../../src/grandorgue/sound/playing \
//       fir_batch_vs_planar_bench.cpp ../../src/grandorgue/sound/playing/GOSoundResample.cpp \
//       -o /tmp/claude-1000/firbench
//
// (RenderAccumulate carries its own target("avx2,fma") attribute; the TU
// itself stays baseline-ISA like production.)

#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sound/playing/GOSoundResample.h"
#include "sound/playing/GOSoundResampleBatch.h"

namespace {

constexpr unsigned BLOCK_FRAMES = 128; // one callback at the 128 buffer size
constexpr unsigned LANES = GOSoundResampleBatch::LANES;
constexpr unsigned REPS = 11;
constexpr unsigned SPAN = 4096; // per-lane source span (well past one block)

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

struct PlanarVectorAvx2 : GOSoundResample::FloatingSampleVector<2> {
  static constexpr bool kPlanarInt32Taps = true;
  const int32_t *plane[2];
  const int32_t *p = nullptr;

  inline void Seek(unsigned index, uint8_t ch) { p = plane[ch] + index; }
  inline const int32_t *GetPtr() const { return p; }
  inline float NextSample() { return (float)*p++; }
};

} // namespace

int main() {
  const GOSoundResample resample;

  // Per-lane pitch factors around 1.0 - a realistic random-speaking /
  // temperament spread within one bin rung.
  const double factors[LANES]
    = {0.97, 0.985, 1.0, 1.01, 1.02, 1.035, 1.05, 1.06};

  // Synthetic organ-ish source per lane, planar and interleaved copies.
  std::vector<int32_t> planar[2]; // [ch][lane*SPAN + s]
  planar[0].resize(LANES * SPAN);
  planar[1].resize(LANES * SPAN);
  std::vector<int32_t> interleaved[2]; // [ch][s*LANES + lane]
  interleaved[0].resize(SPAN * LANES);
  interleaved[1].resize(SPAN * LANES);
  for (unsigned l = 0; l < LANES; l++)
    for (unsigned s = 0; s < SPAN; s++)
      for (unsigned c = 0; c < 2; c++) {
        const double phase = (double)s * (0.011 + 0.001 * l) + c * 0.5;
        const int32_t v = (int32_t)(2000000.0
          * (std::sin(phase) + 0.5 * std::sin(3.01 * phase)
             + 0.25 * std::sin(7.02 * phase)));

        planar[c][l * SPAN + s] = v;
        interleaved[c][(size_t)s * LANES + l] = v;
      }

  std::vector<float> outClassic(BLOCK_FRAMES * 2);
  std::vector<float> outBatch(BLOCK_FRAMES * 2);

  // How many source frames one block consumes at most (factor ~1.06).
  const unsigned maxConsume = (unsigned)(BLOCK_FRAMES * 1.1) + 8;
  const unsigned iterations = (SPAN - GOSoundResample::POLYPHASE_POINTS - 8)
    / maxConsume;

  // --- A: classic planar AVX2, 8 voices sequentially ------------------------
  const double secClassic = bestSeconds(REPS, [&] {
    for (unsigned l = 0; l < LANES; l++) {
      GOSoundResample::PolyphaseResampler poly(resample);
      GOSoundResample::ResamplingPosition pos;
      PlanarVectorAvx2 vec;

      vec.plane[0] = planar[0].data() + l * SPAN;
      vec.plane[1] = planar[1].data() + l * SPAN;
      pos.Init((float)factors[l], 0);
      for (unsigned it = 0; it < iterations; it++)
        poly.ResampleBlock<PlanarVectorAvx2, 2>(
          pos, vec, outClassic.data(), BLOCK_FRAMES);
    }
    g_sinkF = outClassic[0];
  });

  // --- B: fused 8-lane gather kernel ----------------------------------------
  const int32_t *inPtrs[MAX_OUTPUT_CHANNELS]
    = {interleaved[0].data(), interleaved[1].data()};
  const double secBatch = bestSeconds(REPS, [&] {
    GOSoundResampleBatch::Lane lanes[LANES];

    for (unsigned l = 0; l < LANES; l++) {
      lanes[l].rel = 0;
      lanes[l].fraction = 0;
      lanes[l].increment
        = (unsigned)(factors[l] * GOSoundResample::UPSAMPLE_FACTOR + 0.5);
      lanes[l].g0 = 0.7f;
      lanes[l].dg = 0.0f;
    }
    for (unsigned it = 0; it < iterations; it++) {
      // Rewind rel each iteration (stay inside SPAN) - the work per call
      // is identical to production: BLOCK_FRAMES output frames, 8 lanes.
      for (unsigned l = 0; l < LANES; l++) {
        lanes[l].rel = 0;
        lanes[l].fraction = (it * 977) & GOSoundResample::UPSAMPLE_MASK;
      }
      std::fill(outBatch.begin(), outBatch.end(), 0.0f);
      GOSoundResampleBatch::RenderAccumulate(
        lanes,
        LANES,
        inPtrs,
        2,
        BLOCK_FRAMES,
        resample.m_PolyphaseCoefs,
        outBatch.data());
    }
    g_sinkF = outBatch[0];
  });

  // --- C: B plus the planar->interleaved store the decode kernel pays -------
  // (models GOSoundCompressionCacheV3Batch's final interleave pass: the
  // decoder produces planar[ch][lane][s] registers/stack first and stores
  // lane-interleaved for the render kernel).
  std::vector<int32_t> scratchIl[2];
  scratchIl[0].resize(maxConsume * LANES + 64);
  scratchIl[1].resize(maxConsume * LANES + 64);
  const double secBatchStore = bestSeconds(REPS, [&] {
    GOSoundResampleBatch::Lane lanes[LANES];

    for (unsigned l = 0; l < LANES; l++) {
      lanes[l].increment
        = (unsigned)(factors[l] * GOSoundResample::UPSAMPLE_FACTOR + 0.5);
      lanes[l].g0 = 0.7f;
      lanes[l].dg = 0.0f;
    }
    for (unsigned it = 0; it < iterations; it++) {
      // The interleave store pass (source: planar, dest: interleaved).
      for (unsigned c = 0; c < 2; c++)
        for (unsigned s = 0; s < maxConsume; s++)
          for (unsigned l = 0; l < LANES; l++)
            scratchIl[c][(size_t)s * LANES + l]
              = planar[c][l * SPAN + ((it * maxConsume + s) % (SPAN - 1))];
      const int32_t *ptrs[MAX_OUTPUT_CHANNELS]
        = {scratchIl[0].data(), scratchIl[1].data()};

      for (unsigned l = 0; l < LANES; l++) {
        lanes[l].rel = 0;
        lanes[l].fraction = (it * 977) & GOSoundResample::UPSAMPLE_MASK;
      }
      std::fill(outBatch.begin(), outBatch.end(), 0.0f);
      GOSoundResampleBatch::RenderAccumulate(
        lanes, LANES, ptrs, 2, BLOCK_FRAMES, resample.m_PolyphaseCoefs,
        outBatch.data());
    }
    g_sinkF = outBatch[0];
  });

  // --- D: interleaved -> planar 8x8 SIMD transpose + classic render ---------
  // The lockstep decoder's natural output IS interleaved (one 32B store per
  // sample across the 8 lanes); planar output needs a transpose SOMEWHERE.
  // Today the render kernel pays it implicitly as per-tap gathers (B). This
  // scenario pays it once, explicitly, on the L1/L2-warm span, then renders
  // every lane with the fast classic contiguous-tap kernel.
  std::vector<int32_t> planeScratch[2];
  planeScratch[0].resize((maxConsume + 16) * LANES);
  planeScratch[1].resize((maxConsume + 16) * LANES);
  const double secTranspose = bestSeconds(REPS, [&] {
    for (unsigned it = 0; it < iterations; it++) {
      const unsigned base = (it * maxConsume) % (SPAN - maxConsume - 16);

      // 8x8 int32 transpose tiles: 8 interleaved rows -> 8 planar rows.
      for (unsigned c = 0; c < 2; c++) {
        const int32_t *src = interleaved[c].data() + (size_t)base * LANES;
        const unsigned planeStride = maxConsume + 16;

        for (unsigned s8 = 0; s8 + 8 <= maxConsume + 8; s8 += 8) {
          __m256i r0 = _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 0) * LANES));
          __m256i r1 = _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 1) * LANES));
          __m256i r2 = _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 2) * LANES));
          __m256i r3 = _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 3) * LANES));
          __m256i r4 = _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 4) * LANES));
          __m256i r5 = _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 5) * LANES));
          __m256i r6 = _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 6) * LANES));
          __m256i r7 = _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 7) * LANES));

          __m256i t0 = _mm256_unpacklo_epi32(r0, r1);
          __m256i t1 = _mm256_unpackhi_epi32(r0, r1);
          __m256i t2 = _mm256_unpacklo_epi32(r2, r3);
          __m256i t3 = _mm256_unpackhi_epi32(r2, r3);
          __m256i t4 = _mm256_unpacklo_epi32(r4, r5);
          __m256i t5 = _mm256_unpackhi_epi32(r4, r5);
          __m256i t6 = _mm256_unpacklo_epi32(r6, r7);
          __m256i t7 = _mm256_unpackhi_epi32(r6, r7);

          __m256i u0 = _mm256_unpacklo_epi64(t0, t2);
          __m256i u1 = _mm256_unpackhi_epi64(t0, t2);
          __m256i u2 = _mm256_unpacklo_epi64(t1, t3);
          __m256i u3 = _mm256_unpackhi_epi64(t1, t3);
          __m256i u4 = _mm256_unpacklo_epi64(t4, t6);
          __m256i u5 = _mm256_unpackhi_epi64(t4, t6);
          __m256i u6 = _mm256_unpacklo_epi64(t5, t7);
          __m256i u7 = _mm256_unpackhi_epi64(t5, t7);

          int32_t *dst = planeScratch[c].data();
          _mm256_storeu_si256((__m256i *)(dst + 0 * planeStride + s8),
            _mm256_permute2x128_si256(u0, u4, 0x20));
          _mm256_storeu_si256((__m256i *)(dst + 1 * planeStride + s8),
            _mm256_permute2x128_si256(u1, u5, 0x20));
          _mm256_storeu_si256((__m256i *)(dst + 2 * planeStride + s8),
            _mm256_permute2x128_si256(u2, u6, 0x20));
          _mm256_storeu_si256((__m256i *)(dst + 3 * planeStride + s8),
            _mm256_permute2x128_si256(u3, u7, 0x20));
          _mm256_storeu_si256((__m256i *)(dst + 4 * planeStride + s8),
            _mm256_permute2x128_si256(u0, u4, 0x31));
          _mm256_storeu_si256((__m256i *)(dst + 5 * planeStride + s8),
            _mm256_permute2x128_si256(u1, u5, 0x31));
          _mm256_storeu_si256((__m256i *)(dst + 6 * planeStride + s8),
            _mm256_permute2x128_si256(u2, u6, 0x31));
          _mm256_storeu_si256((__m256i *)(dst + 7 * planeStride + s8),
            _mm256_permute2x128_si256(u3, u7, 0x31));
        }
      }
      // Classic per-lane render from the transposed planes.
      for (unsigned l = 0; l < LANES; l++) {
        GOSoundResample::PolyphaseResampler poly(resample);
        GOSoundResample::ResamplingPosition pos;
        PlanarVectorAvx2 vec;
        const unsigned planeStride = maxConsume + 16;

        vec.plane[0] = planeScratch[0].data() + (size_t)l * planeStride;
        vec.plane[1] = planeScratch[1].data() + (size_t)l * planeStride;
        pos.Init((float)factors[l], 0);
        poly.ResampleBlock<PlanarVectorAvx2, 2>(
          pos, vec, outClassic.data(), BLOCK_FRAMES);
      }
      g_sinkF = outClassic[0];
    }
  });

  // --- E: streaming delay-line kernel (user design 2026-07-11) --------------
  // No gathers anywhere: all lanes start block-aligned at rel=0, so their
  // 8-tap windows begin identical and drift apart only by the fraction
  // spread (a few samples over a whole block). Per output frame the kernel
  // loads at most maxAdvance interleaved ROWS (contiguous 32B - one source
  // sample across all 8 lanes) and shifts them into per-tap lane vectors
  // via blends, only for lanes whose integer position actually advanced.
  // Taps live tap-major in registers -> direct FMA with the coefficient
  // vectors (built from 8 contiguous per-lane row loads + an 8x8 float
  // transpose instead of 8 gathers). Structurally the legacy inner loop,
  // 8 lanes wide.
  std::vector<float> outStream(BLOCK_FRAMES * 2);
  const double secStream = bestSeconds(REPS, [&] {
    for (unsigned it = 0; it < iterations; it++) {
      alignas(32) unsigned fracInit[LANES];
      alignas(32) unsigned incrA[LANES];

      for (unsigned l = 0; l < LANES; l++) {
        fracInit[l] = (it * 977) & GOSoundResample::UPSAMPLE_MASK;
        incrA[l]
          = (unsigned)(factors[l] * GOSoundResample::UPSAMPLE_FACTOR + 0.5);
      }
      std::fill(outStream.begin(), outStream.end(), 0.0f);

      alignas(32) float coefStack[BLOCK_FRAMES ? 1 : 1]; // placeholder
      (void)coefStack;

      for (unsigned c = 0; c < 2; c++) {
        const int32_t *src = interleaved[c].data();
        __m256i frac = _mm256_load_si256((const __m256i *)fracInit);
        const __m256i incr = _mm256_load_si256((const __m256i *)incrA);
        const __m256i maskFrac
          = _mm256_set1_epi32(GOSoundResample::UPSAMPLE_MASK);
        __m256 gains = _mm256_set1_ps(0.7f);
        const __m256 dg = _mm256_setzero_ps();

        // Prefill: all lanes aligned at rel=0 -> taps = rows 0..7.
        __m256 tap[8];
        for (unsigned k = 0; k < 8; k++)
          tap[k] = _mm256_cvtepi32_ps(_mm256_loadu_si256(
            (const __m256i *)(src + (size_t)k * LANES)));
        unsigned rowPos = 8; // next interleaved row to stream in

        const float *coefsFlat = &resample.m_PolyphaseCoefs[0][0];

        for (unsigned j = 0; j < BLOCK_FRAMES; j++) {
          /* Coefficient vectors, tap-major: 8 contiguous per-lane row
           * loads + 8x8 float transpose (no gathers). */
          alignas(32) unsigned fracA2[LANES];
          _mm256_store_si256((__m256i *)fracA2, frac);
          __m256 cr[8];
          for (unsigned l = 0; l < 8; l++)
            cr[l] = _mm256_loadu_ps(coefsFlat + (size_t)fracA2[l] * 8);
          __m256 q0 = _mm256_unpacklo_ps(cr[0], cr[1]);
          __m256 q1 = _mm256_unpackhi_ps(cr[0], cr[1]);
          __m256 q2 = _mm256_unpacklo_ps(cr[2], cr[3]);
          __m256 q3 = _mm256_unpackhi_ps(cr[2], cr[3]);
          __m256 q4 = _mm256_unpacklo_ps(cr[4], cr[5]);
          __m256 q5 = _mm256_unpackhi_ps(cr[4], cr[5]);
          __m256 q6 = _mm256_unpacklo_ps(cr[6], cr[7]);
          __m256 q7 = _mm256_unpackhi_ps(cr[6], cr[7]);
          __m256 h0 = _mm256_shuffle_ps(q0, q2, 0x44);
          __m256 h1 = _mm256_shuffle_ps(q0, q2, 0xEE);
          __m256 h2 = _mm256_shuffle_ps(q1, q3, 0x44);
          __m256 h3 = _mm256_shuffle_ps(q1, q3, 0xEE);
          __m256 h4 = _mm256_shuffle_ps(q4, q6, 0x44);
          __m256 h5 = _mm256_shuffle_ps(q4, q6, 0xEE);
          __m256 h6 = _mm256_shuffle_ps(q5, q7, 0x44);
          __m256 h7 = _mm256_shuffle_ps(q5, q7, 0xEE);
          __m256 coefV[8];
          coefV[0] = _mm256_permute2f128_ps(h0, h4, 0x20);
          coefV[1] = _mm256_permute2f128_ps(h1, h5, 0x20);
          coefV[2] = _mm256_permute2f128_ps(h2, h6, 0x20);
          coefV[3] = _mm256_permute2f128_ps(h3, h7, 0x20);
          coefV[4] = _mm256_permute2f128_ps(h0, h4, 0x31);
          coefV[5] = _mm256_permute2f128_ps(h1, h5, 0x31);
          coefV[6] = _mm256_permute2f128_ps(h2, h6, 0x31);
          coefV[7] = _mm256_permute2f128_ps(h3, h7, 0x31);

          // FIR: taps are already tap-major.
          __m256 acc = _mm256_setzero_ps();
          for (unsigned t = 0; t < 8; t++)
            acc = _mm256_fmadd_ps(tap[t], coefV[t], acc);

          const float v = [&] {
            __m128 sum4 = _mm_add_ps(
              _mm256_castps256_ps128(_mm256_mul_ps(acc, gains)),
              _mm256_extractf128_ps(_mm256_mul_ps(acc, gains), 1));
            sum4 = _mm_hadd_ps(sum4, sum4);
            sum4 = _mm_hadd_ps(sum4, sum4);
            return _mm_cvtss_f32(sum4);
          }();
          outStream[(size_t)j * 2 + c] += v;

          /* Advance: per-lane integer step a_l = (frac+incr)>>13, then
           * stream rows in step by step, shifting only lanes with
           * a_l >= s (blend masks from integer compares). */
          const __m256i fracNext = _mm256_add_epi32(frac, incr);
          const __m256i adv
            = _mm256_srli_epi32(fracNext, GOSoundResample::UPSAMPLE_BITS);
          frac = _mm256_and_si256(fracNext, maskFrac);
          gains = _mm256_add_ps(gains, dg);

          alignas(32) unsigned advA[LANES];
          _mm256_store_si256((__m256i *)advA, adv);
          unsigned maxAdv = 0;
          for (unsigned l = 0; l < LANES; l++)
            maxAdv = std::max(maxAdv, advA[l]);

          for (unsigned st = 1; st <= maxAdv; st++) {
            const __m256 row = _mm256_cvtepi32_ps(_mm256_loadu_si256(
              (const __m256i *)(src + (size_t)rowPos * LANES)));
            rowPos++;
            const __m256 doShift = _mm256_castsi256_ps(_mm256_cmpgt_epi32(
              adv, _mm256_set1_epi32((int)st - 1)));

            for (unsigned k = 0; k < 7; k++)
              tap[k] = _mm256_blendv_ps(tap[k], tap[k + 1], doShift);
            tap[7] = _mm256_blendv_ps(tap[7], row, doShift);
          }
        }
      }
      g_sinkF = outStream[0];
    }
  });

  // --- F: WO soll die Transponierung leben? ---------------------------------
  // Modelliert die STORE-Stufe des Decoders (die Prädiktor-Arithmetik ist in
  // beiden Varianten identisch - der Delta isoliert die Store-Frage):
  //   F1: Lockstep-Produzent -> interleaved Stores -> separater
  //       8x8-Transpose-Pass -> Per-Lane-Klassik   (= D inkl. Produzent)
  //   F2: Lockstep-Produzent -> 8 Samples in Registern sammeln ->
  //       in-Register-Transpose -> planare Stores -> Per-Lane-Klassik
  //       (Transponierung in den Decoder-Store fusioniert)
  const unsigned spanF = ((maxConsume + 8 + 7) / 8) * 8;
  std::vector<int32_t> resid[2]; // "Residuen"-Strom, interleaved rows
  resid[0].resize((size_t)spanF * LANES);
  resid[1].resize((size_t)spanF * LANES);
  for (unsigned c = 0; c < 2; c++)
    for (size_t i = 0; i < resid[c].size(); i++)
      resid[c][i] = (int32_t)((i * 2654435761u) >> 24) - 128;
  std::vector<int32_t> ilF[2];
  ilF[0].resize((size_t)spanF * LANES);
  ilF[1].resize((size_t)spanF * LANES);
  std::vector<int32_t> plF[2];
  plF[0].resize((size_t)spanF * LANES);
  plF[1].resize((size_t)spanF * LANES);

  auto transpose8x8Store = [](
    __m256i r0, __m256i r1, __m256i r2, __m256i r3,
    __m256i r4, __m256i r5, __m256i r6, __m256i r7,
    int32_t *dst, unsigned stride, unsigned s8) {
    __m256i t0 = _mm256_unpacklo_epi32(r0, r1);
    __m256i t1 = _mm256_unpackhi_epi32(r0, r1);
    __m256i t2 = _mm256_unpacklo_epi32(r2, r3);
    __m256i t3 = _mm256_unpackhi_epi32(r2, r3);
    __m256i t4 = _mm256_unpacklo_epi32(r4, r5);
    __m256i t5 = _mm256_unpackhi_epi32(r4, r5);
    __m256i t6 = _mm256_unpacklo_epi32(r6, r7);
    __m256i t7 = _mm256_unpackhi_epi32(r6, r7);
    __m256i u0 = _mm256_unpacklo_epi64(t0, t2);
    __m256i u1 = _mm256_unpackhi_epi64(t0, t2);
    __m256i u2 = _mm256_unpacklo_epi64(t1, t3);
    __m256i u3 = _mm256_unpackhi_epi64(t1, t3);
    __m256i u4 = _mm256_unpacklo_epi64(t4, t6);
    __m256i u5 = _mm256_unpackhi_epi64(t4, t6);
    __m256i u6 = _mm256_unpacklo_epi64(t5, t7);
    __m256i u7 = _mm256_unpackhi_epi64(t5, t7);
    _mm256_storeu_si256((__m256i *)(dst + 0 * stride + s8),
      _mm256_permute2x128_si256(u0, u4, 0x20));
    _mm256_storeu_si256((__m256i *)(dst + 1 * stride + s8),
      _mm256_permute2x128_si256(u1, u5, 0x20));
    _mm256_storeu_si256((__m256i *)(dst + 2 * stride + s8),
      _mm256_permute2x128_si256(u2, u6, 0x20));
    _mm256_storeu_si256((__m256i *)(dst + 3 * stride + s8),
      _mm256_permute2x128_si256(u3, u7, 0x20));
    _mm256_storeu_si256((__m256i *)(dst + 4 * stride + s8),
      _mm256_permute2x128_si256(u0, u4, 0x31));
    _mm256_storeu_si256((__m256i *)(dst + 5 * stride + s8),
      _mm256_permute2x128_si256(u1, u5, 0x31));
    _mm256_storeu_si256((__m256i *)(dst + 6 * stride + s8),
      _mm256_permute2x128_si256(u2, u6, 0x31));
    _mm256_storeu_si256((__m256i *)(dst + 7 * stride + s8),
      _mm256_permute2x128_si256(u3, u7, 0x31));
  };

  auto renderFromPlanes = [&](const std::vector<int32_t> *planes) {
    for (unsigned l = 0; l < LANES; l++) {
      GOSoundResample::PolyphaseResampler poly(resample);
      GOSoundResample::ResamplingPosition pos;
      PlanarVectorAvx2 vec;

      vec.plane[0] = planes[0].data() + (size_t)l * spanF;
      vec.plane[1] = planes[1].data() + (size_t)l * spanF;
      pos.Init((float)factors[l], 0);
      poly.ResampleBlock<PlanarVectorAvx2, 2>(
        pos, vec, outClassic.data(), BLOCK_FRAMES);
    }
  };

  // Lockstep-"Decoder": Prädiktor v = p + ((p-la)>>1) + res, vektorisiert
  // über die Lanes wie der echte Kernel; identisch in F1 und F2.
  const double secF1 = bestSeconds(REPS, [&] {
    for (unsigned it = 0; it < iterations; it++) {
      for (unsigned c = 0; c < 2; c++) {
        __m256i v = _mm256_set1_epi32(1000), pr = _mm256_set1_epi32(990),
                la = _mm256_set1_epi32(985);
        const int32_t *rs = resid[c].data();
        int32_t *dst = ilF[c].data();

        for (unsigned smp = 0; smp < spanF; smp++) {
          la = pr;
          pr = v;
          const __m256i pred = _mm256_add_epi32(
            pr, _mm256_srai_epi32(_mm256_sub_epi32(pr, la), 1));
          v = _mm256_add_epi32(pred,
            _mm256_loadu_si256((const __m256i *)(rs + (size_t)smp * LANES)));
          _mm256_storeu_si256((__m256i *)(dst + (size_t)smp * LANES), v);
        }
      }
      // separater Transpose-Pass (interleaved -> planar)
      for (unsigned c = 0; c < 2; c++) {
        const int32_t *src = ilF[c].data();
        int32_t *dst = plF[c].data();

        for (unsigned s8 = 0; s8 < spanF; s8 += 8)
          transpose8x8Store(
            _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 0) * LANES)),
            _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 1) * LANES)),
            _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 2) * LANES)),
            _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 3) * LANES)),
            _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 4) * LANES)),
            _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 5) * LANES)),
            _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 6) * LANES)),
            _mm256_loadu_si256((const __m256i *)(src + (size_t)(s8 + 7) * LANES)),
            dst, spanF, s8);
      }
      renderFromPlanes(plF);
      g_sinkF = outClassic[0];
    }
  });

  const double secF2 = bestSeconds(REPS, [&] {
    for (unsigned it = 0; it < iterations; it++) {
      for (unsigned c = 0; c < 2; c++) {
        __m256i v = _mm256_set1_epi32(1000), pr = _mm256_set1_epi32(990),
                la = _mm256_set1_epi32(985);
        const int32_t *rs = resid[c].data();
        int32_t *dst = plF[c].data();
        __m256i buf[8];

        for (unsigned smp = 0; smp < spanF; smp++) {
          la = pr;
          pr = v;
          const __m256i pred = _mm256_add_epi32(
            pr, _mm256_srai_epi32(_mm256_sub_epi32(pr, la), 1));
          v = _mm256_add_epi32(pred,
            _mm256_loadu_si256((const __m256i *)(rs + (size_t)smp * LANES)));
          buf[smp & 7] = v;
          if ((smp & 7) == 7)
            transpose8x8Store(
              buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
              dst, spanF, smp - 7);
        }
      }
      renderFromPlanes(plF);
      g_sinkF = outClassic[0];
    }
  });

  // --- K: Vektor-Kachel-Fusion vs Produktions-Zwei-Pass ----------------------
  // K1 = Produktion heute: ResampleBlock (per-Frame-hsum) -> temp ->
  //      separater 8-wide Accumulate-Pass.
  // K2 = Kachel: 4 Output-Frames im Flug, EINE gemeinsame Faltung
  //      (3x hadd + lo+hi = 4 Summen), Ergebnisse liegen gepackt ->
  //      direkte 8-wide Akkumulation von 4 Stereo-Frames, kein temp.
  std::vector<float> outAcc(BLOCK_FRAMES * 2, 0.0f);
  const double secK1 = bestSeconds(REPS, [&] {
    for (unsigned l = 0; l < LANES; l++) {
      GOSoundResample::PolyphaseResampler poly(resample);
      GOSoundResample::ResamplingPosition pos;
      PlanarVectorAvx2 vec;
      float temp[BLOCK_FRAMES * 2];

      vec.plane[0] = planar[0].data() + l * SPAN;
      vec.plane[1] = planar[1].data() + l * SPAN;
      pos.Init((float)factors[l], 0);
      for (unsigned it = 0; it < iterations; it++) {
        poly.ResampleBlock<PlanarVectorAvx2, 2>(
          pos, vec, temp, BLOCK_FRAMES);
        // 8-wide Accumulate (Produktions-accumConstAvx2-Form)
        const __m256 gv = _mm256_set1_ps(0.7f);
        for (unsigned i = 0; i + 8 <= BLOCK_FRAMES * 2; i += 8) {
          const __m256 sv = _mm256_loadu_ps(temp + i);
          const __m256 dv = _mm256_loadu_ps(outAcc.data() + i);
          _mm256_storeu_ps(
            outAcc.data() + i, _mm256_add_ps(dv, _mm256_mul_ps(sv, gv)));
        }
      }
    }
    g_sinkF = outAcc[0];
  });

  const double secK2 = bestSeconds(REPS, [&] {
    for (unsigned l = 0; l < LANES; l++) {
      const int32_t *pl0 = planar[0].data() + l * SPAN;
      const int32_t *pl1 = planar[1].data() + l * SPAN;
      const unsigned incr
        = (unsigned)(factors[l] * GOSoundResample::UPSAMPLE_FACTOR + 0.5);
      const __m256 gv = _mm256_set1_ps(0.7f);

      for (unsigned it = 0; it < iterations; it++) {
        unsigned idx = 0, frac = (it * 977) & GOSoundResample::UPSAMPLE_MASK;

        for (unsigned j = 0; j < BLOCK_FRAMES; j += 4) {
          __m256 a0[4], a1[4];

          for (unsigned k = 0; k < 4; k++) {
            // Mini-Tabellen-Lerp wie der Produktions-AVX2-Pfad
            const unsigned si = frac >> GOSoundResample::SMALL_SHIFT;
            const __m256 wv = _mm256_set1_ps(
              (float)(frac - (si << GOSoundResample::SMALL_SHIFT))
              / (float)(1u << GOSoundResample::SMALL_SHIFT));
            const __m256 ca = _mm256_loadu_ps(resample.m_PolyphaseSmall[si]);
            const __m256 cb
              = _mm256_loadu_ps(resample.m_PolyphaseSmall[si + 1]);
            const __m256 coefs = _mm256_add_ps(
              ca, _mm256_mul_ps(_mm256_sub_ps(cb, ca), wv));

            a0[k] = _mm256_mul_ps(
              _mm256_cvtepi32_ps(
                _mm256_loadu_si256((const __m256i *)(pl0 + idx))),
              coefs);
            a1[k] = _mm256_mul_ps(
              _mm256_cvtepi32_ps(
                _mm256_loadu_si256((const __m256i *)(pl1 + idx))),
              coefs);
            frac += incr;
            idx += frac >> GOSoundResample::UPSAMPLE_BITS;
            frac &= GOSoundResample::UPSAMPLE_FACTOR - 1;
          }
          // Gemeinsame Faltung: 4 Summen pro Kanal in einem xmm
          auto fold4 = [](__m256 A, __m256 B, __m256 C, __m256 D) -> __m128 {
            const __m256 s1 = _mm256_hadd_ps(A, B);
            const __m256 s2 = _mm256_hadd_ps(C, D);
            const __m256 s3 = _mm256_hadd_ps(s1, s2);
            return _mm_add_ps(
              _mm256_castps256_ps128(s3), _mm256_extractf128_ps(s3, 1));
          };
          const __m128 c0 = fold4(a0[0], a0[1], a0[2], a0[3]);
          const __m128 c1 = fold4(a1[0], a1[1], a1[2], a1[3]);
          // Verschraenken zu [f0c0,f0c1,f1c0,f1c1 | f2c0,f2c1,f3c0,f3c1]
          const __m128 lo = _mm_unpacklo_ps(c0, c1);
          const __m128 hi = _mm_unpackhi_ps(c0, c1);
          const __m256 tile
            = _mm256_insertf128_ps(_mm256_castps128_ps256(lo), hi, 1);
          // Kachel-Akkumulation: 4 Stereo-Frames, 8-wide, kein temp
          float *dst = outAcc.data() + (size_t)j * 2;
          _mm256_storeu_ps(
            dst,
            _mm256_add_ps(
              _mm256_loadu_ps(dst), _mm256_mul_ps(tile, gv)));
        }
      }
    }
    g_sinkF = outAcc[0];
  });

  const double voiceFrames = (double)iterations * BLOCK_FRAMES * LANES;

  printf("FIR kernels, %u-frame blocks, 8 stereo voices, warm data\n",
    BLOCK_FRAMES);
  printf("  A classic planar AVX2 (contiguous taps)  %7.2f ns/voice-frame\n",
    secClassic / voiceFrames * 1e9);
  printf("  B fused 8-lane batch (gather taps)       %7.2f ns/voice-frame\n",
    secBatch / voiceFrames * 1e9);
  printf("  C = B + planar->interleaved store pass   %7.2f ns/voice-frame\n",
    secBatchStore / voiceFrames * 1e9);
  printf("  D transpose 8x8 + classic per-lane       %7.2f ns/voice-frame\n",
    secTranspose / voiceFrames * 1e9);
  printf("  E streaming delay-line (row loads+blend) %7.2f ns/voice-frame\n",
    secStream / voiceFrames * 1e9);
  printf("\n  Wo lebt die Transponierung? (inkl. Decoder-Store-Modell + Render)\n");
  printf("  F1 interleaved Store + separater Pass    %7.2f ns/voice-frame\n",
    secF1 / voiceFrames * 1e9);
  printf("  F2 Transpose IM Decoder-Store (fusion)   %7.2f ns/voice-frame\n",
    secF2 / voiceFrames * 1e9);
  printf("  F2/F1: %.2fx (Ersparnis %.2f ns/vf)\n",
    secF2 / secF1, (secF1 - secF2) / voiceFrames * 1e9);
  printf("\n  Vektor-Kachel-Frage (4-Frame-Faltung + Kachel-Accumulate):\n");
  printf("  K1 Zwei-Pass (Produktion, temp+8wide)    %7.2f ns/voice-frame\n",
    secK1 / voiceFrames * 1e9);
  printf("  K2 Kachel-fusioniert                     %7.2f ns/voice-frame\n",
    secK2 / voiceFrames * 1e9);
  printf("  K2/K1: %.3fx\n", secK2 / secK1);
  printf("  E/A: %.2fx   E/B: %.2fx\n",
    secStream / secClassic, secStream / secBatch);
  printf(
    "  B/A: %.2fx   C/A: %.2fx   D/A: %.2fx   D/C: %.2fx\n",
    secBatch / secClassic,
    secBatchStore / secClassic,
    secTranspose / secClassic,
    secTranspose / secBatchStore);
  return 0;
}
