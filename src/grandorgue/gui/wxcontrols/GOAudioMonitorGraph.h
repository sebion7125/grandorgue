/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOAUDIOMONITORGRAPH_H
#define GOAUDIOMONITORGRAPH_H

#include <array>
#include <vector>

#include <wx/bitmap.h>
#include <wx/control.h>

// A small rolling strip-chart with four optionally-hidden series:
//  - Polyphony usage, 0..100%, drawn against the left axis.
//  - Period compute time (WakeupThreads() until the period is actually
//    fully rendered, see GOSoundOrganEngine::NextPeriod()), callback
//    duration, and callback gap, all in microseconds, sharing a right axis
//    scaled to 2x the nominal buffer period (with a dashed reference line at
//    the nominal buffer period - sustained crossing means the budget is gone).
// Drop events (polyphony-limiting forced fades) are shown as short
// magenta tick marks at the bottom of the plot area, transient-detector
// firings as cyan ones just above them.  The latter answer a question the
// numbers alone cannot: on a HELD tone the detector must not fire, so a
// row of cyan ticks under a steady chord is a bug in plain sight.
// Used by GOAudioMonitorDlg, which owns the checkboxes that toggle
// visibility and feeds PushSample() from a GUI timer.
class GOAudioMonitorGraph : public wxControl {
public:
  enum Series {
    SERIES_POLYPHONY = 0,
    SERIES_COMPUTE_US,
    SERIES_CALLBACK_US,
    SERIES_GAP_US,
    // Share of the period's voice work the TIME_CRITICAL audio thread
    // performed itself (percent, left axis) - the live view of the
    // "callback computes along" effect.
    SERIES_CB_SHARE,
    SERIES_COUNT,
  };

private:
  static constexpr unsigned HISTORY_SIZE = 180; // ~1 min at a 333ms poll

  std::array<std::vector<float>, SERIES_COUNT> m_Series;
  // Parallel ring buffer: non-zero entries mark poll intervals where at
  // least one polyphony-limiting drop occurred.
  std::vector<uint32_t> m_DropCounts;
  // Same idea for the concealment's transient detector: how many times it
  // fired during each poll interval.
  std::vector<uint32_t> m_OnsetCounts;
  std::array<bool, SERIES_COUNT> m_Visible;
  unsigned m_NextIndex;
  unsigned m_FilledCount;
  int64_t m_NominalPeriodUs;
  unsigned m_PollIntervalMs;

  // Shared rendering logic used by OnPaint() and RenderToBitmap().
  void DoPaint(wxDC &dc, int w, int h);

  void OnPaint(wxPaintEvent &event);

public:
  GOAudioMonitorGraph(wxWindow *parent, wxWindowID id);

  // polyphonyPct: 0..100. computeUs/callbackUs/gapUs: microseconds, this
  // poll's peak.
  // drops: polyphony-drop events that occurred during this poll interval.
  // onsets: transient-detector firings during this poll interval.
  void PushSample(
    float polyphonyPct,
    float computeUs,
    float callbackUs,
    float gapUs,
    uint32_t drops = 0,
    float cbSharePct = 0.0f,
    uint32_t onsets = 0);

  void SetSeriesVisible(Series s, bool visible);
  // The buffer period (microseconds) at the current sample
  // rate/buffer size, drawn as a dashed reference line on the right axis -
  // render time or callback gap crossing it is an audible glitch.
  void SetNominalPeriodUs(int64_t us) { m_NominalPeriodUs = us; }

  // How often PushSample() is called (GOAudioMonitorDlg's timer interval),
  // used only to label the time axis ("-60s"/"now" etc).
  void SetPollIntervalMs(unsigned ms) { m_PollIntervalMs = ms; }

  // Render the current graph state into a new bitmap of the given size.
  // Useful for embedding a PNG snapshot in a report file.
  wxBitmap RenderToBitmap(int w, int h);

  // Export the raw time series as a CSV string (header + one row per stored
  // sample, oldest first). Suitable for attaching to a bug report.
  wxString GetCSV() const;

  DECLARE_EVENT_TABLE()
};

#endif /* GOAUDIOMONITORGRAPH_H */
