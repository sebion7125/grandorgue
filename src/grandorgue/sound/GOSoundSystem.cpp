/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundSystem.h"

#include <chrono>
#include <thread>

#include <wx/app.h>
#include <wx/intl.h>
#include <wx/time.h>
#include <wx/window.h>

#include "buffer/GOSoundBufferMutable.h"
#include "config/GOConfig.h"
#include "config/GOPortsConfig.h"
#include "ports/GOSoundPortFactory.h"
#include "threading/GOMultiMutexLocker.h"
#include "threading/GOMutexLocker.h"

#include "GOEvent.h"
#include "GOOrganController.h"
#include "GOSoundDefs.h"

// How often the watchdog checks m_LastAudioCallbackMs
static constexpr unsigned WATCHDOG_POLL_INTERVAL_MS = 500;
// How long the backend may stay silent before being considered lost
static constexpr int64_t WATCHDOG_CALLBACK_TIMEOUT_MS = 1500;
// How long to wait after a system resume before reopening the audio device,
// giving USB/ASIO devices time to become ready again
static constexpr unsigned RESUME_DELAY_MS = 2000;
// How long OpenSoundAsync/CloseSoundAsync wait for their worker thread
// before giving up and marking the device DRIVER_HUNG instead of blocking
// the GUI thread indefinitely
static constexpr unsigned AUDIO_OPEN_TIMEOUT_MS = 5000;
static constexpr unsigned AUDIO_CLOSE_TIMEOUT_MS = 5000;
// Shorter close timeout used when the stream was already DEVICE_LOST before
// the close was requested (e.g. Audio Panic after the device disappeared):
// the old stream object is tied to a now-gone device session, so a graceful
// Close() succeeding at all is already unlikely - no point waiting as long
// as for a normal close.
static constexpr unsigned AUDIO_CLOSE_TIMEOUT_AFTER_LOST_MS = 1500;

static const char *GOSoundDeviceStateToCString(GOSoundDeviceState state) {
  switch (state) {
  case GOSoundDeviceState::CLOSED:
    return "Closed";
  case GOSoundDeviceState::OPENING:
    return "Opening";
  case GOSoundDeviceState::RUNNING:
    return "Running";
  case GOSoundDeviceState::SUSPENDED:
    return "Suspended";
  case GOSoundDeviceState::DEVICE_LOST:
    return "DeviceLost";
  case GOSoundDeviceState::CLOSING:
    return "Closing";
  case GOSoundDeviceState::OPEN_FAILED:
    return "OpenFailed";
  case GOSoundDeviceState::DRIVER_HUNG:
    return "DriverHung";
  }
  return "Unknown";
}

void GOSoundSystem::SetState(GOSoundDeviceState newState) {
  GOSoundDeviceState oldState = m_State.exchange(newState);

  if (oldState != newState)
    wxLogDebug(
      "Audio: state %s -> %s",
      GOSoundDeviceStateToCString(oldState),
      GOSoundDeviceStateToCString(newState));
}

void GOSoundSystem::HandleTimer() {
  if (m_State.load() != GOSoundDeviceState::RUNNING)
    return;

  const int64_t lastMs = m_LastAudioCallbackMs.load(std::memory_order_relaxed);
  const int64_t nowMs = wxGetLocalTimeMillis().GetValue();

  if (nowMs - lastMs > WATCHDOG_CALLBACK_TIMEOUT_MS) {
    SetState(GOSoundDeviceState::DEVICE_LOST);
    wxLogWarning(
      _("Audio device appears to be lost: no audio callbacks received for "
        "over %d ms."),
      (int)WATCHDOG_CALLBACK_TIMEOUT_MS);
  }
}

GOSoundSystem::GOSoundSystem(GOConfig &settings)
  : m_config(settings),
    m_midi(settings),
    m_open(false),
    m_State(GOSoundDeviceState::CLOSED),
    logSoundErrors(true),
    m_SampleRate(0),
    m_SamplesPerBuffer(0),
    m_OrganController(0),
    m_DefaultAudioDevice(GOSoundDevInfo::getInvalideDeviceInfo()),
    m_IsRunning(false),
    m_NCallbacksEntered(0),
    m_LastAudioCallbackMs(0),
    m_CallbackCondition(m_CallbackMutex),
    meter_counter(0),
    m_WaitCount(0),
    m_CalcCount(0),
    m_ResumeCallback(*this),
    m_WasRunningBeforeSuspend(false),
    m_AliveFlag(std::make_shared<std::atomic<bool>>(true)) {}

GOSoundSystem::~GOSoundSystem() {
  // Must be the first statement: lets a still-running open worker's
  // deferred completion (see OpenSoundAsync) detect that `this` is no
  // longer valid, instead of risking a use-after-free.
  m_AliveFlag->store(false);

  AssureSoundIsClosed();

  GOMidiPortFactory::terminate();
  GOSoundPortFactory::terminate();
}

// Runs entirely on a worker thread. Touches only `job` - the inputs it
// needs were snapshotted by OpenSoundAsync() on the GUI thread, and the
// newly created ports are written into job->outputs, not into
// GOSoundSystem::m_AudioOutputs - so this function never needs `this` to
// still be a live GOSoundSystem. The `this` pointer passed into
// GOSoundPortFactory::create() below is stored inside the new GOSoundPort
// objects for later use by their audio callback, which can only start
// firing after StartStreams() - and that only happens later, from
// ApplyOpenJobResult() on the GUI thread, once `this` has been confirmed
// alive again.
void GOSoundSystem::OpenJobWorker(std::shared_ptr<GOSoundOpenJob> job) {
  wxString errorMessage;
  bool ok = false;

  try {
    job->outputs.resize(job->audioConfig.size());

    for (unsigned n = job->outputs.size(), i = 0; i < n; i++) {
      GOAudioDeviceConfig &deviceConfig = job->audioConfig[i];
      GODeviceNamePattern *pNamePattern = &deviceConfig;
      GODeviceNamePattern defaultPattern = job->defaultDevicePattern;

      if (!pNamePattern->IsFilled())
        pNamePattern = &defaultPattern;

      GOSoundPort *pPort
        = GOSoundPortFactory::create(job->portsConfig, this, *pNamePattern);

      if (!pPort)
        throw wxString::Format(
          _("Output device %s not found - no sound output will occur"),
          pNamePattern->GetRegEx());
      job->outputs[i].port = pPort;
      pPort->Init(
        deviceConfig.GetChannels(),
        job->sampleRate,
        job->samplesPerBuffer,
        deviceConfig.GetDesiredLatency(),
        i);
    }

    for (GOSoundOutput &output : job->outputs)
      output.port->Open();

    if (job->samplesPerBuffer > MAX_FRAME_SIZE)
      throw wxString::Format(
        _("Cannot use buffer size above %d samples; "
          "unacceptable quantization would occur."),
        MAX_FRAME_SIZE);

    ok = true;
  } catch (wxString &msg) {
    errorMessage = msg;
  } catch (const std::exception &e) {
    errorMessage = wxString::FromUTF8(e.what());
  } catch (...) {
    errorMessage = _("Unknown error while opening the audio device");
  }

  if (!ok)
    for (GOSoundOutput &output : job->outputs)
      if (output.port) {
        GOSoundPort *port = output.port;

        output.port = nullptr;
        try {
          port->Close();
        } catch (...) {
          // a background thread must never let an exception escape
        }
        delete port;
      }

  {
    std::lock_guard<std::mutex> lock(job->mutex);

    job->ok = ok;
    job->errorMessage = errorMessage;
    job->done = true;
  }
  job->condition.notify_all();
}

// Installs a finished open job's result into GOSoundSystem. Safe to call
// more than once for the same job (only the first call after `done` does
// anything) and safe to call from either the bounded wait in
// OpenSoundAsync() or a later GUI-thread completion for a worker that took
// longer than the timeout - both paths only ever run on the GUI thread.
void GOSoundSystem::ApplyOpenJobResult(
  const std::shared_ptr<GOSoundOpenJob> &job) {
  {
    std::lock_guard<std::mutex> lock(job->mutex);

    if (job->applied || !job->done)
      return;
    job->applied = true;
  }

  if (job->ok) {
    m_SampleRate = job->sampleRate;
    m_SamplesPerBuffer = job->samplesPerBuffer;
    m_AudioOutputs = std::move(job->outputs);

    try {
      // Callbacks fired during stream start are no-ops: the audio callback
      // checks m_IsRunning first and exits early while it is false.
      // m_IsRunning is set to true only in StartSoundSystem().
      StartStreams();
      OpenMidi();
      m_AudioRecorder.SetSampleRate(m_SampleRate);
      m_open = true;
      // seed before SetState so the watchdog's first tick has a fair grace
      // period instead of comparing against a stale or zero timestamp
      m_LastAudioCallbackMs.store(
        wxGetLocalTimeMillis().GetValue(), std::memory_order_relaxed);
      SetState(GOSoundDeviceState::RUNNING);
      m_Watchdog.SetRelativeTimer(
        WATCHDOG_POLL_INTERVAL_MS, this, WATCHDOG_POLL_INTERVAL_MS);

      if (m_OrganController) {
        BuildAndStartEngine();
        StartSoundSystem();
        NotifySoundIsOpen();
      }
      return;
    } catch (wxString &msg) {
      // StartStream() failed even though Open() succeeded; fall through to
      // the same cleanup/reporting as an open failure below
      job->errorMessage = msg;
      job->ok = false;
      for (GOSoundOutput &output : m_AudioOutputs)
        if (output.port) {
          GOSoundPort *port = output.port;

          output.port = nullptr;
          try {
            port->Close();
          } catch (...) {
          }
          delete port;
        }
      m_AudioOutputs.clear();
      m_open = false;
    }
  }

  if (logSoundErrors)
    GOMessageBox(job->errorMessage, _("Error"), wxOK | wxICON_ERROR, NULL);
  else
    m_LastErrorMessage = job->errorMessage;

  SetState(GOSoundDeviceState::OPEN_FAILED);
}

bool GOSoundSystem::OpenSoundAsync(unsigned timeoutMs) {
  assert(!m_open);
  assert(m_AudioOutputs.size() == 0);

  if (m_State.load() == GOSoundDeviceState::DRIVER_HUNG) {
    wxLogWarning(
      _("Audio driver is already marked as hung. Restart GrandOrgue before "
        "retrying."));
    return false;
  }

  if (m_PendingCloseJob) {
    // A previous close may have been fired without waiting for it (see
    // SuspendAudioForPowerEvent); wait for it here, inside the same
    // timeout budget, rather than risk opening a new port for the same
    // physical device while it might still be touching it.
    std::shared_ptr<GOSoundCloseJob> pendingClose = m_PendingCloseJob;
    bool closeFinished;
    {
      std::unique_lock<std::mutex> lock(pendingClose->mutex);

      closeFinished = pendingClose->condition.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [&pendingClose] {
          return pendingClose->done;
        });
    }
    if (closeFinished)
      ApplyCloseJobResult(pendingClose);
    else {
      SetState(GOSoundDeviceState::DRIVER_HUNG);
      wxLogWarning(_("Audio: the previous close has still not finished; not "
                     "reopening the device to avoid touching it twice. Restart "
                     "GrandOrgue if sound does not come back by itself."));
      return false;
    }
  }

  SetState(GOSoundDeviceState::OPENING);
  m_LastErrorMessage = wxEmptyString;

  auto job = std::make_shared<GOSoundOpenJob>();

  job->portsConfig = m_config.GetSoundPortsConfig();
  job->audioConfig = m_config.GetAudioDeviceConfig();
  job->sampleRate = m_config.SampleRate();
  job->samplesPerBuffer = m_config.SamplesPerBuffer();
  FillDeviceNamePattern(
    GetDefaultAudioDevice(job->portsConfig), job->defaultDevicePattern);
  m_AudioRecorder.SetBytesPerSample(m_config.WaveFormatBytesPerSample());

  std::shared_ptr<std::atomic<bool>> aliveFlag = m_AliveFlag;

  std::thread([this, job]() { OpenJobWorker(job); }).detach();

  {
    std::unique_lock<std::mutex> lock(job->mutex);

    job->condition.wait_for(
      lock, std::chrono::milliseconds(timeoutMs), [&job] { return job->done; });
  }

  if (job->done)
    ApplyOpenJobResult(job);
  else {
    SetState(GOSoundDeviceState::DRIVER_HUNG);
    wxLogWarning(
      _("Audio driver did not return while opening the device. The driver "
        "may be stuck, e.g. after suspend/resume. Restart GrandOrgue if "
        "sound does not come back by itself."));
  }

  // The worker may still be running (DRIVER_HUNG above) or may have just
  // missed the lock above by a hair; either way, apply its result once it
  // does finish, but only if GOSoundSystem is still alive by then.
  // ApplyOpenJobResult() is idempotent, so this is harmless if the result
  // was already applied just above.
  if (wxTheApp)
    wxTheApp->CallAfter([this, job, aliveFlag]() {
      if (aliveFlag->load())
        ApplyOpenJobResult(job);
    });

  return m_open;
}

void GOSoundSystem::StartSoundSystem() {
  // Enable all outputs
  for (GOSoundOutput &output : m_AudioOutputs) {
    GOMutexLocker dev_lock(output.mutex);

    output.wait = false;
    output.waiting = true;
  }
  m_WaitCount.store(0);
  m_CalcCount.store(0);
  m_NCallbacksEntered.store(0);
  m_IsRunning.store(true);
  SetState(GOSoundDeviceState::RUNNING);
}

void GOSoundSystem::NotifySoundIsOpen() {
  m_OrganController->PreparePlayback(
    &GetEngine(), &GetMidi(), &m_AudioRecorder);
}

void GOSoundSystem::NotifySoundIsClosing() { m_OrganController->Abort(); }

void GOSoundSystem::StopSoundSystem() {
  SetState(GOSoundDeviceState::CLOSING);
  m_IsRunning.store(false);

  // wait for all started callbacks to finish
  {
    GOMutexLocker lock(m_CallbackMutex);

    while (m_NCallbacksEntered.load() > 0)
      m_CallbackCondition.WaitOrStop(
        "GOSoundSystem::StopSoundSystem waits for all callbacks to finish",
        nullptr);
  }

  // Disable all outputs
  {
    GOMultiMutexLocker multi;

    for (GOSoundOutput &output : m_AudioOutputs)
      multi.Add(output.mutex);

    for (GOSoundOutput &output : m_AudioOutputs) {
      output.waiting = false;
      output.wait = false;
      output.condition.Broadcast();
    }
  }
}

// Detaches m_AudioOutputs immediately (so GOSoundSystem no longer
// references those ports at all) and starts closing/deleting them on a
// worker thread, without waiting for it - the caller decides whether and
// how long to wait via the returned job. The worker itself never needs to
// call back into GOSoundSystem - there is nothing left here for it to
// install - it just goes on quietly tearing down orphaned GOSoundPort
// objects if it outlives whatever the caller ends up waiting for.
std::shared_ptr<GOSoundSystem::GOSoundCloseJob> GOSoundSystem::StartCloseJob() {
  m_Watchdog.DeleteTimer(this);

  auto job = std::make_shared<GOSoundCloseJob>();

  job->outputs = std::move(m_AudioOutputs);
  m_AudioOutputs.clear();
  m_open = false;
  m_PendingCloseJob = job;

  std::thread([job]() {
    for (GOSoundOutput &output : job->outputs)
      if (output.port) {
        GOSoundPort *port = output.port;

        output.port = nullptr;
        try {
          port->Close();
        } catch (...) {
          // a background thread must never let an exception escape
        }
        delete port;
      }
    {
      std::lock_guard<std::mutex> lock(job->mutex);

      job->done = true;
    }
    job->condition.notify_all();
  }).detach();

  ResetMeters();
  return job;
}

// Idempotent: only the first call after the job is done does anything.
// Safe to call from the bounded wait in CloseSoundAsync()/OpenSoundAsync()
// (already on the GUI thread) or from a later GUI-thread completion for a
// job that took longer than whoever was waiting cared to wait.
void GOSoundSystem::ApplyCloseJobResult(
  const std::shared_ptr<GOSoundCloseJob> &job) {
  std::lock_guard<std::mutex> lock(job->mutex);

  if (job->applied || !job->done)
    return;
  job->applied = true;
  if (m_PendingCloseJob == job)
    m_PendingCloseJob.reset();
}

void GOSoundSystem::CloseSoundAsync(unsigned timeoutMs) {
  std::shared_ptr<GOSoundCloseJob> job = StartCloseJob();

  bool finishedInTime;
  {
    std::unique_lock<std::mutex> lock(job->mutex);

    finishedInTime = job->condition.wait_for(
      lock, std::chrono::milliseconds(timeoutMs), [&job] { return job->done; });
  }

  if (finishedInTime) {
    ApplyCloseJobResult(job);
    SetState(GOSoundDeviceState::CLOSED);
    return;
  }

  SetState(GOSoundDeviceState::DRIVER_HUNG);
  wxLogWarning(
    _("Audio driver did not return while closing the device. The audio "
      "ports will be released in the background; restart GrandOrgue if "
      "problems persist."));

  std::shared_ptr<std::atomic<bool>> aliveFlag = m_AliveFlag;

  if (wxTheApp)
    wxTheApp->CallAfter([this, job, aliveFlag]() {
      if (aliveFlag->load())
        ApplyCloseJobResult(job);
    });
}

void GOSoundSystem::StartStreams() {
  for (GOSoundOutput &output : m_AudioOutputs)
    output.port->StartStream();
}

void GOSoundSystem::BuildAndStartEngine() {
  m_SoundEngine.BuildAndStart(
    m_config,
    m_SampleRate,
    m_SamplesPerBuffer,
    static_cast<GOOrganModel &>(*m_OrganController),
    m_OrganController->GetMemoryPool(),
    m_config.ReleaseConcurrency(),
    m_AudioRecorder);
}

void GOSoundSystem::StopAndDestroyEngine() { m_SoundEngine.StopAndDestroy(); }

bool GOSoundSystem::AssureSoundIsOpen() {
  // BuildAndStartEngine/StartSoundSystem/NotifySoundIsOpen now happen
  // inside ApplyOpenJobResult(), so they still run correctly even if the
  // open finishes late, after OpenSoundAsync() already gave up waiting.
  if (!m_open)
    OpenSoundAsync(AUDIO_OPEN_TIMEOUT_MS);
  return m_open;
}

void GOSoundSystem::AssureSoundIsClosed() {
  if (m_open) {
    // capture before StopSoundSystem() unconditionally switches to CLOSING
    const bool wasAlreadyLost
      = m_State.load() == GOSoundDeviceState::DEVICE_LOST;

    if (m_OrganController) {
      NotifySoundIsClosing();
      StopSoundSystem();
      StopAndDestroyEngine();
    }
    CloseSoundAsync(
      wasAlreadyLost ? AUDIO_CLOSE_TIMEOUT_AFTER_LOST_MS
                     : AUDIO_CLOSE_TIMEOUT_MS);
  }
}

void GOSoundSystem::WithOrganEngineQuiesced(
  const std::function<void()> &action) {
  GOMutexLocker locker(m_lock);
  GOMultiMutexLocker multi;
  for (unsigned i = 0; i < m_AudioOutputs.size(); i++)
    multi.Add(m_AudioOutputs[i].mutex);

  if (m_OrganController) {
    m_SoundEngine.GetScheduler().PauseGivingWork();
    m_SoundEngine.WaitForThreadsIdle();
  }
  action();
  if (m_OrganController)
    m_SoundEngine.GetScheduler().ResumeGivingWork();
}

void GOSoundSystem::AssignOrganFile(GOOrganController *pNewOrganController) {
  if (pNewOrganController != m_OrganController) {
    GOMutexLocker locker(m_lock);

    if (m_open && m_OrganController) {
      NotifySoundIsClosing();
      StopSoundSystem();
      StopAndDestroyEngine();
    }

    m_OrganController = pNewOrganController;

    if (m_open && m_OrganController) {
      BuildAndStartEngine();
      StartSoundSystem();
      NotifySoundIsOpen();
    }
  }
}

void GOSoundSystem::SuspendAudioForPowerEvent() {
  // cancel a delayed resume left over from an earlier suspend/resume blip
  m_ResumeTimer.DeleteTimer(&m_ResumeCallback);

  if (m_State.load() == GOSoundDeviceState::SUSPENDED)
    return;
  // Leave a hung or already-lost device alone: m_open stays true while
  // DEVICE_LOST (we don't auto-close on watchdog timeout), but that does
  // NOT mean the stream is healthy - closing it is unlikely to succeed
  // (the old stream object is tied to a device session that's already
  // gone) and there is nothing useful to resume into afterwards anyway.
  GOSoundDeviceState stateBeforeSuspend = m_State.load();

  if (
    stateBeforeSuspend == GOSoundDeviceState::DRIVER_HUNG
    || stateBeforeSuspend == GOSoundDeviceState::DEVICE_LOST) {
    m_WasRunningBeforeSuspend = false;
    SetState(GOSoundDeviceState::SUSPENDED);
    return;
  }

  m_WasRunningBeforeSuspend = stateBeforeSuspend == GOSoundDeviceState::RUNNING;
  if (m_WasRunningBeforeSuspend) {
    wxLogWarning(_("Audio: suspend event received, closing the audio device."));

    // Windows expects WM_POWERBROADCAST handling to return quickly, so -
    // unlike AssureSoundIsClosed()/CloseSoundAsync() - this must not block
    // waiting for the (possibly hanging) driver Close() call. The engine
    // stop sequence below is pure in-process thread coordination, not a
    // driver call, so it stays synchronous; only the port close is
    // deferred.
    if (m_OrganController) {
      NotifySoundIsClosing();
      StopSoundSystem();
      StopAndDestroyEngine();
    }

    std::shared_ptr<GOSoundCloseJob> job = StartCloseJob();
    std::shared_ptr<std::atomic<bool>> aliveFlag = m_AliveFlag;

    if (wxTheApp)
      wxTheApp->CallAfter([this, job, aliveFlag]() {
        if (aliveFlag->load())
          ApplyCloseJobResult(job);
      });
  }
  SetState(GOSoundDeviceState::SUSPENDED);
}

void GOSoundSystem::ResumeAudioAfterPowerEvent() {
  if (m_State.load() != GOSoundDeviceState::SUSPENDED)
    return;

  m_ResumeTimer.DeleteTimer(&m_ResumeCallback);

  if (!m_WasRunningBeforeSuspend) {
    SetState(GOSoundDeviceState::CLOSED);
    return;
  }

  wxLogWarning(
    _("Audio: resume event received, reopening the audio device after a "
      "delay."));
  m_ResumeTimer.SetRelativeTimer(RESUME_DELAY_MS, &m_ResumeCallback);
}

void GOSoundSystem::DoDelayedResume() {
  m_WasRunningBeforeSuspend = false;
  if (m_State.load() == GOSoundDeviceState::SUSPENDED)
    AssureSoundIsOpen();
}

std::vector<GOSoundDevInfo> GOSoundSystem::GetAudioDevices(
  const GOPortsConfig &portsConfig) {
  // Getting a device list tries to open and close each device
  // Because some devices (ex. ASIO) cann't be open more than once
  // then close the current audio device
  AssureSoundIsClosed();
  m_DefaultAudioDevice = GOSoundDevInfo::getInvalideDeviceInfo();

  std::vector<GOSoundDevInfo> list
    = GOSoundPortFactory::getDeviceList(portsConfig);

  for (const auto &devInfo : list)
    if (devInfo.IsDefault()) {
      m_DefaultAudioDevice = devInfo;
      break;
    }
  return list;
}

const GOSoundDevInfo &GOSoundSystem::GetDefaultAudioDevice(
  const GOPortsConfig &portsConfig) {
  if (!m_DefaultAudioDevice.IsValid())
    GetAudioDevices(portsConfig);
  return m_DefaultAudioDevice;
}

void GOSoundSystem::FillDeviceNamePattern(
  const GOSoundDevInfo &deviceInfo, GODeviceNamePattern &pattern) {
  pattern.SetLogicalName(deviceInfo.GetDefaultLogicalName());
  pattern.SetRegEx(deviceInfo.GetDefaultNameRegex());
  pattern.SetPortName(deviceInfo.GetPortName());
  pattern.SetApiName(deviceInfo.GetApiName());
  pattern.SetPhysicalName(deviceInfo.GetFullName());
}

void GOSoundSystem::ResetMeters() {
  wxWindow *const topWindow = wxTheApp ? wxTheApp->GetTopWindow() : nullptr;

  if (topWindow) {
    wxCommandEvent event(wxEVT_METERS, 0);

    event.SetInt(0x1);
    topWindow->GetEventHandler()->AddPendingEvent(event);
  }
}

void GOSoundSystem::UpdateMeter() {
  /* Update meters */
  meter_counter += m_SamplesPerBuffer;
  if (meter_counter >= 6144) // update 44100 / (N / 2) = ~14 times per second
  {
    wxCommandEvent event(wxEVT_METERS, 0);
    event.SetInt(0x0);
    if (wxTheApp->GetTopWindow())
      wxTheApp->GetTopWindow()->GetEventHandler()->AddPendingEvent(event);
    meter_counter = 0;
  }
}

bool GOSoundSystem::AudioCallback(
  unsigned devIndex, GOSoundBufferMutable &outBuffer) {
  // realtime-safe: no logging, no locking, no allocation
  m_LastAudioCallbackMs.store(
    wxGetLocalTimeMillis().GetValue(), std::memory_order_relaxed);

  bool wasEntered = false;
  const unsigned nSamples = outBuffer.GetNFrames();

  if (m_IsRunning.load()) {
    if (nSamples == m_SamplesPerBuffer) {
      m_NCallbacksEntered.fetch_add(1);
      wasEntered = true;
    } else
      wxLogError(
        _("No sound output will happen. Samples per buffer has been "
          "changed by the sound driver to %d"),
        nSamples);
  }
  // assure that m_IsRunning has not yet been changed after
  // m_NCallbacksEntered.fetch_add, otherwise the control thread may not wait
  if (wasEntered && m_IsRunning.load()) {
    GOSoundOutput &device = m_AudioOutputs[devIndex];
    GOMutexLocker locker(device.mutex);

    while (device.wait && device.waiting)
      device.condition.Wait();

    unsigned cnt = m_CalcCount.fetch_add(1);
    m_SoundEngine.GetAudioOutput(
      devIndex, cnt + 1 >= m_AudioOutputs.size(), outBuffer);
    device.wait = true;
    unsigned count = m_WaitCount.fetch_add(1);

    if (count + 1 == m_AudioOutputs.size()) {
      m_SoundEngine.NextPeriod();
      UpdateMeter();

      m_SoundEngine.WakeupThreads();
      m_CalcCount.exchange(0);
      m_WaitCount.exchange(0);

      for (unsigned i = 0; i < m_AudioOutputs.size(); i++) {
        GOMutexLocker lock(m_AudioOutputs[i].mutex, i == devIndex);
        m_AudioOutputs[i].wait = false;
        m_AudioOutputs[i].condition.Signal();
      }
    }
  } else
    outBuffer.FillWithSilence();
  if (
    wasEntered && m_NCallbacksEntered.fetch_sub(1) <= 1
    && !m_IsRunning.load()) {
    // ensure that the control thread enters into m_NCallbackCondition.Wait()
    GOMutexLocker lk(m_CallbackMutex);

    // notify the control thread
    m_CallbackCondition.Broadcast();
  }
  return true;
}

wxString GOSoundSystem::getState() {
  if (!m_AudioOutputs.size())
    return _("No sound output occurring");
  wxString result = wxString::Format(
    _("%d samples per buffer, %d Hz\n"),
    m_SamplesPerBuffer,
    m_SoundEngine.GetSampleRate());
  for (unsigned i = 0; i < m_AudioOutputs.size(); i++)
    result = result + _("\n") + m_AudioOutputs[i].port->getPortState();
  return result;
}
