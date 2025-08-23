/*
 * GrandOrgue - a free pipe organ simulator
 *
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2024 GrandOrgue contributors (see AUTHORS)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "GOProgressDialog.h"
#include "gui/wxcontrols/go_gui_utils.h"

#include <wx/progdlg.h>
#include <wx/stopwatch.h>
#include <wx/log.h>

#define DLG_MAX_VALUE 0x10000

GOProgressDialog::GOProgressDialog()
  : m_dlg(NULL), m_last(0), m_const(0), m_value(0), m_max(0) {}

GOProgressDialog::~GOProgressDialog() {
  if (m_dlg)
    m_dlg->Destroy();
  
}

void GOProgressDialog::Setup(
  long max, const wxString &title, const wxString &msg) {
  if (m_dlg)
    m_dlg->Destroy();
  m_dlg = new wxProgressDialog(
    title,
    msg,
    DLG_MAX_VALUE,
    NULL,
    wxPD_CAN_ABORT | wxPD_APP_MODAL | wxPD_ELAPSED_TIME | wxPD_ESTIMATED_TIME
      | wxPD_REMAINING_TIME);
  m_dlg->SetIcon(get_go_icon());
  m_last = 0;
  m_const = 0;
  m_value = 0;
  m_max = 0;
  // percent-range defaults (disabled)
  m_usePercentRange = false;
  m_rangeStartPct = 0;
  m_rangeEndPct = 100;
  m_segmentMaxUnits = 0;
  // last reported native dialog value (monotonic guard)
  m_lastReportedValue = 0;
  if (!max)
    max = 1;
  Reset(max, msg);
}

void GOProgressDialog::Reset(long max, const wxString &msg) {
  // Switch back to cumulative mode.
  // To avoid visible regressions when moving from a percent-range segment
  // into cumulative segments, align m_const so the cumulative mapping
  // starts at the currently displayed percentage.
  m_usePercentRange = false;

  // compute current displayed percent from lastReportedValue
  double currentPct = 0.0;
  if (m_lastReportedValue > 0)
    currentPct = (double)m_lastReportedValue * 100.0 / double(DLG_MAX_VALUE - 1);

  // new total max after adding this segment
  long newMax = m_max + (max ? max : 1);

  // set m_const so that (m_const / newMax) ~= currentPct/100
  m_const = (long)std::llround(currentPct * (double)newMax / 100.0);

  // adopt new max and reset value for the upcoming segment
  m_max = newMax;
  m_value = 0;

  // update dialog without moving backwards
  m_last--;
  Update(0, msg);
  m_last--;
}

void GOProgressDialog::ResetRange(long max, int start_pct, int end_pct, const wxString &msg) {
  // Enable percent-range mode for the next segment.
  if (start_pct < 0) start_pct = 0;
  if (end_pct > 100) end_pct = 100;
  if (end_pct <= start_pct) end_pct = std::min(100, start_pct + 1);

  // Prevent new segment from starting below the currently displayed percent
  // to avoid visible backward jumps.
  int currentDisplayedPct = 0;
  if (m_lastReportedValue > 0)
    currentDisplayedPct = (int)((double)m_lastReportedValue * 100.0 / double(DLG_MAX_VALUE - 1));
  if (start_pct < currentDisplayedPct)
    start_pct = currentDisplayedPct;

  // Ensure end_pct is still valid after adjusting start_pct above
  if (end_pct <= start_pct)
    end_pct = std::min(100, start_pct + 1);

  m_usePercentRange = true;
  m_rangeStartPct = start_pct;
  m_rangeEndPct = end_pct;
  m_segmentMaxUnits = max ? max : 1;
  m_value = 0;
  // Log of segment parameters suppressed to reduce noisy logs.
  m_last--;
  Update(0, msg);
  m_last--;
}

bool GOProgressDialog::Update(unsigned value, const wxString &msg) {
  if (!m_dlg)
    return true;
  m_value = value;
  if (m_last == wxGetUTCTime())
    return true;
  m_last = wxGetUTCTime();

  int newValue = 0;
  if (m_usePercentRange) {
    // Map value in [0..m_segmentMaxUnits] to percent range [m_rangeStartPct..m_rangeEndPct]
    double frac = 0.0;
    if (m_segmentMaxUnits > 0)
      frac = (double)std::min<unsigned>(m_value, (unsigned)m_segmentMaxUnits) / (double)m_segmentMaxUnits;
    double pct = m_rangeStartPct + frac * (m_rangeEndPct - m_rangeStartPct);
    // Clamp pct defensively to [0..100] to avoid overflow from misconfigured ranges
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    newValue = (int)((DLG_MAX_VALUE - 1) * (pct / 100.0));
  } else {
    // legacy cumulative mapping
    if (m_max == 0)
      newValue = 0;
    else {
      double frac = (double)(m_value + m_const) / (double)m_max;
      if (frac < 0.0) frac = 0.0;
      if (frac > 1.0) frac = 1.0;
      newValue = (int)((DLG_MAX_VALUE - 1) * frac);
    }
  }

  if (newValue < 0)
    newValue = 0;
  if (newValue > DLG_MAX_VALUE - 1)
    newValue = DLG_MAX_VALUE - 1;

  // enforce monotonic non-decreasing updates to avoid visible regressions
  if (newValue < (int)m_lastReportedValue)
    newValue = (int)m_lastReportedValue;

  // Debug log suppressed to avoid flooding logs with progress updates.

  if (!m_dlg->Update(newValue, msg))
    return false;

  // record last reported value
  m_lastReportedValue = newValue;
  return true;
}
