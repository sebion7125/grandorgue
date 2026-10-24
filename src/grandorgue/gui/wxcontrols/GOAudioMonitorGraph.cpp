/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOAudioMonitorGraph.h"

#include <algorithm>

#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/intl.h>
#include <wx/pen.h>

namespace {
// Dark theme: brighter/more saturated than the old light-background palette
// so the series still pop against a dark plot.
const wxColour SERIES_COLOUR[GOAudioMonitorGraph::SERIES_COUNT] = {
  wxColour(90, 160, 255), // polyphony - blue
  wxColour(255, 100, 90), // period compute time - red
  wxColour(120, 230, 160), // callback duration - green
  wxColour(255, 190, 60), // callback gap - amber
  wxColour(220, 120, 255), // callback voice-work share - violet
};
const wxString SERIES_LABEL[GOAudioMonitorGraph::SERIES_COUNT] = {
  wxT("Polyphony (%)"),
  wxT("Compute time"),
  wxT("Callback time"),
  wxT("Callback gap"),
  wxT("CB work (%)"),
};
// Drop-event tick colour: bright magenta so it stands out from all series.
const wxColour DROP_COLOUR(255, 60, 210);
// Transientenzuendung - kuehl gegen das warme Drop-Magenta, damit die
// beiden Markenreihen auch bei dichter Belegung auseinanderzuhalten sind.
const wxColour ONSET_COLOUR(80, 220, 235);

const wxColour PLOT_BACKGROUND(24, 24, 28);
const wxColour GRIDLINE_COLOUR(60, 60, 66);
const wxColour DEADLINE_COLOUR(150, 150, 158);
const wxColour AXIS_TEXT_COLOUR(220, 220, 225);
const wxColour TIME_AXIS_TEXT_COLOUR(160, 160, 168);

// "823" -> "823 us", "5805" -> "5.8 ms"
wxString FormatUs(float us) {
  if (us >= 1000.0f)
    return wxString::Format(wxT("%.1f ms"), us / 1000.0f);
  return wxString::Format(wxT("%.0f us"), us);
}

constexpr int LEFT_MARGIN = 42;
constexpr int RIGHT_MARGIN = 62;
constexpr int TOP_MARGIN = 6;
constexpr int XAXIS_LABEL_HEIGHT = 16;
constexpr int LEGEND_HEIGHT = 18;
constexpr int BOTTOM_MARGIN = XAXIS_LABEL_HEIGHT + LEGEND_HEIGHT + 6;
} // namespace

BEGIN_EVENT_TABLE(GOAudioMonitorGraph, wxControl)
EVT_PAINT(GOAudioMonitorGraph::OnPaint)
END_EVENT_TABLE()

GOAudioMonitorGraph::GOAudioMonitorGraph(wxWindow *parent, wxWindowID id)
  : wxControl(parent, id, wxDefaultPosition, wxSize(460, 200), wxBORDER_THEME),
    m_NextIndex(0),
    m_FilledCount(0),
    m_NominalPeriodUs(0),
    m_PollIntervalMs(333) {
  for (unsigned s = 0; s < SERIES_COUNT; ++s) {
    m_Series[s].assign(HISTORY_SIZE, 0.0f);
    m_Visible[s] = true;
  }
  m_DropCounts.assign(HISTORY_SIZE, 0);
  m_OnsetCounts.assign(HISTORY_SIZE, 0);
  SetBackgroundStyle(wxBG_STYLE_PAINT);
}

void GOAudioMonitorGraph::PushSample(
  float polyphonyPct,
  float computeUs,
  float callbackUs,
  float gapUs,
  uint32_t drops,
  float cbSharePct,
  uint32_t onsets) {
  m_Series[SERIES_POLYPHONY][m_NextIndex] = polyphonyPct;
  m_Series[SERIES_CB_SHARE][m_NextIndex] = cbSharePct;
  m_Series[SERIES_COMPUTE_US][m_NextIndex] = computeUs;
  m_Series[SERIES_CALLBACK_US][m_NextIndex] = callbackUs;
  m_Series[SERIES_GAP_US][m_NextIndex] = gapUs;
  m_DropCounts[m_NextIndex] = drops;
  m_OnsetCounts[m_NextIndex] = onsets;
  m_NextIndex = (m_NextIndex + 1) % HISTORY_SIZE;
  if (m_FilledCount < HISTORY_SIZE)
    ++m_FilledCount;
  Refresh(false);
}

void GOAudioMonitorGraph::SetSeriesVisible(Series s, bool visible) {
  if (s < SERIES_COUNT && m_Visible[s] != visible) {
    m_Visible[s] = visible;
    Refresh(false);
  }
}

void GOAudioMonitorGraph::DoPaint(wxDC &dc, int w, int h) {
  dc.SetBackground(wxBrush(PLOT_BACKGROUND));
  dc.Clear();
  dc.SetFont(GetFont());

  const bool haveRightAxis
    = m_Visible[SERIES_COMPUTE_US] || m_Visible[SERIES_CALLBACK_US]
    || m_Visible[SERIES_GAP_US];
  const int plotLeft = LEFT_MARGIN;
  const int plotRight = w - (haveRightAxis ? RIGHT_MARGIN : 8);
  const int plotTop = TOP_MARGIN;
  const int plotBottom = h - BOTTOM_MARGIN;
  const int plotW = plotRight - plotLeft;
  const int plotH = plotBottom - plotTop;

  if (plotW < 2 || plotH < 2)
    return;

  // Fixed (not auto-scaled) so the two axes stay directly comparable: a
  // trace reaching the top of the plot always means the same thing, be it
  // 100% polyphony or compute time at 2x the nominal buffer period.
  float rightMaxUs = 2.0f * (float)m_NominalPeriodUs;

  if (rightMaxUs <= 0.0f)
    rightMaxUs = 1.0f;

  auto yOf = [&](float v, float axisMax) {
    return plotBottom
      - (int)(std::min(std::max(v, 0.0f) / axisMax, 1.0f) * plotH);
  };

  // Left-axis (0/50/100%) gridlines + labels.
  dc.SetTextForeground(AXIS_TEXT_COLOUR);
  for (int p = 0; p <= 100; p += 50) {
    const int y = yOf((float)p, 100.0f);

    dc.SetPen(wxPen(GRIDLINE_COLOUR));
    dc.DrawLine(plotLeft, y, plotRight, y);

    const wxString label = wxString::Format(wxT("%d%%"), p);
    const wxSize ext = dc.GetTextExtent(label);
    dc.DrawText(label, plotLeft - ext.GetWidth() - 4, y - ext.GetHeight() / 2);
  }

  if (haveRightAxis) {
    for (float v : {0.0f, rightMaxUs}) {
      const int y = yOf(v, rightMaxUs);
      const wxString label = FormatUs(v);
      dc.DrawText(
        label, plotRight + 6, y - dc.GetTextExtent(label).GetHeight() / 2);
    }

    if (m_NominalPeriodUs > 0) {
      const int yDeadline = yOf((float)m_NominalPeriodUs, rightMaxUs);

      dc.SetPen(wxPen(DEADLINE_COLOUR, 1, wxPENSTYLE_SHORT_DASH));
      dc.DrawLine(plotLeft, yDeadline, plotRight, yDeadline);
      const wxString label
        = FormatUs((float)m_NominalPeriodUs) + wxT(" deadline");
      dc.DrawText(
        label,
        plotRight + 6,
        yDeadline - dc.GetTextExtent(label).GetHeight() / 2);
    }
  }

  if (m_FilledCount > 0) {
    const unsigned denom = std::max<unsigned>(1, m_FilledCount - 1);

    auto drawSeries = [&](Series s, float axisMax) {
      if (!m_Visible[s])
        return;
      dc.SetPen(wxPen(SERIES_COLOUR[s], 2));
      wxPoint prev;
      bool havePrev = false;

      for (unsigned i = 0; i < m_FilledCount; ++i) {
        const unsigned idx
          = (m_NextIndex + HISTORY_SIZE - m_FilledCount + i) % HISTORY_SIZE;
        const int x = plotLeft + (int)((float)i / (float)denom * plotW);
        const int y = yOf(m_Series[s][idx], axisMax);
        const wxPoint cur(x, y);

        if (havePrev)
          dc.DrawLine(prev, cur);
        prev = cur;
        havePrev = true;
      }
    };

    drawSeries(SERIES_POLYPHONY, 100.0f);
    drawSeries(SERIES_CB_SHARE, 100.0f); // percent, left axis
    drawSeries(SERIES_COMPUTE_US, rightMaxUs);
    drawSeries(SERIES_CALLBACK_US, rightMaxUs);
    drawSeries(SERIES_GAP_US, rightMaxUs);

    // Drop-event tick marks: short magenta vertical lines at the bottom of
    // the plot area. Rendered on top of the series lines so they're always
    // visible regardless of what else is happening at that x position.
    dc.SetPen(wxPen(DROP_COLOUR, 2));
    for (unsigned i = 0; i < m_FilledCount; ++i) {
      const unsigned idx
        = (m_NextIndex + HISTORY_SIZE - m_FilledCount + i) % HISTORY_SIZE;
      if (m_DropCounts[idx] == 0)
        continue;
      const int x = plotLeft + (int)((float)i / (float)denom * plotW);
      dc.DrawLine(x, plotBottom, x, plotBottom - 14);
    }

    // Transientenzuendungen: dieselbe Machart, eine Etage hoeher.  Auf
    // gehaltenem Klang gehoert hier NICHTS hin - eine Reihe Striche unter
    // einem stehenden Akkord ist ein sichtbarer Fehler, keine Statistik,
    // die man erst interpretieren muss.
    dc.SetPen(wxPen(ONSET_COLOUR, 2));
    for (unsigned i = 0; i < m_FilledCount; ++i) {
      const unsigned idx
        = (m_NextIndex + HISTORY_SIZE - m_FilledCount + i) % HISTORY_SIZE;
      if (m_OnsetCounts[idx] == 0)
        continue;
      const int x = plotLeft + (int)((float)i / (float)denom * plotW);
      dc.DrawLine(x, plotBottom - 16, x, plotBottom - 30);
    }
  }

  // Time axis: three labels (oldest, midpoint, "now").
  if (m_FilledCount > 1) {
    const float spanSeconds
      = (float)(m_FilledCount - 1) * (float)m_PollIntervalMs / 1000.0f;
    const int yLabel = plotBottom + 2;

    dc.SetTextForeground(TIME_AXIS_TEXT_COLOUR);
    for (float frac : {0.0f, 0.5f, 1.0f}) {
      const wxString label = frac >= 1.0f
        ? wxString(_("now"))
        : wxString::Format(wxT("-%.0fs"), spanSeconds * (1.0f - frac));
      const wxSize ext = dc.GetTextExtent(label);
      int x = plotLeft + (int)(frac * plotW) - ext.GetWidth() / 2;

      x = std::max(0, std::min(x, w - ext.GetWidth()));
      dc.DrawText(label, x, yLabel);
    }
  }

  // Legend: coloured line + series name for each visible series, plus the
  // drop-event tick symbol if any drop is visible in the current window.
  int legendX = plotLeft;
  const int legendY
    = plotBottom + (BOTTOM_MARGIN - LEGEND_HEIGHT) + LEGEND_HEIGHT / 2;

  for (unsigned s = 0; s < SERIES_COUNT; ++s) {
    if (!m_Visible[s])
      continue;
    dc.SetPen(wxPen(SERIES_COLOUR[s], 3));
    dc.DrawLine(legendX, legendY, legendX + 18, legendY);
    legendX += 24;

    dc.SetTextForeground(AXIS_TEXT_COLOUR);
    dc.DrawText(
      SERIES_LABEL[s],
      legendX,
      legendY - dc.GetTextExtent(SERIES_LABEL[s]).GetHeight() / 2);
    legendX += dc.GetTextExtent(SERIES_LABEL[s]).GetWidth() + 18;
  }

  // Drop legend entry - only shown when at least one drop tick is visible.
  bool haveDrops = false;
  for (unsigned i = 0; i < m_FilledCount && !haveDrops; ++i)
    if (m_DropCounts
          [(m_NextIndex + HISTORY_SIZE - m_FilledCount + i) % HISTORY_SIZE])
      haveDrops = true;

  if (haveDrops) {
    dc.SetPen(wxPen(DROP_COLOUR, 2));
    dc.DrawLine(legendX, legendY - 5, legendX, legendY + 5);
    legendX += 10;
    dc.SetTextForeground(AXIS_TEXT_COLOUR);
    const wxString dropLabel = wxT("Drop");
    dc.DrawText(
      dropLabel,
      legendX,
      legendY - dc.GetTextExtent(dropLabel).GetHeight() / 2);
    legendX += dc.GetTextExtent(dropLabel).GetWidth() + 18;
  }

  bool haveOnsets = false;
  for (unsigned i = 0; i < m_FilledCount && !haveOnsets; ++i)
    if (m_OnsetCounts
          [(m_NextIndex + HISTORY_SIZE - m_FilledCount + i) % HISTORY_SIZE])
      haveOnsets = true;

  if (haveOnsets) {
    dc.SetPen(wxPen(ONSET_COLOUR, 2));
    dc.DrawLine(legendX, legendY - 5, legendX, legendY + 5);
    legendX += 10;
    dc.SetTextForeground(AXIS_TEXT_COLOUR);
    const wxString onsetLabel = wxT("Transient");
    dc.DrawText(
      onsetLabel,
      legendX,
      legendY - dc.GetTextExtent(onsetLabel).GetHeight() / 2);
  }
}

void GOAudioMonitorGraph::OnPaint(wxPaintEvent &WXUNUSED(event)) {
  wxAutoBufferedPaintDC dc(this);
  const wxSize size = GetClientSize();
  DoPaint(dc, size.GetWidth(), size.GetHeight());
}

wxBitmap GOAudioMonitorGraph::RenderToBitmap(int w, int h) {
  wxBitmap bmp(w, h, 24);
  wxMemoryDC mdc(bmp);
  DoPaint(mdc, w, h);
  return bmp;
}

wxString GOAudioMonitorGraph::GetCSV() const {
  wxString out;
  out += wxT("timestamp_ms,polyphony_pct,compute_us,callback_us,gap_us,drops,"
             "cb_share_pct,transients\n");
  for (unsigned i = 0; i < m_FilledCount; ++i) {
    const unsigned idx
      = (m_NextIndex + HISTORY_SIZE - m_FilledCount + i) % HISTORY_SIZE;
    const int64_t t = (int64_t)i * m_PollIntervalMs;

    /* FromCDouble, not Format("%.2f"): under a comma-decimal locale (German
     * Windows) wxString::Format emits "0,75", which collides with the CSV
     * field separator and shifts every following column. */
    out += wxString::Format(
      wxT("%lld,%s,%s,%s,%s,%u,%s,%u\n"),
      (long long)t,
      wxString::FromCDouble((double)m_Series[SERIES_POLYPHONY][idx], 2),
      wxString::FromCDouble((double)m_Series[SERIES_COMPUTE_US][idx], 0),
      wxString::FromCDouble((double)m_Series[SERIES_CALLBACK_US][idx], 0),
      wxString::FromCDouble((double)m_Series[SERIES_GAP_US][idx], 0),
      m_DropCounts[idx],
      wxString::FromCDouble((double)m_Series[SERIES_CB_SHARE][idx], 2),
      m_OnsetCounts[idx]);
  }
  return out;
}
