/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOAutoMidiArchiver.h"

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/log.h>

#include "config/GOConfig.h"
#include "midi/GOMidiRecorder.h"
#include "midi/events/GOMidiEvent.h"

#include "GOOrganController.h"
#include "GOTimer.h"

GOAutoMidiArchiver::GOAutoMidiArchiver(
  GOOrganController &controller,
  GOMidiRecorder &recorder,
  GOTimer &timer)
  : r_controller(controller),
    r_recorder(recorder),
    r_timer(timer),
    m_active(false) {}

GOAutoMidiArchiver::~GOAutoMidiArchiver() { Deactivate(); }

void GOAutoMidiArchiver::Activate() { m_active = true; }

void GOAutoMidiArchiver::Deactivate() {
  m_active = false;
  r_timer.DeleteTimer(this);
  if (r_recorder.IsRecording())
    r_recorder.StopRecording();
}

void GOAutoMidiArchiver::OnMidiActivity(const GOMidiEvent &event) {
  if (!m_active)
    return;
  // Never start an archive recording from MIDI player playback events.
  if (r_controller.IsMidiPlayerPlaying())
    return;
  const GOConfig &cfg = r_controller.GetSettings();
  if (!cfg.AutoMidiArchiveEnabled())
    return;
  if (
    event.GetMidiType() != GOMidiEvent::MIDI_NOTE || event.GetValue() == 0)
    return;

  if (!r_recorder.IsRecording()) {
    const wxString archivePath = cfg.AutoMidiArchivePath();
    if (archivePath.IsEmpty()) {
      wxLogWarning(
        _("Auto MIDI archive: no archive folder configured, skipping."));
      return;
    }
    if (cfg.AutoMidiCleanupEnabled())
      CleanupOldFiles(archivePath);
    r_recorder.SetPathOverride(archivePath);
    r_recorder.SetInitialSilenceMs(1000);
    r_recorder.StartRecording(false);
  }

  // Restart the inactivity timer on every note-on. Delete any previous
  // entry first — GOTimer accumulates one entry per SetRelativeTimer call
  // and does not coalesce them, so stale entries would fire immediately
  // on the next Notify() and stop the next recording prematurely.
  const unsigned timeoutMs = cfg.AutoMidiInactivityTimeoutSec() * 1000u;
  r_timer.DeleteTimer(this);
  r_timer.SetRelativeTimer(timeoutMs, this);
}

void GOAutoMidiArchiver::HandleTimer() {
  if (r_recorder.IsRecording())
    r_recorder.StopRecording();
}

void GOAutoMidiArchiver::CleanupOldFiles(const wxString &archivePath) const {
  const GOConfig &cfg = r_controller.GetSettings();
  const unsigned maxAgeDays = cfg.AutoMidiCleanupAgeDays();
  if (maxAgeDays == 0)
    return;

  wxDir dir(archivePath);
  if (!dir.IsOpened())
    return;

  const wxDateTime cutoff
    = wxDateTime::Now().Subtract(wxDateSpan::Days(maxAgeDays));

  wxString filename;
  std::vector<wxString> toDelete;
  bool cont = dir.GetFirst(&filename, wxT("*.mid"), wxDIR_FILES);
  while (cont) {
    wxFileName fullPath(archivePath, filename);
    wxDateTime fileDate;
    fullPath.GetTimes(nullptr, nullptr, &fileDate);
    if (fileDate.IsValid() && fileDate < cutoff)
      toDelete.push_back(fullPath.GetFullPath());
    cont = dir.GetNext(&filename);
  }

  for (const wxString &path : toDelete) {
    if (!wxRemoveFile(path))
      wxLogWarning(_("Auto MIDI archive: could not delete old file: %s"), path);
  }
}
