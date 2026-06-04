/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOOrganController.h"

#include <algorithm>
#include <math.h>

#include <wx/filename.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/txtstrm.h>
#include <wx/wfstream.h>
#include <wx/stopwatch.h>
#include <typeinfo>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "archive/GOArchive.h"
#include "archive/GOArchiveFile.h"
#include "combinations/GODivisionalSetter.h"
#include "combinations/GOSetter.h"
#include "combinations/control/GOGeneralButtonControl.h"
#include "config/GOConfig.h"
#include "config/GOConfigFileReader.h"
#include "config/GOConfigFileWriter.h"
#include "config/GOConfigReader.h"
#include "config/GOConfigReaderDB.h"
#include "config/GOConfigWriter.h"
#include "contrib/sha1.h"
#include "control/GOElementCreator.h"
#include "control/GOPushbuttonControl.h"
#include "files/GOOpenedFile.h"
#include "files/GOStdFileName.h"
#include "files/GOStandardFile.h"
#include "gui/dialogs/GOProgressDialog.h"
#include "gui/dialogs/go-message-boxes.h"
#include "gui/panels/GOGUIBankedGeneralsPanel.h"
#include "gui/panels/GOGUICouplerManualsAndVolumePanel.h"
#include "gui/panels/GOGUICouplerPanel.h"
#include "gui/panels/GOGUICrescendoPanel.h"
#include "gui/panels/GOGUIDivisionalsPanel.h"
#include "gui/panels/GOGUIMasterPanel.h"
#include "gui/panels/GOGUIMetronomePanel.h"
#include "gui/panels/GOGUIPanel.h"
#include "gui/panels/GOGUIPanelCreator.h"
#include "gui/panels/GOGUIRecorderPanel.h"
#include "gui/panels/GOGUISequencerPanel.h"
#include "loader/GOLoadThread.h"
#include "loader/GOLoaderFilename.h"
#include "loader/cache/GOCache.h"
#include "loader/cache/GOCacheWriter.h"
#include "midi/GOMidiPlayer.h"
#include "midi/GOMidiRecorder.h"
#include "midi/GOMidiSystem.h"
#include "midi/events/GOMidiEvent.h"
#include "model/GOCoupler.h"
#include "model/GODivisionalCoupler.h"
#include "model/GOEnclosure.h"
#include "model/GOManual.h"
#include "model/GORank.h"
#include "model/GOSoundingPipe.h"
#include "model/GOSwitch.h"
#include "model/GOTremulant.h"
#include "sound/GOCrossfadeParam.h"
#include "sound/GOSoundOrganEngine.h"
#include "sound/playing/GOLutCacheFile.h"
#include "sound/playing/GOSoundReleaseAlignTable.h"
#include "temperaments/GOTemperament.h"
#include "yaml/GOYamlModel.h"

#include "go_defs.h"

#ifndef LOG_TIMING
// Fallbacks: if the generated go_defs.h (from CMake) does not define these
// macros yet (e.g. configure hasn't been rerun), provide safe no-op defaults.
# define LOG_TIMING(...) do{}while(0)
#endif
#ifndef LOG_GUI_GAP
# define LOG_GUI_GAP(...) do{}while(0)
#endif

#include "GOAudioRecorder.h"
#include "GOBuffer.h"
#include "GODocument.h"
#include "GOEvent.h"
#include "GOHash.h"
#include "GOMetronome.h"
#include "GOOrgan.h"
#include "go_path.h"
#include "GOMemoryPool.h"

//#define GUI_GAP_TRACER 0

static const wxString WX_ORGAN = wxT("Organ");
static const wxString WX_GRANDORGUE_VERSION = wxT("GrandOrgueVersion");

GOOrganController::GOOrganController(GOConfig &config, bool isAppInitialized)
  : GOEventDistributor(this),
    GOOrganModel(config),
    m_config(config),
    m_odf(),
    m_ArchiveID(),
    m_hash(),
    m_FileStore(config),
    m_CacheFilename(),
    m_SettingFilename(),
    m_ODFHash(),
    m_Cacheable(false),
    m_lutReleaseCount(0),
    m_setter(0),
    m_AudioRecorder(NULL),
    m_MidiPlayer(NULL),
    m_MidiRecorder(NULL),
    m_timer(NULL),
    p_OnStateButton(nullptr),
    m_volume(0),
    m_b_customized(false),
    m_CurrentPitch(999999.0), // for enforcing updating the label first time
    m_OrganModified(false),
    m_ChurchAddress(),
    m_OrganBuilder(),
    m_OrganBuildDate(),
    m_OrganComments(),
    m_RecordingDetails(),
    m_InfoFilename(),
    m_panels(),
    m_panelcreators(),
    m_elementcreators(),
    m_soundengine(0),
    m_midi(0),
    m_MidiSamplesetMatch(),
    m_SampleSetId1(0),
    m_SampleSetId2(0),
    mp_ImageCache(nullptr),
    m_PitchLabel(*this),
    m_TemperamentLabel(*this),
    m_MainWindowData(this, wxT("MainWindow")) {
  if (isAppInitialized) {
    // Load here objects that needs App (wx) to be loaded
    m_timer = new GOTimer();
    mp_ImageCache = new GOImageCache(m_FileStore);
  }
  GOOrganModel::SetModelModificationListener(this);
  m_setter = new GOSetter(this);
  // Set memory limit from settings (no automatic clamping; 0 = unlimited)
  {
    size_t cfgMB = m_config.MemoryLimit();
    m_pool.SetMemoryLimit(cfgMB ? cfgMB * 1024 * 1024 : 0);
  }
}

GOOrganController::~GOOrganController() {
  p_OnStateButton = nullptr;
  m_FileStore.CloseArchives();
  GOEventHandlerList::Cleanup();
  // Just to be sure, that the sound providers are freed before the pool
  m_manuals.clear();
  m_tremulants.clear();
  m_ranks.clear();
  m_VirtualCouplers.Cleanup();
  GOOrganModel::Cleanup();
  GOOrganModel::SetModelModificationListener(nullptr);
  GOOrganModel::SetCombinationController(nullptr);
  m_elementcreators.clear();
  // some elementcreator may reference to m_timer so we respect the deletion
  // order
  if (mp_ImageCache)
    delete mp_ImageCache;
  if (m_timer)
    delete m_timer;
}

void GOOrganController::SetOrganModified(bool modified) {
  if (modified != m_OrganModified) {
    m_OrganModified = modified;
    m_setter->UpdateModified(modified);
  }
  GOModificationProxy::OnIsModifiedChanged(modified);
}

void GOOrganController::OnIsModifiedChanged(bool modified) {
  if (modified) {
    // Update the pitch label if it has been changed
    const float newPitch
      = GetRootPipeConfigNode().GetPipeConfig().GetManualTuning();

    if (newPitch != m_CurrentPitch) {
      m_PitchLabel.SetContent(wxString::Format(_("%0.1f cent"), newPitch));
      m_CurrentPitch = newPitch;
    }
    // If the organ model is modified then the organ is also modified
    SetOrganModified(true);
  }
  // else nothing because the organ may be modified without the model
}

void GOOrganController::ResetOrganModified() {
  // if the whole organ becomes not modified then the model also becomes
  // not modified
  ResetOrganModelModified();
  SetOrganModified(false);
}

GOHashType GOOrganController::GenerateCacheHash() {
  GOHash hash;

  UpdateHash(hash);
  hash.Update(sizeof(GOSoundAudioSection));
  hash.Update(sizeof(GOSoundingPipe));
  hash.Update(sizeof(GOSoundReleaseAlignTable));
  hash.Update(BLOCK_HISTORY);
  hash.Update(GOSoundAudioSection::getMaxReadAhead());
  hash.Update(SHORT_LOOP_LENGTH);
  GOSoundProvider::UpdateCacheHash(hash);
  hash.Update(sizeof(GOSoundAudioSection::StartSegment));
  hash.Update(sizeof(GOSoundAudioSection::EndSegment));
  return hash.getHash();
}

#include "loader/GOLoadExceptions.h"

// Helper for diagnostic logging before throwing so we can trace where
// an early abort originates during load.
static inline void ThrowGOLoadAborted(const char *where) {
  /*wxLogMessage("GOLoadAborted thrown at: %s", where);
  wxLog::FlushActive();*/  // don't log exceptions anymore. this was just a debug helper
  ThrowGOLoadAbortedEarly(__func__);
}

void GOOrganController::ReadOrganFile(GOConfigReader &cfg, GOProgressDialog *dlg) {
  /* load church info */
  cfg.ReadString(
    ODFSetting, WX_ORGAN, wxT("HauptwerkOrganFileFormatVersion"), false);
  m_ChurchAddress = cfg.ReadString(ODFSetting, WX_ORGAN, wxT("ChurchAddress"));
  m_OrganBuilder
    = cfg.ReadString(ODFSetting, WX_ORGAN, wxT("OrganBuilder"), false);
  m_OrganBuildDate
    = cfg.ReadString(ODFSetting, WX_ORGAN, wxT("OrganBuildDate"), false);
  m_OrganComments
    = cfg.ReadString(ODFSetting, WX_ORGAN, wxT("OrganComments"), false);
  m_RecordingDetails
    = cfg.ReadString(ODFSetting, WX_ORGAN, wxT("RecordingDetails"), false);
  wxString info_filename
    = cfg.ReadFileName(ODFSetting, WX_ORGAN, wxT("InfoFilename"), false);
  wxFileName fn;
  m_InfoFilename = wxEmptyString;
  if (info_filename.IsEmpty()) {
    /* Resolve organ file path */
    fn = GetODFFilename();
    fn.SetExt(wxT("html"));
    if (fn.FileExists() && !m_FileStore.AreArchivesUsed())
      m_InfoFilename = fn.GetFullPath();
  } else {
    if (!m_FileStore.AreArchivesUsed()) {
      fn = GOLoaderFilename::generateFullPath(
        info_filename, wxFileName(GetODFFilename()).GetPath());
      if (
        fn.FileExists()
        && (fn.GetExt() == wxT("html") || fn.GetExt() == wxT("htm")))
        m_InfoFilename = fn.GetFullPath();
      else if (m_config.ODFCheck())
        wxLogWarning(
          _("InfoFilename %s either does not exist or is not a html file"),
          fn.GetFullPath());
    }
  }

  /* load basic organ information */
  unsigned NumberOfPanels = cfg.ReadInteger(
    ODFSetting, WX_ORGAN, wxT("NumberOfPanels"), 0, 100, false);
  cfg.ReadString(CMBSetting, WX_ORGAN, WX_GRANDORGUE_VERSION, false);
  m_volume = cfg.ReadInteger(
    CMBSetting, WX_ORGAN, wxT("Volume"), -120, 100, false, m_config.Volume());
  if (m_volume > 20)
    m_volume = 0;
  m_Temperament
    = cfg.ReadString(CMBSetting, WX_ORGAN, wxT("Temperament"), false);

  // Read persisted crossfade mode for this organ (if present)
  // Backwards compatibility: older organ files that do not contain a
  // CrossfadeMode entry should keep the legacy Linear behaviour.
  {
    // Check presence by attempting to read the entry as a string (non-required).
    const wxString cf_entry = cfg.ReadString(
      CMBSetting, WX_ORGAN, wxT("CrossfadeMode"), false, wxEmptyString);
    if (cf_entry.IsEmpty()) {
      // Old organ file: keep legacy default (Linear)
      GOAudioParams::SetCrossfadeMode(GOCrossfadeMode::Linear);
    } else {
      // Newer files: parse stored integer (fall back to SinEqualPower if parse fails)
      long cf = static_cast<long>(GOCrossfadeMode::SinEqualPower);
      cf = cfg.ReadInteger(
        CMBSetting, WX_ORGAN, wxT("CrossfadeMode"), 0, 10, false, cf);
      GOAudioParams::SetCrossfadeMode(static_cast<GOCrossfadeMode>(cf));
    }
  }

  // It must be created before GOOrganModel::Load because lots of objects
  // reference to it
  GOOrganModel::SetCombinationController(m_setter);
  m_elementcreators.push_back(m_setter);

  GOOrganModel::Load(cfg);

  m_VirtualCouplers.Load(*this, cfg);

  GOOrganModel::LoadCmbButtons(cfg);

  m_DivisionalSetter = new GODivisionalSetter(this, m_setter->GetState());
  m_elementcreators.push_back(m_DivisionalSetter);
  m_AudioRecorder = new GOAudioRecorder(this);
  m_MidiRecorder = new GOMidiRecorder(this);
  m_MidiPlayer = new GOMidiPlayer(this);
  m_elementcreators.push_back(m_AudioRecorder);
  m_elementcreators.push_back(m_MidiPlayer);
  m_elementcreators.push_back(m_MidiRecorder);
  m_elementcreators.push_back(new GOMetronome(this));
  m_panelcreators.push_back(new GOGUICouplerPanel(this, m_VirtualCouplers));
  m_panelcreators.push_back(new GOGUICouplerManualsAndVolumePanel(this));
  m_panelcreators.push_back(new GOGUIMetronomePanel(this));
  m_panelcreators.push_back(new GOGUICrescendoPanel(this));
  m_panelcreators.push_back(new GOGUIDivisionalsPanel(this));
  m_panelcreators.push_back(new GOGUIBankedGeneralsPanel(this));
  m_panelcreators.push_back(new GOGUISequencerPanel(this));
  m_panelcreators.push_back(new GOGUIMasterPanel(this));
  m_panelcreators.push_back(new GOGUIRecorderPanel(this));

#ifdef GUI_GAP_TRACER
  {
    wxStopWatch __go_elcre_sw;
    for (unsigned i = 0; i < m_elementcreators.size(); i++)
      m_elementcreators[i]->Load(cfg);
    { wxString __log = wxString::Format("GUI.ElementCreators.Load total_ms=%ld creators=%u", __go_elcre_sw.Time(), (unsigned)m_elementcreators.size()); LOG_GUI_GAP("%s", __log); }
  }
#else
  for (unsigned i = 0; i < m_elementcreators.size(); i++)
    m_elementcreators[i]->Load(cfg);
#endif

  p_OnStateButton = GetButtonControl(GOSetter::KEY_ON_STATE);

  if (p_OnStateButton) {
    // we do not want to send midi events on m_OnStateButton together with
    // other events. They will be sent separately.
    UnRegisterLifecycleListener(p_OnStateButton);
  }

  m_PitchLabel.Load(cfg, wxT("SetterMasterPitch"), _("organ pitch"));
  m_TemperamentLabel.Load(
    cfg, wxT("SetterMasterTemperament"), _("temperament"));

  {
    wxStopWatch __go_mainwnd_sw;
    __go_mainwnd_sw.Start();
    m_MainWindowData.Load(cfg);
    LOG_TIMING("Timing: MainWindowData.Load %ld ms", __go_mainwnd_sw.Time());
  }

  // Load dialog sizes (measured)
  if (GODialogSizeSet::isPresentInCfg(cfg, CMBSetting)) {
    wxStopWatch __go_dialogsizes_sw;
    __go_dialogsizes_sw.Start();
    m_config.m_DialogSizes.Load(cfg, CMBSetting);
    LOG_TIMING("Timing: DialogSizes.Load %ld ms", __go_dialogsizes_sw.Time());
  }

  m_panels.resize(0);
  m_panels.push_back(new GOGUIPanel(this));

#ifdef GUI_GAP_TRACER
  {
    wxStopWatch __go_panelsload_sw;
    wxStopWatch __sw_panels;
    __sw_panels.Start();
    m_panels[0]->Load(cfg, wxT(""));
    wxString buffer;
    unsigned totalPanels = NumberOfPanels;
    // Panels occupy measured window.
    if (dlg) dlg->ResetRange(totalPanels, 30, 43, _("Loading panels"));
    for (unsigned i = 0; i < NumberOfPanels; i++) {
      buffer.Printf(wxT("Panel%03d"), i + 1);
      m_panels.push_back(new GOGUIPanel(this));
      // load the panel first so we can use its real display name in progress
      m_panels[m_panels.size() - 1]->Load(cfg, buffer);
      // update progress dialog with current panel name/count if available,
      // otherwise fall back to log entry
      if (dlg) {
        wxString panelName = m_panels[m_panels.size() - 1]->GetName();
        wxString msg = _("Loading panel: ");
        msg += panelName;
        msg += wxString::Format(_(" (%u/%u)"), (unsigned)m_panels.size(), totalPanels);
        if (!dlg->Update(i + 1, msg))
          ThrowGOLoadAborted(__func__);
      } else {
        wxString __panelLog = wxString::Format("Progress: Loading panel \"%s\" (%u/%u)", m_panels[m_panels.size() - 1]->GetName().c_str(), (unsigned)m_panels.size(), totalPanels);
        LOG_GUI_GAP("%s", __panelLog);
      }
    }

    __tim_panels_ms = __sw_panels.Time();
    { wxString __log = wxString::Format("GUI.Panels.Load total_ms=%ld panels=%u", __go_panelsload_sw.Time(), (unsigned)m_panels.size()); LOG_GUI_GAP("%s", __log); }
  }
#else
  {
    // Measure panels loading also in non-tracer builds to ensure we record
    // panel load time (was previously only measured under GUI_GAP_TRACER).
    wxStopWatch __go_panelsload_sw;
    __go_panelsload_sw.Start();

    m_panels[0]->Load(cfg, wxT(""));
    wxString buffer;
    unsigned totalPanels = NumberOfPanels;
    if (dlg) dlg->ResetRange(totalPanels, 35, 40, _("Loading panels"));
    for (unsigned i = 0; i < NumberOfPanels; i++) {
      buffer.Printf(wxT("Panel%03d"), i + 1);
      m_panels.push_back(new GOGUIPanel(this));
      // load panel first to get its name
      m_panels[m_panels.size() - 1]->Load(cfg, buffer);
      if (dlg) {
        wxString panelName = m_panels[m_panels.size() - 1]->GetName();
        wxString msg = _("Loading panel: ");
        msg += panelName;
        msg += wxString::Format(_(" (%u/%u)"), (unsigned)m_panels.size(), totalPanels);
        if (!dlg->Update(i + 1, msg))
          ThrowGOLoadAborted(__func__);
      } else {
        wxString __panelLog = wxString::Format("Progress: Loading panel \"%s\" (%u/%u)", m_panels[m_panels.size() - 1]->GetName().c_str(), (unsigned)m_panels.size(), totalPanels);
        LOG_GUI_GAP("%s", __panelLog);
      }
    }

    // store measured panel time into the member variable for final summary
    __tim_panels_ms = __go_panelsload_sw.Time();
    LOG_TIMING(wxString::Format("GUI.Panels.Load total_ms=%ld panels=%u", __go_panelsload_sw.Time(), (unsigned)m_panels.size()));
  }
#endif

  m_StopWindowSizeKeeper.Load(cfg, wxT("Stops"));

#ifdef GUI_GAP_TRACER
  {
    wxStopWatch __go_createpanels_sw;
    for (unsigned i = 0; i < m_panelcreators.size(); i++)
      m_panelcreators[i]->CreatePanels(cfg);
    { wxString __log = wxString::Format("GUI.CreatePanels total_ms=%ld creators=%u", __go_createpanels_sw.Time(), (unsigned)m_panelcreators.size()); LOG_GUI_GAP("%s", __log); }
  }
#else
  for (unsigned i = 0; i < m_panelcreators.size(); i++)
    m_panelcreators[i]->CreatePanels(cfg);
#endif

#ifdef GUI_GAP_TRACER
  {
    wxStopWatch __go_panelslayout_sw;
    for (unsigned i = 0; i < m_panels.size(); i++)
      m_panels[i]->Layout();
    { wxString __log = wxString::Format("GUI.Panels.Layout total_ms=%ld panels=%u", __go_panelslayout_sw.Time(), (unsigned)m_panels.size()); LOG_GUI_GAP("%s", __log); }
  }
#else
  for (unsigned i = 0; i < m_panels.size(); i++)
    m_panels[i]->Layout();
#endif

  const wxString &organName = GetOrganName();

  GetRootPipeConfigNode().SetName(organName);
  OnIsModifiedChanged(true);
  ReadCombinations(cfg);
  m_setter->OnCombinationsLoaded(GetCombinationsDir(), wxEmptyString);
  ResetOrganModified();

  GOHash hash;
  const auto organNameUtf8 = organName.utf8_str();

  hash.Update(organNameUtf8, strlen(organNameUtf8));
  GOHashType result = hash.getHash();
  m_SampleSetId1 = ((result.hash[0] & 0x7F) << 24)
    | ((result.hash[1] & 0x7F) << 16) | ((result.hash[2] & 0x7F) << 8)
    | (result.hash[3] & 0x7F);
  m_SampleSetId2 = ((result.hash[4] & 0x7F) << 24)
    | ((result.hash[5] & 0x7F) << 16) | ((result.hash[6] & 0x7F) << 8)
    | (result.hash[7] & 0x7F);
}

wxString GOOrganController::GenerateSettingFileName() {
  return m_config.OrganSettingsPath() + wxFileName::GetPathSeparator()
    + GOStdFileName::composeSettingFileName(GetOrganHash(), m_config.Preset());
}

wxString GOOrganController::GenerateCacheFileName() {
  return m_config.OrganCachePath() + wxFileName::GetPathSeparator()
    + GOStdFileName::composeCacheFileName(GetOrganHash(), m_config.Preset());
}

// Shared body for Phase 3 (Load) and Phase 6 (ApplyLutCacheNow).
static void ApplyLutReaderToOrgan(
  const GOLutCacheReader             &reader,
  const std::vector<GOCacheObject *> &cacheObjects) {
  const std::vector<int32_t>    &releaseMap = reader.GetReleaseMap();
  const std::vector<GOLutEntry> &lutEntries = reader.GetLuts();
  for (GOCacheObject *obj : cacheObjects) {
    GOSoundingPipe *pipe = obj->AsSoundingPipe();
    if (!pipe) continue;
    for (unsigned i = 0; i < pipe->GetReleaseCount(); i++) {
      const GOSoundAudioSection *sec = pipe->GetReleaseSection(i);
      if (!sec) continue;
      const unsigned parseIdx = sec->GetReleaseParseIndex();
      if (parseIdx >= (unsigned)releaseMap.size()) continue;
      const int32_t lutIdx = releaseMap[parseIdx];
      if (lutIdx < 0) continue;
      GOSoundReleaseAlignTable *aligner = sec->GetReleaseAligner();
      if (!aligner) continue;
      const GOLutEntry &entry = lutEntries[(unsigned)lutIdx];
      std::vector<GOSoundReleaseAlignTable::CorrPoint> pts;
      pts.reserve(entry.size());
      for (const GOLutPoint &pt : entry)
        pts.push_back({pt.loop_pos, pt.best_r});
      aligner->OverrideCorrLutsFromCache(std::move(pts));
    }
  }
}

bool GOOrganController::ApplyLutCacheNow() {
  const wxString path
    = GOLutCacheWriter::MakePath(m_config.OrganCachePath(), GetOrganHash());
  GOLutCacheReader reader;
  if (!reader.Load(path, m_ODFHash, m_lutReleaseCount))
    return false;
  ApplyLutReaderToOrgan(reader, GetCacheObjects());
  return true;
}

wxString GOOrganController::GetLutCachePath() const {
  return GOLutCacheWriter::MakePath(m_config.OrganCachePath(), m_hash);
}

void GOOrganController::ClearAllCachedLuts() {
  for (GOCacheObject *obj : GetCacheObjects()) {
    GOSoundingPipe *pipe = obj->AsSoundingPipe();
    if (!pipe) continue;
    for (unsigned i = 0; i < pipe->GetReleaseCount(); i++) {
      const GOSoundAudioSection *sec = pipe->GetReleaseSection(i);
      if (!sec) continue;
      GOSoundReleaseAlignTable *aligner = sec->GetReleaseAligner();
      if (aligner)
        aligner->ClearCachedLut();
    }
  }
}

void GOOrganController::DeleteLutCache() {
  const wxString path
    = GOLutCacheWriter::MakePath(m_config.OrganCachePath(), GetOrganHash());
  if (wxFileExists(path))
    wxRemoveFile(path);
}

bool GOOrganController::GenerateLutCache(wxString &errorMsg, bool forceAll) {
  // Always re-enumerate so the generator works even if Load() exited early.
  EnumerateReleaseParseIndices();

  if (m_lutReleaseCount == 0) {
    if (GetCacheObjects().empty()) {
      errorMsg = _("No organ loaded. Load an organ first.");
    } else {
      // Count sounding pipes to give a useful diagnostic.
      unsigned pipeCount = 0;
      for (const GOCacheObject *obj : GetCacheObjects())
        if (obj->AsSoundingPipe()) pipeCount++;
      errorMsg = wxString::Format(
        _("No release audio sections found (%u sounding pipes checked).\n"
          "The LUT cache requires loop-based samples (WAV files with loop and\n"
          "release markers).  Simple single-file or percussive pipes are not\n"
          "supported.\n\n"
          "If this organ should have looped releases, try deleting the\n"
          ".gorgan cache (File > Delete Cache) and reloading."),
        pipeCount);
    }
    return false;
  }

  std::vector<int32_t>    releaseMap(m_lutReleaseCount, -1);
  std::vector<GOLutEntry> luts;

  for (GOCacheObject *obj : GetCacheObjects()) {
    GOSoundingPipe *pipe = obj->AsSoundingPipe();
    if (!pipe) continue;
    for (unsigned i = 0; i < pipe->GetReleaseCount(); i++) {
      const GOSoundAudioSection *sec = pipe->GetReleaseSection(i);
      if (!sec) continue;
      const unsigned parseIdx = sec->GetReleaseParseIndex();
      if (parseIdx >= m_lutReleaseCount) continue;

      const GOSoundReleaseAlignTable *aligner = sec->GetReleaseAligner();
      std::vector<GOSoundReleaseAlignTable::CorrPoint> permPts;

      // Determine point source: live LUT or permissive recompute.
      // Multi-LUT releases (>1 attack joinable) are skipped: the .golut
      // format stores one LUT per release, and using only the first LUT
      // as a fallback for all attacks would silently ignore the others.
      const std::vector<GOSoundReleaseAlignTable::CorrPoint> *pts = nullptr;
      if (aligner && aligner->GetCorrLutCount() == 1) {
        pts = aligner->GetFirstLutPoints();
      } else if (forceAll && (!aligner || aligner->GetCorrLutCount() == 0)) {
        permPts = pipe->TryPermissiveLutForRelease(i);
        if (!permPts.empty()) pts = &permPts;
      }
      if (!pts || pts->empty()) continue;

      GOLutEntry entry;
      entry.reserve(pts->size());
      for (const auto &cp : *pts)
        entry.push_back({cp.loop_pos, cp.best_r});

      releaseMap[parseIdx] = (int32_t)luts.size();
      luts.push_back(std::move(entry));
    }
  }

  const wxString path
    = GOLutCacheWriter::MakePath(m_config.OrganCachePath(), GetOrganHash());

  GOLutGeneratorCriteria criteria; // default thresholds matching Python v47
  if (!GOLutCacheWriter::Write(
        path, m_ODFHash, m_lutReleaseCount, releaseMap, luts, criteria)) {
    errorMsg = wxString::Format(
      _("Failed to write LUT cache to %s"), path);
    return false;
  }
  return true;
}

unsigned GOOrganController::EnumerateReleaseParseIndices() {
  unsigned counter = 0;
  for (GOCacheObject *obj : GetCacheObjects()) {
    GOSoundingPipe *pipe = obj->AsSoundingPipe();
    if (pipe)
      counter = pipe->AssignReleaseParseIndices(counter);
  }
  m_lutReleaseCount = counter;
  return counter;
}

 
wxString GOOrganController::Load(
  GOProgressDialog *dlg,
  const GOOrgan &organ,
  const wxString &file2,
  bool isGuiOnly) {
  GOBuffer<char> dummy;
  wxString errMsg;
  // timing variables are now class members; initialize them here so Load
  // reuses the same measurements that ReadOrganFile (and other methods)
  // populate (avoid local shadowing which gives 0 ms results).
  __tim_parse_ms = 0;
  __tim_cmb_ms = 0;
  __tim_readorgan_ms = 0;
  __tim_cache_ms = 0;
  __tim_panels_ms = 0;
  __tim_ranks_ms = 0;
  __tim_modelrest_ms = 0;
#ifdef GO_PROFILE_ODFLOAD
  wxStopWatch sw_total;
  sw_total.Start();
  wxStopWatch sw_phase;
  sw_phase.Start();
#endif

  try {
    GOLoaderFilename odf_name;

    m_ArchiveID = organ.GetArchiveID();
    if (m_ArchiveID != wxEmptyString) {
      dlg->Setup(1, _("Loading sample set"), _("Parsing organ packages"));

      wxString errMsg1;

      if (!m_FileStore.LoadArchives(
            m_config,
            m_config.OrganCachePath(),
            organ.GetArchiveID(),
            organ.GetArchivePath(),
            errMsg1))
        throw errMsg1;
      m_ArchivePath = organ.GetArchivePath();
      m_odf = organ.GetODFPath();
      odf_name.Assign(m_odf);
    } else {
      wxString file = organ.GetODFPath();
      m_odf = go_normalize_path(file);
      odf_name.AssignAbsolute(m_odf);
      m_FileStore.SetDirectory(go_get_path(m_odf));
    }
    m_hash = organ.GetOrganHash();
    dlg->Setup(
      1, _("Loading sample set"), _("Parsing sample set definition file"));
    m_SettingFilename = GenerateSettingFileName();
    m_CacheFilename = GenerateCacheFileName();
    m_Cacheable = false;

    GOConfigFileReader odf_ini_file;

    {
      auto opened = odf_name.Open(m_FileStore);

      // Install a simple progress sink: forward percentages/units to the dialog.
      // We will use ResetRange before each major phase so Update() receives
      // values appropriate for the current segment (e.g. 0..100 for percent,
      // or n objects for object lists).
      SetProgressSink([dlg](unsigned pc, const wxString &msg) {
        // If the user cancelled via the dialog, Update returns false.
        // Propagate a cooperative abort so the surrounding load logic can
        // stop promptly and consistently.
        if (!dlg->Update(pc, msg))
          ThrowGOLoadAbortedEarly(__func__);
      });
      // Parsing is a percent-based phase (0..100). Use measured window.
      if (dlg) dlg->ResetRange(100, 0, 1, _("Parsing sample set definition file"));

      // measure parsing time
      wxStopWatch __sw_parse;
      __sw_parse.Start();
      bool ok = odf_ini_file.ReadWithProgress(
        opened.get(),
        _("Parsing sample set definition file"),
        [dlg](unsigned pc, const wxString &msg) {
          // Forward percent directly (0..100). ResetRange above maps this
          // into the 0..3% window.
          if (!dlg->Update(pc, msg))
            ThrowGOLoadAbortedEarly(__func__);
        });
      __tim_parse_ms = __sw_parse.Time();
      if (!ok)
        throw wxString::Format(_("Unable to read '%s'"), odf_name.GetPath());
    }

    m_ODFHash = odf_ini_file.GetHash();
    m_b_customized = false;
    GOConfigReaderDB ini(m_config.ODFCheck());
    ini.ReadData(odf_ini_file, ODFSetting, false);

    wxString setting_file = file2;
    bool can_read_cmb_directly = true;

    if (setting_file.IsEmpty()) {
      if (wxFileExists(m_SettingFilename)) {
        setting_file = m_SettingFilename;
        m_b_customized = true;
      } else {
        wxString bundledSettingsFile = m_odf.BeforeLast('.') + wxT(".cmb");
        if (!m_FileStore.AreArchivesUsed()) {
          if (wxFileExists(bundledSettingsFile)) {
            setting_file = bundledSettingsFile;
            m_b_customized = true;
          }
        } else {
          if (m_FileStore.FindArchiveContaining(m_odf)->containsFile(
                bundledSettingsFile)) {
            setting_file = bundledSettingsFile;
            m_b_customized = true;
            can_read_cmb_directly = false;
          }
        }
      }
    }

      if (!setting_file.IsEmpty()) {
      GOConfigFileReader extra_odf_config;
      // Read organ settings (.cmb). Use measured window.
      if (dlg) dlg->ResetRange(100, 1, 3, _("Reading organ settings (.cmb)"));
      // measure cmb read time
      wxStopWatch __sw_cmb;
      __sw_cmb.Start();
      if (can_read_cmb_directly) {
        GOStandardFile cmbFile(setting_file);
        if (
          !extra_odf_config.ReadWithProgress(
            &cmbFile,
            _("Reading organ settings (.cmb)"),
            [dlg](unsigned pc, const wxString &msg) {
              // .cmb is small but treat it as percent-based and map into the
              // model percent window (3..19%). ResetRange will be set before
              // calling this block.
              if (!dlg->Update(pc, msg))
                ThrowGOLoadAborted(__func__);
            }))
          throw wxString::Format(_("Unable to read '%s'"), setting_file);
      } else {
        GOOpenedFile *cmbFromArchive
          = m_FileStore.FindArchiveContaining(m_odf)->OpenFile(setting_file);
        if (
          !extra_odf_config.ReadWithProgress(
            cmbFromArchive,
            _("Reading organ settings (.cmb)"),
            [dlg](unsigned pc, const wxString &msg) {
              if (!dlg->Update(pc, msg))
                ThrowGOLoadAborted(__func__);
            }))
          throw wxString::Format(_("Unable to read '%s'"), setting_file);
      }
      __tim_cmb_ms = __sw_cmb.Time();

      if (
        odf_ini_file.getEntry(WX_ORGAN, wxT("ChurchName")).Trim()
        != extra_odf_config.getEntry(WX_ORGAN, wxT("ChurchName")).Trim())
        wxLogWarning(
          _("This .cmb file was originally created for:\n%s"),
          extra_odf_config.getEntry(WX_ORGAN, wxT("ChurchName")).c_str());

      ini.ReadData(extra_odf_config, CMBSetting, false);
      wxString hash = extra_odf_config.getEntry(WX_ORGAN, wxT("ODFHash"));
      if (hash != wxEmptyString)
        if (hash != m_ODFHash) {
          if (
            wxMessageBox(
              _("The .cmb file does not exactly match the current "
                "ODF. Importing it can cause various problems. "
                "Should it really be imported?"),
              _("Import"),
              wxYES_NO,
              NULL)
            == wxNO) {
            ini.ClearCMB();
          }
        }
    } else {
      bool old_go_settings = ini.ReadData(odf_ini_file, CMBSetting, true);
      if (old_go_settings)
        if (
          wxMessageBox(
            _("The ODF contains GrandOrgue 0.2 styled saved "
              "settings. Should they be imported?"),
            _("Import"),
            wxYES_NO,
            NULL)
          == wxNO) {
          ini.ClearCMB();
        }
    }

    GOConfigReader cfg(ini, m_config.ODFCheck(), m_config.ODFHw1Check());

    /* skip informational items */
    cfg.ReadString(CMBSetting, WX_ORGAN, wxT("ChurchName"), false);
    cfg.ReadString(CMBSetting, WX_ORGAN, wxT("ChurchAddress"), false);
    cfg.ReadString(CMBSetting, WX_ORGAN, wxT("ODFPath"), false);
    cfg.ReadString(CMBSetting, WX_ORGAN, wxT("ODFHash"), false);
    cfg.ReadString(CMBSetting, WX_ORGAN, wxT("ArchiveID"), false);
    // Model progress sink already set earlier; set model percent window then call ReadOrganFile.
    // Map entire model (ranks + rest) into measured window.
    if (dlg) dlg->ResetRange(100, 3, 30, _("Building model"));
    wxStopWatch __sw_readorgan;
    __sw_readorgan.Start();
    ReadOrganFile(cfg, dlg);
    __tim_readorgan_ms = __sw_readorgan.Time();
    // obtain model sub-step timings measured inside GOOrganModel::Load
    __tim_ranks_ms = GetModelRanksMs();
    __tim_modelrest_ms = GetModelRestMs();
    SetProgressSink({});
    if (m_config.ODFCheck())
      ini.ReportUnused();

    if (!isGuiOnly) {
      try {
        bool cache_ok = false;

        dummy.resize(1024 * 1024 * 50);
#ifdef GO_PROFILE_ODFLOAD
        LOG_TIMING(wxString::Format("Timing: Parse/ODF processing: %ld ms", sw_phase.Time()));
        sw_phase.Start();
#endif

        // Measure any delay between model completion and start of reference resolution
        wxStopWatch __go_postmodel_sw;
        __go_postmodel_sw.Start();

        dlg->Reset(1, _("Resolving object references"));
        ResolveReferences();

        // Time from end of model -> end of ResolveReferences
        LOG_TIMING(wxString::Format("Timing: Post-model -> ResolveReferences total %ld ms", __go_postmodel_sw.Time()));

#ifdef GO_PROFILE_ODFLOAD
        LOG_TIMING(wxString::Format("Timing: ResolveReferences: %ld ms", sw_phase.Time()));
        sw_phase.Start();
#endif

        /* Figure out list of pipes to load */
#ifdef GO_PROFILE_ODFLOAD
        LOG_TIMING("Timing: Preparing audio objects...");
#endif
        dlg->Reset(1, _("Preparing audio objects"));
        GOCacheObjectDistributor objectDistributor(GetCacheObjects());

        // Audio loading occupies the remaining portion (mapped to objects).
        // Each object contributes one unit; total units = object count.
        // Defensive: callers may report positions in range [0..N], ensure the
        // progress dialog uses a unit count that prevents off-by-one producing
        // intermediate values > 100% in some environments.
        unsigned __objUnits = objectDistributor.GetNObjects() ? objectDistributor.GetNObjects() : 1;
        // Use the reported object count directly here; GOProgressDialog now maps
        // value/units defensively. For non-GUI (no dlg) use the same units.
        if (dlg) dlg->ResetRange(__objUnits, 43, 100, _("Loading audio data (cache/disk)"));
        else dlg->Reset(__objUnits, _("Loading audio data (cache/disk)"));

        wxStopWatch __sw_cache;
        __sw_cache.Start();
        GOCacheObject *obj = nullptr;

        /* Load pipes */
        // Measure small sequence: preparing->cache open/header
        if (wxFileExists(m_CacheFilename)) {
          wxStopWatch __go_prep_to_cache_sw;
          __go_prep_to_cache_sw.Start();

          wxFile cache_file(m_CacheFilename);
          GOCache reader(cache_file, m_pool);
          cache_ok = cache_file.IsOpened();

          if (cache_ok) {
            GOHashType hash1, hash2;
            if (!reader.ReadHeader()) {
              cache_ok = false;
              wxLogWarning(_("Cache file had bad magic bypassing cache."));
            }
            hash1 = GenerateCacheHash();
            if (!reader.Read(&hash2, sizeof(hash2)) || memcmp(&hash1, &hash2, sizeof(hash1))) {
              cache_ok = false;
              reader.FreeCacheFile();
              wxLogWarning(_("Cache file had diffent hash bypassing cache."));
            }
          }

          // Log time taken to reach cache-open+header stage (aggregated small log)
          { wxString __log = wxString::Format("Timing: PreparingObjects->CacheOpenHeader %ld ms", __go_prep_to_cache_sw.Time()); LOG_TIMING("%s", __log); }

          GOCacheObject *obj = nullptr;

          if (cache_ok) {
            // Aggregated summary: total objects, total time, average time (single line)
            long long __go_cache_total_ms = 0;
            long long __go_cache_objects = 0;

            // Logpoint immediately before cache deserialization starts
            { wxString __log = wxString::Format("Timing: BeforeCacheDeserialization objects=%u", objectDistributor.GetNObjects()); LOG_TIMING("%s", __log); }

            // Aggregate timing by title (top-N reporting)
            struct __Agg { uint32_t count = 0; long long total_ms = 0; };
            std::unordered_map<std::string, __Agg> __go_byTitle;

            while ((obj = objectDistributor.FetchNext())) {
              wxStopWatch __go_obj_sw;
              __go_obj_sw.Start();

              if (!obj->LoadFromCacheWithoutExc(m_pool, reader)) {
                wxLogWarning(_("Cache load failure: %s"), obj->GetLoadError());
                break;
              }

              long long __ms = __go_obj_sw.Time();
              __go_cache_total_ms += __ms;
              __go_cache_objects++;

              // collect by-title aggregates
              std::string __title = std::string(obj->GetLoadTitle().utf8_str());
              auto &ent = __go_byTitle[__title];
              ent.count++;
              ent.total_ms += __ms;

              // Time-based progress estimation (on-the-fly)
              unsigned __processed = __go_cache_objects;
              unsigned __total = __objUnits;
              unsigned __syntheticUnit = __processed; // fallback to count
              if (__processed > 0 && __total > 0) {
                double avg_ms = (double)__go_cache_total_ms / (double)__processed;
                unsigned long long remaining = (__total > __processed) ? (__total - __processed) : 0;
                double rem_est_ms = avg_ms * (double)remaining;
                double frac_time = 0.0;
                double denom = (double)__go_cache_total_ms + rem_est_ms;
                if (denom > 0.0)
                  frac_time = (double)__go_cache_total_ms / denom;
                else
                  frac_time = (double)__processed / (double)__total;
                if (frac_time < 0.0) frac_time = 0.0;
                if (frac_time > 1.0) frac_time = 1.0;
                long long rounded = std::llround(frac_time * (double)__total);
                if (rounded < 1 && __processed > 0)
                  rounded = 1;
                if ((unsigned)rounded > __total)
                  rounded = __total;
                __syntheticUnit = (unsigned)rounded;
              }

              if (!dlg->Update(__syntheticUnit, obj->GetLoadTitle()))
                ThrowGOLoadAbortedPartial(__func__); // Skip the rest of the loading code
            }

            if (!obj)
              m_Cacheable = true;
            else
              // obj points to an object with a load error. We will try to load
              // it from the file later
              cache_ok = false;

            if (__go_cache_objects > 0) {
              long long __go_cache_avg_ms = __go_cache_total_ms / __go_cache_objects;
              { wxString __log = wxString::Format(
                "Timing: Cache load summary total_objects=%lld total_ms=%lld avg_ms=%lld",
                __go_cache_objects, __go_cache_total_ms, __go_cache_avg_ms); LOG_TIMING("%s", __log); }

              // compute top 10 by total_ms
              std::vector<std::pair<std::string, __Agg>> __vec;
              __vec.reserve(__go_byTitle.size());
              for (auto &p : __go_byTitle)
                __vec.emplace_back(p.first, p.second);
              std::sort(__vec.begin(), __vec.end(), [](auto &a, auto &b) {
                return a.second.total_ms > b.second.total_ms;
              });
              int __topN = std::min<int>(10, (int)__vec.size());
              for (int i = 0; i < __topN; ++i) {
                auto &it = __vec[i];
                double avg = it.second.count ? (double)it.second.total_ms / it.second.count : 0.0;
                { wxString __log = wxString::Format(
                  "Timing: Cache.byTitle[%d] title=\"%s\" count=%u total_ms=%lld avg_ms=%.2f",
                  i + 1, it.first.c_str(), it.second.count, it.second.total_ms, avg); LOG_TIMING("%s", __log); }
              }
            }
          }

          if (!cache_ok && !m_config.ManageCache())
            wxLogWarning(
              _("The cache for this organ is outdated. Please update "
                "or delete it."));

          reader.Close();
        }

          if (!cache_ok) {
            GOAudioParams::SetCorrLutDownsampling(
              m_config.CorrLutDownsampling());
            GOLoadWorker thisWorker(m_FileStore, m_pool, objectDistributor);
            ptr_vector<GOLoadThread> threads;

            // Create and run additional worker threads
            for (unsigned i = 0; i < m_config.LoadConcurrency(); i++)
              threads.push_back(
                new GOLoadThread(m_FileStore, m_pool, objectDistributor));
            for (unsigned i = 0; i < threads.size(); i++)
              threads[i]->Run();

            // try to load the object that we could not load from cache
            if (obj)
              thisWorker.LoadObjectNoExc(obj);

            while (true) {
              if (!thisWorker.LoadNextObject(obj))
                break;

              // show the progress and process possible Cancel
              if (!dlg->Update(objectDistributor.GetPos(), obj->GetLoadTitle()))
                ThrowGOLoadAbortedPartial(__func__); // skip the rest loading code
            }

          // rethrow exception if any occured in thisWorker.LoadNextObject
            bool wereExceptions = thisWorker.WereExceptions();

          for (unsigned i = 0; i < threads.size(); i++)
            wereExceptions |= threads[i]->CheckExceptions();

          // Detect cooperative aborts originating inside worker threads.
          bool anyUserAbort = false;
          for (unsigned i = 0; i < threads.size(); i++) {
            if (threads[i]->WasUserAbort()) {
              anyUserAbort = true;
              break;
            }
          }

          if (anyUserAbort) {
            // User cancelled during worker activity; show partial-load warning
            // and avoid updating the cache.
            GOMessageBox(
              _("Load aborted by the user - only parts of the organ are loaded."),
              _("Load error"),
              wxOK | wxICON_ERROR,
              NULL);
            m_Cacheable = false;
          } else if (wereExceptions) {
            for (auto obj : GetCacheObjects()) {
              if (!obj->IsReady())
                wxLogError(obj->GetLoadError());
            }
            GOMessageBox(
              _("There are errors while loading the organ. See Log Messages."),
              _("Load error"),
              wxOK | wxICON_ERROR,
              NULL);
          } else {
            if (objectDistributor.IsComplete())
              m_Cacheable = true;
            if (m_config.ManageCache() && m_Cacheable)
              UpdateCache(dlg, m_config.CompressCache());
          }

          // Despite a possible exception automatic calling ~GOLoadThread from
          // ~ptr_vector stops all additional worker threads
        }
      __tim_cache_ms = __sw_cache.Time();

      // Assign sequential parse indices to all release sections.
      // The resulting count is stored in m_lutReleaseCount and used as
      // releaseCount in the LUT cache file header.
      EnumerateReleaseParseIndices();

      // Phase 3: Apply pre-computed LUT cache if available.
      // Silently ignored on any mismatch (wrong ODF, version, count).
      // Safe: PreparePlayback has not been called yet, no audio thread active.
      {
        const wxString lutPath = GOLutCacheWriter::MakePath(
          m_config.OrganCachePath(), GetOrganHash());
        GOLutCacheReader lutReader;
        if (lutReader.Load(lutPath, m_ODFHash, m_lutReleaseCount))
          ApplyLutReaderToOrgan(lutReader, GetCacheObjects());
      }

    } catch (const GOOutOfMemory &e) {
        GOMessageBox(
          _("Out of memory - only parts of the organ are loaded. Please "
            "reduce memory footprint via the sample loading settings."),
          _("Load error"),
          wxOK | wxICON_ERROR,
          NULL);
      } catch (const GOLoadAbortedPartial &) {
        // User aborted during the audio/cache phase: show the standard
        // partial-load warning but continue so the rest of the cleanup and
        // possible partial initialization proceeds as before.
        GOMessageBox(
          _("Load aborted by the user - only parts of the organ are loaded."),
          _("Load error"),
          wxOK | wxICON_ERROR,
          NULL);
      } catch (const GOLoadAbortedEarly &) {
        // Early abort (pre-audio): treat as a controlled user cancel.
        // Show the user message, close archives and return the sentinel so
        // higher-level callers know this was a user cancel and should not log.
        wxString userCancel = wxT("!");
        GOMessageBox(
          _("Load aborted by the user - loading cancelled."),
          _("Load error"),
          wxOK | wxICON_ERROR,
          NULL);
        m_FileStore.CloseArchives();
        return userCancel;
      }
    }
  } catch (const wxString &error_) {
    errMsg = error_;
  } catch (const GOLoadAbortedEarly &) {
    // Early abort (pre-audio): treat as a controlled user cancel.
    // Show the user message, close archives and return the sentinel so
    // higher-level callers know this was a user cancel and should not log.
    wxString userCancel = wxT("!");
    GOMessageBox(
      _("Load aborted by the user - loading cancelled."),
      _("Load error"),
      wxOK | wxICON_ERROR,
      NULL);
    m_FileStore.CloseArchives();
    return userCancel;
  } catch (const std::exception &e) {
    errMsg = e.what();
  } catch (...) { // We must not allow unhandled exceptions here
    errMsg.Printf("Unknown exception");
  }
  // Final measured totals and percentages (logged for analysis)
  // include detailed measured sub-times in the final summary
  long long totalMeasured = __tim_parse_ms + __tim_cmb_ms + __tim_ranks_ms + __tim_modelrest_ms + __tim_panels_ms + __tim_cache_ms;
  if (totalMeasured == 0)
    totalMeasured = 1;
  double p_parse = (double)__tim_parse_ms * 100.0 / (double)totalMeasured;
  double p_cmb = (double)__tim_cmb_ms * 100.0 / (double)totalMeasured;
  double p_ranks = (double)__tim_ranks_ms * 100.0 / (double)totalMeasured;
  double p_modelrest = (double)__tim_modelrest_ms * 100.0 / (double)totalMeasured;
  double p_panels = (double)__tim_panels_ms * 100.0 / (double)totalMeasured;
  double p_cache = (double)__tim_cache_ms * 100.0 / (double)totalMeasured;
  { wxString __log = wxString::Format("Timing: Percentages parse=%.2f%% cmb=%.2f%% ranks=%.2f%% modelrest=%.2f%% panels=%.2f%% cache=%.2f%% total_ms=%lld",
    p_parse, p_cmb, p_ranks, p_modelrest, p_panels, p_cache, totalMeasured); LOG_TIMING("%s", __log); }
  // raw ms values as requested
  { wxString __log2 = wxString::Format("Timing: Measured parse=%lld cmb=%lld ranks=%lld modelrest=%lld panels=%lld cache=%lld ms",
    __tim_parse_ms, __tim_cmb_ms, __tim_ranks_ms, __tim_modelrest_ms, __tim_panels_ms, __tim_cache_ms); LOG_TIMING("%s", __log2); }

  dummy.free();
  m_FileStore.CloseArchives();
  if (errMsg.IsEmpty())
    SetTemperament(m_Temperament);
  return errMsg;
}

// const wxString &WX_CMB = wxT(".cmb");
const wxString &WX_YAML = wxT("yaml");
const wxString WX_GRANDORGUE_COMBINATIONS = "GrandOrgue Combinations";

wxString GOOrganController::ExportCombination(const wxString &fileName) {
  GOYamlModel::Out yamlOut(GetOrganName(), WX_GRANDORGUE_COMBINATIONS);

  yamlOut << *m_setter;
  yamlOut << *m_DivisionalSetter;

  const wxString errMsg = yamlOut.writeTo(fileName);

  m_setter->OnCombinationsSaved(fileName);
  return errMsg;
}

void GOOrganController::LoadCombination(const wxString &file) {
  wxString errMsg;
  const wxFileName fileName(file);

  try {
    const wxString fileExt = fileName.GetExt();

    if (fileExt == WX_YAML) {
      GOYamlModel::In inYaml(GetOrganName(), file, WX_GRANDORGUE_COMBINATIONS);

      if (is_to_import_to_this_organ(
            GetOrganName(),
            WX_GRANDORGUE_COMBINATIONS,
            file,
            inYaml.GetFileOrganName())) {
        inYaml >> *m_setter;
        inYaml >> *m_DivisionalSetter;
        m_setter->OnCombinationsLoaded(fileName.GetPath(), file);
      }
    } else {
      GOConfigFileReader odf_ini_file;

      if (!odf_ini_file.Read(file))
        throw wxString::Format(_("Unable to read '%s'"), file.c_str());

      GOConfigReaderDB ini;
      ini.ReadData(odf_ini_file, CMBSetting, false);
      GOConfigReader cfg(ini);
      wxString fileOrganName
        = cfg.ReadString(CMBSetting, WX_ORGAN, wxT("ChurchName"));

      if (is_to_import_to_this_organ(
            GetOrganName(), wxT("Organ Settings"), file, fileOrganName)) {
        wxString hash = odf_ini_file.getEntry(WX_ORGAN, wxT("ODFHash"));
        if (hash != wxEmptyString)
          if (hash != m_ODFHash) {
            wxLogWarning(_(
              "The combination file does not exactly match the current ODF."));
          }
        /* skip informational items */
        cfg.ReadString(CMBSetting, WX_ORGAN, wxT("ChurchAddress"), false);
        cfg.ReadString(CMBSetting, WX_ORGAN, wxT("ODFPath"), false);

        ReadCombinations(cfg);
        m_setter->OnCombinationsLoaded(GetCombinationsDir(), wxEmptyString);
      }
    }
    SetOrganModified();
  } catch (const wxString &error) {
    errMsg = error;
  } catch (const std::exception &e) {
    errMsg = e.what();
  } catch (...) { // We must not allow unhandled exceptions here
    errMsg.Printf("Unknown exception");
  }
  if (!errMsg.IsEmpty()) {
    wxLogError(errMsg);
    GOMessageBox(errMsg, _("Load error"), wxOK | wxICON_ERROR, NULL);
  }
}

bool GOOrganController::UpdateCache(GOProgressDialog *dlg, bool compress) {
  bool isOk = false;

  DeleteCache();

  /* Figure out the list of pipes to save */
  GOCacheObjectDistributor objectDistributor(GetCacheObjects());

  dlg->Setup(objectDistributor.GetNObjects(), _("Creating sample cache"));

  wxFileOutputStream file(m_CacheFilename);

  if (file.IsOk()) {
    GOCacheWriter writer(file, compress);

    /* Save pipes to cache */
    isOk = writer.WriteHeader();

    GOHashType hash = GenerateCacheHash();
    if (!writer.Write(&hash, sizeof(hash)))
      isOk = false;

    while (isOk) {
      GOCacheObject *obj = objectDistributor.FetchNext();

      if (!obj)
        break;
      if (!obj->SaveCache(writer)) {
        isOk = false;
        wxLogError(
          _("Save of %s to the cache failed"), obj->GetLoadTitle().c_str());
      }
      if (!dlg->Update(objectDistributor.GetPos(), obj->GetLoadTitle())) {
        writer.Close();
        DeleteCache();
        isOk = false;
      }
    }
    writer.Close();
    if (!isOk)
      DeleteCache();
  } else
    wxLogError(_("Opening the cache file %s failed"), m_CacheFilename);
  return isOk;
}

void GOOrganController::DeleteCache() {
  if (CachePresent())
    wxRemoveFile(m_CacheFilename);
}

void GOOrganController::DeleteSettings() { wxRemoveFile(m_SettingFilename); }

bool GOOrganController::Save() {
  if (!Export(m_SettingFilename))
    return false;
  ResetOrganModified();
  return true;
}

bool GOOrganController::Export(const wxString &cmb) {
  GOConfigFileWriter cfg_file;
  GOConfigWriter cfg(cfg_file, false);

  m_b_customized = true;
  cfg.WriteString(WX_ORGAN, wxT("ODFHash"), m_ODFHash);
  cfg.WriteString(WX_ORGAN, wxT("ChurchName"), GetOrganName());
  cfg.WriteString(WX_ORGAN, wxT("ChurchAddress"), m_ChurchAddress);
  cfg.WriteString(WX_ORGAN, wxT("ODFPath"), GetODFFilename());
  if (m_ArchiveID != wxEmptyString)
    cfg.WriteString(WX_ORGAN, wxT("ArchiveID"), m_ArchiveID);
  cfg.WriteString(WX_ORGAN, WX_GRANDORGUE_VERSION, wxT(APP_VERSION));
  cfg.WriteInteger(WX_ORGAN, wxT("Volume"), m_volume);
  cfg.WriteString(WX_ORGAN, wxT("Temperament"), m_Temperament);

  // Persist current crossfade mode for this organ
  {
    const long cf = static_cast<long>(GOAudioParams::GetCrossfadeMode());
    cfg.WriteInteger(WX_ORGAN, wxT("CrossfadeMode"), cf);
  }

  GOEventDistributor::Save(cfg);
  GetDialogSizeSet().Save(cfg);
  m_StopWindowSizeKeeper.Save(cfg);
  m_VirtualCouplers.Save(cfg);

  wxString tmp_name = cmb + wxT(".new");

  if (::wxFileExists(tmp_name) && !::wxRemoveFile(tmp_name)) {
    wxLogError(_("Could not write to '%s'"), tmp_name);
    return false;
  }
  if (!cfg_file.Save(tmp_name)) {
    wxLogError(_("Could not write to '%s'"), tmp_name);
    return false;
  }
  if (!go_rename_file(tmp_name, cmb))
    return false;
  return true;
}

GOEnclosure *GOOrganController::GetEnclosure(
  const wxString &name, bool is_panel) {
  for (unsigned i = 0; i < m_elementcreators.size(); i++) {
    GOEnclosure *c = m_elementcreators[i]->GetEnclosure(name, is_panel);
    if (c)
      return c;
  }
  return NULL;
}

GOLabelControl *GOOrganController::GetLabel(
  const wxString &name, bool is_panel) {
  for (unsigned i = 0; i < m_elementcreators.size(); i++) {
    GOLabelControl *c = m_elementcreators[i]->GetLabelControl(name, is_panel);
    if (c)
      return c;
  }
  return NULL;
}

GOButtonControl *GOOrganController::GetButtonControl(
  const wxString &name, bool is_panel) {
  for (unsigned i = 0; i < m_elementcreators.size(); i++) {
    GOButtonControl *c = m_elementcreators[i]->GetButtonControl(name, is_panel);
    if (c)
      return c;
  }
  return NULL;
}

const wxString GOOrganController::GetOrganPathInfo() {
  if (m_ArchiveID == wxEmptyString)
    return GetODFFilename();
  const GOArchiveFile *archive = m_config.GetArchiveByID(m_ArchiveID);
  wxString name = GetODFFilename();
  if (archive)
    name += wxString::Format(
      _(" from '%s' (%s)"), archive->GetName().c_str(), m_ArchiveID.c_str());
  else
    name += wxString::Format(_(" from %s"), m_ArchiveID.c_str());
  return name;
}

GOOrgan GOOrganController::GetOrganInfo() {
  return GOOrgan(
    GetODFFilename(),
    m_ArchiveID,
    m_ArchivePath,
    GetOrganName(),
    GetOrganBuilder(),
    GetRecordingDetails());
}

wxString GOOrganController::GetCombinationsDir() const {
  return wxFileName(m_config.OrganCombinationsPath(), GetOrganName())
    .GetFullPath();
}

void GOOrganController::LoadMIDIFile(wxString const &filename) {
  m_MidiPlayer->LoadFile(
    filename, GetODFManualCount() - 1, GetFirstManualIndex() == 0);
}

void GOOrganController::Abort() {
  m_soundengine = NULL;

  GOEventDistributor::AbortPlayback();

  m_MidiPlayer->Cleanup();
  m_MidiRecorder->StopRecording();
  m_AudioRecorder->StopRecording();
  m_AudioRecorder->SetAudioRecorder(NULL);
  if (p_OnStateButton)
    p_OnStateButton->AbortPlayback();
  GOOrganModel::GOSoundOrganInterfaceProxy::Disconnect();
  GOOrganModel::SetMidi(nullptr, nullptr);
  m_midi = NULL;
}

void GOOrganController::PreconfigRecorder() {
  for (unsigned i = GetFirstManualIndex(); i <= GetManualAndPedalCount(); i++) {
    wxString id = wxString::Format(wxT("M%d"), i);
    m_MidiRecorder->PreconfigureMapping(id, false);
  }
}

void GOOrganController::PreparePlayback(
  GOSoundOrganEngine *engine, GOMidiSystem *midi, GOSoundRecorder *recorder) {
  m_soundengine = engine;
  m_midi = midi;
  m_MidiRecorder->SetOutputDevice(m_config.MidiRecorderOutputDevice());
  m_AudioRecorder->SetAudioRecorder(recorder);

  m_MidiRecorder->Clear();
  PreconfigRecorder();
  m_MidiRecorder->SetSamplesetId(m_SampleSetId1, m_SampleSetId2);
  PreconfigRecorder();

  m_MidiSamplesetMatch.clear();
  GOOrganModel::SetMidi(midi, m_MidiRecorder);
  GOOrganModel::GOSoundOrganInterfaceProxy::Connect(engine);
  GOEventDistributor::PreparePlayback();

  m_setter->UpdateModified(m_OrganModified);

  GOEventDistributor::StartPlayback();
  GOEventDistributor::PrepareRecording();
  m_MidiPlayer->Setup(midi);

  // Light the OnState button
  if (p_OnStateButton) {
    p_OnStateButton->PreparePlayback();
    p_OnStateButton->StartPlayback();
    p_OnStateButton->PrepareRecording();
  }
}

void GOOrganController::PrepareRecording() {
  m_MidiRecorder->Clear();
  PreconfigRecorder();
  m_MidiRecorder->SetSamplesetId(m_SampleSetId1, m_SampleSetId2);
  PreconfigRecorder();

  GOEventDistributor::PrepareRecording();
}

void GOOrganController::Update() {
  for (unsigned i = 0; i < m_switches.size(); i++)
    m_switches[i]->Update();

  for (unsigned i = m_FirstManual; i < m_manuals.size(); i++)
    m_manuals[i]->Update();

  for (unsigned i = 0; i < m_tremulants.size(); i++)
    m_tremulants[i]->Update();

  for (unsigned i = 0; i < m_DivisionalCoupler.size(); i++)
    m_DivisionalCoupler[i]->Update();

  m_setter->Update();
}

void GOOrganController::ProcessMidi(const GOMidiEvent &event) {
  if (event.GetMidiType() == GOMidiEvent::MIDI_RESET) {
    Reset();
    return;
  }
  while (m_MidiSamplesetMatch.size() < event.GetDevice())
    m_MidiSamplesetMatch.push_back(true);

  if (event.GetMidiType() == GOMidiEvent::MIDI_SYSEX_GO_CLEAR)
    m_MidiSamplesetMatch[event.GetDevice()] = true;
  else if (event.GetMidiType() == GOMidiEvent::MIDI_SYSEX_GO_SAMPLESET) {
    if (
      event.GetKey() == m_SampleSetId1 && event.GetValue() == m_SampleSetId2) {
      m_MidiSamplesetMatch[event.GetDevice()] = true;
    } else {
      m_MidiSamplesetMatch[event.GetDevice()] = false;
      return;
    }
  } else if (event.GetMidiType() == GOMidiEvent::MIDI_SYSEX_GO_SETUP) {
    if (!m_MidiSamplesetMatch[event.GetDevice()])
      return;
  }

  GOEventDistributor::SendMidi(event);
}

void GOOrganController::Reset() {
  for (unsigned l = 0; l < GetSwitchCount(); l++)
    GetSwitch(l)->Reset();
  for (unsigned k = GetFirstManualIndex(); k <= GetManualAndPedalCount(); k++)
    GetManual(k)->Reset();
  for (unsigned l = 0; l < GetTremulantCount(); l++)
    GetTremulant(l)->Reset();
  for (unsigned j = 0; j < GetDivisionalCouplerCount(); j++)
    GetDivisionalCoupler(j)->Reset();
  for (unsigned k = 0; k < GetGeneralCount(); k++)
    GetGeneral(k)->Display(false);
  m_setter->ResetCmbButtons();
}

void GOOrganController::SetTemperament(const GOTemperament &temperament) {
  m_TemperamentLabel.SetContent(temperament.GetTitle());
  for (unsigned k = 0; k < m_ranks.size(); k++)
    m_ranks[k]->SetTemperament(temperament);
}

void GOOrganController::SetTemperament(const wxString &name) {
  const GOTemperament &temperament
    = m_config.GetTemperaments().GetTemperament(name);
  m_Temperament = temperament.GetName();
  SetTemperament(temperament);
}

void GOOrganController::AllNotesOff() {
  for (unsigned k = GetFirstManualIndex(); k <= GetManualAndPedalCount(); k++)
    GetManual(k)->AllNotesOff();
}
