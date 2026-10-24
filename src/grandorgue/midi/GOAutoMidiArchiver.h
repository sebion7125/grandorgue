/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOAUTOMIDIARCHIVER_H
#define GOAUTOMIDIARCHIVER_H

#include <wx/string.h>

#include "GOTimerCallback.h"

class GOMidiEvent;
class GOMidiRecorder;
class GOOrganController;
class GOTimer;

/**
 * Automatic MIDI archive recorder.
 *
 * When enabled via GOConfig::AutoMidiArchiveEnabled, this class monitors
 * incoming MIDI note events from GOOrganController::ProcessMidi(). On the
 * first note-on after a period of silence it starts the auto recorder.
 * An inactivity timer resets on each note-on and fires when no note has been
 * received for AutoMidiInactivityTimeoutSec seconds, at which point the
 * current archive recording is closed.
 *
 * The auto recorder runs completely independently of the manual recorder:
 * starting or stopping the manual recorder has no effect here, and vice versa.
 */
class GOAutoMidiArchiver final : private GOTimerCallback {
  GOOrganController &r_controller;
  GOMidiRecorder &r_recorder;
  GOTimer &r_timer;
  bool m_active; // archiver is currently enabled and organ is loaded

  void HandleTimer() override;
  void CleanupOldFiles(const wxString &archivePath) const;

public:
  GOAutoMidiArchiver(
    GOOrganController &controller,
    GOMidiRecorder &recorder,
    GOTimer &timer);
  ~GOAutoMidiArchiver();

  // Called once when the organ finishes loading (PreparePlayback).
  void Activate();

  // Called when the organ is unloaded (AbortPlayback). Stops any running
  // auto recording and cancels the inactivity timer.
  void Deactivate();

  // Called from GOOrganController::ProcessMidi() for every incoming MIDI event.
  // Only MIDI_NOTE events with velocity > 0 are used as activity triggers.
  void OnMidiActivity(const GOMidiEvent &event);
};

#endif /* GOAUTOMIDIARCHIVER_H */
