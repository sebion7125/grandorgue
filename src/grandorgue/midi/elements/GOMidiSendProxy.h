/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOMIDISENDPROXY_H
#define GOMIDISENDPROXY_H

class GOMidiSystem;
class GOMidiEvent;
class GOMidiRecorder;

class GOMidiSendProxy {
  GOMidiSystem *p_midi = nullptr;
  GOMidiRecorder *p_MidiRecorder = nullptr;
  GOMidiRecorder *p_AutoMidiRecorder = nullptr;
  // When non-null, SendMidiRecorderMessage routes only to this recorder.
  // Used during PrepareRecording() to isolate the state-dump to the newly
  // starting recorder without disturbing any already-running recorder.
  GOMidiRecorder *p_RecordingTarget = nullptr;
  bool m_suppressExternalSends = false;

public:
  void SetMidi(GOMidiSystem *pMidi, GOMidiRecorder *pMidiRecorder);
  void SetAutoMidiRecorder(GOMidiRecorder *pAutoRecorder);
  void SetRecordingTarget(GOMidiRecorder *pTarget);
  void ClearRecordingTarget();
  void SendMidiMessage(const GOMidiEvent &e);
  void SendMidiRecorderMessage(GOMidiEvent &e);
  void SetSuppressExternalSends(bool suppress) {
    m_suppressExternalSends = suppress;
  }
};

#endif /* GOMIDISENDPROXY_H */
