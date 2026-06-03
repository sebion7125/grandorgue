/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundProvider.h"

#include <cmath>

#include <wx/intl.h>

#include "loader/cache/GOCache.h"
#include "loader/cache/GOCacheWriter.h"
#include "sound/playing/GOSoundAudioSection.h"
#include "sound/playing/GOSoundReleaseAlignTable.h"

#include "GOHash.h"
#include "GOMemoryPool.h"
#include "GOSampleStatistic.h"

#define DELETE_AND_NULL(x)                                                     \
  do {                                                                         \
    if (x) {                                                                   \
      delete x;                                                                \
      x = NULL;                                                                \
    }                                                                          \
  } while (0)

void GOSoundProvider::UpdateCacheHash(GOHash &hash) {
  hash.Update(sizeof(AttackSelector));
  hash.Update(sizeof(ReleaseSelector));
  // Bump when the cache format changes (v5: float period T for phase accuracy)
  static const uint8_t CACHE_FORMAT_VERSION = 6;
  hash.Update(CACHE_FORMAT_VERSION);
}

GOSoundProvider::GOSoundProvider()
  : m_MidiKeyNumber(0),
    m_MidiPitchFract(0),
    m_HarmonicNumber(8),
    m_Tuning(1),
    m_ToneBalanceValue(0),
    m_IsWaveTremulantActive(BOOL3_FALSE),
    m_ReleaseTail(0),
    m_Attack(),
    m_AttackInfo(),
    m_Release(),
    m_ReleaseInfo(),
    m_VelocityVolumeBase(1),
    m_VelocityVolumeIncrement(0),
    m_AttackSwitchCrossfadeLength(184) {
  m_Gain = 0.0f;
}

GOSoundProvider::~GOSoundProvider() {}

void GOSoundProvider::ClearData() {
  m_Attack.clear();
  m_AttackInfo.clear();
  m_Release.clear();
  m_ReleaseInfo.clear();
}

bool GOSoundProvider::LoadCache(GOMemoryPool &pool, GOCache &cache) {
  if (!cache.Read(&m_MidiKeyNumber, sizeof(m_MidiKeyNumber)))
    return false;
  if (!cache.Read(&m_MidiPitchFract, sizeof(m_MidiPitchFract)))
    return false;
  if (!cache.Read(
        &m_AttackSwitchCrossfadeLength, sizeof(m_AttackSwitchCrossfadeLength)))
    return false;

  unsigned attacks;
  if (!cache.Read(&attacks, sizeof(attacks)))
    return false;
  for (unsigned i = 0; i < attacks; i++) {
    AttackSelector info;
    if (!cache.Read(&info, sizeof(info)))
      return false;
    m_AttackInfo.push_back(info);
    m_Attack.push_back(new GOSoundAudioSection(pool));
    if (!m_Attack[i]->LoadCache(cache))
      return false;
  }

  unsigned releases;
  if (!cache.Read(&releases, sizeof(releases)))
    return false;
  for (unsigned i = 0; i < releases; i++) {
    ReleaseSelector info;
    if (!cache.Read(&info, sizeof(info)))
      return false;
    m_ReleaseInfo.push_back(info);
    m_Release.push_back(new GOSoundAudioSection(pool));
    if (!m_Release[i]->LoadCache(cache))
      return false;
  }

  // Restore per-attack LUT pointers (lost across cache serialisation).
  RebuildAlignmentPointers();
  return true;
}

bool GOSoundProvider::SaveCache(GOCacheWriter &cache) const {
  if (!cache.Write(&m_MidiKeyNumber, sizeof(m_MidiKeyNumber)))
    return false;
  if (!cache.Write(&m_MidiPitchFract, sizeof(m_MidiPitchFract)))
    return false;
  if (!cache.Write(
        &m_AttackSwitchCrossfadeLength, sizeof(m_AttackSwitchCrossfadeLength)))
    return false;

  unsigned attacks = m_Attack.size();
  if (!cache.Write(&attacks, sizeof(attacks)))
    return false;
  for (unsigned i = 0; i < m_Attack.size(); i++) {
    if (!cache.Write(&m_AttackInfo[i], sizeof(m_AttackInfo[i])))
      return false;
    if (!m_Attack[i]->SaveCache(cache))
      return false;
  }

  unsigned releases = m_Release.size();
  if (!cache.Write(&releases, sizeof(releases)))
    return false;
  for (unsigned i = 0; i < m_Release.size(); i++) {
    if (!cache.Write(&m_ReleaseInfo[i], sizeof(m_ReleaseInfo[i])))
      return false;
    if (!m_Release[i]->SaveCache(cache))
      return false;
  }

  return true;
}

void GOSoundProvider::RebuildAlignmentPointers() {
  std::vector<const GOSoundAudioSection *> sections;
  for (int8_t k = BOOL3_MIN; k <= BOOL3_MAX; ++k) {
    sections.clear();
    for (unsigned i = 0; i < m_Attack.size(); i++)
      if (m_AttackInfo[i].m_WaveTremulantStateFor == k)
        sections.push_back(m_Attack[i]);
    for (unsigned i = 0; i < m_Release.size(); i++)
      if (m_ReleaseInfo[i].m_WaveTremulantStateFor == k)
        m_Release[i]->AssignAttackLutPointers(sections);
  }
}

void GOSoundProvider::ComputeReleaseAlignmentInfo() {
  // Compute the actual fundamental frequency of this pipe's audio.
  // m_MidiKeyNumber + m_MidiPitchFract give the base pitch from the WAV file.
  // m_HarmonicNumber corrects for foot length: 8'=1x, 4'=2x, 16'=0.5x, 2'=4x.
  const float sample_freq_hz = 440.f
    * std::pow(2.f, ((float)m_MidiKeyNumber + m_MidiPitchFract / 100.f - 69.f)
                      / 12.f)
    * ((float)m_HarmonicNumber / 8.f);

  std::vector<const GOSoundAudioSection *> sections;
  for (int8_t k = BOOL3_MIN; k <= BOOL3_MAX; ++k) {
    sections.clear();
    for (unsigned i = 0; i < m_Attack.size(); i++)
      if (m_AttackInfo[i].m_WaveTremulantStateFor == k)
        sections.push_back(m_Attack[i]);
    for (unsigned i = 0; i < m_Release.size(); i++) {
      if (m_ReleaseInfo[i].m_WaveTremulantStateFor != k)
        continue;
      const unsigned my_max = m_ReleaseInfo[i].max_playback_time;
      // min_ms = highest max_playback_time in this group that is strictly below my_max.
      // This works regardless of release sort order.
      unsigned min_ms = 0;
      for (unsigned j = 0; j < m_Release.size(); j++) {
        if (j == i || m_ReleaseInfo[j].m_WaveTremulantStateFor != k)
          continue;
        const unsigned their_max = m_ReleaseInfo[j].max_playback_time;
        if (their_max < my_max && their_max > min_ms)
          min_ms = their_max;
      }
      // max_key_press_ms=0 signals "no limit" (unlimited release).
      const unsigned max_ms = (my_max == (unsigned)-1) ? 0u : my_max;
      m_Release[i]->SetupStreamAlignment(
        sections, 0, sample_freq_hz, m_HarmonicNumber, min_ms, max_ms);
    }

    sections.clear();
    for (unsigned i = 0; i < m_Attack.size(); i++)
      if (m_AttackInfo[i].m_WaveTremulantStateFor != k)
        sections.push_back(m_Attack[i]);
    for (unsigned i = 0; i < m_Attack.size(); i++)
      if (m_AttackInfo[i].m_WaveTremulantStateFor == k)
        m_Attack[i]->SetupStreamAlignment(sections, 1, sample_freq_hz, m_HarmonicNumber);
  }

  for (unsigned i = 1; i < m_Attack.size(); i++)
    if (m_Attack[0]->IsOneshot() != m_Attack[i]->IsOneshot())
      throw(wxString)
        _("Mixing of percussive and non-percussive samples in one pipe not "
          "allowed");
}

int GOSoundProvider::IsOneshot() const {
  if (!m_Attack.size())
    return false;
  for (unsigned i = 0; i < m_Attack.size(); i++)
    if (!m_Attack[i]->IsOneshot())
      return false;

  return true;
}

void GOSoundProvider::SetTuning(float cent) {
  m_Tuning = pow(pow(2, 1.0 / 1200.0), cent);
}

void GOSoundProvider::SetToneBalanceValue(int8_t value) {
  m_ToneBalanceValue = value;
  m_ToneBalance.Init(m_ToneBalanceValue);
}

void GOSoundProvider::SetToneBalanceFilterSamplerate(unsigned samplerate) {
  m_ToneBalance.SetFilterSamplerate(samplerate);
  m_ToneBalance.Init(m_ToneBalanceValue);
}

unsigned GOSoundProvider::GetMidiKeyNumber() const { return m_MidiKeyNumber; }

float GOSoundProvider::GetMidiPitchFract() const { return m_MidiPitchFract; }

void GOSoundProvider::SetVelocityParameter(float min_volume, float max_volume) {
  if (min_volume == max_volume) {
    m_VelocityVolumeBase = min_volume / 100;
    m_VelocityVolumeIncrement = 0;
  } else {
    m_VelocityVolumeBase = min_volume / 100;
    m_VelocityVolumeIncrement = (max_volume - min_volume) / 12700.0;
  }
}

float GOSoundProvider::GetVelocityVolume(unsigned velocity) const {
  return m_VelocityVolumeBase + (velocity * m_VelocityVolumeIncrement);
}

const GOSoundAudioSection *GOSoundProvider::GetAttack(
  unsigned velocity, unsigned releasedDurationMs) const {
  const unsigned x = abs(rand());
  int best_match = -1;

  for (unsigned i = 0; i < m_Attack.size(); i++) {
    const unsigned idx = (i + x) % m_Attack.size();

    if (
      IsWaveTremulantStateSuitable(m_AttackInfo[idx].m_WaveTremulantStateFor)
      && m_AttackInfo[idx].min_attack_velocity <= velocity
      && m_AttackInfo[idx].max_released_time >= releasedDurationMs) {
      if (best_match == -1)
        best_match = idx;
      else if (
        m_AttackInfo[best_match].min_attack_velocity
          < m_AttackInfo[idx].min_attack_velocity
        && m_AttackInfo[best_match].max_released_time
          > m_AttackInfo[idx].max_released_time)
        best_match = idx;
    }
  }

  if (best_match != -1)
    return m_Attack[best_match];
  return NULL;
}

const GOSoundAudioSection *GOSoundProvider::GetRelease(
  GOBool3 waveTremulantStateFor, unsigned playbackDurationMs) const {
  const unsigned x = abs(rand());
  int best_match = -1;

  for (unsigned i = 0; i < m_Release.size(); i++) {
    const unsigned idx = (i + x) % m_Release.size();
    if (m_ReleaseInfo[idx].m_WaveTremulantStateFor != waveTremulantStateFor)
      continue;
    if (m_ReleaseInfo[idx].max_playback_time < playbackDurationMs)
      continue;
    if (best_match == -1)
      best_match = idx;
    else if (
      m_ReleaseInfo[best_match].max_playback_time
      > m_ReleaseInfo[idx].max_playback_time)
      best_match = idx;
  }

  if (best_match != -1)
    return m_Release[best_match];

  return NULL;
}

bool GOSoundProvider::CheckForMissingRelease() {
  for (int8_t k = BOOL3_MIN; k <= BOOL3_MAX; k++) {
    unsigned cnt = 0;
    bool max_release = false;
    for (unsigned i = 0; i < m_Release.size(); i++)
      if (m_ReleaseInfo[i].m_WaveTremulantStateFor == k) {
        cnt++;
        if (m_ReleaseInfo[i].max_playback_time == (unsigned)-1)
          max_release = true;
      }
    if (cnt && !max_release)
      return true;
  }
  return false;
}

bool GOSoundProvider::CheckForMissingAttack() {
  for (int8_t k = BOOL3_MIN; k <= BOOL3_MAX; k++) {
    unsigned cnt = 0;
    bool max_release = false;
    for (unsigned i = 0; i < m_Attack.size(); i++)
      if (m_AttackInfo[i].m_WaveTremulantStateFor == k) {
        cnt++;
        if (m_AttackInfo[i].max_released_time == (unsigned)-1)
          max_release = true;
      }
    if (cnt && !max_release)
      return true;
  }
  return false;
}

bool GOSoundProvider::CheckMissingRelease() {
  if (IsOneshot())
    return false;
  for (int8_t k = BOOL3_MIN; k <= BOOL3_MAX; k++) {
    bool found = false;
    for (unsigned i = 0; i < m_Attack.size() && !found; i++)
      if (m_AttackInfo[i].m_WaveTremulantStateFor == k)
        found = true;
    if (!found)
      continue;
    found = false;
    for (unsigned i = 0; i < m_Release.size() && !found; i++)
      if (m_ReleaseInfo[i].m_WaveTremulantStateFor == k)
        found = true;
    if (!found)
      return true;
  }
  return false;
}
bool GOSoundProvider::CheckNotNecessaryRelease() {
  if (IsOneshot() && m_Release.size())
    return true;
  return false;
}

GOSampleStatistic GOSoundProvider::GetStatistic() {
  GOSampleStatistic stat;
  for (unsigned i = 0; i < m_Attack.size(); i++)
    stat.Cumulate(m_Attack[i]->GetStatistic());
  for (unsigned i = 0; i < m_Release.size(); i++)
    stat.Cumulate(m_Release[i]->GetStatistic());
  return stat;
}
