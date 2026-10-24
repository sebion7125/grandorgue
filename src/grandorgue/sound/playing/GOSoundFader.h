/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDFADER_H_
#define GOSOUNDFADER_H_

// Enable runtime trace dump for fast crossfade diagnosis.
// Produces a short trace file when a non-linear crossfade block is first
// processed. Comment out to disable. #define GO_FAST_XFADE_RUNTIME_DUMP 1 Use
// precomputed templates for runtime gains (exact, cache-backed). Comment out to
// fallback to steppers only.
#define GO_XFADE_USE_TEMPLATES 1

// Enable reference evaluator in the hotloop (bypasses steppers) to validate
// mapping. Comment out to re-enable stepper-based evaluation. #define
// GO_XFADE_RUNTIME_USE_REF 1 Verify Template vs Reference per Block (cheap
// precheck, auto-fallback on mismatch) #define GO_XFADE_VERIFY_REF 1

#include "../GOCrossfadeMode.h"
#include "../GOCrossfadeParam.h"
#include "../fast_crossfade.h"
#include "GOHotPath.h"
#include <assert.h>
#include <cmath>

// log stuff:
#include <wx/log.h>
#include <wx/memory.h>
#include <wx/string.h>

// Which gain kernel a fused fade-and-accumulate call actually executed.
// Diagnostics only: returned by ProcessAndAccumulate() so the engine can
// histogram the voice mix for the Audio Monitor (constant and ramp are the
// auto-vectorized cheap loops; an active non-linear crossfade runs the
// scalar LUT/stepper loop).
enum class GOFadeKernelKind : uint8_t {
  Constant = 0,
  Ramp = 1,
  NonLinearFade = 2,
};

/**
 * This class is responsible for smoothly changing a volume of samples.
 *
 * There are different types of volumes
 * - Target volume. It is defined by total capacity of the sample size and may
 *   be adjusted in odf and settings.
 * - External volume. It may be changed by an enclosure during playback.
 * - VelocityVolume. It is defined when a key is pressed and may be changed if
 *   another coupled key with the same pipe is pressed with another velocity.
 *   It is considered as part of an
 * - Total volume. It is a production of of all volumes above.
 *
 * There are three stages of playing one sample related to the target volume:
 * - Increasing: from 0 to m_TargetVolume during m_IncreasingFrames. It is used
 *   only for crossfade between two samples.
 * - Constant playing: the volume is not changed. Usually it is used for the
 *   attack sample (because the increasing is already sampled).
 * - Decreasing: from m_СurrentVolume to 0. It is used for both the crossfade
 *   from a loop to a release sample and when playing the release sample when
 *   ReleseTail limitation is used
 *
 * totalVol = targetVolume * externalVolume
 * This volume is applied in the Process() call
 */

#if 0
// Sinus fade mode
enum class FadeMode { None, Linear, Sinus };
#endif
// Legacy FadeMode is replaced by GOCrossfadeMode (see GOCrossfadeMode.h).
// The enum above is retained in an #if 0 block to preserve reference until
// all uses are migrated to the new GOCrossfadeMode and GOAudioParams.

class GOSoundFader {
private:
  // The final volume after increasing
  float m_TargetVolume;
  // Additional coeff.
  float m_VelocityVolume;

  // a volume delta for one frame when increasing
  float m_IncreasingDeltaPerFrame;

  // a volume delta for one frame when decreasing
  float m_DecreasingDeltaPerFrame;

  // Last volume points are the volumes at the end of previous Process()
  float m_LastTargetVolumePoint;
  float m_LastExternalVolumePoint;

  GOCrossfadeMode m_CurrentFadeMode = GOCrossfadeMode::Linear;

  // for sinus-fade
  unsigned m_FadeStartSample = 0;
  unsigned m_FadeInLengthSamples = 0;
  unsigned m_FadeOutLengthSamples = 0;
  float m_FadeStartVolume = 0.0f;

  // global progress
  unsigned m_CurrentSampleCounter = 0;

  // separate, independent envelopes
  bool m_InActive = false;
  unsigned m_InLen = 0;
  unsigned m_InPos = 0;

  bool m_OutActive = false;
  unsigned m_OutLen = 0;
  unsigned m_OutPos = 0;

  // fast_crossfade steppers (per-fader instances; cheap)
  GOAudioParams::CFLinear m_InLin, m_OutLin;
  GOAudioParams::CFSinEP m_InSin, m_OutSin;
  GOAudioParams::CFSin2 m_InSin2, m_OutSin2;
  GOAudioParams::CFX2 m_InX2, m_OutX2;
  GOAudioParams::CFSqrtEP m_InSqrt, m_OutSqrt;

  // Optional precomputed templates (safe, exact, cache-backed)
  std::shared_ptr<GOAudioParams::FastXfadeTemplate> m_TplIn;
  std::shared_ptr<GOAudioParams::FastXfadeTemplate> m_TplOut;

  GOCrossfadeMode m_ModeCached = GOCrossfadeMode::Linear;
  // runtime dump guard: set true after a single runtime trace has been emitted
  bool m_RuntimeDumpDone = false;

  // Set by StartDecreasingVolume(..., forceLinear=true) (panic/load-drop
  // fades). Process()/ProcessAndAccumulate() dispatch on the *global*
  // crossfade mode, not on any per-fader state - without this flag, a
  // forced-linear fade-out's m_DecreasingDeltaPerFrame would be computed
  // but never consumed whenever the global mode isn't Linear, since
  // dispatch would still go to the non-linear processor, which never even
  // looks at m_DecreasingDeltaPerFrame. The sampler would then never reach
  // IsSilent() and would never be returned to the pool. Reset in Setup() so
  // a pooled fader doesn't carry the override into its next, unrelated
  // note.
  bool m_ModeOverrideLinear = false;

  // Non-linear fader: accumulate into output (internal helper).
  GOFadeKernelKind ProcessNonLinearFadeAndAccumulate(
    unsigned nFrames, const float *in, float *out, float externalVolume);

public:
  /**
   * Setup the fader for constant volume or for increasing from 0 to
   * targetVolume
   * @param targetVolume target volume to reach
   * @param velocityVolume the initial velocity volume. It may be changed later
   * @param nFramesToIncreaseIn if it is 0 then the target volume will be
   *   constant. Else it will smoothly increase from 0 to targetVolume in this
   *   number of frames
   */
  void Setup(
    float targetVolume, float velocityVolume, unsigned nFramesToIncreaseIn = 0);

  /**
   * Start decreasing the volume from the current value
   * (m_LastTargetVolumePoint) to 0
   * @param nFrames number of frames for full decay
   * @param forceLinear if true, always use the plain linear fade-out
   *   regardless of the configured crossfade mode, bypassing
   *   FastCrossfadeCache. For panic/load-dropping fades, where the curve
   *   shape doesn't matter and the fade length is randomized per call.
   */
  inline void StartDecreasingVolume(unsigned nFrames, bool forceLinear = false) {
    // REMOVE ME: I'm just a debugging helper
    /*if(IsSilent())
    {    return; wxLogInfo("This Sample is already silent!"); }*/ // <- got you removed :P you little bug!

    // Use current runtime crossfade mode (don't rely on a per-fader cached
    // copy).
    using namespace GOAudioParams;
    m_ModeOverrideLinear = forceLinear;
    const auto mode = forceLinear ? GOCrossfadeMode::Linear : GetCrossfadeMode();
    if (mode != GOCrossfadeMode::Linear) {
      if (
        m_OutActive && (m_OutPos < m_OutLen)
        && ((m_OutLen - m_OutPos) < nFrames))
        return; // ignore Fade Out when shorter fade is already active

      m_OutActive = true;
      m_OutLen = (nFrames ? nFrames : 1);
      m_OutPos = 0;
      // marker for non-linear path
      m_DecreasingDeltaPerFrame = 1.0f;
#ifdef GO_XFADE_USE_TEMPLATES
      m_ModeCached = mode;
      FastCrossfadeCache::EnsureTemplate(mode, m_OutLen);
      m_TplOut = FastCrossfadeCache::GetTemplate(mode, m_OutLen);
#endif
      return;
    }

    // Taking the Legacy Linear path (global Linear mode, or forceLinear
    // overriding an in-progress non-linear fade-out): clear any stale
    // non-linear Out-envelope state. IsSilent() and
    // go_advance_legacy_shadow_state() both check m_OutLen first - if a
    // prior non-linear StartDecreasingVolume() left m_OutActive/m_OutLen/
    // m_OutPos set, they would keep tracking that old envelope's length
    // instead of this (possibly much shorter/longer) linear fade, making the
    // sampler silent too early or too late regardless of the correct
    // m_LastTargetVolumePoint-based linear decay computed below.
    m_OutActive = false;
    m_OutLen = 0;
    m_OutPos = 0;
    m_TplOut.reset();

    // --- Legacy Linear behavior (keep exact semantics) ---
    m_DecreasingDeltaPerFrame
      = (nFrames ? (m_TargetVolume / nFrames) : m_TargetVolume);

    if (m_IncreasingDeltaPerFrame > 0.0f) {
      // Interrupt an ongoing fade-in (legacy overlap formula)
      assert(m_LastTargetVolumePoint < m_TargetVolume);
      m_TargetVolume = (m_TargetVolume - m_LastTargetVolumePoint)
          * m_IncreasingDeltaPerFrame
          / (m_IncreasingDeltaPerFrame + m_DecreasingDeltaPerFrame)
        + m_LastTargetVolumePoint;
    }
  }

  inline float GetVelocityVolume() const { return m_VelocityVolume; }
  inline void SetVelocityVolume(float volume) { m_VelocityVolume = volume; }

  GO_HOT void Process(unsigned nFrames, float *buffer, float externalVolume);

  // True when this fader's next block must run the non-linear
  // crossfade path (LUT/template kernel) instead of the linear
  // constant/ramp gains - the batch render path uses this to route such
  // voices through the classic single-voice fader. Mirrors the dispatch
  // at the top of ProcessAndAccumulate().
  bool NeedsNonLinearProcessing() const {
    return !m_ModeOverrideLinear
      && GOAudioParams::GetCrossfadeMode() != GOCrossfadeMode::Linear;
  }

  // The linear-path state advance of ProcessAndAccumulate() WITHOUT the
  // sample loop: advances the fade/external-volume state for nFrames
  // frames exactly as a Process/ProcessAndAccumulate call would, and
  // returns this block's frame-gain sequence as gain(i) = outG0 +
  // i * outDg (outDg == 0 for a constant block). The batch render path
  // feeds these into the 8-lane accumulate kernel
  // (GOSoundResampleBatch); ProcessAndAccumulate below uses it for its
  // own scalar loop, so the two paths share one state machine by
  // construction. Must not be called when NeedsNonLinearProcessing().
  GO_HOT GOFadeKernelKind ComputeFrameGains(
    unsigned nFrames, float externalVolume, float &outG0, float &outDg);

  /* Non-linear counterpart of ComputeFrameGains for the batch render
   * path: computes this block's per-frame TOTAL gains into
   * dst[i * stride] (i = 0..nFrames-1) and advances the fade/external
   * state exactly like ProcessNonLinearFadeAndAccumulate would - the two
   * share the template evaluation and the state-advance math, so a voice
   * may switch between the scalar and the batch path between blocks
   * without any drift. Covers the production paths of the non-linear
   * processor: the template-driven active fades and the no-envelope
   * constant/ramp tail. Returns false WITHOUT touching the envelope or
   * external-volume state when an active envelope has no template bound
   * (the stepper-only fallback stays classic); the caller then routes
   * the voice through the classic laneOut detour for this block. Must
   * only be called when NeedsNonLinearProcessing(). */
  GO_HOT bool FillFrameGains(
    unsigned nFrames, float externalVolume, float *dst, unsigned stride);

  // Fused variant: scales and accumulates into output in a single pass.
  // Does NOT modify 'in'. 'out' is incremented by (in * gain(t)).
  // Semantically equivalent to: Process(n, temp, extVol) + for(...) out += temp
  // — but in a single loop (perf).
  // Returns which kernel ran (diagnostics; see GOFadeKernelKind).
  /* Steady-linear fast-path query (2026-07-13; generalizes the former
   * steady-constant one): returns true - with the block's linear gain
   * sequence gain(i) = outGain0 + i*outDGain - exactly when this block
   * would take the non-linear SUSTAIN branch (no active envelopes,
   * non-linear mode, no linear override); outDGain != 0 when the
   * external volume takes a smoothing step this block. On true, the
   * fader's block-end state is advanced identically to what
   * ProcessNonLinearFadeAndAccumulate's sustain branch would have done,
   * and the caller accumulates out += fir * gain(i) itself (fused into
   * the FIR loop, no temp round-trip). On false, NOTHING was mutated
   * beyond what the full path would do first anyway - call
   * ProcessAndAccumulate as before. */
  GO_HOT bool TrySteadyLinearGain(
    float externalVolume, unsigned nFrames, float &outGain0, float &outDGain);

  GO_HOT GOFadeKernelKind ProcessAndAccumulate(
    unsigned nFrames, const float *in, float *out, float externalVolume);
  void ProcessNonLinearFade(
    unsigned nFrames, float *buffer, float externalVolume);

  bool IsSilent() const {
    // If an Out envelope is present, consider the sampler silent when the
    // envelope position has reached its length (mode-agnostic).
    if (m_OutLen > 0)
      return (m_OutPos >= m_OutLen);

    // Fallback to legacy behavior: if no Out-envelope is present, use the
    // last target-volume check (compatible with linear path).
    return (m_LastTargetVolumePoint <= 0.0f);
  }
  /*bool IsSilent() const {
  return (
    m_DecreasingDeltaPerFrame == 0.0f &&
    m_IncreasingDeltaPerFrame == 0.0f &&
    m_LastTargetVolumePoint <= 0.00001f &&
    m_CurrentSampleCounter >= m_FadeOutLengthSamples
  ); }*/
};

#endif /* GOSOUNDFADER_H_ */
