/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDOUTPUTTASK_H
#define GOSOUNDOUTPUTTASK_H

#include <atomic>
#include <vector>

#include "sound/reverb/GOSoundReverb.h"
#include "sound/scheduler/GOSoundTask.h"
#include "threading/GOMutex.h"

#include "GOSoundBufferTaskBase.h"

class GOSoundConcealment;

class GOSoundOutputTask : public GOSoundBufferTaskBase {
private:
  std::vector<float> m_ScaleFactors;
  std::vector<GOSoundBufferTaskBase *> m_Outputs;
  unsigned m_OutputCount;
  std::vector<float> m_MeterInfo;
  GOSoundReverb *m_Reverb;
  GOMutex m_Mutex;
  std::atomic_bool m_Done;
  std::atomic_bool m_Stop;

  // Set by DeliverBail (audio thread) when the period's budget is gone:
  // the straggling mix checks it at loop boundaries and returns without
  // publishing (no m_Done, no reverb, no history snapshot).  Cleared in
  // Reset().
  std::atomic_bool m_Abandon;
  // This period was already delivered as a prediction - a mix finishing
  // now must REPLACE that entry in the history, not append to it.
  std::atomic_bool m_WasConcealed;
  // A straggler is currently allowed to run past its deadline.  At most
  // ONE may be in flight: m_Abandon is what bounds the work per period
  // (it skips the output groups' Finish(), where the render time
  // actually goes), so letting stragglers pile up removes the only
  // backpressure and the backlog grows without limit.
  std::atomic_bool m_StragglerRunning;
  // "Your deadline has passed" - set on EVERY bail, whether or not the
  // render is abandoned.  The forced-miss injection waits on this, not on
  // m_Abandon: its job is to make the mix LATE, and it must stop sleeping
  // the moment the callback has given up, otherwise it keeps the task
  // blocked for its full cap instead of for one period.
  std::atomic_bool m_DeadlinePassed;
  // Only every second miss keeps its straggler.  One straggler is a full
  // extra period of render work; at 6000 injected misses per minute that
  // would be ~27 % on top of the normal load, which is enough to push a
  // machine that is already at its limit into real underruns.  Halving it
  // costs little: truth for every second missed period measured -12.6 dB
  // against -15.1 for all of them, where doing nothing gives -10.8.
  unsigned m_MissCounter;

  // Deferred reset: Reset() must NEVER block the callback thread on a
  // straggling mixer (the abandon checks sit between outputs, but the
  // straggler can be stuck INSIDE a GroupTask::Finish for the rest of a
  // saturated period - the mutex wait then delayed NextPeriod by up to a
  // full period and turned single overloads into xrun salvos).  If the
  // try-lock fails, the reset is recorded here and applied by whoever
  // held the mutex right after releasing it; IsDone() reports false while
  // one is pending, so the next delivery routes into the concealment path
  // instead of trusting a stale buffer.
  std::atomic_bool m_PendingReset;

  // The locked mix body of Run(); plain returns inside are safe - the
  // pending-reset application happens in Run() after the mutex scope.
  void RunMixLocked(GOSoundThread *pThread, bool audio);
  void ApplyPendingReset();

  // Monotonic-clock timestamp (us) of when m_Done last became true - i.e.
  // when this device's output (and transitively everything it depends on)
  // was fully computed, regardless of whether that happened on a
  // background worker thread or via Finish() on the realtime audio thread.
  // Used by GOSoundOrganEngine to measure total period-completion latency
  // for the audio diagnostics graph - see GetDoneUs().
  std::atomic<int64_t> m_DoneUs;

  // Deadline-miss concealment (emergency buffer), see GOSoundConcealment.
  // Owned; created by SetupConcealment() on the control thread while the
  // sound system is stopped, so no locking against Run()/DeliverAudio().
  GOSoundConcealment *m_Concealment;

public:
  GOSoundOutputTask(
    unsigned channels,
    std::vector<float> scale_factors,
    unsigned samples_per_buffer);
  ~GOSoundOutputTask();

  void SetOutputs(std::vector<GOSoundBufferTaskBase *> outputs);

  unsigned GetGroup();
  unsigned GetCost();
  bool GetRepeat();
  void Run(GOSoundThread *pThread = nullptr);
  void Exec();
  void Finish(bool stop, GOSoundThread *pThread = nullptr);

  void Clear();
  void Reset();

  void SetupReverb(
    const GOSoundReverb::ReverbConfig &config,
    unsigned nSamplesPerBuffer,
    unsigned sampleRate);

  void SetupConcealment(unsigned sampleRate, const char *wisdomPath);

  // Audio callback inside the budget watch: TRY to win the mix mutex and
  // do the mix here - the callback-help contribution, worth a measurable
  // slice of the polyphony ceiling.  Never blocks: on contention (a
  // worker is mid-mix) it returns immediately and the caller keeps
  // polling until its bail deadline.  Returns true once the mix is
  // complete.  Note that winning the mutex is a COMMITMENT: while we mix
  // we cannot bail, so a deep overload overshoots into PipeWire's
  // pipeline slack exactly like the legacy path did.
  bool TryHelpMix(bool isLast);

  // Audio callback fast path check: has this period's mix been completed?
  // A pending deferred reset means the visible m_Done belongs to a PAST
  // period - report not-done so the delivery conceals instead.
  bool IsDone() const {
    return m_Done.load(std::memory_order_acquire)
      && !m_PendingReset.load(std::memory_order_acquire);
  }

  // Audio callback, after Finish(): copy this task's finished mix into
  // outBuffer (with the concealment re-entry crossfade if the previous
  // period was concealed).
  void DeliverAudio(GOSoundBufferMutable &outBuffer);

  // Audio callback, when the mix missed its budget (real or injected):
  // abandon the straggling mix (it returns at the next check point and its
  // result is discarded - "drop the late buffer") and deliver the
  // emergency shadow (or silence, per concealment mode) instead.
  void DeliverBail(GOSoundBufferMutable &outBuffer, bool isLast);


  const std::vector<float> &GetMeterInfo();
  void ResetMeterInfo();

  int64_t GetDoneUs() const { return m_DoneUs.load(std::memory_order_relaxed); }

  // Diagnostics (2026-07-25) - see the .cpp. Aggregate across all output
  // tasks; the audio thread is the pThread==nullptr caller.
  static uint64_t GetAudioRuns();
  static uint64_t GetAudioDoneEarly(); // a worker finished it first (too late)
  static uint64_t GetAudioLostLock();  // lost the mutex race to a worker
  static uint64_t GetAudioMixed();     // audio thread actually did the mix
  static uint64_t GetMixUsSum();       // serial mix time (any thread), us
  static uint64_t GetMixCount();
  static int64_t GetMixUsMax();
  static void ResetDiag();
};

#endif
