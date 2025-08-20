/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2024 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundFader.h"

#include <algorithm>
#include <cstdio>


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

  m_CurrentFadeMode = FadeMode::Sinus;
  
  /*FILE* f = fopen("debug.txt", "a");
fprintf(f, "[FADER] Setup: target=%f, velocity=%f, nFrames=%u\n",
  targetVolume, velocityVolume, nFramesToIncreaseIn);
fclose(f);*/
  
  m_TargetVolume = targetVolume;
  m_VelocityVolume = velocityVolume;
  m_DecreasingDeltaPerFrame = 0.0f;  // kein FadeOut aktiv
  m_LastExternalVolumePoint = -1.0f;

  m_CurrentSampleCounter = 0;
  
  if (nFramesToIncreaseIn == 0) {
    // Sofort auf Ziel-Lautstärke
    m_LastTargetVolumePoint = targetVolume;
    m_IncreasingDeltaPerFrame = 0.0f;
    m_FadeLengthSamples = 0;
    m_FadeStartSample = 0;
    m_FadeStartVolume = targetVolume;
  } else {
    m_LastTargetVolumePoint = 0.0f;
    m_FadeLengthSamples = nFramesToIncreaseIn;
    m_FadeStartSample = 0;

    if (m_CurrentFadeMode == FadeMode::Sinus) {
      /*FILE* f = fopen("debug.txt", "a");
      fprintf(f, "[FADER] Sinus-Fade aktiv: Sample %u / %u\n",
        m_CurrentSampleCounter, m_FadeLengthSamples);
      fclose(f);*/
              
      m_FadeStartVolume = 0.0f;
      m_IncreasingDeltaPerFrame = 1.0f;  // Marker: Sinus-Fade aktiv
      m_FadeStartSample = m_CurrentSampleCounter = 0;
    } else {
      /*FILE* f = fopen("debug.txt", "a");
      fprintf(f, "[FADER] Linear-Fade aktiv: Sample %u / %u\n",
        m_CurrentSampleCounter, m_FadeLengthSamples);
      fclose(f);*/
      m_FadeStartVolume = targetVolume;
      m_IncreasingDeltaPerFrame = targetVolume / nFramesToIncreaseIn;
    }
  }
}

// if the external volume is changed, do it smoothly in this number of frames
static constexpr unsigned EXTERNAL_VOLUME_CHANGE_FRAMES = 1024;

void GOSoundFader::Process(  
  unsigned nFrames, float *buffer, float externalVolume) {
  // setup process
  
  if (m_CurrentFadeMode == FadeMode::Sinus) {
    ProcessSinusFade(nFrames, buffer, externalVolume);
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

      // stop increasing
      m_IncreasingDeltaPerFrame = 0.0f;

      // calculate how many frames left after increasing
      framesLeftAfterIncreasing = nFrames
        - unsigned((m_TargetVolume - m_LastTargetVolumePoint)
                   / m_IncreasingDeltaPerFrame);

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

// sinus mode processing
void GOSoundFader::ProcessSinusFade(unsigned nFrames, float* buffer, float externalVolume) {
  if (nFrames == 0)
    return;

  // Zielwert berechnen
  float targetExternalVolume = m_VelocityVolume * externalVolume;
  if (m_LastExternalVolumePoint < 0.0f)
    m_LastExternalVolumePoint = targetExternalVolume;

  float startExternalVolume = m_LastExternalVolumePoint;

  if (targetExternalVolume != startExternalVolume) {
    m_LastExternalVolumePoint +=
      (targetExternalVolume - startExternalVolume)
      * std::max(nFrames, EXTERNAL_VOLUME_CHANGE_FRAMES)
      / EXTERNAL_VOLUME_CHANGE_FRAMES;
  }

  float endExternalVolume = m_LastExternalVolumePoint;

  // Delta pro Frame
  float externalDelta = (endExternalVolume - startExternalVolume) / nFrames;

  float volume = 0.0f;

  for (unsigned i = 0; i < nFrames; ++i, buffer += 2, ++m_CurrentSampleCounter) {
    // aktuelles ExternalVolume interpolieren
    float currentExternal = startExternalVolume + i * externalDelta;
    float baseVolume = m_TargetVolume * currentExternal;

    // Sinus-Fade berechnen
    float x = float(m_CurrentSampleCounter) / float(m_FadeLengthSamples);
    if (x > 1.0f) x = 1.0f;

    float fadeFactor = 1.0f;
    if (m_IncreasingDeltaPerFrame > 0.0f) {
      float s = sinf(0.5f * M_PI * x);
      fadeFactor = s;
    } else if (m_DecreasingDeltaPerFrame > 0.0f) {
      float c = cosf(0.5f * M_PI * x);
      fadeFactor = c;
    }

    volume = baseVolume * fadeFactor;
    buffer[0] *= volume;
    buffer[1] *= volume;
  }

  m_LastTargetVolumePoint = volume;

  // Fade abschließen
  if (m_CurrentSampleCounter >= m_FadeLengthSamples) {
    if (m_IncreasingDeltaPerFrame > 0.0f) {
      m_LastTargetVolumePoint = m_TargetVolume;
      m_IncreasingDeltaPerFrame = 0.0f;
    } else if (m_DecreasingDeltaPerFrame > 0.0f) {
      m_LastTargetVolumePoint = 0.0f;
      m_DecreasingDeltaPerFrame = 0.0f;
    }
  }
}