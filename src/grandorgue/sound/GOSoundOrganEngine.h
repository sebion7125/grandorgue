/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDORGANENGINE_H
#define GOSOUNDORGANENGINE_H

#include <atomic>
#include <memory>
#include <vector>

#include "ptrvector.h"
#include "threading/GOCpuPlacementProbe.h"
#include "threading/GOMutex.h"

#include "playing/GOSoundResample.h"
#include "playing/GOSoundSampler.h"
#include "playing/GOSoundSamplerPool.h"
#include "scheduler/GOSoundScheduler.h"

#include "GOSoundOrganInterface.h"

class GOConfig;
class GOMemoryPool;
class GOOrganModel;
class GOSoundBufferMutable;
class GOSoundProvider;
class GOSoundRecorder;
class GOSoundGroupTask;
class GOSoundOutputTask;
class GOSoundReleaseTask;
#include "scheduler/GOSoundThread.h"
class GOSoundTouchTask;
class GOSoundTremulantTask;
class GOSoundWindchestTask;
class GOSoundTask;
class GOWindchest;

typedef struct {
  unsigned channels;
  std::vector<std::vector<float>> scale_factors;
} GOAudioOutputConfiguration;

/**
 * This class represents a sound engine of one loaded organ. It reflects some
 * GrandOrgue-wide objects (AudioGroup) and some objects of it's model
 * (WindChests, Tremulants)
 */

class GOSoundOrganEngine : public GOSoundOrganInterface {
private:
  static constexpr int DETACHED_RELEASE_TASK_ID = 0;

  unsigned m_PolyphonySoftLimit;
  bool m_PolyphonyLimiting;
  bool m_ScaledReleases;
  bool m_ReleaseAlignmentEnabled;
  bool m_RandomizeSpeaking;
  int m_Volume;
  unsigned m_SamplesPerBuffer;
  float m_Gain;
  unsigned m_SampleRate;

  // time in samples
  uint64_t m_CurrentTime;
  GOSoundSamplerPool m_SamplerPool;
  unsigned m_AudioGroupCount;
  std::atomic_uint m_UsedPolyphony;

  // Separate from m_UsedPolyphony - see GetAndResetUsedPolyphonyPeak() for
  // why a diagnostics graph needs its own atomic rather than peeking at the
  // toolbar meter's.
  std::atomic_uint m_UsedPolyphonyWindowPeak;

  // Polyphony emergency-drop counters, for an "Audio Diagnostics
  // Report"/graph - how often ProcessSampler() had to force-fade a release
  // sampler early because the polyphony soft limit was exceeded. Cumulative
  // for the engine's lifetime (not reset by reading them), unlike
  // m_UsedPolyphony above which is a per-meter-poll peak.
  std::atomic<uint64_t> m_PolyphonyPanicDrops;
  std::atomic<uint64_t> m_PolyphonyNormalDrops;
  // Windowed drop counter for the live graph: total drops (panic + normal)
  // since the last GetAndResetPolyphonyDropCount() call. Separate from the
  // cumulative counters above so the graph can mark exact poll intervals
  // without consuming the lifetime totals.
  std::atomic<uint64_t> m_PolyphonyDropsWindow;

  // Age (ms) after which a still-sounding release sampler gets logged once
  // as a diagnostic (see stale_release_logged in GOSoundSampler.h).
  static constexpr unsigned kStaleReleaseAgeMs = 5000;

  // Leak/stuck-voice diagnostics (see "silent voices never end" bug hunt).
  // Every ProcessSampler() call increments this once, regardless of outcome.
  // Since every currently-reachable sampler (active or release, across all
  // group/tremulant tasks) gets exactly one ProcessSampler() call per period,
  // the count accumulated over one period is a proxy for "how many samplers
  // were actually reachable from the processing lists" during that period.
  // Compared against m_SamplerPool.UsedSamplerCount() in NextPeriod(): a
  // sustained (non-recovering) gap where the pool believes more samplers are
  // in use than are ever reachable from the lists indicates samplers were
  // orphaned - still counted by the pool, never returned, but no longer
  // processed by anything.
  std::atomic<uint64_t> m_ProcessSamplerCallCount;
  // Counts of ProcessSampler calls in the current GUI-poll window:
  // fast-path (identity, no FIR) and total. Both are reset together by
  // GetAndResetFastPathStats() so the ratio is always self-consistent.
  std::atomic<unsigned> m_FastPathVoicesWindow{0};
  std::atomic<unsigned> m_TotalVoicesWindow{0};
  // Voice-mix histogram for the same GUI-poll window (Audio Monitor):
  // which fader kernel each fused voice call took, indexed by
  // GOFadeKernelKind (0=constant, 1=ramp, 2=active non-linear crossfade),
  // plus index 3 for the unfused legacy path (ToneBalance filter active or
  // fuse disabled). And how many voice calls read from a compressed section.
  static constexpr unsigned kFaderKernelLegacyIdx = 3;
  std::atomic<unsigned> m_FaderKernelWindow[4]{};
  std::atomic<unsigned> m_CompressedVoicesWindow{0};
  // Master switch for the per-voice-call window counters above (fast-path,
  // total, compressed, fader kernel). Set by the Audio Monitor while it is
  // open. When false, ProcessSampler skips all of them - each is an atomic
  // read-modify-write (a lock-prefixed add, ~20 cycles + memory-barrier
  // semantics), and at millions of voice calls per second four of those
  // add up to a measurable ~1-2% machine load; a single relaxed *load* of
  // this flag plus a predicted branch is effectively free in comparison.
  // m_ProcessSamplerCallCount is NOT gated: the orphan-leak detection in
  // NextPeriod() depends on it and must stay armed without the monitor.
  std::atomic<bool> m_VoiceStatsEnabled{false};
  // Consecutive periods (checked in NextPeriod()) where UsedSamplerCount()
  // exceeded the previous period's m_ProcessSamplerCallCount by more than
  // kOrphanGapTolerance. Reset to 0 whenever the gap disappears - a real
  // leak never recovers, so this only matters once it stays elevated.
  unsigned m_OrphanSuspectStreak = 0;
  // Set once a sustained gap has been logged, so we don't spam the log every
  // period afterwards. Latches until Reset(). Audio-thread-only (NextPeriod).
  bool m_OrphanLeakLogged = false;
  // GUI-visible mirror of m_OrphanLeakLogged, for the Audio Monitor dialog.
  std::atomic<bool> m_OrphanLeakDetected{false};
  // Snapshot of the last period's m_ProcessSamplerCallCount, for the Audio
  // Monitor dialog to display alongside UsedSamplerCount() live.
  std::atomic<uint64_t> m_LastProcessedSamplerCount{0};

  // Monotonic-clock timestamp (us) of the last WakeupThreads() call, i.e.
  // when this period's work was made available to the worker thread pool.
  // m_LongestPeriodCompletionUs is the longest "from there until every
  // GOSoundOutputTask is actually done" span seen since the last
  // GetAndResetPeriodCompletionPeakUs() call - the *total* time this
  // period's computation took, regardless of whether that was background
  // workers finishing in parallel or the realtime audio thread joining in
  // via Finish(). Continuous and load-correlated, unlike GOSoundSystem's
  // render-time diagnostics (which read ~0 whenever the worker pool
  // finishes before the audio thread even asks) - see NextPeriod().
  std::atomic<int64_t> m_PeriodStartUs;
  // See Get/SetCallbackHelpPolicy.
  std::atomic<int> m_CallbackHelpPolicy{0};
  std::atomic<int64_t> m_LongestPeriodCompletionUs;

  // Callback interarrival jitter counters - see the getters below.
  std::atomic<int64_t> m_LastCbUs{0};
  std::atomic<int64_t> m_CbInterMinUs{INT64_MAX};
  std::atomic<int64_t> m_CbInterMaxUs{0};
  std::atomic<uint64_t> m_CbCount{0};
  std::atomic<uint64_t> m_CbLateCount{0}; // gap > 1.5x the buffer period

  /* Per-period cost of ONE voice-frame, in picoseconds, sampled at period
   * end into a lock-free ring (the monitor reports min/median/max over it).
   *
   * This is a THROUGHPUT metric, deliberately unlike the ceiling sweep: the
   * ceiling is a THRESHOLD, so one background disturbance falsifies a whole
   * run (measured 2026-07-15: the same build missed 24k once and cleared it
   * minutes later). A continuous per-voice cost can be aggregated instead -
   * min is the undisturbed truth (best-case = what the code can do), max
   * exposes interference, median the typical case. Comparing two builds
   * means comparing their min at the same polyphony.
   *
   * Picoseconds because one voice-frame costs well under 1 ns (~0.5 ns at
   * 20k voices), which would round to 0 in ns. Only sampled from
   * VOICE_COST_MIN_VOICES up: below that the per-period fixed cost
   * dominates and the per-voice figure is meaningless. */
  std::atomic<uint32_t> m_VoiceCostPsRing[512]{};
  std::atomic<unsigned> m_VoiceCostWrites{0};
  /* Sustained-overrun detector (audio thread only): completion latency
   * above the period duration for >= 3 CONSECUTIVE periods means the
   * buffering slack is drained - guaranteed-audible overload - while
   * isolated over-budget periods are absorbed and must not count (a
   * peak-based criterion falsely penalizes the memory-latency-spiky V3
   * profile against steady legacy compute). Counts every period spent
   * inside such a run from its third period on. */
  unsigned m_OverBudgetRun = 0;
  // Audio-thread-private: last applied affinity mode (see NextPeriod).
  int m_AffinityApplied = -1;
  std::atomic<uint64_t> m_SustainedOverrunPeriods{0};
  /* Cumulative microseconds the audio thread spends inside
   * WakeupThreads()'s signal loop (20 workers x 375 periods/s = 7500
   * syscalls/s, serial, pre-work - the last known fixed serial cost of
   * a period after the drain removal). Monitor shows the per-second
   * rate; the number decides whether a wake cascade / shared broadcast
   * is worth its concurrency risk. */
  std::atomic<uint64_t> m_WakeLoopUsSum{0};

  // Scheduling policy/priority of the audio callback thread, detected
  // once on the first NextPeriod() call (which runs on the audio thread).
  // -1 = not yet detected. On Linux: sched_getscheduler() result
  // (SCHED_FIFO=1 or SCHED_RR=2 means RT; SCHED_OTHER=0 means normal);
  // if the backend did not deliver an RT thread, NextPeriod() first tries
  // to elevate it to SCHED_FIFO 70 itself, so 0 here means that attempt
  // failed (no rtprio rlimit). On Windows: GetThreadPriority() value
  // (15 = TIME_CRITICAL).
  std::atomic<int> m_AudioThreadSchedInfo;

  // Where the audio callback thread runs, sampled per period (see
  // GetAudioCpuProbe).
  GOCpuPlacementProbe m_AudioCpuProbe;

  // Number of voices to look ahead when issuing software prefetch hints in
  // the audio group task loop. 0 = disabled. Read on the audio thread,
  // written from the GUI thread — relaxed ordering is fine (stale value for
  // one period is harmless).
  std::atomic<int> m_PrefetchDistance;

  // protects mp_MeterInfo/p_MeterInfo against concurrent replacement
  // (SetAudioOutput vs GetMeterInfo)
  GOMutex m_MeterMutex;

  // Per-channel peak levels for the meter display (one element per real output
  // channel; polyphony is tracked separately in m_UsedPolyphony).
  //
  // Ownership and access pattern:
  //   Audio thread   — p_MeterInfo.load(acquire) each period; writes peaks via
  //                    atomic_fetch_max_relaxed(); no mutex held
  //   GUI thread     — GetMeterInfo() under m_MeterMutex; reads *p_MeterInfo,
  //                    resets elements via exchange(0); prepends polyphony
  //                    ratio from m_UsedPolyphony.exchange(0)
  //   Control thread — SetAudioOutput() under m_MeterMutex; creates a new
  //                    vector when channel count changes, stores pointer via
  //                    p_MeterInfo.store(release)
  //
  // mp_MeterInfo owns the vector; p_MeterInfo is an atomic pointer to the
  // same object (always == mp_MeterInfo.get()). SetAudioOutput() always
  // allocates a new vector, so size and data pointer are always consistent
  // within a single vector object published atomically via p_MeterInfo.
  // std::shared_ptr with atomic access would provide automatic lifetime safety
  // but incurs atomic reference-count increments/decrements on every
  // NextPeriod() call (~100/s), adding unnecessary overhead in the audio
  // thread. float is used instead of double because std::atomic<float> is
  // lock-free on all supported platforms (x86, ARM), while std::atomic<double>
  // is not.
  std::unique_ptr<std::vector<std::atomic<float>>> mp_MeterInfo;
  // always == mp_MeterInfo.get()
  std::atomic<std::vector<std::atomic<float>> *> p_MeterInfo;

  ptr_vector<GOSoundTremulantTask> m_TremulantTasks;
  ptr_vector<GOSoundWindchestTask> m_WindchestTasks;
  ptr_vector<GOSoundGroupTask> m_AudioGroupTasks;
  ptr_vector<GOSoundOutputTask> m_AudioOutputTasks;
  // One per device output task: first scheduler task of every period,
  GOSoundRecorder *m_AudioRecorder;
  GOSoundReleaseTask *m_ReleaseProcessor;
  std::unique_ptr<GOSoundTouchTask> m_TouchTask;
  GOSoundScheduler m_Scheduler;

  std::vector<std::unique_ptr<GOSoundThread>> mp_threads;
  GOMutex m_ThreadLock;

  GOSoundResample m_resample;
  GOSoundResample::InterpolationType m_interpolation;

  std::atomic_bool m_HasBeenSetup;

  void StartThreads(unsigned nConcurrency);
  void StopThreads();

  unsigned MsToSamples(unsigned ms) const { return m_SampleRate * ms / 1000; }

  unsigned SamplesDiffToMs(uint64_t fromSamples, uint64_t toSamples) const;

  /* samplerTaskId:
     -1 .. -n Tremulants
     0 (DETACHED_RELEASE_TASK_ID) detached release
     1 .. n Windchests
  */
  inline static unsigned isWindchestTask(int taskId) { return taskId >= 0; }

  inline static unsigned windchestTaskToIndex(int taskId) {
    return (unsigned)taskId;
  }

  inline static unsigned tremulantTaskToIndex(int taskId) {
    return -taskId - 1;
  }

  void StartSampler(GOSoundSampler *sampler);

  GOSoundSampler *CreateTaskSample(
    const GOSoundProvider *soundProvider,
    int samplerTaskId,
    unsigned audioGroup,
    unsigned velocity,
    unsigned delay,
    uint64_t prevEventTime,
    bool isRelease,
    uint64_t *pStartTimeSamples);
  void CreateReleaseSampler(GOSoundSampler *sampler);

  /**
   * Creates a new sampler with decay of current loop.
   * Switch this sampler to the new attack.
   * It is used when a wave tremulant is switched on or off.
   * @param pSampler current playing sampler for switching to a new attack
   */
  void SwitchToAnotherAttack(GOSoundSampler *pSampler);
  float GetRandomFactor() const;
  unsigned GetBufferSizeFor(unsigned outputIndex, unsigned n_frames) const;

public:
  GOSoundOrganEngine();
  ~GOSoundOrganEngine();

  // Group 1: Independent setters (no call-order dependencies)
  unsigned GetSampleRate() const override { return m_SampleRate; }
  void SetSampleRate(unsigned sample_rate) { m_SampleRate = sample_rate; }
  void SetSamplesPerBuffer(unsigned sample_per_buffer) {
    m_SamplesPerBuffer = sample_per_buffer;
  }
  int GetVolume() const { return m_Volume; }
  void SetVolume(int volume);
  unsigned GetHardPolyphony() const { return m_SamplerPool.GetUsageLimit(); }
  void SetHardPolyphony(unsigned polyphony);
  // Diagnostics only (audio monitor report): arena geometry probes.
  const GOSoundSamplerPool &GetSamplerPool() const { return m_SamplerPool; }

  // The polyphony peak since the last call to this method, for a live
  // diagnostics graph. Deliberately a *separate* atomic from m_UsedPolyphony
  // (see below) rather than a non-destructive peek at it: m_UsedPolyphony is
  // exchange(0)'d by the toolbar meter at its own ~14/s cadence, independent
  // of and uncoordinated with whatever cadence a diagnostics dialog polls
  // at - a non-destructive read of that same atomic would, depending on
  // exactly when it landed relative to the meter's own reset, sometimes see
  // only the few milliseconds since the meter's last reset and report a
  // spurious near-zero/zero reading even under sustained load. Mirrors
  // GOSoundSystem::GetAndResetRenderGapPeaks().
  unsigned GetAndResetUsedPolyphonyPeak() {
    return m_UsedPolyphonyWindowPeak.exchange(0, std::memory_order_relaxed);
  }

  // Cumulative polyphony emergency-drop counts, for an "Audio Diagnostics
  // Report"/graph - see m_PolyphonyPanicDrops/m_PolyphonyNormalDrops above.
  uint64_t GetPolyphonyPanicDrops() const {
    return m_PolyphonyPanicDrops.load(std::memory_order_relaxed);
  }
  uint64_t GetPolyphonyNormalDrops() const {
    return m_PolyphonyNormalDrops.load(std::memory_order_relaxed);
  }
  // Total drops (panic + normal) since the last call. Resets to 0 on read.
  // For the live diagnostics graph drop-event markers.
  uint64_t GetAndResetPolyphonyDropCount() {
    return m_PolyphonyDropsWindow.exchange(0, std::memory_order_relaxed);
  }
  // Number of active worker threads in the audio processing pool.
  unsigned GetWorkerThreadCount() const {
    return static_cast<unsigned>(mp_threads.size());
  }
  // Elevation status of the worker thread pool. All threads attempt the same
  // platform call, so querying the first thread is representative.
  // Returns Pending if the pool is empty or threads haven't started yet.
  GOSoundThread::ElevationStatus GetWorkerElevationStatus() const {
    if (mp_threads.empty())
      return GOSoundThread::ElevationStatus::Pending;
    return mp_threads[0]->GetElevationStatus();
  }
  /* Census over ALL workers - the single-thread getter above samples only
   * worker 0 and hides partial elevation. MMCSS grants are a per-thread
   * request against a shared system-wide budget, so "some workers Pro
   * Audio, some fallback" is a real state - and a prime suspect for the
   * binary FAST/SLOW process-start lottery (2026-07-23): non-elevated
   * workers are throttled by the SystemResponsiveness reserve, stretching
   * every period from the middle (not dispatch, not tail). */
  void CountWorkerElevations(
    unsigned &mmcss, unsigned &fallback, unsigned &other) const {
    mmcss = fallback = other = 0;
    for (auto &t : mp_threads)
      switch (t->GetElevationStatus()) {
      case GOSoundThread::ElevationStatus::MmcssSucceeded:
        mmcss++;
        break;
      case GOSoundThread::ElevationStatus::FallbackHighest:
        fallback++;
        break;
      default:
        other++;
      }
  }
  // Raw GetThreadPriority() reading for a worker thread (Windows only; see
  // GOSoundThread::GetWorkerThreadPriority). -1000 if the pool is empty or
  // no thread has run Entry() yet.
  int GetWorkerThreadPriority() const {
    if (mp_threads.empty())
      return -1000;
    return mp_threads[0]->GetWorkerThreadPriority();
  }
  // Min/max GetThreadPriority() reading across ALL started workers. If min
  // != max the pool did not all land at the same scheduling priority (a
  // partial elevation grant). Returns false if no worker has run Entry()
  // yet (nothing to compare). Windows-meaningful; harmless elsewhere.
  bool GetWorkerPriorityRange(int &minPrio, int &maxPrio) const {
    bool any = false;

    for (auto &t : mp_threads) {
      const int p = t->GetWorkerThreadPriority();

      if (p == -1000)
        continue; // this worker has not captured its priority yet
      if (!any) {
        minPrio = maxPrio = p;
        any = true;
      } else {
        if (p < minPrio)
          minPrio = p;
        if (p > maxPrio)
          maxPrio = p;
      }
    }
    return any;
  }

  // -1 = callback hasn't run yet; on Linux SCHED_FIFO/SCHED_RR = RT;
  // on Windows 15 = THREAD_PRIORITY_TIME_CRITICAL.
  int GetAudioThreadSchedInfo() const {
    return m_AudioThreadSchedInfo.load(std::memory_order_relaxed);
  }

  // Passive CPU-placement probes (diagnostics report): audio thread
  // sampled per period in NextPeriod(), workers per wakeup.
  const GOCpuPlacementProbe &GetAudioCpuProbe() const {
    return m_AudioCpuProbe;
  }
  const GOCpuPlacementProbe &GetWorkerCpuProbe(unsigned i) const {
    return mp_threads[i]->GetCpuProbe();
  }

  int GetPrefetchDistance() const {
    return m_PrefetchDistance.load(std::memory_order_relaxed);
  }
  void SetPrefetchDistance(int d) {
    m_PrefetchDistance.store(d, std::memory_order_relaxed);
  }

  /* Policy for the TIME_CRITICAL audio thread joining the period's voice
   * work via GOSoundGroupTask::Finish (0 = always help, the historical
   * behaviour; 1 = never help, wait on the condition only; 2 =
   * deadline-aware: help only once ~70% of the period budget has
   * elapsed). Helping at full load runs 21 compute threads on 20 logical
   * CPUs: the RT thread evicts a worker mid-claim and can CREATE the very
   * straggler it then waits for - but it stays a robustness escape hatch
   * when the period is genuinely late. Runtime A/B via the Audio
   * Monitor. */
  int GetCallbackHelpPolicy() const {
    return m_CallbackHelpPolicy.load(std::memory_order_relaxed);
  }
  void SetCallbackHelpPolicy(int p) {
    m_CallbackHelpPolicy.store(p, std::memory_order_relaxed);
  }
  // Decision point used by GOSoundGroupTask::Finish for pThread == nullptr
  // (only the audio thread passes no worker context).
  bool MayCallbackHelp() const;

  // Leak/stuck-voice diagnostics - see m_ProcessSamplerCallCount above.
  uint64_t GetLastProcessedSamplerCount() const {
    return m_LastProcessedSamplerCount.load(std::memory_order_relaxed);
  }
  // True once a sustained UsedSamplerCount-vs-processed gap has been
  // detected and logged. Stays true until the next Reset() - a real leak
  // does not self-heal, so there is no point auto-clearing it.
  bool GetOrphanLeakDetected() const {
    return m_OrphanLeakDetected.load(std::memory_order_relaxed);
  }

  // See m_LongestPeriodCompletionUs above.
  unsigned GetAndResetFastPathVoices() {
    return m_FastPathVoicesWindow.exchange(0, std::memory_order_relaxed);
  }
  unsigned GetAndResetTotalVoicesWindow() {
    return m_TotalVoicesWindow.exchange(0, std::memory_order_relaxed);
  }

  // See m_FaderKernelWindow above. Consumed by the Audio Monitor together
  // with the fast-path counters so all window ratios stay self-consistent.
  struct FaderKernelStats {
    unsigned constant;
    unsigned ramp;
    unsigned nonLinear;
    unsigned legacy;
  };
  FaderKernelStats GetAndResetFaderKernelStats() {
    return {
      m_FaderKernelWindow[0].exchange(0, std::memory_order_relaxed),
      m_FaderKernelWindow[1].exchange(0, std::memory_order_relaxed),
      m_FaderKernelWindow[2].exchange(0, std::memory_order_relaxed),
      m_FaderKernelWindow[3].exchange(0, std::memory_order_relaxed)};
  }
  unsigned GetAndResetCompressedVoices() {
    return m_CompressedVoicesWindow.exchange(0, std::memory_order_relaxed);
  }

  // See m_VoiceStatsEnabled above. Enabling zeroes the window counters so
  // the first poll window doesn't include residue from the previous
  // monitor session.
  void SetVoiceStatsEnabled(bool enabled) {
    if (enabled && !m_VoiceStatsEnabled.load(std::memory_order_relaxed)) {
      m_FastPathVoicesWindow.store(0, std::memory_order_relaxed);
      m_TotalVoicesWindow.store(0, std::memory_order_relaxed);
      for (auto &counter : m_FaderKernelWindow)
        counter.store(0, std::memory_order_relaxed);
      m_CompressedVoicesWindow.store(0, std::memory_order_relaxed);
    }
    m_VoiceStatsEnabled.store(enabled, std::memory_order_relaxed);
  }

  // Cumulative count of periods spent in sustained (>=3 consecutive)
  // completion overrun - see m_SustainedOverrunPeriods.
  uint64_t GetWakeLoopUsSum() const {
    return m_WakeLoopUsSum.load(std::memory_order_relaxed);
  }
  // Scheduler dispatch latency (broadcast -> first worker resumes),
  // accumulated us; see GOSoundThread::s_DispatchLatUsSum.
  uint64_t GetDispatchLatUsSum() const {
    return GOSoundThread::GetDispatchLatUsSum();
  }
  // Accumulated barrier-tail (first->last worker finish spread) us; see
  // GOSoundThread::s_TailUsSum.
  uint64_t GetTailUsSum() const { return GOSoundThread::GetTailUsSum(); }
  // Burst taxonomy (Linux): wall vs thread-CPU time + involuntary context
  // switches across all worker render bursts - see GOSoundThread.
  uint64_t GetBurstWallUsSum() const {
    return GOSoundThread::GetBurstWallUsSum();
  }
  uint64_t GetBurstCpuUsSum() const {
    return GOSoundThread::GetBurstCpuUsSum();
  }
  uint64_t GetBurstNivcswSum() const {
    return GOSoundThread::GetBurstNivcswSum();
  }
  uint64_t GetBurstCount() const { return GOSoundThread::GetBurstCount(); }
  // Raw MXCSR the first worker inherited from the OS (before GO set FTZ);
  // -1 = not captured. See GOSoundThread::s_WorkerInheritedCsr.
  int GetWorkerInheritedCsr() const {
    return GOSoundThread::GetWorkerInheritedCsr();
  }
  uint64_t GetSustainedOverrunPeriods() const {
    return m_SustainedOverrunPeriods.load(std::memory_order_relaxed);
  }
  int64_t GetAndResetPeriodCompletionPeakUs() {
    return m_LongestPeriodCompletionUs.exchange(0, std::memory_order_relaxed);
  }

  // Callback interarrival jitter (2026-07-25): wall-clock gap between
  // consecutive NextPeriod() calls (= audio callbacks). Ideal = one buffer
  // period; the min/max spread and late-count expose irregular delivery
  // (the "jitter tips the period over its edge" hypothesis).
  int64_t GetCbInterMinUs() const {
    const int64_t v = m_CbInterMinUs.load(std::memory_order_relaxed);
    return v == INT64_MAX ? 0 : v;
  }
  int64_t GetCbInterMaxUs() const {
    return m_CbInterMaxUs.load(std::memory_order_relaxed);
  }
  uint64_t GetCbCount() const {
    return m_CbCount.load(std::memory_order_relaxed);
  }
  uint64_t GetCbLateCount() const {
    return m_CbLateCount.load(std::memory_order_relaxed);
  }
  void ResetCbJitter() {
    m_LastCbUs.store(0, std::memory_order_relaxed);
    m_CbInterMinUs.store(INT64_MAX, std::memory_order_relaxed);
    m_CbInterMaxUs.store(0, std::memory_order_relaxed);
    m_CbCount.store(0, std::memory_order_relaxed);
    m_CbLateCount.store(0, std::memory_order_relaxed);
  }

  // Window and load floor of the per-voice-frame cost samples (public: the
  // monitor sizes its buffer with it and reports the floor).
  static constexpr unsigned VOICE_COST_RING = 512; // ~5.5 s @ 512/48k
  static constexpr unsigned VOICE_COST_MIN_VOICES = 500;

  /* Copies the per-voice-frame cost samples (ps) into `out`, newest window
   * first-come-first-served (order is irrelevant - the caller sorts). Returns
   * how many were valid. Rolling: always the last <=VOICE_COST_RING periods
   * that ran at >=VOICE_COST_MIN_VOICES. See the member comment. */
  unsigned GetVoiceCostSamplesPs(uint32_t *out, unsigned maxOut) const {
    const unsigned written = m_VoiceCostWrites.load(std::memory_order_relaxed);
    const unsigned have = std::min(written, VOICE_COST_RING);
    const unsigned n = std::min(have, maxOut);

    for (unsigned i = 0; i < n; i++)
      out[i] = m_VoiceCostPsRing[i].load(std::memory_order_relaxed);
    return n;
  }
  void ResetVoiceCostSamples() {
    // Drops the window (e.g. after a polyphony change, whose old samples
    // would otherwise mix into min/median for ~5 s).
    for (unsigned i = 0; i < VOICE_COST_RING; i++)
      m_VoiceCostPsRing[i].store(0, std::memory_order_relaxed);
    m_VoiceCostWrites.store(0, std::memory_order_relaxed);
  }
  void SetPolyphonyLimiting(bool limiting) { m_PolyphonyLimiting = limiting; }
  void SetScaledReleases(bool enable) { m_ScaledReleases = enable; }
  void SetRandomizeSpeaking(bool enable) { m_RandomizeSpeaking = enable; }
  void SetInterpolationType(unsigned type) {
    m_interpolation = (GOSoundResample::InterpolationType)type;
  }
  GOSoundResample::InterpolationType GetInterpolationType() const {
    return m_interpolation;
  }
  // Group 2: Dependent setters (must be called in this order)
  // Must be called after SetSamplesPerBuffer because uses m_SamplesPerBuffer
  unsigned GetAudioGroupCount() const { return m_AudioGroupCount; }
  void SetAudioGroupCount(unsigned groups);
  // Must be called after SetAudioGroupCount because uses m_AudioGroupCount and
  // m_SamplesPerBuffer
  void SetAudioOutput(std::vector<GOAudioOutputConfiguration> audio_outputs);
  // Must be called after SetAudioOutput because iterates m_AudioOutputTasks
  void SetupReverb(GOConfig &settings);
  // Must be called after SetAudioOutput because uses m_AudioOutputTasks
  void SetAudioRecorder(GOSoundRecorder *recorder, bool downmix);

  // Group 3: Other getters
  float GetGain() const { return m_Gain; }
  uint64_t GetTime() const { return m_CurrentTime; }
  std::vector<float> GetMeterInfo();
  GOSoundScheduler &GetScheduler() { return m_Scheduler; }

  void Reset();
  void Setup(
    GOOrganModel &organModel,
    GOMemoryPool &memoryPool,
    unsigned releaseCount = 1);
  void ClearSetup();

  GOSoundSampler *StartPipeSample(
    const GOSoundProvider *pipeProvider,
    unsigned windchestN,
    unsigned audioGroup,
    unsigned velocity,
    unsigned delay,
    uint64_t prevEventTime,
    bool isRelease = false,
    uint64_t *pStartTimeSamples = nullptr) override {
    return CreateTaskSample(
      pipeProvider,
      windchestN,
      audioGroup,
      velocity,
      delay,
      prevEventTime,
      isRelease,
      pStartTimeSamples);
  }

  inline GOSoundSampler *StartTremulantSample(
    const GOSoundProvider *tremProvider,
    unsigned tremulantN,
    uint64_t prevEventTime) override {
    return CreateTaskSample(
      tremProvider, -tremulantN, 0, 0x7f, 0, prevEventTime, false, nullptr);
  }

  uint64_t StopSample(
    const GOSoundProvider *pipe, GOSoundSampler *handle) override;
  void SwitchSample(
    const GOSoundProvider *pipe, GOSoundSampler *handle) override;
  void UpdateVelocity(
    const GOSoundProvider *pipe,
    GOSoundSampler *handle,
    unsigned velocity) override;

  /** Configure the engine for the given organ and start worker threads */
  void BuildAndStart(
    GOConfig &config,
    unsigned sampleRate,
    unsigned samplesPerBuffer,
    GOOrganModel &organModel,
    GOMemoryPool &memoryPool,
    unsigned releaseConcurrency,
    GOSoundRecorder &audioRecorder);

  /** Stop worker threads and tear down the organ setup */
  void StopAndDestroy();

  /** Wait for all worker threads to finish their current work item. */
  void WaitForThreadsIdle();

  /** Wake up all worker threads. Called from the audio callback. */
  void WakeupThreads();

  void GetAudioOutput(
    unsigned outputIndex, bool isLast, GOSoundBufferMutable &outBuffer);
  void NextPeriod();

  bool ProcessSampler(
    float *buffer, GOSoundSampler *sampler, unsigned n_frames, float volume);

  /* ProcessSampler split into its three stages, for the fused batch
   * render path (GOSoundGroupTask): PreRender runs the time gate,
   * diagnostics and the polyphony drop logic (which mutates fader state,
   * so it must precede any gain computation) and returns whether the
   * sampler is due this period; AccumulateSamplerBuffer sends an
   * already-rendered block through the unchanged fader/ToneBalance/mix
   * chain; PostRender handles stop/new_attack handover and the
   * keep-alive return value (pass PreRender's result as wasProcessed).
   * ProcessSampler() itself is exactly PreRender + ReadBlock +
   * AccumulateSamplerBuffer + PostRender. */
  bool ProcessSamplerPreRender(GOSoundSampler *sampler);
  void AccumulateSamplerBuffer(
    float *output_buffer,
    GOSoundSampler *sampler,
    unsigned n_frames,
    const float *temp,
    float volume);
  bool ProcessSamplerPostRender(GOSoundSampler *sampler, bool wasProcessed);

  // Voice-mix histogram hook for the batch render path, which computes
  // its gains via GOSoundFader::ComputeFrameGains instead of
  // ProcessAndAccumulate (whose kind-counting lives in
  // AccumulateSamplerBuffer).
  void CountFaderKernel(GOFadeKernelKind kind) {
    if (m_VoiceStatsEnabled.load(std::memory_order_relaxed))
      m_FaderKernelWindow[static_cast<unsigned>(kind)].fetch_add(
        1, std::memory_order_relaxed);
  }

  void ProcessRelease(GOSoundSampler *sampler);
  void PassSampler(GOSoundSampler *sampler);
  void ReturnSampler(GOSoundSampler *sampler);
};

#endif /* GOSOUNDORGANENGINE_H */
