/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundThread.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h> // __rdtsc for the cycle-granular stagger spin
#endif

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h> // getrusage(RUSAGE_THREAD) - burst taxonomy
#include <time.h>         // clock_gettime(CLOCK_THREAD_CPUTIME_ID)
#endif

#include <wx/log.h>

#include "sound/GOFlushToZero.h"
#include "sound/scheduler/GOSoundTask.h"
#include "threading/GOMutexLocker.h"

#include "GOSoundScheduler.h"

GOSoundThread::GOSoundThread(GOSoundScheduler *scheduler, unsigned index)
  : GOThread(),
    m_Scheduler(scheduler),
    m_Condition(m_Mutex),
    m_IdleStateReachedCondition(m_Mutex),
    m_IsIdle(false),
    m_ElevationStatus(ElevationStatus::Pending),
    m_Index(index) {
  wxLogDebug(wxT("Create Thread"));
}

std::atomic<int> GOSoundThread::s_AffinityMode{0};
std::atomic<int> GOSoundThread::s_StaggerDelayUs{0};
std::atomic<uint32_t> GOSoundThread::s_WakeEpoch{0};
std::atomic<int64_t> GOSoundThread::s_BroadcastUs{0};
std::atomic<bool> GOSoundThread::s_DispatchCaptured{true};
std::atomic<uint64_t> GOSoundThread::s_DispatchLatUsSum{0};
std::atomic<int> GOSoundThread::s_WorkerInheritedCsr{-1};
std::atomic<int64_t> GOSoundThread::s_FirstDoneUs{INT64_MAX};
std::atomic<int64_t> GOSoundThread::s_LastDoneUs{0};
std::atomic<uint64_t> GOSoundThread::s_TailUsSum{0};
std::atomic<uint64_t> GOSoundThread::s_BurstWallUsSum{0};
std::atomic<uint64_t> GOSoundThread::s_BurstCpuUsSum{0};
std::atomic<uint64_t> GOSoundThread::s_BurstNivcswSum{0};
std::atomic<uint64_t> GOSoundThread::s_BurstCount{0};

static inline int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
           std::chrono::steady_clock::now().time_since_epoch())
    .count();
}

/* Shared wake object for the whole worker pool - see WakeAll(). Function-
 * local statics: constructed on first use, no init-order hazards. */
static GOMutex &wakeMutex() {
  static GOMutex m;
  return m;
}
static GOCondition &wakeCondition() {
  static GOCondition c(wakeMutex());
  return c;
}

void GOSoundThread::WakeAll() {
  {
    GOMutexLocker lock(wakeMutex(), false, "GOSoundThread::WakeAll");

    s_WakeEpoch.fetch_add(1, std::memory_order_release);
  }
  // Arm the dispatch-latency probe: stamp the broadcast, open the capture
  // for the first worker that resumes (see the worker wait loop).
  s_BroadcastUs.store(nowUs(), std::memory_order_relaxed);
  s_DispatchCaptured.store(false, std::memory_order_relaxed);
  // Arm the barrier-tail probe for this period (workers stamp their idle
  // transition; NextPeriod reads the spread before the next WakeAll).
  s_FirstDoneUs.store(INT64_MAX, std::memory_order_relaxed);
  s_LastDoneUs.store(0, std::memory_order_relaxed);
  /* Broadcast AFTER releasing the mutex: woken waiters must reacquire it
   * to leave wait_for - broadcasting while holding it made all of them
   * pile up against a held lock. Safe with the epoch predicate: a waiter
   * that checked before the bump is inside the wait and receives the
   * notify; one that checks after sees the new epoch and never waits. */
  wakeCondition().Broadcast();
}

void GOSoundThread::CapturePeriodTail() {
  const int64_t first = s_FirstDoneUs.load(std::memory_order_relaxed);
  const int64_t last = s_LastDoneUs.load(std::memory_order_relaxed);

  // Valid only if at least one worker stamped an idle transition this
  // period (first <= last). Skips periods where the pool never idled.
  if (first != INT64_MAX && last >= first)
    s_TailUsSum.fetch_add(
      (uint64_t)(last - first), std::memory_order_relaxed);
}

#ifdef _WIN32
static DWORD_PTR fullAffinityMask() {
  DWORD_PTR proc = 0, sys = 0;

  if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc)
    return proc;
  return ~(DWORD_PTR)0;
}
#endif

void GOSoundThread::ApplyWorkerAffinity(int mode) {
#ifdef _WIN32
  const DWORD_PTR full = fullAffinityMask();
  DWORD_PTR mask = full;

  if (mode == 1) {
    mask = full & ~(DWORD_PTR)0x3; // keep off logical CPUs 0+1 (core 0)
    if (!mask)
      mask = full; // <=2 logical CPUs: experiment not applicable
  } else if (mode == 2) {
    // 1:1 pin: worker k -> logical CPU k. With interleaved HT numbering
    // (physical core c owns logical 2c and 2c+1), workers 2c and 2c+1
    // become that core's two siblings, so the stagger puts a deterministic
    // per-core anti-phase with no migration to wash it out. Falls back to
    // the full mask if this worker's CPU is outside the process mask.
    const unsigned bits = 8u * (unsigned)sizeof(DWORD_PTR);
    const DWORD_PTR bit = (DWORD_PTR)1 << (m_Index % bits);

    if (full & bit)
      mask = bit;
  }
  SetThreadAffinityMask(GetCurrentThread(), mask);
#elif defined(__linux__)
  cpu_set_t set;
  const long n = sysconf(_SC_NPROCESSORS_ONLN);

  CPU_ZERO(&set);
  if (mode == 2 && n > 0) {
    // 1:1 pin, see the Windows branch.
    CPU_SET((long)(m_Index % (unsigned)n), &set);
  } else {
    for (long c = mode == 1 ? 2 : 0; c < n; c++)
      CPU_SET(c, &set);
    if (mode == 1 && n <= 2)
      for (long c = 0; c < n; c++)
        CPU_SET(c, &set);
  }
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
  (void)mode;
#endif
}

void GOSoundThread::ApplyAudioAffinity(int mode) {
#ifdef _WIN32
  SetThreadAffinityMask(
    GetCurrentThread(), mode == 1 ? (DWORD_PTR)0x1 : fullAffinityMask());
#elif defined(__linux__)
  cpu_set_t set;
  const long n = sysconf(_SC_NPROCESSORS_ONLN);

  CPU_ZERO(&set);
  if (mode == 1)
    CPU_SET(0, &set);
  else
    for (long c = 0; c < n; c++)
      CPU_SET(c, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
  (void)mode;
#endif
}

void GOSoundThread::Entry() {
  // Elevate this worker thread to the same scheduling class as the audio
  // callback thread (set by PortAudio/RtAudio), so the OS scheduler cannot
  // preempt a worker mid-computation while the audio thread waits in Finish().
  //
  // Windows: MMCSS "Pro Audio" — the same class the callback thread is in.
  //   avrt.dll is always present on Vista+. Falls back to THREAD_PRIORITY_
  //   HIGHEST if AvSetMmThreadCharacteristicsW unexpectedly fails.
  //   MMCSS reserves ~20% CPU for non-RT tasks (SystemResponsiveness), so
  //   even at 100% audio load Windows stays responsive and does not lock up.
  //
  // Linux: attempts SCHED_FIFO priority 60 via pthread_setschedparam.
  //   Requires either CAP_SYS_NICE (root) or an rtprio limit granted by
  //   /etc/security/limits.d/audio.conf / RTKIT. Silently ignored on
  //   permission failure — the thread just runs at normal priority.
#ifdef _WIN32
  typedef HANDLE (WINAPI *FAvSet)(LPCWSTR, LPDWORD);
  typedef BOOL   (WINAPI *FAvRevert)(HANDLE);

  HANDLE    hAvrt    = LoadLibraryW(L"avrt.dll");
  FAvSet    pAvSet   = hAvrt
    ? (FAvSet)GetProcAddress((HMODULE)hAvrt, "AvSetMmThreadCharacteristicsW")
    : NULL;
  FAvRevert pAvRevert = hAvrt
    ? (FAvRevert)GetProcAddress((HMODULE)hAvrt, "AvRevertMmThreadCharacteristics")
    : NULL;

  HANDLE hMmcssTask = NULL;
  if (pAvSet) {
    DWORD taskIndex = 0;
    hMmcssTask = pAvSet(L"Pro Audio", &taskIndex);
  }
  if (hMmcssTask) {
    m_ElevationStatus.store(
      ElevationStatus::MmcssSucceeded, std::memory_order_relaxed);
  } else {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    m_ElevationStatus.store(
      ElevationStatus::FallbackHighest, std::memory_order_relaxed);
  }
  // Diagnostic: does "Pro Audio" actually land this worker at the same
  // GetThreadPriority() reading as the audio backend's own callback thread
  // (GOSoundOrganEngine::m_AudioThreadSchedInfo)? Never previously checked.
  m_WorkerThreadPriority.store(
    GetThreadPriority(GetCurrentThread()), std::memory_order_relaxed);

#elif defined(__APPLE__)
  // QOS_CLASS_USER_INTERACTIVE is the highest QoS class on macOS (10.10+)
  // and maps to the same scheduling bucket CoreAudio uses for its callback
  // thread. No special entitlements or root required. The Mach time-
  // constraint policy would be more precise but needs the buffer period
  // which is not known at thread-start time.
  m_ElevationStatus.store(
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0
      ? ElevationStatus::QosSucceeded
      : ElevationStatus::NotElevated,
    std::memory_order_relaxed);

#elif defined(__linux__)
  {
    struct sched_param sp = {};
    sp.sched_priority = 60;
    m_ElevationStatus.store(
      pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0
        ? ElevationStatus::SchedFifoSucceeded
        : ElevationStatus::NotElevated,
      std::memory_order_relaxed);
  }
#else
  m_ElevationStatus.store(
    ElevationStatus::NotElevated, std::memory_order_relaxed);
#endif

  /* Denormal handling on THIS worker thread. First capture the MXCSR the
   * OS handed us (diagnostic: shows whether the platform already flushes
   * denormals on worker threads or left them enabled - the whole point of
   * this measurement). Then enable FTZ/DAZ, UNLESS GO_KEEP_DENORMALS is set
   * in the environment - that lets us A/B the fix by running with the
   * inherited mode instead. -ffast-math only set FTZ on the initial thread;
   * pthreads do not inherit it, so without the fix the DSP here runs slow on
   * denormals (release tails, fades). */
  {
    int expected = -1;

    s_WorkerInheritedCsr.compare_exchange_strong(
      expected, (int)GOReadMxcsr(), std::memory_order_relaxed);
  }
  if (!getenv("GO_KEEP_DENORMALS"))
    GOEnableFlushToZero();

  int appliedAffinity = -1;

  while (!ShouldStop()) {
    bool shouldStop = false;
    const int affinityMode = s_AffinityMode.load(std::memory_order_relaxed);

    if (affinityMode != appliedAffinity) {
      ApplyWorkerAffinity(affinityMode);
      appliedAffinity = affinityMode;
      // Fresh migration count for the new regime (drops the pre-pin period).
      m_CpuProbe.Reset();
    }
    // Once per wakeup = once per period while audio runs: enough cadence
    // to catch scheduler migrations without touching the task loop.
    m_CpuProbe.Sample();

    bool didWork = false;
#ifdef __linux__
    /* Burst taxonomy: sample wall clock, this thread's own CPU clock and
     * the involuntary-context-switch count around the render burst. The
     * difference wall - cpu is time this worker sat preempted/runnable
     * MID-BURST (scheduling, IRQ, SMI); nivcsw counts the evictions.
     * Two vDSO/fast syscalls per burst (~1 us at 375 bursts/s) - far off
     * the render path's own cost. */
    struct timespec burstCpu0 = {};
    struct rusage burstRu0 = {};
    const int64_t burstWall0 = nowUs();

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &burstCpu0);
    getrusage(RUSAGE_THREAD, &burstRu0);
#endif

    do {
      // Track the wake epoch for the predicated wait below (the cascade
      // that once lived here was reverted - see WakeupThreads).
      m_SeenEpoch = s_WakeEpoch.load(std::memory_order_acquire);

      GOSoundTask *next = m_Scheduler->GetNextGroup();

      if (next == NULL)
        break;
      didWork = true;
      next->Run(this);
      shouldStop = ShouldStop();
    } while (!shouldStop);

#ifdef __linux__
    if (didWork) {
      struct timespec burstCpu1;
      struct rusage burstRu1;

      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &burstCpu1);
      getrusage(RUSAGE_THREAD, &burstRu1);

      const int64_t wallUs = nowUs() - burstWall0;
      const int64_t cpuUs = (burstCpu1.tv_sec - burstCpu0.tv_sec) * 1000000LL
        + (burstCpu1.tv_nsec - burstCpu0.tv_nsec) / 1000;
      const long nivcsw = burstRu1.ru_nivcsw - burstRu0.ru_nivcsw;

      if (wallUs >= 0) {
        s_BurstWallUsSum.fetch_add(
          (uint64_t)wallUs, std::memory_order_relaxed);
        s_BurstCpuUsSum.fetch_add(
          (uint64_t)(cpuUs > 0 ? cpuUs : 0), std::memory_order_relaxed);
        s_BurstNivcswSum.fetch_add(
          (uint64_t)(nivcsw > 0 ? nivcsw : 0), std::memory_order_relaxed);
        s_BurstCount.fetch_add(1, std::memory_order_relaxed);
      }
    }
#else
    (void)didWork;
#endif

    if (shouldStop)
      break;

    /* Barrier-tail probe: this worker just drained all render work for the
     * period. Stamp its finish time into the per-period min (first done)
     * and max (last done); the spread is the straggler tail. Atomic CAS
     * min/max, relaxed - a pure diagnostic, off the task path. */
    {
      const int64_t done = nowUs();
      int64_t f = s_FirstDoneUs.load(std::memory_order_relaxed);

      while (done < f
             && !s_FirstDoneUs.compare_exchange_weak(
               f, done, std::memory_order_relaxed))
        ;
      int64_t l = s_LastDoneUs.load(std::memory_order_relaxed);
      while (done > l
             && !s_LastDoneUs.compare_exchange_weak(
               l, done, std::memory_order_relaxed))
        ;
    }

    {
      GOMutexLocker lock(m_Mutex, false, "GOSoundThread::Entry", this);

      if (!lock.IsLocked() || ShouldStop())
        break;
      m_IsIdle = true;
      m_IdleStateReachedCondition.Broadcast();
    }

    /* Predicated wait with WATCHDOG on the SHARED wake condition (one
     * Broadcast wakes the whole pool - see WakeAll). The epoch is
     * checked under the shared mutex and bumped+broadcast under the
     * same mutex, so no wake can be lost; a theoretically dropped one
     * self-heals after one THREADING_WAIT_TIMEOUT (user's auto-wake
     * fallback idea). */
    {
      GOMutexLocker wlock(wakeMutex(), false, "GOSoundThread::EntryWake");

      if (wlock.IsLocked())
        while (s_WakeEpoch.load(std::memory_order_acquire) == m_SeenEpoch
               && !ShouldStop())
          wakeCondition().WaitOneOrTimeout("GOSoundThread::Entry");
    }
    /* This worker just resumed from its wait: if it is the first to do so
     * after this period's broadcast, record the dispatch latency (how long
     * the scheduler took to put a woken worker back on a core). One capture
     * per broadcast via the CAS flag. */
    {
      bool expected = false;

      if (s_DispatchCaptured.compare_exchange_strong(
            expected, true, std::memory_order_relaxed)) {
        const int64_t bc = s_BroadcastUs.load(std::memory_order_relaxed);
        const int64_t lat = nowUs() - bc;

        if (bc != 0 && lat >= 0)
          s_DispatchLatUsSum.fetch_add(
            (uint64_t)lat, std::memory_order_relaxed);
      }
    }
    /* Stagger experiment (see s_StaggerDelayUs): odd-indexed workers hold
     * off the start of this period's work by the configured microseconds,
     * phase-offsetting half the pool against the other half. Local
     * busy-wait on the same steady clock the rest of the file uses - no
     * syscall, no extra broadcast. 0 (default) skips it entirely, so the
     * wake stays bit-identical to the lockstep path. */
    {
      // CYCLE-granular staircase (this build): the step is now in TSC ticks,
      // not us. Worker k busy-waits k*step ticks (rdtsc) before its burst -
      // ~0.27 ns/tick at 3.7 GHz nominal, far finer than the us knob, to
      // probe whether a narrow cycle-scale offset the us stepping jumped
      // over does anything. 0 = lockstep.
      const int step = s_StaggerDelayUs.load(std::memory_order_relaxed);
      const uint64_t delayTicks = (uint64_t)m_Index * (unsigned)(step > 0 ? step : 0);

      if (delayTicks > 0 && !ShouldStop()) {
#if defined(__x86_64__) || defined(__i386__)
        const uint64_t until = __rdtsc() + delayTicks;

        while (__rdtsc() < until && !ShouldStop()) {
          // spin
        }
#endif
      }
    }
    {
      GOMutexLocker lock(m_Mutex, false, "GOSoundThread::EntryUnidle", this);

      m_IsIdle = false;
    }
    if (ShouldStop())
      break;
  }

#ifdef _WIN32
  if (pAvRevert && hMmcssTask)
    pAvRevert(hMmcssTask);
  if (hAvrt)
    FreeLibrary((HMODULE)hAvrt);
#endif

  return;
}

void GOSoundThread::WaitForIdle() {
  GOMutexLocker lock(m_Mutex, false, "GOSoundThread::WaitForIdle");
  while (!m_IsIdle) {
    m_IdleStateReachedCondition.Wait();
  }
}

void GOSoundThread::Run() { Start(); }

void GOSoundThread::Wakeup() {
  /* Individual wakes (Delete/shutdown) go through the shared broadcast:
   * uninvolved workers see an unchanged... epoch CHANGED here so every
   * waiter re-checks its predicates (ShouldStop for the one being
   * stopped); the others find work or go back to sleep. Rare path. */
  WakeAll();
}

void GOSoundThread::Delete() {
  MarkForStop();
  Wakeup();
  Wait();
}
