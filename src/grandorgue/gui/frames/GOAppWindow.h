/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOAPPWINDOW_H
#define GOAPPWINDOW_H

#include <memory>
#include <vector>

#include <wx/frame.h>
#include <wx/timer.h>
#include <wx/power.h>

#include "gui/size/GOResizable.h"
#include "help/GOHelpRequestor.h"
#include "midi/GOMidiListener.h"
#include "midi/events/GOMidiCallback.h"
#include "modification/GOModificationListener.h"
#include "threading/GOMutex.h"
#include "updater/GOUpdateChecker.h"

#include "GOEvent.h"

enum {
  ID_PLACE_TIMER = wxID_HIGHEST + 499,
  ID_Crossfade_Linear = wxID_HIGHEST + 500,
  ID_Crossfade_SinEq,
  ID_Crossfade_Sin2,
  ID_Crossfade_SqrtEq,
  ID_Crossfade_X2,
  ID_Crossfade_Custom,
  ID_ReleaseAlign_Legacy,
  ID_ReleaseAlign_Correlation,
  ID_LUT_CACHE_GENERATE,
  ID_LUT_CACHE_DELETE,
};

class GOAudioGauge;
class GOConfig;
class GOGuiApp;
class GOGuiOrgan;
class GOMidiEvent;
class GOOrgan;
class GOOrganController;
class GOProgressDialog;
class GOSoundSystem;
class wxChoice;
class wxMsgBoxEvent;
class wxRenameFileEvent;
class wxSpinCtrl;
class wxToolBar;
class wxToolBarToolBase;

class GOAppWindow : public wxFrame,
                    private GOHelpRequestor,
                    public GOResizable,
                    protected GOMidiCallback,
                    private GOModificationListener {
private:
  GOGuiApp &r_app;
  GOConfig &r_config;
  GOSoundSystem &r_SoundSystem;
  GOMidiSystem &r_MidiSystem;

  GOMutex m_mutex;

  wxMenu *m_file_menu;
  wxMenu *m_audio_menu;
  wxMenu *m_crossfade_menu;
  wxMenu *m_releasealign_menu;
  wxMenu *m_panel_menu;
  wxMenu *m_favorites_menu;
  wxMenu *m_recent_menu;
  wxMenu *m_temperament_menu;
  wxMenu *m_autoMidiMenu;

  std::unique_ptr<GOGuiOrgan> mp_organ;
  GOOrganController *p_OrganController;
  wxToolBar *m_ToolBar;
  GOAudioGauge *m_SamplerUsage;
  wxControl *m_VolumeControl;
  wxToolBarToolBase *m_VolumeControlTool;
  std::vector<GOAudioGauge *> m_VolumeGauge;
  wxSpinCtrl *m_Transpose;
  wxChoice *m_ReleaseLength;
  wxSpinCtrl *m_Polyphony;
  wxSpinCtrl *m_SetterPosition;
  wxSpinCtrl *m_Volume;
  GOMidiListener m_listener;
  wxString m_Title;
  wxString m_Label;
  bool m_MidiMonitor;
  bool m_isMeterReady;
  bool m_IsGuiOnly;
  std::unique_ptr<GOThread> m_UpdateCheckerThread;
  GOUpdateChecker::Result m_StartupUpdateCheckerResult;

  // to avoid event processing when the settings dialog is open
  bool m_InSettings;
  wxEventType m_AfterSettingsEventType;
  int m_AfterSettingsEventId;
  GOOrgan *p_AfterSettingsEventOrgan;

  // Updates ReleseLength in the model, in the config, and in the control
  void UpdateReleaseLength(unsigned releaseLength);
  void UpdatePanelMenu();
  void UpdateFavoritesMenu();
  void UpdateRecentMenu();
  void UpdateTemperamentMenu();
  bool AdjustVolumeControlWithSettings();
  void UpdateSize();
  void UpdateVolumeControlWithSettings();

  void AttachDetachOrganController(bool isToAttach);

  // Processes the organ model modification event:
  // updates some controls according the organ model changes
  void OnIsModifiedChanged(bool modified) override;

  void LoadOrgan(const GOOrgan &organ, const wxString &cmb = wxEmptyString);

  void OnMeters(wxCommandEvent &event);
  void OnLoadFile(wxCommandEvent &event);
  void OnLoad(wxCommandEvent &event);
  void OnLoadFavorite(wxCommandEvent &event);
  void OnLoadRecent(wxCommandEvent &event);
  void OnInstall(wxCommandEvent &event);
  void OnOpen(wxCommandEvent &event);
  void OnSave(wxCommandEvent &event);
  void OnMenuClose(wxCommandEvent &event);
  void OnExit(wxCommandEvent &event);
  void OnImportCombinations(wxCommandEvent &event);
  void OnExportCombinations(wxCommandEvent &event);
  void OnImportSettings(wxCommandEvent &event);
  void OnExport(wxCommandEvent &event);
  void OnCache(wxCommandEvent &event);
  void OnCacheDelete(wxCommandEvent &event);
  void OnReload(wxCommandEvent &event);

public:
  /** Zeigerposition beim Programmstart, gesetzt aus GOGuiApp::OnInit.
   * Bestimmt den Startbildschirm des Hauptfensters: im Konstruktor
   * gemessen ist der Zeiger schon verrutscht. */
  static void SetStartupPointer(const wxPoint &pt);

  // Audio Monitor "Find ceiling" sweep support: change the buffer size
  // (sound restart, organ stays) / the sample codec (requires a reload).
  bool BenchmarkSetBufferSize(unsigned samplesPerBuffer);
  void BenchmarkSetV3AndReload(bool useV3);

private:
  void OnRevert(wxCommandEvent &event);
  void OnProperties(wxCommandEvent &event);

  void OnOrganSettings(wxCommandEvent &event);
  void OnMidiList(wxCommandEvent &event);
  void OnStops(wxCommandEvent &event);

  void OnAudioPanic(wxCommandEvent &event);
  void OnAudioMemset(wxCommandEvent &event);
  void OnAudioState(wxCommandEvent &event);
  // Opens a new Audio Monitor window and records the open state in the
  // global config so the next start reopens it (see the .cpp comment).
  void ShowAudioMonitor();
  void OnAudioMonitor(wxCommandEvent &event);
  void OnSetCrossfade(wxCommandEvent &event);
  void OnSetReleaseAlign(wxCommandEvent &event);
  void OnLutCacheGenerate(wxCommandEvent &event);
  void OnLutCacheDelete(wxCommandEvent &event);
  void OnUpdateLutCache(wxUpdateUIEvent &event);

  void SetEventAfterSettings(
    wxEventType eventType, int eventId, GOOrgan *pOrganFile = NULL);

  void OnSettings(wxCommandEvent &event);
  void OnMidiLoad(wxCommandEvent &event);
  void OnMidiArchiveLoad(wxCommandEvent &event);
  void OnMidiArchiveClear(wxCommandEvent &event);
  void UpdateAutoMidiMenu();
  void OnMidiMonitor(wxCommandEvent &event);

  void OnPreset(wxCommandEvent &event);
  void OnTemperament(wxCommandEvent &event);
  void OnHelp(wxCommandEvent &event);
  void OnHelpAbout(wxCommandEvent &event);

  void OnSettingsVolume(wxCommandEvent &event);
  void OnSettingsPolyphony(wxCommandEvent &event);
  void OnSettingsMemory(wxCommandEvent &event);
  void OnSettingsMemoryEnter(wxCommandEvent &event);
  void OnSettingsTranspose(wxCommandEvent &event);
  void OnSettingsReleaseLength(wxCommandEvent &event);

  void OnKeyCommand(wxKeyEvent &event);
  void OnChangeTranspose(wxCommandEvent &event);
  void OnChangeSetter(wxCommandEvent &event);
  void OnChangeVolume(wxCommandEvent &event);
  void OnPanel(wxCommandEvent &event);

  void OnSize(wxSizeEvent &event);

  void OnMenuOpen(wxMenuEvent &event);
  void OnCloseWindow(wxCloseEvent &event);

#ifdef wxHAS_POWER_EVENTS
  void OnPowerSuspended(wxPowerEvent &event);
  void OnPowerResume(wxPowerEvent &event);
#endif

  void OnMidiEvent(const GOMidiEvent &event) override;

  void OnUpdateLoaded(wxUpdateUIEvent &event);
  void OnSetTitle(wxCommandEvent &event);
  void OnMsgBox(wxMsgBoxEvent &event);
  void OnRenameFile(wxRenameFileEvent &event);

  void OnUpdateCheckingRequested(wxCommandEvent &event);
  void OnUpdateCheckingCompletion(GOUpdateChecker::CompletionEvent &event);
  void OnNewReleaseInfoRequested(wxCommandEvent &event);
  void OnNewReleaseDownload(wxCommandEvent &event);

  // isDiscardChanges: close without the save prompt, dropping unsaved
  // organ-settings changes (automated benchmark reloads).
  bool CloseOrgan(bool isForce = false, bool isDiscardChanges = false);
  bool CloseProgram(bool isForce = false);

  unsigned m_PlaceTries = 0;
  wxTimer m_PlaceTimer;

  /** Die zuletzt aus Konfiguration bzw. Orgel uebernommene Lage.  Unter
   * Wayland kann GO die tatsaechliche nicht erfahren, deshalb gibt
   * GetPosSize() diese hier zurueck - so schreibt ein Wayland-Lauf die
   * gespeicherten Werte unveraendert wieder heraus, statt sie durch den
   * Rand der Fensterdekoration zu ersetzen. */
  wxPoint m_KeptPos = wxDefaultPosition;

  void OnPlaceTimer(wxTimerEvent &event);
  void Open(const GOOrgan &organ);

  bool InstallOrganPackage(wxString name);
  void LoadLastOrgan();
  void LoadFirstOrgan();
  void SendLoadOrgan(const GOOrgan &organ);

public:
  GOAppWindow(
    GOGuiApp &app,
    wxFrame *pParentFrame,
    wxWindowID id,
    const wxString &title,
    const wxPoint &pos,
    const wxSize &size,
    const long type,
    GOSoundSystem &sound);
  virtual ~GOAppWindow(void);

  void Init(const wxString &filename, bool isGuiOnly);

  void DoSplash(bool timeout = true);

  void SendLoadFile(wxString filename);

  virtual GOLogicalRect GetPosSize() const override;
  /** Lage UND Groesse - die Schnittstelle GOResizable.  Beim Orgelladen
   * ruft GOGuiOrgan das mit der pro Orgel gespeicherten Lage auf, und
   * die soll gelten. */
  virtual void SetPosSize(const GOLogicalRect &rect) override;

  /** Nur die GROESSE, fuer den Programmstart: den Bildschirm waehlt
   * dort der Fenstermanager (siehe OnPlaceTimer). */
  void SetStartupSize(const GOLogicalRect &rect);

  DECLARE_EVENT_TABLE()
};

#endif
