/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2024 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundFader.h"
#include "GOCrossfadeMode.h"
#include "GOCrossfadeParam.h"

#include <algorithm>
#include <cstdio>
#include <wx/log.h>
#include <wx/string.h>


// backup old version of setup:
/*void GOSoundFader::Setup(
  float targetVolume, float velocityVolume, unsigned nFramesToIncreaseIn) {
  m_TargetVolume = targetVolume;

  if (nFramesToIncreaseIn) {
    m_LastTargetVolumePoint = 0.0f;
    m_IncreasingDeltaPerFrame = targetVolume / nFramesToIncreaseIn;
  } else {
    m_LastTargetVolumePoint = targetVolume;
    m_IncreasingDeltaPerFrame = 0.0f;
  }
  m_DecreasingDeltaPerFrame = 0.0f;
  m_VelocityVolume = velocityVolume;
  m_LastExternalVolumePoint = -1; // will be set on the first Process() call
}*/

void GOSoundFader::Setup(
  float targetVolume, float velocityVolume, unsigned nFramesToIncreaseIn) {

  using namespace GOAudioParams;
  m_CurrentFadeMode = GetCrossfadeMode();

  m_TargetVolume = targetVolume;
  m_VelocityVolume = velocityVolume;
  m_DecreasingDeltaPerFrame = 0.0f;  // no FadeOut active
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
  }

  // out envelope init
  m_OutActive = false;
  m_OutLen = m_OutPos = 0;
  m_FadeOutLengthSamples = 0;
  // m_DecreasingDeltaPerFrame remains 0.0f (no fade-out active)
}

// if the external volume is changed, do it smoothly in this number of frames
static constexpr unsigned EXTERNAL_VOLUME_CHANGE_FRAMES = 1024;

void GOSoundFader::Process(  
  unsigned nFrames, float *buffer, float externalVolume) {
  // setup process

  // Legacy fast-path: if Linear, continue with existing linear logic below.
  // For any non-linear mode, delegate to the non-linear processor.
  if (m_CurrentFadeMode != GOCrossfadeMode::Linear) {
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

      // former stop increasing here yielded division by zero
      //m_IncreasingDeltaPerFrame = 0.0f;
      //wxLogInfo("Division by zero would have happened in GO Release");

      // calculate how many frames left after increasing
      framesLeftAfterIncreasing = nFrames
        - unsigned((m_TargetVolume - m_LastTargetVolumePoint)
                   / m_IncreasingDeltaPerFrame);
      
      //wxLogInfo("Division by zero yielded %d", framesLeftAfterIncreasing);

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
    m_LastExternalVolumePoint
      += (targetExternalVolume - startExternalVolumePoint)
      // Assume that external volume is to be reached in MAX_FRAME_SIZE frames
      * std::max(nFrames, EXTERNAL_VOLUME_CHANGE_FRAMES)
      / EXTERNAL_VOLUME_CHANGE_FRAMES;

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
}

 // non-linear fade processing
void GOSoundFader::ProcessNonLinearFade(unsigned n, float* buf, float external) {
  if (n == 0)
    return;

  // update external envelope smoothly
  float targetExt = m_VelocityVolume * external;
  if (m_LastExternalVolumePoint < 0.0f) m_LastExternalVolumePoint = targetExt;
  float startExt = m_LastExternalVolumePoint;
  if (targetExt != startExt) {
    m_LastExternalVolumePoint +=
      (targetExt - startExt) * std::max(n, EXTERNAL_VOLUME_CHANGE_FRAMES) / EXTERNAL_VOLUME_CHANGE_FRAMES;
  }
  float endExt = m_LastExternalVolumePoint;
  const float dExt = (endExt - startExt) / n;

  using namespace GOAudioParams;
  const auto mode = GetCrossfadeMode();

  float lastVol = 0.0f;

  for (unsigned i = 0; i < n; ++i, buf += 2) {
    const float extNow = startExt + i * dExt;
    const float base = m_TargetVolume * extNow;

    // x_in/x_out default to neutral
    float x_in = 1.0f;
    float x_out = 0.0f;

    if (m_InActive && m_InLen > 0) {
      x_in = (m_InLen == 1) ? 1.0f
        : float(std::min(m_InPos, m_InLen - 1)) / float(m_InLen - 1);
    }
    if (m_OutActive && m_OutLen > 0) {
      x_out = (m_OutLen == 1) ? 1.0f
        : float(std::min(m_OutPos, m_OutLen - 1)) / float(m_OutLen - 1);
    }

    float w = 1.0f;
    if (m_InActive)  { const auto g = go_crossfade_eval(mode, x_in ); w *= g.b; }
    if (m_OutActive) { const auto g = go_crossfade_eval(mode, x_out); w *= g.a; }

    const float vol = base * w;
    buf[0] *= vol;
    buf[1] *= vol;
    lastVol = vol;

    // advance per envelope separately
    if (m_InActive  && m_InPos  < m_InLen)  ++m_InPos;
    if (m_OutActive && m_OutPos < m_OutLen) ++m_OutPos;
  }

  // finalize status
  if (m_InActive  && m_InPos  >= m_InLen)  m_InActive  = false;
  if (m_OutActive && m_OutPos >= m_OutLen) m_OutActive = false;

  // keep last overall gain for legacy paths
  m_LastTargetVolumePoint = lastVol;
}
