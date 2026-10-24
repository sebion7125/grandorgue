/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundWindchestTask.h"

#include "sound/GOSoundOrganEngine.h"
#include "threading/GOMutexLocker.h"

#include "GOSoundTremulantTask.h"

GOSoundWindchestTask::GOSoundWindchestTask(
  GOSoundOrganEngine &soundEngine, GOWindchest *pWindchest)
  : r_engine(soundEngine),
    m_volume(0),
    m_done(false),
    p_windchest(pWindchest) {}

void GOSoundWindchestTask::Init(
  ptr_vector<GOSoundTremulantTask> &tremulantTasks) {
  m_pTremulantTasks.clear();
  if (p_windchest)
    for (unsigned i = 0; i < p_windchest->GetTremulantCount(); i++)
      m_pTremulantTasks.push_back(
        tremulantTasks[p_windchest->GetTremulantId(i)]);
}

void GOSoundWindchestTask::Reset() {
  // Runs single-threaded at period start (Scheduler::Reset), before any voice
  // reads the volume - no lock needed.
  m_done.store(false, std::memory_order_release);
}

void GOSoundWindchestTask::Run(GOSoundThread *pThread) {
  /* Lock-free (2026-07-25). The windchest volume is a trivial, deterministic
   * product (gain x windchest x tremulants). The old lazy double-checked lock
   * made all 20 render workers stampede this one GOMutex on the first voice
   * touch of the period (measured ~1.6 s / 5 s in __lll_lock_wait via
   * offcputime - the entire Linux @128 render tail; on Windows the mutex spins
   * so it never showed). No lock is needed: if several workers race the init
   * they all compute the SAME value; the atomic store + release flag make it
   * well-defined and the last writer simply wins. */
  if (!m_done.load(std::memory_order_acquire)) {
    float volume = r_engine.GetGain();

    if (p_windchest) {
      volume *= p_windchest->GetVolume();
      for (unsigned i = 0; i < m_pTremulantTasks.size(); i++)
        volume *= m_pTremulantTasks[i]->GetVolume();
    }
    m_volume.store(volume, std::memory_order_relaxed);
    m_done.store(true, std::memory_order_release);
  }
}
