/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundFader.h"
#include "../GOCrossfadeMode.h"
#include "../GOCrossfadeParam.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <wx/log.h>
#include <wx/memory.h>
#include <wx/string.h>

static inline float sanitize_vol(float vol, float /*lastVol*/) {
  // Hot path: no checks/clamps (performance/determinism).
  return vol;
}

static inline float go_fade_in_multiplier_at(
  GOCrossfadeMode mode, unsigned len, unsigned pos) {
  if (len <= 1)
    return 1.0f;
  pos = std::min(pos, len - 1);
  const float t = float(pos) / float(len - 1);
  return go_crossfade_eval(mode, t).b;
}

static inline float go_fade_out_multiplier_at(
  GOCrossfadeMode mode, unsigned len, unsigned pos) {
  if (len <= 1)
    return 0.0f;
  pos = std::min(pos, len - 1);
  const float t = float(pos) / float(len - 1);
  return go_crossfade_eval(mode, t).a;
}

static inline float go_compute_target_state_after_block(
  GOCrossfadeMode mode,
  float targetVolume,
  bool wasInActive,
  bool isInActive,
  unsigned inLen,
  unsigned inPos,
  bool wasOutActive,
  bool isOutActive,
  unsigned outLen,
  unsigned outPos) {
  float mult = 1.0f;

  if (wasInActive && isInActive)
    mult *= go_fade_in_multiplier_at(mode, inLen, inPos);

  if (wasOutActive) {
    if (isOutActive)
      mult *= go_fade_out_multiplier_at(mode, outLen, outPos);
    else
      mult = 0.0f;
  }

  return targetVolume * mult;
}

static inline void go_advance_legacy_shadow_state(
  unsigned nFrames,
  bool &inActive,
  unsigned &inPos,
  unsigned inLen,
  float increasingDeltaPerFrame,
  bool &outActive,
  unsigned &outPos,
  unsigned outLen,
  float decreasingDeltaPerFrame,
  float lastTargetVolumePoint) {
  if (inActive) {
    if (increasingDeltaPerFrame > 0.0f) {
      inPos = std::min(inPos + nFrames, inLen);
    } else {
      inPos = inLen;
      inActive = false;
    }
  }

  if (outActive) {
    if (decreasingDeltaPerFrame > 0.0f && lastTargetVolumePoint > 0.0f) {
      outPos = std::min(outPos + nFrames, outLen);
    } else {
      outPos = outLen;
      outActive = false;
    }
  }
}

void GOSoundFader::Setup(
  float targetVolume, float velocityVolume, unsigned nFramesToIncreaseIn) {

  using namespace GOAudioParams;
  m_CurrentFadeMode = GetCrossfadeMode();
  m_ModeCached = m_CurrentFadeMode;
  m_ModeOverrideLinear = false;

  m_TargetVolume = targetVolume;
  m_VelocityVolume = velocityVolume;
  m_DecreasingDeltaPerFrame = 0.0f; // no FadeOut active
  m_LastExternalVolumePoint = -1.0f;

  m_CurrentSampleCounter = 0;

  // init envelopes
  if (nFramesToIncreaseIn == 0) {
    m_InActive = false;
    m_InLen = m_InPos = 0;
    m_LastTargetVolumePoint = targetVolume; // for legacy paths
    m_FadeInLengthSamples = 0;
    m_FadeStartSample = 0;
    m_FadeStartVolume = targetVolume;
    m_IncreasingDeltaPerFrame = 0.0f;
  } else {
    m_InActive = true;
    m_InLen = nFramesToIncreaseIn;
    m_InPos = 0;
    m_LastTargetVolumePoint = 0.0f;
    m_FadeInLengthSamples = nFramesToIncreaseIn;
    m_FadeStartSample = 0;

    if (m_CurrentFadeMode != GOCrossfadeMode::Linear) {
      m_FadeStartVolume = 0.0f;
      // marker for non-linear (legacy code paths may check >0)
      m_IncreasingDeltaPerFrame = 1.0f;
      m_FadeStartSample = m_CurrentSampleCounter = 0;
    } else {
      m_FadeStartVolume = targetVolume;
      m_IncreasingDeltaPerFrame = targetVolume / nFramesToIncreaseIn;
    }

    // Pointer-only: pre-bind template for fade-in; avoid trig/sqrt init in
    // Setup.
    if (m_CurrentFadeMode != GOCrossfadeMode::Linear && m_InLen > 0) {
      GOAudioParams::FastCrossfadeCache::EnsureTemplate(m_ModeCached, m_InLen);
      m_TplIn
        = GOAudioParams::FastCrossfadeCache::GetTemplate(m_ModeCached, m_InLen);
    } else {
      m_TplIn.reset();
    }
  }

  // out envelope init
  m_OutActive = false;
  m_OutLen = m_OutPos = 0;
  m_FadeOutLengthSamples = 0;
  // m_DecreasingDeltaPerFrame remains 0.0f (no fade-out active)
}

// if the external volume is changed, do it smoothly in this number of frames
static constexpr unsigned EXTERNAL_VOLUME_CHANGE_FRAMES = 1024;

/* One smoothing step of the external volume toward its target: covers
 * nFrames/1024 of the remaining distance (geometric approach). SNAP
 * GUARD: in float the step eventually rounds to a no-op while still
 * != target (tools/compression/fader_extvol_stall_check.cpp: 100% of
 * 200k random pairs freeze ~3e-7 short of the target) - without the
 * snap, every voice that ever saw an external-volume change classifies
 * as Ramp forever (2026-07-12 report: 63% of held voices in the Ramp
 * kernel, locked out of the steady-constant fast path). When the step
 * can make no progress, jump to the target: a ~-130 dB step. */
static inline float go_step_external_volume(
  float start, float target, unsigned nFrames) {
  const float k = float(std::min(nFrames, EXTERNAL_VOLUME_CHANGE_FRAMES))
    / float(EXTERNAL_VOLUME_CHANGE_FRAMES);
  const float next = start + (target - start) * k;

  return next == start ? target : next;
}

void GOSoundFader::Process(
  unsigned nFrames, float *buffer, float externalVolume) {
  // setup process

  // Legacy fast-path: if Linear, continue with existing linear logic below.
  // For any non-linear mode, delegate to the non-linear processor - unless
  // this fader was pinned to the legacy linear path by
  // StartDecreasingVolume(..., forceLinear=true) (see m_ModeOverrideLinear).
  if (
    !m_ModeOverrideLinear
    && GOAudioParams::GetCrossfadeMode() != GOCrossfadeMode::Linear) {
    ProcessNonLinearFade(nFrames, buffer, externalVolume);
    return;
  }

  float startTargetVolumePoint = m_LastTargetVolumePoint;

  // Calculate new m_LastTargetVolumePoint
  // the target volume will be changed from startTargetVolumePoint to
  // m_LastTargetVolumePoint during the nFrames

  unsigned framesLeftAfterIncreasing = nFrames;

  // increasing section
  if (m_IncreasingDeltaPerFrame > 0.0f) {
    // we are increasing now
    float newTargetVolumePoint
      = m_LastTargetVolumePoint + m_IncreasingDeltaPerFrame * nFrames;

    if (newTargetVolumePoint < m_TargetVolume) {
      framesLeftAfterIncreasing = 0; // we are increase during the whole period
      m_LastTargetVolumePoint = newTargetVolumePoint;
    } else {
      // we reach m_TargetVolume inside this nFrames period

      // calculate how many frames left after reaching m_TargetVolume
      framesLeftAfterIncreasing = nFrames
        - unsigned((m_TargetVolume - m_LastTargetVolumePoint)
                   / m_IncreasingDeltaPerFrame);
      // stop increasing
      m_IncreasingDeltaPerFrame = 0.0f;
      m_LastTargetVolumePoint = m_TargetVolume;
    }
  }

  // decreasing section
  if (framesLeftAfterIncreasing && m_DecreasingDeltaPerFrame > 0.0f) {
    // decrease the volume
    float newTargetVolumePoint = m_LastTargetVolumePoint
      - m_DecreasingDeltaPerFrame * framesLeftAfterIncreasing;

    if (newTargetVolumePoint > 0.0f) {
      m_LastTargetVolumePoint = newTargetVolumePoint;
    } else {
      // stop decreasing
      m_DecreasingDeltaPerFrame = 0.0f;
      m_LastTargetVolumePoint = 0.0f;
    }
  }

  // Calculate the external volume
  float targetExternalVolume = m_VelocityVolume * externalVolume;

  if (m_LastExternalVolumePoint < 0.0f)
    m_LastExternalVolumePoint = targetExternalVolume;

  float startExternalVolumePoint = m_LastExternalVolumePoint;

  // Calculate new m_LastExternalVolumePoint
  // the target volume will be changed from startExternalVolumePoint to
  // m_LastExternalVolumePoint during the nFrames period
  if (targetExternalVolume != startExternalVolumePoint)
    m_LastExternalVolumePoint = go_step_external_volume(
      startExternalVolumePoint, targetExternalVolume, nFrames);

  float frameTotalVolume = startTargetVolumePoint * startExternalVolumePoint;

  // Process data
  if (
    (m_LastTargetVolumePoint == startTargetVolumePoint)
    && (m_LastExternalVolumePoint == startExternalVolumePoint)) {
    // Adjust the buffer by frameTotalVolume
    for (unsigned int i = 0; i < nFrames; i++, buffer += 2) {
      buffer[0] *= frameTotalVolume;
      buffer[1] *= frameTotalVolume;
    }
  } else {
    // Adjust the buffer smoothly from frameTotalVolume to
    // m_LastTargetVolumePoint * m_LastExternalVolumePoint
    float frameTotalVolumeDelta // changing the volume by one frame
      = (m_LastTargetVolumePoint * m_LastExternalVolumePoint - frameTotalVolume)
      / nFrames;

    for (unsigned int i = 0; i < nFrames; i++, buffer += 2) {
      buffer[0] *= frameTotalVolume;
      buffer[1] *= frameTotalVolume;
      frameTotalVolume += frameTotalVolumeDelta;
    }
  }
  go_advance_legacy_shadow_state(
    nFrames,
    m_InActive,
    m_InPos,
    m_InLen,
    m_IncreasingDeltaPerFrame,
    m_OutActive,
    m_OutPos,
    m_OutLen,
    m_DecreasingDeltaPerFrame,
    m_LastTargetVolumePoint);
}

// non-linear fade processing

/* ---- Sustain-hot-loop accumulate kernels (2026-07-12) -------------------
 * The per-voice fade+accumulate loops compiled to pure SCALAR code
 * (mulss/addss - the in/out pointers carry no aliasing guarantee, so the
 * auto-vectorizer must assume overlap). At high polyphony every rendered
 * sample of every voice runs through here, making this one of the largest
 * remaining per-voice costs after the render/decode work. Explicit AVX2
 * with a runtime gate; SEPARATE mul+add (fp-contract off, same pattern as
 * GO_RESAMPLE_BATCH_FN) keeps every element bit-identical to the scalar
 * mulss+addss sequence. Only the STEADY loops are vectorized - active
 * envelope loops are short-lived and the template path carries a serial
 * sanitize_vol(vol, lastVol) dependency.
 *
 * x86/x86_64 only: immintrin.h and the AVX2 intrinsics it declares are not
 * portable (found on macOS/arm64 via a CI cross-build, 2026-10-25). The
 * non-x86 branch below keeps the exact same scalar tail loops that the
 * AVX2 versions already fall back to for their remainder elements, so
 * results stay bit-identical, just without the vectorized prefix. */
#if defined(__x86_64__) || defined(__i386__)
#ifndef __clang__
#define GO_FADER_AVX2_FN                                                       \
  __attribute__((target("avx2"), optimize("fp-contract=off")))
#else
#define GO_FADER_AVX2_FN __attribute__((target("avx2")))
#endif

#include <immintrin.h>

namespace {

inline bool faderAvx2Supported() {
  static const bool has = __builtin_cpu_supports("avx2");
  return has;
}

// dst[i] += src[i] * g over nFloats interleaved-stereo floats.
GO_FADER_AVX2_FN void accumConstAvx2(
  const float *src, float *dst, unsigned nFloats, float g) {
  const __m256 gv = _mm256_set1_ps(g);
  unsigned i = 0;

  for (; i + 8 <= nFloats; i += 8) {
    const __m256 sv = _mm256_loadu_ps(src + i);
    const __m256 dv = _mm256_loadu_ps(dst + i);

    _mm256_storeu_ps(dst + i, _mm256_add_ps(dv, _mm256_mul_ps(sv, gv)));
  }
  for (; i < nFloats; i++)
    dst[i] += src[i] * g;
}

/* Closed-form frame ramp: frame i gets vol_i = target * (startExt +
 * (float)i * dExt), both stereo samples share vol_i. The vector path
 * performs the SAME per-element operation sequence (cvt, mul, add, mul),
 * so every element is bit-identical to the scalar loop. */
GO_FADER_AVX2_FN void accumRampClosedAvx2(
  const float *src,
  float *dst,
  unsigned nFrames,
  float target,
  float startExt,
  float dExt) {
  const __m256 targetV = _mm256_set1_ps(target);
  const __m256 startV = _mm256_set1_ps(startExt);
  const __m256 dExtV = _mm256_set1_ps(dExt);
  // Frame indices of the 8 floats {i,i,i+1,i+1,i+2,i+2,i+3,i+3}.
  __m256i idx = _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3);
  const __m256i step = _mm256_set1_epi32(4);
  unsigned i = 0;

  for (; i + 4 <= nFrames; i += 4) {
    const __m256 ext = _mm256_add_ps(
      startV, _mm256_mul_ps(_mm256_cvtepi32_ps(idx), dExtV));
    const __m256 vol = _mm256_mul_ps(targetV, ext);
    const __m256 sv = _mm256_loadu_ps(src + (size_t)i * 2);
    const __m256 dv = _mm256_loadu_ps(dst + (size_t)i * 2);

    _mm256_storeu_ps(
      dst + (size_t)i * 2, _mm256_add_ps(dv, _mm256_mul_ps(sv, vol)));
    idx = _mm256_add_epi32(idx, step);
  }
  for (; i < nFrames; i++) {
    const float vol = target * (startExt + (float)i * dExt);

    dst[(size_t)i * 2] += src[(size_t)i * 2] * vol;
    dst[(size_t)i * 2 + 1] += src[(size_t)i * 2 + 1] * vol;
  }
}

} // namespace
#else // !(x86/x86_64) - portable scalar fallback, no vectorized prefix.
namespace {

inline bool faderAvx2Supported() { return false; }

void accumConstAvx2(const float *src, float *dst, unsigned nFloats, float g) {
  for (unsigned i = 0; i < nFloats; i++)
    dst[i] += src[i] * g;
}

void accumRampClosedAvx2(
  const float *src,
  float *dst,
  unsigned nFrames,
  float target,
  float startExt,
  float dExt) {
  for (unsigned i = 0; i < nFrames; i++) {
    const float vol = target * (startExt + (float)i * dExt);

    dst[(size_t)i * 2] += src[(size_t)i * 2] * vol;
    dst[(size_t)i * 2 + 1] += src[(size_t)i * 2 + 1] * vol;
  }
}

} // namespace
#endif // defined(__x86_64__) || defined(__i386__)

GOFadeKernelKind GOSoundFader::ComputeFrameGains(
  unsigned nFrames, float externalVolume, float &outG0, float &outDg) {
  // ==== identical header logic as in Process(...) ====
  float startTargetVolumePoint = m_LastTargetVolumePoint;
  unsigned framesLeftAfterIncreasing = nFrames;

  // increasing section
  if (m_IncreasingDeltaPerFrame > 0.0f) {
    float newTargetVolumePoint
      = m_LastTargetVolumePoint + m_IncreasingDeltaPerFrame * nFrames;

    if (newTargetVolumePoint < m_TargetVolume) {
      framesLeftAfterIncreasing = 0;
      m_LastTargetVolumePoint = newTargetVolumePoint;
    } else {
      framesLeftAfterIncreasing = nFrames
        - unsigned((m_TargetVolume - m_LastTargetVolumePoint)
                   / m_IncreasingDeltaPerFrame);
      m_IncreasingDeltaPerFrame = 0.0f;
      m_LastTargetVolumePoint = m_TargetVolume;
    }
  }

  // decreasing section
  if (framesLeftAfterIncreasing && m_DecreasingDeltaPerFrame > 0.0f) {
    float newTargetVolumePoint = m_LastTargetVolumePoint
      - m_DecreasingDeltaPerFrame * framesLeftAfterIncreasing;
    if (newTargetVolumePoint > 0.0f) {
      m_LastTargetVolumePoint = newTargetVolumePoint;
    } else {
      m_DecreasingDeltaPerFrame = 0.0f;
      m_LastTargetVolumePoint = 0.0f;
    }
  }

  // External volume
  float targetExternalVolume = m_VelocityVolume * externalVolume;
  if (m_LastExternalVolumePoint < 0.0f)
    m_LastExternalVolumePoint = targetExternalVolume;

  float startExternalVolumePoint = m_LastExternalVolumePoint;
  if (targetExternalVolume != startExternalVolumePoint)
    m_LastExternalVolumePoint = go_step_external_volume(
      startExternalVolumePoint, targetExternalVolume, nFrames);

  float frameTotalVolume = startTargetVolumePoint * startExternalVolumePoint;
  GOFadeKernelKind kind;

  outG0 = frameTotalVolume;
  if (
    (m_LastTargetVolumePoint == startTargetVolumePoint)
    && (m_LastExternalVolumePoint == startExternalVolumePoint)) {
    kind = GOFadeKernelKind::Constant;
    outDg = 0.0f;
  } else {
    // Ramp from frameTotalVolume -> m_LastTargetVolumePoint *
    // m_LastExternalVolumePoint
    kind = GOFadeKernelKind::Ramp;

    const float g1 = m_LastTargetVolumePoint * m_LastExternalVolumePoint;

    outDg = (g1 - frameTotalVolume) / nFrames;
  }
  go_advance_legacy_shadow_state(
    nFrames,
    m_InActive,
    m_InPos,
    m_InLen,
    m_IncreasingDeltaPerFrame,
    m_OutActive,
    m_OutPos,
    m_OutLen,
    m_DecreasingDeltaPerFrame,
    m_LastTargetVolumePoint);
  return kind;
}

bool GOSoundFader::FillFrameGains(
  unsigned nFrames, float externalVolume, float *dst, unsigned stride) {
  if (nFrames == 0)
    return true;

  using namespace GOAudioParams;

  /* Mode-change preamble identical to ProcessNonLinearFadeAndAccumulate:
   * re-init steppers and re-bind templates. Mutating these on a bail-out
   * is harmless - the scalar path would perform the exact same rebinding
   * on its next call. */
  const auto currentMode = GetCrossfadeMode();

  if (currentMode != m_ModeCached) {
    m_ModeCached = currentMode;
    if (m_InLen > 0) {
      m_InLin.init(m_InLen, m_InPos);
      m_InX2.init(m_InLen, m_InPos);
      m_InSin.init(m_InLen, m_InPos);
      m_InSin2.init(m_InLen, m_InPos);
      m_InSqrt.init(m_InLen, m_InPos);
    }
    if (m_OutLen > 0) {
      m_OutLin.init(m_OutLen, m_OutPos);
      m_OutX2.init(m_OutLen, m_OutPos);
      m_OutSin.init(m_OutLen, m_OutPos);
      m_OutSin2.init(m_OutLen, m_OutPos);
      m_OutSqrt.init(m_OutLen, m_OutPos);
    }
#ifdef GO_XFADE_USE_TEMPLATES
    if (m_InLen > 0) {
      GOAudioParams::FastCrossfadeCache::EnsureTemplate(m_ModeCached, m_InLen);
      m_TplIn
        = GOAudioParams::FastCrossfadeCache::GetTemplate(m_ModeCached, m_InLen);
    } else {
      m_TplIn.reset();
    }
    if (m_OutLen > 0) {
      GOAudioParams::FastCrossfadeCache::EnsureTemplate(m_ModeCached, m_OutLen);
      m_TplOut = FastCrossfadeCache::GetTemplate(m_ModeCached, m_OutLen);
    } else {
      m_TplOut.reset();
    }
#endif
  }

  const auto mode = m_ModeCached;
  const bool in_on = (m_InActive && m_InLen > 0);
  const bool out_on = (m_OutActive && m_OutLen > 0);

#ifdef GO_XFADE_USE_TEMPLATES
  if (in_on && (!m_TplIn || m_TplIn->len != m_InLen)) {
    GOAudioParams::FastCrossfadeCache::EnsureTemplate(mode, m_InLen);
    m_TplIn = GOAudioParams::FastCrossfadeCache::GetTemplate(mode, m_InLen);
  }
  if (out_on && (!m_TplOut || m_TplOut->len != m_OutLen)) {
    GOAudioParams::FastCrossfadeCache::EnsureTemplate(mode, m_OutLen);
    m_TplOut = GOAudioParams::FastCrossfadeCache::GetTemplate(mode, m_OutLen);
  }

  const bool tplPath = (in_on && m_TplIn) || (out_on && m_TplOut);
#else
  const bool tplPath = false;
#endif

  if ((in_on || out_on) && !tplPath)
    // An active envelope without a bound template would need the stepper
    // loops - leave those to the classic scalar path, untouched state.
    return false;

  // External-envelope endpoints, matching the scalar path's arithmetic
  // exactly; state is only committed at the end.
  const float targetExt = m_VelocityVolume * externalVolume;
  const float startExt
    = m_LastExternalVolumePoint < 0.0f ? targetExt : m_LastExternalVolumePoint;
  float endExt = startExt;

  if (targetExt != startExt)
    endExt = go_step_external_volume(startExt, targetExt, nFrames);
  const float dExt = (endExt - startExt) / nFrames;

  if (tplPath) {
    // Same per-frame gain math as the scalar template loop (incl. the
    // missing-template 1.0 fallbacks and sanitize_vol), minus the
    // accumulation.
    const unsigned inLen = (in_on && m_TplIn) ? m_TplIn->len : 0;
    const unsigned outLen = (out_on && m_TplOut) ? m_TplOut->len : 0;
    float lastVol = 0.0f;

    for (unsigned i = 0; i < nFrames; ++i) {
      const float extNow = startExt + i * dExt;
      const float base = m_TargetVolume * extNow;
      float bi = 0.0f, ao = 1.0f;

      if (in_on) {
        if (m_TplIn) {
          const unsigned idx = std::min(m_InPos + i, inLen ? inLen - 1 : 0);

          bi = m_TplIn->b[idx];
        } else
          bi = 1.0f;
      }
      if (out_on) {
        if (m_TplOut) {
          const unsigned idx = std::min(m_OutPos + i, outLen ? outLen - 1 : 0);

          ao = m_TplOut->a[idx];
        } else
          ao = 1.0f;
      }

      float vol = base * ((in_on ? bi : 1.0f) * (out_on ? ao : 1.0f));

      vol = sanitize_vol(vol, lastVol);
      dst[(size_t)i * stride] = vol;
      lastVol = vol;
    }
  } else if (endExt == startExt) {
    // neither In nor Out active: constant external volume
    const float base0 = m_TargetVolume * startExt;

    for (unsigned i = 0; i < nFrames; ++i)
      dst[(size_t)i * stride] = base0;
  } else {
    // neither In nor Out active: external volume ramp
    for (unsigned i = 0; i < nFrames; ++i)
      dst[(size_t)i * stride] = m_TargetVolume * (startExt + i * dExt);
  }

  // State advance - identical to ProcessNonLinearFadeAndAccumulate.
  if (in_on) {
    m_InPos += nFrames;
    if (m_InPos >= m_InLen)
      m_InActive = false;
  }
  if (out_on) {
    m_OutPos += nFrames;
    if (m_OutPos >= m_OutLen)
      m_OutActive = false;
  }
  m_LastExternalVolumePoint = endExt;
  m_LastTargetVolumePoint = go_compute_target_state_after_block(
    mode,
    m_TargetVolume,
    in_on,
    m_InActive,
    m_InLen,
    m_InPos,
    out_on,
    m_OutActive,
    m_OutLen,
    m_OutPos);
  return true;
}

bool GOSoundFader::TrySteadyLinearGain(
  float externalVolume, unsigned nFrames, float &outGain0, float &outDGain) {
  using namespace GOAudioParams;

  if (m_ModeOverrideLinear || GetCrossfadeMode() == GOCrossfadeMode::Linear)
    return false;
  if ((m_InActive && m_InLen > 0) || (m_OutActive && m_OutLen > 0))
    return false;

  const float targetExt = m_VelocityVolume * externalVolume;

  /* Same first-touch initialization the full path performs - idempotent
   * with it. From here on the block is committed: with no active
   * envelope the gain is exactly linear (the sustain branch of
   * ProcessNonLinearFadeAndAccumulate) - constant when the external
   * volume has settled, one smoothing step toward it otherwise. */
  if (m_LastExternalVolumePoint < 0.0f)
    m_LastExternalVolumePoint = targetExt;

  const float startExt = m_LastExternalVolumePoint;

  if (targetExt != startExt)
    m_LastExternalVolumePoint
      = go_step_external_volume(startExt, targetExt, nFrames);

  const float endExt = m_LastExternalVolumePoint;

  /* Commit exactly the state the full path's sustain branch would
   * leave behind. Stepper/template rebinds are skipped - they only
   * matter for active envelopes, and every envelope start re-seeds its
   * stepper at block start anyway. */
  const auto mode = GetCrossfadeMode();

  m_ModeCached = mode;
  m_LastTargetVolumePoint = go_compute_target_state_after_block(
    mode,
    m_TargetVolume,
    false,
    m_InActive,
    m_InLen,
    m_InPos,
    false,
    m_OutActive,
    m_OutLen,
    m_OutPos);
  outGain0 = m_TargetVolume * startExt;
  outDGain = endExt == startExt
    ? 0.0f
    : m_TargetVolume * ((endExt - startExt) / nFrames);
  return true;
}

GOFadeKernelKind GOSoundFader::ProcessAndAccumulate(
  unsigned nFrames, const float *in, float *out, float externalVolume) {
  // Non-linear mode: delegate to non-linear accumulator - unless this fader
  // was pinned to the legacy linear path (see m_ModeOverrideLinear above).
  if (
    !m_ModeOverrideLinear
    && GOAudioParams::GetCrossfadeMode() != GOCrossfadeMode::Linear)
    return ProcessNonLinearFadeAndAccumulate(nFrames, in, out, externalVolume);

  float g;
  float dg;
  const GOFadeKernelKind kind = ComputeFrameGains(nFrames, externalVolume, g, dg);
  const float *src = in;
  float *dst = out;

  if (kind == GOFadeKernelKind::Constant) {
    if (faderAvx2Supported())
      accumConstAvx2(src, dst, nFrames * 2, g);
    else
      for (unsigned i = 0; i < nFrames; ++i) {
        dst[0] += src[0] * g;
        dst[1] += src[1] * g;
        src += 2;
        dst += 2;
      }
  } else {
    for (unsigned i = 0; i < nFrames; ++i) {
      dst[0] += src[0] * g;
      dst[1] += src[1] * g;
      src += 2;
      dst += 2;
      g += dg;
    }
  }
  return kind;
}

GOFadeKernelKind GOSoundFader::ProcessNonLinearFadeAndAccumulate(
  unsigned nFrames, const float *in, float *out, float externalVolume) {
  // Mirrors the logic of ProcessNonLinearFade(...), but instead of applying
  // the gain envelope in-place, accumulates into 'out'.
  if (nFrames == 0)
    return GOFadeKernelKind::Constant;

  // update external envelope smoothly
  float targetExt = m_VelocityVolume * externalVolume;
  if (m_LastExternalVolumePoint < 0.0f)
    m_LastExternalVolumePoint = targetExt;
  float startExt = m_LastExternalVolumePoint;
  if (targetExt != startExt)
    m_LastExternalVolumePoint
      = go_step_external_volume(startExt, targetExt, nFrames);
  float endExt = m_LastExternalVolumePoint;
  const float dExt = (endExt - startExt) / nFrames;

  using namespace GOAudioParams;

  // detect mode change and (re-)init steppers if necessary
  const auto currentMode = GetCrossfadeMode();
  if (currentMode != m_ModeCached) {
    m_ModeCached = currentMode;
    if (m_InLen > 0) {
      m_InLin.init(m_InLen, m_InPos);
      m_InX2.init(m_InLen, m_InPos);
      m_InSin.init(m_InLen, m_InPos);
      m_InSin2.init(m_InLen, m_InPos);
      m_InSqrt.init(m_InLen, m_InPos);
    }
    if (m_OutLen > 0) {
      m_OutLin.init(m_OutLen, m_OutPos);
      m_OutX2.init(m_OutLen, m_OutPos);
      m_OutSin.init(m_OutLen, m_OutPos);
      m_OutSin2.init(m_OutLen, m_OutPos);
      m_OutSqrt.init(m_OutLen, m_OutPos);
    }
#ifdef GO_XFADE_USE_TEMPLATES
    // (Re)bind templates when mode changes
    if (m_InLen > 0) {
      GOAudioParams::FastCrossfadeCache::EnsureTemplate(m_ModeCached, m_InLen);
      m_TplIn
        = GOAudioParams::FastCrossfadeCache::GetTemplate(m_ModeCached, m_InLen);
    } else {
      m_TplIn.reset();
    }
    if (m_OutLen > 0) {
      GOAudioParams::FastCrossfadeCache::EnsureTemplate(m_ModeCached, m_OutLen);
      m_TplOut = GOAudioParams::FastCrossfadeCache::GetTemplate(
        m_ModeCached, m_OutLen);
    } else {
      m_TplOut.reset();
    }
#endif
  }

  const auto mode = m_ModeCached;
  const bool in_on = (m_InActive && m_InLen > 0);
  const bool out_on = (m_OutActive && m_OutLen > 0);

#ifdef GO_XFADE_USE_TEMPLATES
  // Ensure templates also bound if lengths changed mid-stream
  if (in_on && (!m_TplIn || m_TplIn->len != m_InLen)) {
    GOAudioParams::FastCrossfadeCache::EnsureTemplate(mode, m_InLen);
    m_TplIn = GOAudioParams::FastCrossfadeCache::GetTemplate(mode, m_InLen);
  }
  if (out_on && (!m_TplOut || m_TplOut->len != m_OutLen)) {
    GOAudioParams::FastCrossfadeCache::EnsureTemplate(mode, m_OutLen);
    m_TplOut = GOAudioParams::FastCrossfadeCache::GetTemplate(mode, m_OutLen);
  }
#endif

  // Re-seed steppers at block start using absolute positions to avoid
  // drift/misalignment. This aligns the stepper state to the same sample index
  // mapping as the reference evaluator.
  switch (mode) {
  case GOCrossfadeMode::Linear:
    if (in_on)
      m_InLin.init(m_InLen, m_InPos);
    if (out_on)
      m_OutLin.init(m_OutLen, m_OutPos);
    break;
  case GOCrossfadeMode::SinEqualPower:
    if (in_on)
      m_InSin.init(m_InLen, m_InPos);
    if (out_on)
      m_OutSin.init(m_OutLen, m_OutPos);
    break;
  case GOCrossfadeMode::Sin2:
    if (in_on)
      m_InSin2.init(m_InLen, m_InPos);
    if (out_on)
      m_OutSin2.init(m_OutLen, m_OutPos);
    break;
  case GOCrossfadeMode::SqrtEqualPower:
    if (in_on)
      m_InSqrt.init(m_InLen, m_InPos);
    if (out_on)
      m_OutSqrt.init(m_OutLen, m_OutPos);
    break;
  case GOCrossfadeMode::X2:
    if (in_on)
      m_InX2.init(m_InLen, m_InPos);
    if (out_on)
      m_OutX2.init(m_OutLen, m_OutPos);
    break;
  case GOCrossfadeMode::Custom:
    // Custom mode: no generic stepper. Use templates (if present) or the
    // reference evaluator in the hot loop instead of the linear stepper.
    break;
  default:
    if (in_on)
      m_InLin.init(m_InLen, m_InPos);
    if (out_on)
      m_OutLin.init(m_OutLen, m_OutPos);
    break;
  }

  float lastVol = 0.0f;
  const float *src = in;
  float *dst = out;

#ifdef GO_XFADE_USE_TEMPLATES
  // Template-driven exact evaluation (fast, branch-minimal)
  if ((in_on && m_TplIn) || (out_on && m_TplOut)) {
    float lastVol = 0.0f;
    const unsigned inLen = in_on && m_TplIn ? m_TplIn->len : 0;
    const unsigned outLen = out_on && m_TplOut ? m_TplOut->len : 0;

#ifdef GO_XFADE_VERIFY_REF
    bool useRef = false;
    {
      const unsigned preN = std::min<unsigned>(nFrames, 64u);
      float maxDiffIn = 0.f, maxDiffOut = 0.f;
      for (unsigned i = 0; i < preN; ++i) {
        if (in_on && m_TplIn && inLen > 0) {
          const unsigned idxIn = std::min(m_InPos + i, inLen - 1);
          const float tIn
            = (inLen > 1) ? float(idxIn) / float(inLen - 1) : 1.0f;
          const auto gref = go_crossfade_eval(mode, tIn);
          const float diff = std::fabs(m_TplIn->b[idxIn] - gref.b);
          if (diff > maxDiffIn)
            maxDiffIn = diff;
        }
        if (out_on && m_TplOut && outLen > 0) {
          const unsigned idxOut = std::min(m_OutPos + i, outLen - 1);
          const float tOut
            = (outLen > 1) ? float(idxOut) / float(outLen - 1) : 1.0f;
          const auto gref = go_crossfade_eval(mode, tOut);
          const float diff = std::fabs(m_TplOut->a[idxOut] - gref.a);
          if (diff > maxDiffOut)
            maxDiffOut = diff;
        }
      }
      constexpr float kEps = 1e-6f;
      if (maxDiffIn > kEps || maxDiffOut > kEps) {
        useRef = true; // auto-fallback to exact reference for this block
      }
    }
#else
    bool useRef = false;
#endif

    for (unsigned i = 0; i < nFrames; ++i) {
      const float extNow = startExt + i * dExt;
      const float base = m_TargetVolume * extNow;

      float bi = 0.0f, ao = 1.0f;
      if (in_on) {
        if (useRef) {
          if (m_InLen > 1) {
            const float t = float(m_InPos + i) / float(m_InLen - 1);
            bi = go_crossfade_eval(mode, t).b;
          } else {
            bi = 1.0f;
          }
        } else if (m_TplIn) {
          const unsigned idx = std::min(m_InPos + i, inLen ? inLen - 1 : 0);
          bi = m_TplIn->b[idx];
        } else {
          bi = 1.0f;
        }
      }
      if (out_on) {
        if (useRef) {
          if (m_OutLen > 1) {
            const float t = float(m_OutPos + i) / float(m_OutLen - 1);
            ao = go_crossfade_eval(mode, t).a;
          } else {
            ao = 0.0f;
          }
        } else if (m_TplOut) {
          const unsigned idx = std::min(m_OutPos + i, outLen ? outLen - 1 : 0);
          ao = m_TplOut->a[idx];
        } else {
          ao = 1.0f;
        }
      }

      float vol = base * ((in_on ? bi : 1.0f) * (out_on ? ao : 1.0f));
      vol = sanitize_vol(vol, lastVol);
      dst[0] += src[0] * vol;
      dst[1] += src[1] * vol;
      src += 2;
      dst += 2;
      lastVol = vol;
    }

    if (in_on) {
      m_InPos += nFrames;
      if (m_InPos >= m_InLen)
        m_InActive = false;
    }
    if (out_on) {
      m_OutPos += nFrames;
      if (m_OutPos >= m_OutLen)
        m_OutActive = false;
    }
    m_LastExternalVolumePoint = endExt;
    m_LastTargetVolumePoint = go_compute_target_state_after_block(
      mode,
      m_TargetVolume,
      in_on,
      m_InActive,
      m_InLen,
      m_InPos,
      out_on,
      m_OutActive,
      m_OutLen,
      m_OutPos);
    return GOFadeKernelKind::NonLinearFade;
  }
#endif

  if (in_on && out_on) {
    switch (mode) {
    case GOCrossfadeMode::Linear: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai, bi, ao, bo;
        m_InLin.next(ai, bi);
        m_OutLin.next(ao, bo);
        float vol = base * (bi * ao);
        vol = sanitize_vol(vol, lastVol);
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::SinEqualPower: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai, bi, ao, bo;
        m_InSin.next(ai, bi);
        m_OutSin.next(ao, bo);
        float vol = base * (bi * ao);
        vol = sanitize_vol(vol, lastVol);
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::Sin2: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai, bi, ao, bo;
        m_InSin2.next(ai, bi);
        m_OutSin2.next(ao, bo);
        const float vol = base * (bi * ao);
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::SqrtEqualPower: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai, bi, ao, bo;
        m_InSqrt.next(ai, bi);
        m_OutSqrt.next(ao, bo);
        const float vol = base * (bi * ao);
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::X2: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai, bi, ao, bo;
        m_InX2.next(ai, bi);
        m_OutX2.next(ao, bo);
        const float vol = base * (bi * ao);
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::Custom: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai = 1.f, bi = 0.f, ao = 1.f, bo = 0.f;
        if (in_on) {
          if (m_TplIn && m_TplIn->len > 0) {
            const unsigned idx = std::min(m_InPos + i, m_TplIn->len - 1);
            ai = m_TplIn->a[idx];
            bi = m_TplIn->b[idx];
          } else if (m_InLen > 1) {
            const float t = float(m_InPos + i) / float(m_InLen - 1);
            const auto g = go_crossfade_eval(mode, t);
            ai = g.a;
            bi = g.b;
          } else {
            ai = 1.f;
            bi = 0.f;
          }
        }
        if (out_on) {
          if (m_TplOut && m_TplOut->len > 0) {
            const unsigned idx = std::min(m_OutPos + i, m_TplOut->len - 1);
            ao = m_TplOut->a[idx];
            bo = m_TplOut->b[idx];
          } else if (m_OutLen > 1) {
            const float t = float(m_OutPos + i) / float(m_OutLen - 1);
            const auto g = go_crossfade_eval(mode, t);
            ao = g.a;
            bo = g.b;
          } else {
            ao = 1.f;
            bo = 0.f;
          }
        }
        float vol = base * ((in_on ? bi : 1.0f) * (out_on ? ao : 1.0f));
        vol = sanitize_vol(vol, lastVol);
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    default: { // fallback to linear
      for (unsigned i = 0; i < nFrames; ++i) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai, bi, ao, bo;
        m_InLin.next(ai, bi);
        m_OutLin.next(ao, bo);
        const float vol = base * (bi * ao);
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    }
  } else if (in_on && !out_on) {
    switch (mode) {
    case GOCrossfadeMode::Linear: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_InLin.next(a, b);
        float vol = base * b;
        vol = sanitize_vol(vol, lastVol);
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::SinEqualPower: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_InSin.next(a, b);
        float vol = base * b;
        vol = sanitize_vol(vol, lastVol);
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::Sin2: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_InSin2.next(a, b);
        const float vol = base * b;
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::SqrtEqualPower: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_InSqrt.next(a, b);
        const float vol = base * b;
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::X2: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_InX2.next(a, b);
        const float vol = base * b;
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::Custom: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a = 1.f, b = 0.f;
        if (m_TplIn && m_TplIn->len > 0) {
          const unsigned idx = std::min(m_InPos + i, m_TplIn->len - 1);
          a = m_TplIn->a[idx];
          b = m_TplIn->b[idx];
        } else if (m_InLen > 1) {
          const float t = float(m_InPos + i) / float(m_InLen - 1);
          const auto g = go_crossfade_eval(mode, t);
          a = g.a;
          b = g.b;
        } else {
          a = 1.f;
          b = 0.f;
        }
        const float vol = base * b;
        const float vol_s = sanitize_vol(vol, lastVol);
        dst[0] += src[0] * vol_s;
        dst[1] += src[1] * vol_s;
        src += 2;
        dst += 2;
        lastVol = vol_s;
      }
      break;
    }

    default: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_InLin.next(a, b);
        const float vol = base * b;
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    }
  } else if (!in_on && out_on) {
    switch (mode) {
    case GOCrossfadeMode::Linear: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_OutLin.next(a, b);
        float vol = base * a;
        vol = sanitize_vol(vol, lastVol);
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::SinEqualPower: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_OutSin.next(a, b);
        float vol = base * a;
        vol = sanitize_vol(vol, lastVol);
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::Sin2: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_OutSin2.next(a, b);
        const float vol = base * a;
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::SqrtEqualPower: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_OutSqrt.next(a, b);
        const float vol = base * a;
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::X2: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_OutX2.next(a, b);
        const float vol = base * a;
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::Custom: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a = 1.f, b = 0.f;
        if (m_TplOut && m_TplOut->len > 0) {
          const unsigned idx = std::min(m_OutPos + i, m_TplOut->len - 1);
          a = m_TplOut->a[idx];
          b = m_TplOut->b[idx];
        } else if (m_OutLen > 1) {
          const float t = float(m_OutPos + i) / float(m_OutLen - 1);
          const auto g = go_crossfade_eval(mode, t);
          a = g.a;
          b = g.b;
        } else {
          a = 1.f;
          b = 0.f;
        }
        const float vol = base * a;
        const float vol_s = sanitize_vol(vol, lastVol);
        dst[0] += src[0] * vol_s;
        dst[1] += src[1] * vol_s;
        src += 2;
        dst += 2;
        lastVol = vol_s;
      }
      break;
    }
    default: {
      for (unsigned i = 0; i < nFrames; ++i) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_OutLin.next(a, b);
        const float vol = base * a;
        dst[0] += src[0] * vol;
        dst[1] += src[1] * vol;
        src += 2;
        dst += 2;
        lastVol = vol;
      }
      break;
    }
    }
  } else {
    // neither In nor Out active: only external volume ramp applies -
    // the sustain hot path of every voice outside its envelopes. AVX2
    // with bit-identical per-element semantics (see the kernels above).
    const float base0 = m_TargetVolume * startExt;
    if (endExt == startExt) {
      if (faderAvx2Supported())
        accumConstAvx2(src, dst, nFrames * 2, base0);
      else
        for (unsigned i = 0; i < nFrames; ++i) {
          dst[0] += src[0] * base0;
          dst[1] += src[1] * base0;
          src += 2;
          dst += 2;
        }
      lastVol = base0;
    } else {
      if (faderAvx2Supported())
        accumRampClosedAvx2(
          src, dst, nFrames, m_TargetVolume, startExt, dExt);
      else
        for (unsigned i = 0; i < nFrames; ++i) {
          const float base = m_TargetVolume * (startExt + i * dExt);
          dst[0] += src[0] * base;
          dst[1] += src[1] * base;
          src += 2;
          dst += 2;
        }
      lastVol = m_TargetVolume * (startExt + (nFrames - 1) * dExt);
    }
  }

  // advance positions (one-shot bulk update)
  if (in_on) {
    m_InPos += nFrames;
    if (m_InPos >= m_InLen)
      m_InActive = false;
  }
  if (out_on) {
    m_OutPos += nFrames;
    if (m_OutPos >= m_OutLen)
      m_OutActive = false;
  }

  m_LastExternalVolumePoint = endExt;
  m_LastTargetVolumePoint = go_compute_target_state_after_block(
    mode,
    m_TargetVolume,
    in_on,
    m_InActive,
    m_InLen,
    m_InPos,
    out_on,
    m_OutActive,
    m_OutLen,
    m_OutPos);
  // Stepper paths above only run while a fade is active; the final else
  // branch (neither In nor Out) is a plain constant/ramp loop.
  return (in_on || out_on) ? GOFadeKernelKind::NonLinearFade
    : (endExt == startExt ? GOFadeKernelKind::Constant
                          : GOFadeKernelKind::Ramp);
}

void GOSoundFader::ProcessNonLinearFade(
  unsigned n, float *buf, float external) {

  if (n == 0)
    return;

  // update external envelope smoothly
  float targetExt = m_VelocityVolume * external;
  if (m_LastExternalVolumePoint < 0.0f)
    m_LastExternalVolumePoint = targetExt;
  float startExt = m_LastExternalVolumePoint;
  if (targetExt != startExt)
    m_LastExternalVolumePoint
      = go_step_external_volume(startExt, targetExt, n);
  float endExt = m_LastExternalVolumePoint;
  const float dExt = (endExt - startExt) / n;

  using namespace GOAudioParams;

  // detect mode change and (re-)init steppers if necessary
  const auto currentMode = GetCrossfadeMode();
  if (currentMode != m_ModeCached) {
    m_ModeCached = currentMode;
    if (m_InLen > 0) {
      m_InLin.init(m_InLen, m_InPos);
      m_InX2.init(m_InLen, m_InPos);
      m_InSin.init(m_InLen, m_InPos);
      m_InSin2.init(m_InLen, m_InPos);
      m_InSqrt.init(m_InLen, m_InPos);
    }
    if (m_OutLen > 0) {
      m_OutLin.init(m_OutLen, m_OutPos);
      m_OutX2.init(m_OutLen, m_OutPos);
      m_OutSin.init(m_OutLen, m_OutPos);
      m_OutSin2.init(m_OutLen, m_OutPos);
      m_OutSqrt.init(m_OutLen, m_OutPos);
    }
#ifdef GO_XFADE_USE_TEMPLATES
    // (Re)bind templates when mode changes
    if (m_InLen > 0) {
      GOAudioParams::FastCrossfadeCache::EnsureTemplate(m_ModeCached, m_InLen);
      m_TplIn
        = GOAudioParams::FastCrossfadeCache::GetTemplate(m_ModeCached, m_InLen);
    } else {
      m_TplIn.reset();
    }
    if (m_OutLen > 0) {
      GOAudioParams::FastCrossfadeCache::EnsureTemplate(m_ModeCached, m_OutLen);
      m_TplOut = GOAudioParams::FastCrossfadeCache::GetTemplate(
        m_ModeCached, m_OutLen);
    } else {
      m_TplOut.reset();
    }
#endif
  }

  const auto mode = m_ModeCached;
  const bool in_on = (m_InActive && m_InLen > 0);
  const bool out_on = (m_OutActive && m_OutLen > 0);

#ifdef GO_XFADE_USE_TEMPLATES
  // Ensure templates also bound if lengths changed mid-stream
  if (in_on && (!m_TplIn || m_TplIn->len != m_InLen)) {
    GOAudioParams::FastCrossfadeCache::EnsureTemplate(mode, m_InLen);
    m_TplIn = GOAudioParams::FastCrossfadeCache::GetTemplate(mode, m_InLen);
  }
  if (out_on && (!m_TplOut || m_TplOut->len != m_OutLen)) {
    GOAudioParams::FastCrossfadeCache::EnsureTemplate(mode, m_OutLen);
    m_TplOut = GOAudioParams::FastCrossfadeCache::GetTemplate(mode, m_OutLen);
  }
#endif

  // Re-seed steppers at block start using absolute positions to avoid
  // drift/misalignment. This aligns the stepper state to the same sample index
  // mapping as the reference evaluator.
  switch (mode) {
  case GOCrossfadeMode::Linear:
    if (in_on)
      m_InLin.init(m_InLen, m_InPos);
    if (out_on)
      m_OutLin.init(m_OutLen, m_OutPos);
    break;
  case GOCrossfadeMode::SinEqualPower:
    if (in_on)
      m_InSin.init(m_InLen, m_InPos);
    if (out_on)
      m_OutSin.init(m_OutLen, m_OutPos);
    break;
  case GOCrossfadeMode::Sin2:
    if (in_on)
      m_InSin2.init(m_InLen, m_InPos);
    if (out_on)
      m_OutSin2.init(m_OutLen, m_OutPos);
    break;
  case GOCrossfadeMode::SqrtEqualPower:
    if (in_on)
      m_InSqrt.init(m_InLen, m_InPos);
    if (out_on)
      m_OutSqrt.init(m_OutLen, m_OutPos);
    break;
  case GOCrossfadeMode::X2:
    if (in_on)
      m_InX2.init(m_InLen, m_InPos);
    if (out_on)
      m_OutX2.init(m_OutLen, m_OutPos);
    break;
  case GOCrossfadeMode::Custom:
    // Custom mode: no generic stepper. Use templates (if present) or the
    // reference evaluator in the hot loop instead of the linear stepper.
    break;
  default:
    if (in_on)
      m_InLin.init(m_InLen, m_InPos);
    if (out_on)
      m_OutLin.init(m_OutLen, m_OutPos);
    break;
  }

#ifdef GO_XFADE_RUNTIME_USE_REF
  // Reference path: evaluate gains via go_crossfade_eval using absolute
  // indices. This bypasses steppers to isolate mapping/stepper issues audibly.
  {
    auto eval_in = [&](unsigned i) -> GOCrossfadeGains {
      if (!in_on)
        return {1.0f, 0.0f};
      if (m_InLen <= 1)
        return {0.0f, 1.0f};
      const float t = float(m_InPos + i) / float(m_InLen - 1);
      return go_crossfade_eval(mode, t);
    };
    auto eval_out = [&](unsigned i) -> GOCrossfadeGains {
      if (!out_on)
        return {1.0f, 0.0f};
      if (m_OutLen <= 1)
        return {0.0f, 1.0f};
      const float t = float(m_OutPos + i) / float(m_OutLen - 1);
      return go_crossfade_eval(mode, t);
    };

    float lastVol = 0.0f;
    if (in_on && out_on) {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        const auto gi = eval_in(i);
        const auto go = eval_out(i);
        float vol = base * (gi.b * go.a);
        vol = sanitize_vol(vol, lastVol);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
    } else if (in_on && !out_on) {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        const auto gi = eval_in(i);
        float vol = base * gi.b;
        vol = sanitize_vol(vol, lastVol);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
    } else if (!in_on && out_on) {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        const auto go = eval_out(i);
        float vol = base * go.a;
        vol = sanitize_vol(vol, lastVol);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
    } else {
      // neither In nor Out active: only external volume ramp applies
      const float base0 = m_TargetVolume * startExt;
      if (endExt == startExt) {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          buf[0] *= base0;
          buf[1] *= base0;
        }
        lastVol = base0;
      } else {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          const float base = m_TargetVolume * (startExt + i * dExt);
          buf[0] *= base;
          buf[1] *= base;
          lastVol = base;
        }
      }
    }

    // advance positions (one-shot bulk update)
    if (in_on) {
      m_InPos += n;
      if (m_InPos >= m_InLen)
        m_InActive = false;
    }
    if (out_on) {
      m_OutPos += n;
      if (m_OutPos >= m_OutLen)
        m_OutActive = false;
    }

    m_LastExternalVolumePoint = endExt;
    m_LastTargetVolumePoint = go_compute_target_state_after_block(
      mode,
      m_TargetVolume,
      in_on,
      m_InActive,
      m_InLen,
      m_InPos,
      out_on,
      m_OutActive,
      m_OutLen,
      m_OutPos);
    return;
  }
#endif

#ifdef GO_FAST_XFADE_RUNTIME_DUMP
  // Emit a single short runtime trace for diagnosis (guarded by compile-time
  // macro).
  if (!m_RuntimeDumpDone && (in_on || out_on)) {
    const unsigned traceSamples = std::min<unsigned>(n, 256u);
    std::ostringstream fname_ss;
    fname_ss << "xfade_runtime_mode" << static_cast<int>(mode) << "_lenin"
             << m_InLen << "_lenout" << m_OutLen;

    const std::time_t now_c = std::time(nullptr);
    char timebuf[32];
    std::strftime(
      timebuf, sizeof(timebuf), "%Y%m%d-%H%M%S", std::localtime(&now_c));
    std::string fn = fname_ss.str() + "_" + timebuf + ".txt";

    std::ofstream out(fn);
    if (out) {
      out << "# sample ai bi ao bo vol\n";

      // local copies of steppers so we don't advance the real ones
      auto inLin = m_InLin;
      auto inX2 = m_InX2;
      auto inSin = m_InSin;
      auto inSin2 = m_InSin2;
      auto inSqrt = m_InSqrt;
      auto outLin = m_OutLin;
      auto outX2 = m_OutX2;
      auto outSin = m_OutSin;
      auto outSin2 = m_OutSin2;
      auto outSqrt = m_OutSqrt;

      for (unsigned i = 0; i < traceSamples; ++i) {
        float ai = 1.f, bi = 0.f, ao = 1.f, bo = 0.f;
        // select appropriate steppers based on mode
        switch (mode) {
        case GOCrossfadeMode::Linear:
          if (in_on)
            inLin.next(ai, bi);
          if (out_on)
            outLin.next(ao, bo);
          break;
        case GOCrossfadeMode::SinEqualPower:
          if (in_on)
            inSin.next(ai, bi);
          if (out_on)
            outSin.next(ao, bo);
          break;
        case GOCrossfadeMode::Sin2:
          if (in_on)
            inSin2.next(ai, bi);
          if (out_on)
            outSin2.next(ao, bo);
          break;
        case GOCrossfadeMode::SqrtEqualPower:
          if (in_on)
            inSqrt.next(ai, bi);
          if (out_on)
            outSqrt.next(ao, bo);
          break;
        case GOCrossfadeMode::X2:
          if (in_on)
            inX2.next(ai, bi);
          if (out_on)
            outX2.next(ao, bo);
          break;
        case GOCrossfadeMode::Custom:
          if (in_on) {
            if (m_InLen > 1) {
              const float t = float(m_InPos + i) / float(m_InLen - 1);
              const auto g = go_crossfade_eval(mode, t);
              ai = g.a;
              bi = g.b;
            } else {
              ai = 1.f;
              bi = 0.f;
            }
          }
          if (out_on) {
            if (m_OutLen > 1) {
              const float t = float(m_OutPos + i) / float(m_OutLen - 1);
              const auto g = go_crossfade_eval(mode, t);
              ao = g.a;
              bo = g.b;
            } else {
              ao = 1.f;
              bo = 0.f;
            }
          }
          break;
        default:
          if (in_on)
            inLin.next(ai, bi);
          if (out_on)
            outLin.next(ao, bo);
          break;
        }
        const float extNow = startExt + i * dExt;
        const float basev = m_TargetVolume * extNow;
        const float vol = basev * ((in_on ? bi : 1.0f) * (out_on ? ao : 1.0f));
        out << i << " " << std::setprecision(9) << ai << " " << bi << " " << ao
            << " " << bo << " " << vol << "\n";
      }
      out.close();
    }
    m_RuntimeDumpDone = true;
  }
#endif

  float lastVol = 0.0f;

  // Four branchless-style paths chosen once per block, then a hot loop per
  // path.
#ifdef GO_XFADE_USE_TEMPLATES
  // Template-driven exact evaluation (fast, branch-minimal)
  if ((in_on && m_TplIn) || (out_on && m_TplOut)) {
    float lastVol = 0.0f;
    const unsigned inLen = in_on && m_TplIn ? m_TplIn->len : 0;
    const unsigned outLen = out_on && m_TplOut ? m_TplOut->len : 0;

#ifdef GO_XFADE_VERIFY_REF
    bool useRef = false;
    {
      const unsigned preN = std::min<unsigned>(n, 64u);
      float maxDiffIn = 0.f, maxDiffOut = 0.f;
      for (unsigned i = 0; i < preN; ++i) {
        if (in_on && m_TplIn && inLen > 0) {
          const unsigned idxIn = std::min(m_InPos + i, inLen - 1);
          const float tIn
            = (inLen > 1) ? float(idxIn) / float(inLen - 1) : 1.0f;
          const auto gref = go_crossfade_eval(mode, tIn);
          const float diff = std::fabs(m_TplIn->b[idxIn] - gref.b);
          if (diff > maxDiffIn)
            maxDiffIn = diff;
        }
        if (out_on && m_TplOut && outLen > 0) {
          const unsigned idxOut = std::min(m_OutPos + i, outLen - 1);
          const float tOut
            = (outLen > 1) ? float(idxOut) / float(outLen - 1) : 1.0f;
          const auto gref = go_crossfade_eval(mode, tOut);
          const float diff = std::fabs(m_TplOut->a[idxOut] - gref.a);
          if (diff > maxDiffOut)
            maxDiffOut = diff;
        }
      }
      constexpr float kEps = 1e-6f;
      if (maxDiffIn > kEps || maxDiffOut > kEps) {
        useRef = true; // auto-fallback to exact reference for this block
      }
    }
#else
    bool useRef = false;
#endif

    for (unsigned i = 0; i < n; ++i, buf += 2) {
      const float extNow = startExt + i * dExt;
      const float base = m_TargetVolume * extNow;

      float bi = 0.0f, ao = 1.0f;
      if (in_on) {
        if (useRef) {
          if (m_InLen > 1) {
            const float t = float(m_InPos + i) / float(m_InLen - 1);
            bi = go_crossfade_eval(mode, t).b;
          } else {
            bi = 1.0f;
          }
        } else if (m_TplIn) {
          const unsigned idx = std::min(m_InPos + i, inLen ? inLen - 1 : 0);
          bi = m_TplIn->b[idx];
        } else {
          bi = 1.0f;
        }
      }
      if (out_on) {
        if (useRef) {
          if (m_OutLen > 1) {
            const float t = float(m_OutPos + i) / float(m_OutLen - 1);
            ao = go_crossfade_eval(mode, t).a;
          } else {
            ao = 0.0f;
          }
        } else if (m_TplOut) {
          const unsigned idx = std::min(m_OutPos + i, outLen ? outLen - 1 : 0);
          ao = m_TplOut->a[idx];
        } else {
          ao = 1.0f;
        }
      }

      float vol = base * ((in_on ? bi : 1.0f) * (out_on ? ao : 1.0f));
      vol = sanitize_vol(vol, lastVol);
      buf[0] *= vol;
      buf[1] *= vol;
      lastVol = vol;
    }

    if (in_on) {
      m_InPos += n;
      if (m_InPos >= m_InLen)
        m_InActive = false;
    }
    if (out_on) {
      m_OutPos += n;
      if (m_OutPos >= m_OutLen)
        m_OutActive = false;
    }
    m_LastExternalVolumePoint = endExt;
    m_LastTargetVolumePoint = go_compute_target_state_after_block(
      mode,
      m_TargetVolume,
      in_on,
      m_InActive,
      m_InLen,
      m_InPos,
      out_on,
      m_OutActive,
      m_OutLen,
      m_OutPos);
    return;
  }
#endif

  if (in_on && out_on) {
    switch (mode) {
    case GOCrossfadeMode::Linear: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai, bi, ao, bo;
        m_InLin.next(ai, bi);
        m_OutLin.next(ao, bo);
        float vol = base * (bi * ao);
        vol = sanitize_vol(vol, lastVol);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::SinEqualPower: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai, bi, ao, bo;
        m_InSin.next(ai, bi);
        m_OutSin.next(ao, bo);
        float vol = base * (bi * ao);
        vol = sanitize_vol(vol, lastVol);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::Sin2: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai, bi, ao, bo;
        m_InSin2.next(ai, bi);
        m_OutSin2.next(ao, bo);
        const float vol = base * (bi * ao);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::SqrtEqualPower: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai, bi, ao, bo;
        m_InSqrt.next(ai, bi);
        m_OutSqrt.next(ao, bo);
        const float vol = base * (bi * ao);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::X2: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai, bi, ao, bo;
        m_InX2.next(ai, bi);
        m_OutX2.next(ao, bo);
        const float vol = base * (bi * ao);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::Custom: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai = 1.f, bi = 0.f, ao = 1.f, bo = 0.f;
        if (in_on) {
          if (m_TplIn && m_TplIn->len > 0) {
            const unsigned idx = std::min(m_InPos + i, m_TplIn->len - 1);
            ai = m_TplIn->a[idx];
            bi = m_TplIn->b[idx];
          } else if (m_InLen > 1) {
            const float t = float(m_InPos + i) / float(m_InLen - 1);
            const auto g = go_crossfade_eval(mode, t);
            ai = g.a;
            bi = g.b;
          } else {
            ai = 1.f;
            bi = 0.f;
          }
        }
        if (out_on) {
          if (m_TplOut && m_TplOut->len > 0) {
            const unsigned idx = std::min(m_OutPos + i, m_TplOut->len - 1);
            ao = m_TplOut->a[idx];
            bo = m_TplOut->b[idx];
          } else if (m_OutLen > 1) {
            const float t = float(m_OutPos + i) / float(m_OutLen - 1);
            const auto g = go_crossfade_eval(mode, t);
            ao = g.a;
            bo = g.b;
          } else {
            ao = 1.f;
            bo = 0.f;
          }
        }
        float vol = base * ((in_on ? bi : 1.0f) * (out_on ? ao : 1.0f));
        vol = sanitize_vol(vol, lastVol);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    default: { // fallback to linear
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float extNow = startExt + i * dExt;
        const float base = m_TargetVolume * extNow;
        float ai, bi, ao, bo;
        m_InLin.next(ai, bi);
        m_OutLin.next(ao, bo);
        const float vol = base * (bi * ao);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    }
  } else if (in_on && !out_on) {
    switch (mode) {
    case GOCrossfadeMode::Linear: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_InLin.next(a, b);
        float vol = base * b;
        vol = sanitize_vol(vol, lastVol);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::SinEqualPower: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_InSin.next(a, b);
        float vol = base * b;
        vol = sanitize_vol(vol, lastVol);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::Sin2: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_InSin2.next(a, b);
        const float vol = base * b;
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::SqrtEqualPower: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_InSqrt.next(a, b);
        const float vol = base * b;
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::X2: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_InX2.next(a, b);
        const float vol = base * b;
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::Custom: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a = 1.f, b = 0.f;
        if (m_TplIn && m_TplIn->len > 0) {
          const unsigned idx = std::min(m_InPos + i, m_TplIn->len - 1);
          a = m_TplIn->a[idx];
          b = m_TplIn->b[idx];
        } else if (m_InLen > 1) {
          const float t = float(m_InPos + i) / float(m_InLen - 1);
          const auto g = go_crossfade_eval(mode, t);
          a = g.a;
          b = g.b;
        } else {
          a = 1.f;
          b = 0.f;
        }
        const float vol = base * b;
        const float vol_s = sanitize_vol(vol, lastVol);
        buf[0] *= vol_s;
        buf[1] *= vol_s;
        lastVol = vol_s;
      }
      break;
    }

    default: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_InLin.next(a, b);
        const float vol = base * b;
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    }
  } else if (!in_on && out_on) {
    switch (mode) {
    case GOCrossfadeMode::Linear: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_OutLin.next(a, b);
        float vol = base * a;
        vol = sanitize_vol(vol, lastVol);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::SinEqualPower: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_OutSin.next(a, b);
        float vol = base * a;
        vol = sanitize_vol(vol, lastVol);
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::Sin2: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_OutSin2.next(a, b);
        const float vol = base * a;
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::SqrtEqualPower: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_OutSqrt.next(a, b);
        const float vol = base * a;
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::X2: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_OutX2.next(a, b);
        const float vol = base * a;
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    case GOCrossfadeMode::Custom: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a = 1.f, b = 0.f;
        if (m_TplOut && m_TplOut->len > 0) {
          const unsigned idx = std::min(m_OutPos + i, m_TplOut->len - 1);
          a = m_TplOut->a[idx];
          b = m_TplOut->b[idx];
        } else if (m_OutLen > 1) {
          const float t = float(m_OutPos + i) / float(m_OutLen - 1);
          const auto g = go_crossfade_eval(mode, t);
          a = g.a;
          b = g.b;
        } else {
          a = 1.f;
          b = 0.f;
        }
        const float vol = base * a;
        const float vol_s = sanitize_vol(vol, lastVol);
        buf[0] *= vol_s;
        buf[1] *= vol_s;
        lastVol = vol_s;
      }
      break;
    }
    default: {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        float a, b;
        m_OutLin.next(a, b);
        const float vol = base * a;
        buf[0] *= vol;
        buf[1] *= vol;
        lastVol = vol;
      }
      break;
    }
    }
  } else {
    // neither In nor Out active: only external volume ramp applies
    const float base0 = m_TargetVolume * startExt;
    if (endExt == startExt) {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        buf[0] *= base0;
        buf[1] *= base0;
      }
      lastVol = base0;
    } else {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        buf[0] *= base;
        buf[1] *= base;
        lastVol = base;
      }
    }
  }

  // advance positions (one-shot bulk update)
  if (in_on) {
    m_InPos += n;
    if (m_InPos >= m_InLen)
      m_InActive = false;
  }
  if (out_on) {
    m_OutPos += n;
    if (m_OutPos >= m_OutLen)
      m_OutActive = false;
  }

  m_LastExternalVolumePoint = endExt;
  m_LastTargetVolumePoint = go_compute_target_state_after_block(
    mode,
    m_TargetVolume,
    in_on,
    m_InActive,
    m_InLen,
    m_InPos,
    out_on,
    m_OutActive,
    m_OutLen,
    m_OutPos);
}
