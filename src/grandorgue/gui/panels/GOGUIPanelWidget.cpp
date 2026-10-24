
/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOGUIPanelWidget.h"

#include <chrono>
#include <cmath>
#include <cstdio>

#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/utils.h>

#include "primitives/GODC.h"
#include "primitives/GOFont.h"

#include "GOGUIControl.h"
#include "GOGUIPanel.h"
#include "GOKeyConvert.h"

DEFINE_LOCAL_EVENT_TYPE(wxEVT_GOCONTROL)
DEFINE_LOCAL_EVENT_TYPE(wxEVT_GOPREPARE_DONE)

namespace {
enum { ID_PANEL_RESIZE_TIMER = wxID_HIGHEST + 1 };
constexpr int RESIZE_SETTLE_MS = 80;
constexpr int ASYNC_THROTTLE_MS = 500;

/* Render on the GUI thread unless GO_ASYNC_PANEL_RENDER says otherwise.
 *
 * The render body is not thread-safe, and cannot be made so by locking: it
 * builds wxBitmaps, drives a wxMemoryDC and lays out text through
 * GODC::DrawText. Under GTK those are cairo, GDK and Pango objects, and
 * wxWidgets states plainly that its GUI classes belong to the main thread
 * alone. GOFont additionally copies *wxNORMAL_FONT - a process-wide stock
 * object whose reference count is an ordinary int, shared with every paint
 * the GUI thread performs.
 *
 * So the other party in the race is the GUI thread itself, which keeps
 * painting while a render runs; no mutex among the render threads can
 * exclude it. That showed up as heap corruption during shutdown, detected
 * far from its cause - once inside the shared bitmap cache, and after that
 * was serialised, again in wxStockGDI::DeleteAll.
 *
 * The switch stays so the asynchronous path remains reachable for
 * measurement; it must not be the default until the work is split so that
 * only the pixel arithmetic (wxImage scaling) leaves the GUI thread. Read
 * once, so it costs nothing after the first call. */
bool IsSyncRender() {
  static const bool sync = [] {
    const char *e = getenv("GO_ASYNC_PANEL_RENDER");
    return !(e && *e && *e != '0');
  }();
  return sync;
}
} // namespace

BEGIN_EVENT_TABLE(GOGUIPanelWidget, wxPanel)
EVT_ERASE_BACKGROUND(GOGUIPanelWidget::OnErase)
EVT_PAINT(GOGUIPanelWidget::OnPaint)
EVT_TIMER(ID_PANEL_RESIZE_TIMER, GOGUIPanelWidget::OnResizeTimer)
EVT_COMMAND(0, wxEVT_GOCONTROL, GOGUIPanelWidget::OnGOControl)
EVT_COMMAND(0, wxEVT_GOPREPARE_DONE, GOGUIPanelWidget::OnAsyncPrepareDone)
EVT_MOTION(GOGUIPanelWidget::OnMouseMove)
EVT_LEFT_DOWN(GOGUIPanelWidget::OnMouseLeftDown)
EVT_LEFT_DCLICK(GOGUIPanelWidget::OnMouseLeftDclick)
EVT_LEFT_UP(GOGUIPanelWidget::OnMouseLeftUp)
EVT_RIGHT_DOWN(GOGUIPanelWidget::OnMouseRightDown)
EVT_RIGHT_DCLICK(GOGUIPanelWidget::OnMouseRightDown)
EVT_MOUSEWHEEL(GOGUIPanelWidget::OnMouseScroll)
EVT_KEY_DOWN(GOGUIPanelWidget::OnKeyCommand)
EVT_KEY_UP(GOGUIPanelWidget::OnKeyUp)
END_EVENT_TABLE()

const wxPoint default_point(wxDefaultCoord, wxDefaultCoord);

GOGUIPanelWidget::GOGUIPanelWidget(
  GOGUIPanel *panel, wxWindow *parent, wxWindowID id)
  : wxPanel(parent, id),
    m_panel(panel),
    m_BGInit(false),
    m_Scale(1),
    m_FontScale(1),
    m_ResizeTimer(this, ID_PANEL_RESIZE_TIMER),
    m_PressedPoint(default_point) {
  m_Background.SetSourceImage(&m_BGImage);
  initFont();
  SetLabel(m_panel->GetName());
  m_ClientBitmap.Create(
    m_panel->GetWidth() * m_Scale, m_panel->GetHeight() * m_Scale);
  m_panel->PrepareDraw(m_Scale, NULL);
  OnUpdate();
  m_LastFullBitmap = m_ClientBitmap;
  m_BGImage = m_ClientBitmap.ConvertToImage();
  m_Background.BuildScaledBitmap(m_Scale, wxRect(0, 0, 0, 0), NULL);
  m_BGInit = true;
  SetCanFocus(m_panel->IsKeyboardInputUsed());
}

GOGUIPanelWidget::~GOGUIPanelWidget() {
  m_ResizeTimer.Stop();
  m_ShuttingDown.store(true, std::memory_order_release);
  // Join the background thread before destruction to prevent use-after-free.
  // If a thread is running this may block briefly while PrepareDraw finishes.
  if (m_PrepareThread.joinable())
    m_PrepareThread.join();
  // Discard any wxEVT_GOPREPARE_DONE that was already queued before join.
  DeletePendingEvents();
}

void GOGUIPanelWidget::initFont() {
  wxMemoryDC dc;
  GOFont font;
  wxCoord cx, cy;
  font.SetPoints(39);
  dc.SetFont(font.GetFont(1));
  dc.GetTextExtent(wxT("M"), &cx, &cy);
  m_FontScale = 62.0 / cy;
}

void GOGUIPanelWidget::Focus() {
  const bool isKeyboardUsed = m_panel->IsKeyboardInputUsed();

  SetCanFocus(isKeyboardUsed);
  if (isKeyboardUsed && !HasFocus())
    SetFocus();
}

wxSize GOGUIPanelWidget::UpdateSize(wxSize size) {
  double scaleX = size.GetWidth() / (double)m_panel->GetWidth();
  double scaleY = size.GetHeight() / (double)m_panel->GetHeight();
  m_TargetScale = scaleX < scaleY ? scaleX : scaleY;
  if (m_TargetScale > 4)
    m_TargetScale = 4;
  if (m_TargetScale < 0.25) // Without this limit, sizing too small causes a crash
    m_TargetScale = 0.25;

  // Display dimensions are based on the TARGET (window) scale, not the render
  // scale m_Scale. m_Scale stays at the old value while the background thread
  // is preparing bitmaps at m_TargetScale, so that OnGOControl can continue
  // drawing with the existing bitmaps and keep controls interactive.
  const int targetWidth = int(m_panel->GetWidth() * m_TargetScale + 0.5);
  const int targetHeight = int(m_panel->GetHeight() * m_TargetScale + 0.5);

  // Cheap immediate feedback: bilinear-scale the last sharp render to the new
  // display size. Always from m_LastFullBitmap (which is at m_Scale), never
  // from m_ClientBitmap (which during a drag is already a bilinear preview —
  // chaining would compound quality loss tick by tick).
  if (m_LastFullBitmap.IsOk() && targetWidth > 0 && targetHeight > 0)
    m_ClientBitmap = (wxBitmap)m_LastFullBitmap.ConvertToImage().Scale(
      targetWidth, targetHeight, wxIMAGE_QUALITY_NORMAL);
  SetSize(targetWidth, targetHeight);
  Refresh();

  /* During a live drag: kick off a render at most once per ASYNC_THROTTLE_MS
   * so the panel sharpens progressively.
   *
   * Only worth doing when that render runs on a background thread. Rendered
   * on the GUI thread it is a full BICUBIC pass over every control in the
   * middle of the drag - the very stall the debounce timer exists to avoid,
   * just at a fixed interval instead of on every tick. Without it the drag
   * stays as smooth as the bilinear preview allows and the sharp render
   * happens once, in OnResizeTimer(), after the border comes to rest. */
  if (!IsSyncRender()) {
    using Clock = std::chrono::steady_clock;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - m_LastAsyncStart);
    if (!m_PrepareRunning.load() && elapsed.count() >= ASYNC_THROTTLE_MS)
      StartAsyncPrepareDraw();
  }

  m_ResizeTimer.StartOnce(RESIZE_SETTLE_MS);
  return GetSize();
}

void GOGUIPanelWidget::OnResizeTimer(wxTimerEvent &WXUNUSED(event)) {
  // On X11 the window manager can deliver WM_SIZE events more than
  // RESIZE_SETTLE_MS apart (slow drag, compositor throttle). If the left
  // button is still held the user is still dragging — re-arm the debounce
  // rather than running the expensive render mid-drag.
  if (wxGetMouseState().LeftIsDown()) {
    m_ResizeTimer.StartOnce(RESIZE_SETTLE_MS);
    return;
  }

  /* Already rendered sharply at exactly this scale? Then there is nothing a
   * re-render could improve - control-state changes are painted incrementally
   * by OnGOControl, never via a full render. Without this guard a settle
   * after a slow render ran the full pass up to three times back to back
   * (measured on the Arnstadt mixer: 3 x 5.7 s for identical output),
   * because size events arriving during a long render re-armed the timer. */
  if (
    std::abs(m_TargetScale - m_Scale) < 1e-6
    && std::abs(
         m_ResultBitmapScale.load(std::memory_order_acquire) - m_TargetScale)
      < 1e-6)
    return;

  const int targetW = int(m_panel->GetWidth() * m_TargetScale + 0.5);
  const int targetH = int(m_panel->GetHeight() * m_TargetScale + 0.5);

  if (m_LastFullBitmap.IsOk() && targetW > 0 && targetH > 0) {
    // Show an immediate bilinear composite of the last sharp render while the
    // background thread rebuilds at the new scale. Use the same filter that the
    // live preview path (OnGOControl) uses so the blur appearance stays
    // consistent across all phases.
    m_ClientBitmap = (wxBitmap)m_LastFullBitmap.ConvertToImage().Scale(
      targetW, targetH, wxIMAGE_QUALITY_NORMAL);
    Refresh();
    Update();
  }

  // Launch background PrepareDraw + full render. Results arrive via
  // wxEVT_GOPREPARE_DONE → OnAsyncPrepareDone() on the GUI thread.
  StartAsyncPrepareDraw();
}

void GOGUIPanelWidget::TriggerImmediateRedraw() {
  m_ResizeTimer.StartOnce(0);
}

void GOGUIPanelWidget::StartAsyncPrepareDraw() {
  // If a render is already in flight *for the same target scale*, let it
  // finish (no need to queue another pass).
  if (m_PrepareRunning.load(std::memory_order_acquire)) {
    if (std::abs(m_TargetScale - m_AsyncRenderScale) < 1e-6)
      return;
  }

  // Allow at most one background render at a time. If one is already running
  // for a different scale, set a flag so it re-runs once finished.
  if (m_PrepareRunning.exchange(true)) {
    m_PendingAsyncRedraw = true;
    return;
  }
  m_PendingAsyncRedraw = false;
  m_LastAsyncStart = std::chrono::steady_clock::now();
  const unsigned gen = ++m_AsyncGeneration;
  m_AsyncRenderScale = m_TargetScale; // remember which scale this render is for

  /* Join the previous thread, do NOT detach it.
   *
   * Detaching was reasoned as safe because m_PrepareRunning is false here, so
   * the old thread has passed its store(false) and is "at most posting the
   * event". But posting is exactly the problem: it still reads m_ShuttingDown
   * and calls wxQueueEvent(this, ...), both through this widget. A detached
   * thread is invisible to the destructor - which can only join a joinable
   * one - so if the widget dies inside that window, those accesses land in
   * freed memory. With an organ that has many panels there are many such
   * windows.
   *
   * Joining closes the hole and costs next to nothing precisely because of
   * the same reasoning: the thread is a few instructions from returning. */
  if (m_PrepareThread.joinable())
    m_PrepareThread.join();

  // Timing diagnostic (2026-07-27): split a full settle-render into pure
  // pixel arithmetic vs GDI-bound work, to decide whether pre-scaling on a
  // worker thread (thread-safe: private wxImage copies, no GUI objects)
  // would eliminate the GUI freeze. One stderr line per render, see below.
  GOBitmap::ResetRenderStats();
  const auto renderT0 = std::chrono::steady_clock::now();

  // Build the background GOBitmap on the GUI thread (modifies m_Background).
  if (m_BGInit)
    m_Background.BuildScaledBitmap(m_TargetScale, wxRect(0, 0, 0, 0), NULL);

  // Flush the shared scale cache: entries from m_Scale are stale for m_TargetScale.
  GOBitmap::ClearCache();

  // Only snapshot if m_ResultBitmaps are actually at m_Scale.  After
  // OnAsyncPrepareDone accepts an intermediate result it always sets
  // m_Scale = m_AsyncRenderScale before calling us, so the invariant holds
  // in the normal path.  The edge case where a new async starts before the
  // previous event was processed (m_ResultBitmapScale != m_Scale) is handled
  // by skipping snapshots entirely — OnGOControl will not draw during that
  // brief window, avoiding a size mismatch.
  if (
    std::abs(
      m_ResultBitmapScale.load(std::memory_order_acquire) - m_Scale) < 1e-6) {
    m_panel->TakeSnapshotBitmaps();
    m_SnapshotsTaken = true;
  } else {
    m_SnapshotsTaken = false;
  }

  // Render at the new TARGET scale, not the old render scale.
  const double scale = m_TargetScale;
  const bool hasBG = m_BGInit;

  auto renderBody = [this, scale, hasBG, gen, renderT0]() {
    // --- runs on background thread -----------------------------------------
    GOBitmap *bg = hasBG ? &m_Background : nullptr;

    // Tell GetResultBitmap() on this thread to return the real m_ResultBitmap
    // (not the snapshot). PrepareDraw writes m_ResultBitmap; the subsequent
    // Draw() must read it — not the old-scale snapshot.
    GOBitmap::SetReadFromOriginal(true);

    // Rebuild all per-control bitmaps at the new scale. The GOBitmap cache
    // shared across instances means identical source images are BICUBIC-scaled
    // only once (e.g. 10 sliders sharing digit images → 128 calls, not 1280).
    m_panel->PrepareDraw(scale, bg);
    const auto tPrep = std::chrono::steady_clock::now();

    // Record the scale at which m_ResultBitmaps now live so that the GUI
    // thread can verify the invariant before calling TakeSnapshotBitmaps().
    m_ResultBitmapScale.store(scale, std::memory_order_release);

    // Render the full composite into a private bitmap (no shared state written
    // beyond the per-control GOBitmaps built above, which the GUI thread will
    // not touch while m_PrepareRunning is true).
    const int w = int(m_panel->GetWidth() * scale + 0.5);
    const int h = int(m_panel->GetHeight() * scale + 0.5);
    auto *result = new wxBitmap;
    if (hasBG)
      *result = wxBitmap(m_BGImage.Scale(w, h, wxIMAGE_QUALITY_BICUBIC));
    else
      result->Create(w, h);
    const auto tBg = std::chrono::steady_clock::now();
    {
      wxMemoryDC dc;
      dc.SelectObject(*result);
      GODC DC(&dc, scale, m_FontScale);
      m_panel->Draw(DC);
    }

    /* One line per settle-render. Reading guide:
     *   total     = full freeze (GUI thread in sync mode)
     *   prepare   = PrepareDraw incl. the pre-work before it
     *     pixel     = tile assembly + BICUBIC scaling -> can move to a worker
     *     convert   = wxImage->wxBitmap               -> must stay on the GUI
     *     alpha     = uncacheable composite path      -> must stay on the GUI
     *   bg        = full-panel background BICUBIC + conversion (mixed)
     *   draw      = composite + text layout           -> must stay on the GUI
     * If pixel dominates, pre-scaling on a worker removes most of the
     * freeze; convert+alpha+draw is the floor that remains. */
    {
      const auto tDraw = std::chrono::steady_clock::now();
      const GOBitmapRenderStats st = GOBitmap::GetRenderStats();
      const auto ms = [](std::chrono::steady_clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
      };
      fprintf(
        stderr,
        "[panel-render] '%s' scale %.2f: total %.0f ms | prepare %.0f ms "
        "(pixel %.1f, convert %.1f, alpha %.1f; %u scaled, %u hits, %u alpha) "
        "| bg %.0f ms | draw %.0f ms\n",
        (const char *)m_panel->GetName().utf8_str(),
        scale,
        ms(tDraw - renderT0),
        ms(tPrep - renderT0),
        st.usPixel / 1000.0,
        st.usConvert / 1000.0,
        st.usAlpha / 1000.0,
        st.nScaled,
        st.nCacheHit,
        st.nAlpha,
        ms(tBg - tPrep),
        ms(tDraw - tBg));
    }

    // Signal done BEFORE posting so the GUI thread never sees Running=false
    // without a corresponding event already in the queue.
    m_PrepareRunning.store(false, std::memory_order_release);

    if (!m_ShuttingDown.load(std::memory_order_acquire)) {
      auto *evt = new wxCommandEvent(wxEVT_GOPREPARE_DONE);
      evt->SetClientData(result);
      evt->SetInt(static_cast<int>(gen));
      wxQueueEvent(this, evt);
    } else {
      delete result;
    }
    // -----------------------------------------------------------------------
  };

  if (IsSyncRender())
    renderBody(); // same work, no second thread - see IsSyncRender()
  else
    m_PrepareThread = std::thread([renderBody]() {
      /* One panel renders at a time, process-wide.
       *
       * Each open panel gets its own widget and therefore its own render
       * thread, and they all go through the shared scaled-bitmap cache in
       * GOBitmap. That cache's map is mutex-protected, but the wxImage it
       * hands out is reference-counted with copy-on-write - and wxWidgets
       * does NOT count those references atomically. The copies outlive the
       * cache lock, so two panel threads adjust the same plain int at the
       * same time: a lost decrement frees an image buffer while it is still
       * in use.
       *
       * Necessary but NOT sufficient: it excludes the render threads from
       * each other, not from the GUI thread, which shares wx's stock GDI
       * objects and GTK's drawing stack with them. See the switch above -
       * that is why this path is opt-in rather than the default. */
      static std::mutex s_renderMutex;
      std::lock_guard<std::mutex> lock(s_renderMutex);

      renderBody();
    });
}

void GOGUIPanelWidget::OnAsyncPrepareDone(wxCommandEvent &event) {
  auto *bmp = static_cast<wxBitmap *>(event.GetClientData());

  // Discard results from renders superseded by a new StartAsyncPrepareDraw.
  if (static_cast<unsigned>(event.GetInt()) != m_AsyncGeneration) {
    delete bmp;
    return;
  }

  // Always accept the render result: m_ResultBitmaps are now at
  // m_AsyncRenderScale. Update m_Scale first so the invariant
  //   "snapshots are at m_Scale" == "m_ResultBitmapScale == m_Scale"
  // holds when StartAsyncPrepareDraw() calls TakeSnapshotBitmaps() below.
  m_panel->ClearSnapshotBitmaps();
  m_SnapshotsTaken = false;
  m_Scale = m_AsyncRenderScale;

  if (std::abs(m_AsyncRenderScale - m_TargetScale) < 1e-6) {
    // Perfect match — install as the final sharp result.
    if (bmp) {
      m_ClientBitmap = std::move(*bmp);
      m_LastFullBitmap = m_ClientBitmap;
      SetSize(m_ClientBitmap.GetWidth(), m_ClientBitmap.GetHeight());
    }
    delete bmp;
    Refresh();
    Update();
    if (m_PendingAsyncRedraw)
      StartAsyncPrepareDraw();
  } else {
    // m_TargetScale changed while the render was in flight. Accept the
    // intermediate as the new reference: m_LastFullBitmap becomes the sharp
    // intermediate at m_AsyncRenderScale, giving the next bilinear preview a
    // better source than the old-scale bitmap would have.
    if (bmp) {
      m_LastFullBitmap = std::move(*bmp);
      const int w = int(m_panel->GetWidth() * m_TargetScale + 0.5);
      const int h = int(m_panel->GetHeight() * m_TargetScale + 0.5);
      if (w > 0 && h > 0)
        m_ClientBitmap = (wxBitmap)m_LastFullBitmap.ConvertToImage().Scale(
          w, h, wxIMAGE_QUALITY_NORMAL);
      SetSize(w, h);
    }
    delete bmp;
    Refresh();
    // Re-render at the new target scale. m_Scale == m_ResultBitmapScale now,
    // so TakeSnapshotBitmaps() will be called with consistent bitmaps.
    StartAsyncPrepareDraw();
  }
}

void GOGUIPanelWidget::OnDraw(wxDC *dc) {
  dc->DrawBitmap(m_ClientBitmap, 0, 0, false);
}

void GOGUIPanelWidget::OnErase(wxEraseEvent &event) {
  wxDC *dc = event.GetDC();
  OnDraw(dc);
}

void GOGUIPanelWidget::OnPaint(wxPaintEvent &event) {
  wxPaintDC dc(this);
  OnDraw(&dc);
}

void GOGUIPanelWidget::OnUpdate() {
  if (m_BGInit)
    m_ClientBitmap = (wxBitmap)m_BGImage.Scale(
      m_panel->GetWidth() * m_Scale + 0.5,
      m_panel->GetHeight() * m_Scale + 0.5,
      wxIMAGE_QUALITY_BICUBIC);
  else
    m_ClientBitmap = wxBitmap(
      m_panel->GetWidth() * m_Scale + 0.5,
      m_panel->GetHeight() * m_Scale + 0.5);
  {
    wxMemoryDC dc;
    dc.SelectObject(m_ClientBitmap);
    GODC DC(&dc, m_Scale, m_FontScale);
    m_panel->Draw(DC);
  }
  SetSize(m_ClientBitmap.GetWidth(), m_ClientBitmap.GetHeight());
  // Keep m_LastFullBitmap in sync: it is the authoritative source for
  // the cheap bilinear preview in UpdateSize() and the BICUBIC in
  // OnResizeTimer(). The DC must be released before copying.
  m_LastFullBitmap = m_ClientBitmap;
}

void GOGUIPanelWidget::OnGOControl(wxCommandEvent &event) {
  GOGUIControl *control = static_cast<GOGUIControl *>(event.GetClientData());

  if (m_Scale != m_TargetScale) {
    // Background thread is rebuilding m_ResultBitmaps at the new scale.
    // Only draw when snapshots are valid (m_SnapshotsTaken=true), meaning
    // TakeSnapshotBitmaps() was called while bitmaps were at m_Scale.
    // In the rare edge case where a new async started before the previous
    // event was processed, m_SnapshotsTaken is false and we skip the draw
    // rather than risk a size mismatch.
    if (m_SnapshotsTaken) {
      {
        wxMemoryDC mdc;
        mdc.SelectObject(m_LastFullBitmap);
        GODC DC(&mdc, m_Scale, m_FontScale);
        control->Draw(DC);
      }
      const int targetW = int(m_panel->GetWidth() * m_TargetScale + 0.5);
      const int targetH = int(m_panel->GetHeight() * m_TargetScale + 0.5);
      if (targetW > 0 && targetH > 0)
        m_ClientBitmap = (wxBitmap)m_LastFullBitmap.ConvertToImage().Scale(
          targetW, targetH, wxIMAGE_QUALITY_NORMAL);
      Refresh();
    }
    event.Skip();
    return;
  }

  // Normal case: render scale == display scale.
  {
    wxMemoryDC mdc;
    mdc.SelectObject(m_ClientBitmap);
    GODC DC(&mdc, m_Scale, m_FontScale);
    control->Draw(DC);
    RefreshRect(DC.ScaleRect(control->GetBoundingRect()), false);
  }
  // Capture the updated state so the next resize has the current control
  // states as its source (DC must be released before copying the bitmap).
  m_LastFullBitmap = m_ClientBitmap;
  event.Skip();
}

bool GOGUIPanelWidget::ForwardMouseEvent(wxMouseEvent &event) {
  if (GetClientRect().Contains(event.GetPosition()))
    return false;
  wxPoint pos = ClientToScreen(event.GetPosition());
  wxWindow *window = wxFindWindowAtPoint(pos);
  if (window) {
    wxMouseEvent e = event;
    e.SetPosition(window->ScreenToClient(pos));
    window->HandleWindowEvent(e);
  }
  return true;
}

void GOGUIPanelWidget::HandleMousePress(const wxPoint &point, bool isRight) {
  if (!isRight)
    m_PressedPoint = point;
  double displayScale = m_TargetScale > 0.0 ? m_TargetScale : m_Scale;
  m_panel->HandleMousePress(
    point.x / displayScale, point.y / displayScale, isRight);
}

void GOGUIPanelWidget::HandleMouseRelease(bool isRight) {
  m_panel->HandleMouseRelease(isRight);
  if (!isRight)
    m_PressedPoint = default_point;
}

void GOGUIPanelWidget::OnMouseMove(wxMouseEvent &event) {
  if (!event.LeftIsDown()) {
    HandleMouseRelease(false);
    return;
  }
  if (ForwardMouseEvent(event))
    return;

  wxPoint point = event.GetPosition();

  // some windows managers call several times at the same position. Skip if so
  if (point != m_PressedPoint)
    HandleMousePress(point, false);
  event.Skip();
}

void GOGUIPanelWidget::OnMouseLeftDown(wxMouseEvent &event) {
  Focus();
  if (ForwardMouseEvent(event))
    return;

  wxPoint point = event.GetPosition();

  // some windows managers call several times at the same position. Skip if so
  if (point != m_PressedPoint) {
    HandleMouseRelease(false);
    HandleMousePress(point, false);
  }
  event.Skip();
}

void GOGUIPanelWidget::OnMouseLeftDclick(wxMouseEvent &event) {
  if (!ForwardMouseEvent(event)) {
    HandleMouseRelease(false);
    HandleMousePress(event.GetPosition(), false);
    event.Skip();
  }
}

void GOGUIPanelWidget::OnMouseRightDown(wxMouseEvent &event) {
  Focus();

  HandleMousePress(event.GetPosition(), true);
  event.Skip();
}

void GOGUIPanelWidget::OnMouseLeftUp(wxMouseEvent &event) {
  if (m_PressedPoint.IsFullySpecified() && !event.LeftIsDown()) {
    HandleMouseRelease(false);
    event.Skip();
  }
}

void GOGUIPanelWidget::OnMouseScroll(wxMouseEvent &event) {
  if (event.GetWheelRotation()) {
    double displayScale = m_TargetScale > 0.0 ? m_TargetScale : m_Scale;
    m_panel->HandleMouseScroll(
      event.GetX() / displayScale,
      event.GetY() / displayScale,
      event.GetWheelRotation());
  }
  event.Skip();
}

void GOGUIPanelWidget::OnKeyCommand(wxKeyEvent &event) {
  int k = event.GetKeyCode();
  if (!event.AltDown()) {
    if (!event.ShiftDown())
      m_panel->HandleKey(259); /* Disable setter */
    if (event.ShiftDown() || event.GetKeyCode() == WXK_SHIFT)
      m_panel->HandleKey(260); /* Enable setter */

    k = GOKeyConvert::wXKtoVK(k);
    if (k)
      m_panel->HandleKey(k);
  }
  event.ResumePropagation(wxEVENT_PROPAGATE_MAX);
  event.Skip();
}

void GOGUIPanelWidget::OnKeyUp(wxKeyEvent &event) {
  int k = event.GetKeyCode();
  if (k == WXK_SHIFT)
    m_panel->HandleKey(259); /* Disable setter */
  event.ResumePropagation(wxEVENT_PROPAGATE_MAX);
  event.Skip();
}
