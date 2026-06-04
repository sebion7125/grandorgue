/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOLutCacheDlg.h"

#include <wx/button.h>
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
    m_statusLabel(nullptr) {

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
      _("Generates a pre-computed LUT cache for all releases that passed\n"
        "the quality criteria during the last organ load.\n\n"
        "The cache is used automatically on the next load and replaces\n"
        "the live correlation computation for cached releases.")),
    0, wxALL, 10);

  // ── Current quality criteria (informational) ──────────────────────────────
  topSizer->Add(
    new wxStaticText(this, wxID_ANY, _("Quality criteria (from current build):")),
    0, wxLEFT | wxRIGHT, 10);

  const GOLutGeneratorCriteria crit;
  topSizer->Add(
    new wxStaticText(
      this, wxID_ANY,
      wxString::Format(
        _("   Min correlation score:  %.2f\n"
          "   Min circular coherence: %.2f\n"
          "   Max gap-fill points:    %u"),
        crit.minScore, crit.minCoherence, crit.maxGapFills)),
    0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

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

  GOLutCacheReader reader;
  const bool valid = reader.Load(
    path, p_controller->GetOdfHash(), p_controller->GetLutReleaseCount());

  if (!valid) {
    m_cacheStatusLabel->SetLabel(
      _("   Invalid (ODF changed, version mismatch, or corrupted)"));
    m_cacheStatusLabel->SetForegroundColour(*wxRED);
    return;
  }

  // Count cached entries
  unsigned cached = 0;
  for (int32_t idx : reader.GetReleaseMap())
    if (idx >= 0) cached++;

  wxFileOffset sz = wxFileName::GetSize(path).GetLo();
  m_cacheStatusLabel->SetLabel(wxString::Format(
    _("   Valid — %u of %u releases cached, %.1f KB"),
    cached,
    p_controller->GetLutReleaseCount(),
    sz / 1024.0));
  m_cacheStatusLabel->SetForegroundColour(*wxBLACK);
}

void GOLutCacheDlg::OnGenerate(wxCommandEvent &) {
  if (!p_controller) return;

  wxProgressDialog prog(
    _("Release Alignment LUT Cache"),
    _("Generating..."),
    100, this,
    wxPD_APP_MODAL | wxPD_AUTO_HIDE);
  prog.Pulse();

  wxString errorMsg;
  const bool ok = p_controller->GenerateLutCache(errorMsg);

  if (ok) {
    // Immediate activation: apply cache to in-memory aligners while the
    // audio engine is quiesced (worker threads idle, no data race).
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
    GOMessageBox(status, _("LUT Cache"), wxOK | wxICON_INFORMATION, this);
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
  p_controller->DeleteLutCache();
  r_soundSystem.WithOrganEngineQuiesced([this]() {
    p_controller->ClearAllCachedLuts();
  });
  m_statusLabel->SetLabel(
    _("Cache deleted. Live computation active until next reload."));
  m_statusLabel->SetForegroundColour(*wxBLACK);
  UpdateCacheStatus();
  Layout();
}
