/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDPROVIDER_H_
#define GOSOUNDPROVIDER_H_

#include <cstdint>
#include <vector>

#include "sound/playing/GOSoundReleaseAlignTable.h"
#include "sound/playing/GOSoundToneBalanceFilter.h"

#include "GOBool3.h"
#include "GOStatisticCallback.h"
#include "ptrvector.h"

// forward:
class GOSoundingPipe;

class GOCache;
class GOCacheWriter;
class GOHash;
class GOMemoryPool;

typedef struct audio_section_stream_s audio_section_stream;

class GOSoundProvider : public GOStatisticCallback {
protected:
  struct AttackSelector {
    unsigned min_attack_velocity;
    unsigned max_released_time;
    GOBool3 m_WaveTremulantStateFor;
  };

  struct ReleaseSelector {
    unsigned max_playback_time;
    GOBool3 m_WaveTremulantStateFor;
  };

  unsigned m_MidiKeyNumber;
  float m_MidiPitchFract;
  unsigned
    m_HarmonicNumber; // foot length as harmonic number: 8=8', 4=4', 16=16'
  float m_Gain;
  float m_Tuning;
  int8_t m_ToneBalanceValue;
  GOSoundToneBalanceFilter m_ToneBalance;
  bool m_IsWaveTremulantActive;
  unsigned m_ReleaseTail;
  ptr_vector<GOSoundAudioSection> m_Attack;
  std::vector<AttackSelector> m_AttackInfo;
  ptr_vector<GOSoundAudioSection> m_Release;
  std::vector<ReleaseSelector> m_ReleaseInfo;
  void ComputeReleaseAlignmentInfo();
  void RebuildAlignmentPointers();
  float m_VelocityVolumeBase;
  float m_VelocityVolumeIncrement;
  unsigned m_AttackSwitchCrossfadeLength;

#if __has_include("sound/playing/GOLogReleaseAlignEnable.h")
#include <string>
  std::string m_label;
#endif

  // Debug Sollution for retrieving the rank/pipe, that is associated with the
  // sound provider. Needed to test Rank specific Release gain model
  GOSoundingPipe *m_OwnerPipe = nullptr;
  unsigned m_OwnerRankId
    = 0; // optionally, the sound provider can be associated with a rank, that
         // is used to retrieve the rank/pipe, that is associated with the sound
         // provider. Needed to test Rank specific Release gain model

  // When true, SetupStreamAlignment() skips ComputeCorrelationLut() because a
  // valid LUT cache will be applied afterwards (avoids redundant computation).
  bool m_skipCorrLutCompute = false;

  // Returns the fundamental frequency of this pipe's recorded audio.
  // Used as the period hint for ComputeCorrelationLut in all three code paths
  // (ComputeReleaseAlignmentInfo, TryPermissiveLutForRelease, TryExhaustive).
  float ComputeSampleFreqHz() const;

public:
  // Setters and getters for the above mentioned debug solution
  void SetOwnerPipe(GOSoundingPipe *p) { m_OwnerPipe = p; }
  GOSoundingPipe *GetOwnerPipe() const { return m_OwnerPipe; }
#if __has_include("sound/playing/GOLogReleaseAlignEnable.h")
  void SetLabel(const std::string &label) { m_label = label; }
  const std::string &GetLabel() const { return m_label; }
#endif
  void SetSkipCorrLutCompute(bool skip) { m_skipCorrLutCompute = skip; }
  void SetHarmonicNumber(unsigned n) { m_HarmonicNumber = (n > 0) ? n : 8; }
  void SetOwnerRankId(unsigned id) { m_OwnerRankId = id; }  // optional
  unsigned GetOwnerRankId() const { return m_OwnerRankId; } // optional

  static void UpdateCacheHash(GOHash &hash);

  GOSoundProvider();
  virtual ~GOSoundProvider();

  void ClearData();

  virtual bool LoadCache(GOMemoryPool &pool, GOCache &cache);
  virtual bool SaveCache(GOCacheWriter &cache) const;

  bool IsWaveTremulant() const { return m_IsWaveTremulantActive; }
  void SetWaveTremulant(bool isActive) { m_IsWaveTremulantActive = isActive; }

  void SetVelocityParameter(float min_volume, float max_volume);

  bool IsWaveTremulantStateSuitable(GOBool3 waveTremulantStateFor) const {
    return to_bool(waveTremulantStateFor, m_IsWaveTremulantActive)
      == m_IsWaveTremulantActive;
  }

  const GOSoundAudioSection *GetAttack(
    unsigned velocity, unsigned releasedDurationMs) const;
  const GOSoundAudioSection *GetRelease(
    GOBool3 waveTremulantStateFor, unsigned playbackDurationMs) const;
  float GetGain() const;
  int IsOneshot() const;

  float GetTuning() const;
  void SetTuning(float cent);
  int8_t GetToneBalanceValue() const;
  void SetToneBalanceValue(int8_t value);
  void SetToneBalanceFilterSamplerate(unsigned samplerate);
  const GOSoundToneBalanceFilter *GetToneBalance() const {
    return &m_ToneBalance;
  }
  unsigned GetReleaseTail() const { return m_ReleaseTail; }
  void SetReleaseTail(unsigned releaseTail) { m_ReleaseTail = releaseTail; }

  unsigned GetReleaseCount() const { return (unsigned)m_Release.size(); }
  const GOSoundAudioSection *GetReleaseSection(unsigned i) const {
    return (i < m_Release.size()) ? m_Release[i] : nullptr;
  }

  // Assign sequential parse indices to all release sections starting at
  // startIndex.  Returns the next available index (= startIndex + release
  // count).
  unsigned AssignReleaseParseIndices(unsigned startIndex);

  // LUT computation result: support points plus the period used during
  // generation, so the caller can store both in the .golut cache.
  struct LutResult {
    std::vector<GOSoundReleaseAlignTable::CorrPoint> points;
    uint32_t period_samples = 0;
    double period_float = 0.0;
  };

  // Compute a permissive (no quality-guards) LUT for a release.
  // Used by the non-force generator path for legacy-fallback releases.
  // Does NOT modify the live in-memory aligner.
  LutResult TryPermissiveLutForRelease(unsigned releaseIdx) const;

  // Compute an exhaustive LUT for a release: corr_at() for every period
  // n in [n_start, n_end), no quality guards, no MAX_TOTAL cap.
  // Used by the force-all generator to produce complete coverage.
  // Does NOT modify the live in-memory aligner.
  LutResult TryExhaustiveLutForRelease(unsigned releaseIdx) const;

  unsigned GetMidiKeyNumber() const;
  float GetMidiPitchFract() const;
  unsigned GetAttackSwitchCrossfadeLength() const {
    return m_AttackSwitchCrossfadeLength;
  }

  float GetVelocityVolume(unsigned velocity) const;

  bool CheckForMissingAttack();
  bool CheckForMissingRelease();
  bool CheckMissingRelease();
  bool CheckNotNecessaryRelease();

  GOSampleStatistic GetStatistic();
};

inline float GOSoundProvider::GetGain() const { return m_Gain; }

inline float GOSoundProvider::GetTuning() const { return m_Tuning; }

inline int8_t GOSoundProvider::GetToneBalanceValue() const {
  return m_ToneBalanceValue;
}

#endif /* GOSOUNDPROVIDER_H_ */
