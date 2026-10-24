/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundReleaseTask.h"

#include "GOSoundGroupTask.h"
#include "sound/GOSoundOrganEngine.h"

GOSoundReleaseTask::GOSoundReleaseTask(
  GOSoundOrganEngine &sound_engine, ptr_vector<GOSoundGroupTask> &audio_groups)
  : m_engine(sound_engine), m_AudioGroups(audio_groups), m_Stop(false) {}

unsigned GOSoundReleaseTask::GetGroup() { return RELEASE; }

unsigned GOSoundReleaseTask::GetCost() { return 0; }

bool GOSoundReleaseTask::GetRepeat() { return true; }

void GOSoundReleaseTask::Clear() { m_List.Clear(); }

void GOSoundReleaseTask::Reset() {
  m_Stop.store(false);
  m_Cnt.store(0);
}

void GOSoundReleaseTask::Add(GOSoundSampler *sampler) { m_List.Put(sampler); }

void GOSoundReleaseTask::Run(GOSoundThread *pThread) {
  GOSoundSampler *sampler;
  do {
    while ((sampler = m_List.Get())) {
      m_Cnt.fetch_add(1);
      m_engine.ProcessRelease(sampler);
      if (m_Stop.load() && m_Cnt > 10)
        break;
    }
    /* Ordered walk with a single pre-park jump. Two failed designs frame
     * this one: the ORIGINAL strictly ordered walk parked workers on
     * group k's condition while groups k+1..n still had unclaimed work
     * (2026-07-10 wait profile: 6-7 of 20 workers parked that way); the
     * UNORDERED help-everywhere replacement fixed that but destroyed
     * cache locality by making workers hop between the groups' flat
     * arrays constantly (same voice throughput at +17% cycles, L3 hit
     * rate down, prefetch-hint cost doubled). So: stay loyal to the
     * first unfinished group as long as it has claimable work - and
     * only when the alternative would be SLEEPING, take one look at the
     * remaining groups and jump to the first one that has work. The
     * jump is the exception (at most a couple per worker per period),
     * not the steady state. */
    GOSoundGroupTask *pPrimary = nullptr;

    for (unsigned i = 0; i < m_AudioGroups.size(); i++) {
      GOSoundGroupTask *const pGroup = m_AudioGroups[i];

      if (pGroup && !pGroup->IsPeriodDone()) {
        pPrimary = pGroup;
        break;
      }
    }
    if (!pPrimary)
      break; // every audio group has completed this period
    if (pPrimary->HasClaimableWork()) {
      pPrimary->Run(pThread);
      continue;
    }

    // The primary group is claimed-out but still in flight: one scan for
    // claimable work elsewhere before parking on the primary's condition
    // (its completion broadcast re-runs this loop from the top).
    GOSoundGroupTask *pJump = nullptr;

    for (unsigned i = 0; i < m_AudioGroups.size(); i++) {
      GOSoundGroupTask *const pGroup = m_AudioGroups[i];

      if (
        pGroup && pGroup != pPrimary && !pGroup->IsPeriodDone()
        && pGroup->HasClaimableWork()) {
        pJump = pGroup;
        break;
      }
    }
    if (pJump)
      pJump->Run(pThread);
    else
      pPrimary->Finish(false, pThread);
  } while (
    !m_Stop.load() && (pThread == nullptr || !pThread->ShouldStop()));
}

void GOSoundReleaseTask::Exec() {
  m_Stop.store(true);
  Run();
  GOSoundSampler *sampler;
  while ((sampler = m_List.Get()))
    m_engine.PassSampler(sampler);
}
