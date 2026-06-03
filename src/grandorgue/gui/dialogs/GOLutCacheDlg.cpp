/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOLutCacheDlg.h"

#include <wx/button.h>
#include <wx/intl.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "GOEvent.h"
#include "GOOrganController.h"
#include "sound/playing/GOLutCacheFile.h"

enum {
  ID_BTN_GENERATE = wxID_HIGHEST + 1,
  ID_BTN_DELETE,
};

wxBEGIN_EVENT_TABLE(GOLutCacheDlg, wxDialog)
  EVT_BUTTON(ID_BTN_GENERATE, GOLutCacheDlg::OnGenerate)
  EVT_BUTTON(ID_BTN_DELETE,   GOLutCacheDlg::OnDelete)
wxEND_EVENT_TABLE()

GOLutCacheDlg::GOLutCacheDlg(wxWindow *parent, GOOrganController *controller)
  : wxDialog(
      parent,
      wxID_ANY,
      _("Release Alignment LUT Cache"),
      wxDefaultPosition,
      wxDefaultSize,
      wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    p_controller(controller),
    m_statusLabel(nullptr) {

  wxBoxSizer *topSizer = new wxBoxSizer(wxVERTICAL);

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

  // ── Status label ──────────────────────────────────────────────────────────
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

void GOLutCacheDlg::OnGenerate(wxCommandEvent &) {
  if (!p_controller) return;

  wxProgressDialog prog(
    _("Release Alignment LUT Cache"),
    _("Generating…"),
    100, this,
    wxPD_APP_MODAL | wxPD_AUTO_HIDE);
  prog.Pulse();

  wxString errorMsg;
  const bool ok = p_controller->GenerateLutCache(errorMsg);

  if (ok) {
    m_statusLabel->SetLabel(
      wxString::Format(_("Cache generated: %u releases cached."),
                       p_controller->GetLutReleaseCount()));
    m_statusLabel->SetForegroundColour(*wxBLACK);
    GOMessageBox(
      _("Release Alignment LUT cache generated successfully."),
      _("LUT Cache"), wxOK | wxICON_INFORMATION, this);
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
  m_statusLabel->SetLabel(_("Cache deleted."));
  m_statusLabel->SetForegroundColour(*wxBLACK);
  Layout();
}
