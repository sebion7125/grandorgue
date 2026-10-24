/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2025 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSettingsPaths.h"

#include <wx/checkbox.h>
#include <wx/filepicker.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/string.h>

#include "config/GOConfig.h"
#include "gui/wxcontrols/GODirPickerCtrl.h"

GOSettingsPaths::GOSettingsPaths(GOConfig &settings, wxWindow *parent)
  : wxPanel(parent, wxID_ANY), m_config(settings) {
  wxBoxSizer *const topSizer = new wxBoxSizer(wxVERTICAL);
  wxFlexGridSizer *const grid = new wxFlexGridSizer(2, 5, 5);

  grid->AddGrowableCol(1, 1);

  grid->Add(
    new wxStaticText(this, wxID_ANY, _("Samplesets:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
  grid->Add(
    m_Samplesets = new GODirPickerCtrl(
      this,
      wxID_ANY,
      _("Select directory for open new organs at"),
      wxDefaultPosition,
      wxDefaultSize,
      wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST | wxDIRP_SMALL),
    0,
    wxEXPAND);

  grid->Add(
    new wxStaticText(this, wxID_ANY, _("Organ packages:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
  grid->Add(
    m_OrganPackages = new GODirPickerCtrl(
      this,
      wxID_ANY,
      _("Select directory for load new organ packages from"),
      wxDefaultPosition,
      wxDefaultSize,
      wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST | wxDIRP_SMALL),
    0,
    wxEXPAND);

  grid->Add(
    new wxStaticText(this, wxID_ANY, _("Organ cache:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
  grid->Add(
    m_OrganCache = new GODirPickerCtrl(
      this,
      wxID_ANY,
      _("Select directory for storing organ cache"),
      wxDefaultPosition,
      wxDefaultSize,
      wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST | wxDIRP_SMALL),
    0,
    wxEXPAND);
  grid->Add(
    new wxStaticText(this, wxID_ANY, _("Organ settings:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
  grid->Add(
    m_OrganSettings = new GODirPickerCtrl(
      this,
      wxID_ANY,
      _("Select directory for storing organ settings"),
      wxDefaultPosition,
      wxDefaultSize,
      wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST | wxDIRP_SMALL),
    0,
    wxEXPAND);

  grid->Add(
    new wxStaticText(this, wxID_ANY, _("Combinations:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
  grid->Add(
    m_OrganCombinations = new GODirPickerCtrl(
      this,
      wxID_ANY,
      _("Select directory for export/import organ combinations"),
      wxDefaultPosition,
      wxDefaultSize,
      wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST | wxDIRP_SMALL),
    0,
    wxEXPAND);

  grid->Add(
    new wxStaticText(this, wxID_ANY, _("Export/import:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
  grid->Add(
    m_ExportImport = new GODirPickerCtrl(
      this,
      wxID_ANY,
      _("Select directory for export/import organ settings"),
      wxDefaultPosition,
      wxDefaultSize,
      wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST | wxDIRP_SMALL),
    0,
    wxEXPAND);

  grid->Add(
    new wxStaticText(this, wxID_ANY, _("Audio recording:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
  grid->Add(
    m_AudioRecorder = new GODirPickerCtrl(
      this,
      wxID_ANY,
      _("Select directory for your audio recordings"),
      wxDefaultPosition,
      wxDefaultSize,
      wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST | wxDIRP_SMALL),
    0,
    wxEXPAND);

  grid->Add(
    new wxStaticText(this, wxID_ANY, _("MIDI recording:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
  grid->Add(
    m_MidiRecorder = new GODirPickerCtrl(
      this,
      wxID_ANY,
      _("Select directory for your MIDI recording"),
      wxDefaultPosition,
      wxDefaultSize,
      wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST | wxDIRP_SMALL),
    0,
    wxEXPAND);

  grid->Add(
    new wxStaticText(this, wxID_ANY, _("MIDI player:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
  grid->Add(
    m_MidiPlayer = new GODirPickerCtrl(
      this,
      wxID_ANY,
      _("Select directory for the MIDI player"),
      wxDefaultPosition,
      wxDefaultSize,
      wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST | wxDIRP_SMALL),
    0,
    wxEXPAND);

  // --- Auto MIDI Archive section ---
  wxStaticBoxSizer *const archiveBox = new wxStaticBoxSizer(
    new wxStaticBox(this, wxID_ANY, _("Automatic MIDI Archive")), wxVERTICAL);
  wxFlexGridSizer *const ag = new wxFlexGridSizer(2, 5, 5);
  ag->AddGrowableCol(1, 1);

  m_AutoArchiveEnabled
    = new wxCheckBox(this, wxID_ANY, _("Enable automatic MIDI archive"));
  archiveBox->Add(m_AutoArchiveEnabled, 0, wxALL, 5);

  ag->Add(
    new wxStaticText(this, wxID_ANY, _("Archive folder:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
  ag->Add(
    m_AutoArchivePath = new GODirPickerCtrl(
      this,
      wxID_ANY,
      _("Select directory for automatic MIDI archive"),
      wxDefaultPosition,
      wxDefaultSize,
      wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST | wxDIRP_SMALL),
    0,
    wxEXPAND);

  ag->Add(
    new wxStaticText(this, wxID_ANY, _("Inactivity timeout:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
  {
    wxBoxSizer *const row = new wxBoxSizer(wxHORIZONTAL);
    m_AutoArchiveTimeout = new wxSpinCtrl(
      this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
      wxSP_ARROW_KEYS, 5, 300, 8);
    row->Add(m_AutoArchiveTimeout, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(
      new wxStaticText(this, wxID_ANY, _(" seconds")),
      0,
      wxALIGN_CENTER_VERTICAL | wxLEFT,
      4);
    ag->Add(row, 0, wxEXPAND);
  }

  m_AutoArchiveCleanupEnabled = new wxCheckBox(
    this, wxID_ANY, _("Delete archive files older than"));
  ag->Add(m_AutoArchiveCleanupEnabled, 0, wxALIGN_CENTER_VERTICAL);
  {
    wxBoxSizer *const row = new wxBoxSizer(wxHORIZONTAL);
    m_AutoArchiveCleanupDays = new wxSpinCtrl(
      this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
      wxSP_ARROW_KEYS, 1, 3650, 30);
    row->Add(m_AutoArchiveCleanupDays, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(
      new wxStaticText(this, wxID_ANY, _(" days")),
      0,
      wxALIGN_CENTER_VERTICAL | wxLEFT,
      4);
    ag->Add(row, 0, wxEXPAND);
  }

  archiveBox->Add(ag, 0, wxALL | wxEXPAND, 5);

  m_AutoLoadAfterRecord = new wxCheckBox(
    this,
    wxID_ANY,
    _("Load last manual recording into MIDI player after stopping"));
  archiveBox->Add(m_AutoLoadAfterRecord, 0, wxALL, 5);

  topSizer->Add(grid, 0, wxALL | wxEXPAND, 5);
  topSizer->Add(archiveBox, 0, wxALL | wxEXPAND, 5);
  this->SetSizer(topSizer);
  topSizer->Fit(this);

  m_Samplesets->SetPath(m_config.OrganPath());
  m_OrganPackages->SetPath(m_config.OrganPackagePath());
  m_OrganCache->SetPath(m_config.OrganCachePath());
  m_OrganSettings->SetPath(m_config.OrganSettingsPath());
  m_OrganCombinations->SetPath(m_config.OrganCombinationsPath());
  m_ExportImport->SetPath(m_config.ExportImportPath());
  m_AudioRecorder->SetPath(m_config.AudioRecorderPath());
  m_MidiRecorder->SetPath(m_config.MidiRecorderPath());
  m_MidiPlayer->SetPath(m_config.MidiPlayerPath());

  m_AutoArchiveEnabled->SetValue(m_config.AutoMidiArchiveEnabled());
  m_AutoArchivePath->SetPath(m_config.AutoMidiArchivePath());
  m_AutoArchiveTimeout->SetValue(m_config.AutoMidiInactivityTimeoutSec());
  m_AutoArchiveCleanupEnabled->SetValue(m_config.AutoMidiCleanupEnabled());
  m_AutoArchiveCleanupDays->SetValue(m_config.AutoMidiCleanupAgeDays());
  m_AutoLoadAfterRecord->SetValue(m_config.AutoLoadAfterManualRecord());

  // Sync enabled state of dependent controls
  const bool archiveOn = m_config.AutoMidiArchiveEnabled();
  m_AutoArchivePath->Enable(archiveOn);
  m_AutoArchiveTimeout->Enable(archiveOn);
  m_AutoArchiveCleanupEnabled->Enable(archiveOn);
  m_AutoArchiveCleanupDays->Enable(
    archiveOn && m_config.AutoMidiCleanupEnabled());

  m_AutoArchiveEnabled->Bind(
    wxEVT_CHECKBOX, &GOSettingsPaths::OnAutoArchiveToggle, this);
  m_AutoArchiveCleanupEnabled->Bind(
    wxEVT_CHECKBOX, &GOSettingsPaths::OnAutoArchiveToggle, this);
}

void GOSettingsPaths::OnAutoArchiveToggle(wxCommandEvent &WXUNUSED(event)) {
  const bool archiveOn = m_AutoArchiveEnabled->GetValue();
  m_AutoArchivePath->Enable(archiveOn);
  m_AutoArchiveTimeout->Enable(archiveOn);
  m_AutoArchiveCleanupEnabled->Enable(archiveOn);
  m_AutoArchiveCleanupDays->Enable(
    archiveOn && m_AutoArchiveCleanupEnabled->GetValue());
}

bool GOSettingsPaths::TransferDataFromWindow() {
  m_config.OrganPath(m_Samplesets->GetPath());
  m_config.OrganPackagePath(m_OrganPackages->GetPath());
  m_config.OrganCachePath(m_OrganCache->GetPath());
  m_config.OrganSettingsPath(m_OrganSettings->GetPath());
  m_config.OrganCombinationsPath(m_OrganCombinations->GetPath());
  m_config.ExportImportPath(m_ExportImport->GetPath());
  m_config.AudioRecorderPath(m_AudioRecorder->GetPath());
  m_config.MidiRecorderPath(m_MidiRecorder->GetPath());
  m_config.MidiPlayerPath(m_MidiPlayer->GetPath());

  m_config.AutoMidiArchiveEnabled(m_AutoArchiveEnabled->GetValue());
  m_config.AutoMidiArchivePath(m_AutoArchivePath->GetPath());
  m_config.AutoMidiInactivityTimeoutSec(m_AutoArchiveTimeout->GetValue());
  m_config.AutoMidiCleanupEnabled(m_AutoArchiveCleanupEnabled->GetValue());
  m_config.AutoMidiCleanupAgeDays(m_AutoArchiveCleanupDays->GetValue());
  m_config.AutoLoadAfterManualRecord(m_AutoLoadAfterRecord->GetValue());
  return true;
}
