/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundOutputTask.h"

#include <algorithm>
#include <chrono>

#include "sound/GOSoundConcealment.h"
#include "sound/reverb/GOSoundReverb.h"
#include "sound/scheduler/GOSoundThread.h"
#include "threading/GOMutexLocker.h"

// Maximum sound items amplitude for output
static constexpr float CLAMP_MIN = -1.0f;
static constexpr float CLAMP_MAX = 1.0f;

/* Diagnostics (2026-07-25): audio-thread help autopsy + serial-mix cost.
 * There is one audio callback but several GOSoundOutputTasks, so these are
 * file-static and the report aggregates across them. The audio thread runs
 * Run(pThread==nullptr) via GetAudioOutput->Finish(isLast); workers pass a
 * real pThread. The counters answer: when the callback COULD help (policy
 * allows), does it ever reach the mix, or does a worker always finish the
 * OutputTask first (m_Done at entry = "too late") / win the mutex race
 * ("lost lock")? And how long is the single-threaded mix itself? */
static std::atomic<uint64_t> s_AudioRuns{0};      // audio thread entered Run
static std::atomic<uint64_t> s_AudioDoneEarly{0}; // m_Done already true
static std::atomic<uint64_t> s_AudioLostLock{0};  // lost the mutex to a worker
static std::atomic<uint64_t> s_AudioMixed{0};     // audio thread did the mix
static std::atomic<uint64_t> s_MixUsSum{0}; // serial mix time (any thread)
static std::atomic<uint64_t> s_MixCount{0};
static std::atomic<int64_t> s_MixUsMax{0};

static inline int64_t diagNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
           std::chrono::steady_clock::now().time_since_epoch())
    .count();
}

uint64_t GOSoundOutputTask::GetAudioRuns() {
  return s_AudioRuns.load(std::memory_order_relaxed);
}
uint64_t GOSoundOutputTask::GetAudioDoneEarly() {
  return s_AudioDoneEarly.load(std::memory_order_relaxed);
}
uint64_t GOSoundOutputTask::GetAudioLostLock() {
  return s_AudioLostLock.load(std::memory_order_relaxed);
}
uint64_t GOSoundOutputTask::GetAudioMixed() {
  return s_AudioMixed.load(std::memory_order_relaxed);
}
uint64_t GOSoundOutputTask::GetMixUsSum() {
  return s_MixUsSum.load(std::memory_order_relaxed);
}
uint64_t GOSoundOutputTask::GetMixCount() {
  return s_MixCount.load(std::memory_order_relaxed);
}
int64_t GOSoundOutputTask::GetMixUsMax() {
  return s_MixUsMax.load(std::memory_order_relaxed);
}
void GOSoundOutputTask::ResetDiag() {
  s_AudioRuns.store(0, std::memory_order_relaxed);
  s_AudioDoneEarly.store(0, std::memory_order_relaxed);
  s_AudioLostLock.store(0, std::memory_order_relaxed);
  s_AudioMixed.store(0, std::memory_order_relaxed);
  s_MixUsSum.store(0, std::memory_order_relaxed);
  s_MixCount.store(0, std::memory_order_relaxed);
  s_MixUsMax.store(0, std::memory_order_relaxed);
}

GOSoundOutputTask::GOSoundOutputTask(
  unsigned channels,
  std::vector<float> scale_factors,
  unsigned samples_per_buffer)
  : GOSoundBufferTaskBase(channels, samples_per_buffer),
    m_ScaleFactors(scale_factors),
    m_Outputs(),
    m_OutputCount(0),
    m_MeterInfo(channels),
    m_Reverb(0),
    m_Done(false),
    m_DoneUs(0),
    m_Abandon(false),
    m_WasConcealed(false),
    m_StragglerRunning(false),
    m_DeadlinePassed(false),
    m_MissCounter(0),
    m_PendingReset(false),
    m_Concealment(nullptr) {
  m_Reverb = new GOSoundReverb(channels);
}

GOSoundOutputTask::~GOSoundOutputTask() {
  if (m_Reverb)
    delete m_Reverb;
  if (m_Concealment)
    delete m_Concealment;
}

void GOSoundOutputTask::SetOutputs(
  std::vector<GOSoundBufferTaskBase *> outputs) {
  m_Outputs = outputs;
  m_OutputCount = m_Outputs.size() * 2;
}

void GOSoundOutputTask::Run(GOSoundThread *pThread) {
  const bool audio = (pThread == nullptr); // the realtime callback path
  if (audio)
    s_AudioRuns.fetch_add(1, std::memory_order_relaxed);
  if (m_Done.load()) {
    if (audio) // a worker already finished this output: the callback is late
      s_AudioDoneEarly.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  bool held = false;
  {
    GOMutexLocker locker(m_Mutex, false, "GOSoundOutputTask::Run", pThread);

    held = locker.IsLocked();
    if (m_Done.load() || !held) {
      if (audio) {
        if (m_Done.load()) // finished between the two checks
          s_AudioDoneEarly.fetch_add(1, std::memory_order_relaxed);
        else // a worker holds the mutex and is mid-mix
          s_AudioLostLock.fetch_add(1, std::memory_order_relaxed);
      }
    } else
      RunMixLocked(pThread, audio);
  }
  // Whoever held the mutex applies a reset that NextPeriod could not take
  // while we were mixing (see m_PendingReset in the header).
  if (held)
    ApplyPendingReset();
}

bool GOSoundOutputTask::TryHelpMix(bool isLast) {
  if (isLast)
    m_Stop.store(true);
  if (m_Done.load(std::memory_order_acquire))
    return true;

  bool held = false;
  {
    GOMutexLocker locker(m_Mutex, true, "GOSoundOutputTask::TryHelpMix");

    held = locker.IsLocked();
    s_AudioRuns.fetch_add(1, std::memory_order_relaxed);
    if (!held) // a worker holds it and is mid-mix
      s_AudioLostLock.fetch_add(1, std::memory_order_relaxed);
    else if (m_Done.load())
      s_AudioDoneEarly.fetch_add(1, std::memory_order_relaxed);
    else
      RunMixLocked(nullptr, true);
  }
  if (held)
    ApplyPendingReset();
  return IsDone();
}

void GOSoundOutputTask::ApplyPendingReset() {
  if (m_PendingReset.exchange(false, std::memory_order_acq_rel)) {
    m_Done.store(false);
    m_Stop.store(false);
    m_Abandon.store(false, std::memory_order_relaxed);
    m_WasConcealed.store(false, std::memory_order_relaxed);
    m_DeadlinePassed.store(false, std::memory_order_relaxed);
    m_DoneUs.store(0, std::memory_order_relaxed);
  }
}

void GOSoundOutputTask::RunMixLocked(GOSoundThread *pThread, bool audio) {
  {
    if (audio)
      s_AudioMixed.fetch_add(1, std::memory_order_relaxed);
    const int64_t mixStartUs = diagNowUs();

    // Wait-injection test mode: instead of faking a miss with a finished
    // mix (the old scheme, which needed fragile latch emulation), the test
    // makes the mix GENUINELY late - everything downstream is then the
    // real bail path.  Worker threads only; the sleep polls m_Abandon so
    // the callback's bail releases us (and the mutex) within ~50 us.
    if (!audio && m_Concealment)
      m_Concealment->MaybeInjectDelay(m_DeadlinePassed);

    /* initialise the output buffer */
    FillWithSilence();

    const unsigned nChannels = GetNChannels();

    for (unsigned i = 0; i < nChannels; i++) {
      for (unsigned j = 0; j < m_OutputCount; j++) {
        // Budget gone, shadow delivered: this period's mix is discarded
        // ("drop the late buffer") - return without publishing anything.
        if (m_Abandon.load(std::memory_order_relaxed))
          return;

        float factor = m_ScaleFactors[i * m_OutputCount + j];

        if (factor == 0)
          continue;

        GOSoundBufferTaskBase *output = m_Outputs[j / 2];

        output->Finish(m_Stop.load(), pThread);
        if (pThread && pThread->ShouldStop())
          return;

        AddChannelFrom(*output, j % 2, i, factor);
      }
    }

    if (m_Abandon.load(std::memory_order_relaxed))
      return; // keep the reverb state untouched by a discarded period

    m_Reverb->Process(GetData(), GetNFrames());

    /* Clamp the output and put the maximum amplitude to m_MeterInfo */
    float *pData = GetData();
    float *pMeterInfo = m_MeterInfo.data();

    for (unsigned nItems = GetNItems(), itemI = 0, channelI = 0; itemI < nItems;
         itemI++, pData++) {
      float f = std::clamp(*pData, CLAMP_MIN, CLAMP_MAX);
      float absF = std::abs(f);

      if (f != *pData)
        *pData = f;
      if (absF > pMeterInfo[channelI])
        pMeterInfo[channelI] = absF;

      // Move to next channel (circular: after last channel, wrap to first)
      channelI++;
      if (channelI >= nChannels)
        channelI = 0;
    }

    // Last gate before publishing: a bail may have struck during the
    // reverb/clamp above.  (Residual race: a bail arriving between this
    // check and the snapshot below double-appends the ring - the window is
    // microseconds wide, only reachable when the mix finishes within a
    // hair of the deadline, and self-heals after one period.)
    if (m_Abandon.load(std::memory_order_relaxed))
      return;

    m_DoneUs.store(
      std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count(),
      std::memory_order_relaxed);

    // Concealment history snapshot: still under the mutex (the buffer must
    // not change under us), a plain ring append.  The shadow prediction
    // itself happens on the concealment's own shadow thread.
    if (m_Concealment && GOSoundConcealment::IsShadowActive()) {
      // If this period was already concealed, the ring holds the
      // PREDICTION for it - overwrite that with what the mix really
      // produced instead of appending a second entry for one slot.
      m_StragglerRunning.store(false, std::memory_order_relaxed);
      if (
        GOSoundConcealment::GetStragglerTruth()
        && m_WasConcealed.exchange(false, std::memory_order_relaxed))
        m_Concealment->ReplaceConcealedWithReal(GetData());
      else
        m_Concealment->SnapshotMix(GetData());
      // Wake the shadow THREAD here, not a scheduler task.  This block is
      // single-threaded (mutex + m_Done guard) and runs only because the
      // mix finished and was not abandoned - the exact condition the
      // shadow needs, decided once.  The former gap task could not have
      // it: the task list dispatches on "handed out", not "finished", so
      // the task fired early, did nothing, and NextPeriod()'s Exec()
      // then ran the whole shadow ON THE CALLBACK THREAD - the monitor's
      // green curve was the shadow, one to one, growing with the AR
      // order and the frame size even at idle.
      m_Concealment->OpenShadow(audio);
    }

    m_Done.store(true);

    // Serial-mix cost: this whole block ran single-threaded (mutex + m_Done
    // guard) while the worker pool was idle - the Amdahl tail of the period.
    const int64_t mixDurUs = diagNowUs() - mixStartUs;
    s_MixUsSum.fetch_add(
      (uint64_t)(mixDurUs > 0 ? mixDurUs : 0), std::memory_order_relaxed);
    s_MixCount.fetch_add(1, std::memory_order_relaxed);
    for (int64_t old = s_MixUsMax.load(std::memory_order_relaxed);
         mixDurUs > old
         && !s_MixUsMax.compare_exchange_weak(
           old, mixDurUs, std::memory_order_relaxed);)
      ;
  }
}

void GOSoundOutputTask::Exec() { Run(); }

void GOSoundOutputTask::Finish(bool stop, GOSoundThread *pThread) {
  if (stop)
    m_Stop.store(true);
  if (!m_Done.load())
    Run(pThread);
}

void GOSoundOutputTask::Clear() {
  m_Reverb->Reset();
  ResetMeterInfo();
}

void GOSoundOutputTask::ResetMeterInfo() {
  {
    GOMutexLocker locker(m_Mutex);

    for (unsigned i = 0; i < m_MeterInfo.size(); i++)
      m_MeterInfo[i] = 0;
  }
  // We held the mutex: a NextPeriod reset may have deferred to us.
  ApplyPendingReset();
}

void GOSoundOutputTask::Reset() {
  // Called from NextPeriod on the callback thread: must NEVER block on a
  // straggling mixer (it can sit inside a GroupTask::Finish for the rest
  // of a saturated period - that wait turned overloads into xrun salvos).
  // Try-lock; on contention the holder applies the reset right after
  // releasing the mutex, and IsDone() reports not-done meanwhile.
  GOMutexLocker locker(m_Mutex, true, "GOSoundOutputTask::Reset");

  if (!locker.IsLocked()) {
    m_PendingReset.store(true, std::memory_order_release);
    return;
  }
  m_Done.store(false);
  m_Stop.store(false);
  m_Abandon.store(false, std::memory_order_relaxed);
  m_WasConcealed.store(false, std::memory_order_relaxed);
  m_DeadlinePassed.store(false, std::memory_order_relaxed);
  m_DoneUs.store(0, std::memory_order_relaxed);
  m_PendingReset.store(false, std::memory_order_relaxed);
}

unsigned GOSoundOutputTask::GetGroup() { return AUDIOOUTPUT; }

unsigned GOSoundOutputTask::GetCost() { return 0; }

bool GOSoundOutputTask::GetRepeat() { return false; }

void GOSoundOutputTask::SetupReverb(
  const GOSoundReverb::ReverbConfig &config,
  unsigned nSamplesPerBuffer,
  unsigned sampleRate) {
  m_Reverb->Setup(config, nSamplesPerBuffer, sampleRate);
}

void GOSoundOutputTask::SetupConcealment(
  unsigned sampleRate, const char *wisdomPath) {
  // Control thread, sound stopped (same contract as SetupReverb): safe to
  // replace the instance without locking against Run()/DeliverAudio().
  if (m_Concealment)
    delete m_Concealment;
  m_Concealment = new GOSoundConcealment(
    GetNChannels(), GetNFrames(), sampleRate, wisdomPath);
}

void GOSoundOutputTask::DeliverAudio(GOSoundBufferMutable &outBuffer) {
  if (m_Concealment)
    m_Concealment->Deliver(outBuffer, *this);
  else
    outBuffer.CopyFrom(*this);
}

void GOSoundOutputTask::DeliverBail(
  GOSoundBufferMutable &outBuffer, bool isLast) {
  // Keep the last-device flag plumbing of the normal Finish(isLast) path.
  if (isLast)
    m_Stop.store(true);
  // Stop the straggler first (it checks at loop boundaries), then send the
  // emergency buffer.  Unless straggler-truth is on: then the mix is left
  // running to completion, because its result - worthless for playback,
  // its slot is gone - is the TRUTH for the prediction history and breaks
  // the self-feeding loop at its source.
  const bool keepStraggler = GOSoundConcealment::GetStragglerTruth();

  // Wake the injected sleep in every case: it exists to make the mix
  // late, not to keep the task blocked.  Before this was separated from
  // m_Abandon, switching stragglers on made every injected miss sleep its
  // full 3x cap instead of ending at the bail - three periods of a
  // blocked OutputTask per miss, which is what actually drove the device
  // into underruns.
  m_DeadlinePassed.store(true, std::memory_order_relaxed);

  // At most ONE straggler in flight.  Abandoning is what bounds the work
  // per period - it skips the output groups' Finish(), which is where the
  // render time goes - so a second straggler starting while the first is
  // still running would remove the backpressure entirely and let the
  // backlog grow until the device really underruns.  A straggler
  // therefore gets exactly one extra period: if it has not finished by
  // the next miss, it is stopped after all.
  const bool everySecond = (++m_MissCounter & 1u) != 0;

  if (
    keepStraggler && everySecond
    && !m_StragglerRunning.exchange(true, std::memory_order_acq_rel))
    // Tells the epilog to REPLACE the prediction in the history rather
    // than append to it.  (Setting this unconditionally was a bug: an
    // abandoned mix never reaches the epilog, so the flag survived into
    // the next period and made a normal mix rewind the ring.)
    m_WasConcealed.store(true, std::memory_order_relaxed);
  else {
    m_Abandon.store(true, std::memory_order_relaxed);
    m_StragglerRunning.store(false, std::memory_order_relaxed);
    m_WasConcealed.store(false, std::memory_order_relaxed);
  }
  if (m_Concealment)
    m_Concealment->DeliverBail(outBuffer);
  else
    outBuffer.FillWithSilence();
}


const std::vector<float> &GOSoundOutputTask::GetMeterInfo() {
  return m_MeterInfo;
}
