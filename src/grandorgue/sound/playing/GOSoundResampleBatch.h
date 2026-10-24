/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDRESAMPLEBATCH_H
#define GOSOUNDRESAMPLEBATCH_H

#include <cstdint>

/* Same guard as GO_V3_BATCH_AVX2 (GOSoundCompressionCacheV3Batch.h):
 * x86 gcc/clang only. The kernel functions carry target("avx2") so the
 * rest of the binary keeps the project's baseline ISA; callers must check
 * CpuSupported() at runtime before calling into the kernel. */
#if (defined(__GNUC__) || defined(__clang__))                                  \
  && (defined(__x86_64__) || defined(__i386__))
#define GO_RESAMPLE_BATCH_AVX2 1
#include <immintrin.h>
#endif

#include "sound/GOSoundDefs.h"

#include "GOSoundResample.h"

#ifdef GO_RESAMPLE_BATCH_AVX2

/* Pin the floating-point semantics of this kernel: gcc's default
 * -ffp-contract=fast fuses even INTRINSIC mul+add pairs into FMA (it
 * fused the gain multiply into the hsum tree's first add during test
 * bring-up), which would make the results compiler-version-dependent.
 * With contraction off, the only FMAs are the explicit _mm256_fmadd_ps
 * calls - exactly the operations the bit-exactness contract (and the
 * test reference) specifies. Same technique as the NDP float kernel. */
#ifndef __clang__
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#endif

// AVX2 8-voice batched polyphase resample + fade + accumulate: the render
// half of the fused per-group pipeline (decode -> resample -> fade -> mix,
// all while the group's decoded data is still cache-warm - see
// GOSoundStream::BuildAndRenderBatch and the Phase 2 decision recorded in
// tools/compression/V3_SUBBLOCK_IMPLEMENTATION_PLAN.md Step 12: the
// per-voice decoded-block intermediate stage made the engine memory-bound,
// PCM-measured, so the renderer now consumes the decode kernel's
// interleaved output DIRECTLY).
//
// lane = voice, exactly like the decode kernel
// (GOSoundCompressionCacheV3Batch): 8 independent voices' resampling
// positions advance together, the channel loop is outer/sequential. The
// input is the decode kernel's interleaved scratch layout
// in[channel][srcSample * LANES + lane]. All blocks are callback-aligned
// (GOSoundStream 2026-07-09), so every lane's block-relative read position
// starts at 0 and drifts by its own fraction increment from there - which
// is why the taps are fetched with per-lane gathers rather than one
// contiguous load: after the first output sample the 8 lanes' source
// indices are no longer equal.
//
// Bit-exactness contract (this is what the tests pin):
// - Per lane, the FIR accumulates as fmadd(tap, coef, acc) in tap order,
//   with an EXPLICIT _mm256_fmadd_ps: gcc contracts even intrinsic
//   mul+add pairs to FMA under -O3 -ffp-contract=fast (caught red-handed
//   by the first run of TestBatchRenderBitExact), so relying on separate
//   mul/add would be compiler-version-dependent. An explicit FMA is
//   deterministic, and the test reference reproduces it exactly with
//   std::fmaf in the same tap order. (This makes the kernel differ from
//   the scalar resampler in the last bits - like the existing
//   single-voice AVX2 path already does via its different summation
//   order.)
// - Per lane, the frame gain advances additively (gains += dg after each
//   frame, applied after both channels) - bit-identical to
//   GOSoundFader::ProcessAndAccumulate's ramp loop.
// - The 8 lanes' contributions are summed into the output frame with the
//   fixed hsum tree defined in HorizontalSum8 below. This ordering
//   differs from the classic path's sequential per-voice accumulation,
//   so multi-voice sums may differ in the last bits - acceptable: the
//   task mix already sums worker-local buffers in nondeterministic
//   thread-completion order across voices.
class GOSoundResampleBatch {
public:
  static constexpr unsigned LANES = 8;
  static constexpr unsigned TAPS = GOSoundResample::POLYPHASE_POINTS;

  /* One lane's live state for a batch call. Gains follow the two-stage
   * design (user requirement: keep the door open for batching non-linear
   * crossfades later): variant (a), used here, generates each frame's
   * gain on the fly from (g0, dg) - covering GOSoundFader's Constant and
   * Ramp kernels, 100% of voice calls in production measurements. A
   * variant (b) taking a precomputed interleaved gain buffer
   * gains[frame * LANES + lane] (e.g. filled from the non-linear
   * LUT/template path) can reuse the whole kernel body below unchanged
   * except for the gain source - see RenderAccumulate's gainBuf
   * parameter, already wired through.
   *
   * A voice that must NOT contribute to the mixed output (it needs the
   * classic single-voice fader/filter path, e.g. active ToneBalance)
   * simply passes g0 = dg = 0 and picks its raw resampled samples out of
   * laneOut instead. */
  struct Lane {
    // Block-relative integer source position (0 at block start).
    unsigned rel;
    // Fraction in 1/UPSAMPLE_FACTOR units and its per-frame increment.
    unsigned fraction;
    unsigned increment;
    // Frame gain at j=0 (target * external * windchest volume) and its
    // additive per-frame delta (0 for a constant-gain voice).
    float g0;
    float dg;
  };

  /* Runtime gate for the target("avx2,fma") kernel below. AVX2 and FMA
   * are formally separate CPUID flags (in practice every AVX2 CPU has
   * FMA, but check both - the kernel uses an explicit fmadd). */
  static inline bool CpuSupported() {
    static const bool has
      = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    return has;
  }

private:
  /* Fixed summation tree for the 8 lanes' contributions to one output
   * frame: (low128 + high128), then two hadd steps. Tests reproduce this
   * exact tree; do not "simplify" it into a different ordering. */
#ifndef __clang__
#define GO_RESAMPLE_BATCH_FN                                                   \
  __attribute__((target("avx2,fma"), optimize("fp-contract=off")))
#else
#define GO_RESAMPLE_BATCH_FN __attribute__((target("avx2,fma")))
#endif

  GO_RESAMPLE_BATCH_FN static inline float HorizontalSum8(__m256 v) {
    __m128 sum4
      = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));

    sum4 = _mm_hadd_ps(sum4, sum4);
    sum4 = _mm_hadd_ps(sum4, sum4);
    return _mm_cvtss_f32(sum4);
  }

public:
  /**
   * Resamples nFrames output frames for up to LANES voices from the
   * interleaved decode scratch and accumulates the faded result into the
   * caller's interleaved-stereo output buffer.
   *
   * @param lanes        per-voice state; rel/fraction are advanced in
   *                     place (same integer arithmetic as
   *                     ResamplingPosition::Inc(), so the caller may
   *                     cross-check against AdvanceBy())
   * @param numLanes     1..LANES; unused lanes are padded internally with
   *                     copies of lane 0's position and zero gain, so
   *                     their gathers read valid memory and contribute
   *                     nothing
   * @param in           in[c] = decode scratch of channel c, layout
   *                     srcSample * LANES + lane; every lane must have
   *                     valid data up to its last read position + TAPS
   *                     (the callback-aligned block guarantees
   *                     validSamples + GUARD_SAMPLES of it)
   * @param nChannels    source channels (1 = mono - rendered once and
   *                     duplicated to both output channels, exactly like
   *                     the scalar path's "copy from previous channel";
   *                     2 = stereo)
   * @param nFrames      output frames (the engine's samples_per_buffer)
   * @param coefs        GOSoundResample::m_PolyphaseCoefs
   *                     ([UPSAMPLE_FACTOR][TAPS] floats)
   * @param output       interleaved stereo, ACCUMULATED:
   *                     output[j*2 + c] += sum over lanes
   * @param laneOut      optional (may be null): laneOut[c] receives every
   *                     lane's RAW resampled samples (pre-gain) in the
   *                     same interleaved layout frame * LANES + lane -
   *                     for voices that need the classic single-voice
   *                     fader/filter path
   * @param gainBuf      optional (may be null): variant (b) gain source -
   *                     per-frame gains gainBuf[j * LANES + lane]
   *                     OVERRIDING the (g0, dg) ramp for all lanes.
   *                     Currently unused in production (non-linear
   *                     crossfade batching is a planned extension); wired
   *                     through now so adding it later cannot require a
   *                     kernel rework.
   */
  GO_RESAMPLE_BATCH_FN static void RenderAccumulate(
    Lane lanes[],
    unsigned numLanes,
    const int32_t *const in[MAX_OUTPUT_CHANNELS],
    unsigned nChannels,
    unsigned nFrames,
    const float (*coefs)[TAPS],
    float *output,
    float *const laneOut[MAX_OUTPUT_CHANNELS] = nullptr,
    const float *gainBuf = nullptr) {
    alignas(32) unsigned relA[LANES];
    alignas(32) unsigned fracA[LANES];
    alignas(32) unsigned incrA[LANES];
    alignas(32) float g0A[LANES];
    alignas(32) float dgA[LANES];

    for (unsigned l = 0; l < LANES; l++) {
      // Pad unused lanes with lane 0's position (valid gathers) and zero
      // gain (no contribution). Unlike the decode kernel there is no
      // mutable per-lane object here, so plain copies are safe.
      const Lane &src = lanes[l < numLanes ? l : 0];

      relA[l] = src.rel;
      fracA[l] = src.fraction;
      incrA[l] = src.increment;
      g0A[l] = l < numLanes ? src.g0 : 0.0f;
      dgA[l] = l < numLanes ? src.dg : 0.0f;
    }

    const __m256i laneIds = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const __m256i maskV = _mm256_set1_epi32(GOSoundResample::UPSAMPLE_MASK);
    __m256i rel = _mm256_load_si256((const __m256i *)relA);
    __m256i frac = _mm256_load_si256((const __m256i *)fracA);
    const __m256i incr = _mm256_load_si256((const __m256i *)incrA);
    __m256 gains = _mm256_load_ps(g0A);
    const __m256 dg = _mm256_load_ps(dgA);
    const float *coefsFlat = &coefs[0][0];

    for (unsigned j = 0; j < nFrames; j++) {
      /* Coefficient rows for the 8 lanes' current fractions, one gather
       * per tap (row stride = TAPS floats), shared by both channels. */
      const __m256i coefRow
        = _mm256_slli_epi32(frac, 3); // frac * TAPS (TAPS == 8)
      __m256 coefV[TAPS];

      for (unsigned t = 0; t < TAPS; t++)
        coefV[t] = _mm256_i32gather_ps(
          coefsFlat,
          _mm256_add_epi32(coefRow, _mm256_set1_epi32((int)t)),
          4);

      /* Tap base index in the interleaved scratch: (rel + t)*LANES + lane
       * = rel*8 + t*8 + lane. rel*8 + lane is loop-invariant per frame. */
      const __m256i tapBase
        = _mm256_add_epi32(_mm256_slli_epi32(rel, 3), laneIds);
      const __m256 frameGains
        = gainBuf ? _mm256_loadu_ps(gainBuf + (size_t)j * LANES) : gains;
      float frameOut[MAX_OUTPUT_CHANNELS];

      for (unsigned c = 0; c < nChannels; c++) {
        __m256 acc = _mm256_setzero_ps();

        for (unsigned t = 0; t < TAPS; t++) {
          const __m256i idx
            = _mm256_add_epi32(tapBase, _mm256_set1_epi32((int)(t * LANES)));
          const __m256 taps = _mm256_cvtepi32_ps(
            _mm256_i32gather_epi32(in[c], idx, 4));

          // Explicit FMA in tap order - see the bit-exactness contract
          // in the class comment (the test reference uses std::fmaf).
          acc = _mm256_fmadd_ps(taps, coefV[t], acc);
        }
        if (laneOut)
          _mm256_storeu_ps(laneOut[c] + (size_t)j * LANES, acc);
        frameOut[c] = HorizontalSum8(_mm256_mul_ps(acc, frameGains));
      }
      /* Mono duplicates its single rendered channel to both output
       * channels, matching the scalar path. */
      output[(size_t)j * 2] += frameOut[0];
      output[(size_t)j * 2 + 1] += frameOut[nChannels > 1 ? 1 : 0];

      /* Advance the 8 positions - same integer arithmetic as
       * ResamplingPosition::Inc(). */
      frac = _mm256_add_epi32(frac, incr);
      rel = _mm256_add_epi32(
        rel, _mm256_srli_epi32(frac, GOSoundResample::UPSAMPLE_BITS));
      frac = _mm256_and_si256(frac, maskV);
      gains = _mm256_add_ps(gains, dg);
    }

    _mm256_store_si256((__m256i *)relA, rel);
    _mm256_store_si256((__m256i *)fracA, frac);
    for (unsigned l = 0; l < numLanes; l++) {
      lanes[l].rel = relA[l];
      lanes[l].fraction = fracA[l];
    }
  }

  /* Interleaved -> planar 8x8 transpose of a group's decode scratch
   * (render variant D, 2026-07-12): flips `span` source samples of all
   * LANES lanes so every lane's samples lie contiguous - ready for the
   * classic contiguous-tap polyphase, which measures 2.90 vs the gather
   * kernel's 4.85 ns/voice-frame (fir_batch_vs_planar_bench.cpp). The
   * lockstep decoder's natural output stays interleaved (one 32B row
   * store per sample across the lanes); this pass pays the layout
   * conversion ONCE on the thin stream - each source sample serves ~8
   * output frames - instead of per frame on the dense side (sample
   * gathers today, or per-frame coefficient transposes in the
   * delay-line alternative). ~24 shuffles per 8 source rows, L1-warm.
   *
   * span must be a multiple of 8 (callers round up within the scratch
   * capacity; rows beyond the decoded content transpose garbage into
   * the planes' unread tail - the FIR never reads past valid+GUARD).
   * planes[c] receives lane l's run at planes[c] + l*planeStride. */
  GO_RESAMPLE_BATCH_FN static void TransposeToPlanes(
    const int32_t *const in[MAX_OUTPUT_CHANNELS],
    unsigned nChannels,
    unsigned span,
    int32_t *const planes[MAX_OUTPUT_CHANNELS],
    unsigned planeStride) {
    for (unsigned c = 0; c < nChannels; c++) {
      const int32_t *src = in[c];
      int32_t *dst = planes[c];

      for (unsigned s8 = 0; s8 < span; s8 += 8) {
        const __m256i r0 = _mm256_loadu_si256(
          (const __m256i *)(src + (size_t)(s8 + 0) * LANES));
        const __m256i r1 = _mm256_loadu_si256(
          (const __m256i *)(src + (size_t)(s8 + 1) * LANES));
        const __m256i r2 = _mm256_loadu_si256(
          (const __m256i *)(src + (size_t)(s8 + 2) * LANES));
        const __m256i r3 = _mm256_loadu_si256(
          (const __m256i *)(src + (size_t)(s8 + 3) * LANES));
        const __m256i r4 = _mm256_loadu_si256(
          (const __m256i *)(src + (size_t)(s8 + 4) * LANES));
        const __m256i r5 = _mm256_loadu_si256(
          (const __m256i *)(src + (size_t)(s8 + 5) * LANES));
        const __m256i r6 = _mm256_loadu_si256(
          (const __m256i *)(src + (size_t)(s8 + 6) * LANES));
        const __m256i r7 = _mm256_loadu_si256(
          (const __m256i *)(src + (size_t)(s8 + 7) * LANES));

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

        _mm256_storeu_si256(
          (__m256i *)(dst + 0 * (size_t)planeStride + s8),
          _mm256_permute2x128_si256(u0, u4, 0x20));
        _mm256_storeu_si256(
          (__m256i *)(dst + 1 * (size_t)planeStride + s8),
          _mm256_permute2x128_si256(u1, u5, 0x20));
        _mm256_storeu_si256(
          (__m256i *)(dst + 2 * (size_t)planeStride + s8),
          _mm256_permute2x128_si256(u2, u6, 0x20));
        _mm256_storeu_si256(
          (__m256i *)(dst + 3 * (size_t)planeStride + s8),
          _mm256_permute2x128_si256(u3, u7, 0x20));
        _mm256_storeu_si256(
          (__m256i *)(dst + 4 * (size_t)planeStride + s8),
          _mm256_permute2x128_si256(u0, u4, 0x31));
        _mm256_storeu_si256(
          (__m256i *)(dst + 5 * (size_t)planeStride + s8),
          _mm256_permute2x128_si256(u1, u5, 0x31));
        _mm256_storeu_si256(
          (__m256i *)(dst + 6 * (size_t)planeStride + s8),
          _mm256_permute2x128_si256(u2, u6, 0x31));
        _mm256_storeu_si256(
          (__m256i *)(dst + 7 * (size_t)planeStride + s8),
          _mm256_permute2x128_si256(u3, u7, 0x31));
      }
    }
  }

  /**
   * Filter+fade+accumulate pass for up to LANES ToneBalance voices, fed
   * from the main kernel's laneOut (their batch-accumulate contribution
   * was neutralized via zero gain there). Replaces the per-voice scalar
   * legacy detour (copy, fader.Process, FilterState::ProcessBuffer,
   * accumulate) with one vectorized pass; the semantic order matches the
   * scalar path exactly: gain FIRST (s = x * gain, float, like
   * fader.Process), then the first-order IIR on the gained signal, then
   * accumulation.
   *
   * Numerics: coefficients are double (as in GOSoundFilter); the state
   * recursion runs fully in double within the pass and the state is
   * truncated to float only at export (the scalar loop truncates every
   * frame - a sub-ulp difference that decays with the filter pole).
   *
   * Non-participating lanes must have zero coefficients, zero state and
   * zero gain columns: they then contribute exactly 0 to every frame.
   *
   * @param b0,b1,a1  per-lane filter coefficients (zeros = inert lane)
   * @param state     per-channel per-lane IIR state, in/out
   * @param laneOut   the main kernel's raw resampled lanes,
   *                  laneOut[c][frame * LANES + lane]
   * @param gains     per-frame gains, gains[frame * LANES + lane]
   * @param nChannels 1 = mono: channel 0 is filtered once (state[0]) and
   *                  its result duplicated to both output channels -
   *                  matching the classic detour's duplicated temp
   *                  buffer; state[1] is left untouched (the caller
   *                  mirrors state[0] into it, keeping a later classic-
   *                  path takeover consistent)
   * @param output    interleaved stereo, ACCUMULATED
   */
  GO_RESAMPLE_BATCH_FN static void FilterFadeAccumulate(
    const double b0[LANES],
    const double b1[LANES],
    const double a1[LANES],
    double state[MAX_OUTPUT_CHANNELS][LANES],
    const float *const laneOut[MAX_OUTPUT_CHANNELS],
    const float *gains,
    unsigned nChannels,
    unsigned nFrames,
    float *output) {
    const __m256d b0lo = _mm256_loadu_pd(b0);
    const __m256d b0hi = _mm256_loadu_pd(b0 + 4);
    const __m256d b1lo = _mm256_loadu_pd(b1);
    const __m256d b1hi = _mm256_loadu_pd(b1 + 4);
    const __m256d a1lo = _mm256_loadu_pd(a1);
    const __m256d a1hi = _mm256_loadu_pd(a1 + 4);
    __m256d stLo[MAX_OUTPUT_CHANNELS], stHi[MAX_OUTPUT_CHANNELS];

    for (unsigned c = 0; c < nChannels; c++) {
      stLo[c] = _mm256_loadu_pd(state[c]);
      stHi[c] = _mm256_loadu_pd(state[c] + 4);
    }

    for (unsigned j = 0; j < nFrames; j++) {
      const __m256 gv = _mm256_loadu_ps(gains + (size_t)j * LANES);
      float frameOut[MAX_OUTPUT_CHANNELS];

      for (unsigned c = 0; c < nChannels; c++) {
        // s = x * gain, in float - the same operation and precision as
        // the scalar fader.Process pre-scaling.
        const __m256 sv = _mm256_mul_ps(
          _mm256_loadu_ps(laneOut[c] + (size_t)j * LANES), gv);
        const __m256d sLo = _mm256_cvtps_pd(_mm256_castps256_ps128(sv));
        const __m256d sHi = _mm256_cvtps_pd(_mm256_extractf128_ps(sv, 1));

        // out = B0*s + state; state' = B1*s - A1*out (first-order IIR,
        // explicit fma/fmsub - the test reference reproduces exactly
        // this op sequence)
        const __m256d outLo = _mm256_fmadd_pd(b0lo, sLo, stLo[c]);
        const __m256d outHi = _mm256_fmadd_pd(b0hi, sHi, stHi[c]);

        stLo[c] = _mm256_fmsub_pd(b1lo, sLo, _mm256_mul_pd(a1lo, outLo));
        stHi[c] = _mm256_fmsub_pd(b1hi, sHi, _mm256_mul_pd(a1hi, outHi));

        const __m256 contrib = _mm256_insertf128_ps(
          _mm256_castps128_ps256(_mm256_cvtpd_ps(outLo)),
          _mm256_cvtpd_ps(outHi),
          1);

        frameOut[c] = HorizontalSum8(contrib);
      }
      output[(size_t)j * 2] += frameOut[0];
      output[(size_t)j * 2 + 1] += frameOut[nChannels > 1 ? 1 : 0];
    }

    for (unsigned c = 0; c < nChannels; c++) {
      _mm256_storeu_pd(state[c], stLo[c]);
      _mm256_storeu_pd(state[c] + 4, stHi[c]);
    }
  }
};

#ifndef __clang__
#pragma GCC pop_options
#endif

#endif // GO_RESAMPLE_BATCH_AVX2

#endif // GOSOUNDRESAMPLEBATCH_H
