/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOMidiSendProxy.h"

#include "midi/GOMidiRecorder.h"
#include "midi/GOMidiSystem.h"

void GOMidiSendProxy::SetMidi(
  GOMidiSystem *pMidi, GOMidiRecorder *pMidiRecorder) {
  p_midi = pMidi;
  p_MidiRecorder = pMidiRecorder;
}

void GOMidiSendProxy::SetAutoMidiRecorder(GOMidiRecorder *pAutoRecorder) {
  p_AutoMidiRecorder = pAutoRecorder;
}

void GOMidiSendProxy::SetRecordingTarget(GOMidiRecorder *pTarget) {
  p_RecordingTarget = pTarget;
}

void GOMidiSendProxy::ClearRecordingTarget() { p_RecordingTarget = nullptr; }

void GOMidiSendProxy::SendMidiMessage(const GOMidiEvent &e) {
  if (p_midi && !m_suppressExternalSends)
    p_midi->Send(e);
}

void GOMidiSendProxy::SendMidiRecorderMessage(GOMidiEvent &e) {
  if (p_RecordingTarget) {
    p_RecordingTarget->SendMidiRecorderMessage(e);
    return;
  }
  if (p_MidiRecorder)
    p_MidiRecorder->SendMidiRecorderMessage(e);
  if (p_AutoMidiRecorder)
    p_AutoMidiRecorder->SendMidiRecorderMessage(e);
}
