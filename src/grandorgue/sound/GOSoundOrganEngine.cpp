/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundOrganEngine.h"

#include "GOFlushToZero.h"
#include "playing/GOSoundStream.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include "GOCrossfadeParam.h"
#include "buffer/GOSoundBufferMutable.h"
#include "config/GOConfig.h"
#include "fast_crossfade.h"
#include "model/GOOrganModel.h"
#include "model/GOPipe.h"
#include "model/GOWindchest.h"
#include "playing/GOSoundReleaseAlignTable.h"
#include "playing/GOSoundSampler.h"
#include "playing/GOSoundSubblockGrid.h"
#include "providers/GOSoundProvider.h"
#include "scheduler/GOSoundThread.h"
#include "GOSoundConcealment.h"
#include "tasks/GOSoundGroupTask.h"
#include "tasks/GOSoundOutputTask.h"
#include "tasks/GOSoundReleaseTask.h"
#include "tasks/GOSoundTouchTask.h"
#include "tasks/GOSoundTremulantTask.h"
#include "tasks/GOSoundWindchestTask.h"
#include "threading/GOMutexLocker.h"

#include "GOCrossfadeParam.h"
#include "GOEvent.h"
#include "GOSoundRecorder.h"
#include "GO_Attack_Parameters.h"
#include "fast_crossfade.h"

// needed for debugging the release model
#include "GO_DebugRelease.h"
#include "model/GORank.h"
#include "model/GOSoundingPipe.h"
#include <wx/filename.h>
#include <wx/log.h>

GOSoundOrganEngine::GOSoundOrganEngine()
  : m_PolyphonyLimiting(true),
    m_ScaledReleases(true),
    m_ReleaseAlignmentEnabled(true),
    m_RandomizeSpeaking(true),
    m_Volume(-15),
    m_SamplesPerBuffer(1),
    m_Gain(1),
    m_SampleRate(0),
    m_CurrentTime(1),
    m_SamplerPool(),
    m_AudioGroupCount(1),
    m_UsedPolyphony(0),
    m_UsedPolyphonyWindowPeak(0),
    m_PolyphonyPanicDrops(0),
    m_PolyphonyNormalDrops(0),
    m_PolyphonyDropsWindow(0),
    m_ProcessSamplerCallCount(0),
    m_PeriodStartUs(0),
    m_LongestPeriodCompletionUs(0),
    m_AudioThreadSchedInfo(-1),
    // 0 = no software prefetch (default since 2026-07-26): at high polyphony
    // the workload is DRAM-latency-capacity-bound and explicit prefetches
    // compete with demand misses for the line-fill buffers - measured
    // ceiling 24k -> 25.8k with prefetch off. The old "2 voices ahead"
    // helped only 1-2% PCM UTIL at low load; see the Audio Monitor's
    // Prefetch distance spinner to tune per-machine.
    m_PrefetchDistance(0),
    mp_MeterInfo(std::make_unique<std::vector<std::atomic<float>>>(0)),
    p_MeterInfo(mp_MeterInfo.get()),
    m_TremulantTasks(),
    m_WindchestTasks(),
    m_AudioGroupTasks(),
    m_AudioOutputTasks(),
    m_AudioRecorder(NULL),
    m_TouchTask(),
    m_HasBeenSetup(false) {
  m_SamplerPool.SetUsageLimit(2048);
  m_PolyphonySoftLimit = (m_SamplerPool.GetUsageLimit() * 3) / 4;
  m_ReleaseProcessor = new GOSoundReleaseTask(*this, m_AudioGroupTasks);
  Reset();
}

GOSoundOrganEngine::~GOSoundOrganEngine() {
  if (m_ReleaseProcessor)
    delete m_ReleaseProcessor;
}

void GOSoundOrganEngine::SetVolume(int volume) {
  m_Volume = volume;
  m_Gain = powf(10.0f, m_Volume * 0.05f);
}

void GOSoundOrganEngine::SetHardPolyphony(unsigned polyphony) {
  m_SamplerPool.SetUsageLimit(polyphony);
  m_PolyphonySoftLimit = (m_SamplerPool.GetUsageLimit() * 3) / 4;
}

void GOSoundOrganEngine::SetAudioGroupCount(unsigned groups) {
  if (groups < 1)
    groups = 1;
  m_AudioGroupCount = groups;
  m_AudioGroupTasks.clear();
  for (unsigned i = 0; i < m_AudioGroupCount; i++)
    m_AudioGroupTasks.push_back(
      new GOSoundGroupTask(*this, m_SamplesPerBuffer));
}

void GOSoundOrganEngine::SetAudioOutput(
  std::vector<GOAudioOutputConfiguration> audio_outputs) {
  m_AudioOutputTasks.clear();
  {
    std::vector<float> scale_factors;
    scale_factors.resize(m_AudioGroupCount * 2 * 2);
    std::fill(scale_factors.begin(), scale_factors.end(), 0.0f);
    for (unsigned i = 0; i < m_AudioGroupCount; i++) {
      scale_factors[i * 4] = 1;
      scale_factors[i * 4 + 3] = 1;
    }
    m_AudioOutputTasks.push_back(
      new GOSoundOutputTask(2, scale_factors, m_SamplesPerBuffer));
  }
  unsigned channels = 0;
  for (unsigned i = 0; i < audio_outputs.size(); i++) {
    std::vector<float> scale_factors;
    scale_factors.resize(m_AudioGroupCount * audio_outputs[i].channels * 2);
    std::fill(scale_factors.begin(), scale_factors.end(), 0.0f);
    for (unsigned j = 0; j < audio_outputs[i].channels; j++)
      for (unsigned k = 0; k < audio_outputs[i].scale_factors[j].size(); k++) {
        if (k >= m_AudioGroupCount * 2)
          continue;
        float factor = audio_outputs[i].scale_factors[j][k];
        if (factor >= -120 && factor < 40)
          factor = powf(10.0f, factor * 0.05f);
        else
          factor = 0;
        scale_factors[j * m_AudioGroupCount * 2 + k] = factor;
      }
    m_AudioOutputTasks.push_back(new GOSoundOutputTask(
      audio_outputs[i].channels, scale_factors, m_SamplesPerBuffer));
    channels += audio_outputs[i].channels;
  }
  std::vector<GOSoundBufferTaskBase *> outputs;
  for (unsigned i = 0; i < m_AudioGroupTasks.size(); i++)
    outputs.push_back(m_AudioGroupTasks[i]);
  for (unsigned i = 0; i < m_AudioOutputTasks.size(); i++)
    m_AudioOutputTasks[i]->SetOutputs(outputs);
  // No gap tasks: the shadow runs on GOSoundConcealment's own thread
  // (FIFO 61), woken directly by each mix epilog.  As a scheduler task it
  // was dispatched on "handed out", not "finished", fired early, did
  // nothing - and NextPeriod()'s Exec() then ran it on the CALLBACK
  // thread, which is where the monitor's green curve got its shadow
  // cost from.
  {
    // Control thread: prevent GetMeterInfo() from reading the buffer while it
    // is being replaced. SetAudioOutput() is called only when all audio
    // callbacks are stopped (between StopSoundSystem and StartSoundSystem),
    // so the audio thread is not accessing p_MeterInfo concurrently.
    GOMutexLocker locker(m_MeterMutex);

    if (mp_MeterInfo->size() != channels) {
      auto newInfo
        = std::make_unique<std::vector<std::atomic<float>>>(channels);

      // Prior to C++20, std::atomic default construction does not
      // zero-initialize the value; initialize explicitly for safety.
      for (auto &v : *newInfo)
        v.store(0.0f, std::memory_order_relaxed);

      // Store the new pointer before destroying the old vector so that
      // p_MeterInfo always points to a valid object.
      // release: the audio thread will see the new vector (size + data) via
      // acquire-load of p_MeterInfo on the next NextPeriod() call.
      p_MeterInfo.store(newInfo.get(), std::memory_order_release);
      mp_MeterInfo = std::move(newInfo);
    }
  }
}

void GOSoundOrganEngine::SetupReverb(GOConfig &settings) {
  const GOSoundReverb::ReverbConfig reverbConfig
    = GOSoundReverb::createReverbConfig(settings);

  // Cached FFTW plans for the concealment predictor: measured once per
  // machine and reused from here on every later start.
  const wxString wisdomPath = settings.OrganCachePath()
    + wxFileName::GetPathSeparator() + wxT("fftw-wisdomf.dat");

  for (unsigned i = 0; i < m_AudioOutputTasks.size(); i++)
    if (m_AudioOutputTasks[i]) {
      m_AudioOutputTasks[i]->SetupReverb(
        reverbConfig, settings.SamplesPerBuffer(), settings.SampleRate());
      // Deadline-miss concealment; index 0 is the recorder downmix task,
      // which is never delivered to a device (GetAudioOutput uses i+1).
      if (i > 0)
        m_AudioOutputTasks[i]->SetupConcealment(
          settings.SampleRate(), wisdomPath.utf8_str());
    }
}

void GOSoundOrganEngine::SetAudioRecorder(
  GOSoundRecorder *recorder, bool downmix) {
  m_AudioRecorder = recorder;
  std::vector<GOSoundBufferTaskBase *> outputs;
  if (downmix)
    outputs.push_back(m_AudioOutputTasks[0]);
  else {
    m_Scheduler.Remove(m_AudioOutputTasks[0]);
    delete m_AudioOutputTasks[0];
    m_AudioOutputTasks[0] = NULL;
    for (unsigned i = 1; i < m_AudioOutputTasks.size(); i++)
      outputs.push_back(m_AudioOutputTasks[i]);
  }
  m_AudioRecorder->SetOutputs(outputs, m_SamplesPerBuffer);
}

std::vector<float> GOSoundOrganEngine::GetMeterInfo() {
  // GUI thread: m_MeterMutex prevents concurrent replacement by SetAudioOutput
  GOMutexLocker locker(m_MeterMutex);
  std::vector<std::atomic<float>> &info = *p_MeterInfo.load();
  const unsigned hardPolyphony = GetHardPolyphony();
  std::vector<float> result(info.size() + 1);
  float *pResult = result.data();

  assert(hardPolyphony > 0);
  // exchange(0) reads accumulated max and resets it for the next poll
  *(pResult++)
    = m_UsedPolyphony.exchange(0) / static_cast<float>(hardPolyphony);
  for (std::atomic<float> &v : info)
    *(pResult++) = v.exchange(0.0f);
  return result;
}

void GOSoundOrganEngine::Reset() {
  if (m_HasBeenSetup.load()) {
    for (unsigned i = 0; i < m_WindchestTasks.size(); i++)
      m_WindchestTasks[i]->Init(m_TremulantTasks);
  }

  m_Scheduler.Clear();

  if (m_HasBeenSetup.load()) {

    for (unsigned i = 0; i < m_TremulantTasks.size(); i++)
      m_Scheduler.Add(m_TremulantTasks[i]);
    for (unsigned i = 0; i < m_WindchestTasks.size(); i++)
      m_Scheduler.Add(m_WindchestTasks[i]);
    for (unsigned i = 0; i < m_AudioGroupTasks.size(); i++)
      m_Scheduler.Add(m_AudioGroupTasks[i]);
    for (unsigned i = 0; i < m_AudioOutputTasks.size(); i++)
      m_Scheduler.Add(m_AudioOutputTasks[i]);
    m_Scheduler.Add(m_AudioRecorder);
    m_Scheduler.Add(m_ReleaseProcessor);
    if (m_TouchTask)
      m_Scheduler.Add(m_TouchTask.get());
  }
  m_UsedPolyphony.store(0);
  m_UsedPolyphonyWindowPeak.store(0);
  m_PolyphonyDropsWindow.store(0);
  m_ProcessSamplerCallCount.store(0, std::memory_order_relaxed);
  m_FastPathVoicesWindow.store(0, std::memory_order_relaxed);
  m_TotalVoicesWindow.store(0, std::memory_order_relaxed);
  for (auto &counter : m_FaderKernelWindow)
    counter.store(0, std::memory_order_relaxed);
  m_CompressedVoicesWindow.store(0, std::memory_order_relaxed);
  m_OrphanSuspectStreak = 0;
  m_OrphanLeakLogged = false;
  m_AudioThreadSchedInfo.store(-1, std::memory_order_relaxed);
  m_SamplerPool.ReturnAll();
  m_CurrentTime = 1;
  m_Scheduler.Reset();
}

void GOSoundOrganEngine::Setup(
  GOOrganModel &organModel, GOMemoryPool &memoryPool, unsigned releaseCount) {
  m_Scheduler.Clear();
  if (releaseCount < 1)
    releaseCount = 1;
  m_Scheduler.SetRepeatCount(releaseCount);
  m_TremulantTasks.clear();
  for (unsigned i = 0; i < organModel.GetTremulantCount(); i++)
    m_TremulantTasks.push_back(
      new GOSoundTremulantTask(*this, m_SamplesPerBuffer));
  m_WindchestTasks.clear();
  // a special windchest task for detached releases
  m_WindchestTasks.push_back(new GOSoundWindchestTask(*this, NULL));
  for (unsigned i = 0; i < organModel.GetWindchestCount(); i++)
    m_WindchestTasks.push_back(
      new GOSoundWindchestTask(*this, organModel.GetWindchest(i)));
  m_TouchTask
    = std::unique_ptr<GOSoundTouchTask>(new GOSoundTouchTask(memoryPool));
  // Precompute fast crossfade templates for the lengths actually used by
  // this organ (attack-switch crossfade, release crossfade, release tail),
  // so the exact (mode, len) entries needed during play already exist
  // instead of being built lazily on the audio thread on first key release.
  // Templates are also still created on-demand by the fader as a fallback.
  // The cache is cleared first so templates for a previously loaded organ,
  // sample rate or crossfade mode don't accumulate for the process lifetime
  // (fades already in progress keep their own shared_ptr to their template,
  // see FastCrossfadeCache::Clear()).
  {
    using namespace GOAudioParams;
    const auto mode = GetCrossfadeMode();
    std::vector<unsigned> lengths;

    if (mode != GOCrossfadeMode::Linear)
      for (unsigned ms : organModel.GetCrossfadeLengthsMs())
        lengths.push_back(MsToSamples(ms));
    FastCrossfadeCache::BeginPrecomputeJob();
    std::thread([mode, lengths = std::move(lengths)]() {
      FastCrossfadeCache::Clear();
      FastCrossfadeCache::PrecomputeForMode(mode, lengths);
      FastCrossfadeCache::EndPrecomputeJob();
    }).detach();
  }
  m_HasBeenSetup.store(true);
  Reset();
}

void GOSoundOrganEngine::ClearSetup() {
  m_HasBeenSetup.store(false);

  // the winchests may be still used from audio callbacks.
  // clear the pending sound before destroying the windchests
  for (unsigned i = 0; i < m_AudioGroupTasks.size(); i++)
    m_AudioGroupTasks[i]->WaitAndClear();

  m_Scheduler.Clear();
  m_WindchestTasks.clear();
  m_TremulantTasks.clear();
  m_TouchTask = NULL;
  Reset();
}

float GOSoundOrganEngine::GetRandomFactor() const {
  if (m_RandomizeSpeaking) {
    const double factor = (pow(2, 1.0 / 1200.0) - 1) / (RAND_MAX / 2);
    int num = rand() - RAND_MAX / 2;
    return 1 + num * factor;
  }
  return 1;
}

void GOSoundOrganEngine::PassSampler(GOSoundSampler *sampler) {
  int taskId = sampler->m_SamplerTaskId;

  if (isWindchestTask(taskId))
    m_AudioGroupTasks[sampler->m_AudioGroupId]->Add(sampler);
  else
    m_TremulantTasks[tremulantTaskToIndex(taskId)]->Add(sampler);
}

void GOSoundOrganEngine::StartSampler(GOSoundSampler *sampler) {
  int taskId = sampler->m_SamplerTaskId;

  sampler->stop = 0;
  sampler->new_attack = 0;
  sampler->p_WindchestTask = isWindchestTask(taskId)
    ? m_WindchestTasks[windchestTaskToIndex(taskId)]
    : nullptr;
  PassSampler(sampler);
}

bool GOSoundOrganEngine::ProcessSamplerPreRender(GOSoundSampler *sampler) {
  const bool process_sampler = (sampler->time <= m_CurrentTime);

  // Leak/stuck-voice diagnostics - see m_ProcessSamplerCallCount above.
  // Clocked from fade-out start, not sampler start: natural full-length
  // releases never start a fade-out (they end by stream exhaustion, not via
  // IsSilent()) and may legitimately play for 10s+, while every fade-out
  // (panic ~20ms, normal drop <=1.5s, crossfade ~184ms, release tail <=
  // a few s) is short - a sampler still audible kStaleReleaseAgeMs after
  // its fade-out started is genuinely stuck.
  m_ProcessSamplerCallCount.fetch_add(1, std::memory_order_relaxed);
  // Monitor-only window counters: each is an atomic read-modify-write, so
  // they are gated on the (plain-load, branch-predicted) master switch and
  // cost nothing while the Audio Monitor is closed.
  const bool voiceStats = m_VoiceStatsEnabled.load(std::memory_order_relaxed);

  if (voiceStats) {
    m_TotalVoicesWindow.fetch_add(1, std::memory_order_relaxed);
    if (sampler->stream.IsIdentityResample())
      m_FastPathVoicesWindow.fetch_add(1, std::memory_order_relaxed);
    if (sampler->stream.IsCompressedSection())
      m_CompressedVoicesWindow.fetch_add(1, std::memory_order_relaxed);
  }
  if (
    sampler->is_release && !sampler->stale_release_logged
    && sampler->fade_out_start_time
    && (m_CurrentTime - sampler->fade_out_start_time)
      > MsToSamples(kStaleReleaseAgeMs)
    && !sampler->fader.IsSilent()) {
    sampler->stale_release_logged = true;
    wxLogWarning(
      "GOSoundOrganEngine: release sampler still audible %ums after its "
      "fade-out started (sampler age %ums, drop_state=%d, "
      "UsedSamplerCount=%u, SoftLimit=%u, PolyphonyLimiting=%d)",
      SamplesDiffToMs(sampler->fade_out_start_time, m_CurrentTime),
      SamplesDiffToMs(sampler->time, m_CurrentTime),
      (int)sampler->polyphony_drop_state,
      m_SamplerPool.UsedSamplerCount(),
      m_PolyphonySoftLimit,
      (int)m_PolyphonyLimiting);
  }

  if (process_sampler) {
    // Polyphony load dropping. Runs BEFORE any rendering because
    // StartDecreasingVolume mutates the fader state the render's gain
    // computation must already see.
    //
    // StartDecreasingVolume(..., forceLinear=true)
    // has no guard against being re-triggered while a fade-out is already
    // running (unlike the non-linear path in GOSoundFader), and the drop
    // conditions below stay true for as long as the sampler remains
    // over-limit - i.e. on every subsequent audio block, not just once.
    // Gate each drop on sampler->polyphony_drop_state so it fires exactly
    // once per sampler; otherwise the per-call random duration gets
    // overwritten every block and averages out, making concurrently
    // triggered drops fade out in near-lockstep instead of being spread out.

    if (sampler->is_release && sampler->normal_drop_age_threshold == 0)
      // Picked once, as soon as this sampler starts releasing - not lazily
      // inside the load-gated check below. The load ratio is a single
      // shared value: without this, every release sampler already past a
      // *fixed* age threshold would become eligible in the very same block
      // the instant load crosses 80%, no matter how the fade length itself
      // is randomized. Assigning each sampler its own trigger age up front
      // means concurrently-released notes (e.g. a chord) already diverge
      // before load ever crosses the threshold.
      // 500-1500ms, expressed via MsToSamples() so it scales with the
      // configured sample rate instead of being a fixed sample count.
      sampler->normal_drop_age_threshold = MsToSamples(500 + (rand() % 1001));

    // panic mode load dropping, if Soft limit gets exceeded
    if (sampler->is_release && m_PolyphonyLimiting &&
        m_SamplerPool.UsedSamplerCount() >= m_PolyphonySoftLimit &&
        m_CurrentTime - sampler->time > MsToSamples(42)) {
      if (sampler->polyphony_drop_state != GOSoundPolyphonyDropState::Panic) {
        sampler->polyphony_drop_state = GOSoundPolyphonyDropState::Panic;
        // Panic fade: intentionally short (10–14 ms). The hard polyphony
        // ceiling is enforced by MaxPolyphony (new voices are rejected there).
        // The soft/panic percentage thresholds exist to guarantee a pool of
        // free samplers is always available for incoming attacks — if every
        // sampler slot is occupied, new notes simply cannot start. Panic mode
        // must free slots fast; a slow fade (like the 500 ms normal drop)
        // would leave the pool exhausted for too long. Keep this narrow.
        // forceLinear: curve shape is irrelevant for a panic fade, and the
        // randomized length would otherwise churn FastCrossfadeCache.
        // 20ms + rand()%5 - 10 → [10, 14] ms spread avoids synchronized drops.
        sampler->fader.StartDecreasingVolume(
          MsToSamples(20 + (rand() % 5) - 10), true);
        sampler->fade_out_start_time = m_CurrentTime;
        m_PolyphonyPanicDrops.fetch_add(1, std::memory_order_relaxed);
        m_PolyphonyDropsWindow.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // normal randomized load dropping
    else if (
      sampler->is_release && m_PolyphonyLimiting &&
      m_SamplerPool.UsedSamplerCount() * 5 >= m_PolyphonySoftLimit * 4 &&
      m_CurrentTime - sampler->time > sampler->normal_drop_age_threshold) {
      if (sampler->polyphony_drop_state == GOSoundPolyphonyDropState::None) {
        sampler->polyphony_drop_state = GOSoundPolyphonyDropState::Normal;
        // Random jitter time to prevent concurrent drops from synchronizing.
        sampler->fader.StartDecreasingVolume(
          MsToSamples(500 + (rand() % 1000)), true);
        sampler->fade_out_start_time = m_CurrentTime;
        m_PolyphonyNormalDrops.fetch_add(1, std::memory_order_relaxed);
        m_PolyphonyDropsWindow.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  return process_sampler;
}

void GOSoundOrganEngine::AccumulateSamplerBuffer(
  float *output_buffer,
  GOSoundSampler *sampler,
  unsigned n_frames,
  const float *temp,
  float volume) {
  const bool voiceStats = m_VoiceStatsEnabled.load(std::memory_order_relaxed);

  if (!sampler->toneBalanceFilterState.IsToApply()) {
    // Fader scales and accumulates directly into output_buffer
    const GOFadeKernelKind kernel = sampler->fader.ProcessAndAccumulate(
      n_frames, temp, output_buffer, volume);

    if (voiceStats)
      m_FaderKernelWindow[static_cast<unsigned>(kernel)].fetch_add(
        1, std::memory_order_relaxed);
  } else {
    // Legacy path (no fused accumulation or ToneBalance active).
    // Process() does not modify its input in the fused sense, but it does
    // scale in place - copy first so callers may reuse their buffer.
    float scaled[n_frames * 2];

    memcpy(scaled, temp, sizeof(float) * n_frames * 2);
    if (voiceStats)
      m_FaderKernelWindow[kFaderKernelLegacyIdx].fetch_add(
        1, std::memory_order_relaxed);
    sampler->fader.Process(n_frames, scaled, volume);
    if (sampler->toneBalanceFilterState.IsToApply())
      sampler->toneBalanceFilterState.ProcessBuffer(n_frames, scaled);

    /* Add these samples to the current output buffer shifting
     * right by the necessary amount to bring the sample gain back
     * to unity (this value is computed in GOPipe.cpp)
     */
    for (unsigned i = 0; i < n_frames * 2; i++)
      output_buffer[i] += scaled[i];
  }
}

bool GOSoundOrganEngine::ProcessSamplerPostRender(
  GOSoundSampler *sampler, bool wasProcessed) {
  if (wasProcessed) {
    if (
      (sampler->stop && sampler->stop <= m_CurrentTime)
      || (sampler->new_attack && sampler->new_attack <= m_CurrentTime)) {
      // Leaving its group task's persistent bucket: invalidate the bucket
      // entry BEFORE the release processor can see (and re-home) the
      // sampler - see GOSoundSampler::m_ResidenceToken.
      sampler->m_ResidenceToken.store(0, std::memory_order_relaxed);
      m_ReleaseProcessor->Add(sampler);
      return false;
    }
  }

  if (
    !sampler->p_SoundProvider
    || (sampler->fader.IsSilent() && wasProcessed)) {
    ReturnSampler(sampler);
    return false;
  } else
    return true;
}

bool GOSoundOrganEngine::ProcessSampler(
  float *output_buffer,
  GOSoundSampler *sampler,
  unsigned n_frames,
  float volume) {
  const bool process_sampler = ProcessSamplerPreRender(sampler);

  if (process_sampler) {
    float temp[n_frames * 2];

    /* The decoded sampler frame will contain values containing
     * sampler->pipe_section->sample_bits worth of significant bits.
     * It is the responsibility of the fade engine to bring these bits
     * back into a sensible state. This is achieved during setup of the
     * fade parameters. The gain target should be:
     *
     *     playback gain * (2 ^ -sampler->pipe_section->sample_bits)
     */
    if (!sampler->stream.ReadBlock(temp, n_frames))
      sampler->p_SoundProvider = NULL;

    AccumulateSamplerBuffer(output_buffer, sampler, n_frames, temp, volume);
  }
  return ProcessSamplerPostRender(sampler, process_sampler);
}

void GOSoundOrganEngine::ProcessRelease(GOSoundSampler *sampler) {
  if (sampler->stop) {
    CreateReleaseSampler(sampler);
    sampler->stop = 0;
  } else if (sampler->new_attack) {
    SwitchToAnotherAttack(sampler);
    sampler->new_attack = 0;
  }
  PassSampler(sampler);
}

void GOSoundOrganEngine::ReturnSampler(GOSoundSampler *sampler) {
  // Invalidate any persistent-bucket entry still referencing this voice
  // (lazy removal - see GOSoundSampler::m_ResidenceToken) before the pool
  // may recycle it.
  sampler->m_ResidenceToken.store(0, std::memory_order_relaxed);
  m_SamplerPool.ReturnSampler(sampler);
}

unsigned GOSoundOrganEngine::GetBufferSizeFor(
  unsigned outputIndex, unsigned nFrames) const {
  return sizeof(float) * nFrames
    * m_AudioOutputTasks[outputIndex + 1]->GetNChannels();
}

void GOSoundOrganEngine::GetAudioOutput(
  unsigned outputIndex, bool isLast, GOSoundBufferMutable &outBuffer) {
  if (!m_HasBeenSetup.load()) {
    outBuffer.FillWithSilence();
    return;
  }

  GOSoundOutputTask *pOutputTask = m_AudioOutputTasks[outputIndex + 1];

  /* Grace 100 = legacy delivery (A/B reference): the pre-concealment
   * blocking Finish, INCLUDING the callback-help contribution (the audio
   * thread may win the mutex and mix/render itself - measurable work at
   * the polyphony ceiling that the poll loop below deliberately gives
   * up).  No budget watch, no bail: real misses behave like old GO. */
  const int gracePct = GOSoundConcealment::GetMixGracePct();

  if (gracePct >= 100) {
    pOutputTask->Finish(isLast);
    pOutputTask->DeliverAudio(outBuffer);
    return;
  }

  /* Budget watch (deadline-miss concealment): wait for the real mix, but
   * never past (entry + period * grace).  Mix grace is the tolerated
   * lateness of a mix - and every microsecond spent waiting is taken
   * from the NEXT period's head start, because NextPeriod/WakeupThreads
   * run only after this returns (root-cause analysis 12.08.2026).  The
   * remaining (100 - grace) percent must therefore cover the delivery
   * chain, the next mix AND the shadow round; see the dimensioning rule
   * at GOSoundConcealment::SetMixGracePct().
   *
   * This replaces the former unconditional (and unbounded) blocking
   * Finish(): the callback POLLS m_Done instead of parking on the
   * OutputTask mutex - on Linux that mutex wait was the whole story of
   * "callback help" anyway.  When the mix is already done (the normal
   * case), the cost is one atomic load. */
  if (!pOutputTask->IsDone()) {
    const auto entry = std::chrono::steady_clock::now();
    const int64_t budgetUs
      = (int64_t)m_SamplesPerBuffer * 1000000 / (int64_t)m_SampleRate;
    const auto bailAt
      = entry + std::chrono::microseconds(budgetUs * gracePct / 100);

    while (!pOutputTask->IsDone()) {
      /* Callback-help first, per the existing policy knob: contributing
       * the mix on the audio thread is worth a real slice of the
       * polyphony ceiling (measured: Reserve=0 vs 200 decides the 24k
       * benchmark), at the price of committing - once mixing, we cannot
       * bail, and a deep overload then overshoots into PipeWire's
       * one-cycle pipeline slack exactly like the legacy path.  Policy
       * "never" keeps the pure bail path for maximum delivery
       * punctuality. */
      // Help by TRYING to win the mix mutex, never by blocking on it: a
      // blocking Finish() here took the first loop iteration every time
      // (the default policy always permits help) and thereby made the
      // bail below unreachable - the concealment was silently dead.
      if (MayCallbackHelp() && pOutputTask->TryHelpMix(isLast))
        break;
      if (std::chrono::steady_clock::now() >= bailAt) {
        // Budget gone: abandon the straggler ("drop the late buffer") and
        // send the emergency shadow (or silence, per concealment mode).
        GOSoundConcealment::RecordDeliveryWait(
          std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - entry)
            .count(),
          true);
        pOutputTask->DeliverBail(outBuffer, isLast);
        return;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(15));
    }
    // The real mix arrived while we were waiting - count it, so "sounds
    // fine" can be told apart from "concealment fired" in the monitor.
    GOSoundConcealment::RecordDeliveryWait(
      std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - entry)
        .count(),
      false);
  }

  pOutputTask->Finish(isLast); // done: only records the last-device flag
  pOutputTask->DeliverAudio(outBuffer);
}

/**
 * Atomically updates maxValue to max(maxValue, value) with relaxed ordering.
 * std::atomic<T>::fetch_max is only available in C++26.
 */
template <typename T>
static void atomic_fetch_max_relaxed(std::atomic<T> &maxValue, T value) {
  T oldMax = maxValue.load(std::memory_order_relaxed);

  while (oldMax < value
         && !maxValue.compare_exchange_weak(
           oldMax, value, std::memory_order_relaxed))
    ;
}

bool GOSoundOrganEngine::MayCallbackHelp() const {
  const int policy = m_CallbackHelpPolicy.load(std::memory_order_relaxed);

  if (policy == 0)
    return true;
  if (policy == 1)
    return false;
  // Deadline-aware: only lend the TIME_CRITICAL thread once ~70% of the
  // period budget has elapsed and the work is evidently late.
  const int64_t startUs = m_PeriodStartUs.load(std::memory_order_relaxed);

  if (startUs == 0)
    return true;

  const int64_t budgetUs
    = (int64_t)m_SamplesPerBuffer * 1000000 / (int64_t)m_SampleRate;

  const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();

  return nowUs - startUs > (budgetUs * 7) / 10;
}

void GOSoundOrganEngine::NextPeriod() {
  /* Callback interarrival jitter: NextPeriod runs once per audio callback on
   * the callback thread, so the wall-clock gap between calls is the callback
   * delivery interval. Ideal = one buffer period; a fat max / high late-count
   * means the backend delivers callbacks irregularly (tests whether jitter,
   * not compute, tips periods over the 2.67 ms edge). */
  {
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const int64_t last = m_LastCbUs.exchange(nowUs, std::memory_order_relaxed);

    if (last != 0 && m_SampleRate != 0) {
      const int64_t gap = nowUs - last;
      const int64_t budgetUs
        = (int64_t)m_SamplesPerBuffer * 1000000 / (int64_t)m_SampleRate;

      m_CbCount.fetch_add(1, std::memory_order_relaxed);
      if (gap > (budgetUs * 3) / 2)
        m_CbLateCount.fetch_add(1, std::memory_order_relaxed);
      for (int64_t old = m_CbInterMinUs.load(std::memory_order_relaxed);
           gap < old
           && !m_CbInterMinUs.compare_exchange_weak(
             old, gap, std::memory_order_relaxed);)
        ;
      for (int64_t old = m_CbInterMaxUs.load(std::memory_order_relaxed);
           gap > old
           && !m_CbInterMaxUs.compare_exchange_weak(
             old, gap, std::memory_order_relaxed);)
        ;
    }
  }

  /* CPU-affinity experiment (Audio Monitor toggle, see
   * GOSoundThread::s_AffinityMode): apply the audio-thread mask lazily on
   * this thread when the mode changes. m_AffinityApplied is audio-thread-
   * private. */
  {
    const int mode
      = GOSoundThread::s_AffinityMode.load(std::memory_order_relaxed);

    if (mode != m_AffinityApplied) {
      GOSoundThread::ApplyAudioAffinity(mode);
      m_AffinityApplied = mode;
    }
  }
  m_AudioCpuProbe.Sample();

  // One-shot RT-priority detection on the audio callback thread. Stored so
  // the GUI can include it in a diagnostics report without needing to know
  // which thread the callback runs on.
  if (m_AudioThreadSchedInfo.load(std::memory_order_relaxed) < 0) {
    int policy = 0;
#ifdef __linux__
    policy = sched_getscheduler(0);
    if (policy < 0)
      policy = 0;
    // On Windows the backend elevates the callback thread (MMCSS/ASIO), and
    // under JACK the server hands us an RT thread - but plain ALSA/Pulse
    // callbacks arrive at SCHED_OTHER. Elevate above the workers' SCHED_FIFO
    // 60 (GOSoundThread), otherwise the RT workers can preempt the one
    // thread that owns the deadline. Needs an rtprio rlimit or CAP_SYS_NICE;
    // on failure retry clamped to the rlimit, else stay at normal priority
    // (the Audio Monitor shows the outcome either way).
    if (policy != SCHED_FIFO && policy != SCHED_RR) {
      struct sched_param sp = {};

      sp.sched_priority = 70;
      if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0)
        policy = SCHED_FIFO;
      else {
        struct rlimit rl;

        if (
          getrlimit(RLIMIT_RTPRIO, &rl) == 0 && rl.rlim_cur > 0
          && rl.rlim_cur < 70) {
          sp.sched_priority = (int)rl.rlim_cur;
          if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0)
            policy = SCHED_FIFO;
        }
      }
    }
#elif defined(_WIN32)
    policy = GetThreadPriority(GetCurrentThread());
#endif
    m_AudioThreadSchedInfo.store(policy, std::memory_order_relaxed);

    // Same denormal guard as the workers (GOFlushToZero.h): the audio
    // callback thread runs the final mix and, with cbHelp, render code too.
    GOEnableFlushToZero();
  }

  // Completion latency for the period that's ending: how long from
  // WakeupThreads() (m_PeriodStartUs) until the last GOSoundOutputTask
  // actually finished (whichever thread did it - background worker or the
  // realtime audio thread via Finish()). Read before m_Scheduler.Reset()
  // below clears the per-task m_DoneUs/m_Done for the next period.
  {
    const int64_t startUs = m_PeriodStartUs.load(std::memory_order_relaxed);

    if (startUs != 0) {
      int64_t maxDoneUs = 0;

      for (unsigned i = 1; i < m_AudioOutputTasks.size(); i++)
        maxDoneUs = std::max(maxDoneUs, m_AudioOutputTasks[i]->GetDoneUs());
      if (maxDoneUs != 0) {
        const int64_t completionUs = maxDoneUs - startUs;

        atomic_fetch_max_relaxed(m_LongestPeriodCompletionUs, completionUs);
        // Spaete-Scheiben-Diagnose: dieselbe Latenz, getrennt nach dem
        // Weckkontext des Mixes (Vorlieferung Bail oder Pass).
        GOSoundConcealment::RecordMixCompletion(completionUs);

        /* Throughput sample: what did ONE voice-frame cost this period?
         * (see m_VoiceCostPsRing). Only meaningful under real load, hence
         * the voice floor. Realtime-safe: one relaxed store into a ring. */
        {
          const uint64_t voices
            = m_UsedPolyphony.load(std::memory_order_relaxed);

          if (voices >= VOICE_COST_MIN_VOICES && m_SamplesPerBuffer != 0) {
            const uint64_t voiceFrames = voices * m_SamplesPerBuffer;
            const uint64_t ps
              = (uint64_t)completionUs * 1000000ull / voiceFrames;
            const unsigned slot
              = m_VoiceCostWrites.fetch_add(1, std::memory_order_relaxed)
              % VOICE_COST_RING;

            m_VoiceCostPsRing[slot].store(
              (uint32_t)std::min<uint64_t>(ps, UINT32_MAX),
              std::memory_order_relaxed);
          }
        }

        // Sustained-overrun detector - see the member comment.
        if (m_SampleRate != 0) {
          const int64_t budgetUs
            = (int64_t)m_SamplesPerBuffer * 1000000 / m_SampleRate;

          if (completionUs > budgetUs) {
            if (++m_OverBudgetRun >= 3)
              m_SustainedOverrunPeriods.fetch_add(
                1, std::memory_order_relaxed);
          } else
            m_OverBudgetRun = 0;
        }
      }
    }
  }

  // Barrier-tail: fold this just-completed period's first->last worker
  // spread into the running sum, before WakeupThreads() (called by the
  // sound system right after NextPeriod) resets it for the next period.
  GOSoundThread::CapturePeriodTail();

  m_Scheduler.Exec();

  m_CurrentTime += m_SamplesPerBuffer;
  {
    const unsigned usedSamplers = m_SamplerPool.UsedSamplerCount();

    atomic_fetch_max_relaxed(m_UsedPolyphony, usedSamplers);
    atomic_fetch_max_relaxed(m_UsedPolyphonyWindowPeak, usedSamplers);

    // Leak/stuck-voice diagnostics - see m_ProcessSamplerCallCount above.
    // Tolerance absorbs the natural one-period skew from samplers created or
    // returned while this period's processing was in flight; the streak
    // threshold requires the gap to persist for a while before logging,
    // since only a genuine leak stays elevated instead of settling back down.
    constexpr unsigned kOrphanGapTolerance = 8;
    constexpr unsigned kOrphanStreakThreshold = 200;
    const uint64_t processedLastPeriod
      = m_ProcessSamplerCallCount.exchange(0, std::memory_order_relaxed);

    m_LastProcessedSamplerCount.store(
      processedLastPeriod, std::memory_order_relaxed);

    if (usedSamplers > processedLastPeriod + kOrphanGapTolerance) {
      if (
        ++m_OrphanSuspectStreak >= kOrphanStreakThreshold
        && !m_OrphanLeakLogged) {
        m_OrphanLeakLogged = true;
        m_OrphanLeakDetected.store(true, std::memory_order_relaxed);
        wxLogWarning(
          "GOSoundOrganEngine: sustained gap between UsedSamplerCount (%u) "
          "and samplers reachable from processing lists (%llu) over %u "
          "periods - possible orphaned sampler leak",
          usedSamplers,
          (unsigned long long)processedLastPeriod,
          kOrphanStreakThreshold);
      }
    } else {
      m_OrphanSuspectStreak = 0;
      m_OrphanLeakLogged = false;
    }
  }

  // Audio thread: load with acquire so that the new vector written by
  // SetAudioOutput() (store with release) is visible after a Stop/Start cycle.
  // Values accumulate between GUI polls; GetMeterInfo() resets via exchange(0).
  std::vector<std::atomic<float>> *const pMeterInfo
    = p_MeterInfo.load(std::memory_order_acquire);
  const std::atomic<float> *const pMeterEnd
    = pMeterInfo->data() + pMeterInfo->size();
  std::atomic<float> *pMeter = pMeterInfo->data();

  // task[0] is the downmix/recorder task; its channels have never been part of
  // the meter display. mp_MeterInfo holds exactly the real output channels from
  // tasks[1..N]. GOFrame::OnMeters guards against size mismatches with
  // vals.size() == m_VolumeGauge.size() + 1, so a temporary count change
  // (e.g. during Stop/Start) safely skips one GUI tick rather than crashing.
  for (unsigned i = 1; i < m_AudioOutputTasks.size(); i++) {
    GOSoundOutputTask *const pTask = m_AudioOutputTasks[i];

    assert(pTask);
    for (const float f : pTask->GetMeterInfo()) {
      assert(pMeter < pMeterEnd);
      atomic_fetch_max_relaxed(*(pMeter++), f);
    }
    pTask->ResetMeterInfo();
  }

  m_Scheduler.Reset();
}

unsigned GOSoundOrganEngine::SamplesDiffToMs(
  uint64_t fromSamples, uint64_t toSamples) const {
  return (unsigned)std::min(
    (toSamples - fromSamples) * 1000 / m_SampleRate, (uint64_t)UINT_MAX);
}

GOSoundSampler *GOSoundOrganEngine::CreateTaskSample(
  const GOSoundProvider *pSoundProvider,
  int samplerTaskId,
  unsigned audioGroup,
  unsigned velocity,
  unsigned delay,
  uint64_t prevEventTime,
  bool isRelease,
  uint64_t *pStartTimeSamples) {
  unsigned delay_samples = (delay * m_SampleRate) / (1000);
  uint64_t start_time = m_CurrentTime + delay_samples;
  unsigned eventIntervalMs = SamplesDiffToMs(prevEventTime, start_time);

  GOSoundSampler *sampler = nullptr;
  const GOSoundAudioSection *section = isRelease
    ? pSoundProvider->GetRelease(BOOL3_DEFAULT, eventIntervalMs)
    : pSoundProvider->GetAttack(velocity, eventIntervalMs);

  if (pStartTimeSamples) {
    *pStartTimeSamples = start_time;
  }
  if (section && section->GetChannels()) {
    sampler = m_SamplerPool.GetSampler();
    if (sampler) {
      sampler->p_SoundProvider = pSoundProvider;
      sampler->m_WaveTremulantStateFor = section->GetWaveTremulantStateFor();
      sampler->velocity = velocity;
      sampler->stream.InitStream(
        &m_resample,
        section,
        m_interpolation,
        GetRandomFactor() * pSoundProvider->GetTuning() / (float)m_SampleRate);

      const float playback_gain
        = pSoundProvider->GetGain() * section->GetNormGain();

      sampler->fader.Setup(
        playback_gain, pSoundProvider->GetVelocityVolume(velocity));
      sampler->delay = delay_samples;
      sampler->time = start_time;
      sampler->toneBalanceFilterState.Init(
        sampler->p_SoundProvider->GetToneBalance()->GetFilter());
      sampler->is_release = isRelease;
      sampler->m_SamplerTaskId = samplerTaskId;
      sampler->m_AudioGroupId = audioGroup;
      StartSampler(sampler);
    }
  }
  return sampler;
}

void GOSoundOrganEngine::SwitchToAnotherAttack(GOSoundSampler *pSampler) {
  const GOSoundProvider *pProvider = pSampler->p_SoundProvider;

  if (pProvider && !pSampler->is_release) {
    const GOSoundAudioSection *section
      = pProvider->GetAttack(pSampler->velocity, 1000);

    if (section) {
      GOSoundSampler *new_sampler = m_SamplerPool.GetSampler();

      if (new_sampler != NULL) {
        float gain_target = pProvider->GetGain() * section->GetNormGain();
        unsigned crossFadeSamples
          = MsToSamples(pProvider->GetAttackSwitchCrossfadeLength());

        // copy old sampler to the new one (residence token stays clear -
        // the clone joins a bucket only via Add + period window)
        new_sampler->CloneFrom(*pSampler);

        // start decay in the new sampler
        new_sampler->is_release = true;
        new_sampler->time = m_CurrentTime;
        new_sampler->fader.StartDecreasingVolume(crossFadeSamples);
        new_sampler->fade_out_start_time = m_CurrentTime;

        // start new section stream in the old sampler
        pSampler->m_WaveTremulantStateFor = section->GetWaveTremulantStateFor();
        pSampler->stream.InitAlignedStream(
          section, m_interpolation, &new_sampler->stream);
        pSampler->p_SoundProvider = pProvider;
        pSampler->time = m_CurrentTime + 1;

        pSampler->fader.Setup(
          gain_target,
          new_sampler->fader.GetVelocityVolume(),
          crossFadeSamples);
        pSampler->is_release = false;
        // pSampler keeps playing as a fresh attack - clear the diagnostic
        // fade-out clock so a later real fade-out is measured correctly.
        pSampler->fade_out_start_time = 0;
        pSampler->stale_release_logged = false;

        new_sampler->toneBalanceFilterState.Init(
          new_sampler->p_SoundProvider->GetToneBalance()->GetFilter());

        StartSampler(new_sampler);
      }
    }
  }
}

namespace {
// Helper functions and types for the release model (internal linkage)
enum ChannelKind { CK_Unknown, CK_Dry, CK_Front, CK_Rear };

static ChannelKind ChannelFromRankName(const wxString &n) {
  if (n.Contains("dry"))
    return CK_Dry;
  if (n.Contains("rear:"))
    return CK_Rear;
  if (n.Contains("front:"))
    return CK_Front;
  return CK_Dry;
}

static bool IsChamade(const wxString &n) {
  return n.Contains("trompeta batalla 8") || n.Contains("batalla")
    || n.Contains("cham.");
}

} // anonymous namespace

void GOSoundOrganEngine::CreateReleaseSampler(GOSoundSampler *handle) {

  if (!handle->p_SoundProvider)
    return;

  // handle->p_SoundProvider->GetAttack(unsigned int velocity, unsigned int
  // releasedDurationMs)

  /* The beloow code creates a new sampler to playback the release, the
   * following code takes the active sampler for this pipe (which will be
   * in either the attack or loop section) and sets the fadeout property
   * which will decay this portion of the pipe. The sampler will
   * automatically be placed back in the pool when the fade restores to
   * zero. */
  const GOSoundProvider *this_pipe = handle->p_SoundProvider;
  const GOSoundAudioSection *release_section = this_pipe->GetRelease(
    handle->m_WaveTremulantStateFor,
    SamplesDiffToMs(handle->time, m_CurrentTime));
  unsigned crossFadeSamples = MsToSamples(
    release_section ? release_section->GetReleaseCrossfadeLength()
                    : this_pipe->GetAttackSwitchCrossfadeLength());

  handle->fader.StartDecreasingVolume(crossFadeSamples);
  handle->fade_out_start_time = m_CurrentTime;
  handle->is_release = true;

  int taskId = handle->m_SamplerTaskId;
  float vol = isWindchestTask(taskId)
    ? m_WindchestTasks[windchestTaskToIndex(taskId)]->GetWindchestVolume()
    : 1.0f;

  // FIXME: this is wrong... the intention is to not create a release for a
  // sample being played back with zero amplitude but this is a comparison
  // against a double. We should test against a minimum level.
  if (vol && release_section) {
    GOSoundSampler *new_sampler = m_SamplerPool.GetSampler();
    if (new_sampler != NULL) {
      new_sampler->p_SoundProvider = this_pipe;
      new_sampler->time = m_CurrentTime + 1;
      new_sampler->m_WaveTremulantStateFor
        = release_section->GetWaveTremulantStateFor();

      unsigned gain_decay_length = 0;
      float gain_target = this_pipe->GetGain() * release_section->GetNormGain();
      const bool not_a_tremulant = isWindchestTask(handle->m_SamplerTaskId);

      if (not_a_tremulant) {

        /* Because this sampler is about to be moved to a detached
         * windchest, we must apply the gain of the existing windchest
         * to the gain target for this fader - otherwise the playback
         * volume on the detached chest will not match the volume on
         * the existing chest. */
        gain_target *= vol;
        if (m_ScaledReleases) {
          /* Note: "time" is in milliseconds. */
          int time = ((m_CurrentTime - handle->time) * 1000) / m_SampleRate;
          unsigned midikey_frequency = this_pipe->GetMidiKeyNumber();
          /* if MidiKeyNumber is not within the range of organ pipes (64 feet
           * to 1 foot), we assume average pipe (MIDI = 60) */
          if (midikey_frequency > 127 || midikey_frequency == 0)
            midikey_frequency = 60;
          /* attack duration is assumed 50 ms above MIDI 96, 800 ms below MIDI
           * 24 and linear in between */
          float attack_duration = 50.0f;
          if (midikey_frequency < 96) {
            if (midikey_frequency < 24)
              attack_duration = 500.0f;
            else
              attack_duration
                = 500.0f + ((24.0f - (float)midikey_frequency) * 6.25f);
          }

          // Identify rank channel and whether it is a Chamade stop
          auto *pipe = this_pipe->GetOwnerPipe();
          GORank *rank = pipe ? pipe->GetRank() : nullptr;
          const wxString rankName = rank ? rank->GetName().Lower() : wxString();

          const ChannelKind chan = ChannelFromRankName(rankName);
          const bool chamade = IsChamade(rankName);

          CHAMADE_DEBUG(
            "Release: Rank: %s , bCham=%i, Type=%i",
            rankName.mb_str(),
            chamade,
            chan);

          float g_0 = 0.1f;

          if (chamade) {
            switch (chan) {
            case CK_Dry:
              attack_duration = tmax_dry_by_midi[midikey_frequency];
              g_0 = g0_dry_by_midi[midikey_frequency];
              CHAMADE_DEBUG("Release: Chamade Dry");
              break;
            case CK_Front:
              attack_duration = tmax_front_by_midi[midikey_frequency];
              g_0 = g0_front_by_midi[midikey_frequency];
              CHAMADE_DEBUG("Release: Chamade Front");
              break;
            case CK_Rear:
              attack_duration = tmax_rear_by_midi[midikey_frequency];
              g_0 = g0_rear_by_midi[midikey_frequency];
              CHAMADE_DEBUG("Release: Chamade Rear");
              break;
            default:
              break;
            }

            if (time < (int)attack_duration) {
              float attack_index = (float)time / attack_duration;
              float gain_delta
                = (1.0f - attack_index) * g_0 + attack_index * 1.0f;
              gain_delta = std::clamp(gain_delta, 0.0f, 1.0f);
              gain_target *= gain_delta;
            }
          } else {
            // General pipes: linear ramp from g_0 to 1 over attack_duration
            attack_duration = 120;
            g_0 = 0.1f;

            if (time < (int)attack_duration) {
              float attack_index = (float)time / attack_duration;
              float gain_delta
                = (1.0f - attack_index) * g_0 + attack_index * 1.0f;
              gain_delta = std::clamp(gain_delta, 0.0f, 1.0f);
              gain_target *= gain_delta;
            }
          }
        }
      }

      const unsigned releaseLength = this_pipe->GetReleaseTail();

      new_sampler->fader.Setup(
        gain_target, handle->fader.GetVelocityVolume(), crossFadeSamples);

      if (
        releaseLength > 0
        && (releaseLength < gain_decay_length || gain_decay_length == 0))
        gain_decay_length = releaseLength;

      if (gain_decay_length > 0) {
        new_sampler->fader.StartDecreasingVolume(
          MsToSamples(gain_decay_length));
        new_sampler->fade_out_start_time = m_CurrentTime;
      }

      if (
        m_ReleaseAlignmentEnabled
        && release_section->SupportsStreamAlignment()) {
#if __has_include(                                                             \
  "playing/GOLogReleaseAlignEnable.h")                                         \
  && __has_include("playing/GOLogReleaseAlignVerbose.h")
        {
          auto *dbgPipe = this_pipe->GetOwnerPipe();
          auto *dbgRank = dbgPipe ? dbgPipe->GetRank() : nullptr;
          const wxString dbgRankName
            = dbgRank ? dbgRank->GetName().Lower() : wxString();
          const ChannelKind dbgChan = ChannelFromRankName(dbgRankName);
          const char *dbgChanStr = (dbgChan == CK_Front) ? "front"
            : (dbgChan == CK_Rear)                       ? "rear"
                                                         : "dry";
          // GetKeyMidiNumber() = ODF key position (pressed key).
          // GetMidiKeyNumber() = smpl-chunk pitch (recording pitch) — wrong
          // here.
          const unsigned dbgKeyMidi = dbgPipe ? dbgPipe->GetKeyMidiNumber()
                                              : this_pipe->GetMidiKeyNumber();
          char dbgLabel[64];
          std::snprintf(
            dbgLabel,
            sizeof(dbgLabel),
            "%s|midi=%u|%s",
            dbgRankName.utf8_str().data(),
            dbgKeyMidi,
            dbgChanStr);
          new_sampler->stream.InitAlignedStream(
            release_section, m_interpolation, &handle->stream, dbgLabel);
        }
#else
        new_sampler->stream.InitAlignedStream(
          release_section, m_interpolation, &handle->stream);
#endif
      } else {
        new_sampler->stream.InitStream(
          &m_resample,
          release_section,
          m_interpolation,
          this_pipe->GetTuning() / (float)m_SampleRate);
      }
      new_sampler->is_release = true;

      new_sampler->m_SamplerTaskId = not_a_tremulant
        ? /* detached releases are enabled and the pipe was on a regular
           * windchest. Play the release on the detached windchest */
        DETACHED_RELEASE_TASK_ID
        /* detached releases are disabled (or this isn't really a pipe)
         * so put the release on the same windchest as the pipe (which
         * means it will still be affected by tremulants - yuck). */
        : handle->m_SamplerTaskId;
      new_sampler->m_AudioGroupId = handle->m_AudioGroupId;
      new_sampler->toneBalanceFilterState.Init(
        new_sampler->p_SoundProvider->GetToneBalance()->GetFilter());
      StartSampler(new_sampler);
      handle->time = m_CurrentTime;
    }
  }
}

uint64_t GOSoundOrganEngine::StopSample(
  const GOSoundProvider *pipe, GOSoundSampler *handle) {
  assert(handle);
  assert(pipe);

  // The following condition could arise if a one-shot sample is played,
  // decays away (and hence the sampler is discarded back into the pool), and
  // then the user releases a key. If the sampler had already been reused
  // with another pipe, that sample would erroneously be told to decay.
  if (pipe != handle->p_SoundProvider)
    return 0;

  handle->stop = m_CurrentTime + handle->delay;
  return handle->stop;
}

void GOSoundOrganEngine::SwitchSample(
  const GOSoundProvider *pipe, GOSoundSampler *handle) {
  assert(handle);
  assert(pipe);

  // The following condition could arise if a one-shot sample is played,
  // decays away (and hence the sampler is discarded back into the pool), and
  // then the user releases a key. If the sampler had already been reused
  // with another pipe, that sample would erroneously be told to decay.
  if (pipe != handle->p_SoundProvider)
    return;

  handle->new_attack = m_CurrentTime + handle->delay;
}

void GOSoundOrganEngine::StartThreads(unsigned nConcurrency) {
  GOMutexLocker thread_locker(m_ThreadLock);

  for (unsigned i = 0; i < nConcurrency; i++)
    mp_threads.push_back(std::make_unique<GOSoundThread>(&m_Scheduler, i));


  for (auto &pThread : mp_threads)
    pThread->Run();
}

void GOSoundOrganEngine::StopThreads() {
  for (auto &pThread : mp_threads)
    pThread->Delete();

  GOMutexLocker thread_locker(m_ThreadLock);

  mp_threads.clear();
}

void GOSoundOrganEngine::BuildAndStart(
  GOConfig &config,
  unsigned sampleRate,
  unsigned samplesPerBuffer,
  GOOrganModel &organModel,
  GOMemoryPool &memoryPool,
  unsigned releaseConcurrency,
  GOSoundRecorder &audioRecorder) {
  const unsigned audio_group_count = config.GetAudioGroups().size();
  std::vector<GOAudioDeviceConfig> &audio_config
    = config.GetAudioDeviceConfig();
  const unsigned audioDeviceCount = audio_config.size();
  std::vector<GOAudioOutputConfiguration> engine_config;

  engine_config.resize(audioDeviceCount);
  for (unsigned deviceI = 0; deviceI < audioDeviceCount; deviceI++) {
    const GOAudioDeviceConfig &deviceConfig = audio_config[deviceI];
    const auto &deviceOutputs = deviceConfig.GetChannelOututs();
    GOAudioOutputConfiguration &engineConfig = engine_config[deviceI];

    engineConfig.channels = deviceConfig.GetChannels();
    engineConfig.scale_factors.resize(engineConfig.channels);
    for (unsigned channelI = 0; channelI < engineConfig.channels; channelI++) {
      std::vector<float> &scaleFactors = engineConfig.scale_factors[channelI];

      scaleFactors.resize(audio_group_count * 2);
      std::fill(
        scaleFactors.begin(),
        scaleFactors.end(),
        GOAudioDeviceConfig::MUTE_VOLUME);

      if (channelI < deviceOutputs.size()) {
        for (const auto &groupOutput : deviceOutputs[channelI]) {
          int id = config.GetStrictAudioGroupId(groupOutput.GetName());

          if (id >= 0) {
            scaleFactors[id * 2] = groupOutput.GetLeft();
            scaleFactors[id * 2 + 1] = groupOutput.GetRight();
          }
        }
      }
    }
  }

  SetSamplesPerBuffer(samplesPerBuffer);
  /* V3 subblock bin ladder: base = the engine's REAL samples_per_buffer
   * (a voice's private block must cover one callback's source
   * consumption), top rung = MAX_FACTOR * base. Must be set up before
   * SetHardPolyphony below creates the pooled samplers, so their block
   * buffers get sized to the ladder's top rung. Static (process-wide)
   * like SetUseSubblockCache - safe here, no streams are active before
   * Setup. */
  GOSoundSubblockBins::SetBase(
    config.UseV3SubblockCache() ? samplesPerBuffer : 0, sampleRate);
  m_SamplerPool.SetVoiceBlockChannelStride(
    config.UseV3SubblockCache()
      ? GOSoundSubblockBins::GetMaxBin() + GOSoundStream::GUARD_SAMPLES
      : 0);
  SetPolyphonyLimiting(config.ManagePolyphony());
  SetHardPolyphony(config.PolyphonyLimit());
  SetScaledReleases(config.ScaleRelease());
  SetRandomizeSpeaking(config.RandomizeSpeaking());
  SetInterpolationType(config.m_InterpolationType());
  /* Static (process-wide) toggle, read by each stream once at voice start -
   * safe to set here because no streams are active before Setup. */
  GOSoundStream::SetUseSubblockCache(config.UseV3SubblockCache());
  SetAudioGroupCount(audio_group_count);
  SetSampleRate(sampleRate);
  SetAudioOutput(engine_config);
  SetupReverb(config);
  SetAudioRecorder(&audioRecorder, config.RecordDownmix());
  Setup(organModel, memoryPool, releaseConcurrency);
  StartThreads(config.Concurrency());
  m_Scheduler.ResumeGivingWork();
}

void GOSoundOrganEngine::StopAndDestroy() {
  m_Scheduler.PauseGivingWork();
  for (auto &pThread : mp_threads)
    pThread->WaitForIdle();
  StopThreads();
  ClearSetup();
}

void GOSoundOrganEngine::WaitForThreadsIdle() {
  for (auto &pThread : mp_threads)
    pThread->WaitForIdle();
}

void GOSoundOrganEngine::WakeupThreads() {
  // Marks the start of this period's work becoming available to the pool -
  // see m_PeriodStartUs/NextPeriod() for what it's used for.
  m_PeriodStartUs.store(
    std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count(),
    std::memory_order_relaxed);

  const auto wakeT0 = std::chrono::steady_clock::now();

  {
    GOMutexLocker thread_locker(m_ThreadLock);

    /* ONE Broadcast wakes the whole pool: the workers wait on a shared
     * condition, the kernel fans out to all of them simultaneously with
     * priorities intact (user's question 2026-07-13 - yes, one call
     * suffices). History: 20 serial per-thread Signal() calls measured
     * ~7700 us/s here; a wake CASCADE via the workers themselves was
     * tried and reverted (delegation to normal-priority threads added
     * per-hop scheduling variance -> permanent jitter). The kernel-side
     * fan-out has neither problem. */
    GOSoundThread::WakeAll();
  }
  m_WakeLoopUsSum.fetch_add(
    (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - wakeT0)
      .count(),
    std::memory_order_relaxed);
}

void GOSoundOrganEngine::UpdateVelocity(
  const GOSoundProvider *pipe, GOSoundSampler *handle, unsigned velocity) {
  assert(handle);
  assert(pipe);

  if (handle->p_SoundProvider == pipe) {
    // we've just checked that handle is still playing the same pipe;
    // maybe handle was switched to another pipe between checking and
    // SetVelocityVolume, but we don't want to lock it because this
    // functionality is not so important. Concurrent update is acceptable,
    // as it just updates a float
    handle->velocity = velocity;
    handle->fader.SetVelocityVolume(pipe->GetVelocityVolume(velocity));
  }
}
