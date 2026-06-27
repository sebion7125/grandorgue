/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDSYSTEM_H
#define GOSOUNDSYSTEM_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <wx/log.h>
#include <wx/string.h>

#include "config/GOAudioDeviceConfig.h"
#include "config/GODeviceNamePattern.h"
#include "config/GOPortsConfig.h"
#include "midi/GOMidiSystem.h"
#include "threading/GOCondition.h"
#include "threading/GOMutex.h"

#include "ptrvector.h"

#include "GOSoundDevInfo.h"
#include "GOSoundOrganEngine.h"
#include "GOSoundRecorder.h"
#include "GOTimer.h"
#include "GOTimerCallback.h"

class GOConfig;
class GOOrganController;
class GOSoundBufferMutable;
class GOSoundPort;

/**
 * The lifecycle state of the audio device, as tracked by GOSoundSystem.
 * This is distinct from m_open: m_open only records that GOSoundSystem
 * believes it has asked the backend to open a stream, while this state also
 * reflects whether the stream is actually alive (see the device-loss
 * watchdog and suspend/resume handling below).
 */
enum class GOSoundDeviceState {
  CLOSED,
  OPENING,
  RUNNING,
  SUSPENDED,
  DEVICE_LOST,
  CLOSING,
  OPEN_FAILED,
  DRIVER_HUNG,
};

/**
 * This class represents a GrandOrgue-wide sound system. It may be used even
 * without a loaded organ
 */

class GOSoundSystem : private GOTimerCallback {
  class GOSoundOutput {
  public:
    GOSoundPort *port;
    GOMutex mutex;
    GOCondition condition;
    bool wait;
    bool waiting;

    GOSoundOutput() : condition(mutex) {
      port = 0;
      wait = false;
      waiting = false;
    }

    GOSoundOutput(const GOSoundOutput &old) : condition(mutex) {
      port = old.port;
      wait = old.wait;
      waiting = old.waiting;
    }

    const GOSoundOutput &operator=(const GOSoundOutput &old) {
      port = old.port;
      wait = old.wait;
      waiting = old.waiting;
      return *this;
    }
  };

private:
  GOConfig &m_config;

  GOMidiSystem m_midi;
  GOSoundRecorder m_AudioRecorder;
  GOSoundOrganEngine m_SoundEngine;

  bool m_open;
  std::atomic<GOSoundDeviceState> m_State;
  bool logSoundErrors;
  unsigned m_SampleRate;
  unsigned m_SamplesPerBuffer;
  std::vector<GOSoundOutput> m_AudioOutputs;

  wxString m_LastErrorMessage;

  GOOrganController *m_OrganController;

  GOSoundDevInfo m_DefaultAudioDevice;

  std::atomic_bool m_IsRunning;
  // counter of audio callbacks that have been entered but have not yet been
  // exited
  std::atomic_uint m_NCallbacksEntered;

  // Timestamp (ms) of the last time the backend invoked AudioCallback,
  // updated unconditionally (even while m_IsRunning is false) so the planned
  // watchdog can tell a genuinely silent device apart from a lost one
  std::atomic<int64_t> m_LastAudioCallbackMs;

  // For waiting for and notifying when m_NCallbacksEntered bacomes 0
  GOMutex m_CallbackMutex;
  GOCondition m_CallbackCondition;

  GOMutex m_lock;

  unsigned meter_counter;

  std::atomic_uint m_WaitCount;
  std::atomic_uint m_CalcCount;

  // Polls m_LastAudioCallbackMs while m_State is RUNNING and flags
  // DEVICE_LOST if the backend has stopped invoking AudioCallback. Runs on
  // the GUI thread, started/stopped together with the audio port lifecycle
  // (OpenSoundAsync/CloseSoundAsync), independent of the organ engine.
  GOTimer m_Watchdog;

  // Separate GOTimerCallback identity for the delayed resume-after-suspend
  // timer below, kept apart from GOSoundSystem's own HandleTimer() (used for
  // the watchdog) so that GOTimer::DeleteTimer() can cancel one without
  // accidentally cancelling the other.
  class GOSoundResumeCallback : public GOTimerCallback {
  private:
    GOSoundSystem &r_System;

  public:
    explicit GOSoundResumeCallback(GOSoundSystem &system) : r_System(system) {}
    void HandleTimer() override { r_System.DoDelayedResume(); }
  };

  GOSoundResumeCallback m_ResumeCallback;
  GOTimer m_ResumeTimer;
  bool m_WasRunningBeforeSuspend;

  // Runs every queued task on a single, persistent background thread - in
  // order, one at a time, forever. This matters because PortAudio's ASIO
  // and WASAPI host APIs initialise COM as apartment-threaded (see
  // pa_win_coinitialize.c / RtApiAsio's constructor in the vendored
  // PortAudio/RtAudio sources) on whichever thread first touches them, and
  // ASIO in particular documents that it "cannot run on a multi-threaded
  // apartment". Calling Open() on one thread and later Close() on another
  // (e.g. a fresh std::thread per operation) crosses that apartment
  // boundary, which is consistent with hangs/crashes seen in practice.
  // Routing every driver call through the same dedicated thread restores
  // the "always the same thread" property the old, fully synchronous code
  // had on the GUI thread - just off the GUI thread, so it can no longer
  // freeze it.
  class GOSoundAudioWorker {
  private:
    struct State {
      std::mutex mutex;
      std::condition_variable condition;
      std::deque<std::function<void()>> tasks;
      bool stop = false;
    };

    // Held by both GOSoundAudioWorker and the (detached) thread itself, so
    // the mutex/condition/queue stay alive for as long as the thread might
    // still touch them, even if GOSoundAudioWorker is destroyed first while
    // the thread is stuck running a task and never gets back to checking
    // `stop`.
    std::shared_ptr<State> p_State;

  public:
    GOSoundAudioWorker() : p_State(std::make_shared<State>()) {
      std::shared_ptr<State> state = p_State;

      std::thread([state]() {
        for (;;) {
          std::function<void()> task;
          {
            std::unique_lock<std::mutex> lock(state->mutex);

            state->condition.wait(
              lock, [&state] { return state->stop || !state->tasks.empty(); });
            if (state->stop && state->tasks.empty())
              return;
            task = std::move(state->tasks.front());
            state->tasks.pop_front();
          }
          // An exception escaping a std::thread's entry function calls
          // std::terminate() and kills the whole process - and this loop
          // IS that entry function for every task ever posted here, for
          // the rest of the process's lifetime. Individual tasks (e.g.
          // OpenJobWorker) have their own try/catch for proper error
          // reporting, but this catch-all is the one that actually
          // guarantees a single bad task can never bring down GO.
          try {
            task();
          } catch (...) {
            wxLogError("GOSoundAudioWorker: an audio task threw an unhandled "
                       "exception; ignoring it to avoid crashing the process.");
          }
        }
      }).detach();
    }

    ~GOSoundAudioWorker() {
      std::lock_guard<std::mutex> lock(p_State->mutex);

      p_State->stop = true;
      p_State->condition.notify_all();
    }

    void Post(std::function<void()> task) {
      std::lock_guard<std::mutex> lock(p_State->mutex);

      p_State->tasks.push_back(std::move(task));
      p_State->condition.notify_one();
    }
  };

  GOSoundAudioWorker m_AudioWorker;

  // A job for opening the audio ports off the GUI thread. Only the inputs
  // below are read by the worker thread; the worker never touches
  // GOSoundSystem (`this`) itself while building `outputs`, so
  // GOSoundSystem's lifetime is irrelevant to that part of the work. Once
  // `done`, the result is collected either by the bounded wait inside
  // OpenSoundAsync() (the common, fast case) or by a completion posted to
  // the GUI thread for a worker that finishes later than the timeout - the
  // `applied` flag makes ApplyOpenJobResult() safe to call from both places.
  struct GOSoundOpenJob {
    // inputs, snapshotted on the GUI thread before the worker starts
    GOPortsConfig portsConfig;
    std::vector<GOAudioDeviceConfig> audioConfig;
    GODeviceNamePattern defaultDevicePattern;
    unsigned sampleRate = 0;
    unsigned samplesPerBuffer = 0;

    // completion signalling; touched by both threads only while holding
    // mutex. Deliberately plain std::mutex/condition_variable rather than
    // GOMutex/GOCondition, since this needs a true bounded wait_for() with
    // an absolute give-up time, which GOCondition::WaitOrStop() does not
    // provide (it either waits forever for a signal, or polls forever on a
    // fixed interval while a GOThread has not been told to stop).
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    bool applied = false;

    // outputs, written by the worker thread before setting done = true
    bool ok = false;
    wxString errorMessage;
    std::vector<GOSoundOutput> outputs;
  };

  // A job for closing already-detached audio ports off the GUI thread. By
  // the time this runs, the ports it owns are no longer referenced by
  // GOSoundSystem at all (see StartCloseJob), so unlike GOSoundOpenJob, its
  // *worker* needs no lifetime guard for GOSoundSystem. Applying its result
  // (clearing m_PendingCloseJob) does touch GOSoundSystem, though, so that
  // part is guarded the same way as the open job's, via m_AliveFlag.
  struct GOSoundCloseJob {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    bool applied = false;
    std::vector<GOSoundOutput> outputs;
  };

  // A job for enumerating audio devices off the GUI thread. Listing
  // devices constructs fresh RtAudio/PortAudio host API objects just like
  // OpenJobWorker does (see GOSoundRtPort::create()/addDevices()), so it
  // needs to run on the same GOSoundAudioWorker thread for the same
  // apartment-threading reason - otherwise enumeration on the GUI thread
  // could itself cross the apartment boundary against Open()/Close()/
  // StartStream() on the worker thread.
  struct GOSoundEnumJob {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    std::vector<GOSoundDevInfo> result;
  };

  // Sentinel set to false as the first statement in ~GOSoundSystem() and
  // captured by value (as a shared_ptr, so the flag itself outlives
  // GOSoundSystem if need be) into a worker's GUI-thread completion
  // callback, letting that callback detect a destroyed GOSoundSystem
  // instead of risking a use-after-free.
  std::shared_ptr<std::atomic<bool>> m_AliveFlag;

  // Set by StartCloseJob() and cleared by ApplyCloseJobResult() once that
  // job is done. While set, OpenSoundAsync() waits for it before opening a
  // new port for the same physical device, so a close that was fired
  // without waiting (see SuspendAudioForPowerEvent) can never race with a
  // subsequent open.
  std::shared_ptr<GOSoundCloseJob> m_PendingCloseJob;

  void OpenJobWorker(std::shared_ptr<GOSoundOpenJob> job);
  void ApplyOpenJobResult(const std::shared_ptr<GOSoundOpenJob> &job);

  /** Detaches m_AudioOutputs and starts closing them on a worker thread,
   *  without waiting for it. Safe to call repeatedly; the caller decides
   *  whether/how long to wait for the result via m_PendingCloseJob. */
  std::shared_ptr<GOSoundCloseJob> StartCloseJob();
  void ApplyCloseJobResult(const std::shared_ptr<GOSoundCloseJob> &job);

  /** Lists audio devices on the GOSoundAudioWorker thread and waits up to
   *  AUDIO_ENUM_TIMEOUT_MS for the result; returns an empty list if the
   *  worker doesn't return in time rather than blocking the GUI forever. */
  std::vector<GOSoundDevInfo> EnumerateAudioDevices(
    const GOPortsConfig &portsConfig);

  /** Opens the audio ports off the GUI thread. Waits up to timeoutMs for
   *  the common case, but gives up and marks DRIVER_HUNG - instead of
   *  blocking the GUI thread forever - if the backend doesn't return in
   *  time; the worker keeps running in the background and is still applied
   *  if it eventually succeeds. Also waits (within the same timeoutMs
   *  budget) for any still-pending close job first - see
   *  m_PendingCloseJob. */
  bool OpenSoundAsync(unsigned timeoutMs);
  /** Calls StartCloseJob() and waits up to timeoutMs for it, then marks
   *  CLOSED or DRIVER_HUNG accordingly. Use this when the caller needs a
   *  definite answer soon (e.g. AssureSoundIsClosed()); use StartCloseJob()
   *  directly when the caller must not block at all (e.g. a Windows power
   *  suspend handler, which the OS expects to return quickly). */
  void CloseSoundAsync(unsigned timeoutMs);

  void OpenMidi() { m_midi.Open(); }

  /** Update m_State and log the transition, if any */
  void SetState(GOSoundDeviceState newState);

  /** GOTimerCallback: checks m_LastAudioCallbackMs for the watchdog */
  void HandleTimer() override;

  /** Reopens the device after a suspend/resume cycle, if still wanted */
  void DoDelayedResume();

  void UpdateMeter();
  void ResetMeters();

  /** Start audio streams and mark system as running */
  void StartSoundSystem();
  /** Notify the organ controller that sound is open and begin playback */
  void NotifySoundIsOpen();

  /** Notify the organ controller that sound is about to close */
  void NotifySoundIsClosing();
  /** Stop audio streams and wait for all callbacks to finish */
  void StopSoundSystem();

  /** Build the sound organ engine and start its worker threads */
  void BuildAndStartEngine();
  /** Stop the sound organ engine worker threads and destroy the engine */
  void StopAndDestroyEngine();

public:
  static void FillDeviceNamePattern(
    const GOSoundDevInfo &deviceInfo, GODeviceNamePattern &pattern);

  GOSoundSystem(GOConfig &settings);
  ~GOSoundSystem();

  GOConfig &GetSettings() { return m_config; }
  GOMidiSystem &GetMidi() { return m_midi; }
  GOSoundOrganEngine &GetEngine() { return m_SoundEngine; }

  std::vector<GOSoundDevInfo> GetAudioDevices(const GOPortsConfig &portsConfig);
  const GOSoundDevInfo &GetDefaultAudioDevice(const GOPortsConfig &portsConfig);
  wxString getLastErrorMessage() const { return m_LastErrorMessage; }
  GOOrganController *GetOrganFile() { return m_OrganController; }
  unsigned GetSampleRate() const { return m_SampleRate; }
  unsigned GetSamplesPerBuffer() const { return m_SamplesPerBuffer; }
  wxString getState();
  GOSoundDeviceState GetDeviceState() const { return m_State.load(); }

  void SetLogSoundErrorMessages(bool isVisible) { logSoundErrors = isVisible; }

  bool AssureSoundIsOpen();
  void AssureSoundIsClosed();
  void AssignOrganFile(GOOrganController *pNewOrganController);

  /** Closes the audio device immediately, e.g. on a system suspend event */
  void SuspendAudioForPowerEvent();
  /** Schedules reopening the device after a delay, e.g. on system resume */
  void ResumeAudioAfterPowerEvent();

  bool AudioCallback(unsigned devIndex, GOSoundBufferMutable &outBuffer);
};

#endif
