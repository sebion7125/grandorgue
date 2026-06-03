/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOLUT_CACHE_DLG_H
#define GOLUT_CACHE_DLG_H

#include <wx/dialog.h>

class GOOrganController;
class wxStaticText;

// Dialog for generating and deleting the Release Alignment LUT cache.
// The generation runs synchronously in the GUI thread; a progress bar
// is shown via wxProgressDialog.
class GOLutCacheDlg : public wxDialog {
public:
  GOLutCacheDlg(wxWindow *parent, GOOrganController *controller);

private:
  GOOrganController *p_controller;
  wxStaticText      *m_statusLabel;

  void OnGenerate(wxCommandEvent &event);
  void OnDelete(wxCommandEvent &event);

  wxDECLARE_EVENT_TABLE();
};

#endif /* GOLUT_CACHE_DLG_H */
