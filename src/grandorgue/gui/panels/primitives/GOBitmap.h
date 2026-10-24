/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOBITMAP_H
#define GOBITMAP_H

#include <cstdint>

#include <wx/bitmap.h>
#include <wx/image.h>

class wxRect;

/**
 * Where a full panel render spends its time, split along the line that
 * matters for taking work off the GUI thread: pure pixel arithmetic
 * (thread-safe on privately owned images) versus GDI-bound work (wxBitmap
 * conversion, DC compositing - GUI thread only). Collected per render via
 * ResetRenderStats()/GetRenderStats(); microseconds.
 */
struct GOBitmapRenderStats {
  uint64_t usPixel = 0;   // tile assembly + BICUBIC scaling (pure pixel math)
  uint64_t usConvert = 0; // wxImage -> wxBitmap conversions (GDI)
  uint64_t usAlpha = 0;   // uncacheable alpha-composite path (DC work)
  unsigned nScaled = 0;   // BICUBIC scales actually computed
  unsigned nConvert = 0;  // wxBitmap conversions
  unsigned nAlpha = 0;    // alpha-composite builds
  unsigned nCacheHit = 0; // scale skipped thanks to the shared cache
};

/**
 * This class is designed for building a result wxBitmap instance from a source
 * wxImage instance. It supports both scaling and tiling
 */

class GOBitmap {
private:
  const wxImage *p_SourceImage = nullptr;
  wxBitmap m_ResultBitmap;
  /**
   * The wxImage produced by the last non-alpha BICUBIC scale. Kept so that
   * BuildScaledBitmap / BuildTileBitmap can populate the shared scale cache
   * without a wxBitmap→wxImage round-trip.
   */
  wxImage m_LastScaledImage;
  /** Snapshot of m_ResultBitmap taken before a background PrepareDraw. */
  wxBitmap m_SnapshotBitmap;
  bool m_HasSnapshot = false;
  /**
   * Thread-local: when true, GetResultBitmap() bypasses snapshots and returns
   * m_ResultBitmap directly. Set on the background render thread so that
   * Draw() (after PrepareDraw) sees the freshly-built new-scale bitmaps.
   */
  static thread_local bool s_readFromOriginal;
  double m_Scale = 0.0;
  int m_ResultWidth = 0;
  int m_ResultHeight = 0;
  unsigned m_ResultXOffset = 0;
  unsigned m_ResultYOffset = 0;
  bool m_ResultValid = false;

  void BuildBitmapFrom(
    const wxImage &img, double scale, const wxRect &rect, GOBitmap *background);

public:
  void SetSourceImage(const wxImage *pSourceImg) { p_SourceImage = pSourceImg; }

  /**
   * The scaled result as a wxImage, valid after any non-alpha build
   * (including cache hits). Used by the alpha-composite path of OTHER
   * GOBitmaps, which needs the background's pixels without a
   * wxBitmap->wxImage round-trip.
   */
  const wxImage &GetLastScaledImage() const { return m_LastScaledImage; }

  unsigned GetSourceWidth() const;
  unsigned GetSourceHeight() const;

  void BuildScaledBitmap(
    double scale, const wxRect &rect, GOBitmap *background);
  void BuildTileBitmap(
    double scale,
    const wxRect &newRect,
    unsigned newXOffset,
    unsigned newYOffset,
    GOBitmap *background);

  /**
   * Returns the bitmap to display. While a background PrepareDraw is running
   * at a new scale, the GUI thread uses TakeSnapshot() / ClearSnapshot() to
   * pin the old-scale bitmaps; the background thread calls
   * SetReadFromOriginal(true) so that its own Draw() sees the freshly-built
   * new-scale bitmaps instead of the snapshot.
   */
  const wxBitmap *GetResultBitmap() const {
    if (m_HasSnapshot && !s_readFromOriginal)
      return &m_SnapshotBitmap;
    return m_ResultValid ? &m_ResultBitmap : nullptr;
  }

  /**
   * Pin the current m_ResultBitmap as an immutable snapshot for the GUI
   * thread. wxBitmap copy is O(1) (shared reference). Called on GUI thread
   * before launching the background PrepareDraw.
   */
  void TakeSnapshot() {
    if (m_ResultValid) {
      m_SnapshotBitmap = m_ResultBitmap;
      m_HasSnapshot = true;
    }
  }

  /** Release the snapshot taken by TakeSnapshot(). */
  void ClearSnapshot() {
    m_HasSnapshot = false;
    m_SnapshotBitmap = wxBitmap();
  }

  /**
   * When true the calling thread bypasses snapshots and reads m_ResultBitmap
   * directly. Set to true on the background render thread so that Draw()
   * (which runs after PrepareDraw()) sees the new-scale bitmaps, not the
   * old-scale snapshot. Never called from the GUI thread.
   */
  static void SetReadFromOriginal(bool v) { s_readFromOriginal = v; }

  /**
   * Clears the shared scaled-bitmap cache. Call before starting a new
   * PrepareDraw at a different scale to release stale entries and reclaim
   * memory. Thread-safe.
   */
  static void ClearCache();

  /**
   * Render timing diagnostics (see GOBitmapRenderStats). Reset before a full
   * panel render, read after it. Not synchronised: only one render collects
   * at a time (GUI thread in sync mode; the mutex-serialised render thread
   * otherwise).
   */
  static void ResetRenderStats();
  static GOBitmapRenderStats GetRenderStats();
};

#endif
