/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2024 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDFADER_H_
#define GOSOUNDFADER_H_

 // Enable runtime trace dump for fast crossfade diagnosis.
 // Produces a short trace file when a non-linear crossfade block is first processed.
 // Comment out to disable.
 // #define GO_FAST_XFADE_RUNTIME_DUMP 1
 // Use precomputed templates for runtime gains (exact, cache-backed).
 // Comment out to fallback to steppers only.
 #define GO_XFADE_USE_TEMPLATES 1

 // Enable reference evaluator in the hotloop (bypasses steppers) to validate mapping.
 // Comment out to re-enable stepper-based evaluation.
 // #define GO_XFADE_RUNTIME_USE_REF 1
 // Verify Template vs Reference per Block (cheap precheck, auto-fallback on mismatch)
 // #define GO_XFADE_VERIFY_REF 1

#include <assert.h>
#include <cmath>
#include "../GOCrossfadeMode.h"
#include "../GOCrossfadeParam.h"
#include "../fast_crossfade.h"

// log stuff:
#include <wx/log.h>
#include <wx/memory.h>
#include <wx/string.h>

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
  
  GOCrossfadeMode m_CurrentFadeMode = GOCrossfadeMode::SinEqualPower;

  // for sinus-fade
  unsigned m_FadeStartSample = 0;
  unsigned m_FadeInLengthSamples = 0;
  unsigned m_FadeOutLengthSamples = 0;
  float m_FadeStartVolume = 0.0f;

  // global progress
  unsigned m_CurrentSampleCounter = 0;

  // separate, independent envelopes
  bool     m_InActive   = false;
  unsigned m_InLen      = 0;
  unsigned m_InPos      = 0;

  bool     m_OutActive  = false;
  unsigned m_OutLen     = 0;
  unsigned m_OutPos     = 0;

  // fast_crossfade steppers (per-fader instances; cheap)
  GOAudioParams::CFLinear   m_InLin, m_OutLin;
  GOAudioParams::CFSinEP    m_InSin, m_OutSin;
  GOAudioParams::CFSin2     m_InSin2, m_OutSin2;
  GOAudioParams::CFX2       m_InX2, m_OutX2;
  GOAudioParams::CFSqrtEP   m_InSqrt, m_OutSqrt;

  // Optional precomputed templates (safe, exact, cache-backed)
  std::shared_ptr<GOAudioParams::FastXfadeTemplate> m_TplIn;
  std::shared_ptr<GOAudioParams::FastXfadeTemplate> m_TplOut;

  GOCrossfadeMode           m_ModeCached = GOCrossfadeMode::Linear;
  // runtime dump guard: set true after a single runtime trace has been emitted
  bool                      m_RuntimeDumpDone = false;

  // Nichtlinearer Fader in die Ausgabe akkumulieren (interner Helfer).
  void ProcessNonLinearFadeAndAccumulate(unsigned nFrames,
                                         const float* in, float* out,
                                         float externalVolume);

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
   */
  inline void StartDecreasingVolume(unsigned nFrames) {
    // REMOVE ME: I'm just a debugging helper
    /*if(IsSilent())
    {    return; wxLogInfo("This Sample is already silent!"); }*/ // <- got you removed :P you little bug!

    // Use current runtime crossfade mode (don't rely on a per-fader cached copy).
    using namespace GOAudioParams;
    const auto mode = GetCrossfadeMode();
    if (mode != GOCrossfadeMode::Linear) {
      if(m_OutActive && ((m_OutLen-m_OutPos)<nFrames))
        return;   // ignore Fade Out when shorter fade is already active

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

    // --- Legacy Linear behavior (keep exact semantics) ---
    m_DecreasingDeltaPerFrame = (nFrames ? (m_TargetVolume / nFrames) : m_TargetVolume);

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

  void Process(unsigned nFrames, float *buffer, float externalVolume);

  // Fused-Variante: skaliert und akkumuliert in einem Pass in den Output.
  // Verändert 'in' NICHT. 'out' wird um (in * gain(t)) erhöht.
  // Entspricht semantisch: Process(n, temp, extVol) + for(...) out += temp
  // – nur eben in einem Loop (perf).
  void ProcessAndAccumulate(unsigned nFrames,
                            const float* in, float* out,
                            float externalVolume);
  void ProcessNonLinearFade(unsigned nFrames, float *buffer, float externalVolume);

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
