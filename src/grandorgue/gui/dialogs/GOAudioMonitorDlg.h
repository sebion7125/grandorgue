/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOAUDIOMONITORDLG_H
#define GOAUDIOMONITORDLG_H

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <wx/frame.h>
#include <wx/spinctrl.h>
#include <wx/timer.h>

#include "model/GOOrganLifecycleListener.h"

class GOAudioMonitorGraph;
class GOSoundSampler;
class GOSoundingPipe;
class GODarkCheckBox;
class GOManual;
class GOOrganModel;
class GOSoundSystem;
class GODrawstop;
class wxButton;
class wxCheckBox;
class wxScrolledWindow;
class wxSplitterWindow;
class wxChoice;
class wxSlider;
class wxSpinCtrl;
class wxStaticText;

// Non-modal "live" diagnostics window: polls the realtime-safe diagnostic
// counters already exposed by GOSoundSystem, GOSoundOrganEngine and
// GOAudioParams::FastCrossfadeCache on a GUI-thread timer (no cross-thread
// marshalling needed - the getters are themselves thread-safe atomics) and
// shows them as a small rolling graph (polyphony usage, render time,
// callback gap - each toggleable) plus text counters for the rest.
//
// A plain wxFrame (not wxDialog), same as a detached organ panel
// (GOGUIPanelView::createWithFrame): an independent top-level window that
// never blocks interaction with the main window or other panels, and
// forwards key events (Escape/Panic, Ctrl+S etc.) to the main window's
// GOAppWindow::OnKeyCommand while it has focus, exactly like a panel.
//
// The "Create Report" button assembles everything into an AudioGlitchReport
// ZIP: summary.txt (version/config/counters), graph.png (rendered snapshot),
// samples.csv (raw time series), sysinfo.txt (OS/CPU/RAM) - intended to give
// developers everything they need to reproduce a glitch from a user report.
class GOAudioMonitorDlg : public wxFrame, private GOOrganLifecycleListener {
public:
  // getOrganModel is called on every poll tick to fetch the *current* organ
  // model (or nullptr if none is loaded) - never cached beyond one tick,
  // since the caller may load/unload/replace the organ while this window
  // stays open.
  // setBufferSize / setV3AndReload: host (GOAppWindow) callbacks for the
  // automated "Find ceiling" sweep - change the audio buffer size (sound
  // restart, organ stays loaded) resp. the sample codec (config + organ
  // reload). May be empty; the sweep UI is then disabled.
  explicit GOAudioMonitorDlg(
    wxWindow *parent,
    GOSoundSystem &soundSystem,
    std::function<GOOrganModel *()> getOrganModel,
    std::function<bool(unsigned)> setBufferSize = nullptr,
    std::function<void(bool)> setV3AndReload = nullptr);
  ~GOAudioMonitorDlg();

  /**
   * Retract any benchmark-forced registration ("Enable All Stops &
   * Couplers") of the currently open monitor window, if one exists.
   *
   * MUST be called before the organ's .cmb is saved (GOGuiOrgan::Save /
   * Export): GOAppWindow::CloseOrgan saves FIRST and tears the organ down
   * afterwards, so the AbortPlayback-based reset comes too late and the
   * forced states would be persisted. That is exactly what poisoned a
   * Rotterdam config with all 123 switches engaged - on a set that wires
   * its tremulants through a switch graph, the redundant OR paths then
   * keep them audibly on no matter what the visible button does.
   */
  static void ResetAnyForcedRegistration();

private:
  // GOOrganLifecycleListener: registered on the current p_OrganModel
  // whenever it's set to a non-null value (see RefreshOrganAvailability()).
  // AbortPlayback() is broadcast synchronously by GOOrganController::Abort()
  // - called from GOSoundSystem::NotifySoundIsClosing(), itself called from
  // AssignOrganFile() BEFORE GOGuiOrgan::CloseOrgan() deletes the
  // controller - so it fires while p_OrganModel is still safe to use. This
  // is what makes it safe to load a new organ or exit GO while the
  // benchmark is running: without this hook, p_OrganModel would only be
  // refreshed by the next slow (333ms) UpdateNow() poll, and could dangle
  // for several ticks (or indefinitely, since GOAppWindow::p_OrganController
  // itself does not get reassigned until the *new* organ has fully loaded,
  // which can take seconds) in the meantime.
  void PreparePlayback() override {}
  void AbortPlayback() override;


  // Identifies one (manual, MIDI note) key, for the polyphony benchmark.
  struct NoteKey {
    unsigned manualIndex;
    unsigned midiNote;

    bool operator==(const NoteKey &o) const {
      return manualIndex == o.manualIndex && midiNote == o.midiNote;
    }
  };

  // A scheduled future action on a benchmark-triggered note.
  struct PendingNoteAction {
    NoteKey note;
    wxLongLong dueTimeMs;
    // RELEASE: attack's hold time elapsed - release it.
    // REATTACK: the note was briefly released (fresh transient attack, or a
    // held note interrupted by a collision) - press it again.
    enum { RELEASE, REATTACK } action;
    // Only meaningful for RELEASE: this note was on loan from m_HeldNotes
    // (a collision interrupted it) - once this release fires, refill one
    // held slot so the held count returns to the slider's target.
    bool refillHeldOnRelease = false;
  };

  GOSoundSystem &r_SoundSystem;
  std::function<GOOrganModel *()> m_GetOrganModel;
  // Cache of m_GetOrganModel(), refreshed every UpdateNow() tick. Never used
  // directly by outside code - always go through the refreshed cache so a
  // mid-session organ load/unload/reload is picked up automatically.
  GOOrganModel *p_OrganModel;
  GOAudioMonitorGraph *m_Graph;
  GODarkCheckBox *m_ShowPolyphony;
  GODarkCheckBox *m_ShowRenderTime;
  GODarkCheckBox *m_ShowCallbackTime;
  GODarkCheckBox *m_ShowCallbackGap;
  GODarkCheckBox *m_ShowCbShare;
  // Der Zaehlertext lebt in einem eigenen Rollfenster: als nackter
  // wxStaticText wuchs seine BESTE Groesse mit jeder neuen Zeile, und weil
  // der Graph das einzige dehnbare Element ist, kam jede Zeile aus dessen
  // Hoehe (Nutzerbefund 12.08. - Graph war weg).  Gedeckelt kann er das
  // nicht mehr; ueberzaehlige Zeilen bekommen einen Rollbalken.
  // -1 = noch nie gesetzt.  Verhindert, dass der 333-ms-Takt GTKs
  // Tooltip-Zustand staendig zurueckstellt - siehe RefreshOrganAvailability.
  int m_BenchTipState = -1;
  // Ziehbarer Trenner Graph / Zaehlertext.  Sash-Gravitation 0: waechst
  // das Fenster, geht der Zuwachs vollstaendig an den Textbereich.
  wxSplitterWindow *m_Split;
  wxScrolledWindow *m_CounterScroll;
  /* Selbst gefuehrte Textgroesse.  wxST_NO_AUTORESIZE (noetig, damit der
   * 333-ms-Takt kein Relayout ausloest) haelt das Label davon ab, seine
   * Groesse nachzufuehren - ohne diese Buchhaltung behielt es die Hoehe
   * des ERSTEN Textes, und spaeter dazukommende Zeilen (Hybrid!) waren
   * abgeschnitten und kamen nie wieder (Nutzerbefund 12.08.). */
  wxSize m_CounterTextSize = wxSize(-1, -1);
  // Ueberschuessige Hoehe des Textbereichs an den Graphen zurueckgeben.
  void FitCounterPane();
  wxStaticText *m_CounterLabel;
  /* The colour legend above the graph. Its text is long enough that, left
   * as a single line, its natural width alone decides how wide the dialog
   * has to be. Kept here with its unwrapped text so OnDialogResize() can
   * re-flow it - wxStaticText::Wrap() edits the label in place, so the
   * original has to be restored before each re-wrap. */
  wxStaticText *m_Legend = nullptr;
  wxString m_LegendText;
  int m_LegendWrapWidth = -1;
  /* The counter label reserves a height estimate at construction so the
   * initial fit is not computed against an empty label; the reservation is
   * dropped again once real content arrives. See both places. */
  bool m_CounterHeightReleased = false;
  void OnDialogResize(wxSizeEvent &event);
  wxButton *m_CreateReportBtn;
  wxSpinCtrl *m_PrefetchSpin;
  wxSpinCtrl *m_StaggerSpin;
  wxSpinCtrl *m_XrunTestSpin;
  wxChoice *m_XrunPatternChoice;
  wxSpinCtrl *m_FreqInertiaSpin;
  wxControl *m_StragglerTruthCheck;
  wxControl *m_PronyOrderCheck;
  wxControl *m_SeamChoice;
  wxControl *m_NoTrendCheck;
  wxChoice *m_ConcealModeChoice;
  wxSpinCtrl *m_MixGraceSpin;

  // The single open monitor window (nullptr when closed) - GUI thread only.
  // Enables ResetAnyForcedRegistration() from the save path without the
  // saver having to hold a pointer to this window.
  static GOAudioMonitorDlg *s_Instance;
  wxCheckBox *m_PinWorkersCheck;
  wxCheckBox *m_ReadAheadCheck;
  wxButton *m_LatencyProbeBtn;
  wxStaticText *m_LatencyLabel;
  bool m_PrioMismatchLogged = false;
  wxTimer m_Timer;

  // --- Polyphony benchmark ---
  wxButton *m_AllStopsBtn;
  GODarkCheckBox *m_BenchmarkActive;
  wxSlider *m_RateSlider;
  wxStaticText *m_RateLabel;
  wxSlider *m_HeldSlider;
  wxStaticText *m_HeldLabel;
  // --- Held pipes: a pure sustain-polyphony dial ---
  // Starts random pipe voices that never get a note-off (until the slider
  // comes down or the benchmark stops). Unlike held KEYS, this scales
  // beyond the organ's key*stop count: the same pipe may sound multiple
  // times (voices are fully independent since the per-voice render
  // redesign), giving an objective, stepless capacity probe.
  wxSlider *m_HeldPipesSlider;
  wxSpinCtrl *m_HeldPipesSpin;
  wxStaticText *m_HeldPipesLabel;
  // All sounding pipes of the current organ (rebuilt per organ).
  std::vector<GOSoundingPipe *> m_PipePool;
  // Currently held benchmark voices (pipe, handle) in start order.
  std::vector<std::pair<GOSoundingPipe *, GOSoundSampler *>> m_HeldPipes;
  wxTimer m_BenchmarkTimer;
  bool m_AllStopsLatched;
  std::vector<GODrawstop *> m_ForciblyEngaged;
  // Notes currently held indefinitely (simulated sustained chord). Adjusted
  // to match m_HeldSlider's value whenever the benchmark is active.
  std::vector<NoteKey> m_HeldNotes;
  // Transient attacks awaiting their scheduled release, plus held notes
  // awaiting their post-collision re-attack.
  std::vector<PendingNoteAction> m_PendingActions;
  wxLongLong m_NextTriggerDueMs;
  // Total accessible keys of the organ as of the last
  // RefreshOrganAvailability() call - used to detect a
  // changed/unloaded/reloaded organ and to size m_HeldSlider's range.
  unsigned m_LastKnownTotalKeys;

  int64_t m_WorstRenderUs;
  int64_t m_WorstCallbackUs;
  int64_t m_WorstGapUs;
  // Cumulative drop total from the previous UpdateNow() call, used to
  // compute the per-interval delta passed to PushSample().
  uint64_t m_LastTotalDrops;
  // Previous UpdateNow() snapshot of the cumulative V3 decode counters
  // (GOSoundGroupTask::GetDecodeStats) plus its timestamp, for the
  // per-second rates shown in the counter label (and thus the report).
  uint64_t m_LastV3Rebuilds;
  uint64_t m_LastV3BatchCalls;
  uint64_t m_LastV3Solo;
  uint64_t m_LastV3BatchRendered;
  uint64_t m_LastV3SyncFallbacks;
  uint64_t m_LastV3CbVoices = 0;
  uint64_t m_LastV3RebuildsGraph = 0;
  uint64_t m_LastV3DrainUs = 0;
  uint64_t m_LastWakeUs = 0;
  uint64_t m_LastDispatchUs = 0;
  uint64_t m_LastTailUs = 0;
  uint64_t m_LastBurstWallUs = 0;
  uint64_t m_LastBurstCpuUs = 0;
  uint64_t m_LastBurstNivcsw = 0;
  uint64_t m_LastBurstCount = 0;
  uint64_t m_LastStDec = 0, m_LastStFir = 0, m_LastStFad = 0,
           m_LastStFus = 0, m_LastStAdm = 0;
  uint64_t m_LastV3CbRuns = 0;
  uint64_t m_LastV3CbDenied = 0;
  uint64_t m_LastV3ExecLate = 0;
  uint64_t m_LastV3ListSamples;
  uint64_t m_LastV3ListVoices;
  wxLongLong m_LastV3SampleMs;

  void UpdateNow();
  void OnTimer(wxTimerEvent &event);
  void OnToggleSeries(wxCommandEvent &event);
  void OnCreateReport(wxCommandEvent &event);
  // Shows the loaded sample set's V3 bin-ladder histogram (which ladder
  // rungs its sections fall into - see GOSoundSubblockBins::RecordSection).
  void OnCacheBins(wxCommandEvent &event);
  void OnCbHelpChanged(wxCommandEvent &event);
  void OnAffinityToggle(wxCommandEvent &event);
  void OnPrefetchChanged(wxSpinEvent &event);
  void OnStaggerChanged(wxSpinEvent &event);
  void OnXrunTestChanged(wxSpinEvent &event);
  void OnXrunPatternChanged(wxCommandEvent &event);
  void OnFreqInertiaChanged(wxSpinEvent &event);
  void OnStragglerTruthToggled(wxCommandEvent &event);
  void OnPronyOrderChanged(wxSpinEvent &event);
  void OnSeamChanged(wxCommandEvent &event);
  void OnNoTrendToggled(wxCommandEvent &event);
  void OnHybridToggled(wxCommandEvent &event);
  void OnHybHandChanged(wxSpinEvent &event);
  void OnHybCapChanged(wxSpinEvent &event);
  void OnHybAllBandsToggled(wxCommandEvent &event);
  void OnSafetyCapChanged(wxSpinDoubleEvent &event);
  void OnAnchorTauChanged(wxSpinEvent &event);
  void OnRootClampChanged(wxSpinDoubleEvent &event);
  void OnDetectRhoChanged(wxSpinDoubleEvent &event);
  void OnResGateChanged(wxSpinDoubleEvent &event);
  void OnV0HindcastToggle(wxCommandEvent &event);
  void OnReentryFadeChanged(wxSpinEvent &event);
  void OnConcealModeChanged(wxCommandEvent &event);
  void OnMixGraceChanged(wxSpinEvent &event);
  void OnPinWorkersChanged(wxCommandEvent &event);
  void OnDecodeReadAheadChanged(wxCommandEvent &event);
  void OnLatencyProbe(wxCommandEvent &event);
  void OnCloseButton(wxCommandEvent &event);
  void OnCloseWindow(wxCloseEvent &event);
  // Forwards key events (Escape/Panic, Ctrl+S etc.) to the main window while
  // this window has focus, same as a detached organ panel.
  void OnCharHook(wxKeyEvent &event);

  // --- Polyphony benchmark ---
  void OnAllStops(wxCommandEvent &event);
  void ResetForciblyEngagedStops();
  void OnBenchmarkToggle(wxCommandEvent &event);
  void OnRateSliderChanged(wxCommandEvent &event);
  void OnHeldSliderChanged(wxCommandEvent &event);
  void OnHeldPipesSliderChanged(wxCommandEvent &event);
  void OnHeldPipesSpin(wxSpinEvent &event);
  // Keeps slider, spin field and label showing the same target.
  void SyncHeldPipesTarget(int target, bool fromSpin);
  // Ramps m_HeldPipes toward the slider target, bounded per benchmark tick
  // (both directions - stopping tens of thousands at once would BE the
  // release-all stress test, not a controlled ramp).
  void AdjustHeldPipes();
  // Drops all held-pipe voices. stopVoices=true sends proper note-offs
  // (slider down / benchmark stop); false just forgets the handles
  // (playback aborting / organ changing - the voices die with it, and the
  // old pipe pointers must not be touched).
  void ClearHeldPipes(bool stopVoices);
  void OnBenchmarkTimer(wxTimerEvent &event);
  // Re-fetches the current organ model via m_GetOrganModel() and, if it
  // differs from the cached p_OrganModel (organ loaded/unloaded/replaced),
  // stops the benchmark (references to the old organ's notes would no
  // longer be valid) and refreshes the benchmark controls' enabled state,
  // slider range and labels.
  void RefreshOrganAvailability();
  // Picks a uniformly random accessible (manual, note) pair of the loaded
  // organ. Returns false if no organ is loaded or no manual has any
  // accessible key.
  bool PickRandomKey(NoteKey &out);
  bool IsHeld(const NoteKey &note) const;
  bool IsPendingRelease(const NoteKey &note) const;
  // Picks one new random note not already held/pending, presses it and adds
  // it to m_HeldNotes. Returns false if none could be found (organ too
  // small / fully occupied).
  bool AddOneHeldNote();
  // Presses/releases notes so m_HeldNotes.size() matches m_HeldSlider.
  void AdjustHeldNotes();
  // Releases every held and pending-transient note and clears the
  // benchmark's bookkeeping. Safe to call repeatedly / when inactive.
  void StopBenchmark();
  unsigned GetTotalAccessibleKeys() const;

  // --- "Find ceiling" sweep: automated, objective capacity benchmark ---
  // Replaces the error-prone manual slider probing: a closed-loop controller
  // on the held-pipes ramp finds, PER (codec, buffer size) cell, the largest
  // polyphony that stays completely clean over an observation window (zero
  // gap / render-overrun / driver-underrun / drop deltas AND peak period
  // compute time within the budget), by exponential ramp-up to the first
  // failure followed by bisection. Between cells it switches the buffer size
  // (sound restart) and the codec (config + organ reload) via the host
  // callbacks, so the user can start it and walk away; the result table ends
  // up in the counter label and thus in every audio report.
  std::function<bool(unsigned)> m_SetBufferSize;
  std::function<void(bool)> m_SetV3AndReload;
  GODarkCheckBox *m_CeilCheck = nullptr;
  wxStaticText *m_CeilStatus = nullptr;
  enum class CeilPhase : uint8_t {
    Idle,      // not running
    ApplyCell, // switch codec/buffer for the next cell (may reload)
    WaitReady, // wait for organ + sound to be up after a switch
    Settle,    // wait for the held-pipes ramp to reach the target
    Evaluate,  // observation window at constant target
    Restore,   // sweep over: put the user's codec/buffer back
    Done
  };
  struct CeilCell {
    bool v3;
    unsigned buffer;
  };
  std::vector<CeilCell> m_CeilCells;
  size_t m_CeilCellIdx = 0;
  CeilPhase m_CeilPhase = CeilPhase::Idle;
  bool m_CeilRampMode = true; // exponential ramp (before the first failure)
  unsigned m_CeilLo = 0;      // largest target proven clean
  // Whether m_CeilLo passed a FULL observation window (fast-forwarded
  // probes are provisional; a cell result must never rest on one - the
  // convergence check re-verifies an unproven lo with a strict window).
  bool m_CeilLoStrict = true;
  // One free retry per target: a single Windows hiccup (background
  // service causing one underrun) must not permanently cap the cell -
  // true overload fails twice within seconds (early-fail on the first
  // event), so the retry costs almost nothing where it matters.
  bool m_CeilRetriedTarget = false;
  // Restore performed at least one device restart/reload - guarantees the
  // benchmark voices were killed even when nothing needed restoring.
  bool m_CeilRestoreKicked = false;
  unsigned m_CeilHi = 0;      // smallest target proven dirty (0 = none yet)
  unsigned m_CeilTarget = 0;
  int m_CeilPollsLeft = 0;         // countdown within the current phase
  int64_t m_CeilWinWorstCompute = 0;
  // Polls of the current window where the callback-time peak fell BELOW
  // the compute-time peak - proof that the callback still returned
  // before the period's compute would have finished, i.e. headroom
  // existed in that poll. The edge law (user, from the monitor graphs):
  // a target is saturated only when the callback curve no longer falls
  // below the compute curve AT ALL within the whole window - counting
  // "near the crossing" polls instead underestimated the ceiling by
  // thousands of voices (measured 2026-07-11).
  int m_CeilBelowPolls = 0;
  wxString m_CeilResults;          // finished cells, one line each
  bool m_CeilOrigV3 = false;       // user's settings, restored afterwards
  bool m_CeilOrigCache = false;
  unsigned m_CeilOrigBuffer = 0;
  unsigned m_CeilSavedPolyphonyLimit = 0;
  bool m_CeilCodecSwitched = false; // current config differs from orig
  // Reentrancy guard: the organ reload triggered from CeilingTick() shows a
  // progress dialog that yields events - the monitor timer can fire INTO
  // the still-running tick. Without this, ApplyCell would start the reload
  // twice and Restore could "finish" mid-reload.
  bool m_CeilInTick = false;

  void OnCeilingToggle(wxCommandEvent &event);
  void CeilingStart();
  // Aborts / finishes the sweep. If settings were changed, enters the
  // Restore phase first (async); Idle only once everything is back.
  void CeilingRequestStop();
  void CeilingFinish(); // Restore done - release everything, phase Done/Idle
  // One controller step, called from every UpdateNow() poll (333ms) while
  // the sweep runs. computeUs = this poll's period compute peak.
  void CeilingTick(
    int64_t computeUs, int64_t callbackUs, unsigned usedPolyphony);
  void CeilingSetTarget(unsigned target);
  void CeilingCellDone(unsigned result);
  void CeilingStatus(const wxString &text);

  wxDECLARE_EVENT_TABLE();
};

#endif /* GOAUDIOMONITORDLG_H */
