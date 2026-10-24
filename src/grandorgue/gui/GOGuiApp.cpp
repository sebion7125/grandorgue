/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOGuiApp.h"

#include <wx/cmdline.h>
#include <wx/filesys.h>
#include <wx/fs_zip.h>
#include <wx/regex.h>

#include "config/GOConfig.h"
#include "frames/GOAppWindow.h"
#include "sound/GOSoundSystem.h"
#include "sound/fast_crossfade.h"

#include "GOGuiLog.h"
#include "GOStdPath.h"
#include "go_defs.h"

#ifdef __WXMAC__
#include <ApplicationServices/ApplicationServices.h>
#endif

#ifdef __WIN32__
#include <windows.h>
#endif

#ifdef GO_SET_PRGNAME
#include <glib.h>
#endif

IMPLEMENT_APP(GOGuiApp)

GOGuiApp::~GOGuiApp() = default;

void GOGuiApp::TemporaryLog::DoLogTextAtLevel(
  wxLogLevel level, const wxString &msg) {
  FILE *output = (level <= wxLOG_Warning) ? stderr : stdout;

  fprintf(output, "%s\n", msg.mb_str().data());
  if (level <= wxLOG_Error)
    wxMessageBox(msg, "Error", wxOK | wxICON_ERROR);
}

static const char *const SWITCH_GUI = "g";
static const char *const SWITCH_HELP = "h";
static const char *const OPTION_INSTANCE = "i";
static const char *const OPTION_CONFIG_FILE = "c";

static const wxCmdLineEntryDesc cmd_line_desc[] = {
  {wxCMD_LINE_SWITCH,
   SWITCH_GUI,
   "gui-only",
   wxTRANSLATE("Load just GUI. Not to load any sound samples"),
   wxCMD_LINE_VAL_NONE,
   0},
  {wxCMD_LINE_SWITCH,
   SWITCH_HELP,
   "help",
   wxTRANSLATE("displays help on the command line parameters"),
   wxCMD_LINE_VAL_NONE,
   wxCMD_LINE_OPTION_HELP},
  {wxCMD_LINE_OPTION,
   OPTION_INSTANCE,
   "instance",
   wxTRANSLATE("specify GrandOrgue instance name"),
   wxCMD_LINE_VAL_STRING,
   wxCMD_LINE_PARAM_OPTIONAL},
  {wxCMD_LINE_OPTION,
   OPTION_CONFIG_FILE,
   "config",
   wxTRANSLATE("specify GrandOrgue config file name"),
   wxCMD_LINE_VAL_STRING,
   wxCMD_LINE_PARAM_OPTIONAL},
  {wxCMD_LINE_SWITCH,
   "v",
   "verbose",
   wxTRANSLATE("generate verbose log messages"),
   wxCMD_LINE_VAL_NONE,
   0x0},
  {wxCMD_LINE_PARAM,
   NULL,
   NULL,
   wxTRANSLATE("organ file"),
   wxCMD_LINE_VAL_STRING,
   wxCMD_LINE_PARAM_OPTIONAL},
  {wxCMD_LINE_NONE}};

void GOGuiApp::OnInitCmdLine(wxCmdLineParser &parser) {
  parser.SetLogo(wxString::Format(
    _("GrandOrgue %s - Virtual Pipe Organ Software"), wxT(APP_VERSION)));
  parser.SetDesc(cmd_line_desc);
}

bool GOGuiApp::OnCmdLineParsed(wxCmdLineParser &parser) {
  bool res = wxApp::OnCmdLineParsed(parser);
  wxString str;

  if (res)
    m_IsGuiOnly = parser.FoundSwitch(SWITCH_GUI) == wxCMD_SWITCH_ON;
  if (res && parser.Found(OPTION_INSTANCE, &str)) {
    wxRegEx r(wxT("^[A-Za-z0-9]+$"), wxRE_ADVANCED);

    if (r.Matches(str))
      m_InstanceName = std::string("-") + str.ToStdString();
    else {
      wxMessageOutput::Get()->Printf(_("Invalid instance name"));
      res = false;
    }
  }
  if (res && parser.Found(OPTION_CONFIG_FILE, &str))
    m_ConfigFilePath = str.ToStdString();
  if (res)
    for (unsigned i = 0; i < parser.GetParamCount(); i++)
      m_FileName = parser.GetParam(i);
  return res;
}

bool GOGuiApp::OnInit() {
  // ZEIGERPOSITION SOFORT festhalten - der frueheste Moment, den wir
  // haben, und damit der naechste am Klick auf die Verknuepfung.  Sie
  // entscheidet spaeter, auf welchem Bildschirm das Hauptfenster
  // erscheint (GOAppWindow::SetPosSize).  Im Fensterkonstruktor
  // gemessen war sie bereits verrutscht: einmal lag sie zwei Pixel vor
  // der Bildschirmgrenze und schickte GO auf den Nachbarschirm.
  GOAppWindow::SetStartupPointer(::wxGetMousePosition());
  wxLog::SetActiveTarget(mp_TemporaryLog.get());

#ifdef __WXMAC__
  /* This ensures that the executable (when it is not in the form of an OS X
   * bundle, is brought into the foreground). GetCurrentProcess() should not
   * be used as it has been deprecated as of 10.9. We use a "Process
   * Identification Constant" instead. See the "Process Manager Reference"
   * document for more information. */
  static const ProcessSerialNumber PSN = {0, kCurrentProcess};
  TransformProcessType(&PSN, kProcessTransformToForegroundApplication);
#endif

  SetAppName(wxT("GrandOrgue"));
  SetClassName(wxT("GrandOrgue"));
  SetVendorName(wxT("Our Organ"));

#ifdef GO_SET_PRGNAME
  /* Wayland has no equivalent of the X11 _NET_WM_ICON property: a window
   * carries no image, it only identifies itself by an app_id, and the
   * compositor uses that string to look up the matching .desktop file - and
   * with it the icon and the taskbar entry. GTK takes the app_id (and the X11
   * WM_CLASS) from g_get_prgname(), which gtk_init() derives from the name of
   * the executable file. That name is not always "GrandOrgue": inside an
   * AppImage it is "AppRun.wrapped", so the lookup fails and the window ends
   * up with a placeholder icon. SetAppName()/SetClassName() above do not
   * reach the app_id, hence the explicit call - it has to happen before the
   * first top level window is created, which is where GTK reads the name. */
  g_set_prgname("GrandOrgue");
#endif

  wxIdleEvent::SetMode(wxIDLE_PROCESS_SPECIFIED);
  wxFileSystem::AddHandler(new wxZipFSHandler);
  wxImage::AddHandler(new wxJPEGHandler);
  wxImage::AddHandler(new wxGIFHandler);
  wxImage::AddHandler(new wxPNGHandler);
  wxImage::AddHandler(new wxBMPHandler);
  wxImage::AddHandler(new wxICOHandler);
  srand(::wxGetUTCTime());

#ifdef __WIN32__
  SetThreadExecutionState(
    ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
#endif

  if (!wxApp::OnInit())
    return false;

  mp_config = std::make_unique<GOConfig>(m_InstanceName, m_ConfigFilePath);
  mp_config->Load();

  GOStdPath::InitLocaleDir();
  m_locale.Init(mp_config->GetLanguageId());
  m_locale.AddCatalog(wxT("GrandOrgue"));

  mp_SoundSystem = std::make_unique<GOSoundSystem>(*mp_config);

  p_AppWindow = new GOAppWindow(
    *this,
    NULL,
    wxID_ANY,
    wxString::Format(_("GrandOrgue %s"), wxT(APP_VERSION)),
    wxDefaultPosition,
    wxDefaultSize,
    wxMINIMIZE_BOX | wxRESIZE_BORDER | wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX
      | wxCLIP_CHILDREN | wxFULL_REPAINT_ON_RESIZE,
    *mp_SoundSystem);
  SetTopWindow(p_AppWindow);
  mp_log = std::make_unique<GOGuiLog>(p_AppWindow);
  wxLog::SetActiveTarget(mp_log.get());
  p_AppWindow->Init(m_FileName, m_IsGuiOnly);

  return true;
}

#ifdef __WXMAC__
void GOGuiApp::MacOpenFile(const wxString &filename) {
  if (p_AppWindow)
    p_AppWindow->SendLoadFile(filename);
}
#endif

int GOGuiApp::OnRun() { return wxApp::OnRun(); }

int GOGuiApp::OnExit() {
  // Bounded wait for any still-running FastCrossfadeCache precompute job
  // (detached background thread from GOSoundOrganEngine::Setup() or
  // GOAppWindow::OnSetCrossfade()) - its s_cache/s_mutex are process-lifetime
  // statics, so a thread still touching them once those statics are
  // destroyed during exit would be undefined behaviour.
  GOAudioParams::FastCrossfadeCache::WaitForPendingJobs();

  wxLog::FlushActive();
  wxLog::SetActiveTarget(nullptr);

  int rc = wxApp::OnExit();

  if (m_IsToRestartAfterExit) {
    wchar_t **cmdargs(argv);

    wxExecute(cmdargs);
  }
  return rc;
}

void GOGuiApp::CleanUp() {
  // Ensure that GOAppWindow and other objects are destroyed before deleting
  wxApp::CleanUp();
  // CleanUp() may be called even if OnInit() has not succeed, so unique_ptr
  // reset() is safe to call even if the objects were never created
  mp_SoundSystem.reset();
  mp_config.reset();
  mp_log.reset();
}