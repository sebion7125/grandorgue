/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2024 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDFADER_H_
#define GOSOUNDFADER_H_

#include <assert.h>
#include <cmath>
#include "GOCrossfadeMode.h"
#include "GOCrossfadeParam.h"

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
    // Non-linear modes: enable the Out envelope (new double-envelope logic)
    if (m_CurrentFadeMode != GOCrossfadeMode::Linear) {
      m_OutActive = true;
      m_OutLen = (nFrames ? nFrames : 1);
      m_OutPos = 0;
      // marker for non-linear path
      m_DecreasingDeltaPerFrame = 1.0f;
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
  void ProcessNonLinearFade(unsigned nFrames, float *buffer, float externalVolume);

  bool IsSilent() const {
    // Linear: legacy-compatible — silent when internal last target volume reached 0.
    if (m_CurrentFadeMode == GOCrossfadeMode::Linear) {
      return (m_LastTargetVolumePoint <= 0.0f);
    }

    // Non-linear: if an explicit Out envelope WAS active, it's silent only when it has been finished.
    //if (m_OutActive)
    if(m_OutLen > 0)
    return (m_OutPos >= m_OutLen);

    // No Out envelope active => not silent (stream will run to EOF)
    return false;
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
