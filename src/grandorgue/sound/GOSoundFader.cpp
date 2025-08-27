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
  m_ModeCached = m_CurrentFadeMode;

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

    // Initialize fast steppers for this fader once at Setup (cheap)
    // This avoids trig/sqrt calls inside Process() hot-loop.
    // Note: initialization cost occurs per fader Setup; later we can move to
    // a global template/cache if necessary.
    if (m_InLen > 0) {
      m_InLin.init(m_InLen, m_InPos);
      m_InX2.init(m_InLen, m_InPos);
      m_InSin.init(m_InLen, m_InPos);
      m_InSin2.init(m_InLen, m_InPos);
      m_InSqrt.init(m_InLen, m_InPos);
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
  }

  const auto mode = m_ModeCached;
  const bool in_on  = (m_InActive  && m_InLen  > 0);
  const bool out_on = (m_OutActive && m_OutLen > 0);

  float lastVol = 0.0f;

  // Four branchless-style paths chosen once per block, then a hot loop per path.
  if (in_on && out_on) {
    switch (mode) {
      case GOCrossfadeMode::Linear: {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          const float extNow = startExt + i * dExt;
          const float base = m_TargetVolume * extNow;
          float ai, bi, ao, bo;
          m_InLin.next(ai, bi);
          m_OutLin.next(ao, bo);
          const float vol = base * (bi * ao);
          buf[0] *= vol; buf[1] *= vol;
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
          const float vol = base * (bi * ao);
          buf[0] *= vol; buf[1] *= vol;
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
          buf[0] *= vol; buf[1] *= vol;
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
          buf[0] *= vol; buf[1] *= vol;
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
          buf[0] *= vol; buf[1] *= vol;
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
          buf[0] *= vol; buf[1] *= vol;
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
          float a,b;
          m_InLin.next(a,b);
          const float vol = base * b;
          buf[0] *= vol; buf[1] *= vol;
          lastVol = vol;
        }
        break;
      }
      case GOCrossfadeMode::SinEqualPower: {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          const float base = m_TargetVolume * (startExt + i * dExt);
          float a,b;
          m_InSin.next(a,b);
          const float vol = base * b;
          buf[0] *= vol; buf[1] *= vol;
          lastVol = vol;
        }
        break;
      }
      case GOCrossfadeMode::Sin2: {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          const float base = m_TargetVolume * (startExt + i * dExt);
          float a,b;
          m_InSin2.next(a,b);
          const float vol = base * b;
          buf[0] *= vol; buf[1] *= vol;
          lastVol = vol;
        }
        break;
      }
      case GOCrossfadeMode::SqrtEqualPower: {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          const float base = m_TargetVolume * (startExt + i * dExt);
          float a,b;
          m_InSqrt.next(a,b);
          const float vol = base * b;
          buf[0] *= vol; buf[1] *= vol;
          lastVol = vol;
        }
        break;
      }
      case GOCrossfadeMode::X2: {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          const float base = m_TargetVolume * (startExt + i * dExt);
          float a,b;
          m_InX2.next(a,b);
          const float vol = base * b;
          buf[0] *= vol; buf[1] *= vol;
          lastVol = vol;
        }
        break;
      }
      default: {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          const float base = m_TargetVolume * (startExt + i * dExt);
          float a,b;
          m_InLin.next(a,b);
          const float vol = base * b;
          buf[0] *= vol; buf[1] *= vol;
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
          float a,b;
          m_OutLin.next(a,b);
          const float vol = base * a;
          buf[0] *= vol; buf[1] *= vol;
          lastVol = vol;
        }
        break;
      }
      case GOCrossfadeMode::SinEqualPower: {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          const float base = m_TargetVolume * (startExt + i * dExt);
          float a,b;
          m_OutSin.next(a,b);
          const float vol = base * a;
          buf[0] *= vol; buf[1] *= vol;
          lastVol = vol;
        }
        break;
      }
      case GOCrossfadeMode::Sin2: {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          const float base = m_TargetVolume * (startExt + i * dExt);
          float a,b;
          m_OutSin2.next(a,b);
          const float vol = base * a;
          buf[0] *= vol; buf[1] *= vol;
          lastVol = vol;
        }
        break;
      }
      case GOCrossfadeMode::SqrtEqualPower: {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          const float base = m_TargetVolume * (startExt + i * dExt);
          float a,b;
          m_OutSqrt.next(a,b);
          const float vol = base * a;
          buf[0] *= vol; buf[1] *= vol;
          lastVol = vol;
        }
        break;
      }
      case GOCrossfadeMode::X2: {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          const float base = m_TargetVolume * (startExt + i * dExt);
          float a,b;
          m_OutX2.next(a,b);
          const float vol = base * a;
          buf[0] *= vol; buf[1] *= vol;
          lastVol = vol;
        }
        break;
      }
      default: {
        for (unsigned i = 0; i < n; ++i, buf += 2) {
          const float base = m_TargetVolume * (startExt + i * dExt);
          float a,b;
          m_OutLin.next(a,b);
          const float vol = base * a;
          buf[0] *= vol; buf[1] *= vol;
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
        buf[0] *= base0; buf[1] *= base0;
      }
      lastVol = base0;
    } else {
      for (unsigned i = 0; i < n; ++i, buf += 2) {
        const float base = m_TargetVolume * (startExt + i * dExt);
        buf[0] *= base; buf[1] *= base;
        lastVol = base;
      }
    }
  }

  // advance positions (one-shot bulk update)
  if (in_on)  { m_InPos  += n; if (m_InPos  >= m_InLen)  m_InActive  = false; }
  if (out_on) { m_OutPos += n; if (m_OutPos >= m_OutLen) m_OutActive = false; }

  m_LastExternalVolumePoint = endExt;
  m_LastTargetVolumePoint   = lastVol;
}
