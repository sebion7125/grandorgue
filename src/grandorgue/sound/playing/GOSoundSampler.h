/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2024 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDSAMPLER_H_
#define GOSOUNDSAMPLER_H_

#include <atomic>
#include <cstdint>
#include <cstring>

#include "GOBool3.h"
#include "GOSoundFader.h"
#include "GOSoundFilter.h"
#include "GOSoundStream.h"

class GOSoundProvider;
class GOSoundWindchestTask;

// One-shot polyphony load-dropping state: prevents ProcessSampler() from
// re-triggering StartDecreasingVolume() with a fresh random duration on
// every audio block while the drop condition remains true, which would
// otherwise average away the per-sampler randomization and make concurrent
// drops fade out in near-lockstep. None == 0 so a memset-cleared sampler
// (see GOSoundSamplerPool::GetSampler) starts in this state.
enum class GOSoundPolyphonyDropState : uint8_t { None, Normal, Panic };

struct GOSoundSampler {
  GOSoundSampler *next;
  /* Persistent-bucket residence token (see GOSoundGroupTask): while this
   * voice is a member of a group task's persistent bucket, this holds the
   * unique nonzero token its bucket entry captured at insertion. Set to 0
   * whenever the voice LEAVES that residence - death (ReturnSampler),
   * migration to the release processor - and by the pool's memset on
   * reuse; a bucket entry whose captured token no longer matches is
   * stale and gets skipped by the claim copy (lazy removal, compacted by
   * the period window). Uniqueness of tokens makes ABA impossible: a
   * recycled sampler re-inserted anywhere gets a FRESH token, so entries
   * from its previous life can never match again. Atomic because claim
   * copies read it concurrently with the owning worker's store. */
  std::atomic<uint64_t> m_ResidenceToken;
  /* HOT CONTROL FIELDS FIRST (2026-07-12): every period, every live
   * voice gets its control fields touched (claim-copy token check,
   * ProcessSamplerPreRender/PostRender: delay/time/stop/new_attack/
   * provider/windchest, the drop bookkeeping). With ~18k voices the
   * sampler pool's working set is several MB walked 375x/s - each
   * ADDITIONAL cache line per voice costs ~450 MB/s of L3/DRAM traffic
   * on the render's latency chain. Previously these scalars sat BEHIND
   * the big stream/fader/filter members and straddled extra lines; now
   * the whole control set fits the first two lines of the struct, and
   * the big members follow on their own lines. Pure reordering - the
   * pool memsets the whole struct and CloneFrom memcpys it, no layout
   * assumptions exist elsewhere. */
  const GOSoundProvider *p_SoundProvider;
  GOSoundWindchestTask *p_WindchestTask;
  int m_SamplerTaskId;
  unsigned m_AudioGroupId;
  uint64_t time;
  /* current index of the current block into this sample */
  volatile unsigned long stop;
  volatile unsigned long new_attack;
  uint64_t fade_out_start_time;
  unsigned velocity;
  unsigned delay;
  unsigned drop_counter;
  unsigned normal_drop_age_threshold;
  GOBool3 m_WaveTremulantStateFor;
  bool is_release;
  GOSoundPolyphonyDropState polyphony_drop_state;
  bool stale_release_logged;

  // --- big members on their own cache lines from here on ---
  GOSoundStream stream;
  GOSoundFader fader;
  GOSoundFilter::FilterState toneBalanceFilterState;
  /* (normal_drop_age_threshold, stale_release_logged and
   * fade_out_start_time moved into the hot control block above - their
   * documentation: normal_drop_age_threshold is the per-sampler
   * randomized age threshold for normal load dropping, 0 = not yet
   * picked; stale_release_logged and fade_out_start_time serve the
   * stale-release diagnostic in GOSoundOrganEngine::ProcessSampler.) */

  /* Clone helper (GOSoundOrganEngine::SwitchAttackSection): byte-copies
   * the whole sampler - the pool already treats it as memset-able - but
   * clears the clone's residence token: the clone is not a member of any
   * persistent bucket until it is Add()ed and stamped by a period window.
   * (The atomic member deletes the implicit copy assignment, which is
   * exactly the reminder that a plain copy would smuggle a live token
   * into a second sampler.) */
  void CloneFrom(const GOSoundSampler &src) {
    memcpy((void *)this, (const void *)&src, sizeof(GOSoundSampler));
    m_ResidenceToken.store(0, std::memory_order_relaxed);
  }
};

#endif /* GOSOUNDSAMPLER_H_ */
