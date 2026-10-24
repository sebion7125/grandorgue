/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOAppWindow.h"

#include <algorithm>

#include <wx/accel.h>
#include <wx/choice.h>
#include <wx/dir.h>
#include <wx/display.h>
#include <wx/fileconf.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/platinfo.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/splash.h>
#include <wx/textctrl.h>
#include <wx/toolbar.h>

#include <algorithm>
#include <thread>

#include "archive/GOArchiveManager.h"
#include "combinations/GOSetter.h"
#include "config/GOConfig.h"
#include "config/GORegisteredOrgan.h"
#include "files/GOStdFileName.h"
#include "gui/GOGuiApp.h"
#include "gui/GOGuiOrgan.h"
#include "gui/dialogs/GOAudioMonitorDlg.h"
#include "gui/dialogs/GOLutCacheDlg.h"
#include "gui/dialogs/GONewReleaseDialog.h"
#include "gui/dialogs/GOProgressDialog.h"
#include "gui/dialogs/GOPropertiesDialog.h"
#include "gui/dialogs/GOSelectOrganDialog.h"
#include "gui/dialogs/GOSplash.h"
#include "gui/dialogs/settings/GOSettingsDialog.h"
#include "gui/dialogs/settings/GOSettingsReason.h"
#include "gui/panels/GOGUIPanel.h"
#include "gui/size/GOWindowPlacement.h"
#include "gui/wxcontrols/GOAudioGauge.h"
#include "gui/wxcontrols/go_gui_utils.h"
#include "loader/cache/GOCacheCleaner.h"
#include "midi/GOMidiSystem.h"
#include "midi/events/GOMidiEvent.h"
#include "sound/GOCrossfadeParam.h"
#include "sound/GOSoundSystem.h"
#include "sound/fast_crossfade.h"
#include "temperaments/GOTemperament.h"
#include "threading/GOMutexLocker.h"

#include "GOEvent.h"
#include "GOOrganController.h"
#include "Images.h"
#include "go_defs.h"
#include "go_ids.h"
#include "go_limits.h"
#include "go_path.h"

namespace {
struct ShortcutEntry {
  int mod, key, id;
};
// Single source of truth for all keyboard shortcuts.
// To add a shortcut: append one line here. The menu label's \tXxx annotation
// handles display; this table handles dispatch (for both GOAppWindow focus and
// forwarded events from detached panel frames).
const ShortcutEntry s_shortcuts[] = {
  {wxACCEL_NORMAL, WXK_ESCAPE, ID_AUDIO_PANIC},
  {wxACCEL_NORMAL, WXK_F1, wxID_HELP},
  {wxACCEL_NORMAL, WXK_F2, ID_ReleaseAlign_Legacy},
  {wxACCEL_NORMAL, WXK_F3, ID_ReleaseAlign_Correlation},
  {wxACCEL_NORMAL, WXK_F7, ID_Crossfade_Linear},
  {wxACCEL_NORMAL, WXK_F8, ID_Crossfade_SinEq},
  {wxACCEL_NORMAL, WXK_F9, ID_Crossfade_Sin2},
  {wxACCEL_NORMAL, WXK_F10, ID_Crossfade_SqrtEq},
  {wxACCEL_NORMAL, WXK_F11, ID_Crossfade_X2},
  {wxACCEL_NORMAL, WXK_F12, ID_Crossfade_Custom},
  {wxACCEL_CTRL, 'L', ID_FILE_LOAD},
  {wxACCEL_CTRL, 'O', ID_FILE_OPEN},
  {wxACCEL_CTRL, 'S', ID_FILE_SAVE},
  {wxACCEL_CTRL, 'I', ID_FILE_INSTALL},
  {wxACCEL_CTRL, 'P', ID_MIDI_LOAD},
};
} // namespace


BEGIN_EVENT_TABLE(GOAppWindow, wxFrame)
EVT_TIMER(ID_PLACE_TIMER, GOAppWindow::OnPlaceTimer)
EVT_MSGBOX(GOAppWindow::OnMsgBox)
EVT_RENAMEFILE(GOAppWindow::OnRenameFile)
EVT_CLOSE(GOAppWindow::OnCloseWindow)
#ifdef wxHAS_POWER_EVENTS
EVT_POWER_SUSPENDED(GOAppWindow::OnPowerSuspended)
EVT_POWER_RESUME(GOAppWindow::OnPowerResume)
#endif
EVT_CHAR_HOOK(GOAppWindow::OnKeyCommand)
EVT_COMMAND(0, wxEVT_METERS, GOAppWindow::OnMeters)
EVT_COMMAND(0, wxEVT_LOADFILE, GOAppWindow::OnLoadFile)
EVT_MENU_OPEN(GOAppWindow::OnMenuOpen)
EVT_MENU(ID_FILE_OPEN, GOAppWindow::OnOpen)
EVT_MENU(ID_FILE_LOAD, GOAppWindow::OnLoad)
EVT_MENU(ID_FILE_INSTALL, GOAppWindow::OnInstall)
EVT_MENU_RANGE(ID_LOAD_FAV_FIRST, ID_LOAD_FAV_LAST, GOAppWindow::OnLoadFavorite)
EVT_MENU_RANGE(ID_LOAD_LRU_FIRST, ID_LOAD_LRU_LAST, GOAppWindow::OnLoadRecent)
EVT_MENU(ID_FILE_SAVE, GOAppWindow::OnSave)
EVT_MENU(ID_FILE_CLOSE, GOAppWindow::OnMenuClose)
EVT_MENU(ID_FILE_EXIT, GOAppWindow::OnExit)
EVT_MENU(ID_FILE_RELOAD, GOAppWindow::OnReload)
EVT_MENU(ID_FILE_REVERT, GOAppWindow::OnRevert)
EVT_MENU(ID_FILE_PROPERTIES, GOAppWindow::OnProperties)
EVT_MENU(ID_FILE_IMPORT_COMBINATIONS, GOAppWindow::OnImportCombinations)
EVT_MENU(ID_FILE_EXPORT_COMBINATIONS, GOAppWindow::OnExportCombinations)
EVT_MENU(ID_FILE_IMPORT_SETTINGS, GOAppWindow::OnImportSettings)
EVT_MENU(ID_FILE_EXPORT, GOAppWindow::OnExport)
EVT_MENU(ID_FILE_CACHE, GOAppWindow::OnCache)
EVT_MENU(ID_FILE_CACHE_DELETE, GOAppWindow::OnCacheDelete)
EVT_MENU(ID_ORGAN_EDIT, GOAppWindow::OnOrganSettings)
EVT_MENU(ID_MIDI_LIST, GOAppWindow::OnMidiList)
EVT_MENU(ID_STOPS, GOAppWindow::OnStops)
EVT_MENU(ID_MIDI_MONITOR, GOAppWindow::OnMidiMonitor)
EVT_MENU(ID_AUDIO_PANIC, GOAppWindow::OnAudioPanic)
EVT_MENU(ID_AUDIO_MEMSET, GOAppWindow::OnAudioMemset)
EVT_MENU(ID_AUDIO_STATE, GOAppWindow::OnAudioState)
EVT_MENU(ID_AUDIO_MONITOR, GOAppWindow::OnAudioMonitor)
EVT_MENU_RANGE(
  ID_Crossfade_Linear, ID_Crossfade_Custom, GOAppWindow::OnSetCrossfade)
EVT_MENU_RANGE(
  ID_ReleaseAlign_Legacy,
  ID_ReleaseAlign_Correlation,
  GOAppWindow::OnSetReleaseAlign)
EVT_MENU(ID_LUT_CACHE_GENERATE, GOAppWindow::OnLutCacheGenerate)
EVT_MENU(ID_LUT_CACHE_DELETE, GOAppWindow::OnLutCacheDelete)
EVT_UPDATE_UI(ID_LUT_CACHE_GENERATE, GOAppWindow::OnUpdateLutCache)
EVT_UPDATE_UI(ID_LUT_CACHE_DELETE, GOAppWindow::OnUpdateLutCache)
EVT_MENU(ID_SETTINGS, GOAppWindow::OnSettings)
EVT_MENU(ID_MIDI_LOAD, GOAppWindow::OnMidiLoad)
EVT_MENU(ID_MIDI_ARCHIVE_CLEAR, GOAppWindow::OnMidiArchiveClear)
EVT_MENU_RANGE(
  ID_MIDI_ARCHIVE_FIRST, ID_MIDI_ARCHIVE_LAST, GOAppWindow::OnMidiArchiveLoad)
EVT_MENU(wxID_HELP, GOAppWindow::OnHelp)
EVT_MENU(wxID_ABOUT, GOAppWindow::OnHelpAbout)
EVT_COMMAND(0, wxEVT_WINTITLE, GOAppWindow::OnSetTitle)
EVT_MENU(ID_VOLUME, GOAppWindow::OnSettingsVolume)
EVT_MENU(ID_POLYPHONY, GOAppWindow::OnSettingsPolyphony)
EVT_MENU(ID_MEMORY, GOAppWindow::OnSettingsMemoryEnter)
EVT_MENU(ID_TRANSPOSE, GOAppWindow::OnSettingsTranspose)
EVT_MENU(ID_RELEASELENGTH, GOAppWindow::OnSettingsReleaseLength)
EVT_MENU_RANGE(ID_PANEL_FIRST, ID_PANEL_LAST, GOAppWindow::OnPanel)
EVT_MENU_RANGE(ID_PRESET_0, ID_PRESET_LAST, GOAppWindow::OnPreset)
EVT_MENU_RANGE(
  ID_TEMPERAMENT_0, ID_TEMPERAMENT_LAST, GOAppWindow::OnTemperament)
EVT_SIZE(GOAppWindow::OnSize)
EVT_TEXT(ID_METER_TRANSPOSE_SPIN, GOAppWindow::OnSettingsTranspose)
EVT_TEXT_ENTER(ID_METER_TRANSPOSE_SPIN, GOAppWindow::OnSettingsTranspose)
EVT_COMMAND(
  ID_METER_TRANSPOSE_SPIN, wxEVT_SETVALUE, GOAppWindow::OnChangeTranspose)
EVT_CHOICE(ID_RELEASELENGTH_SELECT, GOAppWindow::OnSettingsReleaseLength)
EVT_TEXT(ID_METER_POLY_SPIN, GOAppWindow::OnSettingsPolyphony)
EVT_TEXT_ENTER(ID_METER_POLY_SPIN, GOAppWindow::OnSettingsPolyphony)
EVT_TEXT(ID_METER_FRAME_SPIN, GOAppWindow::OnSettingsMemory)
EVT_TEXT_ENTER(ID_METER_FRAME_SPIN, GOAppWindow::OnSettingsMemoryEnter)
EVT_COMMAND(ID_METER_FRAME_SPIN, wxEVT_SETVALUE, GOAppWindow::OnChangeSetter)
EVT_SLIDER(ID_METER_FRAME_SPIN, GOAppWindow::OnChangeSetter)
EVT_TEXT(ID_METER_AUDIO_SPIN, GOAppWindow::OnSettingsVolume)
EVT_TEXT_ENTER(ID_METER_AUDIO_SPIN, GOAppWindow::OnSettingsVolume)
EVT_COMMAND(ID_METER_AUDIO_SPIN, wxEVT_SETVALUE, GOAppWindow::OnChangeVolume)
EVT_MENU(ID_CHECK_FOR_UPDATES, GOAppWindow::OnUpdateCheckingRequested)
EVT_MENU(ID_SHOW_RELEASE_CHANGELOG, GOAppWindow::OnNewReleaseInfoRequested)
EVT_MENU(ID_DOWNLOAD_NEW_RELEASE, GOAppWindow::OnNewReleaseDownload)
EVT_UPDATE_CHECKING_COMPLETION(GOAppWindow::OnUpdateCheckingCompletion)
EVT_UPDATE_UI_RANGE(
  ID_FILE_RELOAD, ID_AUDIO_MEMSET, GOAppWindow::OnUpdateLoaded)
EVT_UPDATE_UI_RANGE(ID_PRESET_0, ID_PRESET_LAST, GOAppWindow::OnUpdateLoaded)
END_EVENT_TABLE()

GOAppWindow::GOAppWindow(
  GOGuiApp &app,
  wxFrame *pParentFrame,
  wxWindowID id,
  const wxString &title,
  const wxPoint &pos,
  const wxSize &size,
  const long type,
  GOSoundSystem &sound)
  : wxFrame(pParentFrame, id, title, pos, size, type),
    GOHelpRequestor(this),
    r_app(app),
    r_config(sound.GetSettings()),
    r_SoundSystem(sound),
    r_MidiSystem(sound.GetMidi()),
    m_file_menu(NULL),
    m_panel_menu(NULL),
    m_favorites_menu(NULL),
    m_recent_menu(NULL),
    p_OrganController(nullptr),
    m_SamplerUsage(NULL),
    m_VolumeControl(NULL),
    m_VolumeGauge(),
    m_Transpose(NULL),
    m_ReleaseLength(NULL),
    m_Polyphony(NULL),
    m_SetterPosition(NULL),
    m_Volume(NULL),
    m_listener(),
    m_Title(title),
    m_Label(),
    m_MidiMonitor(false),
    m_isMeterReady(false),
    m_IsGuiOnly(false),
    m_InSettings(false),
    m_AfterSettingsEventType(wxEVT_NULL),
    m_AfterSettingsEventId(0),
    p_AfterSettingsEventOrgan(NULL) {
  SetIcon(get_go_icon());

  wxArrayString choices;

  m_file_menu = new wxMenu;

  m_favorites_menu = new wxMenu;

  m_recent_menu = new wxMenu;

  wxMenu *preset_menu = new wxMenu;
  for (unsigned i = ID_PRESET_0; i <= ID_PRESET_LAST; i++)
    preset_menu->Append(
      i,
      wxString::Format(_("Preset %d"), i - ID_PRESET_0),
      wxEmptyString,
      wxITEM_CHECK);

  m_file_menu->Append(
    ID_FILE_LOAD, _("&Load\tCtrl+L"), wxEmptyString, wxITEM_NORMAL);
  m_file_menu->Append(wxID_ANY, _("&Favorites"), m_favorites_menu);
  m_file_menu->Append(
    ID_FILE_OPEN, _("&Open\tCtrl+O"), wxEmptyString, wxITEM_NORMAL);
  m_file_menu->Append(wxID_ANY, _("Open &Recent"), m_recent_menu);
  m_file_menu->Append(
    ID_FILE_INSTALL,
    _("&Install organ package\tCtrl+I"),
    wxEmptyString,
    wxITEM_NORMAL);
  m_file_menu->AppendSeparator();
  m_file_menu->Append(
    ID_FILE_PROPERTIES, _("Organ &Properties"), wxEmptyString, wxITEM_NORMAL);
  m_file_menu->AppendSeparator();
  m_file_menu->AppendSubMenu(preset_menu, _("Pr&eset"));
  m_file_menu->AppendSeparator();
  m_file_menu->Append(
    ID_FILE_SAVE, _("&Save\tCtrl+S"), wxEmptyString, wxITEM_NORMAL);
  m_file_menu->Append(
    ID_FILE_CACHE, _("&Update Cache..."), wxEmptyString, wxITEM_NORMAL);
  m_file_menu->Append(
    ID_FILE_CACHE_DELETE, _("Delete &Cache..."), wxEmptyString, wxITEM_NORMAL);
  m_file_menu->AppendSeparator();
  m_file_menu->Append(
    ID_FILE_RELOAD, _("Re&load"), wxEmptyString, wxITEM_NORMAL);
  m_file_menu->Append(
    ID_FILE_REVERT, _("Reset to &Defaults"), wxEmptyString, wxITEM_NORMAL);
  m_file_menu->AppendSeparator();
  m_file_menu->Append(
    ID_FILE_IMPORT_COMBINATIONS,
    _("Import &Combinations"),
    wxEmptyString,
    wxITEM_NORMAL);
  m_file_menu->Append(
    ID_FILE_EXPORT_COMBINATIONS,
    _("Export &Combinations"),
    wxEmptyString,
    wxITEM_NORMAL);
  m_file_menu->Append(
    ID_FILE_IMPORT_SETTINGS,
    _("&Import Settings"),
    wxEmptyString,
    wxITEM_NORMAL);
  m_file_menu->Append(
    ID_FILE_EXPORT, _("&Export Settings"), wxEmptyString, wxITEM_NORMAL);
  m_file_menu->AppendSeparator();
  m_file_menu->Append(
    ID_SETTINGS, wxT("&Settings..."), wxEmptyString, wxITEM_NORMAL);
  m_file_menu->AppendSeparator();
  m_file_menu->Append(ID_FILE_CLOSE, _("&Close"), wxEmptyString, wxITEM_NORMAL);
  m_file_menu->Append(ID_FILE_EXIT, _("E&xit"), wxEmptyString, wxITEM_NORMAL);

  m_temperament_menu = new wxMenu;

  m_audio_menu = new wxMenu;
  m_audio_menu->AppendSubMenu(m_temperament_menu, _("&Temperament"));
  m_audio_menu->Append(
    ID_ORGAN_EDIT, _("&Organ settings"), wxEmptyString, wxITEM_CHECK);
  m_audio_menu->Append(
    ID_MIDI_LIST, _("M&idi Objects"), wxEmptyString, wxITEM_CHECK);
  m_audio_menu->Append(ID_STOPS, _("Stops"), wxEmptyString, wxITEM_CHECK);
  m_audio_menu->AppendSeparator();
  m_audio_menu->Append(
    ID_AUDIO_STATE, _("&Sound Output State"), wxEmptyString, wxITEM_NORMAL);
  m_audio_menu->Append(
    ID_AUDIO_MONITOR, _("Audio &Monitor..."), wxEmptyString, wxITEM_NORMAL);
  m_audio_menu->AppendSeparator();
  m_audio_menu->Append(
    ID_AUDIO_PANIC, _("&Panic\tEscape"), wxEmptyString, wxITEM_NORMAL);
  m_audio_menu->Append(
    ID_AUDIO_MEMSET, _("&Memory Set\tShift"), wxEmptyString, wxITEM_CHECK);
  m_audio_menu->AppendSeparator();
  m_audio_menu->Append(
    ID_MIDI_LOAD, _("Load &MIDI\tCtrl+P"), wxEmptyString, wxITEM_NORMAL);
  m_autoMidiMenu = new wxMenu;
  m_audio_menu->AppendSubMenu(m_autoMidiMenu, _("&Recently Played"));
  m_audio_menu->Append(
    ID_MIDI_MONITOR, _("&Log MIDI events"), wxEmptyString, wxITEM_CHECK);

  m_crossfade_menu = new wxMenu;
  m_crossfade_menu->AppendRadioItem(ID_Crossfade_Linear, _("Linear\tF7"));
  m_crossfade_menu->AppendRadioItem(
    ID_Crossfade_SinEq, _("Sinus (equal power)\tF8"));
  m_crossfade_menu->AppendRadioItem(ID_Crossfade_Sin2, _("Sin^2\tF9"));
  m_crossfade_menu->AppendRadioItem(
    ID_Crossfade_SqrtEq, _("Sqrt (equal power)\tF10"));
  m_crossfade_menu->AppendRadioItem(ID_Crossfade_X2, _("x^2\tF11"));
  m_crossfade_menu->AppendRadioItem(
    ID_Crossfade_Custom, _("Custom (Placeholder)\tF12"));
  m_audio_menu->AppendSubMenu(m_crossfade_menu, _("&Crossfade"));

  m_releasealign_menu = new wxMenu;
  m_releasealign_menu->AppendRadioItem(
    ID_ReleaseAlign_Legacy, _("Legacy (instantaneous values)\tF2"));
  m_releasealign_menu->AppendRadioItem(
    ID_ReleaseAlign_Correlation, _("Correlation (new)\tF3"));
  m_releasealign_menu->AppendSeparator();
  m_releasealign_menu->Append(
    ID_LUT_CACHE_GENERATE,
    _("Generate LUT Cache..."),
    wxEmptyString,
    wxITEM_NORMAL);
  m_releasealign_menu->Append(
    ID_LUT_CACHE_DELETE, _("Delete LUT Cache"), wxEmptyString, wxITEM_NORMAL);
  m_audio_menu->AppendSubMenu(m_releasealign_menu, _("&Release Alignment"));

  {
    using namespace GOAudioParams;
    if (GetReleaseAlignMode() == GOReleaseAlignMode::Correlation)
      m_releasealign_menu->Check(ID_ReleaseAlign_Correlation, true);
    else
      m_releasealign_menu->Check(ID_ReleaseAlign_Legacy, true);
  }

  // Mark the menu radio item that matches the current runtime crossfade mode
  // (uses GOAudioParams::GetCrossfadeMode())
  {
    using namespace GOAudioParams;
    switch (GetCrossfadeMode()) {
    case GOCrossfadeMode::Linear:
      m_crossfade_menu->Check(ID_Crossfade_Linear, true);
      break;
    case GOCrossfadeMode::SinEqualPower:
      m_crossfade_menu->Check(ID_Crossfade_SinEq, true);
      break;
    case GOCrossfadeMode::Sin2:
      m_crossfade_menu->Check(ID_Crossfade_Sin2, true);
      break;
    case GOCrossfadeMode::SqrtEqualPower:
      m_crossfade_menu->Check(ID_Crossfade_SqrtEq, true);
      break;
    case GOCrossfadeMode::X2:
      m_crossfade_menu->Check(ID_Crossfade_X2, true);
      break;
    case GOCrossfadeMode::Custom:
      m_crossfade_menu->Check(ID_Crossfade_Custom, true);
      break;
    default:
      m_crossfade_menu->Check(ID_Crossfade_SinEq, true);
      break;
    }
  }

  wxMenu *help_menu = new wxMenu;
  help_menu->Append(wxID_HELP, _("&Help\tF1"), wxEmptyString, wxITEM_NORMAL);
  help_menu->Append(wxID_ABOUT, _("&About"), wxEmptyString, wxITEM_NORMAL);
  help_menu->Append(
    ID_CHECK_FOR_UPDATES,
    _("&Check for updates"),
    wxEmptyString,
    wxITEM_NORMAL);
  m_panel_menu = new wxMenu();

  wxMenuBar *menu_bar = new wxMenuBar;
  menu_bar->Append(m_file_menu, _("&File"));
  menu_bar->Append(m_audio_menu, _("&Audio/MIDI"));
  menu_bar->Append(m_panel_menu, _("&Panel"));
  menu_bar->Append(help_menu, _("&Help"));
  SetMenuBar(menu_bar);

  m_ToolBar = CreateToolBar(wxNO_BORDER | wxTB_HORIZONTAL | wxTB_FLAT);
  m_ToolBar->SetToolBitmapSize(wxSize(16, 16));
  m_ToolBar->AddTool(
    ID_AUDIO_MEMSET,
    _("&Memory Set\tShift"),
    GetImage_set(),
    _("Memory Set"),
    wxITEM_CHECK);
  m_ToolBar->AddTool(
    ID_MEMORY,
    _("&Memory Level"),
    GetImage_memory(),
    _("Memory Level"),
    wxITEM_NORMAL);
  m_SetterPosition = new wxSpinCtrl(
    m_ToolBar,
    ID_METER_FRAME_SPIN,
    wxEmptyString,
    wxDefaultPosition,
    wxSize(50, wxDefaultCoord),
    wxSP_ARROW_KEYS,
    0,
    999);
  m_ToolBar->AddControl(m_SetterPosition);
  m_SetterPosition->SetValue(0);

  m_ToolBar->AddTool(
    ID_VOLUME, _("&Volume"), GetImage_volume(), _("Volume"), wxITEM_NORMAL);
  m_Volume = new wxSpinCtrl(
    m_ToolBar,
    ID_METER_AUDIO_SPIN,
    wxEmptyString,
    wxDefaultPosition,
    wxSize(50, wxDefaultCoord),
    wxSP_ARROW_KEYS,
    -120,
    20);
  m_ToolBar->AddControl(m_Volume);
  m_VolumeControl = new wxControl(m_ToolBar, wxID_ANY);
  m_VolumeControlTool = m_ToolBar->AddControl(m_VolumeControl);
  AdjustVolumeControlWithSettings();
  m_Volume->SetValue(r_config.Volume());

  m_ToolBar->AddTool(
    ID_RELEASELENGTH,
    _("&Release tail length"),
    GetImage_reverb(),
    _("Release tail length"),
    wxITEM_NORMAL);
  choices.clear();
  choices.push_back(_("Max"));
  for (unsigned i = 1; i <= 60; i++)
    choices.push_back(wxString::Format(_("%d ms"), i * 50));
  m_ReleaseLength = new wxChoice(
    m_ToolBar,
    ID_RELEASELENGTH_SELECT,
    wxDefaultPosition,
    wxDefaultSize,
    choices);
  m_ToolBar->AddControl(m_ReleaseLength);
  UpdateReleaseLength(0);

  m_ToolBar->AddTool(
    ID_TRANSPOSE,
    _("&Transpose"),
    GetImage_transpose(),
    _("Transpose"),
    wxITEM_NORMAL);
  m_Transpose = new wxSpinCtrl(
    m_ToolBar,
    ID_METER_TRANSPOSE_SPIN,
    wxEmptyString,
    wxDefaultPosition,
    wxSize(46, wxDefaultCoord),
    wxSP_ARROW_KEYS,
    -11,
    11);
  m_ToolBar->AddControl(m_Transpose);
  m_Transpose->SetValue(r_config.Transpose());

  m_ToolBar->AddTool(
    ID_POLYPHONY,
    _("&Polyphony"),
    GetImage_polyphony(),
    _("Polyphony"),
    wxITEM_NORMAL);
  m_Polyphony = new wxSpinCtrl(
    m_ToolBar,
    ID_METER_POLY_SPIN,
    wxEmptyString,
    wxDefaultPosition,
    wxSize(62, wxDefaultCoord),
    wxSP_ARROW_KEYS,
    1,
    MAX_POLYPHONY);
  m_ToolBar->AddControl(m_Polyphony);

  m_SamplerUsage = new GOAudioGauge(m_ToolBar, wxID_ANY, wxDefaultPosition);
  m_ToolBar->AddControl(m_SamplerUsage);
  m_Polyphony->SetValue(r_config.PolyphonyLimit());

  m_ToolBar->AddTool(
    ID_AUDIO_PANIC,
    _("&Panic\tEscape"),
    GetImage_panic(),
    _("Panic"),
    wxITEM_NORMAL);

  SetAutoLayout(true);

  UpdateSize();

  // Beim START nur die Groesse - den Bildschirm waehlt der
  // Fenstermanager (OnPlaceTimer zieht danach die Ecke nach).  Der
  // Orgel-Ladepfad ruft dagegen SetPosSize MIT Lage auf: jede Orgel
  // merkt sich ihre eigene Hauptfensterlage.
  SetStartupSize(r_config.GetMainWindowRect());

  m_listener.Register(&r_MidiSystem);
  m_isMeterReady = true;

  // NACH dem Anzeigen des Hauptfensters, nicht hier im Konstruktor:
  // ein Kindfenster, dessen Eltern noch nicht auf dem Bildschirm sind,
  // hat keine Eltern-Geometrie zum Anlehnen - der Fenstermanager
  // platzierte den Monitor dann nach eigenem Gutduenken (bei
  // Mehrschirm-Betrieb regelmaessig auf dem falschen).  CallAfter
  // stellt den Aufruf in die Ereignisschleife, die erst nach Show()
  // des Rahmens laeuft, so dass der Monitor beim Hauptfenster landet -
  // genau wie ein spaeter von Hand geoeffneter.
  // Startlage: obere linke Ecke - aber auf DEM Bildschirm, den der
  // Fenstermanager gewaehlt hat (das ist der des Aufrufs).  Zwei
  // Fehlversuche stecken in dieser Zeile: ein CallAfter direkt nach dem
  // Anzeigen kam zu FRUEH (der Fenstermanager platziert beim Einblenden
  // und ueberschrieb es), und die Mausposition VOR dem Anzeigen ist
  // unter GTK unzuverlaessig - schlug sie fehl, landete alles auf
  // Display 0, also immer demselben Schirm.
  //
  // Deshalb: beim ersten Leerlauf, in dem das Fenster WIRKLICH auf dem
  // Schirm ist (IsShownOnScreen), seinen Bildschirm erfragen und dort
  // nach oben links schieben - danach die Behandlung wieder abmelden,
  // damit spaetere Verschiebungen des Nutzers bleiben.
  // Ecke nachziehen, sobald der Fenstermanager platziert hat: er waehlt
  // den Bildschirm (richtig), wir schieben nur nach oben links.  Der
  // Zeitgeber wartet auf ein Fenster, das sichtbar ist UND eine echte
  // Position gemeldet hat - eine Korrektur bei noch gemeldeten 0,0 war
  // ein Nulleingriff, nach dem KWin in Ruhe zentrierte.
  m_PlaceTimer.SetOwner(this, ID_PLACE_TIMER);
  m_PlaceTimer.Start(20, wxTIMER_CONTINUOUS);
  if (r_config.AudioMonitorOpen())
    CallAfter([this]() { ShowAudioMonitor(); });
}

GOAppWindow::~GOAppWindow() {
  m_isMeterReady = false;
  m_listener.SetCallback(NULL);
}

bool GOAppWindow::AdjustVolumeControlWithSettings() {
  const unsigned count = r_config.GetTotalAudioChannels();
  bool rc = false;

  if (count != m_VolumeGauge.size()) {
    const wxPortId portId = wxPlatformInfo::Get().GetPortId();
    const bool isOsX = portId == wxPORT_COCOA || portId == wxPORT_OSX;
    int volCtlId = m_VolumeControlTool->GetId();
    int volCtlPos = m_ToolBar->GetToolPos(volCtlId);

    // OsX doesn't relayout the toolbar correctly after changing the size of
    // a control so we need to remove it and to reinsert it later
    // But RemoveTool hangs on windows, so we do it only on OsX
    if (isOsX)
      m_ToolBar->RemoveTool(volCtlId);

    m_VolumeGauge.clear();
    /* Release the old sizer BEFORE destroying the windows it refers to.
     *
     * The other way round is a use-after-free: DestroyChildren() deletes the
     * gauges immediately (they are not top-level windows), while the old
     * sizer still holds pointers to them - and when it is finally deleted,
     * by the SetSizer() below, every wxSizerItem writes to its window one
     * last time (SetContainingSizer(NULL)). That write lands in freed
     * memory. SetSizer(NULL) deletes the old sizer while its children are
     * still alive, so the pointers it lets go of are all valid.
     *
     * Reached whenever the configured channel count changes, i.e. from the
     * audio settings - not on the first build, where there is neither an old
     * sizer nor children yet. */
    m_VolumeControl->SetSizer(NULL);
    m_VolumeControl->DestroyChildren();
    wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *vsizer = NULL;

    for (unsigned i = 0; i < count; i++) {
      if ((i % 2) == 0) {
        vsizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(vsizer);
      }
      GOAudioGauge *gauge
        = new GOAudioGauge(m_VolumeControl, wxID_ANY, wxDefaultPosition);
      m_VolumeGauge.push_back(gauge);
      vsizer->Add(gauge, 0, wxFIXED_MINSIZE);
    }

    m_VolumeControl->SetSizer(sizer);
    sizer->Fit(m_VolumeControl);

    // reinsert the control and relayout the toolbar on OsX
    if (isOsX)
      m_ToolBar->InsertTool(volCtlPos, m_VolumeControlTool);
    m_ToolBar->Realize();
    rc = true;
  }
  return rc;
}

void GOAppWindow::UpdateSize() {
  wxToolBar *tb = GetToolBar();

  tb->Realize();

  const wxSize bestTbSize(tb->GetBestSize());
  const int bestClientWidth = bestTbSize.GetWidth() + 10;

  SetClientSize(bestClientWidth, 0);

  const wxSize frameSize(GetSize());

  SetMinSize(wxSize(frameSize.GetWidth() / 4, frameSize.GetHeight()));
  SetMaxSize(wxSize(frameSize.GetWidth() * 4, frameSize.GetHeight()));
}

GOLogicalRect GOAppWindow::GetPosSize() const {
  GOLogicalRect lRect;
  const wxRect gRect(GetRect());
  // Unter Wayland ist gRect.x/y erfunden (siehe GOWindowPlacement.h): die
  // gemerkte Lage durchreichen, damit Beenden unter Wayland die unter X11
  // eingerichteten Koordinaten nicht ueberschreibt.  Beide Aufrufer - die
  // Programmkonfiguration und die pro Orgel gespeicherte Lage - gehen hier
  // durch, deshalb steht die Absicherung an dieser Stelle.
  const bool isPosReal = GOCanPlaceWindows();

  lRect.x = isPosReal ? gRect.x : m_KeptPos.x;
  lRect.y = isPosReal ? gRect.y : m_KeptPos.y;
  lRect.width = gRect.width;
  lRect.height = gRect.height;
  return lRect;
}

// Zeigerposition beim Programmstart; wxDefaultPosition = nicht gesetzt.
static wxPoint s_StartupPointer = wxDefaultPosition;

void GOAppWindow::SetStartupPointer(const wxPoint &pt) {
  s_StartupPointer = pt;
}

void GOAppWindow::SetPosSize(const GOLogicalRect &lRect) {
  m_KeptPos = wxPoint(lRect.x, lRect.y);
  if (lRect.width && lRect.height) { // settings are valid
    wxRect rect(lRect.x, lRect.y, lRect.width, lRect.height);
    const wxRect minSize(GetMinSize());
    const wxRect maxSize(GetMaxSize());

    if (rect.width < minSize.width)
      rect.width = minSize.width;
    if (rect.width > maxSize.width)
      rect.width = maxSize.width;
    if (rect.height < minSize.height)
      rect.height = minSize.height;
    if (rect.height > maxSize.height)
      rect.height = maxSize.height;
    // Die uebergebene Lage GILT - hier kommt die Orgel her, und ihre
    // gespeicherte Hauptfensterlage hat Vorrang vor der
    // Start-Ecke; deshalb auch den Zeitgeber stoppen, sonst zoege er
    // das Fenster gleich danach in die Ecke zurueck.
    m_PlaceTimer.Stop();
    SetSize(rect);
  }
}

void GOAppWindow::SetStartupSize(const GOLogicalRect &lRect) {
  // Die Lage wird hier bewusst NICHT angewandt, aber gemerkt: unter Wayland
  // ist sie beim Beenden das einzige, was GO ueber seine eigene Position
  // weiss, und nur so ueberlebt sie einen Wayland-Lauf unbeschadet.
  m_KeptPos = wxPoint(lRect.x, lRect.y);
  if (!lRect.width || !lRect.height)
    return; // nichts gespeichert - der Fenstermanager macht alles

  wxRect rect(lRect.x, lRect.y, lRect.width, lRect.height);
  const wxRect minSize(GetMinSize());
  const wxRect maxSize(GetMaxSize());

  if (rect.width < minSize.width)
    rect.width = minSize.width;
  if (rect.width > maxSize.width)
    rect.width = maxSize.width;
  if (rect.height < minSize.height)
    rect.height = minSize.height;
  if (rect.height > maxSize.height)
    rect.height = maxSize.height;
  // Ohne Position: den BILDSCHIRM kennt nur der Fenstermanager.  Unter
  // Wayland sieht eine XWayland-Anwendung wie GO die Leiste gar nicht,
  // auf der geklickt wurde, und der abgefragte Zeiger friert auf seinem
  // letzten X11-Wert ein.  Die gespeicherte Ecke taugt ebensowenig: GO
  // sichert beim Beenden und startet dort wieder - Selbstverstaerkung.
  SetSize(rect.width, rect.height);
}

void GOAppWindow::UpdateReleaseLength(unsigned releaseLength) {
  int releaseLengthIndex = releaseLength / 50;

  if (p_OrganController && p_OrganController->GetReleaseTail() != releaseLength)
    p_OrganController->SetReleaseTail(releaseLength);
  if (m_ReleaseLength->GetSelection() != releaseLengthIndex)
    m_ReleaseLength->SetSelection(releaseLengthIndex);
}

void GOAppWindow::UpdateVolumeControlWithSettings() {
  m_isMeterReady = false;
  if (AdjustVolumeControlWithSettings())
    UpdateSize();
  m_isMeterReady = true;
}

void GOAppWindow::Init(const wxString &filename, bool isGuiOnly) {
  if (r_config.CheckForUpdatesAtStartup()) {
    // Start update checker thread that will fire events to this frame
    m_UpdateCheckerThread = GOUpdateChecker::StartThread(
      GOUpdateChecker::CheckReason::STARTUP, this);
  }

  m_IsGuiOnly = isGuiOnly;
  Show(true);

  SettingsReasons settingsReasons;

  r_SoundSystem.SetLogSoundErrorMessages(false);

  bool soundProblems = !r_SoundSystem.AssureSoundIsOpen();

  if (soundProblems)
    settingsReasons.push_back(GOSettingsReason(
      r_SoundSystem.getLastErrorMessage(), GOSettingsDialog::PAGE_AUDIO));
  r_SoundSystem.SetLogSoundErrorMessages(true);

  bool midiProblems
    = !r_MidiSystem.HasActiveDevice() && r_config.IsToCheckMidiOnStart();

  if (!soundProblems && midiProblems)
    settingsReasons.push_back(GOSettingsReason(
      _("No active MIDI input devices"), GOSettingsDialog::PAGE_MIDI_DEVICES));

  if (soundProblems || midiProblems) {
    wxCommandEvent event(wxEVT_COMMAND_MENU_SELECTED, ID_SETTINGS);

    SettingsReasons *const pReasons = new SettingsReasons(settingsReasons);

    event.SetClientData(pReasons);
    GetEventHandler()->AddPendingEvent(event);
  }

  GOArchiveManager manager(r_config, r_config.OrganCachePath());

  manager.RegisterPackageDirectory(r_config.GetPackageDirectory());
  manager.RegisterPackageDirectory(r_config.OrganPackagePath());

  // Remove demo organs that have been registered from temporary (appimage)
  // directories and they are not more valid
  r_config.AddOrgansFromArchives();
  r_config.RemoveInvalidTmpOrgans();

  if (!filename.IsEmpty())
    SendLoadFile(filename);
  else
    switch (r_config.LoadLastFile()) {
    case GOInitialLoadType::LOAD_LAST_USED:
      LoadLastOrgan();
      break;

    case GOInitialLoadType::LOAD_FIRST:
      LoadFirstOrgan();
      break;

    case GOInitialLoadType::LOAD_NONE:
      break;
    }

  m_listener.SetCallback(this);
  GOCacheCleaner clean(r_config);
  clean.Cleanup();
}

void GOAppWindow::AttachDetachOrganController(bool isToAttach) {
  if (p_OrganController) {
    p_OrganController->SetModificationListener(isToAttach ? this : nullptr);

    if (isToAttach && m_releasealign_menu) {
      using namespace GOAudioParams;
      if (GetReleaseAlignMode() == GOReleaseAlignMode::Correlation)
        m_releasealign_menu->Check(ID_ReleaseAlign_Correlation, true);
      else
        m_releasealign_menu->Check(ID_ReleaseAlign_Legacy, true);
    }

    // Sync crossfade menu with current runtime value when attaching an organ
    if (isToAttach && m_crossfade_menu) {
      using namespace GOAudioParams;
      switch (GetCrossfadeMode()) {
      case GOCrossfadeMode::Linear:
        m_crossfade_menu->Check(ID_Crossfade_Linear, true);
        break;
      case GOCrossfadeMode::SinEqualPower:
        m_crossfade_menu->Check(ID_Crossfade_SinEq, true);
        break;
      case GOCrossfadeMode::Sin2:
        m_crossfade_menu->Check(ID_Crossfade_Sin2, true);
        break;
      case GOCrossfadeMode::SqrtEqualPower:
        m_crossfade_menu->Check(ID_Crossfade_SqrtEq, true);
        break;
      case GOCrossfadeMode::X2:
        m_crossfade_menu->Check(ID_Crossfade_X2, true);
        break;
      case GOCrossfadeMode::Custom:
        m_crossfade_menu->Check(ID_Crossfade_Custom, true);
        break;
      default:
        m_crossfade_menu->Check(ID_Crossfade_SinEq, true);
        break;
      }
    }
  }
}

bool GOAppWindow::CloseOrgan(bool isForce, bool isDiscardChanges) {
  bool isClosed = true;

  if (mp_organ) {
    if (mp_organ->IsModified() && !isDiscardChanges) {
      int choice = isForce ? wxYES
                           : wxMessageBox(
                             _("The organ settings have been modified\n"
                               "Do you want to save them?"),
                             _("Save organ settings"),
                             wxYES_NO | wxCANCEL | wxCENTRE,
                             this);

      switch (choice) {
      case wxYES:
        isClosed = mp_organ->Save();
        break;
      case wxCANCEL:
        isClosed = false;
        break;
      }
    }

    if (isClosed) {
      GOMutexLocker m_locker(m_mutex, true);

      if (m_locker.IsLocked()) {
        AttachDetachOrganController(false);
        p_OrganController = nullptr;
        mp_organ.reset();
        UpdatePanelMenu();
      } else
        isClosed = false;
    }
  }
  return isClosed;
}

void GOAppWindow::LoadOrgan(const GOOrgan &organ, const wxString &cmb) {
  if (mp_organ) {
    GOProgressDialog dlg(this);

    // mp_organ->LoadOrgan() deletes the old controller right away, but only
    // returns the new one when the (long) load is done. The progress dialog
    // yields events in between, so anything polling p_OrganController during
    // the load (e.g. the audio monitor's timer) must see nullptr instead of
    // the dangling old pointer. Same detach/clear pair as CloseOrgan().
    AttachDetachOrganController(false);
    p_OrganController = nullptr;

    p_OrganController = mp_organ->LoadOrgan(organ, cmb, m_IsGuiOnly, dlg);
    OnIsModifiedChanged(false);

    // for reflecting model changes
    AttachDetachOrganController(true);
  }
}

void GOAppWindow::Open(const GOOrgan &organ) {
  if (CloseOrgan(false)) {
    GOMutexLocker m_locker(m_mutex, true);

    if (m_locker.IsLocked()) {
      mp_organ = std::make_unique<GOGuiOrgan>(this, &r_SoundSystem);
      LoadOrgan(organ);
    }
  }
}

void GOAppWindow::OnPanel(wxCommandEvent &event) {
  unsigned no = event.GetId() - ID_PANEL_FIRST;

  if (p_OrganController && no < p_OrganController->GetPanelCount())
    mp_organ->ShowPanel(no);
}

void GOAppWindow::OnIsModifiedChanged(bool modified) {
  if (p_OrganController)
    UpdateReleaseLength(p_OrganController->GetReleaseTail());
  UpdatePanelMenu();
}

void GOAppWindow::UpdatePanelMenu() {
  unsigned panelcount
    = (p_OrganController && p_OrganController->GetPanelCount())
    ? p_OrganController->GetPanelCount()
    : 0;
  panelcount = std::min(panelcount, (unsigned)(ID_PANEL_LAST - ID_PANEL_FIRST));

  while (m_panel_menu->GetMenuItemCount() > 0)
    m_panel_menu->Destroy(
      m_panel_menu->FindItemByPosition(m_panel_menu->GetMenuItemCount() - 1));

  for (unsigned i = 0; i < panelcount; i++) {
    GOGUIPanel *panel = p_OrganController->GetPanel(i);
    wxMenu *menu = NULL;
    if (panel->GetGroupName() == wxEmptyString)
      menu = m_panel_menu;
    else {
      for (unsigned j = 0; j < m_panel_menu->GetMenuItemCount(); j++) {
        wxMenuItem *it = m_panel_menu->FindItemByPosition(j);
        if (it->GetItemLabel() == panel->GetGroupName() && it->GetSubMenu())
          menu = it->GetSubMenu();
      }
      if (!menu) {
        menu = new wxMenu();
        m_panel_menu->AppendSubMenu(menu, panel->GetGroupName());
      }
    }
    wxMenuItem *item = menu->AppendCheckItem(ID_PANEL_FIRST + i, wxT("_"));
    item->SetItemLabel(panel->GetName());
    item->Check(
      mp_organ->WindowExists(GOGuiOrgan::PANEL, panel) ? true : false);
  }
}

void GOAppWindow::UpdateFavoritesMenu() {
  while (m_favorites_menu->GetMenuItemCount() > 0)
    m_favorites_menu->Destroy(m_favorites_menu->FindItemByPosition(
      m_favorites_menu->GetMenuItemCount() - 1));

  const ptr_vector<GOOrgan> &organs = r_config.GetOrganList();
  for (unsigned i = 0;
       i < organs.size() && i <= ID_LOAD_FAV_LAST - ID_LOAD_FAV_FIRST;
       i++) {
    m_favorites_menu->AppendCheckItem(
      ID_LOAD_FAV_FIRST + i,
      wxString::Format(
        _("&%d: %s"), (i + 1) % 10, organs[i]->GetUITitle().c_str()));
  }
}

void GOAppWindow::UpdateRecentMenu() {
  while (m_recent_menu->GetMenuItemCount() > 0)
    m_recent_menu->Destroy(
      m_recent_menu->FindItemByPosition(m_recent_menu->GetMenuItemCount() - 1));

  std::vector<const GOOrgan *> organs = r_config.GetLRUOrganList();
  for (unsigned i = 0;
       i < organs.size() && i <= ID_LOAD_LRU_LAST - ID_LOAD_LRU_FIRST;
       i++) {
    m_recent_menu->AppendCheckItem(
      ID_LOAD_LRU_FIRST + i,
      wxString::Format(
        _("&%d: %s"), (i + 1) % 10, organs[i]->GetUITitle().c_str()));
  }
}

void GOAppWindow::UpdateTemperamentMenu() {
  wxString temperament = wxEmptyString;
  if (p_OrganController)
    temperament = p_OrganController->GetTemperament();

  while (m_temperament_menu->GetMenuItemCount() > 0)
    m_temperament_menu->Destroy(m_temperament_menu->FindItemByPosition(
      m_temperament_menu->GetMenuItemCount() - 1));

  for (unsigned i = 0; i < r_config.GetTemperaments().GetTemperamentCount()
       && i < ID_TEMPERAMENT_LAST - ID_TEMPERAMENT_0;
       i++) {
    const GOTemperament &t = r_config.GetTemperaments().GetTemperament(i);
    wxMenu *menu;
    wxString group = t.GetGroupTitle();
    if (group == wxEmptyString)
      menu = m_temperament_menu;
    else {
      menu = NULL;
      for (unsigned j = 0; j < m_temperament_menu->GetMenuItemCount(); j++) {
        wxMenuItem *it = m_temperament_menu->FindItemByPosition(j);
        if (it->GetItemLabel() == group && it->GetSubMenu())
          menu = it->GetSubMenu();
      }
      if (!menu) {
        menu = new wxMenu();
        m_temperament_menu->AppendSubMenu(menu, group);
      }
    }
    wxMenuItem *e = menu->Append(
      ID_TEMPERAMENT_0 + i, t.GetTitle(), wxEmptyString, wxITEM_CHECK);
    e->Enable(p_OrganController);
    e->Check(t.GetName() == temperament);
  }
}

void GOAppWindow::OnSize(wxSizeEvent &event) {
  wxWindow *child = (wxWindow *)NULL;
  for (wxWindowList::compatibility_iterator node = GetChildren().GetFirst();
       node;
       node = node->GetNext()) {
    wxWindow *win = node->GetData();
    if (!win->IsTopLevel() && !IsOneOfBars(win)) {
      child = win;
    }
  }
  if (child)
    child->SetSize(
      0, 0, GetClientSize().GetWidth(), GetClientSize().GetHeight());
}

void GOAppWindow::OnMeters(wxCommandEvent &event) {
  if (m_isMeterReady) {
    const std::vector<float> vals = r_SoundSystem.GetEngine().GetMeterInfo();

    if (vals.size() == m_VolumeGauge.size() + 1) {
      m_SamplerUsage->SetValue(33 * vals[0]);
      for (unsigned i = 1; i < vals.size(); i++)
        m_VolumeGauge[i - 1]->SetValue(lrint(32.50000000000001 * vals[i]));
      if (event.GetInt()) {
        for (unsigned i = 0; i < m_VolumeGauge.size(); i++)
          m_VolumeGauge[i]->ResetClip();
        m_SamplerUsage->ResetClip();
      }
    }
  }
}

void GOAppWindow::OnUpdateLoaded(wxUpdateUIEvent &event) {
  if (ID_PRESET_0 <= event.GetId() && event.GetId() <= ID_PRESET_LAST) {
    event.Check(r_config.Preset() == (unsigned)(event.GetId() - ID_PRESET_0));
    return;
  }

  if (event.GetId() == ID_AUDIO_MEMSET)
    event.Check(
      p_OrganController && p_OrganController->GetSetter()
      && p_OrganController->GetSetter()->GetState().m_IsActive);
  else if (event.GetId() == ID_ORGAN_EDIT)
    event.Check(
      mp_organ && mp_organ->WindowExists(GOGuiOrgan::ORGAN_DIALOG, NULL));
  else if (event.GetId() == ID_MIDI_LIST)
    event.Check(
      mp_organ && mp_organ->WindowExists(GOGuiOrgan::MIDI_LIST, NULL));
  else if (event.GetId() == ID_STOPS)
    event.Check(mp_organ && mp_organ->WindowExists(GOGuiOrgan::STOPS, NULL));
  else if (event.GetId() == ID_MIDI_LIST)
    event.Check(m_MidiMonitor);

  if (event.GetId() == ID_FILE_CACHE_DELETE)
    event.Enable(p_OrganController && p_OrganController->CachePresent());
  else if (event.GetId() == ID_FILE_CACHE)
    event.Enable(p_OrganController && p_OrganController->IsCacheable());
  else if (event.GetId() == ID_MIDI_MONITOR)
    event.Enable(true);
  else
    event.Enable(
      p_OrganController
      && (event.GetId() == ID_FILE_REVERT ? p_OrganController->IsCustomized() : true));
}

void GOAppWindow::OnPreset(wxCommandEvent &event) {
  unsigned id = event.GetId() - ID_PRESET_0;
  if (id == r_config.Preset())
    return;
  r_config.Preset(id);
  if (mp_organ)
    ProcessCommand(ID_FILE_RELOAD);
}

void GOAppWindow::OnTemperament(wxCommandEvent &event) {
  unsigned id = event.GetId() - ID_TEMPERAMENT_0;

  if (
    p_OrganController && id < r_config.GetTemperaments().GetTemperamentCount())
    p_OrganController->SetTemperament(
      r_config.GetTemperaments().GetTemperament(id).GetName());
}

void GOAppWindow::OnLoadFile(wxCommandEvent &event) {
  GOOrgan *pOrgan = (GOOrgan *)event.GetClientData();

  if (!m_InSettings) {
    Open(*pOrgan);
    delete pOrgan;
  } else
    SetEventAfterSettings(event.GetEventType(), event.GetId(), pOrgan);
}

void GOAppWindow::OnLoadFavorite(wxCommandEvent &event) {
  unsigned id = event.GetId() - ID_LOAD_FAV_FIRST;
  const GOOrgan &organ = *r_config.GetOrganList()[id];
  Open(organ);
}

void GOAppWindow::OnLoadRecent(wxCommandEvent &event) {
  unsigned id = event.GetId() - ID_LOAD_LRU_FIRST;
  const GOOrgan &organ = *r_config.GetLRUOrganList()[id];
  Open(organ);
}

void GOAppWindow::OnLoad(wxCommandEvent &event) {
  GOSelectOrganDialog dlg(this, r_config);

  if (dlg.ShowModal() == wxID_OK)
    Open(*dlg.GetSelection());
}

void GOAppWindow::OnOpen(wxCommandEvent &event) {
  wxFileDialog dlg(
    this,
    _("Open organ"),
    r_config.OrganPath(),
    wxEmptyString,
    GOStdFileName::getOdfDlgWildcard(),
    wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (dlg.ShowModal() == wxID_OK) {
    Open(GOOrgan(dlg.GetPath()));
  }
}

void GOAppWindow::OnInstall(wxCommandEvent &event) {
  wxFileDialog dlg(
    this,
    _("Install organ package"),
    r_config.OrganPath(),
    wxEmptyString,
    GOStdFileName::getPackageDlgWildcard(),
    wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (dlg.ShowModal() == wxID_OK)
    if (InstallOrganPackage(dlg.GetPath())) {
      GOMessageBox(
        _("The organ package has been registered"),
        _("Install organ package"),
        wxOK,
        this);
      r_config.Flush();
    }
}

void GOAppWindow::OnImportCombinations(wxCommandEvent &event) {
  if (p_OrganController) {
    wxFileDialog dlg(
      this,
      _("Import Combinations"),
      p_OrganController->GetCombinationsDir(),
      wxEmptyString,
      _("Combinations files (*.yaml)|*.yaml|Settings files (*.cmb)|*.cmb"),
      wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dlg.ShowModal() == wxID_OK)
      p_OrganController->LoadCombination(dlg.GetPath());
  }
}

void GOAppWindow::OnExportCombinations(wxCommandEvent &event) {
  if (p_OrganController) {
    const wxString organCmbDir = p_OrganController->GetCombinationsDir();

    wxFileName::Mkdir(organCmbDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    wxFileDialog dlg(
      this,
      _("Export Combinations"),
      organCmbDir,
      wxEmptyString,
      _("Combination files (*.yaml)|*.yaml"),
      wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    if (dlg.ShowModal() == wxID_OK) {
      wxString exportedFilePath = dlg.GetPath();

      if (!exportedFilePath.EndsWith(wxT(".yaml"), NULL))
        exportedFilePath += wxT(".yaml");
      const wxString errMsg
        = p_OrganController->ExportCombination(exportedFilePath);

      if (!errMsg.IsEmpty())
        GOMessageBox(
          wxString::Format(
            _("Failed to export combinations to '%s': %s"),
            exportedFilePath,
            errMsg),
          _("Error"),
          wxOK | wxICON_ERROR,
          this);
    }
  }
}

void GOAppWindow::OnImportSettings(wxCommandEvent &event) {
  if (p_OrganController) {
    wxFileDialog dlg(
      this,
      _("Import Settings"),
      r_config.ExportImportPath(),
      wxEmptyString,
      _("Settings files (*.cmb)|*.cmb"),
      wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dlg.ShowModal() == wxID_OK) {
      GOOrgan organ = p_OrganController->GetOrganInfo();
      GOMutexLocker m_locker(m_mutex, true);

      if (m_locker.IsLocked()) {
        LoadOrgan(organ, dlg.GetPath());
      }
    }
  }
}

void GOAppWindow::OnExport(wxCommandEvent &event) {
  if (p_OrganController) {
    wxFileDialog dlg(
      this,
      _("Export Settings"),
      r_config.ExportImportPath(),
      wxEmptyString,
      _("Settings files (*.cmb)|*.cmb"),
      wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    if (dlg.ShowModal() == wxID_OK) {
      wxString exportedFilePath = dlg.GetPath();
      if (!exportedFilePath.EndsWith(wxT(".cmb"), NULL))
        exportedFilePath += wxT(".cmb");
      if (!mp_organ->Export(exportedFilePath))
        GOMessageBox(
          wxString::Format(
            _("Failed to export settings to '%s'"), exportedFilePath.c_str()),
          _("Error"),
          wxOK | wxICON_ERROR,
          this);
    }
  }
}

void GOAppWindow::OnSave(wxCommandEvent &event) {
  if (p_OrganController && !mp_organ->Save())
    GOMessageBox(
      _("Failed to save the organ setting"),
      _("Error"),
      wxOK | wxICON_ERROR,
      this);
}

wxString formatSize(wxLongLong &size) {
  double n = (double)size.ToLong();
  const wxString sizes[] = {
    wxTRANSLATE("KB"), wxTRANSLATE("MB"), wxTRANSLATE("GB"), wxTRANSLATE("TB")};
  int i;

  for (i = 0; i < 3; i++) {
    n /= 1024.0;
    if (n < 1024.0)
      break;
  }
  return wxString::Format(wxT("%.2f %s"), n, wxGetTranslation(sizes[i]));
}

void GOAppWindow::OnCache(wxCommandEvent &event) {
  bool res = true;
  GOMutexLocker m_locker(m_mutex, true);

  if (!m_locker.IsLocked())
    return;

  GOProgressDialog dlg(this);

  if (p_OrganController)
    res = p_OrganController->UpdateCache(r_config.CompressCache(), dlg);
  if (!res) {
    wxLogError(_("Creating the cache failed"));
    GOMessageBox(
      _("Creating the cache failed"), _("Error"), wxOK | wxICON_ERROR, this);
  }
}

void GOAppWindow::OnCacheDelete(wxCommandEvent &event) {
  if (p_OrganController)
    p_OrganController->DeleteCache();
}

bool GOAppWindow::BenchmarkSetBufferSize(unsigned samplesPerBuffer) {
  // Same recipe as applying changed audio settings (see OnSettings): the
  // reopen reads the config. The organ stays loaded; all voices die with
  // the device, which is exactly what the ceiling sweep wants.
  r_config.SamplesPerBuffer(samplesPerBuffer);
  r_SoundSystem.AssureSoundIsClosed();
  return r_SoundSystem.AssureSoundIsOpen();
}

void GOAppWindow::BenchmarkSetV3AndReload(bool useV3) {
  r_config.UseV3SampleCompression(useV3);
  if (p_OrganController) {
    GOOrgan organ = p_OrganController->GetOrganInfo();

    /* Discard unsaved organ-settings changes WITHOUT the "organ has been
     * modified - save?" prompt: a modal dialog would stall the automated
     * sweep indefinitely, and the modifications are almost always the
     * benchmark's own artifacts (all stops engaged etc.) - persisting
     * those into the .cmb would be worse. Anything the user wants kept
     * must be saved before starting the sweep. */
    /* Skip writing the sample cache for this load: the sweep switches
     * right back afterwards, so the (minutes-long, gigabytes-heavy)
     * cache write for the temporary codec would never be read. An
     * EXISTING cache is still used for loading - only the write is
     * suppressed. Restored immediately after the synchronous Open(). */
    const bool savedManageCache = r_config.ManageCache();

    r_config.ManageCache(false);
    if (CloseOrgan(false, true))
      Open(organ);
    r_config.ManageCache(savedManageCache);
  }
}

void GOAppWindow::OnReload(wxCommandEvent &event) {
  if (p_OrganController) {
    GOOrgan organ = p_OrganController->GetOrganInfo();

    if (CloseOrgan(false))
      Open(organ);
  }
}

void GOAppWindow::OnMenuClose(wxCommandEvent &event) {
  if (mp_organ)
    CloseOrgan(false);
}

bool GOAppWindow::CloseProgram(bool isForce) {
  if (m_UpdateCheckerThread) {
    // Stop update checking so that we can safely destruct the thread
    m_UpdateCheckerThread->Stop();
  }

  bool isClosed = CloseOrgan(isForce);

  if (isClosed) {
    /* Persist settings changed outside any dialog's OK path (e.g. the
     * audio-monitor open flag, set when the monitor is opened/closed).
     * Without this flush they only reached the disk when the user happened
     * to confirm one of the dialogs that call Flush() themselves. */
    // Fenster-GROESSE sichern - hier und nicht in OnCloseWindow, denn
    // 'Datei -> Beenden' ruft CloseProgram direkt auf und umgeht den
    // Schliessen-Ereignishandler (genau daran scheiterte der erste
    // Versuch).  Minimiert/maximiert nicht speichern, sonst merkt sich
    // GO die Taskleisten- bzw. Vollbildmasse als Normalgroesse.
    if (!IsIconized() && !IsMaximized())
      r_config.SetMainWindowRect(GetPosSize());
    r_config.Flush();
    Destroy();
  }
  return isClosed;
}

void GOAppWindow::OnExit(wxCommandEvent &event) { CloseProgram(); }


void GOAppWindow::OnPlaceTimer(wxTimerEvent &event) {
  const wxPoint pos(GetPosition());
  const bool placed = IsShownOnScreen() && (pos.x || pos.y);

  // Notbremse nach einer halben Sekunde: lieber einmal danebengreifen
  // als das Fenster in der Mitte stehen lassen.
  if (!placed && ++m_PlaceTries < 25)
    return;
  m_PlaceTimer.Stop();

  const int iDisplay = wxDisplay::GetFromWindow(this);

  if (iDisplay != wxNOT_FOUND)
    Move(wxDisplay(iDisplay).GetClientArea().GetTopLeft());
}

void GOAppWindow::OnCloseWindow(wxCloseEvent &event) {
  CloseProgram(!event.CanVeto());
}

#ifdef wxHAS_POWER_EVENTS
void GOAppWindow::OnPowerSuspended(wxPowerEvent &event) {
  r_SoundSystem.SuspendAudioForPowerEvent();
  event.Skip();
}

void GOAppWindow::OnPowerResume(wxPowerEvent &event) {
  r_SoundSystem.ResumeAudioAfterPowerEvent();
  event.Skip();
}
#endif

void GOAppWindow::OnRevert(wxCommandEvent &event) {
  if (
    wxMessageBox(
      _("Any customizations you have saved to this\norgan definition file "
        "will be lost!\n\nReset to defaults and reload?"),
      _("GrandOrgue"),
      wxYES_NO | wxICON_EXCLAMATION,
      this)
    == wxYES) {
    if (p_OrganController) {
      p_OrganController->DeleteSettings();
      LoadOrgan(p_OrganController->GetOrganInfo());
    }
  }
}

void GOAppWindow::OnProperties(wxCommandEvent &event) {
  if (p_OrganController) {
    GOPropertiesDialog dlg(p_OrganController, this);
    dlg.ShowModal();
  }
}

void GOAppWindow::OnAudioPanic(wxCommandEvent &WXUNUSED(event)) {
  r_SoundSystem.AssureSoundIsClosed();
  r_SoundSystem.AssureSoundIsOpen();
}

void GOAppWindow::OnMidiMonitor(wxCommandEvent &WXUNUSED(event)) {
  m_MidiMonitor = !m_MidiMonitor;
}

void GOAppWindow::OnMidiLoad(wxCommandEvent &WXUNUSED(event)) {
  wxFileDialog dlg(
    this,
    _("Load MIDI file"),
    r_config.MidiPlayerPath(),
    wxEmptyString,
    _("MID files (*.mid)|*.mid"),
    wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (dlg.ShowModal() == wxID_OK) {
    if (p_OrganController)
      p_OrganController->LoadMIDIFile(dlg.GetPath());
  }
}

// Formats seconds as "m:ss" or "h:mm:ss"
static wxString FormatDuration(unsigned sec) {
  if (sec < 3600)
    return wxString::Format(wxT("%u:%02u"), sec / 60, sec % 60);
  return wxString::Format(
    wxT("%u:%02u:%02u"), sec / 3600, (sec % 3600) / 60, sec % 60);
}

// Parses "_d{sec}_n{notes}" suffix from a base filename (without extension).
// Returns false if no metadata suffix present.
static bool ParseArchiveMetadata(
  const wxString &baseName, unsigned &durationSec, unsigned &noteCount) {
  const int dPos = baseName.Find(wxT("_d"));
  const int nPos = baseName.Find(wxT("_n"));
  if (dPos == wxNOT_FOUND || nPos == wxNOT_FOUND || nPos <= dPos)
    return false;
  const wxString durStr = baseName.Mid(dPos + 2, nPos - dPos - 2);
  const wxString noteStr = baseName.Mid(nPos + 2);
  if (!durStr.ToULong((unsigned long *)&durationSec))
    return false;
  if (!noteStr.ToULong((unsigned long *)&noteCount))
    return false;
  return true;
}

// Formats a menu label from the archive filename base name.
// Expected prefix format: YYYY-MM-DD-HH-MM-SS.mmm
static wxString FormatArchiveLabel(const wxString &baseName) {
  // Parse time from filename prefix "YYYY-MM-DD-HH-MM-SS.mmm..."
  wxString timeStr;
  if (baseName.length() >= 19) {
    // positions: Y=0, M=5, D=8, h=11, m=14, s=17
    timeStr = baseName.Mid(11, 2) + wxT(":") + baseName.Mid(14, 2) + wxT(":")
      + baseName.Mid(17, 2);
  }

  unsigned durationSec = 0, noteCount = 0;
  const bool hasMeta = ParseArchiveMetadata(baseName, durationSec, noteCount);

  if (!hasMeta)
    return timeStr.IsEmpty() ? baseName : timeStr;

  wxString label = timeStr.IsEmpty() ? wxString() : timeStr + wxT("  —  ");
  label += FormatDuration(durationSec);
  label += wxString::Format(wxT("  —  %u notes"), noteCount);
  return label;
}

void GOAppWindow::UpdateAutoMidiMenu() {
  while (m_autoMidiMenu->GetMenuItemCount() > 0)
    m_autoMidiMenu->Delete(m_autoMidiMenu->FindItemByPosition(0));

  // Always show the clear button at the top
  m_autoMidiMenu->Append(
    ID_MIDI_ARCHIVE_CLEAR, _("Clear archive..."), wxEmptyString, wxITEM_NORMAL);
  m_autoMidiMenu->AppendSeparator();

  const wxString archivePath = r_config.AutoMidiArchivePath();
  if (!r_config.AutoMidiArchiveEnabled() || archivePath.IsEmpty()) {
    m_autoMidiMenu->Append(
      wxID_ANY, _("(auto archive not enabled)"), wxEmptyString, wxITEM_NORMAL);
    m_autoMidiMenu->Enable(
      m_autoMidiMenu->FindItemByPosition(2)->GetId(), false);
    return;
  }

  wxDir dir(archivePath);
  if (!dir.IsOpened()) {
    m_autoMidiMenu->Append(
      wxID_ANY, _("(archive folder not found)"), wxEmptyString, wxITEM_NORMAL);
    m_autoMidiMenu->Enable(
      m_autoMidiMenu->FindItemByPosition(2)->GetId(), false);
    return;
  }

  // Collect all .mid files as full paths
  std::vector<wxString> files;
  wxString fname;
  bool cont = dir.GetFirst(&fname, wxT("*.mid"), wxDIR_FILES);
  while (cont) {
    files.push_back(wxFileName(archivePath, fname).GetFullPath());
    cont = dir.GetNext(&fname);
  }

  if (files.empty()) {
    m_autoMidiMenu->Append(
      wxID_ANY, _("(no recordings yet)"), wxEmptyString, wxITEM_NORMAL);
    m_autoMidiMenu->Enable(
      m_autoMidiMenu->FindItemByPosition(2)->GetId(), false);
    m_autoMidiMenu->Enable(ID_MIDI_ARCHIVE_CLEAR, false);
    return;
  }

  // Sort by filename descending: filenames start with YYYY-MM-DD-HH-MM-SS so
  // lexicographic reverse order = newest recording first, independent of
  // filesystem modification-time precision.
  std::sort(
    files.begin(), files.end(), [](const wxString &a, const wxString &b) {
      return wxFileName(a).GetName() > wxFileName(b).GetName();
    });

  const int maxItems = ID_MIDI_ARCHIVE_LAST - ID_MIDI_ARCHIVE_FIRST + 1;
  const int count = std::min((int)files.size(), maxItems);
  for (int i = 0; i < count; i++) {
    wxFileName fn(files[i]);
    const wxString label = FormatArchiveLabel(fn.GetName());
    m_autoMidiMenu->Append(
      ID_MIDI_ARCHIVE_FIRST + i, label, fn.GetFullPath(), wxITEM_NORMAL);
  }
}

void GOAppWindow::OnMidiArchiveLoad(wxCommandEvent &event) {
  if (!p_OrganController)
    return;
  const int idx = event.GetId() - ID_MIDI_ARCHIVE_FIRST;
  wxMenuItem *item
    = m_autoMidiMenu->FindItemByPosition(idx + 2); // +2: clear + separator
  if (!item)
    return;
  // The help string holds the full file path set during UpdateAutoMidiMenu
  const wxString fullPath = item->GetHelp();
  if (wxFileExists(fullPath))
    p_OrganController->LoadMIDIFile(fullPath);
}

void GOAppWindow::OnMidiArchiveClear(wxCommandEvent &WXUNUSED(event)) {
  const wxString archivePath = r_config.AutoMidiArchivePath();
  if (archivePath.IsEmpty())
    return;
  if (
    wxMessageBox(
      _("Delete all MIDI recordings from the auto archive?"),
      _("Clear Archive"),
      wxYES_NO | wxICON_WARNING | wxNO_DEFAULT,
      this)
    != wxYES)
    return;

  wxDir dir(archivePath);
  if (!dir.IsOpened())
    return;
  wxString fname;
  bool cont = dir.GetFirst(&fname, wxT("*.mid"), wxDIR_FILES);
  while (cont) {
    wxRemoveFile(archivePath + wxFileName::GetPathSeparator() + fname);
    cont = dir.GetNext(&fname);
  }
}

void GOAppWindow::OnAudioMemset(wxCommandEvent &WXUNUSED(event)) {
  if (p_OrganController)
    p_OrganController->GetSetter()->ToggleSetter();
}

void GOAppWindow::SetEventAfterSettings(
  wxEventType eventType, int eventId, GOOrgan *pOrganFile) {
  if (p_AfterSettingsEventOrgan)
    delete p_AfterSettingsEventOrgan;
  m_AfterSettingsEventType = eventType;
  m_AfterSettingsEventId = eventId;
  p_AfterSettingsEventOrgan = pOrganFile;
}

void GOAppWindow::OnSettings(wxCommandEvent &event) {
  m_InSettings = true;

  bool isToContinue = true; // will GO continue running? Otherwise it will exit
  SettingsReasons *const pReasons = (SettingsReasons *)event.GetClientData();
  GOSettingsDialog dialog(
    this, r_config, r_SoundSystem, r_MidiSystem, pReasons);

  if (dialog.ShowModal() == wxID_OK) {
    GOArchiveManager manager(r_config, r_config.OrganCachePath());
    manager.RegisterPackageDirectory(r_config.OrganPackagePath());

    UpdateVolumeControlWithSettings();

    // because the sound settings might be changed, close sound.
    // It will reopened later
    r_SoundSystem.AssureSoundIsClosed();

    r_config.Flush();
    if (
      dialog.NeedRestart()
      && wxMessageBox(
           "Some settings changes effect after GrandOrgue "
           "restarts.\n\nWould you like to restart now?",
           "GrandOrgue",
           wxYES_NO | wxICON_QUESTION,
           this)
        == wxYES) {
      r_app.SetToRestartAfterExit();
      SetEventAfterSettings(wxEVT_COMMAND_MENU_SELECTED, ID_FILE_EXIT);
      isToContinue = false;
    } else if (
      dialog.NeedReload() && r_SoundSystem.GetOrganFile() != NULL
      && wxMessageBox(
           _("Some changed settings effect unless the sample "
             "set is reloaded.\n\nWould you like to reload "
             "the sample set now?"),
           _("GrandOrgue"),
           wxYES_NO | wxICON_QUESTION,
           this)
        == wxYES) {
      SetEventAfterSettings(wxEVT_COMMAND_MENU_SELECTED, ID_FILE_RELOAD);
    }
  }
  if (pReasons)
    delete pReasons;

  // The sound might be closed in the settings dialog (for obtaining the list of
  // devices) or later if the settings were changed
  if (isToContinue)
    r_SoundSystem.AssureSoundIsOpen();

  if (m_AfterSettingsEventType != wxEVT_NULL) {
    wxCommandEvent event(m_AfterSettingsEventType, m_AfterSettingsEventId);

    if (p_AfterSettingsEventOrgan)
      event.SetClientData(p_AfterSettingsEventOrgan);
    GetEventHandler()->AddPendingEvent(event);
    p_AfterSettingsEventOrgan = NULL;
    m_AfterSettingsEventType = wxEVT_NULL;
  }
  m_InSettings = false;
}

void GOAppWindow::OnAudioState(wxCommandEvent &WXUNUSED(event)) {
  GOMessageBox(r_SoundSystem.getState(), _("Sound output"), wxOK, this);
}

void GOAppWindow::ShowAudioMonitor() {
  // Single instance: re-invoking the menu item raises the open monitor
  // instead of spawning another one (multiple monitors double the polling
  // and confuse the "reopen on start" persistence). Looked up by window
  // name so no lifetime bookkeeping is needed here.
  if (wxWindow *pExisting = wxWindow::FindWindowByName(
        wxT("GOAudioMonitorDlg"), nullptr)) {
    pExisting->Show();
    pExisting->Raise();
    return;
  }

  // Non-modal: a live monitor should stay open and update while the user
  // keeps playing. The organ getter is re-invoked on every poll tick
  // rather than passing p_OrganController directly, so the window picks up
  // a later organ load/unload/reload instead of freezing on whatever was
  // loaded (or not) when it was opened.
  //
  // Open-state persistence: remembered globally (not per organ - the
  // monitor is a diagnostics tool, not an organ artifact). Set here;
  // cleared when the user closes the window (GOAudioMonitorDlg's close
  // handler). An app shutdown destroys the child frame WITHOUT a close
  // event, so the flag survives exactly when the window was still open.
  r_config.AudioMonitorOpen(true);
  GOAudioMonitorDlg *pMon = new GOAudioMonitorDlg(
    this,
    r_SoundSystem,
    [this]() -> GOOrganModel * { return p_OrganController; },
    [this](unsigned n) { return BenchmarkSetBufferSize(n); },
    [this](bool v3) { BenchmarkSetV3AndReload(v3); });

  // Ein wxFrame ohne eigene Position wird vom Fenstermanager platziert -
  // unter KDE auf dem AKTIVEN Bildschirm, also dem, auf dem die
  // Verknuepfung angeklickt wurde, nicht bei GO (das seine Geometrie
  // gespeichert hat).  Die Eltern-Beziehung allein genuegt dafuer nicht;
  // erst eine ausdrueckliche Position bindet ihn ans Hauptfenster.
  pMon->CentreOnParent();
  pMon->Show();
}

void GOAppWindow::OnAudioMonitor(wxCommandEvent &WXUNUSED(event)) {
  ShowAudioMonitor();
}

void GOAppWindow::OnSetCrossfade(wxCommandEvent &e) {
  using namespace GOAudioParams;
  GOCrossfadeMode m = GOCrossfadeMode::SinEqualPower;
  switch (e.GetId()) {
  case ID_Crossfade_Linear:
    m = GOCrossfadeMode::Linear;
    break;
  case ID_Crossfade_SinEq:
    m = GOCrossfadeMode::SinEqualPower;
    break;
  case ID_Crossfade_Sin2:
    m = GOCrossfadeMode::Sin2;
    break;
  case ID_Crossfade_SqrtEq:
    m = GOCrossfadeMode::SqrtEqualPower;
    break;
  case ID_Crossfade_X2:
    m = GOCrossfadeMode::X2;
    break;
  case ID_Crossfade_Custom:
    m = GOCrossfadeMode::Custom;
    break;
  }
  SetCrossfadeMode(m);

  // Clear stale templates from the previous mode and re-warm for the newly
  // selected one with this organ's actual lengths (see Setup()'s analogous
  // precompute for why: fades already in progress are unaffected, they hold
  // their own shared_ptr to their template).
  {
    std::vector<unsigned> lengths;

    if (m != GOCrossfadeMode::Linear && p_OrganController) {
      const unsigned sampleRate = r_SoundSystem.GetSampleRate();

      for (unsigned ms : p_OrganController->GetCrossfadeLengthsMs())
        lengths.push_back(sampleRate * ms / 1000);
    }
    GOAudioParams::FastCrossfadeCache::BeginPrecomputeJob();
    std::thread([m, lengths = std::move(lengths)]() {
      GOAudioParams::FastCrossfadeCache::Clear();
      GOAudioParams::FastCrossfadeCache::PrecomputeForMode(m, lengths);
      GOAudioParams::FastCrossfadeCache::EndPrecomputeJob();
    }).detach();
  }
}

void GOAppWindow::OnSetReleaseAlign(wxCommandEvent &e) {
  using namespace GOAudioParams;
  GOReleaseAlignMode m = GOReleaseAlignMode::Legacy;
  if (e.GetId() == ID_ReleaseAlign_Correlation)
    m = GOReleaseAlignMode::Correlation;
  SetReleaseAlignMode(m);
}

void GOAppWindow::OnUpdateLutCache(wxUpdateUIEvent &event) {
  event.Enable(p_OrganController != nullptr);
}

void GOAppWindow::OnLutCacheGenerate(wxCommandEvent &) {
  if (!p_OrganController)
    return;
  GOLutCacheDlg dlg(this, p_OrganController, r_SoundSystem);
  dlg.ShowModal();
}

void GOAppWindow::OnLutCacheDelete(wxCommandEvent &) {
  if (!p_OrganController)
    return;
  p_OrganController->DeleteLutCache();
  r_SoundSystem.WithOrganEngineQuiesced(
    [this]() { p_OrganController->ClearAllCachedLuts(); });
}

void GOAppWindow::OnOrganSettings(wxCommandEvent &event) {
  if (mp_organ)
    mp_organ->ShowOrganSettingsDialog();
}

void GOAppWindow::OnMidiList(wxCommandEvent &event) {
  if (mp_organ)
    mp_organ->ShowMidiList();
}

void GOAppWindow::OnStops(wxCommandEvent &event) {
  if (mp_organ)
    mp_organ->ShowStops();
}

void GOAppWindow::OnHelp(wxCommandEvent &event) {
  GOHelpRequestor::DisplayHelp(_("User Interface"), false);
}

void GOAppWindow::OnSettingsVolume(wxCommandEvent &event) {
  long n = m_Volume->GetValue();

  r_SoundSystem.GetEngine().SetVolume(n);
  if (p_OrganController && p_OrganController->GetVolume() != (int)n) {
    /* Write the change back into the organ controller - that is the value
     * GOOrganController::Save() persists into the organ's CMB settings.
     * Without this the spinner only ever changed the live engine, and
     * saving the organ settings silently re-wrote the volume the organ
     * had been LOADED with. Marking the organ modified makes GO offer to
     * save after a volume-only change, same as transpose does. The
     * equality guard keeps the load path (spinner set from the loaded
     * value via wxEVT_SETVALUE, see GOGuiOrgan::LoadOrgan) from
     * immediately flagging a freshly loaded organ as modified. */
    p_OrganController->SetVolume((int)n);
    /* Mark modified only on the TRANSITION: SetOrganModified notifies
     * the modification listeners unconditionally, and this window's
     * handler rebuilds the whole panel menu on every notification - with
     * one notification per mouse-wheel tick the menu flickered and the
     * spinner could not even repaint its value. Once the organ is
     * already marked, further volume ticks have nothing new to say. */
    if (!p_OrganController->IsOrganModified())
      p_OrganController->SetOrganModified();
  }
  for (unsigned i = 0; i < m_VolumeGauge.size(); i++)
    m_VolumeGauge[i]->ResetClip();
}

void GOAppWindow::OnSettingsPolyphony(wxCommandEvent &event) {
  long n = m_Polyphony->GetValue();

  r_config.PolyphonyLimit(n);
  r_SoundSystem.GetEngine().SetHardPolyphony(n);
  m_SamplerUsage->ResetClip();
}

void GOAppWindow::OnSettingsMemoryEnter(wxCommandEvent &event) {
  long n = m_SetterPosition->GetValue();

  if (p_OrganController)
    p_OrganController->GetSetter()->SetPosition(n);
}

void GOAppWindow::OnSettingsMemory(wxCommandEvent &event) {
  long n = m_SetterPosition->GetValue();

  if (p_OrganController)
    p_OrganController->GetSetter()->UpdatePosition(n);
}

void GOAppWindow::OnSettingsTranspose(wxCommandEvent &event) {
  if (p_OrganController) {
    long n = m_Transpose->GetValue();

    r_config.Transpose(n);
    p_OrganController->GetSetter()->SetTranspose(n);
  }
}

void GOAppWindow::OnSettingsReleaseLength(wxCommandEvent &event) {
  UpdateReleaseLength(m_ReleaseLength->GetSelection() * 50);
}

void GOAppWindow::OnHelpAbout(wxCommandEvent &event) { DoSplash(false); }

void GOAppWindow::DoSplash(bool timeout) { GOSplash::DoSplash(timeout, this); }

void GOAppWindow::OnMenuOpen(wxMenuEvent &event) {
  DoMenuUpdates(event.GetMenu());
  if (event.GetMenu() == m_panel_menu)
    UpdatePanelMenu();
  if (event.GetMenu() == m_file_menu)
    UpdateFavoritesMenu();
  if (event.GetMenu() == m_file_menu)
    UpdateRecentMenu();
  if (event.GetMenu() == m_audio_menu)
    UpdateTemperamentMenu();
  if (event.GetMenu() == m_autoMidiMenu)
    UpdateAutoMidiMenu();
  event.Skip();
}

void GOAppWindow::OnChangeTranspose(wxCommandEvent &event) {
  m_Transpose->SetValue(event.GetInt());
}

void GOAppWindow::OnChangeSetter(wxCommandEvent &event) {
  m_SetterPosition->SetValue(event.GetInt());
}

void GOAppWindow::OnChangeVolume(wxCommandEvent &event) {
  m_Volume->SetValue(event.GetInt());
}

void GOAppWindow::OnKeyCommand(wxKeyEvent &event) {
  int flags = wxACCEL_NORMAL;
  if (event.ControlDown())
    flags |= wxACCEL_CTRL;
  if (event.AltDown())
    flags |= wxACCEL_ALT;
  if (event.ShiftDown())
    flags |= wxACCEL_SHIFT;
  const int k = event.GetKeyCode();
  for (const auto &s : s_shortcuts) {
    if (s.mod == flags && s.key == k) {
      ProcessCommand(s.id);
      return;
    }
  }
  event.Skip();
}

void GOAppWindow::OnMidiEvent(const GOMidiEvent &event) {
  if (m_MidiMonitor) {
    wxLogWarning(_("MIDI event: ") + event.ToString(r_MidiSystem.GetMidiMap()));
  }

  if (event.IsAllowedToReload()) {
    ptr_vector<GOOrgan> &organs = r_config.GetOrganList();
    for (auto pOrgan : organs) {
      GORegisteredOrgan *pO = dynamic_cast<GORegisteredOrgan *>(pOrgan);

      if (pO && pO->Match(event) && pO->IsUsable(r_config)) {
        SendLoadOrgan(*pO);
        break;
      }
    }
  }
}

void GOAppWindow::OnSetTitle(wxCommandEvent &event) {
  m_Label = event.GetString();
  if (m_Label == wxEmptyString)
    SetTitle(m_Title);
  else
    SetTitle(m_Title + _(" - ") + m_Label);
}

void GOAppWindow::OnMsgBox(wxMsgBoxEvent &event) {
  wxMessageBox(event.getText(), event.getTitle(), event.getStyle(), this);
}

void GOAppWindow::OnRenameFile(wxRenameFileEvent &event) {
  wxFileName filepath = event.getFilename();

  wxFileDialog dlg(
    this,
    _("Save as"),
    event.getDirectory(),
    event.getFilename(),
    event.getFilter(),
    wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
  if (dlg.ShowModal() == wxID_OK) {
    if (filepath.GetFullPath() != dlg.GetPath())
      go_rename_file(filepath.GetFullPath(), dlg.GetPath());
  } else
    wxRemoveFile(filepath.GetFullPath());

  go_sync_directory(filepath.GetPath());
}

void GOAppWindow::OnUpdateCheckingRequested(wxCommandEvent &event) {
  if (m_UpdateCheckerThread) {
    // Update checking is already in progress, ignore request
    return;
  }
  wxBeginBusyCursor();
  m_UpdateCheckerThread = GOUpdateChecker::StartThread(
    GOUpdateChecker::CheckReason::USER_REQUEST, this);
}

void GOAppWindow::OnUpdateCheckingCompletion(
  GOUpdateChecker::CompletionEvent &event) {
  // Remove update checker thread
  m_UpdateCheckerThread->Wait();
  m_UpdateCheckerThread = nullptr;

  // Handle result
  const GOUpdateChecker::Result &result = event.GetResult();
  if (result.checkReason == GOUpdateChecker::CheckReason::USER_REQUEST) {
    wxEndBusyCursor();
    if (result.successful) {
      if (result.updateAvailable) {
        GONewReleaseDialog dialog(
          this, r_config, event.GetResult().latestRelease);
        if (dialog.ShowModal() == wxID_OK) {
          r_config.Flush();
        }
      } else {
        wxMessageBox(
          _("You are using the latest GrandOrgue version"),
          _("Update checker"));
      }
    } else {
      wxMessageBox(
        _("Failed to check for updates: %s").Format(result.errorMessage));
    }
  } else if (
    result.checkReason == GOUpdateChecker::CheckReason::STARTUP
    && result.successful && result.updateAvailable) {
    m_StartupUpdateCheckerResult = result;
    wxMenu *updateAvailableMenu = new wxMenu();
    wxString versionChangeStr
      = wxString::Format("%s -> %s", APP_VERSION, result.latestRelease.version);
    updateAvailableMenu->Append(ID_SHOW_RELEASE_CHANGELOG, versionChangeStr);
    updateAvailableMenu->Append(
      ID_SHOW_RELEASE_CHANGELOG, _("&Show changelog"));
    updateAvailableMenu->Append(ID_DOWNLOAD_NEW_RELEASE, _("&Download"));
    wxMenuBar *menuBar = GetMenuBar();
    menuBar->Append(updateAvailableMenu, _("&Update available!"));
  }
}

void GOAppWindow::OnNewReleaseInfoRequested(wxCommandEvent &event) {
  GONewReleaseDialog dialog(
    this, r_config, m_StartupUpdateCheckerResult.latestRelease);
  if (dialog.ShowModal() == wxID_OK) {
    r_config.Flush();
  }
}

void GOAppWindow::OnNewReleaseDownload(wxCommandEvent &event) {
  GOUpdateChecker::OpenDownloadPageInBrowser();
}

bool GOAppWindow::InstallOrganPackage(wxString name) {
  GOArchiveManager manager(r_config, r_config.OrganCachePath());
  wxString result = manager.InstallPackage(name);
  if (result != wxEmptyString) {
    GOMessageBox(result, _("Error"), wxOK | wxICON_ERROR, this);
    return false;
  } else
    return true;
}

void GOAppWindow::LoadLastOrgan() {
  std::vector<const GOOrgan *> list = r_config.GetLRUOrganList();
  if (list.size() > 0)
    SendLoadOrgan(*list[0]);
}

void GOAppWindow::LoadFirstOrgan() {
  ptr_vector<GOOrgan> &list = r_config.GetOrganList();
  if (list.size() > 0)
    SendLoadOrgan(*list[0]);
}

void GOAppWindow::SendLoadFile(wxString filename) {
  wxFileName name = filename;
  if (name.GetExt() == wxT("orgue")) {
    if (InstallOrganPackage(filename))
      LoadLastOrgan();
  } else
    SendLoadOrgan(GOOrgan(filename));
}

void GOAppWindow::SendLoadOrgan(const GOOrgan &organ) {
  wxCommandEvent evt(wxEVT_LOADFILE, 0);
  evt.SetClientData(new GOOrgan(organ));
  GetEventHandler()->AddPendingEvent(evt);
}
