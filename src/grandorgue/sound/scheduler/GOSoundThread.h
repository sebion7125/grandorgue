/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2023 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDTHREAD_H
#define GOSOUNDTHREAD_H

#include <atomic>

#include "threading/GOCondition.h"
#include "threading/GOCpuPlacementProbe.h"
#include "threading/GOMutex.h"
#include "threading/GOThread.h"

class GOSoundScheduler;

class GOSoundThread : public GOThread {
public:
  /* Runtime CPU-affinity experiment (2026-07-12, Audio Monitor toggle):
   * 0 = OS default. 1 = pin the audio callback thread to logical CPU 0
   * and keep the workers off logical CPUs 0/1 (core 0 and its HT
   * sibling on the usual Intel numbering) - gives the TIME_CRITICAL
   * thread an exclusive physical core, trading one worker CPU for
   * lower callback jitter. Applied lazily: workers check on every
   * wakeup, the audio thread in NextPeriod(). */
  static std::atomic<int> s_AffinityMode;

  /* Stagger experiment (2026-07-24): STAIRCASE desync, CYCLE-granular in
   * this build - the step is TSC TICKS, not us (name kept for churn). After
   * each wake, worker k busy-spins (k * step) rdtsc ticks (~0.27 ns/tick at
   * 3.7 GHz nominal) before its burst, fanning the pool out in time far
   * finer than the us knob could - to probe a narrow cycle-scale offset the
   * us stepping (~5000 ticks/us) jumped clean over. If a non-zero step
   * raises IPC / lowers denied (or, below the deadline, lowers UTIL =
   * regained headroom), it is a live knob. 0 = bit-identical to lockstep.
   * Local busy-wait, no syscall. NOTE: worker (N-1) waits (N-1)*step
   * ticks. */
  static std::atomic<int> s_StaggerDelayUs;

  /* Wake epoch (2026-07-12): drives the workers' PREDICATED wait - the
   * wait sleeps only while no new epoch arrived, checked under the
   * thread's own mutex, and Wakeup() signals under that mutex. This
   * closes the old lost-wakeup window (a signal could slip past a
   * worker between finishing work and entering the wait, costing a full
   * period), and the timed-wait watchdog (WaitOneOrTimeout) self-heals
   * even a theoretically lost signal. NOTE: a wake CASCADE (workers
   * forwarding to heap children, engine waking only the roots) was
   * tried and reverted the same day - delegating wakes to
   * normal-priority workers added scheduling-latency variance: leaf
   * workers started late and jittery, causing real gaps at the edge.
   * The engine's direct 20-signal loop on the TIME_CRITICAL thread
   * (~20 us, deterministic) is the better trade. */
  static std::atomic<uint32_t> s_WakeEpoch;

  /* Single-call wake (2026-07-13): all workers wait on ONE shared
   * condition, so waking the pool is a single Broadcast (notify_all) -
   * the KERNEL fans out to all 20 threads simultaneously with their
   * priorities intact. This replaces 20 serial per-thread Signal()
   * syscalls (~20 us measured on the TIME_CRITICAL thread) without the
   * reverted cascade's flaw (delegating wakes to normal-priority
   * workers added scheduling variance per hop). The epoch is bumped and
   * broadcast under the shared mutex; waiters check it under the same
   * mutex -> no lost wakeups; the timed-wait watchdog stays. */
  static void WakeAll();

  /* Dispatch-latency probe (diagnostics): the gap between issuing the
   * period broadcast (WakeAll) and the FIRST worker actually resuming
   * from its wait. This is the scheduler dispatch cost - invisible in
   * the "wake" number (which only times the broadcast call itself). One
   * capture per broadcast; accumulated us for the report's per-second
   * rate. If no worker was sleeping (pool never idled between periods),
   * nothing is captured for that period. */
  static std::atomic<int64_t> s_BroadcastUs;
  static std::atomic<bool> s_DispatchCaptured;
  static std::atomic<uint64_t> s_DispatchLatUsSum;
  static uint64_t GetDispatchLatUsSum() {
    return s_DispatchLatUsSum.load(std::memory_order_relaxed);
  }

  /* Diagnostic: the raw MXCSR the first worker INHERITED from the OS at
   * thread start, captured before GO sets FTZ/DAZ. Reveals whether the
   * platform already flushed denormals on worker threads (bit 0x8000=FTZ,
   * 0x0040=DAZ) or left them enabled. -1 = not captured yet. */
  static std::atomic<int> s_WorkerInheritedCsr;
  static int GetWorkerInheritedCsr() {
    return s_WorkerInheritedCsr.load(std::memory_order_relaxed);
  }

  /* Barrier-tail probe (diagnostics): the spread between the FIRST worker
   * finishing its render work for a period and the LAST one. A period only
   * completes when the slowest worker is done, so a large tail = stragglers
   * (one worker preempted or load-imbalanced) inflating wall-clock voice
   * cost without extra compute. Each worker stamps its idle transition;
   * CapturePeriodTail() (called from NextPeriod, before WakeAll resets)
   * accumulates last-first into the per-second sum. */
  static std::atomic<int64_t> s_FirstDoneUs;
  static std::atomic<int64_t> s_LastDoneUs;
  static std::atomic<uint64_t> s_TailUsSum;
  static void CapturePeriodTail();
  static uint64_t GetTailUsSum() {
    return s_TailUsSum.load(std::memory_order_relaxed);
  }

  /* Burst taxonomy (diagnostics, Linux-only): per render burst each worker
   * samples wall time, its own thread-CPU time (CLOCK_THREAD_CPUTIME_ID)
   * and the involuntary context-switch count (ru_nivcsw). wall - cpu is
   * the time the worker sat runnable-but-preempted MID-BURST; nivcsw is
   * how often it was thrown off its core. This splits the elevated
   * wall-clock voice cost into "workers preempted" (scheduling/IRQ/SMI
   * interference) vs "workers computing slowly" (memory/frequency) - the
   * two are indistinguishable in the existing wall-only metrics. Elsewhere
   * the counters stay zero and the report line is suppressed. */
  static std::atomic<uint64_t> s_BurstWallUsSum;
  static std::atomic<uint64_t> s_BurstCpuUsSum;
  static std::atomic<uint64_t> s_BurstNivcswSum;
  static std::atomic<uint64_t> s_BurstCount;
  static uint64_t GetBurstWallUsSum() {
    return s_BurstWallUsSum.load(std::memory_order_relaxed);
  }
  static uint64_t GetBurstCpuUsSum() {
    return s_BurstCpuUsSum.load(std::memory_order_relaxed);
  }
  static uint64_t GetBurstNivcswSum() {
    return s_BurstNivcswSum.load(std::memory_order_relaxed);
  }
  static uint64_t GetBurstCount() {
    return s_BurstCount.load(std::memory_order_relaxed);
  }

  // Applies the WORKER mask for the current mode to the calling thread.
  // Non-static: mode 2 (1:1 pin) needs this worker's m_Index.
  void ApplyWorkerAffinity(int mode);
  // Applies the AUDIO-thread mask for the current mode (call on the
  // audio thread itself).
  static void ApplyAudioAffinity(int mode);

public:
  // Result of the scheduling-elevation attempt made at thread start.
  // All threads on a given system will produce the same result, so
  // querying any one thread is representative of the pool as a whole.
  enum class ElevationStatus : uint8_t {
    Pending,            // Entry() hasn't run yet
    MmcssSucceeded,     // Windows: AvSetMmThreadCharacteristicsW ok
    FallbackHighest,    // Windows: MMCSS unavailable, THREAD_PRIORITY_HIGHEST set
    QosSucceeded,       // macOS: pthread_set_qos_class_self_np ok
    SchedFifoSucceeded, // Linux: pthread_setschedparam(SCHED_FIFO) ok
    NotElevated,        // all attempts failed or no elevation code for platform
  };
  ElevationStatus GetElevationStatus() const {
    return m_ElevationStatus.load(std::memory_order_relaxed);
  }

  // Raw GetThreadPriority() value captured right after the elevation
  // attempt in Entry() (Windows only; -1000 elsewhere/not yet run). Lets
  // the Audio Monitor report compare a worker's actual priority against the
  // callback thread's own GetThreadPriority() reading (see
  // GOSoundOrganEngine::m_AudioThreadSchedInfo) - MMCSS's "Pro Audio" class
  // is not necessarily the same numeric level the audio backend's own
  // callback thread gets, so this was never actually verified before.
  int GetWorkerThreadPriority() const {
    return m_WorkerThreadPriority.load(std::memory_order_relaxed);
  }

  // Passive CPU-placement observation, sampled once per wakeup in Entry()
  // (diagnostics report; never changes affinity).
  const GOCpuPlacementProbe &GetCpuProbe() const { return m_CpuProbe; }

private:
  GOCpuPlacementProbe m_CpuProbe;
  std::atomic<ElevationStatus> m_ElevationStatus;
  std::atomic<int> m_WorkerThreadPriority{-1000};
  GOSoundScheduler *m_Scheduler;

  GOMutex m_Mutex;
  GOCondition m_Condition;
  GOCondition m_IdleStateReachedCondition;
  // whether the thread sleeps and waits for waking up with m_Condition
  bool m_IsIdle; // guarded by m_Mutex
  uint32_t m_SeenEpoch = 0; // only touched by this thread
  unsigned m_Index = 0;     // pool index; parity selects the stagger group

  void Entry();

public:
  GOSoundThread(GOSoundScheduler *scheduler, unsigned index);

  // Stagger experiment knob (see s_StaggerDelayUs). Live-settable from the
  // audio monitor; 0 restores the lockstep wake.
  static void SetStaggerDelayUs(int us) {
    s_StaggerDelayUs.store(us, std::memory_order_relaxed);
  }
  static int GetStaggerDelayUs() {
    return s_StaggerDelayUs.load(std::memory_order_relaxed);
  }

  // Worker affinity mode: 0 = free (scheduler decides), 1 = keep off core 0,
  // 2 = 1:1 pin (worker k -> logical CPU k, no migration; for the HT-sibling
  // stagger test). Applied lazily by each worker on the next wake.
  static void SetAffinityMode(int m) {
    s_AffinityMode.store(m, std::memory_order_relaxed);
  }
  static int GetAffinityMode() {
    return s_AffinityMode.load(std::memory_order_relaxed);
  }

  /*
   * === Prerequisites ===
   * During the execution the following must be true:
   * 1. m_Scheduler->GetNextGroup() always returns nullptr
   * 2. thread is running and is not marked to be stopped
   *
   * === Result ===
   * Method returns when the thread is idle (i.e. does not run any work items)
   * (so that work items can be deleted safely)
   */
  void WaitForIdle();
  void Run();
  void Delete();
  void Wakeup();
};

#endif
