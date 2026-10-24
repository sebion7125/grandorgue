/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2025 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOGUIENCLOSURE_H
#define GOGUIENCLOSURE_H

#include <wx/colour.h>

#include <vector>

#include "primitives/GOBitmap.h"
#include "primitives/GOFont.h"

#include "GOGUIMidiControl.h"

class GOEnclosure;

class GOGUIEnclosure : public GOGUIMidiControl {
private:
  GOEnclosure *m_enclosure;
  unsigned m_FontSize;
  wxString m_FontName;
  GOFont m_Font;
  wxColor m_TextColor;
  wxString m_Text;
  wxRect m_TextRect;
  unsigned m_TextWidth;
  wxRect m_MouseRect;
  int m_MouseAxisStart;
  int m_MouseAxisEnd;
  unsigned m_TileOffsetX;
  unsigned m_TileOffsetY;
  std::vector<GOBitmap> m_Bitmaps;

  /**
   * Parameters of the last PrepareDraw(), kept for lazy builds: an enclosure
   * holds up to 128 state bitmaps of which exactly one is ever displayed, so
   * PrepareDraw builds only that one and Draw() builds any other state on
   * first use (a few ms, imperceptible). Eagerly building all states
   * dominated the whole panel render: the Arnstadt mixer spent 5.1 of 5.7 s
   * on 10 x 128 slider states, ~99% of them never shown.
   */
  double m_PrepScale;
  GOBitmap *m_PrepBackground;

  /** The index into m_Bitmaps that Draw() would display right now. */
  unsigned GetDisplayedBitmapIndex() const;

public:
  GOGUIEnclosure(GOGUIPanel *panel, GOEnclosure *control);

  void Init(GOConfigReader &cfg, wxString group);
  void Load(GOConfigReader &cfg, wxString group);
  void Layout();

  void PrepareDraw(double scale, GOBitmap *background);
  void Draw(GODC &dc);
  void TakeSnapshotBitmaps() override { for (auto &b : m_Bitmaps) b.TakeSnapshot(); }
  void ClearSnapshotBitmaps() override { for (auto &b : m_Bitmaps) b.ClearSnapshot(); }
  bool HandleMousePress(int x, int y, bool right, GOGUIMouseState &state);
  bool HandleMouseScroll(int x, int y, int amount);
};

#endif
