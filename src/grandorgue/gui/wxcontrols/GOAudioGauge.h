/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2023 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOAUDIOGAUGE_H
#define GOAUDIOGAUGE_H

#include <wx/control.h>
#include <wx/dcmemory.h>

class GOAudioGauge : public wxControl {
private:
  int m_Value;
  bool m_Clip;
  bool m_Update;
  /* Bitmap BEFORE the DC, deliberately: members are destroyed in reverse
   * declaration order, so the other way round tore the bitmap down while it
   * was still selected into the memory DC. wxWidgets requires a bitmap to be
   * deselected before it is destroyed; violating that corrupted the heap
   * during ~GOAudioGauge, and the damage only surfaced a few frees later -
   * when GTKDisconnect released this control's signal closures, or, further
   * along, when wx cleaned up its own modules. The destructor deselects
   * explicitly as well; the order alone would do it, but the pairing should
   * not depend on someone remembering why these two lines sit like this. */
  wxBitmap m_gauge;
  wxMemoryDC m_gaugedc;

  void OnPaint(wxPaintEvent &event);
  void Update();

public:
  GOAudioGauge(
    wxWindow *parent, wxWindowID id, const wxPoint &pos = wxDefaultPosition);
  ~GOAudioGauge(void);

  void SetValue(int what);
  void ResetClip();

  DECLARE_EVENT_TABLE()
};

#endif
