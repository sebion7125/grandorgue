/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOBitmap.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/image.h>

// ---------------------------------------------------------------------------
// Shared scaled-bitmap cache
//
// Keyed by (source image pointer, scaled width, scaled height, xOffset,
// yOffset). Prevents duplicate BICUBIC computation when multiple GOBitmap
// instances share the same source wxImage at the same output dimensions.
//
// Only background-INDEPENDENT results are cacheable. Note that this excludes
// more than one might hope: any source with alpha on a panel with a
// background goes through the composite path (measured on the Arnstadt
// mixer: its 10x128 slider digit states are ALL alpha composites - 1280
// uncacheable builds per render, 1 cache hit). The composite path is cheap
// since it became plain pixel arithmetic, but it is not shared.
//
// wxImage (not wxBitmap) is stored because wxImage is ref-counted and
// platform-independent; the caller converts to wxBitmap on its own thread.
// ---------------------------------------------------------------------------

namespace {

struct BitmapCacheKey {
  const wxImage *source;
  int scaledW;
  int scaledH;
  unsigned xOffset;
  unsigned yOffset;

  bool operator==(const BitmapCacheKey &o) const noexcept {
    return source == o.source && scaledW == o.scaledW && scaledH == o.scaledH
      && xOffset == o.xOffset && yOffset == o.yOffset;
  }
};

struct BitmapCacheKeyHash {
  size_t operator()(const BitmapCacheKey &k) const noexcept {
    size_t h = reinterpret_cast<uintptr_t>(k.source) * 2654435761u;
    h ^= static_cast<size_t>(k.scaledW) * 40503u;
    h ^= static_cast<size_t>(k.scaledH) * 12345u;
    h ^= k.xOffset * 97u + k.yOffset;
    return h;
  }
};

std::unordered_map<BitmapCacheKey, wxImage, BitmapCacheKeyHash> s_cache;
std::mutex s_cacheMutex;

// Render timing (see GOBitmapRenderStats in the header). One collector at a
// time by construction, so plain variables suffice.
GOBitmapRenderStats s_renderStats;

uint64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
           std::chrono::steady_clock::now().time_since_epoch())
    .count();
}

wxImage LookupCache(const BitmapCacheKey &key) {
  std::lock_guard<std::mutex> lock(s_cacheMutex);
  auto it = s_cache.find(key);
  return it != s_cache.end() ? it->second : wxImage();
}

void StoreCache(const BitmapCacheKey &key, const wxImage &img) {
  std::lock_guard<std::mutex> lock(s_cacheMutex);
  s_cache.emplace(key, img);
}

} // namespace

thread_local bool GOBitmap::s_readFromOriginal = false;

void GOBitmap::ClearCache() {
  std::lock_guard<std::mutex> lock(s_cacheMutex);
  s_cache.clear();
}

void GOBitmap::ResetRenderStats() { s_renderStats = GOBitmapRenderStats(); }

GOBitmapRenderStats GOBitmap::GetRenderStats() { return s_renderStats; }

unsigned GOBitmap::GetSourceWidth() const {
  return p_SourceImage ? p_SourceImage->GetWidth() : 0;
}

unsigned GOBitmap::GetSourceHeight() const {
  return p_SourceImage ? p_SourceImage->GetHeight() : 0;
}

void GOBitmap::BuildBitmapFrom(
  const wxImage &img, double scale, const wxRect &rect, GOBitmap *background) {
  const int imgHeight = img.GetHeight();
  const int imgWidth = img.GetWidth();
  const int newHeight = imgHeight * scale;
  const int newWidth = imgWidth * scale;

  m_ResultValid = newHeight > 0 && newWidth > 0;
  if (m_ResultValid) {
    const wxBitmap *pBackgroundBitmap
      = background ? background->GetResultBitmap() : nullptr;

    if (pBackgroundBitmap && img.HasAlpha()) {
      const uint64_t t0 = NowUs();
      /* Composite the (semi-)transparent source over the panel background.
       *
       * This used to run through GDI: build a wxBitmap, DrawBitmap the
       * background and the source into a wxMemoryDC, ConvertToImage back.
       * Under GTK each of those is a cairo surface round-trip, measured at
       * ~4 ms per call - and the Arnstadt mixer makes 1280 such calls per
       * render (10 sliders x 128 digit states), 5.1 of its 5.7 seconds.
       * The same composite as plain pixel arithmetic is microseconds.
       *
       * Semantics kept from the GDI version, including its quirk: the
       * background is the SCALED panel background, sampled at UNSCALED
       * control offsets - only exact at scale 1. Out-of-range samples were
       * undefined before (uninitialized bitmap memory); they now clamp to
       * the background's edge. Images with a mask (instead of alpha) never
       * enter this branch - it requires HasAlpha(). */
      const wxImage &bgImg = background->GetLastScaledImage();

      if (bgImg.IsOk() && !img.HasMask()) {
        const int bgW = bgImg.GetWidth();
        const int bgH = bgImg.GetHeight();
        const unsigned char *bgData = bgImg.GetData();
        const unsigned char *fgData = img.GetData();
        const unsigned char *fgAlpha = img.GetAlpha();
        wxImage comp(imgWidth, imgHeight, false);
        unsigned char *dst = comp.GetData();
        const int offX = rect.GetX();
        const int offY = rect.GetY();

        for (int y = 0; y < imgHeight; y++) {
          int by = y + offY;

          by = by < 0 ? 0 : (by >= bgH ? bgH - 1 : by);

          const unsigned char *bgRow = bgData + (size_t)by * bgW * 3;
          const unsigned char *fgRow = fgData + (size_t)y * imgWidth * 3;
          const unsigned char *aRow = fgAlpha + (size_t)y * imgWidth;
          unsigned char *dRow = dst + (size_t)y * imgWidth * 3;

          for (int x = 0; x < imgWidth; x++) {
            int bx = x + offX;

            bx = bx < 0 ? 0 : (bx >= bgW ? bgW - 1 : bx);

            const unsigned a = aRow[x];
            const unsigned char *b = bgRow + (size_t)bx * 3;
            const unsigned char *f = fgRow + (size_t)x * 3;

            dRow[(size_t)x * 3 + 0] = (f[0] * a + b[0] * (255 - a) + 127) / 255;
            dRow[(size_t)x * 3 + 1] = (f[1] * a + b[1] * (255 - a) + 127) / 255;
            dRow[(size_t)x * 3 + 2] = (f[2] * a + b[2] * (255 - a) + 127) / 255;
          }
        }
        comp.SetAlpha(NULL); // allocate, then take the source's alpha verbatim
        memcpy(comp.GetAlpha(), fgAlpha, (size_t)imgWidth * imgHeight);
        m_LastScaledImage = wxImage(); // context-dependent, not cached
        m_ResultBitmap
          = (wxBitmap)comp.Scale(newWidth, newHeight, wxIMAGE_QUALITY_BICUBIC);
      } else {
        // Fallback: background not available as an image (should not happen -
        // it is built through the non-alpha path) or a mask is in play.
        wxBitmap bmp(imgWidth, imgHeight);
        wxBitmap orig(img);
        wxMemoryDC dc;

        dc.SelectObject(bmp);
        dc.DrawBitmap(*pBackgroundBitmap, -rect.GetX(), -rect.GetY(), false);
        dc.DrawBitmap(orig, 0, 0, true);
        bmp.SetMask(orig.GetMask());

        wxImage img_result = bmp.ConvertToImage();

        if (!img_result.HasAlpha())
          img_result.InitAlpha();
        memcpy(img_result.GetAlpha(), img.GetAlpha(), imgWidth * imgHeight);
        m_LastScaledImage = wxImage(); // context-dependent, not cached
        m_ResultBitmap = (wxBitmap)img_result.Scale(
          newWidth, newHeight, wxIMAGE_QUALITY_BICUBIC);
      }
      s_renderStats.usAlpha += NowUs() - t0;
      s_renderStats.nAlpha++;
    } else {
      const uint64_t t0 = NowUs();
      m_LastScaledImage = img.Scale(newWidth, newHeight, wxIMAGE_QUALITY_BICUBIC);
      const uint64_t t1 = NowUs();
      m_ResultBitmap = wxBitmap(m_LastScaledImage);
      s_renderStats.usPixel += t1 - t0;
      s_renderStats.nScaled++;
      s_renderStats.usConvert += NowUs() - t1;
      s_renderStats.nConvert++;
    }
  }
  m_Scale = scale;
}

void GOBitmap::BuildScaledBitmap(
  double scale, const wxRect &rect, GOBitmap *background) {
  if (p_SourceImage && (scale != m_Scale || m_ResultWidth || m_ResultHeight)) {
    // Cache is valid when the result is background-independent.  A background
    // is only composited in BuildBitmapFrom when the source has alpha; without
    // alpha the background parameter has no effect on the output.
    const bool canCache = !background || !p_SourceImage->HasAlpha();
    if (canCache) {
      const int newW = int(p_SourceImage->GetWidth() * scale);
      const int newH = int(p_SourceImage->GetHeight() * scale);
      if (newW > 0 && newH > 0) {
        BitmapCacheKey key{p_SourceImage, newW, newH, 0, 0};
        wxImage cached = LookupCache(key);
        if (cached.IsOk()) {
          const uint64_t t0 = NowUs();
          m_ResultBitmap = wxBitmap(cached);
          s_renderStats.usConvert += NowUs() - t0;
          s_renderStats.nConvert++;
          s_renderStats.nCacheHit++;
          // Keep the image form current too - the alpha-composite path of
          // dependent bitmaps reads the background via GetLastScaledImage().
          m_LastScaledImage = cached;
          m_ResultValid = true;
          m_Scale = scale;
          m_ResultWidth = 0;
          m_ResultHeight = 0;
          return;
        }
        BuildBitmapFrom(*p_SourceImage, scale, rect, nullptr);
        if (m_ResultValid && m_LastScaledImage.IsOk())
          StoreCache(key, m_LastScaledImage);
        m_ResultWidth = 0;
        m_ResultHeight = 0;
        return;
      }
    }
    BuildBitmapFrom(*p_SourceImage, scale, rect, background);
    m_ResultWidth = 0;
    m_ResultHeight = 0;
  }
}

void GOBitmap::BuildTileBitmap(
  double scale,
  const wxRect &newRect,
  unsigned newXOffset,
  unsigned newYOffset,
  GOBitmap *background) {
  const int tgtHeight = newRect.GetHeight();
  const int tgtWidth = newRect.GetWidth();

  if (tgtWidth <= 0 || tgtHeight <= 0) {
    m_ResultValid = false;
    return;
  }
  /* Note the corrected width/height pairing: this used to compare
   * m_ResultWidth against tgtHeight (and vice versa), so for any non-square
   * target the check never held and every call rebuilt. Harmless while
   * PrepareDraw ran once per scale change; fatal for lazy builds from
   * Draw(), which rely on this being a cheap no-op when already current. */
  if (
    p_SourceImage
    && (scale != m_Scale || m_ResultWidth != tgtWidth || m_ResultHeight != tgtHeight || newXOffset != m_ResultXOffset || newYOffset != m_ResultYOffset)) {
    // Cache when background-independent (same logic as in BuildScaledBitmap).
    const bool canCache = !background || !p_SourceImage->HasAlpha();
    if (canCache) {
      const int scaledW = int(tgtWidth * scale);
      const int scaledH = int(tgtHeight * scale);
      if (scaledW > 0 && scaledH > 0) {
        BitmapCacheKey key{p_SourceImage, scaledW, scaledH, newXOffset, newYOffset};
        wxImage cached = LookupCache(key);
        if (cached.IsOk()) {
          const uint64_t t0 = NowUs();
          m_ResultBitmap = wxBitmap(cached);
          s_renderStats.usConvert += NowUs() - t0;
          s_renderStats.nConvert++;
          s_renderStats.nCacheHit++;
          m_ResultValid = true;
          m_Scale = scale;
          m_ResultHeight = tgtHeight;
          m_ResultWidth = tgtWidth;
          m_ResultXOffset = newXOffset;
          m_ResultYOffset = newYOffset;
          return;
        }
      }
    }

    const int srcHeight = p_SourceImage->GetHeight();
    const int srcWidth = p_SourceImage->GetWidth();
    if (srcWidth <= 0 || srcHeight <= 0) {
      m_ResultValid = false;
      return;
    }
    const uint64_t tAssembly = NowUs();
    wxImage img(tgtWidth, tgtHeight);

    for (int y = -newYOffset; y < tgtHeight; y += srcHeight)
      for (int x = -newXOffset; x < tgtWidth; x += srcWidth) {
        // Calculate source starting position using std::max(0, -offset)
        const int srcX = std::max(0, -x);
        const int srcY = std::max(0, -y);

        // Ensure copy stays within target bounds using std::min
        const int copyWidth = std::min(srcWidth - srcX, tgtWidth - x);
        const int copyHeight = std::min(srcHeight - srcY, tgtHeight - y);

        // Paste only if valid copy region exists
        if (copyWidth > 0 && copyHeight > 0) {
          if (copyWidth != (int)srcWidth || copyHeight != (int)srcHeight) {
            // Partial copy - use GetSubImage
            wxImage tile = p_SourceImage->GetSubImage(
              wxRect(srcX, srcY, copyWidth, copyHeight));
            img.Paste(tile, x, y);
          } else {
            // Full tile copy - use direct Paste
            img.Paste(*p_SourceImage, x, y);
          }
        }
      }
    s_renderStats.usPixel += NowUs() - tAssembly; // tile assembly: pure pixels
    BuildBitmapFrom(img, scale, newRect, background);

    if (canCache && m_ResultValid && m_LastScaledImage.IsOk()) {
      const int scaledW = int(tgtWidth * scale);
      const int scaledH = int(tgtHeight * scale);
      BitmapCacheKey key{p_SourceImage, scaledW, scaledH, newXOffset, newYOffset};
      StoreCache(key, m_LastScaledImage);
    }

    m_ResultHeight = tgtHeight;
    m_ResultWidth = tgtWidth;
    m_ResultXOffset = newXOffset;
    m_ResultYOffset = newYOffset;
  }
}
