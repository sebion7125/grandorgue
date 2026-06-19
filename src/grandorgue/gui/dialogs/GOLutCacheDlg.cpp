/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOLutCacheDlg.h"

#include <atomic>
#include <thread>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/filename.h>
#include <wx/intl.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>

#include "GOEvent.h"
#include "GOOrganController.h"
#include "sound/GOSoundSystem.h"
#include "sound/playing/GOLutCacheFile.h"

enum {
  ID_BTN_GENERATE = wxID_HIGHEST + 1,
  ID_BTN_DELETE,
};

wxBEGIN_EVENT_TABLE(GOLutCacheDlg, wxDialog)
  EVT_BUTTON(ID_BTN_GENERATE, GOLutCacheDlg::OnGenerate)
  EVT_BUTTON(ID_BTN_DELETE,   GOLutCacheDlg::OnDelete)
wxEND_EVENT_TABLE()

GOLutCacheDlg::GOLutCacheDlg(
  wxWindow          *parent,
  GOOrganController *controller,
  GOSoundSystem     &soundSystem)
  : wxDialog(
      parent,
      wxID_ANY,
      _("Release Alignment LUT Cache"),
      wxDefaultPosition,
      wxDefaultSize,
      wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    p_controller(controller),
    r_soundSystem(soundSystem),
    m_cacheStatusLabel(nullptr),
    m_statusLabel(nullptr),
    m_cbForceAll(nullptr) {

  wxBoxSizer *topSizer = new wxBoxSizer(wxVERTICAL);

  // ── Current cache status ──────────────────────────────────────────────────
  topSizer->Add(
    new wxStaticText(this, wxID_ANY, _("Current cache status:")),
    0, wxLEFT | wxTOP | wxRIGHT, 10);
  m_cacheStatusLabel = new wxStaticText(this, wxID_ANY, wxEmptyString);
  topSizer->Add(m_cacheStatusLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
  UpdateCacheStatus();

  topSizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

  // ── Info text ─────────────────────────────────────────────────────────────
  topSizer->Add(
    new wxStaticText(
      this, wxID_ANY,
      _("Generates a pre-computed LUT cache for release alignment.\n\n"
        "Default: only releases that passed quality checks during the last\n"
        "organ load are cached (recommended for most organs).\n\n"
        "Force all: also includes releases that normally use the legacy\n"
        "fallback path (drift, low coherence, mixtures). The resulting LUT\n"
        "may be imperfect but avoids recomputation on every load.")),
    0, wxALL, 10);

  // ── Force-all checkbox ────────────────────────────────────────────────────
  m_cbForceAll = new wxCheckBox(
    this, wxID_ANY,
    _("Force generation for all releases (including legacy-fallback releases)"));
  topSizer->Add(m_cbForceAll, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

  // ── CPU hint ──────────────────────────────────────────────────────────────
  wxStaticText *hint = new wxStaticText(
    this, wxID_ANY,
    _("Cache generation is CPU intensive.\n"
      "Audio playback remains available during generation\n"
      "but may stutter on slower systems."));
  hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
  topSizer->Add(hint, 0, wxALL, 10);

  // ── Action status label ───────────────────────────────────────────────────
  m_statusLabel = new wxStaticText(this, wxID_ANY, wxEmptyString);
  topSizer->Add(m_statusLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

  // ── Buttons ───────────────────────────────────────────────────────────────
  wxBoxSizer *btnSizer = new wxBoxSizer(wxHORIZONTAL);
  btnSizer->Add(
    new wxButton(this, ID_BTN_GENERATE, _("Generate")),
    0, wxRIGHT, 6);
  btnSizer->Add(
    new wxButton(this, ID_BTN_DELETE, _("Delete Cache")),
    0, wxRIGHT, 6);
  btnSizer->AddStretchSpacer();
  btnSizer->Add(
    new wxButton(this, wxID_CLOSE, _("Close")),
    0);
  topSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 10);

  SetSizerAndFit(topSizer);
  SetEscapeId(wxID_CLOSE);
}

void GOLutCacheDlg::UpdateCacheStatus() {
  if (!m_cacheStatusLabel || !p_controller) return;

  const wxString path = p_controller->GetLutCachePath();

  if (!wxFileExists(path)) {
    m_cacheStatusLabel->SetLabel(_("   Not present"));
    m_cacheStatusLabel->SetForegroundColour(
      wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    return;
  }

  // headerOnly=true: reads ~104 bytes only, avoids loading the full file
  // just for the status display.  The LUT data is already in RAM from Load().
  GOLutCacheReader reader;
  const bool valid = reader.Load(
    path, p_controller->GetOdfHash(), p_controller->GetLutReleaseCount(),
    /*headerOnly=*/true);

  if (!valid) {
    m_cacheStatusLabel->SetLabel(
      _("   Invalid (ODF changed, version mismatch, or corrupted)"));
    m_cacheStatusLabel->SetForegroundColour(*wxRED);
    return;
  }

  wxFileOffset sz = wxFileName::GetSize(path).GetLo();
  m_cacheStatusLabel->SetLabel(wxString::Format(
    _("   Valid - %u of %u releases cached, %.1f KB"),
    reader.GetLutCount(),
    p_controller->GetLutReleaseCount(),
    sz / 1024.0));
  m_cacheStatusLabel->SetForegroundColour(*wxBLACK);
}

void GOLutCacheDlg::OnGenerate(wxCommandEvent &) {
  if (!p_controller) return;

  const bool forceAll = m_cbForceAll && m_cbForceAll->IsChecked();
  const unsigned total = p_controller->GetLutReleaseCount();

  if (total == 0) {
    // Let GenerateLutCache provide the detailed error.
    wxString err;
    p_controller->GenerateLutCache(err, forceAll);
    m_statusLabel->SetLabel(wxString::Format(_("Error: %s"), err));
    m_statusLabel->SetForegroundColour(*wxRED);
    Layout();
    return;
  }

  // Progress dialog with elapsed/remaining time and Cancel.
  wxProgressDialog prog(
    _("Release Alignment LUT Cache"),
    _("Initialising..."),
    (int)total, this,
    wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT |
    wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);

  std::atomic<unsigned> doneCount{0};
  std::atomic<bool>     cancelled{false};
  std::atomic<bool>     workerDone{false};
  wxString errorMsg;
  bool     ok = false;

  // Run computation + file write in a background thread.
  std::thread worker([&]() {
    ok = p_controller->GenerateLutCache(errorMsg, forceAll,
                                        &doneCount, &cancelled);
    workerDone.store(true, std::memory_order_release);
  });

  // Poll until worker finishes (computation AND write).
  while (!workerDone.load(std::memory_order_acquire)) {
    const unsigned done = doneCount.load(std::memory_order_relaxed);
    wxString msg;
    if (done < total)
      msg = wxString::Format(_("%u / %u releases processed"), done, total);
    else
      msg = _("Writing cache file to disk...");
    if (!prog.Update((int)std::min(done, total), msg))
      cancelled.store(true, std::memory_order_relaxed);
    wxMilliSleep(80);
  }

  worker.join();

  if (cancelled.load() && !ok) {
    m_statusLabel->SetLabel(_("Generation cancelled."));
    m_statusLabel->SetForegroundColour(
      wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    UpdateCacheStatus();
    Layout();
    return;
  }

  if (ok) {
    bool applied = false;
    r_soundSystem.WithOrganEngineQuiesced([this, &applied]() {
      applied = p_controller->ApplyLutCacheNow();
    });
    const wxString status = applied
      ? _("Cache generated and activated immediately.")
      : _("Cache generated. Will be used on next organ load.");
    m_statusLabel->SetLabel(status);
    m_statusLabel->SetForegroundColour(*wxBLACK);
    UpdateCacheStatus();
    // No MessageBox on success — status label is sufficient.
  } else {
    m_statusLabel->SetLabel(wxString::Format(_("Error: %s"), errorMsg));
    m_statusLabel->SetForegroundColour(*wxRED);
    GOMessageBox(
      wxString::Format(_("Cache generation failed:\n%s"), errorMsg),
      _("LUT Cache"), wxOK | wxICON_ERROR, this);
  }
  Layout();
}

void GOLutCacheDlg::OnDelete(wxCommandEvent &) {
  if (!p_controller) return;
  // Only delete the file.  Do NOT clear in-memory aligners: after
  // OverrideCorrLutsFromCache() the live-computed LUTs are gone and clearing
  // the injected LUT would leave empty aligners (legacy fallback, not
  // correlation).  Correct behaviour: current session keeps its alignment
  // state; the deletion takes effect on the next organ load.
  p_controller->DeleteLutCache();
  m_statusLabel->SetLabel(
    _("Cache file deleted. Current alignment state unchanged until next reload."));
  m_statusLabel->SetForegroundColour(*wxBLACK);
  UpdateCacheStatus();
  Layout();
}
