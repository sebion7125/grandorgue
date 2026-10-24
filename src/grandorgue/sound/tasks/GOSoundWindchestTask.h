/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDWINDCHESTTASK_H
#define GOSOUNDWINDCHESTTASK_H

#include <atomic>

#include "model/GOWindchest.h"
#include "sound/scheduler/GOSoundTask.h"
#include "threading/GOMutex.h"

#include "ptrvector.h"

class GOSoundOrganEngine;
class GOSoundTremulantTask;
class GOWindchest;

class GOSoundWindchestTask : public GOSoundTask {
private:
  GOSoundOrganEngine &r_engine;
  // Lock-free volume (2026-07-25): deterministic value, benign init race - see
  // Run(). Atomic makes the concurrent store/load well-defined; the former
  // GOMutex became a per-period 20-worker thundering herd and was removed.
  std::atomic<float> m_volume;
  std::atomic_bool m_done;
  GOWindchest *p_windchest;
  std::vector<GOSoundTremulantTask *> m_pTremulantTasks;

public:
  GOSoundWindchestTask(
    GOSoundOrganEngine &sound_engine, GOWindchest *windchest);

  unsigned GetGroup() override { return WINDCHEST; }
  unsigned GetCost() override { return 0; }
  bool GetRepeat() override { return false; }
  void Run(GOSoundThread *pThread = nullptr) override;
  void Exec() override {}

  void Clear() override { Reset(); }
  void Reset() override;
  void Init(ptr_vector<GOSoundTremulantTask> &tremulantTasks);

  float GetWindchestVolume() const {
    return p_windchest ? p_windchest->GetVolume() : 1;
  }

  float GetVolume() {
    if (!m_done.load(std::memory_order_acquire))
      Run();
    return m_volume.load(std::memory_order_relaxed);
  }
};

#endif
