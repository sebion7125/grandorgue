/*
 * GrandOrgue - free pipe organ simulator based on MyOrgan
 *
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2024 GrandOrgue contributors (see AUTHORS)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef GOSOUNDRESAMPLE_H_
#define GOSOUNDRESAMPLE_H_

#include <cstdint>

#include "GOHotPath.h"

/* AVX2 polyphase kernel: only for x86 gcc/clang builds. Uses a runtime
 * __builtin_cpu_supports check, so the binary stays runnable on older CPUs. */
#if (defined(__GNUC__) || defined(__clang__))                                  \
  && (defined(__x86_64__) || defined(__i386__))
#define GO_POLYPHASE_AVX2 1
#include <immintrin.h>
#endif

/**
 * This class provides algorithms for resampling audio buffers
 * Now two algorithms are supported: Linear and Polyphase.
 * They calculate a next output sample based on a vector of a few continous
 * input samples.
 */

class GOSoundResample {
public:
  static constexpr unsigned POLYPHASE_POINTS = 8;
  static constexpr unsigned LINEAR_POINTS = 2;
  static constexpr unsigned UPSAMPLE_BITS = 13;
  static constexpr unsigned UPSAMPLE_FACTOR = 1 << UPSAMPLE_BITS;
  static constexpr unsigned UPSAMPLE_MASK = UPSAMPLE_FACTOR - 1;

  enum InterpolationType {
    GO_LINEAR_INTERPOLATION = 0,
    GO_POLYPHASE_INTERPOLATION = 1,
  };

  /**
   * A position in the source sample stream for the current position in the
   * target stream. Because the numbers of source and target samples differ, the
   * source position may be not integer and has a fractional part
   */
  class ResamplingPosition {
  private:
    // an integer part
    unsigned m_index;
    // a fractional part in 1/UPSAMPLE_FACTOR units
    unsigned m_fraction;
    // Increment of the source position for one target sample in
    //   1/UPSAMPLE_FACTOR units
    unsigned m_FractionIncrement;

  public:
    GO_ALWAYS_INLINE unsigned GetIndex() const { return m_index; }
    inline void SetIndex(unsigned newIndex) { m_index = newIndex; }
    GO_ALWAYS_INLINE unsigned GetFraction() const { return m_fraction; }
    inline unsigned GetFractionIncrement() const { return m_FractionIncrement; }

    /**
     * A resampling factor equals to (source length / target length) or to
     * (source sample rate / target sample rate)
     * @return the resampling factor
     */
    inline float GetResamplingFactor() const {
      return (float)m_FractionIncrement / UPSAMPLE_FACTOR;
    }

    void Init(
      float factor,
      unsigned startIndex = 0,
      const ResamplingPosition *pOld = nullptr);

    /**
     * Advance the position for the next target sample
     */
    GO_ALWAYS_INLINE void Inc() {
      m_fraction += m_FractionIncrement;
      m_index += m_fraction >> UPSAMPLE_BITS;
      m_fraction &= UPSAMPLE_MASK;
    }

    /**
     * Advance the position by n target samples in one step. Bit-identical
     * to calling Inc() n times (needed by GOSoundStream's decode-span
     * prediction, which must reproduce the resampler's integer arithmetic
     * exactly, not approximate it).
     */
    inline void AdvanceBy(unsigned n) {
      const uint64_t fraction
        = (uint64_t)m_fraction + (uint64_t)n * m_FractionIncrement;

      m_index += (unsigned)(fraction >> UPSAMPLE_BITS);
      m_fraction = (unsigned)(fraction & UPSAMPLE_MASK);
    }

    /**
     * The integer source index the position will have after n more target
     * samples, without modifying the position. Exact (same arithmetic as
     * Inc()).
     */
    inline unsigned IndexAfter(unsigned n) const {
      return m_index
        + (unsigned)(((uint64_t)m_fraction + (uint64_t)n * m_FractionIncrement)
                     >> UPSAMPLE_BITS);
    }

    /**
     * Calculates the target samples length from given source position to the
     * end index
     * @param endIndex the first index position after the last source sample
     * @result - the number of tatget samples can be received before the current
     *   position exceeds endIndex specified
     */
    inline unsigned AvailableTargetSamples(unsigned endIndex) {
      return m_index < endIndex ? unsigned(
               (
                 // for preventing owerflowing if endIndex >= 2**19 ~ 550000
                 uint64_t(endIndex - m_index) * UPSAMPLE_FACTOR
                 + m_FractionIncrement - 1 // for rounding up
                 - m_fraction)
               / m_FractionIncrement)
                                : 0;
    }
  };

  /**
   * Represents an abstract vector of continous input samples somethere in the
   * input stream. It has nChannels input channels (1 - mono, 2 - stereo)
   * It has two main methods:
   * - void Seek(unsigned index, uint8_t channel) - sets the vector to the index
   *   position in the input stream for the specified channel. The
   *   implementation may restrict moving the position only forward
   * - void NextSample() - returns the next sample of the same channel. The
   *   implementation must provide sufficient number of samples required by
   *   the certain resampling algorithm.
   *
   * These methods are not virtual for a better performance. Their
   * implementation is substituted inline from the subclasses by a resampler.
   */
  template <uint8_t nChannels> struct FloatingSampleVector {
    static constexpr uint8_t m_NChannels = nChannels;
    /* True only for windows whose FIR taps are contiguous int32 in memory
     * after Seek() and that provide GetPtr() (see StreamCacheReadAheadWindow).
     * Enables the permute-free AVX2 kernel in PolyphaseResampler. */
    static constexpr bool kPlanarInt32Taps = false;
  };

  /**
   * A vector of continous samples in a memory region referenced by a pointer.
   * SampleT - a type of one sample. Usually int8_t, int16_t, GOInt24, or float
   * ResT - a type of one sample returned by NextSample(). Usually int or float
   * nChannels - number of channels in the source stream
   *
   * If nChannels>1 it asumes that samples are interleaving for several channels
   *     - 0 left
   *     - 0 right
   *     - 1 left
   *     - 1 right
   *     - ...
   * For the performance reason this NextSample() does not check for the bounds.
   * The calling program must ensure that there are sufficient number of samples
   */
  template <class SampleT, class ResT, uint8_t nChannels>
  class PtrSampleVector : public FloatingSampleVector<nChannels> {
  private:
    // points to the first sample of the 0-channel in the input stream
    const SampleT *p_StartPtr;
    // points to the current sample in the vector
    const SampleT *p_CurrPtr;

  protected:
    /**
     * Checks that the current sample pointer is before the specified end
     * pointer
     * @param endPtr the end pointer
     * @return are there more samples in the vector
     */
    inline bool IsBefore(const SampleT *endPtr) const {
      return p_CurrPtr < endPtr;
    }

  public:
    /**
     * Construct the vector with some start pointer.
     * @param ptr a pointer to the first (0 left) sample
     */
    inline PtrSampleVector(const SampleT *ptr) : p_StartPtr(ptr) {}

    /**
     * Moves the current sample pointer to the specified position in the input
     * stream for the channel specified
     * @param ptr a pointer to the first (0 left) sample
     */
    inline void Seek(unsigned index, uint8_t channel) {
      p_CurrPtr = p_StartPtr + nChannels * index + channel;
    }

    /**
     * Returns the next sample from the vector and moves the current sample
     * pointer to the next sample of the same channel
     * @return the sample
     */
    inline ResT NextSample() {
      ResT res = (ResT)*p_CurrPtr;
      p_CurrPtr += nChannels;
      return res;
    }
  };

  /**
   * A vector of continous samples in memory with checking for bounds on
   * NextSample().
   * This checking reduces the performance dramatically so it is intended to use
   * only in not realtime cases (for example, on loading, but not when playing)
   */
  template <class SampleT, class ResT, uint8_t nChannels>
  class BoundedPtrSampleVector
    : public PtrSampleVector<SampleT, ResT, nChannels> {
  private:
    /**
     * Points to the end of the memory region
     */
    const SampleT *p_EndPtr;

  public:
    /**
     * Constructs the vector with the start pointer and the length of the
     * memory region
     * @param ptr - a pointer to the first sample in the region
     * @param len - a number of samples of each channels
     */
    inline BoundedPtrSampleVector(const SampleT *ptr, unsigned len)
      : PtrSampleVector<SampleT, ResT, nChannels>(ptr),
        p_EndPtr(ptr + nChannels * len) {}

    inline ResT NextSample() {
      return PtrSampleVector<SampleT, ResT, nChannels>::IsBefore(p_EndPtr)
        ? PtrSampleVector<SampleT, ResT, nChannels>::NextSample()
        : (ResT)0;
    }
  };

  // These cofficients are calculated in the constructor and are not more
  // changed

/* Mini polyphase coefficient table (2026-07-12): the full 8192-row table
 * is 256 KB - exactly one core's L2 - and every voice walks it with its
 * own random phase line (random start + random-speaking increment), so at
 * high polyphony the whole table must stay warm and every coefficient
 * fetch is an L2 access on the render's dependency chain. A decimated
 * table with linear interpolation between adjacent rows (plus the
 * polyphase wrap identity coefs[p+8192][k] = coefs[p][k-1] for the top
 * neighbour) measures -121 dB against the exact table at 6 bits (2 KB,
 * 32 cache lines - permanently L1-resident) across all detunes up to a
 * fifth (tools/compression/coef_row_tolerance_gate.cpp v2): 25 dB below
 * the 16-bit dither floor. The lerp costs 3 vector ALU ops that hide in
 * the memory-stall shadow; the removed L2 access does not. 0 disables
 * (full table, previous behaviour). */
#ifndef GO_POLYPHASE_SMALL_BITS
#define GO_POLYPHASE_SMALL_BITS 6
#endif

  // The coefficients for linear interpolation
  float m_LinearCoefs[UPSAMPLE_FACTOR][LINEAR_POINTS];
  // The coefficients for polyphase interpolation
  float m_PolyphaseCoefs[UPSAMPLE_FACTOR][POLYPHASE_POINTS];
#if GO_POLYPHASE_SMALL_BITS
  static constexpr unsigned SMALL_ROWS = 1u << GO_POLYPHASE_SMALL_BITS;
  static constexpr unsigned SMALL_SHIFT
    = UPSAMPLE_BITS - GO_POLYPHASE_SMALL_BITS;
  // +1: top neighbour row for the lerp (wrap identity, see above).
  float m_PolyphaseSmall[SMALL_ROWS + 1][POLYPHASE_POINTS];
#endif

  GOSoundResample();

  /**
   * A resampler that calculates a next output sample as a scalar production of
   * the vector of continous input samples and the vector of coefficients
   */
  template <unsigned nPoints> class ScalarProductionResampler {
  protected:
    // precalculated coefficients for each resampling position fraction
    const float (&r_coefs)[UPSAMPLE_FACTOR][nPoints];

  protected:
    inline ScalarProductionResampler(
      const float (&coefs)[UPSAMPLE_FACTOR][nPoints])
      : r_coefs(coefs) {}

  public:
    /**
     * @return Necessary number of continous input samples for calculating one
     * output sample
     */
    static constexpr unsigned VECTOR_LENGTH = nPoints;

    /**
     * Do actual resampling of an input sample block to the output block of the
     *   given number of samples
     * @param resamplingPos A resampling position in the input stream. It is
     *   advanced during this call
     * @param sV a floating sample vector linked to the input stream
     * @param pOut a pointer to the output sample buffer in interleaving format.
     *   Must have at least nOutChannels*nOutSamples length
     * @param nOutSamples a number of output samples of each channel
     */
    template <class SampleVectorT, uint8_t nOutChannels>
    inline void ResampleBlock(
      ResamplingPosition &resamplingPos,
      SampleVectorT &sV,
      float *pOut,
      unsigned nOutSamples) const {
      for (unsigned i = 0; i < nOutSamples; i++, resamplingPos.Inc()) {
        const float(&coefs)[nPoints] = r_coefs[resamplingPos.GetFraction()];
        float outSample = 0.0f;

        for (uint8_t ch = 0; ch < nOutChannels; ch++) {
          if (ch < SampleVectorT::m_NChannels) {
            const float *pCoef = coefs;

            sV.Seek(resamplingPos.GetIndex(), ch);
            // calculate the next output sample as a scalar production of the
            // input sample vector and the vector of coefficients
            outSample = 0.0f;
            for (unsigned j = 0; j < nPoints; j++)
              outSample += sV.NextSample() * *(pCoef++);
          }
          /* else copy the calculated sample from the previous channel. It is
           * useful only for resampling a mono stream to a stereo one */
          *(pOut++) = outSample;
        }
      }
    }
  };

  /**
   * This resampler use a scalar production of vector of two samples and a
   * vector of two coefficients
   */
  struct LinearResampler : public ScalarProductionResampler<LINEAR_POINTS> {
    inline LinearResampler(const GOSoundResample &r)
      : ScalarProductionResampler<LINEAR_POINTS>(r.m_LinearCoefs) {}
  };

  /**
   * This resampler use a scalar production of vector of eight samples and a
   * vector of eight coefficients.
   *
   * For sample windows with planar int32 taps (kPlanarInt32Taps, currently
   * only the compressed-audio StreamCacheReadAheadWindow) it takes a
   * permute-free AVX2 path: the 8 taps of one channel are one contiguous
   * 256-bit load, converted to float, multiplied by the coefficient row and
   * horizontally summed. This avoids the port-5-bound deinterleave permute
   * that made the previous SIMD attempt on the interleaved layout a wash
   * (see the scratch/simd-polyphase-experiment branch).
   */
  struct PolyphaseResampler
    : public ScalarProductionResampler<POLYPHASE_POINTS> {
#if GO_POLYPHASE_SMALL_BITS
    const float (*r_small)[POLYPHASE_POINTS];

    inline PolyphaseResampler(const GOSoundResample &r)
      : ScalarProductionResampler<POLYPHASE_POINTS>(r.m_PolyphaseCoefs),
        r_small(r.m_PolyphaseSmall) {}

    /* Interpolated coefficient row: lerp between the two adjacent small-
     * table rows. Same arithmetic in the scalar and AVX2 paths so both
     * produce identical values. */
    inline void SmallCoefRow(unsigned fraction, float out[POLYPHASE_POINTS])
      const {
      const unsigned i = fraction >> SMALL_SHIFT;
      const float w = (float)(fraction - (i << SMALL_SHIFT))
        / (float)(1u << SMALL_SHIFT);
      const float *a = r_small[i];
      const float *b = r_small[i + 1];

      for (unsigned k = 0; k < POLYPHASE_POINTS; k++)
        out[k] = a[k] + (b[k] - a[k]) * w;
    }
#else
    inline PolyphaseResampler(const GOSoundResample &r)
      : ScalarProductionResampler<POLYPHASE_POINTS>(r.m_PolyphaseCoefs) {}
#endif

#ifdef GO_POLYPHASE_AVX2
  private:
    static bool hasAvx2() {
      static const bool has = __builtin_cpu_supports("avx2");

      return has;
    }

    /* A separate target("avx2") function instead of a lambda: gcc lambdas do
     * not inherit the target attribute from the enclosing function and fail
     * to compile intrinsics. */
    /* Joint 4-way horizontal fold (2026-07-13 "Vektor-Kachel"): folds
     * FOUR 8-lane accumulators into one xmm [sumA,sumB,sumC,sumD] with
     * 3 hadd + 1 extract/add - ~5 shuffle-class ops for 4 results
     * instead of 4x4 for individual folds. Note: groups the addends
     * differently than horizontalSum8 (last-bit differences, same class
     * of deviation the batch kernel documented). Bench: kernel-level
     * 0.60x vs the per-frame fold + temp + separate accumulate
     * (fir_batch_vs_planar_bench scenario K). */
    __attribute__((target("avx2"))) static inline __m128 fold4(
      __m256 a, __m256 b, __m256 c, __m256 d) {
      const __m256 s1 = _mm256_hadd_ps(a, b);
      const __m256 s2 = _mm256_hadd_ps(c, d);
      const __m256 s3 = _mm256_hadd_ps(s1, s2);

      return _mm_add_ps(
        _mm256_castps256_ps128(s3), _mm256_extractf128_ps(s3, 1));
    }

    /* Coefficient row for one fraction as a vector - shared by the tile
     * kernels below (mini-table lerp when enabled, plain row otherwise). */
    __attribute__((target("avx2"))) inline __m256 coefVec(
      unsigned fraction) const {
#if GO_POLYPHASE_SMALL_BITS
      const unsigned si = fraction >> SMALL_SHIFT;
      const __m256 wv = _mm256_set1_ps(
        (float)(fraction - (si << SMALL_SHIFT)) / (float)(1u << SMALL_SHIFT));
      const __m256 ca = _mm256_loadu_ps(r_small[si]);
      const __m256 cb = _mm256_loadu_ps(r_small[si + 1]);

      return _mm256_add_ps(ca, _mm256_mul_ps(_mm256_sub_ps(cb, ca), wv));
#else
      return _mm256_loadu_ps(r_coefs[fraction]);
#endif
    }

    __attribute__((target("avx2"))) static inline float horizontalSum8(
      __m256 v) {
      __m128 sum4
        = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));

      sum4 = _mm_hadd_ps(sum4, sum4);
      sum4 = _mm_hadd_ps(sum4, sum4);
      return _mm_cvtss_f32(sum4);
    }

    template <class SampleVectorT, uint8_t nOutChannels>
    __attribute__((target("avx2"), hot)) void ResampleBlockAvx2(
      ResamplingPosition &resamplingPos,
      SampleVectorT &sV,
      float *pOut,
      unsigned nOutSamples) const {
      static_assert(
        POLYPHASE_POINTS == 8, "the AVX2 kernel assumes an 8-tap FIR");
      unsigned i = 0;

      /* Vector-tile main loop (see fold4): 4 output frames in flight,
       * one joint fold, one packed 8-float store per stereo tile. */
      if constexpr (nOutChannels == 2) {
        for (; i + 4 <= nOutSamples; i += 4) {
          __m256 a0[4], a1[4];

          for (unsigned k = 0; k < 4; k++, resamplingPos.Inc()) {
            const __m256 coefs = coefVec(resamplingPos.GetFraction());

            sV.Seek(resamplingPos.GetIndex(), 0);
            a0[k] = _mm256_mul_ps(
              _mm256_cvtepi32_ps(
                _mm256_loadu_si256((const __m256i *)sV.GetPtr())),
              coefs);
            if constexpr (SampleVectorT::m_NChannels > 1) {
              sV.Seek(resamplingPos.GetIndex(), 1);
              a1[k] = _mm256_mul_ps(
                _mm256_cvtepi32_ps(
                  _mm256_loadu_si256((const __m256i *)sV.GetPtr())),
                coefs);
            }
          }

          const __m128 c0 = fold4(a0[0], a0[1], a0[2], a0[3]);
          const __m128 c1 = SampleVectorT::m_NChannels > 1
            ? fold4(a1[0], a1[1], a1[2], a1[3])
            : c0; // mono duplicated, like the scalar path
          const __m128 lo = _mm_unpacklo_ps(c0, c1);
          const __m128 hi = _mm_unpackhi_ps(c0, c1);

          _mm256_storeu_ps(
            pOut, _mm256_insertf128_ps(_mm256_castps128_ps256(lo), hi, 1));
          pOut += 8;
        }
      }

      // Frame remainder (and the generic nOutChannels != 2 shape).
      for (; i < nOutSamples; i++, resamplingPos.Inc()) {
        const __m256 coefs = coefVec(resamplingPos.GetFraction());
        float outSample = 0.0f;

        for (uint8_t ch = 0; ch < nOutChannels; ch++) {
          if (ch < SampleVectorT::m_NChannels) {
            sV.Seek(resamplingPos.GetIndex(), ch);

            const __m256 taps = _mm256_cvtepi32_ps(
              _mm256_loadu_si256((const __m256i *)sV.GetPtr()));

            outSample = horizontalSum8(_mm256_mul_ps(taps, coefs));
          }
          /* else copy the calculated sample from the previous channel (mono
           * source to stereo output), same as the scalar implementation */
          *(pOut++) = outSample;
        }
      }
    }

    template <class SampleVectorT, uint8_t nOutChannels>
    __attribute__((target("avx2"), hot)) void ResampleBlockAvx2Accum(
      ResamplingPosition &resamplingPos,
      SampleVectorT &sV,
      float *pOut,
      unsigned nOutSamples,
      float gain,
      float dGain) const {
      static_assert(
        POLYPHASE_POINTS == 8, "the AVX2 kernel assumes an 8-tap FIR");
      unsigned i = 0;

      /* Vector-tile accumulate (the CORRECT fusion granularity - the
       * per-frame scalar accumulate variant measurably lost against the
       * two-pass form): fold 4 frames jointly, then one 8-wide
       * mul(gain)+add into the output tile. The gain may be a linear
       * ramp, gain(i) = gain + i*dGain (dGain == 0 for steady-constant,
       * where the gain vector below stays bit-identical to the former
       * constant-only kernel): the vector walks in 4-frame steps, two
       * extra vaddps per tile. */
      if constexpr (nOutChannels == 2) {
        // Stereo-interleaved tile lanes: frames [i, i, i+1, i+1, ...]
        const __m256 gStep = _mm256_set1_ps(4 * dGain);
        __m256 gv = _mm256_add_ps(
          _mm256_set1_ps(gain),
          _mm256_set_ps(
            3 * dGain, 3 * dGain, 2 * dGain, 2 * dGain, dGain, dGain, 0, 0));

        for (; i + 4 <= nOutSamples; i += 4) {
          __m256 a0[4], a1[4];

          for (unsigned k = 0; k < 4; k++, resamplingPos.Inc()) {
            const __m256 coefs = coefVec(resamplingPos.GetFraction());

            sV.Seek(resamplingPos.GetIndex(), 0);
            a0[k] = _mm256_mul_ps(
              _mm256_cvtepi32_ps(
                _mm256_loadu_si256((const __m256i *)sV.GetPtr())),
              coefs);
            if constexpr (SampleVectorT::m_NChannels > 1) {
              sV.Seek(resamplingPos.GetIndex(), 1);
              a1[k] = _mm256_mul_ps(
                _mm256_cvtepi32_ps(
                  _mm256_loadu_si256((const __m256i *)sV.GetPtr())),
                coefs);
            }
          }

          const __m128 c0 = fold4(a0[0], a0[1], a0[2], a0[3]);
          const __m128 c1 = SampleVectorT::m_NChannels > 1
            ? fold4(a1[0], a1[1], a1[2], a1[3])
            : c0;
          const __m128 lo = _mm_unpacklo_ps(c0, c1);
          const __m128 hi = _mm_unpackhi_ps(c0, c1);
          const __m256 tile
            = _mm256_insertf128_ps(_mm256_castps128_ps256(lo), hi, 1);

          _mm256_storeu_ps(
            pOut,
            _mm256_add_ps(_mm256_loadu_ps(pOut), _mm256_mul_ps(tile, gv)));
          pOut += 8;
          gv = _mm256_add_ps(gv, gStep);
        }
      }

      for (; i < nOutSamples; i++, resamplingPos.Inc()) {
        const __m256 coefs = coefVec(resamplingPos.GetFraction());
        const float frameGain = gain + (float)i * dGain;
        float outSample = 0.0f;

        for (uint8_t ch = 0; ch < nOutChannels; ch++) {
          if (ch < SampleVectorT::m_NChannels) {
            sV.Seek(resamplingPos.GetIndex(), ch);

            const __m256 taps = _mm256_cvtepi32_ps(
              _mm256_loadu_si256((const __m256i *)sV.GetPtr()));

            outSample = horizontalSum8(_mm256_mul_ps(taps, coefs));
          }
          *pOut += outSample * frameGain;
          pOut++;
        }
      }
    }

  public:
    /* Shadows the inherited scalar ResampleBlock. Falls back to it for
     * windows without planar int32 taps and on CPUs without AVX2. */
    template <class SampleVectorT, uint8_t nOutChannels>
    inline void ResampleBlock(
      ResamplingPosition &resamplingPos,
      SampleVectorT &sV,
      float *pOut,
      unsigned nOutSamples) const {
      if constexpr (SampleVectorT::kPlanarInt32Taps) {
        if (hasAvx2()) {
          ResampleBlockAvx2<SampleVectorT, nOutChannels>(
            resamplingPos, sV, pOut, nOutSamples);
          return;
        }
      }
#if GO_POLYPHASE_SMALL_BITS
      /* Scalar fallback with the SAME interpolated rows as the AVX2 path
       * (the inherited ScalarProductionResampler loop reads the full
       * table and would diverge from it). */
      for (unsigned i = 0; i < nOutSamples; i++, resamplingPos.Inc()) {
        float coefs[POLYPHASE_POINTS];
        float outSample = 0.0f;

        SmallCoefRow(resamplingPos.GetFraction(), coefs);
        for (uint8_t ch = 0; ch < nOutChannels; ch++) {
          if (ch < SampleVectorT::m_NChannels) {
            sV.Seek(resamplingPos.GetIndex(), ch);
            outSample = 0.0f;
            for (unsigned j = 0; j < POLYPHASE_POINTS; j++)
              outSample += sV.NextSample() * coefs[j];
          }
          *(pOut++) = outSample;
        }
      }
#else
      ScalarProductionResampler<POLYPHASE_POINTS>::ResampleBlock<
        SampleVectorT,
        nOutChannels>(resamplingPos, sV, pOut, nOutSamples);
#endif
    }

    /* Fused resample + linear-gain accumulate, gain(i) = gain + i*dGain
     * (see the AVX2 kernel's comment). Same dispatch rules as
     * ResampleBlock. */
    template <class SampleVectorT, uint8_t nOutChannels>
    inline void ResampleBlockAccum(
      ResamplingPosition &resamplingPos,
      SampleVectorT &sV,
      float *pOut,
      unsigned nOutSamples,
      float gain,
      float dGain) const {
      if constexpr (SampleVectorT::kPlanarInt32Taps) {
        if (hasAvx2()) {
          ResampleBlockAvx2Accum<SampleVectorT, nOutChannels>(
            resamplingPos, sV, pOut, nOutSamples, gain, dGain);
          return;
        }
      }
      for (unsigned i = 0; i < nOutSamples; i++, resamplingPos.Inc()) {
        float coefs[POLYPHASE_POINTS];
        const float frameGain = gain + (float)i * dGain;
        float outSample = 0.0f;

#if GO_POLYPHASE_SMALL_BITS
        SmallCoefRow(resamplingPos.GetFraction(), coefs);
#else
        for (unsigned k = 0; k < POLYPHASE_POINTS; k++)
          coefs[k] = r_coefs[resamplingPos.GetFraction()][k];
#endif
        for (uint8_t ch = 0; ch < nOutChannels; ch++) {
          if (ch < SampleVectorT::m_NChannels) {
            sV.Seek(resamplingPos.GetIndex(), ch);
            outSample = 0.0f;
            for (unsigned j = 0; j < POLYPHASE_POINTS; j++)
              outSample += sV.NextSample() * coefs[j];
          }
          *pOut += outSample * frameGain;
          pOut++;
        }
      }
    }

#endif // GO_POLYPHASE_AVX2
  };

  /**
   * Returns the necessary sample vector length for the given interpolation type
   * @param interpolation - the interpolation type
   * @return the number of samples
   */
  static unsigned getVectorLength(InterpolationType interpolation);

  /**
   * Allocate a new sample block and fill it with resampled data. Assume that
   *   the samples are mono
   * @param data a pointer to the source samples
   * @param before call - number of source samples. After call it is filled
   *   with number of target samples
   * @param from_samplerate source samplerate
   * @param to_samplerate target samplerate
   * @return a pointer to the new allocated data block. The caller is
   *   responsible to free this block
   */
  float *NewResampledMono(
    const float *data,
    unsigned &len,
    unsigned from_samplerate,
    unsigned to_samplerate);
};

#endif /* GOSOUNDRESAMPLE_H_ */
