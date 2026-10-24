/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2023 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOGUIPANELWIDGET_H
#define GOGUIPANELWIDGET_H

#include <atomic>
#include <chrono>
#include <thread>

#include <wx/bitmap.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include "primitives/GOBitmap.h"

class GOGUIPanel;

DECLARE_LOCAL_EVENT_TYPE(wxEVT_GOCONTROL, -1)
DECLARE_LOCAL_EVENT_TYPE(wxEVT_GOPREPARE_DONE, -1)

class GOGUIPanelWidget : public wxPanel {
private:
  GOGUIPanel *m_panel;
  wxImage m_BGImage;
  bool m_BGInit;
  GOBitmap m_Background;
  wxBitmap m_ClientBitmap;

  /**
   * The bitmap produced by the last full, sharp redraw (OnUpdate() right
   * after PrepareDraw()). UpdateSize() always rescales from this one for its
   * cheap live-drag preview rather than from m_ClientBitmap - which during a
   * drag may itself already be a rescaled preview - to avoid repeatedly
   * rescaling an already-rescaled (and thus progressively softer) image.
   */
  wxBitmap m_LastFullBitmap;
  double m_Scale;
  double m_FontScale;

  /**
   * Fires once resizing has been idle for a short while. UpdateSize() only
   * does a cheap rescale of m_LastFullBitmap on every WM_SIZE tick (a live
   * drag can fire dozens of those); the expensive, sharp re-render of every
   * control bitmap only runs once here, after the user stops moving the
   * border - otherwise every pixel of mouse movement triggered a full
   * BICUBIC rescale of every key/stop/texture, making resizing unusably
   * slow.
   */
  wxTimer m_ResizeTimer;

  // --- Async resize render -------------------------------------------------
  // After a resize settles OnResizeTimer shows an immediate BICUBIC-composite
  // and then launches StartAsyncPrepareDraw(). The background thread runs
  // PrepareDraw (rebuilds all per-control bitmaps at the new scale, using the
  // shared GOBitmap cache to avoid duplicate BICUBIC work) followed by a full
  // Draw into a private wxBitmap, then posts wxEVT_GOPREPARE_DONE back to the
  // GUI thread. During a live drag UpdateSize() starts an async render at most
  // once per ASYNC_THROTTLE_MS so the panel sharpens progressively.

  std::thread m_PrepareThread;
  std::atomic<bool> m_PrepareRunning{false};
  std::atomic<bool> m_ShuttingDown{false};
  bool m_PendingAsyncRedraw = false; // re-run async once current finishes
  std::chrono::steady_clock::time_point m_LastAsyncStart;
  unsigned m_AsyncGeneration = 0; // incremented per StartAsyncPrepareDraw call
  double m_AsyncRenderScale = 1.0; // m_TargetScale captured when thread started

  /**
   * The scale at which the background thread last completed PrepareDraw (i.e.
   * the actual scale of the per-control m_ResultBitmaps right now). Written by
   * the background thread, read on the GUI thread in StartAsyncPrepareDraw() to
   * decide whether TakeSnapshotBitmaps() is safe. Atomic because the GUI thread
   * can call StartAsyncPrepareDraw() between the thread setting m_PrepareRunning
   * to false and the GUI processing the wxEVT_GOPREPARE_DONE event.
   */
  std::atomic<double> m_ResultBitmapScale{1.0};

  /**
   * True if TakeSnapshotBitmaps() was called with per-control bitmaps at
   * m_Scale (so GetResultBitmap() returns the snapshot, which is at m_Scale,
   * and OnGOControl can safely draw with GODC(m_Scale)).  False during the
   * edge-case window where an async completed but its event was not yet
   * processed before a new async started, leaving m_ResultBitmaps at a scale
   * that differs from m_Scale.
   */
  bool m_SnapshotsTaken = false;

  /**
   * The scale the current m_LastFullBitmap and per-control bitmaps were built
   * at. Stays at the old value while a background render is in progress;
   * updated to m_TargetScale in OnAsyncPrepareDone().  OnGOControl uses
   * m_Scale (not m_TargetScale) for its GODC so that incremental draws work
   * correctly on the old-scale bitmaps even while the new scale is being
   * prepared in the background — keeping controls interactive.
   */
  double m_TargetScale = 1.0;
  // -------------------------------------------------------------------------

  /**
   * A point where the mouse has been pressed. Used for deduplication
   */
  wxPoint m_PressedPoint;

  void initFont();
  void OnCreate(wxWindowCreateEvent &event);
  void OnDraw(wxDC *dc);
  void OnErase(wxEraseEvent &event);
  void OnPaint(wxPaintEvent &event);
  void OnGOControl(wxCommandEvent &event);
  void OnResizeTimer(wxTimerEvent &event);
  void StartAsyncPrepareDraw();
  void OnAsyncPrepareDone(wxCommandEvent &event);

  /**
   * Stores m_PressedPoint and calls m_Panel->HandleMousePress with relative
   * coords
   */
  void HandleMousePress(const wxPoint &point, bool isRight);
  /**
   * calls m_Panel->HandleMouseRelease and clears m_PressedPoint
   */
  void HandleMouseRelease(bool isRight);

  void OnMouseMove(wxMouseEvent &event);
  void OnMouseLeftDown(wxMouseEvent &event);
  void OnMouseLeftUp(wxMouseEvent &event);
  void OnMouseLeftDclick(wxMouseEvent &event);
  void OnMouseRightDown(wxMouseEvent &event);
  void OnMouseScroll(wxMouseEvent &event);
  bool ForwardMouseEvent(wxMouseEvent &event);
  void OnKeyCommand(wxKeyEvent &event);
  void OnKeyUp(wxKeyEvent &event);

public:
  GOGUIPanelWidget(
    GOGUIPanel *panel, wxWindow *parent, wxWindowID id = wxID_ANY);
  ~GOGUIPanelWidget();

  void Focus();
  void OnUpdate();
  wxSize UpdateSize(wxSize size);
  void TriggerImmediateRedraw();

  DECLARE_EVENT_TABLE();
};

#endif
