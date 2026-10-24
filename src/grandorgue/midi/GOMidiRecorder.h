/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2025 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOMIDIRECORDER_H
#define GOMIDIRECORDER_H

#include <wx/file.h>
#include <wx/string.h>

#include <vector>

#include "midi/events/GOMidiEvent.h"

#include "control/GOElementCreator.h"
#include "control/GOLabelControl.h"

#include "GOTime.h"
#include "GOTimerCallback.h"
#include "ptrvector.h"

class GOMidiMap;
class GOOrganController;

class GOMidiRecorder : public GOElementCreator, private GOTimerCallback {
  typedef struct {
    unsigned elementID;
    unsigned channel;
    unsigned key;
  } midi_map;

private:
  GOOrganController *m_OrganController;
  GOMidiMap &m_Map;
  GOLabelControl m_RecordingTime;
  unsigned m_RecordSeconds;
  unsigned m_NextChannel;
  unsigned m_NextNRPN;
  std::vector<midi_map> m_Mappings;
  std::vector<midi_map> m_Preconfig;
  unsigned m_OutputDevice;
  wxFile m_file;
  wxString m_Filename;
  // When non-empty, StartRecording() uses this path instead of MidiRecorderPath
  wxString m_PathOverride;
  bool m_DoRename;
  wxLongLong m_StartMs; // wall-clock milliseconds at recording start
  unsigned m_NoteOnCount; // number of note-on events written
  unsigned m_InitialSilenceMs; // silence inserted after setup sysex, before first real event
  // Note-ons deferred during PrepareRecording so they appear after the silence.
  bool m_deferNoteOns;
  std::vector<GOMidiEvent> m_deferredNoteOns;
  // Whether this instance has ODF-panel buttons (false for the auto recorder)
  bool m_hasButtons;
  char m_Buffer[2000];
  unsigned m_BufferPos;
  unsigned m_FileLength;
  GOTime m_Last;

  void ButtonStateChanged(int id, bool newState) override;

  void UpdateDisplay();
  void HandleTimer() override;

  void Ensure(unsigned length);
  void Flush();
  void EncodeLength(unsigned len);
  void Write(const void *data, unsigned len);
  void WriteEvent(GOMidiEvent &e);
  void SendEvent(GOMidiEvent &e);
  bool SetupMapping(unsigned element, bool isNRPN);

  // Internal: stop recording; if autoLoad is true and the setting is enabled,
  // signal the controller to load the just-saved file into the MIDI player.
  void StopRecordingInternal(bool fromUserStop);

public:
  explicit GOMidiRecorder(
    GOOrganController *organController, bool hasButtons = true);
  ~GOMidiRecorder();

  void SetOutputDevice(const wxString &device_id);
  void SetPathOverride(const wxString &path) { m_PathOverride = path; }
  void SetInitialSilenceMs(unsigned ms) { m_InitialSilenceMs = ms; }
  void PreconfigureMapping(const wxString &element, bool isNRPN);
  void PreconfigureMapping(
    const wxString &element, bool isNRPN, const wxString &reference);
  void SetSamplesetId(unsigned id1, unsigned id2);

  void SendMidiRecorderMessage(GOMidiEvent &e);

  void Clear();

  void StartRecording(bool rename);
  bool IsRecording();
  // Stop recording. If fromUserStop is true and AutoLoadAfterManualRecord is
  // enabled, the controller is notified to load the file into the MIDI player.
  void StopRecording(bool fromUserStop = false);

  void Load(GOConfigReader &cfg) override;
  GOEnclosure *GetEnclosure(const wxString &name, bool is_panel) override;
  GOLabelControl *GetLabelControl(const wxString &name, bool is_panel) override;
};

#endif
