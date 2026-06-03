/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOLUT_CACHE_DLG_H
#define GOLUT_CACHE_DLG_H

#include <wx/dialog.h>

class GOOrganController;
class GOSoundSystem;
class wxStaticText;

// Dialog for generating and deleting the Release Alignment LUT cache.
// The generation runs synchronously in the GUI thread; a progress bar
// is shown via wxProgressDialog.  After generation, the cache is applied
// immediately via GOSoundSystem::WithOrganEngineQuiesced (no audio glitch).
class GOLutCacheDlg : public wxDialog {
public:
  GOLutCacheDlg(
    wxWindow          *parent,
    GOOrganController *controller,
    GOSoundSystem     &soundSystem);

private:
  GOOrganController *p_controller;
  GOSoundSystem     &r_soundSystem;
  wxStaticText      *m_cacheStatusLabel; // shows current .golut file state
  wxStaticText      *m_statusLabel;      // shows result of last action

  void UpdateCacheStatus();             // refresh m_cacheStatusLabel from disk
  void OnGenerate(wxCommandEvent &event);
  void OnDelete(wxCommandEvent &event);

  wxDECLARE_EVENT_TABLE();
};

#endif /* GOLUT_CACHE_DLG_H */
