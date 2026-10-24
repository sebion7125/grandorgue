/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOCPUPLACEMENTPROBE_H
#define GOCPUPLACEMENTPROBE_H

#include <atomic>
#include <cstdint>

/* Passive observation of which logical CPU a thread runs on: the owning
 * thread calls Sample() at a natural cadence (a worker per wakeup, the
 * audio thread per period) and any other thread may read the counters
 * (diagnostics report). The migration count tells whether the scheduler
 * keeps the thread in place or shuffles it; the last-seen CPU gives the
 * placement snapshot. Observation only - never changes affinity. */
class GOCpuPlacementProbe {
private:
  std::atomic<int> m_LastCpu{-1};
  std::atomic<uint32_t> m_Migrations{0};
  std::atomic<uint32_t> m_Samples{0};

public:
  // Logical CPU index of the calling thread, -1 if not queryable.
  static int GetCurrentCpu();

  // Called by the owning thread only.
  void Sample() {
    const int cpu = GetCurrentCpu();
    const int last = m_LastCpu.load(std::memory_order_relaxed);

    if (cpu != last) {
      m_LastCpu.store(cpu, std::memory_order_relaxed);
      if (last >= 0)
        m_Migrations.fetch_add(1, std::memory_order_relaxed);
    }
    m_Samples.fetch_add(1, std::memory_order_relaxed);
  }

  // Clear the counters (called by the owning thread when its affinity mode
  // changes, so the migration reading reflects only the new regime instead
  // of averaging the pre-pin period in).
  void Reset() {
    m_Migrations.store(0, std::memory_order_relaxed);
    m_Samples.store(0, std::memory_order_relaxed);
    m_LastCpu.store(-1, std::memory_order_relaxed);
  }

  int GetLastCpu() const { return m_LastCpu.load(std::memory_order_relaxed); }
  uint32_t GetMigrations() const {
    return m_Migrations.load(std::memory_order_relaxed);
  }
  uint32_t GetSamples() const {
    return m_Samples.load(std::memory_order_relaxed);
  }
};

#endif /* GOCPUPLACEMENTPROBE_H */
