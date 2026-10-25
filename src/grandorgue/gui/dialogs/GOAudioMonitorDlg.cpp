/*
 * Copyright 2006 Milan Digital Audio LLC

Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOAudioMonitorDlg.h"

#include <algorithm>
#include <cstdlib>
#ifdef __linux__
#include <fstream>
#include <string>
#endif

#include <wx/stdpaths.h>
#include <wx/app.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h> // __rdtsc for the manual working-set latency probe
#endif
#ifdef _WIN32
#include <windows.h> // SetThreadPriority: RT-boost the latency probe
#endif
#ifdef __linux__
#include <pthread.h> // SCHED_FIFO boost for the latency probe under load
#include <sched.h>
#endif
#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/datetime.h>
#include <wx/filedlg.h>
#include <wx/config.h>
#include <wx/filename.h>
#include <wx/display.h>
#include <wx/spinctrl.h>
#include <wx/image.h>
#include <wx/intl.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/mstream.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/scrolwin.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/thread.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#ifdef _WIN32
// K32GetProcessMemoryInfo lives in kernel32 with PSAPI_VERSION 2 - no
// extra psapi link needed (report's process-memory probe).
#define PSAPI_VERSION 2
#include <psapi.h>
#endif

#include "go_defs.h"

#include "config/GOAudioDeviceConfig.h"
#include "config/GOConfig.h"
#include "GOOrganController.h"
#include "model/GOCoupler.h"
#include "model/GOManual.h"
#include "model/GOOrganModel.h"
#include "model/GORank.h"
#include "model/GOSoundingPipe.h"
#include "model/GOStop.h"
#include "model/GOSwitch.h"
#include "sound/GOCrossfadeMode.h"
#include "sound/GOCrossfadeParam.h"
#include "sound/GOFlushToZero.h"
#include "sound/GOSoundConcealment.h"

#include "xorcist.h"

#ifdef __WXMSW__
#include <windows.h>
#endif
#include "sound/GOSoundOrganEngine.h"
#include "sound/GOSoundSystem.h"
#include "sound/fast_crossfade.h"
#include "sound/playing/GOSoundSubblockGrid.h"
#include "sound/playing/GOSoundStream.h"
#include "sound/scheduler/GOSoundThread.h"
#include "sound/tasks/GOSoundOutputTask.h"
#include "sound/tasks/GOSoundGroupTask.h"

#include "gui/wxcontrols/GOAudioMonitorGraph.h"
#include "gui/wxcontrols/go_gui_utils.h"

namespace {
enum {
  ID_AUDIO_MONITOR_TIMER = wxID_HIGHEST + 1,
  ID_SHOW_POLYPHONY,
  ID_SHOW_RENDER_TIME,
  ID_SHOW_CALLBACK_TIME,
  ID_SHOW_CALLBACK_GAP,
  ID_SHOW_CB_SHARE,
  ID_CREATE_REPORT,
  ID_CACHE_BINS_BTN,
  ID_PREFETCH_SPIN,
  ID_STAGGER_SPIN,
  ID_XRUN_TEST_SPIN,
  ID_XRUN_PATTERN_CHOICE,
  ID_FREQ_INERTIA_SPIN,
  ID_STRAGGLER_TRUTH_CHECK,
  ID_PRONY_ORDER_CHECK,
  ID_SEAM_CHOICE,
  ID_NOTREND_CHECK,
  ID_HYBRID_CHECK,
  ID_HYB_HAND_SPIN,
  ID_HYB_CAP_SPIN,
  ID_HYB_ALLB_CHECK,
  ID_SAFETY_CAP_SPIN,
  ID_ANCHOR_TAU_SPIN,
  ID_ROOT_CLAMP_SPIN,
  ID_DETECT_RHO_SPIN,
  ID_RES_GATE_SPIN,
  ID_V0_HINDCAST_CHECK,
  ID_REENTRY_FADE_SPIN,
  ID_CONCEAL_MODE_CHOICE,
  ID_MIX_GRACE_SPIN,
  ID_PIN_WORKERS,
  ID_DECODE_READAHEAD,
  ID_LATENCY_PROBE,
  ID_ALL_STOPS_BTN,
  ID_BENCHMARK_ACTIVE,
  ID_RATE_SLIDER,
  ID_HELD_SLIDER,
  ID_HELD_PIPES_SLIDER,
  ID_HELD_PIPES_SPIN,
  ID_CB_HELP_CHOICE,
  ID_BENCHMARK_TIMER,
  ID_CEILING_CHECK,
  ID_AFFINITY_CHECK,
};
constexpr int POLL_INTERVAL_MS = 333;
// Benchmark tick. Must be at most the inter-trigger interval at the highest
// rate (RATE_SLIDER_MAX_KPM=6000/min -> 10ms), because the catch-up loop in
// OnBenchmarkTimer() fires every due trigger of a tick in one synchronous
// burst: with a coarser tick the notes clump into simultaneous chords per
// tick (e.g. 50ms tick at 6000/min = 5-note salvos at 20Hz) instead of an
// evenly spaced stream.
constexpr int BENCHMARK_TICK_MS = 10;
// Transient (non-held) notes stay down for a random duration in this range.
constexpr int TRANSIENT_HOLD_MIN_MS = 200;
constexpr int TRANSIENT_HOLD_MAX_MS = 900;
// A held note hit by the random trigger is briefly released, then re-pressed
// after this delay, to let a fresh attack transient sound.
constexpr int COLLISION_REATTACK_DELAY_MS = 60;
constexpr int RATE_SLIDER_MAX_KPM = 6000;

// At low rates, use the full 200-900ms range for a natural, varied feel. At
// high rates, shrink toward the inter-trigger interval so notes can actually
// complete (attack + hold + release) before the same key is likely to be
// picked again, instead of every pick colliding with one still sounding
// from the previous tick - that's what caused the low throughput at high
// rates before this scaling was added.
int RandomTransientHoldMs(int kpm) {
  int maxMs = TRANSIENT_HOLD_MAX_MS;

  if (kpm > 0) {
    const int intervalMs = 60000 / kpm;

    maxMs = std::min(TRANSIENT_HOLD_MAX_MS, std::max(40, intervalMs * 4));
  }

  const int minMs = std::min(TRANSIENT_HOLD_MIN_MS, maxMs / 2);

  return minMs + std::rand() % (maxMs - minMs + 1);
}

/* Fixed seed for the held-pipes RNG (AdjustHeldPipes picks both the pipe and
 * its start offset with std::rand). Re-seeded whenever a mix is built up from
 * zero, so the SAME slider value always holds the SAME set of pipes.
 *
 * Without this every ramp-up holds a different random registration: different
 * sample lengths, channel counts and above all different residual byte widths
 * (1-4) - and the byte width drives DRAM traffic, which is what the polyphony
 * ceiling is bound by. A "lucky" mix is genuinely faster than an unlucky one
 * at identical code, so two runs compared two organs' worth of work rather
 * than two builds. Found 2026-07-15 by the user, after it had caused run-to-
 * run swings of several percent (and an afternoon of hunting a phantom
 * regression). The ceiling sweep already seeded for exactly this reason - the
 * manual slider, which is what one actually measures with, did not. */
static constexpr unsigned HELD_PIPES_RNG_SEED = 20260711;

wxString CrossfadeModeName(GOCrossfadeMode m) {
  switch (m) {
  case GOCrossfadeMode::Linear:
    return wxT("Linear");
  case GOCrossfadeMode::SinEqualPower:
    return wxT("Sin (equal power)");
  case GOCrossfadeMode::Sin2:
    return wxT("Sin²");
  case GOCrossfadeMode::SqrtEqualPower:
    return wxT("Sqrt (equal power)");
  case GOCrossfadeMode::X2:
    return wxT("X²");
  case GOCrossfadeMode::Custom:
    return wxT("Custom");
  }
  return wxT("?");
}

// wxButton/wxCheckBox/wxSpinCtrl don't inherit a parent's explicitly-set
// colours the way wxStaticText does (see the constructor comment) - each
// needs it set directly. On GTK this reliably darkens the whole control
// (checkbox box, button face, spin entry+arrows included); native themed
// platforms may still draw some chrome (e.g. focus rings) in the system
// colour regardless, but the label text and background follow everywhere.
void ApplyDarkChrome(wxWindow *w) {
  w->SetBackgroundColour(wxColour(32, 32, 36));
  w->SetForegroundColour(wxColour(220, 220, 225));
}

/* ---- Statusblock ---------------------------------------------------------
 * Derselbe Text steht im Dialog UND in jedem Audio-Report (summary.txt), er
 * wird im 333-ms-Takt gelesen und ist auf ~30 Zeilen gewachsen.  Drei
 * Regeln halten ihn lesbar (Nutzervorgabe 12.08.2026):
 *
 *  1. Dicktengleiche Schrift, feste Etikettenspalte - nur so rasten Zahlen
 *     in Spalten, und nur so bleibt summary.txt in einem Editor lesbar.
 *  2. Kein Fliesstext.  Erklaerungen stehen im Tooltip des zugehoerigen
 *     Reglers, nicht in einer Anzeige, die man ueberfliegt.
 *  3. Nullgruppen klappen zu einer Zeile zusammen ("device: keine
 *     Ereignisse") - was staendig Nullen zeigt, kostet nur Aufmerksamkeit.
 *
 * Die Histogramme liegen bewusst auf DERSELBEN Topfleiter: dadurch stehen
 * Epilogabstand und Rundenlaufzeit in denselben Spalten uebereinander, und
 * das Kollisionskriterium (Runde > Abstand) ist direkt ablesbar.
 */
// Zeilenbreite der Trennlinien - passend zur breitesten Datenzeile einer
// SPALTE, nicht zur Fensterbreite (zweispaltiger Aufbau, siehe
// StatColumns).
constexpr int STAT_COLS = 62;
constexpr int STAT_LABEL = 12; // Breite der Etikettenspalte

wxString StatRule(
  const wxString &title, const wxString &tail = wxEmptyString) {
  const wxString head = wxT("─── ") + title + wxT(" ");
  const wxString foot
    = tail.IsEmpty() ? wxString() : (wxT(" ") + tail + wxT(" ───"));
  const int fill = STAT_COLS - (int)(head.length() + foot.length());

  return head + wxString(wxUniChar(0x2500), fill > 2 ? fill : 2) + foot
    + wxT("\n");
}

// Feldbreiten als Literal, nicht ueber "%-*s": wxString::Format ersetzt
// %s-Argumente selbst, und die Stern-Breite ist dabei nicht in jedem
// wx-Build zuverlaessig.  Muss zu STAT_LABEL passen.
static_assert(STAT_LABEL == 12, "Feldbreite unten mit anpassen");

wxString StatRow(const wxString &label, const wxString &value) {
  return wxString::Format(wxT("  %-12s %s\n"), label, value);
}

wxString StatRow(
  const wxString &label, const wxString &left, const wxString &right) {
  return wxString::Format(wxT("  %-12s %-22s %s\n"), label, left, right);
}

/* Zwei Spalten nebeneinander verweben - halbiert die Zeilenzahl, damit im
 * Textfeld weniger gescrollt werden muss (Nutzerwunsch 12.08.).
 *
 * Die Spaltenbreite wird GEMESSEN, nicht gesetzt: eine feste Breite haette
 * bei jeder zu langen Zeile die rechte Spalte verschoben (erster Versuch:
 * die 78 Zeichen breiten Trennlinien sprengten eine 64er-Spalte, Ergebnis
 * 156 Zeichen Zeilenbreite).  So bleibt das Raster garantiert. */
wxString StatColumns(const wxString &left, const wxString &right) {
  const wxArrayString l = wxSplit(left, '\n');
  const wxArrayString r = wxSplit(right, '\n');
  const size_t n = std::max(l.GetCount(), r.GetCount());
  size_t w = 0;
  wxString out;

  for (size_t i = 0; i < l.GetCount(); i++)
    w = std::max(w, l[i].length());
  w += 2; // Luft zwischen den Spalten
  for (size_t i = 0; i < n; i++) {
    wxString a = i < l.GetCount() ? l[i] : wxString();
    const wxString b = i < r.GetCount() ? r[i] : wxString();

    if (b.IsEmpty())
      out << a << wxT("\n");
    else {
      if (a.length() < w)
        a += wxString(wxT(' '), w - a.length());
      out << a << b << wxT("\n");
    }
  }
  return out;
}

// Eine Histogrammzeile: Etikett, dann n rechtsbuendige Spalten a 7 Zeichen.
wxString StatHist(const wxString &label, const uint64_t *v, unsigned n) {
  wxString s = wxString::Format(wxT("    %-16s"), label);

  for (unsigned i = 0; i < n; i++)
    s += wxString::Format(wxT("%7llu"), (unsigned long long)v[i]);
  return s + wxT("\n");
}

/* Compact, but never narrower than the control can actually be drawn.
 *
 * The spin controls used to be constructed with a hard wxSize(60, -1) and
 * friends. A GTK3 spin button needs room for the entry plus BOTH arrow
 * buttons, which is more than that on most themes, so the sizer handed the
 * widget less than its minimum and GTK complained on every single layout
 * pass: "gtk_box_gadget_distribute: assertion 'size >= 0' failed in
 * GtkSpinButton". With this dialog refreshing a few times per second that
 * is a steady stream of stderr writes from the GUI thread while the organ
 * plays - noise in the log and pointless I/O in the worst possible place.
 *
 * So: ask for the width we want, but let the control's own best size win
 * whenever it is larger. Windows and macOS keep their previous look
 * because their spin buttons fit into the requested width anyway. */
/* Reserve room for the widest text a label will ever hold, measured in the
 * label's own font rather than guessed in pixels.
 *
 * The hardcoded widths this replaces (50/70/90) were measured against
 * Windows' default font. GTK's is larger, so the value/target readouts next
 * to the sliders were cut off - the classic symptom of a layout that
 * assumes a font size instead of asking for one. */
void SetMinWidthForText(wxWindow *w, const wxString &widestText) {
  w->SetMinSize(wxSize(w->GetTextExtent(widestText).x + 6, wxDefaultCoord));
}

void SetCompactWidth(wxWindow *w, int desiredWidth) {
  /* Width only - the height stays wxDefaultCoord on purpose. GetBestSize()
   * on a freshly created, not yet realized GTK widget can report a height
   * below what the theme actually needs (measured: 12 where the spin
   * button's own minimum is 16), and pinning that produced a second, more
   * obscure complaint: "for_size smaller than min-size (12 < 16) while
   * measuring gadget". Leaving the height unspecified lets the control
   * compute it when it knows better than we do. */
  const wxSize best = w->GetBestSize();

  w->SetMinSize(wxSize(std::max(desiredWidth, best.x), wxDefaultCoord));
}

// Read the first matching key from /proc/cpuinfo (Linux only).
wxString ReadProcCpuInfoField(const wxString &key) {
#ifdef __linux__
  wxFFileInputStream f(wxT("/proc/cpuinfo"));
  if (!f.IsOk())
    return wxEmptyString;
  wxString content;
  char buf[4096];
  while (!f.Eof()) {
    f.Read(buf, sizeof(buf) - 1);
    buf[f.LastRead()] = '\0';
    content += wxString::FromUTF8(buf);
    if (content.length() > 65536)
      break;
  }
  for (const auto &line : wxSplit(content, '\n')) {
    if (line.StartsWith(key + wxT(":"))) {
      wxString val = line.AfterFirst(':').Trim(false).Trim(true);
      if (!val.IsEmpty())
        return val;
    }
  }
#endif
  return wxEmptyString;
}

wxString ReadFirstLine(const wxString &path) {
#ifdef __linux__
  wxFFileInputStream f(path);
  if (!f.IsOk())
    return wxEmptyString;
  char buf[512];
  f.Read(buf, sizeof(buf) - 1);
  buf[f.LastRead()] = '\0';
  wxString s = wxString::FromUTF8(buf);
  return s.BeforeFirst('\n').Trim(true);
#endif
  return wxEmptyString;
}

wxString CountProcCpuInfoOccurrences(const wxString &key) {
#ifdef __linux__
  wxFFileInputStream f(wxT("/proc/cpuinfo"));
  if (!f.IsOk())
    return wxEmptyString;
  wxString content;
  char buf[65536];
  while (!f.Eof()) {
    f.Read(buf, sizeof(buf) - 1);
    buf[f.LastRead()] = '\0';
    content += wxString::FromUTF8(buf);
  }
  unsigned count = 0;
  wxString search = key + wxT(":");
  size_t pos = 0;
  while ((pos = content.find(search, pos)) != wxString::npos) {
    ++count;
    pos += search.length();
  }
  return wxString::Format(wxT("%u"), count);
#endif
  return wxEmptyString;
}

wxString BuildSysInfo() {
  wxString s;
  s += wxT("=== System Information ===\n");

  s += wxString::Format(wxT("OS: %s\n"), wxGetOsDescription());

#ifdef __linux__
  const wxString kernel = ReadFirstLine(wxT("/proc/version"));
  if (!kernel.IsEmpty())
    s += wxString::Format(wxT("Kernel: %s\n"), kernel);

  const wxString cpuModel = ReadProcCpuInfoField(wxT("model name"));
  const wxString coreCount = CountProcCpuInfoOccurrences(wxT("processor"));
  if (!cpuModel.IsEmpty())
    s += wxString::Format(
      wxT("CPU: %s (%s logical cores)\n"), cpuModel, coreCount);

  // RAM from /proc/meminfo (values are in kB).
  {
    wxFFileInputStream meminfo(wxT("/proc/meminfo"));
    if (meminfo.IsOk()) {
      char buf[4096];
      meminfo.Read(buf, sizeof(buf) - 1);
      buf[meminfo.LastRead()] = '\0';
      wxString mi = wxString::FromUTF8(buf);
      auto getKb = [&](const wxString &key) -> long {
        size_t pos = mi.find(key);
        if (pos == wxString::npos)
          return -1;
        wxString rest = mi.Mid(pos + key.length()).Trim(false);
        long v = 0;
        rest.ToLong(&v);
        return v;
      };
      const long total = getKb(wxT("MemTotal:"));
      const long avail = getKb(wxT("MemAvailable:"));
      if (total > 0) {
        s += wxString::Format(
          wxT("RAM: %.1f GB total"), (double)total / (1024.0 * 1024.0));
        if (avail >= 0)
          s += wxString::Format(
            wxT(", %.1f GB available"), (double)avail / (1024.0 * 1024.0));
        s += wxT("\n");
      }
    }
  }

#elif defined(__WXMSW__)
  // CPU model from registry.
  {
    HKEY hKey = nullptr;
    if (
      RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        0,
        KEY_READ,
        &hKey)
      == ERROR_SUCCESS) {
      wchar_t buf[256] = {};
      DWORD size = sizeof(buf);
      if (
        RegQueryValueExW(
          hKey,
          L"ProcessorNameString",
          nullptr,
          nullptr,
          reinterpret_cast<LPBYTE>(buf),
          &size)
        == ERROR_SUCCESS) {
        s += wxString::Format(
          wxT("CPU: %s"), wxString(buf).Trim(true).Trim(false));
      }
      RegCloseKey(hKey);
    }
  }
  {
    // Core count via GetSystemInfo (logical processors).
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    if (si.dwNumberOfProcessors > 0)
      s += wxString::Format(
        wxT(" (%lu logical cores)"), (unsigned long)si.dwNumberOfProcessors);
    s += wxT("\n");
  }
  // RAM via GlobalMemoryStatusEx.
  {
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
      s += wxString::Format(
        wxT("RAM: %.1f GB total, %.1f GB available\n"),
        (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0),
        (double)ms.ullAvailPhys / (1024.0 * 1024.0 * 1024.0));
    }
  }

#else
  // Fallback for other platforms.
  const int cpuCount = wxThread::GetCPUCount();
  if (cpuCount > 0)
    s += wxString::Format(wxT("CPU: %d logical cores\n"), cpuCount);
  const wxMemorySize freeMem = wxGetFreeMemory();
  if (freeMem > 0)
    s += wxString::Format(
      wxT("RAM available: %.1f GB\n"),
      freeMem.ToDouble() / (1024.0 * 1024.0 * 1024.0));
#endif

  return s;
}
} // namespace

// Self-drawn checkbox for this dialog's dark theme. wxCheckBox's native
// indicator glyph (box + tick) is drawn by the platform theme engine and
// ignores SetForegroundColour/SetBackgroundColour on GTK and most other
// platforms (see ApplyDarkChrome() above, which only reaches label text and
// surrounding chrome on buttons/spin controls - there's no equivalent for
// the checkbox glyph itself). Drawing our own sidesteps that entirely and
// stays 100% portable wx (no platform-specific styling calls, no build
// changes). Public interface (GetValue/SetValue, posts wxEVT_CHECKBOX)
// mirrors wxCheckBox closely enough that the rest of this dialog - event
// table EVT_CHECKBOX entries included - didn't need to change at all.
class GODarkCheckBox : public wxControl {
public:
  GODarkCheckBox(wxWindow *parent, wxWindowID id, const wxString &label)
    : wxControl(
      parent,
      id,
      wxDefaultPosition,
      wxDefaultSize,
      wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE),
      m_label(label) {
    SetBackgroundColour(parent->GetBackgroundColour());
    SetForegroundColour(parent->GetForegroundColour());
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCanFocus(true);
    SetInitialSize(ComputeBestSize());

    Bind(wxEVT_PAINT, &GODarkCheckBox::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &GODarkCheckBox::OnMouseDown, this);
    Bind(wxEVT_KEY_DOWN, &GODarkCheckBox::OnKeyDown, this);
    Bind(wxEVT_ENTER_WINDOW, &GODarkCheckBox::OnEnterLeave, this);
    Bind(wxEVT_LEAVE_WINDOW, &GODarkCheckBox::OnEnterLeave, this);
  }

  bool GetValue() const { return m_checked; }

  void SetValue(bool value) {
    if (m_checked != value) {
      m_checked = value;
      Refresh();
    }
  }

private:
  static constexpr int BOX_SIZE = 16;
  static constexpr int LABEL_GAP = 8;

  wxString m_label;
  bool m_checked = false;
  bool m_hovered = false;

  wxSize ComputeBestSize() const {
    wxClientDC dc(const_cast<GODarkCheckBox *>(this));

    dc.SetFont(GetFont());

    const wxSize textSize = dc.GetTextExtent(m_label);

    return wxSize(
      BOX_SIZE + LABEL_GAP + textSize.GetWidth() + 2,
      std::max(BOX_SIZE + 6, textSize.GetHeight() + 6));
  }

  void OnEnterLeave(wxMouseEvent &event) {
    m_hovered = event.GetEventType() == wxEVT_ENTER_WINDOW;
    Refresh();
    event.Skip();
  }

  void OnMouseDown(wxMouseEvent &event) {
    SetFocus();
    Toggle();
    event.Skip();
  }

  void OnKeyDown(wxKeyEvent &event) {
    const int key = event.GetKeyCode();

    if (key == WXK_SPACE || key == WXK_RETURN || key == WXK_NUMPAD_ENTER)
      Toggle();
    else
      event.Skip();
  }

  void Toggle() {
    SetValue(!m_checked);

    wxCommandEvent changeEvent(wxEVT_CHECKBOX, GetId());

    changeEvent.SetEventObject(this);
    changeEvent.SetInt(m_checked ? 1 : 0);
    ProcessWindowEvent(changeEvent);
  }

  void OnPaint(wxPaintEvent &WXUNUSED(event)) {
    wxAutoBufferedPaintDC dc(this);
    const wxSize size = GetSize();

    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();

    const int boxTop = (size.GetHeight() - BOX_SIZE) / 2;
    const wxRect boxRect(0, boxTop, BOX_SIZE, BOX_SIZE);

    dc.SetBrush(
      wxBrush(m_hovered ? wxColour(54, 54, 60) : wxColour(42, 42, 47)));
    dc.SetPen(wxPen(wxColour(95, 95, 105), 1));
    dc.DrawRoundedRectangle(boxRect, 3);

    if (m_checked) {
      // Soft, muted green - matches the graph's own restrained palette
      // rather than a saturated "success" green.
      dc.SetPen(wxPen(wxColour(140, 205, 150), 2));
      dc.DrawLine(
        boxRect.GetLeft() + 4,
        boxRect.GetTop() + 4,
        boxRect.GetRight() - 3,
        boxRect.GetBottom() - 3);
      dc.DrawLine(
        boxRect.GetRight() - 3,
        boxRect.GetTop() + 4,
        boxRect.GetLeft() + 4,
        boxRect.GetBottom() - 3);
    }

    dc.SetFont(GetFont());
    dc.SetTextForeground(GetForegroundColour());

    const wxSize textSize = dc.GetTextExtent(m_label);
    const int textX = BOX_SIZE + LABEL_GAP;
    const int textY = (size.GetHeight() - textSize.GetHeight()) / 2;

    dc.DrawText(m_label, textX, textY);
  }
};

wxBEGIN_EVENT_TABLE(GOAudioMonitorDlg, wxFrame)
  EVT_TIMER(ID_AUDIO_MONITOR_TIMER, GOAudioMonitorDlg::OnTimer)
    EVT_CHECKBOX(ID_SHOW_POLYPHONY, GOAudioMonitorDlg::OnToggleSeries)
      EVT_CHECKBOX(ID_SHOW_RENDER_TIME, GOAudioMonitorDlg::OnToggleSeries)
        EVT_CHECKBOX(ID_SHOW_CALLBACK_TIME, GOAudioMonitorDlg::OnToggleSeries)
          EVT_CHECKBOX(ID_SHOW_CALLBACK_GAP, GOAudioMonitorDlg::OnToggleSeries)
          EVT_CHECKBOX(ID_SHOW_CB_SHARE, GOAudioMonitorDlg::OnToggleSeries)
            EVT_BUTTON(ID_CREATE_REPORT, GOAudioMonitorDlg::OnCreateReport)
            EVT_BUTTON(ID_CACHE_BINS_BTN, GOAudioMonitorDlg::OnCacheBins)
            EVT_CHOICE(ID_XRUN_PATTERN_CHOICE, GOAudioMonitorDlg::OnXrunPatternChanged)
            EVT_SPINCTRL(
              ID_FREQ_INERTIA_SPIN, GOAudioMonitorDlg::OnFreqInertiaChanged)
              EVT_CHECKBOX(
                ID_STRAGGLER_TRUTH_CHECK,
                GOAudioMonitorDlg::OnStragglerTruthToggled)
              EVT_SPINCTRL(
                ID_PRONY_ORDER_CHECK,
                GOAudioMonitorDlg::OnPronyOrderChanged)
              EVT_CHOICE(ID_SEAM_CHOICE, GOAudioMonitorDlg::OnSeamChanged)
              EVT_CHECKBOX(
                ID_NOTREND_CHECK, GOAudioMonitorDlg::OnNoTrendToggled)
                EVT_CHECKBOX(
                  ID_HYBRID_CHECK, GOAudioMonitorDlg::OnHybridToggled)
                EVT_SPINCTRL(
                  ID_HYB_HAND_SPIN, GOAudioMonitorDlg::OnHybHandChanged)
                EVT_SPINCTRL(
                  ID_HYB_CAP_SPIN, GOAudioMonitorDlg::OnHybCapChanged)
                EVT_CHECKBOX(
                  ID_HYB_ALLB_CHECK,
                  GOAudioMonitorDlg::OnHybAllBandsToggled)
                EVT_SPINCTRLDOUBLE(
                  ID_SAFETY_CAP_SPIN, GOAudioMonitorDlg::OnSafetyCapChanged)
                EVT_SPINCTRL(
                  ID_ANCHOR_TAU_SPIN, GOAudioMonitorDlg::OnAnchorTauChanged)
                EVT_SPINCTRLDOUBLE(
                  ID_ROOT_CLAMP_SPIN, GOAudioMonitorDlg::OnRootClampChanged)
                EVT_SPINCTRLDOUBLE(
                  ID_DETECT_RHO_SPIN, GOAudioMonitorDlg::OnDetectRhoChanged)
                  EVT_SPINCTRLDOUBLE(
                    ID_RES_GATE_SPIN, GOAudioMonitorDlg::OnResGateChanged)
                    EVT_CHECKBOX(
                      ID_V0_HINDCAST_CHECK,
                      GOAudioMonitorDlg::OnV0HindcastToggle)
                      EVT_SPINCTRL(
                        ID_REENTRY_FADE_SPIN,
                        GOAudioMonitorDlg::OnReentryFadeChanged)
            EVT_SPINCTRL(ID_PREFETCH_SPIN, GOAudioMonitorDlg::OnPrefetchChanged)
              EVT_SPINCTRL(
                ID_STAGGER_SPIN, GOAudioMonitorDlg::OnStaggerChanged)
              EVT_SPINCTRL(
                ID_XRUN_TEST_SPIN, GOAudioMonitorDlg::OnXrunTestChanged)
              EVT_CHOICE(
                ID_CONCEAL_MODE_CHOICE,
                GOAudioMonitorDlg::OnConcealModeChanged)
              EVT_SPINCTRL(
                ID_MIX_GRACE_SPIN,
                GOAudioMonitorDlg::OnMixGraceChanged)
                EVT_CHECKBOX(
                  ID_PIN_WORKERS, GOAudioMonitorDlg::OnPinWorkersChanged)
                  EVT_CHECKBOX(
                    ID_DECODE_READAHEAD,
                    GOAudioMonitorDlg::OnDecodeReadAheadChanged)
                  EVT_BUTTON(
                    ID_LATENCY_PROBE, GOAudioMonitorDlg::OnLatencyProbe)
            EVT_CHOICE(ID_CB_HELP_CHOICE, GOAudioMonitorDlg::OnCbHelpChanged)
              EVT_BUTTON(ID_ALL_STOPS_BTN, GOAudioMonitorDlg::OnAllStops)
                EVT_CHECKBOX(
                  ID_BENCHMARK_ACTIVE, GOAudioMonitorDlg::OnBenchmarkToggle)
                  EVT_SLIDER(
                    ID_RATE_SLIDER, GOAudioMonitorDlg::OnRateSliderChanged)
                    EVT_SLIDER(
                      ID_HELD_SLIDER, GOAudioMonitorDlg::OnHeldSliderChanged)
                      EVT_SLIDER(
                        ID_HELD_PIPES_SLIDER,
                        GOAudioMonitorDlg::OnHeldPipesSliderChanged)
                      EVT_TIMER(
                        ID_BENCHMARK_TIMER, GOAudioMonitorDlg::OnBenchmarkTimer)
                      EVT_CHECKBOX(
                        ID_CEILING_CHECK, GOAudioMonitorDlg::OnCeilingToggle)
                      EVT_CHECKBOX(
                        ID_AFFINITY_CHECK, GOAudioMonitorDlg::OnAffinityToggle)
                        EVT_BUTTON(wxID_CLOSE, GOAudioMonitorDlg::OnCloseButton)
                          EVT_CLOSE(GOAudioMonitorDlg::OnCloseWindow)
                            wxEND_EVENT_TABLE()

// Ein Text, drei Verwender (Anlage, Konstruktor-Init, Organ-Statuswechsel):
// der Haken braucht IMMER einen erklaerenden Tooltip, nicht nur den
// "No organ loaded."-Hinweis des Sonderfalls.
static wxString BenchmarkActiveTip(bool haveOrgan) {
  return haveOrgan ? _("Starts/stops the note generator: holds the dialed "
                       "number of keys and pipes and keeps firing the "
                       "random attacks, all spread across every manual. "
                       "Unchecking releases everything at once.")
                   : _("No organ loaded.");
}

                              GOAudioMonitorDlg::GOAudioMonitorDlg(
                                wxWindow *parent,
                                GOSoundSystem &soundSystem,
                                std::function<GOOrganModel *()> getOrganModel,
                                std::function<bool(unsigned)> setBufferSize,
                                std::function<void(bool)> setV3AndReload)
  : wxFrame(
    parent,
    wxID_ANY,
    _("Audio Monitor"),
    wxDefaultPosition,
    wxDefaultSize,
    wxMINIMIZE_BOX | wxRESIZE_BORDER | wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX
      | wxCLIP_CHILDREN | wxFULL_REPAINT_ON_RESIZE | wxMAXIMIZE_BOX),
    r_SoundSystem(soundSystem),
    m_GetOrganModel(getOrganModel),
    p_OrganModel(nullptr),
    m_Graph(nullptr),
    m_ShowPolyphony(nullptr),
    m_ShowRenderTime(nullptr),
    m_ShowCallbackTime(nullptr),
    m_ShowCallbackGap(nullptr),
    m_CounterLabel(nullptr),
    m_CreateReportBtn(nullptr),
    m_PrefetchSpin(nullptr),
    m_Timer(this, ID_AUDIO_MONITOR_TIMER),
    m_AllStopsBtn(nullptr),
    m_BenchmarkActive(nullptr),
    m_RateSlider(nullptr),
    m_RateLabel(nullptr),
    m_HeldSlider(nullptr),
    m_HeldLabel(nullptr),
    m_HeldPipesSlider(nullptr),
    m_HeldPipesSpin(nullptr),
    m_HeldPipesLabel(nullptr),
    m_BenchmarkTimer(this, ID_BENCHMARK_TIMER),
    m_AllStopsLatched(false),
    m_NextTriggerDueMs(0),
    m_LastKnownTotalKeys(0),
    m_WorstRenderUs(0),
    m_WorstCallbackUs(0),
    m_WorstGapUs(0),
    m_LastTotalDrops(0),
    m_LastV3Rebuilds(0),
    m_LastV3BatchCalls(0),
    m_LastV3Solo(0),
    m_LastV3BatchRendered(0),
    m_LastV3SyncFallbacks(0),
    m_LastV3ListSamples(0),
    m_LastV3ListVoices(0),
    m_LastV3SampleMs(0) {
  s_Instance = this;
  m_SetBufferSize = std::move(setBufferSize);
  m_SetV3AndReload = std::move(setV3AndReload);
  SetIcon(get_go_icon());

  // Dark theme, matching the graph's own dark plot background. Set before
  // creating any child controls: wx propagates a parent's own (explicitly
  // set) colours to children that opt into inheritance (wxStaticText does;
  // native-themed controls like wxCheckBox/wxButton mostly keep their own
  // system rendering on Windows regardless).
  SetBackgroundColour(wxColour(32, 32, 36));
  SetForegroundColour(wxColour(220, 220, 225));

  // Forward key events to the main window while this window has focus, same
  // as a detached organ panel (GOGUIPanelView) - see OnCharHook().
  if (wxTheApp->GetTopWindow() != this)
    Bind(wxEVT_CHAR_HOOK, &GOAudioMonitorDlg::OnCharHook, this);

  // Fetch the organ once up front so GetTotalAccessibleKeys() below sizes
  // the held-notes slider correctly on the very first layout. Kept in sync
  // afterwards by RefreshOrganAvailability(), called from every UpdateNow()
  // and OnBenchmarkTimer() tick, and by the AbortPlayback() lifecycle hook.
  p_OrganModel = m_GetOrganModel ? m_GetOrganModel() : nullptr;
  if (p_OrganModel)
    p_OrganModel->RegisterLifecycleListener(this);
  m_LastKnownTotalKeys = GetTotalAccessibleKeys();

  wxBoxSizer *topSizer = new wxBoxSizer(wxVERTICAL);

  m_Legend = new wxStaticText(
    this,
    wxID_ANY,
    _("Blue: polyphony usage (left axis, %). Red: period compute time "
        "(WakeupThreads until fully rendered, regardless of who did the "
        "work), green: total callback time, amber: callback gap (right "
        "axis, microseconds - the dashed line is the nominal buffer period; "
        "sustained crossing means the callback budget is gone). Magenta "
      "ticks at the bottom: polyphony-drop "
      "events."));
  /* Wrapped, and re-wrapped on every resize (see OnDialogResize). As one
   * long line its natural width alone would dictate how wide the dialog has
   * to be - and since the minimum size follows the contents, the window
   * could then not be made narrow at all. */
  m_LegendText = m_Legend->GetLabel();
  m_Legend->Wrap(900);
  topSizer->Add(m_Legend, 0, wxALL, 10);

  /* Graph und Zaehlertext haengen an einem ziehbaren Trenner
   * (Nutzerwunsch 12.08.): Der Graph laesst sich damit frei groesser oder
   * kleiner ziehen, und dank Sash-Gravitation 0 bekommt beim Vergroessern
   * des Fensters AUSSCHLIESSLICH der Textbereich den Zuwachs - der Graph
   * behaelt genau die Hoehe, die eingestellt wurde.
   *
   * Die Legenden-Haekchen sitzen mit im oberen Feld, damit die
   * Lesereihenfolge Graph -> Haekchen -> Zahlen erhalten bleibt. */
  m_Split = new wxSplitterWindow(
    this,
    wxID_ANY,
    wxDefaultPosition,
    wxDefaultSize,
    wxSP_LIVE_UPDATE | wxSP_THIN_SASH | wxSP_NOBORDER);
  m_Split->SetMinimumPaneSize(60); // Graph nie ganz wegziehbar
  m_Split->SetSashGravity(0.0);
  ApplyDarkChrome(m_Split);

  wxPanel *graphPane = new wxPanel(m_Split, wxID_ANY);
  wxBoxSizer *graphSizer = new wxBoxSizer(wxVERTICAL);

  ApplyDarkChrome(graphPane);
  m_Graph = new GOAudioMonitorGraph(graphPane, wxID_ANY);
  m_Graph->SetMinSize(wxSize(460, 120));
  graphSizer->Add(m_Graph, 1, wxEXPAND);

  wxBoxSizer *checkSizer = new wxBoxSizer(wxHORIZONTAL);

  m_ShowPolyphony = new GODarkCheckBox(graphPane, ID_SHOW_POLYPHONY, _("Polyphony"));
  m_ShowPolyphony->SetValue(true);
  m_ShowPolyphony->SetToolTip(
    _("Graph curve: active voices as a percent of the polyphony limit - "
      "the load the engine is under."));
  checkSizer->Add(m_ShowPolyphony, 0, wxRIGHT, 12);

  m_ShowRenderTime
    = new GODarkCheckBox(graphPane, ID_SHOW_RENDER_TIME, _("Compute time"));
  m_ShowRenderTime->SetValue(true);
  m_ShowRenderTime->SetToolTip(
    _("Graph curve: worst per-period render time in the poll window (us) "
      "- the engine's own computation, to be compared against the period "
      "budget."));
  checkSizer->Add(m_ShowRenderTime, 0, wxRIGHT, 12);

  m_ShowCallbackTime
    = new GODarkCheckBox(graphPane, ID_SHOW_CALLBACK_TIME, _("Callback time"));
  m_ShowCallbackTime->SetValue(true);
  m_ShowCallbackTime->SetToolTip(
    _("Graph curve: how long the audio callback took (us), INCLUDING any "
      "wait for a late mix - green reaching red usually means waiting, "
      "not computing."));
  checkSizer->Add(m_ShowCallbackTime, 0, wxRIGHT, 12);

  m_ShowCallbackGap
    = new GODarkCheckBox(graphPane, ID_SHOW_CALLBACK_GAP, _("Callback gap"));
  m_ShowCallbackGap->SetValue(true);
  m_ShowCallbackGap->SetToolTip(
    _("Graph curve: time between callback arrivals beyond the nominal "
      "period (us) - the backend's delivery jitter, independent of GO's "
      "compute load."));
  checkSizer->Add(m_ShowCallbackGap, 0, wxRIGHT, 12);

  m_ShowCbShare = new GODarkCheckBox(graphPane, ID_SHOW_CB_SHARE, _("CB work"));
  m_ShowCbShare->SetValue(true);
  m_ShowCbShare->SetToolTip(
    _("Graph curve: percent of processed voices the realtime audio "
      "thread rendered itself (callback help / cleanup sweep) instead of "
      "the workers - the live view of \"the callback computes along\"."));
  checkSizer->Add(m_ShowCbShare, 0);

  graphSizer->Add(checkSizer, 0, wxTOP, 6);
  graphPane->SetSizer(graphSizer);

  /* Der Zaehlertext haengt in einem Rollfenster, und das ist der Kern der
   * Hoehenverteilung: Ein nackter wxStaticText meldet als BESTE Groesse
   * seinen ganzen Inhalt, und da der Graph das einzige dehnbare Element
   * ist, ging jede neue Textzeile direkt von der Graphhoehe ab - bis der
   * Graph verschwand (Nutzerbefund 12.08., nachdem die Timing-Zeilen
   * dazukamen).  Das Rollfenster hat eine EIGENE, feste Mindesthoehe und
   * waechst nicht mit dem Inhalt; was nicht hineinpasst, wird gescrollt.
   *
   * Beide teilen sich den Ueberschuss (Graph 2 : Text 1), damit ein
   * groesseres Fenster beiden zugutekommt - und bei Mindestgroesse
   * behaelt der Graph seine 400 px. */
  m_CounterScroll = new wxScrolledWindow(
    m_Split, wxID_ANY, wxDefaultPosition, wxDefaultSize,
    wxVSCROLL | wxHSCROLL | wxBORDER_NONE);
  m_CounterScroll->SetScrollRate(10, 10);
  ApplyDarkChrome(m_CounterScroll); // sonst leuchtet der Rollbereich weiss
  // wxST_NO_AUTORESIZE ist hier KEINE Kosmetik: ohne den Stil ruft
  // SetLabel() in wxGTK bei jedem 333-ms-Poll InvalidateBestSize +
  // gtk_queue_resize, der ganze Dialog wird neu gelayoutet - und jedes
  // Relayout setzt GTKs ~500-ms-Hover-Timer zurueck, sodass KEIN Tooltip
  // im Dialog je erscheint (Nutzerbefund 12.08.).
  m_CounterLabel = new wxStaticText(
    m_CounterScroll, wxID_ANY, wxEmptyString, wxDefaultPosition,
    wxDefaultSize, wxST_NO_AUTORESIZE);
  // Dicktengleich: ohne feste Zeichenbreite rastert keine einzige Spalte
  // des Statusblocks (siehe StatRow/StatHist).  Etwas kleiner als die
  // Dialogschrift, damit die 78 Spalten bequem hineinpassen.
  {
    wxFont mono(m_CounterLabel->GetFont());

    mono.SetFamily(wxFONTFAMILY_TELETYPE);
    mono.SetFaceName(wxT("Monospace"));
    if (!mono.IsOk())
      mono = wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE));
    mono.SetPointSize(m_CounterLabel->GetFont().GetPointSize() - 1);
    m_CounterLabel->SetFont(mono);
  }
  {
    wxBoxSizer *counterSizer = new wxBoxSizer(wxVERTICAL);

    counterSizer->Add(m_CounterLabel, 0, wxEXPAND);
    m_CounterScroll->SetSizer(counterSizer);
  }
  m_CounterScroll->SetMinSize(
    wxSize(wxDefaultCoord, m_CounterLabel->GetCharHeight() * 8));
  // Startaufteilung: Graph 400 px (der bisherige Mindestwert), Rest Text.
  // Ab hier bestimmt der Nutzer die Graphhoehe mit dem Trenner.
  m_Split->SplitHorizontally(graphPane, m_CounterScroll, 400);
  topSizer->Add(m_Split, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

  wxBoxSizer *tuningSizer = new wxBoxSizer(wxHORIZONTAL);
  // Zweite Zeile: das VERDECKUNGSMODELL (Ordnung, Naht, Trend, Hybrid,
  // Feinregler) - eine Zeile fuer alles war unlesbar geworden.
  wxBoxSizer *tuningSizer2 = new wxBoxSizer(wxHORIZONTAL);
  tuningSizer->Add(
    new wxStaticText(this, wxID_ANY, _("Prefetch distance:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxRIGHT,
    6);
  m_PrefetchSpin = new wxSpinCtrl(
    this,
    ID_PREFETCH_SPIN,
    wxEmptyString,
    wxDefaultPosition,
    wxDefaultSize,
    wxSP_ARROW_KEYS,
    0,
    8,
    // Reflect the engine's actual current setting (GOSoundOrganEngine's own
    // constructor default, tunable here per-machine) rather than hardcoding
    // 0 - previously this always showed "0" even when the engine had
    // already been prefetching for a while.
    r_SoundSystem.GetEngine().GetPrefetchDistance());
  SetCompactWidth(m_PrefetchSpin, 60);
  m_PrefetchSpin->SetToolTip(
    _("Voices to prefetch ahead in the audio loop (0 = off). "
      "Higher values hide RAM latency. Watch 'period compute time' while "
      "adjusting."));
  ApplyDarkChrome(m_PrefetchSpin);
  tuningSizer->Add(m_PrefetchSpin, 0, wxALIGN_CENTER_VERTICAL);

  // Worker stagger STEP (TSC ticks): worker k waits k*step rdtsc ticks
  // (~0.27 ns each) before its burst - cycle-granular staircase. 0 =
  // lockstep. See s_StaggerDelayUs.
  tuningSizer->Add(
    new wxStaticText(this, wxID_ANY, _("Stagger step (ticks):")),
    0,
    wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT,
    6);
  m_StaggerSpin = new wxSpinCtrl(
    this,
    ID_STAGGER_SPIN,
    wxEmptyString,
    wxDefaultPosition,
    wxDefaultSize,
    wxSP_ARROW_KEYS,
    0,
    4000,
    GOSoundThread::GetStaggerDelayUs());
  SetCompactWidth(m_StaggerSpin, 70);
  m_StaggerSpin->SetToolTip(
    _("Per-worker stagger STEP in TSC ticks (~0.27 ns at 3.7 GHz): worker k "
      "waits k*step ticks before each period - cycle-granular staircase, "
      "far finer than µs, to probe a narrow cycle-scale offset. 0 = "
      "lockstep. Sweep small first (10/20/50/100...) while watching IPC / "
      "UTIL / 'period compute time'."));
  ApplyDarkChrome(m_StaggerSpin);
  tuningSizer->Add(m_StaggerSpin, 0, wxALIGN_CENTER_VERTICAL);

  // Deadline-miss concealment test: randomly drop the real mix at this rate
  // and send the emergency shadow buffer instead (GOSoundConcealment), so
  // the concealment chain is audible under low load.  0 = off.
  tuningSizer2->Add(
    new wxStaticText(this, wxID_ANY, _("Forced xruns/min:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT,
    6);
  m_XrunTestSpin = new wxSpinCtrl(
    this,
    ID_XRUN_TEST_SPIN,
    wxEmptyString,
    wxDefaultPosition,
    wxDefaultSize,
    wxSP_ARROW_KEYS,
    0,
    // Deliberately NOT capped at a "reasonable" rate: saturation is a
    // physical quantity - 60 * sampleRate / bufferSize periods exist per
    // minute (22500 at 128 frames / 48 kHz), and beyond that every single
    // period is missed, which is the hardest possible stress test for the
    // shadow's self-feeding.  The injection probability saturates on its
    // own, so no clamping is needed anywhere else.
    60000,
    GOSoundConcealment::GetForcedXrunsPerMinute());
  SetCompactWidth(m_XrunTestSpin, 75);
  m_XrunTestSpin->SetToolTip(
    _("Concealment test: randomly DELAY the real mix past its deadline at "
      "this rate (per minute, per device), so the genuine budget-watch/"
      "bail path runs - test path = real path. The re-entry crossfade "
      "follows one period later. 0 = no injected misses (real ones are "
      "still caught while the method dropdown is not 'off'). Saturation "
      "(every period missed) is 60 * sample rate / buffer size, i.e. "
      "22500 at 128 frames and 48 kHz; above that nothing changes."));
  ApplyDarkChrome(m_XrunTestSpin);
  tuningSizer2->Add(m_XrunTestSpin, 0, wxALIGN_CENTER_VERTICAL);

  // Injection pattern: single misses, frame-skip pairs, or the sustained
  // 1:1 limit case (every second period concealed - 50 % synthetic).
  m_XrunPatternChoice = new wxChoice(
    this, ID_XRUN_PATTERN_CHOICE, wxDefaultPosition, wxDefaultSize, 0,
    nullptr);
  m_XrunPatternChoice->Append(_("single"));
  m_XrunPatternChoice->Append(_("x2 pair"));
  m_XrunPatternChoice->Append(_("1:1"));
  m_XrunPatternChoice->SetSelection(GOSoundConcealment::GetMissPattern());
  m_XrunPatternChoice->SetToolTip(
    _("Forced-miss pattern. single: independent dice per period. x2 pair: "
      "every miss spans two consecutive periods, pairs never back to back "
      "- previews frame-skipping after a late buffer. 1:1: strictly every "
      "second period is concealed for as long as the rate is armed - the "
      "sustained 50 % limit case (the rate value itself is ignored)."));
  ApplyDarkChrome(m_XrunPatternChoice);
  tuningSizer2->Add(
    m_XrunPatternChoice, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);

  // Countermeasure 1: inertia on the per-bin frequency estimate.  100 %
  // means "replace on every analysis" (the classic behaviour).
  tuningSizer2->Add(
    new wxStaticText(this, wxID_ANY, _("freq damp %:")), 0,
    wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
  m_FreqInertiaSpin = new wxSpinCtrl(
    this, ID_FREQ_INERTIA_SPIN, wxEmptyString, wxDefaultPosition,
    wxDefaultSize, wxSP_ARROW_KEYS, 0, 100,
    (int)(GOSoundConcealment::GetFreqDamping() * 100.0f + 0.5f));
  SetCompactWidth(m_FreqInertiaSpin, 55);
  m_FreqInertiaSpin->SetToolTip(
    _("Damping of the per-bin FREQUENCY estimate. 0 % = each analysis "
      "replaces it outright (classic); 100 % = it is frozen after the "
      "first analysis and never moves again. A sluggish estimate cannot "
      "re-fit the predictor's own output, but it only helps where that "
      "estimate actually fluctuates - measured at 50 % concealment, "
      "PV 4k: held real organ tone +0.1 dB (nothing), Vierne finale "
      "+1.5, heavily detuned synthetic chord +2.9. Magnitudes always "
      "stay fresh."));
  ApplyDarkChrome(m_FreqInertiaSpin);
  tuningSizer2->Add(m_FreqInertiaSpin, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

  // Countermeasure 2: let the late mix finish and use it as history.
  GODarkCheckBox *pStrag
    = new GODarkCheckBox(this, ID_STRAGGLER_TRUTH_CHECK, _("straggler"));

  m_StragglerTruthCheck = pStrag;
  pStrag->SetValue(GOSoundConcealment::GetStragglerTruth());
  m_StragglerTruthCheck->SetToolTip(
    _("Let a late mix run to COMPLETION after a miss and feed its result "
      "into the prediction history in place of the prediction. Its audio "
      "is useless for playback (the slot is gone) but it is the truth for "
      "the analysis, which removes the self-feeding at its source: "
      "-10.8 -> -15.1 dB measured. Costs the render time the bail would "
      "have saved, so it is bounded twice: only every SECOND miss keeps "
      "its straggler (-12.6 dB, most of the benefit for half the cost), "
      "and at most one runs at a time - abandoned after all if it has "
      "not finished by the next miss. Still a quality/throughput trade, "
      "not a free win."));
  tuningSizer2->Add(
    m_StragglerTruthCheck, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

  // Order of the per-bin continuation.  1 = the classic rotator.
  // Concealment method for the forced misses above: off = hard silence
  // (the "how it would normally sound" baseline for presentations),
  // PV 2k/4k/8k = phase-vocoder freeze analysis sizes (4k = sweet spot on
  // dense chords, switchable to hear the difference).
  m_ConcealModeChoice = new wxChoice(
    this, ID_CONCEAL_MODE_CHOICE, wxDefaultPosition, wxDefaultSize, 0, nullptr);
  m_ConcealModeChoice->Append(_("off (hard xrun)"));
  m_ConcealModeChoice->Append(_("PV 2k"));
  m_ConcealModeChoice->Append(_("PV 4k"));
  m_ConcealModeChoice->Append(_("PV 8k"));
  // Index 4 = PV 1k, deliberately appended rather than sorted in: 0..3 are
  // baked into saved settings.
  m_ConcealModeChoice->Append(_("PV 1k (cheap)"));
  m_ConcealModeChoice->SetSelection(GOSoundConcealment::GetMode());
  m_ConcealModeChoice->SetToolTip(
    _("Concealment method for the forced xruns: 'off' plays one period of "
      "hard silence (the unconcealed baseline), PV 2k/4k/8k select the "
      "phase-vocoder analysis size of the emergency shadow buffer.\n\n"
      "PV 1k is the SHORT chain the hybrid otherwise only fades in during "
      "an attack, running as the only chain: four times the time "
      "resolution, 47 Hz bins (too coarse to separate the partials of a "
      "chord), no second analysis and no detector. The cheapest way to "
      "make isolated dropouts inaudible - for machines that have no "
      "headroom for 2k plus hybrid. Needs a buffer that divides 256 "
      "(128 or 256); above that it falls back to PV 2k and the status "
      "line below says so."));
  ApplyDarkChrome(m_ConcealModeChoice);
  tuningSizer2->Add(
    m_ConcealModeChoice, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

  tuningSizer2->Add(
    new wxStaticText(this, wxID_ANY, _("AR order:")), 0,
    wxALIGN_CENTER_VERTICAL | wxLEFT, 8);

  wxSpinCtrl *pProny = new wxSpinCtrl(
    this, ID_PRONY_ORDER_CHECK, wxEmptyString, wxDefaultPosition,
    wxSize(56, -1), wxSP_ARROW_KEYS, 1, 8,
    GOSoundConcealment::GetOrder());

  m_PronyOrderCheck = pProny;
  pProny->SetToolTip(
    _("Order of the per-bin continuation. 1 is the phase-vocoder rotator, "
      "an order-1 recursion that is EXACT for ONE partial per bin - a "
      "chord puts three or four into the same 4k bin, where that model is "
      "structurally wrong rather than imprecise. Higher orders fit "
      "X(n) = a1 X(n-1) + ... + ap X(n-p) per bin, the roots being the "
      "partials' rotations, on an analysis hop of frame/8.\n\n"
      "Measured in the engine at PV 4k, 600 misses/min, order 1 -> 4: "
      "single organ tone -15.7 -> -25.3 dB, chord buildup with attacks "
      "-13.8 -> -25.2, full D major chord -4.9 -> -16.5, dense Vierne "
      "-2.4 -> -3.5. Order 6 was measured too and collapses into "
      "overfitting, so 4 is the ceiling.\n\n"
      "COSTS, and NOT only when misses happen - the shadow is computed "
      "every period so it stands ready before a deadline is missed. Per "
      "channel at 128 frames: order 1 64 us, order 2 112, order 3 166, "
      "order 4 214, against a 2667 us period budget. Two channels take "
      "5 / 9 / 13 / 16 % of that budget, eight channels 20 / 37 / 53 / "
      "70 %."));
  ApplyDarkChrome(pProny);
  tuningSizer2->Add(m_PronyOrderCheck, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

  // Seam repair and level trend: both were calibrated for the order-1
  // rotator and cost real quality at the higher orders.
  wxChoice *pSeam = new wxChoice(
    this, ID_SEAM_CHOICE, wxDefaultPosition, wxDefaultSize, 0, nullptr);

  m_SeamChoice = pSeam;
  pSeam->Append(_("seam: adaptive"));
  pSeam->Append(_("seam: anchor"));
  pSeam->Append(_("seam: bridge"));
  pSeam->SetSelection(GOSoundConcealment::GetSeam());
  pSeam->SetToolTip(
    _("How the prediction joins the real signal. ADAPTIVE lets the "
      "density discriminator pick per gap - but that discriminator was "
      "calibrated for the order-1 rotator. At order 4 the anchor is so "
      "good that blending in even the 13 % of Burg bridge the router "
      "chooses on a dense chord costs 1.0 dB (measured 600/min, PV 4k: "
      "-16.5 adaptive against -17.5 forced anchor)."));
  ApplyDarkChrome(m_SeamChoice);
  tuningSizer2->Add(m_SeamChoice, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

  GODarkCheckBox *pNoTrend
    = new GODarkCheckBox(this, ID_NOTREND_CHECK, _("no trend"));

  m_NoTrendCheck = pNoTrend;
  pNoTrend->SetValue(GOSoundConcealment::GetNoTrend());
  pNoTrend->SetToolTip(
    _("Freeze the level through the gap instead of continuing its trend.\n\n"
      "The trend exists because the order-1 rotator has unit-modulus "
      "roots and therefore constant amplitude - without it a swell dies "
      "in the gap. A higher-order recursion does not need that crutch: a "
      "linearly growing sinusoid satisfies an order-4 recursion EXACTLY "
      "(double root on the unit circle), so there the trend double-counts "
      "and destroys what the model already had right. Measured on a "
      "crescendo at 600/min: -127.6 dB with the trend off against -40.4 "
      "with it on; on a dense chord +7.0 dB together with the forced "
      "anchor.\n\n"
      "It is NOT simply a win: at 27 % concealment the self-fed history "
      "has lost the level development, and there the trend is worth "
      "36-39 dB on that same crescendo at every order. Hence a switch "
      "and no default change."));
  tuningSizer2->Add(m_NoTrendCheck, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

  // Zwei-Ketten-Hybrid: das Siegermodell der Lab-Messreihen 06.-08.08.
  GODarkCheckBox *pHyb
    = new GODarkCheckBox(this, ID_HYBRID_CHECK, _("hybrid 1k"));

  pHyb->SetValue(GOSoundConcealment::GetHybrid());
  pHyb->SetToolTip(
    _("Two-chain hybrid (lab winner): a short 1k chain runs warm "
      "alongside; when the 4k onset detector fires, the affected bands "
      "play the short chain and hand back a few rasters later. Measured "
      "at 2k order 8, 600 misses/min: onset gaps -12.1 instead of "
      "-10.8 dB NMR, beating section unchanged.\n\n"
      "COSTS roughly 2x the plain chain, always (the 1k chain must stay "
      "warm to be worth anything): VM-measured 176 -> 326 us per period "
      "at 2 channels.\n\n"
      "Needs a buffer size dividing 256 - at 512+ the library disables "
      "it SILENTLY."));
  ApplyDarkChrome(pHyb);
  tuningSizer2->Add(pHyb, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);

  tuningSizer2->Add(
    new wxStaticText(this, wxID_ANY, _("back@")), 0,
    wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

  wxSpinCtrl *pHybHand = new wxSpinCtrl(
    this, ID_HYB_HAND_SPIN, wxEmptyString, wxDefaultPosition, wxSize(52, -1),
    wxSP_ARROW_KEYS, 1, 40, GOSoundConcealment::GetHybHandOrder());

  pHybHand->SetToolTip(
    _("Hand back to the long chain (X+2) raster points after the onset; "
      "at 2k one raster is two periods, so 8 = 53 ms on the short "
      "chain. Longer protects the transient longer but plays more "
      "short-window texture into the decaying tone."));
  ApplyDarkChrome(pHybHand);
  tuningSizer2->Add(pHybHand, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

  tuningSizer2->Add(
    new wxStaticText(this, wxID_ANY, _("1k ord:")), 0,
    wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

  wxSpinCtrl *pHybCap = new wxSpinCtrl(
    this, ID_HYB_CAP_SPIN, wxEmptyString, wxDefaultPosition, wxSize(52, -1),
    wxSP_ARROW_KEYS, 1, 8, GOSoundConcealment::GetHybShortCap());

  pHybCap->SetToolTip(
    _("Order of the short 1k chain (default 6). Below 4 the onsets INTO "
      "sustained sound suffer: the old tone plus the new attack need "
      "more than two roots per bin (measured -5.5 against -13.3 dB "
      "NMR). From silence the order barely matters."));
  ApplyDarkChrome(pHybCap);
  tuningSizer2->Add(pHybCap, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

  GODarkCheckBox *pHybAll
    = new GODarkCheckBox(this, ID_HYB_ALLB_CHECK, _("all bands"));

  pHybAll->SetValue(GOSoundConcealment::GetHybAllBands());
  pHybAll->SetToolTip(
    _("Switch ALL bands at the onset instead of only the rising ones - "
      "the partial selection cuts partials between the two chains "
      "(measured +16 dB NMR in the hold test)."));
  ApplyDarkChrome(pHybAll);
  tuningSizer2->Add(pHybAll, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

  // ZWEITE Zeile ab hier: die erste war auf einem kleinen Bildschirm
  // breiter als das Fenster, und was rechts herausfiel (Reserve, rho)
  // war schlicht nicht bedienbar.  Schnitt bewusst hinter der
  // Hybrid-Gruppe - was zusammen eingestellt wird, bleibt zusammen.
  wxBoxSizer *tuningSizer3 = new wxBoxSizer(wxHORIZONTAL);

  // Noch nicht endgueltig durchgetestete Feinregler (Lab-Staende als
  // Vorgaben): Sicherheits-Cap, Anker-Tau, Wurzelkappe, Detektor-rho.
  tuningSizer3->Add(
    new wxStaticText(this, wxID_ANY, _("cap:")), 0,
    wxALIGN_CENTER_VERTICAL | wxLEFT, 8);

  wxSpinCtrlDouble *pCap = new wxSpinCtrlDouble(
    this, ID_SAFETY_CAP_SPIN, wxEmptyString, wxDefaultPosition,
    wxSize(64, -1), wxSP_ARROW_KEYS, 0.0, 8.0,
    GOSoundConcealment::GetSafetyCap(), 0.1);

  pCap->SetToolTip(
    _("Safety cap: the shadow is never louder than the recent history "
      "times this factor (scales, does not clip). 1.2 = lab state; 0 "
      "disables."));
  ApplyDarkChrome(pCap);
  tuningSizer3->Add(pCap, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);

  tuningSizer3->Add(
    new wxStaticText(this, wxID_ANY, _("tau:")), 0,
    wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

  wxSpinCtrl *pTau = new wxSpinCtrl(
    this, ID_ANCHOR_TAU_SPIN, wxEmptyString, wxDefaultPosition,
    wxSize(56, -1), wxSP_ARROW_KEYS, 0, 512,
    GOSoundConcealment::GetAnchorTau());

  pTau->SetToolTip(
    _("Anchor time constant in samples; 0 = built-in (rate/3000, 16 at "
      "48 kHz). The anchor subtracts the entry jump as a decaying "
      "exponential - broadband by nature, tau trades bandwidth against "
      "duration."));
  ApplyDarkChrome(pTau);
  tuningSizer3->Add(pTau, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);

  tuningSizer3->Add(
    new wxStaticText(this, wxID_ANY, _("z:")), 0,
    wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

  wxSpinCtrlDouble *pZ = new wxSpinCtrlDouble(
    this, ID_ROOT_CLAMP_SPIN, wxEmptyString, wxDefaultPosition,
    wxSize(64, -1), wxSP_ARROW_KEYS, 0.0, 1.2,
    GOSoundConcealment::GetRootClamp(), 0.01);

  pZ->SetToolTip(
    _("Root-clamp trigger threshold (target is always the unit circle). "
      "1.0 = lab state, only true divergence is pulled back; 0.90 was "
      "the old built-in, which froze transient modes into eternal "
      "oscillators. With rho > 0 only order 1 still reads this."));
  ApplyDarkChrome(pZ);
  tuningSizer3->Add(pZ, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);

  tuningSizer3->Add(
    new wxStaticText(this, wxID_ANY, _("rho:")), 0,
    wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

  wxSpinCtrlDouble *pRho = new wxSpinCtrlDouble(
    this, ID_DETECT_RHO_SPIN, wxEmptyString, wxDefaultPosition,
    wxSize(64, -1), wxSP_ARROW_KEYS, 0.0, 1.2,
    GOSoundConcealment::GetDetectRho(), 0.01);

  pRho->SetToolTip(
    _("Detector mode: raw coefficients pass a rho-Schur test; passing "
      "bins keep their fitted envelopes (no root solve), failing ones "
      "get capped AT rho instead of lifted to 1.0. 1.02 = lab state; "
      "0 = classic path (z clamp). Values well below 1 audibly bleed "
      "the gap dry - that is the knob biting, not a defect."));
  ApplyDarkChrome(pRho);
  tuningSizer3->Add(pRho, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);

  // Residuen-Tor des Transientendetektors.
  tuningSizer3->Add(
    new wxStaticText(this, wxID_ANY, _("res gate:")), 0,
    wxALIGN_CENTER_VERTICAL | wxLEFT, 8);

  wxSpinCtrlDouble *pResGate = new wxSpinCtrlDouble(
    this, ID_RES_GATE_SPIN, wxEmptyString, wxDefaultPosition,
    wxSize(64, -1), wxSP_ARROW_KEYS, 0.0, 1.0,
    GOSoundConcealment::GetDetResGate(), 0.01);

  pResGate->SetToolTip(
    _("Transient detector, residual gate. A band only counts as rising "
      "when its OWN prediction error exceeds this; the width is then "
      "formed from the confirmed bands only. 0 = off.\n\n"
      "This separates what level cannot: beating and decay continue the "
      "long chain, an attack does not. A level floor fails because real "
      "attack bands sit at the same level as the prediction error "
      "(measured: 0.001 of the mean band energy already kills all six "
      "onsets).\n\n"
      "Measured on the D major material at 6000 gaps/min: 0.02 removes "
      "all three plateau false triggers and costs two of six onsets, yet "
      "improves every figure (mean -18.08 -> -18.28 dB, worst case "
      "15.61 -> 15.05, at onsets -13.66 -> -13.81) - a false trigger in "
      "the plateau costs far more than a missed attack."));
  ApplyDarkChrome(pResGate);
  tuningSizer3->Add(pResGate, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);

  // Hindcast-Anker: Hoertest-Schalter Burg-v0 vs. gemessener Versatz.
  GODarkCheckBox *pV0Hind
    = new GODarkCheckBox(this, ID_V0_HINDCAST_CHECK, _("hindcast"));
  pV0Hind->SetValue(GOSoundConcealment::GetV0Hindcast());
  pV0Hind->SetToolTip(
    _("Seam anchor reference: unchecked = Burg one-step prediction of "
      "the next sample (order 32, refit every 8 periods). Checked = "
      "HINDCAST: the synthesis already computes the sample just before "
      "the gap boundary; the seam offset is then MEASURED (shadow[-1] "
      "minus the last real sample, both chains contribute) instead of "
      "predicted, and the whole Burg fit is skipped (4.3 us per round). "
      "Measured 2026-08-12 (D major, 6000 gaps/min, 2k/order 8): "
      "identical without hybrid, onsets identical with hybrid, 0.14 dB "
      "rest on plateaus. Listening switch - default stays Burg until "
      "the ear agrees."));
  tuningSizer3->Add(pV0Hind, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);

  // Wiedereinstiegs-Blende, bislang nur im Lab einstellbar.
  tuningSizer3->Add(
    new wxStaticText(this, wxID_ANY, _("Rejoin (smp):")),
    0,
    wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT,
    6);
  wxSpinCtrl *pReentry = new wxSpinCtrl(
    this,
    ID_REENTRY_FADE_SPIN,
    wxEmptyString,
    wxDefaultPosition,
    wxDefaultSize,
    wxSP_ARROW_KEYS,
    0,
    512,
    GOSoundConcealment::GetReentryFade());
  SetCompactWidth(pReentry, 70);
  pReentry->SetToolTip(
    _("Re-entry crossfade (prediction back to the real signal) in "
      "samples. 0 = auto: 3 ms, capped at one period - 128 samples = "
      "2.67 ms at this buffer size (the fade must fit into the first "
      "real period, the buffers are sized for it; larger values clamp "
      "to that cap). Shorter is free down to 1 sample - the lab series "
      "went down to 0.2 ms. Short fades are what keeps the re-entry "
      "phase error benign: the comb between drifted prediction and real "
      "signal only lasts as long as the fade."));
  ApplyDarkChrome(pReentry);
  tuningSizer3->Add(pReentry, 0, wxALIGN_CENTER_VERTICAL);


  // Mix grace: tolerated lateness of a mix, as a percent of the period.
  // Renamed from "Reserve (us)" 12.08.2026 - that name measured from the
  // WRONG end of the timeline (distance to the next callback) and made
  // 200 us look like a safety margin when it meant "tolerate 93 % of a
  // period of phase slip".
  tuningSizer3->Add(
    new wxStaticText(this, wxID_ANY, _("Mix grace (%):")),
    0,
    wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT,
    6);
  m_MixGraceSpin = new wxSpinCtrl(
    this,
    ID_MIX_GRACE_SPIN,
    wxEmptyString,
    wxDefaultPosition,
    wxDefaultSize,
    wxSP_ARROW_KEYS,
    0,
    100,
    GOSoundConcealment::GetMixGracePct());
  SetCompactWidth(m_MixGraceSpin, 70);
  m_MixGraceSpin->SetToolTip(
    _("Mix grace as a percent of the period: how long the callback waits "
      "for a late mix before delivering concealment. Every microsecond "
      "spent waiting is taken from the NEXT period's head start - the "
      "mix after a bail starts only when the callback returns. The "
      "remaining (100 - grace) must cover the delivery chain, the next "
      "mix AND the shadow round; a shadow start missed anyway is caught "
      "up automatically. 0 = never wait (a late mix is concealed "
      "immediately, zero tolerated phase slip). 100 = LEGACY blocking "
      "delivery (incl. callback-help, no concealment) - the A/B "
      "reference for ceiling benchmarks."));
  ApplyDarkChrome(m_MixGraceSpin);
  tuningSizer3->Add(m_MixGraceSpin, 0, wxALIGN_CENTER_VERTICAL);

  // Pin workers 1:1 to logical CPUs (affinity mode 2), removing migration so
  // the stagger maps deterministically onto HT-sibling pairs. See
  // GOSoundThread::ApplyWorkerAffinity.
  m_PinWorkersCheck = new wxCheckBox(
    this, ID_PIN_WORKERS, _("Pin 1:1"), wxDefaultPosition, wxDefaultSize);
  m_PinWorkersCheck->SetValue(GOSoundThread::GetAffinityMode() == 2);
  m_PinWorkersCheck->SetToolTip(
    _("Pin each worker thread to its own logical CPU (no migration), so the "
      "stagger lands a deterministic anti-phase on every HT-sibling pair. "
      "Combine with 'Stagger step'. Off = scheduler decides placement."));
  ApplyDarkChrome(m_PinWorkersCheck);
  tuningSizer->Add(
    m_PinWorkersCheck, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);

  // Toggle the next-period decode-input read-ahead (GOSoundStream Family 4):
  // a prefetcht2 of ~one callback's compressed residual span into L3, a full
  // period ahead, for every voice. Warms the one DRAM-cold stream but adds
  // speculative bandwidth that competes with the current period at the 24k
  // cliff. Off = test whether killing it matches "prefetch distance 0 = best
  // headroom". Default on = current, bit-identical behaviour.
  m_ReadAheadCheck = new wxCheckBox(
    this, ID_DECODE_READAHEAD, _("Decode read-ahead"), wxDefaultPosition,
    wxDefaultSize);
  m_ReadAheadCheck->SetValue(GOSoundStream::IsDecodeReadAheadEnabled());
  m_ReadAheadCheck->SetToolTip(
    _("Prefetch next period's compressed residual span into L3 one period "
      "ahead, per voice (GOSoundStream read-ahead). On = current behaviour; "
      "off = no speculative decode-input prefetch, to A/B the headroom at "
      "high polyphony. Independent of the 'Prefetch' distance knob."));
  ApplyDarkChrome(m_ReadAheadCheck);
  tuningSizer->Add(
    m_ReadAheadCheck, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);

  // Manual working-set latency probe: a data-dependent pointer chase over
  // the mapped sample cache, measuring true load-to-use latency (defeats
  // prefetch/OoO). Button-triggered so it never perturbs a running
  // polyphony test. Result in the label + the GO log; compare FAST vs SLOW.
  m_LatencyProbeBtn = new wxButton(
    this, ID_LATENCY_PROBE, _("Probe latency"), wxDefaultPosition,
    wxDefaultSize, wxBU_EXACTFIT);
  m_LatencyProbeBtn->SetToolTip(
    _("Chase a dependent pointer chain through the mapped sample cache and "
      "report the true DRAM load-to-use latency (ns/access). Run once in a "
      "FAST and once in a SLOW process - higher ns/access in SLOW at equal "
      "miss counts confirms physical-placement latency as the die. Brief "
      "(~50 ms) busy loop; press between tests, not during."));
  ApplyDarkChrome(m_LatencyProbeBtn);
  tuningSizer->Add(m_LatencyProbeBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);

  m_LatencyLabel = new wxStaticText(this, wxID_ANY, _("latency: -"));
  tuningSizer->Add(
    m_LatencyLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

  tuningSizer->Add(
    new wxStaticText(this, wxID_ANY, _("Callback help:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT,
    6);

  wxChoice *cbHelpChoice = new wxChoice(this, ID_CB_HELP_CHOICE);

  cbHelpChoice->Append(_("always"));
  cbHelpChoice->Append(_("never"));
  cbHelpChoice->Append(_("deadline"));
  cbHelpChoice->SetSelection(
    r_SoundSystem.GetEngine().GetCallbackHelpPolicy());
  cbHelpChoice->SetToolTip(
    _("Whether the TIME_CRITICAL audio thread joins the period's voice "
      "work. 'always' = historical behaviour (21 compute threads on 20 "
      "logical CPUs at full load - can evict a worker mid-claim); "
      "'never' = wait on the condition only; 'deadline' = help only once "
      "~70% of the period budget has elapsed."));
  ApplyDarkChrome(cbHelpChoice);
  tuningSizer->Add(cbHelpChoice, 0, wxALIGN_CENTER_VERTICAL);

  GODarkCheckBox *affinityCheck
    = new GODarkCheckBox(this, ID_AFFINITY_CHECK, _("Pin audio core"));
  affinityCheck->SetToolTip(
    _("Experiment: pins the TIME_CRITICAL audio thread to logical CPU 0 "
      "and keeps the worker threads off core 0 (both hyperthreads). "
      "Trades one worker CPU for an exclusive, jitter-free core for the "
      "callback. Applied within one period; uncheck to restore OS "
      "scheduling."));
  tuningSizer->Add(affinityCheck, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 14);
  topSizer->Add(tuningSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
  topSizer->Add(tuningSizer2, 0, wxLEFT | wxRIGHT, 10);
  topSizer->Add(tuningSizer3, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxTOP, 10);

  // --- Polyphony benchmark ---
  // Enables all stops/couplers (maximum ranks per keypress) plus a random
  // note generator: a target number of notes held indefinitely (simulated
  // sustained chord) and a configurable rate of short transient attacks on
  // top, both spread randomly across all manuals. See OnBenchmarkTimer().
  wxStaticBoxSizer *benchSizer
    = new wxStaticBoxSizer(wxVERTICAL, this, _("Polyphony Benchmark"));

  wxBoxSizer *benchTopRow = new wxBoxSizer(wxHORIZONTAL);
  m_AllStopsBtn
    = new wxButton(this, ID_ALL_STOPS_BTN, _("Enable All Stops && Couplers"));
  m_AllStopsBtn->SetToolTip(
    _("Engages every stop and coupler on every manual, so each keypress "
      "sounds through as many ranks as possible - for maximum polyphony "
      "load."));
  ApplyDarkChrome(m_AllStopsBtn);
  benchTopRow->Add(m_AllStopsBtn, 0, wxRIGHT, 12);
  m_BenchmarkActive
    = new GODarkCheckBox(this, ID_BENCHMARK_ACTIVE, _("Benchmark active"));
  benchTopRow->Add(m_BenchmarkActive, 0, wxALIGN_CENTER_VERTICAL);
  benchSizer->Add(benchTopRow, 0, wxALL, 6);

  wxBoxSizer *rateRow = new wxBoxSizer(wxHORIZONTAL);
  rateRow->Add(
    new wxStaticText(this, wxID_ANY, _("Random attacks/min:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxRIGHT,
    6);
  m_RateSlider = new wxSlider(
    this,
    ID_RATE_SLIDER,
    0, // default 0: transients only when explicitly dialed in
    0,
    RATE_SLIDER_MAX_KPM,
    wxDefaultPosition,
    wxSize(180, -1));
  m_RateSlider->SetToolTip(
    _("Short random attacks per minute, spread across all manuals - the "
      "transient load on top of the held notes. Stresses attack-phase "
      "decode cost and the concealment's onset detector; 0 = sustained "
      "load only."));
  // wxEVT_SLIDER (bound via EVT_SLIDER in the event table below) already
  // fires continuously while dragging on most platforms, but that's not
  // guaranteed everywhere - THUMBTRACK is the event explicitly documented to
  // fire on every intermediate position during a drag, before release, so
  // bind it too rather than relying on EVT_SLIDER alone. wxScrollEvent is a
  // wxCommandEvent, so the existing wxCommandEvent&-taking handler applies
  // unchanged; firing twice for the same move is harmless (cheap label
  // update / held-notes resync).
  m_RateSlider->Bind(
    wxEVT_SCROLL_THUMBTRACK, &GOAudioMonitorDlg::OnRateSliderChanged, this);
  rateRow->Add(m_RateSlider, 1, wxALIGN_CENTER_VERTICAL);
  m_RateLabel = new wxStaticText(this, wxID_ANY, wxEmptyString);
  SetMinWidthForText(m_RateLabel, wxT("9999/min"));
  m_RateLabel->SetLabel(
    wxString::Format(_("%d/min"), m_RateSlider->GetValue()));
  rateRow->Add(m_RateLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
  benchSizer->Add(rateRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

  const unsigned totalKeys = GetTotalAccessibleKeys();

  wxBoxSizer *heldRow = new wxBoxSizer(wxHORIZONTAL);
  heldRow->Add(
    new wxStaticText(this, wxID_ANY, _("Held keys:")),
    0,
    wxALIGN_CENTER_VERTICAL | wxRIGHT,
    6);
  m_HeldSlider = new wxSlider(
    this,
    ID_HELD_SLIDER,
    0,
    0,
    totalKeys ? (int)totalKeys : 1,
    wxDefaultPosition,
    wxSize(180, -1));
  m_HeldSlider->SetToolTip(
    _("Keys held indefinitely (a simulated sustained chord), picked "
      "randomly across all manuals. With all stops and couplers engaged "
      "every key sounds through the maximum number of ranks; the ceiling "
      "is the organ's total key count."));
  m_HeldSlider->Enable(totalKeys > 0);
  m_HeldSlider->Bind(
    wxEVT_SCROLL_THUMBTRACK, &GOAudioMonitorDlg::OnHeldSliderChanged, this);
  heldRow->Add(m_HeldSlider, 1, wxALIGN_CENTER_VERTICAL);
  m_HeldLabel = new wxStaticText(this, wxID_ANY, wxEmptyString);
  SetMinWidthForText(
    m_HeldLabel, wxString::Format(wxT("%u / %u"), totalKeys, totalKeys));
  m_HeldLabel->SetLabel(wxString::Format(_("0 / %u"), totalKeys));
  heldRow->Add(m_HeldLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
  benchSizer->Add(heldRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

  wxBoxSizer *heldPipesRow = new wxBoxSizer(wxHORIZONTAL);
  wxStaticText *heldPipesCaption
    = new wxStaticText(this, wxID_ANY, _("Held pipes:"));

  heldPipesCaption->SetToolTip(
    _("Pure sustain-polyphony dial: starts random pipe voices that never "
      "get a note-off until the slider comes down or the benchmark stops. "
      "Scales beyond the organ's key/stop count (the same pipe may sound "
      "multiple times). Raise the polyphony limit above the target first, "
      "or the limiter's drops falsify the measurement."));
  heldPipesRow->Add(heldPipesCaption, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
  m_HeldPipesSlider = new wxSlider(
    this, ID_HELD_PIPES_SLIDER, 0, 0, 30000, wxDefaultPosition, wxSize(180, -1));
  m_HeldPipesSlider->SetToolTip(
    _("Pure sustain-polyphony dial: random pipe voices with no note-off "
      "until the slider comes down. Scales beyond the key/stop count - "
      "the same pipe may sound multiple times. Raise the polyphony limit "
      "above the target first, or the limiter's drops falsify the "
      "measurement. For reproducible load points use the exact-entry "
      "field to the right."));
  m_HeldPipesSlider->Enable(totalKeys > 0);
  m_HeldPipesSlider->Bind(
    wxEVT_SCROLL_THUMBTRACK,
    &GOAudioMonitorDlg::OnHeldPipesSliderChanged,
    this);
  heldPipesRow->Add(m_HeldPipesSlider, 1, wxALIGN_CENTER_VERTICAL);
  /* Exact target entry: a slider cannot hit a specific voice count again,
   * and comparing two builds REQUIRES the identical load - the per-voice
   * cost depends on it. (2026-07-15: two builds were measured at 10037 vs
   * 10710 voices and the difference was read as a code regression.) */
  m_HeldPipesSpin = new wxSpinCtrl(
    this,
    ID_HELD_PIPES_SPIN,
    wxEmptyString,
    wxDefaultPosition,
    wxDefaultSize,
    wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER,
    0,
    30000,
    0);
  SetCompactWidth(m_HeldPipesSpin, 80);
  m_HeldPipesSpin->SetToolTip(
    _("Exact held-pipe target - type a number to reproduce a load point "
      "precisely (the slider cannot hit the same value twice). Comparing "
      "two builds only means anything at the identical voice count."));
  m_HeldPipesSpin->Enable(totalKeys > 0);
  m_HeldPipesSpin->Bind(
    wxEVT_SPINCTRL, &GOAudioMonitorDlg::OnHeldPipesSpin, this);
  m_HeldPipesSpin->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) {
    SyncHeldPipesTarget(m_HeldPipesSpin->GetValue(), true);
  });
  ApplyDarkChrome(m_HeldPipesSpin);
  heldPipesRow->Add(m_HeldPipesSpin, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
  m_HeldPipesLabel = new wxStaticText(this, wxID_ANY, wxT("0 / 0"));
  // Widest case: the spin control's own maximum on both sides of the slash.
  SetMinWidthForText(m_HeldPipesLabel, wxT("30000 / 30000"));
  heldPipesRow->Add(m_HeldPipesLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
  benchSizer->Add(heldPipesRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

  wxBoxSizer *ceilRow = new wxBoxSizer(wxHORIZONTAL);
  m_CeilCheck
    = new GODarkCheckBox(this, ID_CEILING_CHECK, _("Find ceiling (auto sweep)"));
  m_CeilCheck->SetToolTip(
    _("Fully automated, objective capacity benchmark: finds the largest "
      "glitch-free held-pipes polyphony (zero gaps/underruns/drops over an "
      "observation window) for every codec x buffer-size combination "
      "(V3/legacy x 128/256/512), switching the audio buffer (sound restart) "
      "and the codec (organ reload!) by itself. Start it and walk away - a "
      "full sweep takes many minutes and reloads the organ twice. Your "
      "original settings are restored afterwards; results appear in the "
      "counters above and in every report."));
  m_CeilCheck->Enable(m_SetBufferSize && m_SetV3AndReload && totalKeys > 0);
  ceilRow->Add(m_CeilCheck, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  m_CeilStatus = new wxStaticText(this, wxID_ANY, wxEmptyString);
  ceilRow->Add(m_CeilStatus, 1, wxALIGN_CENTER_VERTICAL);
  benchSizer->Add(ceilRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

  m_BenchmarkActive->Enable(totalKeys > 0);
  m_BenchmarkActive->SetToolTip(BenchmarkActiveTip(totalKeys > 0));

  topSizer->Add(benchSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  wxBoxSizer *btnSizer = new wxBoxSizer(wxHORIZONTAL);
  m_CreateReportBtn
    = new wxButton(this, ID_CREATE_REPORT, _("Create Report..."));
  m_CreateReportBtn->SetToolTip(
    _("Writes a zipped snapshot of everything this window shows - "
      "summary.txt (all counters and configuration), samples.csv (the "
      "graph's time series), graph.png and sysinfo.txt - into a chosen "
      "folder (remembered across restarts). A read-only snapshot: the "
      "cumulative counters keep running."));
  ApplyDarkChrome(m_CreateReportBtn);
  btnSizer->Add(m_CreateReportBtn, 0, wxRIGHT, 8);

  wxButton *cacheBinsBtn
    = new wxButton(this, ID_CACHE_BINS_BTN, _("Cache Bins..."));
  ApplyDarkChrome(cacheBinsBtn);
  cacheBinsBtn->SetToolTip(_(
    "Histogram of the loaded sample set over the V3 render-subblock bin "
    "ladder (per V3-compressed section, by source/device sample-rate "
    "ratio). Empty unless the V3 subblock cache is enabled."));
  btnSizer->Add(cacheBinsBtn, 0, wxRIGHT, 8);
  btnSizer->AddStretchSpacer();

  wxButton *closeBtn = new wxButton(this, wxID_CLOSE, _("Close"));
  ApplyDarkChrome(closeBtn);
  btnSizer->Add(closeBtn, 0);
  topSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 10);

  SetSizerAndFit(topSizer);
  /* SetSizerAndFit made the sizer's best size the window MINIMUM - with
   * three fixed-width slider rows that exceeded smaller (non-4K) screens.
   * Relax the minimum (the sizers compress fine) and clamp the initial
   * size to the actual work area so the dialog never opens off-screen. */
  const wxSize fitted = GetSize(); // what the controls actually need
  // wxDisplay(const wxWindow*) isn't available/resolved consistently
  // across every wx build config in CI (invalid conversion to unsigned
  // int on some) - the portable idiom is GetFromWindow() + the
  // always-present index constructor.
  const int displayIdx = wxDisplay::GetFromWindow(this);
  const wxSize workArea
    = wxDisplay(displayIdx == wxNOT_FOUND ? 0u : (unsigned)displayIdx)
        .GetClientArea()
        .GetSize();
  /* The minimum follows the content, but never exceeds the screen.
   *
   * SetSizerAndFit had made the fitted size the hard minimum, which on
   * smaller (non-4K) screens was larger than the display itself - hence the
   * previous flat 640x480. That went too far the other way: the sizers do
   * not compress below their contents, they clip, so the window could be
   * dragged small enough to cut the sliders and buttons off entirely with
   * no way to reach them. Capping at the work area keeps the dialog usable
   * on small displays while letting large ones keep every control
   * reachable. (A scrolled container would remove the trade-off entirely -
   * worth doing if this dialog ever has to work on a genuinely small
   * screen.) */
  wxSize minSize = fitted;

  minSize.DecTo(workArea);
  SetMinSize(minSize);
  /* Never open SMALLER than that. The 960x840 preference below was measured
   * on Windows; GTK's checkboxes, spin controls and sliders are noticeably
   * taller, so on Linux the fixed rows alone could exceed that height - and
   * since the graph is the sizer's only stretchable item (proportion 1), it
   * was the one that absorbed the deficit, down to an unusable sliver. Using
   * the fitted size as a floor keeps the roomy default without letting a
   * number measured on one platform crush the layout on another. */
  wxSize initial(std::max(960, fitted.x), std::max(840, fitted.y));
  initial.DecTo(workArea);
  SetSize(initial);
  // Re-flows the legend; see OnDialogResize.
  Bind(wxEVT_SIZE, &GOAudioMonitorDlg::OnDialogResize, this);

  const unsigned samplesPerBuffer = r_SoundSystem.GetSamplesPerBuffer();
  const unsigned sampleRate = r_SoundSystem.GetSampleRate();

  m_Graph->SetNominalPeriodUs(
    sampleRate ? (int64_t)samplesPerBuffer * 1000000 / (int64_t)sampleRate : 0);
  m_Graph->SetPollIntervalMs(POLL_INTERVAL_MS);

  // Unique window name for the single-instance lookup in
  // GOAppWindow::ShowAudioMonitor (FindWindowByName).
  SetName(wxT("GOAudioMonitorDlg"));

  // Arm the per-voice-call window counters (fast-path / voice-mix histogram)
  // only while this window exists - they are atomic read-modify-writes in
  // the audio hotpath and are skipped entirely when disarmed.
  r_SoundSystem.GetEngine().SetVoiceStatsEnabled(true);

  UpdateNow();
  m_Timer.Start(POLL_INTERVAL_MS);
}

GOAudioMonitorDlg::~GOAudioMonitorDlg() {
  if (s_Instance == this)
    s_Instance = nullptr;
  r_SoundSystem.GetEngine().SetVoiceStatsEnabled(false);
  if (wxTheApp && wxTheApp->GetTopWindow() != this)
    Unbind(wxEVT_CHAR_HOOK, &GOAudioMonitorDlg::OnCharHook, this);
  // Safe here even without an AbortPlayback() in between: this destructor
  // only runs because the user closed this window while the organ is
  // otherwise untouched, so p_OrganModel (last synced by
  // RefreshOrganAvailability()/AbortPlayback()) is still valid.
  if (p_OrganModel)
    p_OrganModel->UnRegisterLifecycleListener(this);
}

void GOAudioMonitorDlg::AbortPlayback() {
  // Broadcast synchronously by GOOrganController::Abort() while p_OrganModel
  // is still safe to use (see the class-level comment on this override) -
  // release everything referencing it, then treat it as gone immediately:
  // the next RefreshOrganAvailability() poll must not see the same (about
  // to be dangling) pointer value and conclude nothing changed.
  // Held-pipe handles first, WITHOUT stopping: playback is going away,
  // and StopBenchmark below must not send 30k note-offs into a dying
  // engine. The pipe pool refers to the dying organ too.
  ClearHeldPipes(false);
  m_PipePool.clear();
  StopBenchmark();
  ResetForciblyEngagedStops();
  m_BenchmarkActive->SetValue(false);
  p_OrganModel = nullptr;
}

void GOAudioMonitorDlg::UpdateNow() {
  // Fire once if the worker pool did NOT all land at the same scheduling
  // priority (partial MMCSS grant / mixed elevation). Runs on the GUI
  // thread (safe for wxLog); guarded so it warns only on the first
  // detection. User-requested: catch a non-uniform-priority pool if it ever
  // happens.
  if (!m_PrioMismatchLogged) {
    GOSoundOrganEngine &eng = r_SoundSystem.GetEngine();
    unsigned nM = 0, nF = 0, nO = 0;

    eng.CountWorkerElevations(nM, nF, nO);
    if (nM + nF + nO > 0) { // pool has started
      const unsigned distinctElev = (nM > 0) + (nF > 0) + (nO > 0);
      int minP = 0, maxP = 0;
      const bool haveP = eng.GetWorkerPriorityRange(minP, maxP);

      if (distinctElev > 1 || (haveP && minP != maxP)) {
        wxLogWarning(
          wxT("Worker priority NON-UNIFORM: elevation %u MMCSS / %u "
              "fallback / %u other, GetThreadPriority %d..%d - the pool did "
              "not all get the same scheduling priority."),
          nM,
          nF,
          nO,
          minP,
          maxP);
        m_PrioMismatchLogged = true;
      }
    }
  }

  /* Refresh the graph's right-axis scale every poll: when the monitor is
   * reopened automatically at startup (before any organ is loaded), the
   * sound system reports sample rate 0 and the constructor's one-shot
   * setup left the axis at its 1 us fallback forever - the scale never
   * became the 2x-buffer-period ms scale once real data arrived. Also
   * picks up mid-session audio device/buffer changes. */
  {
    const unsigned samplesPerBuffer = r_SoundSystem.GetSamplesPerBuffer();
    const unsigned sampleRate = r_SoundSystem.GetSampleRate();

    m_Graph->SetNominalPeriodUs(
      sampleRate ? (int64_t)samplesPerBuffer * 1000000 / (int64_t)sampleRate
                 : 0);
  }

  using namespace GOAudioParams;

  RefreshOrganAvailability();

  GOSoundOrganEngine &engine = r_SoundSystem.GetEngine();
  const unsigned hardPolyphony = engine.GetHardPolyphony();
  const unsigned usedPolyphony = engine.GetAndResetUsedPolyphonyPeak();
  const float polyphonyPct = hardPolyphony
    ? 100.0f * (float)usedPolyphony / (float)hardPolyphony
    : 0.0f;

  const unsigned fastPathVoices = engine.GetAndResetFastPathVoices();
  const unsigned totalVoicesWindow = engine.GetAndResetTotalVoicesWindow();
  const GOSoundOrganEngine::FaderKernelStats faderStats
    = engine.GetAndResetFaderKernelStats();
  const unsigned compressedVoices = engine.GetAndResetCompressedVoices();

  const int64_t computeUs = engine.GetAndResetPeriodCompletionPeakUs();
  const GOSoundSystem::RenderGapPeaks peaks
    = r_SoundSystem.GetAndResetRenderGapPeaks();

  m_WorstRenderUs = std::max(m_WorstRenderUs, computeUs);
  m_WorstCallbackUs
    = std::max(m_WorstCallbackUs, peaks.longestCallbackUs);
  m_WorstGapUs = std::max(m_WorstGapUs, peaks.longestCallbackGapUs);

  // Compute per-interval drop delta from the cumulative counters.
  const uint64_t totalDrops
    = engine.GetPolyphonyPanicDrops() + engine.GetPolyphonyNormalDrops();
  const uint32_t dropsThisInterval = (uint32_t)(totalDrops - m_LastTotalDrops);
  m_LastTotalDrops = totalDrops;
  // Also consume the windowed atomic so it stays in sync.
  engine.GetAndResetPolyphonyDropCount();

  /* "CB work" share: percentage of this poll window's processed voices
   * that the TIME_CRITICAL audio thread handled itself (Finish help /
   * Exec broom) - the live view of the "callback computes along"
   * effect. */
  float cbSharePct = 0.0f;
  {
    const GOSoundGroupTask::DecodeStats v3p
      = GOSoundGroupTask::GetDecodeStats();
    const uint64_t dCb = v3p.cbVoices - m_LastV3CbVoices;
    const uint64_t dAll = v3p.rebuilds - m_LastV3RebuildsGraph;

    m_LastV3CbVoices = v3p.cbVoices;
    m_LastV3RebuildsGraph = v3p.rebuilds;
    if (dAll > 0)
      cbSharePct = 100.0f * (float)dCb / (float)dAll;
  }

  m_Graph->PushSample(
    polyphonyPct,
    (float)computeUs,
    (float)peaks.longestCallbackUs,
    (float)peaks.longestCallbackGapUs,
    dropsThisInterval,
    cbSharePct,
    GOSoundConcealment::FetchOnsetDelta());

  // One controller step of the automated ceiling sweep (no-op when idle).
  // May switch buffer size or codec (organ reload) synchronously - engine
  // and sound system objects survive both, only the counters restart.
  CeilingTick(computeUs, peaks.longestCallbackUs, usedPolyphony);

  const GOSoundSystem::CallbackDiagnostics cb
    = r_SoundSystem.GetCallbackDiagnostics();
  const GOSoundSystem::DeviceLifecycleDiagnostics dev
    = r_SoundSystem.GetDeviceLifecycleDiagnostics();
  const FastCrossfadeCache::Stats cacheStats = FastCrossfadeCache::GetStats();
  const FastCrossfadeCache::PrecomputeStatus precompute
    = FastCrossfadeCache::GetLastPrecomputeStatus();

  wxString text;
  // Zweispaltiger Aufbau: colA (LOAD + TIMING) und colB (CONCEALMENT +
  // CACHES) werden am Ende nebeneinandergelegt, `wide` bleibt volle
  // Breite (die Histogrammtabelle laesst sich nicht sinnvoll halbieren).
  // `timing` ist ein Zwischenpuffer INNERHALB von colA: seine Zeilen
  // entstehen tief in der Delta-Rechnung des V3-Blocks, die wichtigsten
  // Werte (Spitzenzeiten, Underruns) aber erst danach - gepuffert stehen
  // sie trotzdem oben.
  wxString colA, colB, wide, timing;

  const float fastPathPct = totalVoicesWindow
    ? 100.0f * (float)fastPathVoices / (float)totalVoicesWindow
    : 0.0f;

  colA << StatRule(wxT("LOAD"));
  {
    const unsigned long long panicDrops = engine.GetPolyphonyPanicDrops();
    const unsigned long long normDrops = engine.GetPolyphonyNormalDrops();

    colA << StatRow(
      wxT("polyphony"),
      wxString::Format(wxT("%u / %u peak"), usedPolyphony, hardPolyphony),
      panicDrops || normDrops
        ? wxString::Format(
            _("drops       %llu panic / %llu normal"), panicDrops, normDrops)
        : wxString(_("drops       none")));
  }

  /* Per-voice-frame cost over the last ~5.5s of periods that ran at real
   * load (see GOSoundOrganEngine::m_VoiceCostPsRing). Compare two builds by
   * their MIN at the same polyphony: min is the undisturbed best case, i.e.
   * what the code can actually do. A max far above the median means
   * something else on the machine interfered - then the run says nothing
   * about the code, which the ceiling sweep cannot tell you. */
  {
    uint32_t costPs[GOSoundOrganEngine::VOICE_COST_RING];
    const unsigned n
      = engine.GetVoiceCostSamplesPs(costPs, (unsigned)std::size(costPs));

    wxString cost;

    if (n >= 8) {
      std::sort(costPs, costPs + n);
      const double toNs = 1.0 / 1000.0;

      cost = wxString::Format(
        _("min/med/max %.2f/%.2f/%.2f ns"),
        costPs[0] * toNs,
        costPs[n / 2] * toNs,
        costPs[n - 1] * toNs);
    } else
      cost = wxString::Format(
        _("needs %u voices"), GOSoundOrganEngine::VOICE_COST_MIN_VOICES);
    colA << StatRow(
      wxT("voice cost"),
      cost,
      wxString::Format(
        _("fast path   %.0f %% (%u / %u)"),
        fastPathPct,
        fastPathVoices,
        totalVoicesWindow));
  }

  {
    // Voice-mix histogram: which DSP paths the voice calls of the last poll
    // window actually took (see GOFadeKernelKind / m_FaderKernelWindow).
    const unsigned faderTotal = faderStats.constant + faderStats.ramp
      + faderStats.nonLinear + faderStats.legacy;
    const auto pctOf = [](unsigned part, unsigned total) {
      return total ? 100.0f * (float)part / (float)total : 0.0f;
    };

    colA << StatRow(
      wxT("voice mix"),
      wxString::Format(
        _("fader %.0f/%.0f/%.0f/%.0f %%"),
        pctOf(faderStats.constant, faderTotal),
        pctOf(faderStats.ramp, faderTotal),
        pctOf(faderStats.nonLinear, faderTotal),
        pctOf(faderStats.legacy, faderTotal)),
      wxString::Format(
        _("const/ramp/xfade/filt   sections %.0f %% compr."),
        pctOf(compressedVoices, totalVoicesWindow)));
  }
  {
    /* V3 per-voice decode rates since the previous poll (cumulative,
     * engine-wide counters from the group tasks - see
     * GOSoundGroupTask::GetDecodeStats). Shown here instead of the former
     * 10s wxLog line; flows into the audio report via OnCreateReport's
     * counterText. Lane fill = decoded voices that shared a kernel call,
     * per call - the batch kernels always pay full 8-lane SIMD cost, so
     * a low fill means wasted lanes; solo voices never reach a kernel. */
    const GOSoundGroupTask::DecodeStats v3
      = GOSoundGroupTask::GetDecodeStats();
    const wxLongLong nowMs = wxGetLocalTimeMillis();
    const double dt = m_LastV3SampleMs != 0
      ? (nowMs - m_LastV3SampleMs).ToDouble() / 1000.0
      : 0.0;

    if (!GOSoundStream::IsSubblockCacheEnabled())
      colA << StatRow(wxT("V3 decode"), _("subblock cache disabled"));
    else if (dt > 0.0) {
      const uint64_t dRebuilds = v3.rebuilds - m_LastV3Rebuilds;
      const uint64_t dCalls = v3.batchCalls - m_LastV3BatchCalls;
      const uint64_t dSolo = v3.soloRebuilds - m_LastV3Solo;
      const uint64_t dFused = v3.batchRendered - m_LastV3BatchRendered;
      const uint64_t dFallbacks = v3.syncFallbacks - m_LastV3SyncFallbacks;
      const double fill
        = dCalls ? (double)(dRebuilds - dSolo) / (double)dCalls : 0.0;
      const uint64_t dLists = v3.listSamples - m_LastV3ListSamples;
      const uint64_t dListVoices = v3.listVoicesSum - m_LastV3ListVoices;

      colA << StatRow(
        wxT("V3 decode"),
        wxString::Format(_("%.2f M voices/s"), dRebuilds / dt / 1e6),
        wxString::Format(
          _("fused %.2f M/s   solo %.0f k/s"),
          dFused / dt / 1e6,
          dSolo / dt / 1e3));
      colA << StatRow(
        wxT("V3 lanes"),
        wxString::Format(_("fill %.2f / 8"), fill),
        wxString::Format(
          _("fallbacks %.0f /s   lists %.0f / max %llu"),
          dFallbacks / dt,
          dLists ? (double)dListVoices / (double)dLists : 0.0,
          (unsigned long long)v3.listSizeMax));
    } else
      colA << StatRow(wxT("V3 decode"), _("collecting..."));
    if (GOSoundStream::IsSubblockCacheEnabled() && dt > 0.0)
      // Scheduler anatomy (per second, same delta window): drain time of
      // the periods' first threads, audio-thread help vs policy denials,
      // and Exec calls that found their period unfinished (the RT thread
      // then runs leftovers synchronously).
      timing << StatRow(
        wxT("scheduler"),
        wxString::Format(
          _("wake %.0f · dispatch %.0f · tail %.0f · drain %.0f   us/s"),
          (double)(engine.GetWakeLoopUsSum() - m_LastWakeUs) / dt,
          (double)(engine.GetDispatchLatUsSum() - m_LastDispatchUs) / dt,
          (double)(engine.GetTailUsSum() - m_LastTailUs) / dt,
          (double)(v3.drainUsSum - m_LastV3DrainUs) / dt));
      timing << StatRow(
        wxT("cb help"),
        wxString::Format(
          _("%.0f /s (denied %.0f)"),
          (double)(v3.cbHelpRuns - m_LastV3CbRuns) / dt,
          (double)(v3.cbHelpDenied - m_LastV3CbDenied) / dt),
        wxString::Format(
          _("execLate    %.1f /s"),
          (double)(v3.execLate - m_LastV3ExecLate) / dt));
      m_LastWakeUs = engine.GetWakeLoopUsSum();
      m_LastDispatchUs = engine.GetDispatchLatUsSum();
      m_LastTailUs = engine.GetTailUsSum();

      /* Burst taxonomy (Linux, 2026-07-26): splits the workers' wall time
       * into genuine CPU time vs stolen (preempted/runnable) time and
       * counts involuntary context switches. "stolen" rising while cpu%
       * stays flat = scheduling/IRQ/SMI interference; both rising = real
       * compute growth. Counters stay zero on non-Linux -> line hidden. */
      if (dt > 0.0) {
        const uint64_t dBurstWall
          = engine.GetBurstWallUsSum() - m_LastBurstWallUs;
        const uint64_t dBurstCpu
          = engine.GetBurstCpuUsSum() - m_LastBurstCpuUs;
        const uint64_t dBurstSw
          = engine.GetBurstNivcswSum() - m_LastBurstNivcsw;
        if (dBurstWall > 0)
          timing << StatRow(
            wxT("workers"),
            wxString::Format(
              _("cpu %.1f %% of wall"),
              100.0 * (double)dBurstCpu / (double)dBurstWall),
            wxString::Format(
              _("stolen %.0f ms/s   invol-sw %.0f /s"),
              (double)(dBurstWall > dBurstCpu ? dBurstWall - dBurstCpu : 0)
                / dt / 1000.0,
              (double)dBurstSw / dt));
        m_LastBurstWallUs = engine.GetBurstWallUsSum();
        m_LastBurstCpuUs = engine.GetBurstCpuUsSum();
        m_LastBurstNivcsw = engine.GetBurstNivcswSum();
        m_LastBurstCount = engine.GetBurstCount();
      }

      /* Callback interarrival jitter + audio-thread help autopsy + serial-mix
       * cost (2026-07-25). Cumulative since the last counter reset (organ
       * load / polyphony change) - a single report read is enough. The
       * autopsy answers the "callback never helps under Linux" mystery:
       * too-late = a worker finished the OutputTask first; lost-lock = the
       * callback lost the mutex race mid-mix. */
      {
        const uint64_t runs = GOSoundOutputTask::GetAudioRuns();
        const uint64_t mixed = GOSoundOutputTask::GetAudioMixed();
        const uint64_t mixCnt = GOSoundOutputTask::GetMixCount();
        const uint64_t mixSum = GOSoundOutputTask::GetMixUsSum();

        const uint64_t cbAll = engine.GetCbCount();
        const uint64_t cbLate = engine.GetCbLateCount();

        timing << StatRow(
          wxT("cb interval"),
          wxString::Format(
            _("%lld ... %lld us"),
            (long long)engine.GetCbInterMinUs(),
            (long long)engine.GetCbInterMaxUs()),
          wxString::Format(
            _("late >1.5x   %llu / %llu (%.0f %%)"),
            (unsigned long long)cbLate,
            (unsigned long long)cbAll,
            cbAll ? 100.0 * (double)cbLate / (double)cbAll : 0.0));
        timing << StatRow(
          wxT("out task"),
          wxString::Format(
            _("%llu runs, %llu mixed"),
            (unsigned long long)runs,
            (unsigned long long)mixed),
          wxString::Format(
            _("too-late %llu   lost-lock %llu"),
            (unsigned long long)GOSoundOutputTask::GetAudioDoneEarly(),
            (unsigned long long)GOSoundOutputTask::GetAudioLostLock()));
        timing << StatRow(
          wxT("serial mix"),
          wxString::Format(
            _("%.0f us avg / %lld us max"),
            mixCnt ? (double)mixSum / (double)mixCnt : 0.0,
            (long long)GOSoundOutputTask::GetMixUsMax()));
      }
      {
        const uint64_t dDec = v3.stDecodeCy - m_LastStDec;
        const uint64_t dFir = v3.stFirCy - m_LastStFir;
        const uint64_t dFad = v3.stFaderCy - m_LastStFad;
        const uint64_t dFus = v3.stFusedCy - m_LastStFus;
        const uint64_t dAdm = v3.stAdminCy - m_LastStAdm;
        const uint64_t dSum = dDec + dFir + dFad + dFus + dAdm;

        if (dSum > 0)
          timing << StatRow(
            wxT("V3 stages"),
            wxString::Format(
              _("decode %.0f · fused %.0f · FIR %.0f · fader %.0f · "
                "admin %.0f  %%"),
              100.0 * dDec / dSum,
              100.0 * dFus / dSum,
              100.0 * dFir / dSum,
              100.0 * dFad / dSum,
              100.0 * dAdm / dSum));
        m_LastStDec = v3.stDecodeCy;
        m_LastStFir = v3.stFirCy;
        m_LastStFad = v3.stFaderCy;
        m_LastStFus = v3.stFusedCy;
        m_LastStAdm = v3.stAdminCy;
      }
    m_LastV3DrainUs = v3.drainUsSum;
    m_LastV3CbRuns = v3.cbHelpRuns;
    m_LastV3CbDenied = v3.cbHelpDenied;
    m_LastV3ExecLate = v3.execLate;
    m_LastV3Rebuilds = v3.rebuilds;
    m_LastV3BatchCalls = v3.batchCalls;
    m_LastV3Solo = v3.soloRebuilds;
    m_LastV3BatchRendered = v3.batchRendered;
    m_LastV3SyncFallbacks = v3.syncFallbacks;
    m_LastV3ListSamples = v3.listSamples;
    m_LastV3ListVoices = v3.listVoicesSum;
    m_LastV3SampleMs = nowMs;
  }
  {
    const unsigned samplesPerBuffer = r_SoundSystem.GetSamplesPerBuffer();
    const unsigned sampleRate = r_SoundSystem.GetSampleRate();
    const int64_t periodUs = sampleRate
      ? (int64_t)samplesPerBuffer * 1000000 / (int64_t)sampleRate
      : 0;
    const auto pctOfPeriod = [periodUs](int64_t us) {
      return periodUs ? 100.0 * (double)us / (double)periodUs : 0.0;
    };

    colA << StatRule(
      wxT("TIMING"),
      periodUs ? wxString::Format(
                   _("period %lld us · buffer %u @ %u Hz"),
                   (long long)periodUs,
                   samplesPerBuffer,
                   sampleRate)
               : wxString(_("no device")));
    // Spitzenwerte zuerst - danach erst, wer sie verursacht haben koennte.
    colA << StatRow(
      wxT("worst"),
      wxString::Format(
        _("compute  %lld us (%.0f %%)"),
        (long long)m_WorstRenderUs,
        pctOfPeriod(m_WorstRenderUs)),
      wxString::Format(
        _("callback %lld us (%.0f %%)"),
        (long long)m_WorstCallbackUs,
        pctOfPeriod(m_WorstCallbackUs)));
    // Alles null ist der Normalfall - dann EINE Zeile statt fuenf Nullen.
    if (
      cb.driverReportedUnderruns || cb.callbackGapCount
      || cb.renderOverrunCount)
      colA << StatRow(
        wxT("underruns"),
        wxString::Format(
          _("driver %llu"), (unsigned long long)cb.driverReportedUnderruns),
        wxString::Format(
          _("gaps %llu (worst %lld us)   render overruns %llu"),
          (unsigned long long)cb.callbackGapCount,
          (long long)m_WorstGapUs,
          (unsigned long long)cb.renderOverrunCount));
    else
      colA << StatRow(wxT("underruns"), _("keine (driver · gaps · render)"));
    colA << timing;
    timing.Clear();
  }
  {
    static const wxChar *concealModes[] = {
      wxT("off"), wxT("PV 2k"), wxT("PV 4k"), wxT("PV 8k"), wxT("PV 1k")};
    // Was GEWAEHLT ist und was WIRKLICH laeuft, koennen auseinandergehen:
    // PV 1k braucht einen Puffer, der 256 teilt.  Wirkung anzeigen, nicht
    // Zustand - sonst steht "PV 1k" da und es laeuft 2k.
    const int concealMode = GOSoundConcealment::GetEffectiveMode();
    const int wantedMode = GOSoundConcealment::GetMode();
    const wxString modeLabel = concealMode == wantedMode
      ? wxString(concealModes[std::clamp(concealMode, 0, 4)])
      : wxString::Format(
          wxT("%s - %s not possible at this buffer size"),
          concealModes[std::clamp(concealMode, 0, 4)],
          concealModes[std::clamp(wantedMode, 0, 4)]);

    const unsigned long long bails = GOSoundConcealment::GetForcedCount();
    const unsigned long long cbPeriods = engine.GetCbCount();

    colB << StatRule(
      wxT("CONCEALMENT"),
      wxString::Format(
        _("%s · order %d · grace %d %% · anchor %s"),
        modeLabel,
        GOSoundConcealment::GetEffectiveOrder(),
        GOSoundConcealment::GetMixGracePct(),
        GOSoundConcealment::GetV0Hindcast() ? _("hindcast") : _("Burg")));
    colB << StatRow(
      wxT("bails"),
      wxString::Format(
        _("%llu (%.1f %% of periods)"),
        bails,
        cbPeriods ? 100.0 * (double)bails / (double)cbPeriods : 0.0),
      wxString::Format(
        _("late-real %llu   max wait %lld us"),
        (unsigned long long)GOSoundConcealment::GetLateRealCount(),
        (long long)GOSoundConcealment::GetMaxWaitUs()));
    colB << StatRow(
      wxT("shadow round"),
      wxString::Format(
        _("%lld us last / %lld us max"),
        (long long)GOSoundConcealment::GetShadowLastUs(),
        (long long)GOSoundConcealment::FetchShadowMaxUs()),
      wxString::Format(
        _("%llu ring overlaps"),
        (unsigned long long)GOSoundConcealment::GetStaleDiscards()));
    colB << StatRow(
      wxT("round starts"),
      wxString::Format(
        _("%llu busy-skipped"),
        (unsigned long long)GOSoundConcealment::GetShadowSkipCount()),
      wxString::Format(
        _("%llu caught up"),
        (unsigned long long)GOSoundConcealment::GetCatchupCount()));
    colB << StatRow(
      wxT("anchor lag"),
      wxString::Format(
        _("%llu late slices"),
        (unsigned long long)GOSoundConcealment::GetLateSlices()),
      wxString::Format(
        _("detector %llu firings"),
        (unsigned long long)GOSoundConcealment::GetOnsetCount()));

    // Spaete-Scheiben-Diagnose: WOHER kommen sie?  Die Hypothese steht im
    // Header von GOSoundConcealment (Bail->Pass presst zwei Oeffnungen auf
    // Reserve-Abstand zusammen, die zweite trifft "besetzt").  Bewiesen
    // waere sie, wenn (a) die Besetzt-Abstaende im untersten Bereich
    // klumpen, (b) die Besetzt-Faelle auf Bails folgen und (c) jede spaete
    // Scheibe binnen zwei Perioden nach einem Besetzt-Fall liegt.  Die
    // Statistik setzt sich bei Modus-/Ordnungswechsel zurueck - sie
    // beschreibt NUR die aktuell laufende Einstellung; die Gesamtzahl
    // late slices oben laeuft dagegen seit Start weiter.
    {
      const GOSoundConcealment::TimingStats ts
        = GOSoundConcealment::GetTimingStats();
      constexpr unsigned NB = GOSoundConcealment::TIMING_BUCKETS;

      // Alle vier Reihen auf DERSELBEN Topfleiter - so steht die
      // Rundenlaufzeit direkt unter dem Epilogabstand, und das
      // Kollisionskriterium (Runde > Abstand) ist ablesbar statt
      // nachzurechnen.
      wide << wxT("\n") << wxString::Format(
        wxT("    %-16s%7s%7s%7s%7s%7s%7s%7s\n"),
        _("histograms us"), wxT("<50"), wxT("<100"), wxT("<150"), wxT("<200"),
        wxT("<300"), wxT("<600"), _("more"));
      wide << StatHist(_("epilog gap"), ts.openGap, NB);
      wide << StatHist(_("  of which busy"), ts.skipGap, NB);
      wide << StatHist(_("round time"), ts.roundDur, NB);
      wide << StatHist(_("mix done, bail"), ts.compPostBail, NB);
      wide << StatHist(_("mix done, pass"), ts.compNormal, NB);
      wide << wxString::Format(
        _("    phase: %llu late slices (%llu after a busy case, largest "
          "%llu) · %llu busy after bail · %llu epilogs on audio thread\n"),
        (unsigned long long)ts.phaseLateSlices,
        (unsigned long long)ts.lateAfterSkip,
        (unsigned long long)ts.lateSliceMax,
        (unsigned long long)ts.skipAfterBail,
        (unsigned long long)ts.openByAudio);
    }

    // JE KANAL: wie dicht ist sein Spektrum besetzt, und wie oft hat er
    // gezuendet?  Der summierende Zaehler oben verbirgt genau das, und
    // eine Aufnahme kann es nicht zeigen - GOs Rekorder schreibt einen
    // Stereo-Downmix, und Summieren fuellt Baender, die im Einzelkanal
    // leer sind.  Ein duenn besetzter Kanal zuendet im Dauerlauf, weil
    // "Faktor zwei" in einem leeren Band geschenkt ist (gemessen: 86
    // belegte Baender 0 Zuendungen, 3 belegte 228).
    {
      wxString perCh;

      for (unsigned c = 0; c < 8; c++) {
        uint64_t on = 0;
        const unsigned occ = Xorcist_detectorPerChannel(c, &on);

        if (!occ && !on)
          continue;
        perCh += wxString::Format(
          wxT("%s%u:%u/%llu"),
          perCh.IsEmpty() ? wxT("") : wxT(" · "),
          c,
          occ,
          (unsigned long long)on);
      }
      if (!perCh.IsEmpty())
        colB << StatRow(
          _("detector/ch"),
          wxString::Format(_("occupied/256 : firings   %s"), perCh));
    }

    // Was die Umgebung UEBERSTEUERT, gehoert sichtbar in den Report - sonst
    // steht nach jedem A/B die Frage im Raum, ob die Variable ueberhaupt
    // angekommen ist.  Leer heisst: alles auf Vorgabe.
    {
      const char *ov = Xorcist_diagOverrides();

      if (ov && *ov)
        colB << StatRow(
          _("OVERRIDES"), wxString::FromUTF8(ov));
    }
  }
  colB << StatRule(wxT("CACHES · DEVICE"));
  colB << StatRow(
    wxT("xfade cache"),
    wxString::Format(
      _("%llu hits / %llu templates"),
      (unsigned long long)cacheStats.hitCount,
      (unsigned long long)cacheStats.templateCount),
    wxString::Format(
      _("misses %llu bg / %llu on audio thread"),
      (unsigned long long)cacheStats.missOnBackgroundThread,
      (unsigned long long)cacheStats.missOnAudioThread));
  if (precompute.everRun)
    colB << StatRow(
      wxT("xfade prewarm"),
      wxString::Format(
        _("%u / %u unique · %llu samples · %.1f ms"),
        precompute.uniqueLengths,
        precompute.requestedLengths,
        (unsigned long long)precompute.totalEntries,
        precompute.durationUs / 1000.0));
  else
    colB << StatRow(
      wxT("xfade prewarm"), _("not run (Linear mode or no organ loaded)"));
  // Nullgruppe: sechs Nullen sind sechs Nullen - eine Zeile reicht.
  if (
    dev.deviceLostEvents || dev.reconnectAttempts || dev.suspendEvents
    || dev.resumeEvents)
    colB << StatRow(
      wxT("device"),
      wxString::Format(
        _("%llu lost · %llu reconnects (%llu ok / %llu fail)"),
        (unsigned long long)dev.deviceLostEvents,
        (unsigned long long)dev.reconnectAttempts,
        (unsigned long long)dev.reconnectSuccesses,
        (unsigned long long)dev.reconnectFailures),
      wxString::Format(
        _("%llu suspend / %llu resume"),
        (unsigned long long)dev.suspendEvents,
        (unsigned long long)dev.resumeEvents));
  else
    colB << StatRow(wxT("device"), _("keine Ereignisse"));

  // Zusammenbau: die beiden Spalten nebeneinander, darunter die Tabellen
  // und der Ceiling-Sweep in voller Breite.
  text = StatColumns(colA, colB) << wide;

  // Ceiling-sweep results: kept in the counter label so they land in every
  // audio report (OnCreateReport snapshots this label).
  if (!m_CeilResults.empty())
    text << wxT("\nCeiling benchmark (sustained clean polyphony):\n")
         << m_CeilResults;

  m_CounterLabel->SetLabel(text);
  /* Groesse des Textes SELBST ausrechnen statt GetBestSize zu fragen: mit
   * wxST_NO_AUTORESIZE fuehrt das Label seine beste Groesse nicht mehr
   * nach.  Bei dicktengleicher Schrift ist die Rechnung exakt - Zeilen
   * mal Zeilenhoehe, laengste Zeile mal Zeichenbreite.
   *
   * Nur bei AENDERUNG anwenden: SetMinSize entwertet die beste Groesse
   * die Kette hinauf, und ein Relayout je 333-ms-Tick wuerde GTKs
   * Tooltip-Timer wieder dauerhaft zuruecksetzen (siehe
   * RefreshOrganAvailability). */
  {
    size_t lines = 1, longest = 0, cur = 0;

    for (const wxUniChar c : text)
      if (c == wxT('\n')) {
        lines++;
        longest = std::max(longest, cur);
        cur = 0;
      } else
        cur++;
    longest = std::max(longest, cur);

    const wxSize want(
      (int)longest * m_CounterLabel->GetCharWidth() + 8,
      (int)lines * m_CounterLabel->GetCharHeight() + 4);

    if (want != m_CounterTextSize) {
      m_CounterTextSize = want;
      m_CounterLabel->SetMinSize(want);
      // Layout() auf dem Rollfenster statt auf dem Dialog: nur so bleibt
      // der 333-ms-Takt frei von einem Gesamt-Relayout.
      m_CounterScroll->Layout();
      m_CounterScroll->FitInside();
      FitCounterPane();
    }
  }
  /* Release the height reserved at construction time (see there). It only
   * existed so the initial fit would not be computed against an empty
   * label; now that the label has real content its own best size is
   * correct, and holding the estimate any longer would leave a band of
   * empty space below the text. The window keeps the size it opened with,
   * so the freed height goes to the graph - the sizer's only stretchable
   * item. */
  if (!m_CounterHeightReleased) {
    m_CounterHeightReleased = true;
    m_CounterLabel->SetMinSize(wxSize(wxDefaultCoord, wxDefaultCoord));
  }
  Layout();
}

void GOAudioMonitorDlg::OnTimer(wxTimerEvent &WXUNUSED(event)) { UpdateNow(); }

void GOAudioMonitorDlg::OnToggleSeries(wxCommandEvent &event) {
  switch (event.GetId()) {
  case ID_SHOW_POLYPHONY:
    m_Graph->SetSeriesVisible(
      GOAudioMonitorGraph::SERIES_POLYPHONY, event.IsChecked());
    break;
  case ID_SHOW_RENDER_TIME:
    m_Graph->SetSeriesVisible(
      GOAudioMonitorGraph::SERIES_COMPUTE_US, event.IsChecked());
    break;
  case ID_SHOW_CALLBACK_TIME:
    m_Graph->SetSeriesVisible(
      GOAudioMonitorGraph::SERIES_CALLBACK_US, event.IsChecked());
    break;
  case ID_SHOW_CALLBACK_GAP:
    m_Graph->SetSeriesVisible(
      GOAudioMonitorGraph::SERIES_GAP_US, event.IsChecked());
    break;
  case ID_SHOW_CB_SHARE:
    m_Graph->SetSeriesVisible(
      GOAudioMonitorGraph::SERIES_CB_SHARE, event.IsChecked());
    break;
  }
}

void GOAudioMonitorDlg::OnCreateReport(wxCommandEvent &WXUNUSED(event)) {
  // Snapshot everything at button-press time, before the file dialog opens
  // and the timer continues scrolling the graph behind it.
  const wxString snapshotTime
    = wxDateTime::Now().Format(wxT("%Y-%m-%d %H:%M:%S"));
  const wxString csv = m_Graph->GetCSV();
  const wxBitmap graphBmp = m_Graph->RenderToBitmap(960, 400);
  const wxString counterText = m_CounterLabel->GetLabel();

  // --- Build summary.txt ---
  GOSoundOrganEngine &engine = r_SoundSystem.GetEngine();
  const unsigned sampleRate = r_SoundSystem.GetSampleRate();
  const unsigned samplesPerBuffer = r_SoundSystem.GetSamplesPerBuffer();

  wxString summary;
  summary += wxString::Format(
    wxT("GrandOrgue Audio Glitch Report\nGenerated: %s\n\n"), snapshotTime);

  summary += wxT("=== Configuration ===\n");
  summary += wxString::Format(wxT("GO Version: %s\n"), wxT(APP_VERSION));
  /* WHICH organ produced these numbers. Two reports are only comparable if
   * they ran the same sample set: pipe count, sample lengths, channel counts
   * and residual byte widths all differ per set and drive the DRAM traffic
   * the ceiling is bound by. The ODF path + its mtime also pin down WHICH
   * VERSION of a set it was (they get edited/regenerated).
   * p_OrganModel is a GOOrganController (GOAppWindow passes the controller
   * through the model getter); the cast just reaches the ODF fields. */
  if (p_OrganModel) {
    summary += wxString::Format(
      wxT("Organ: %s\n"), p_OrganModel->GetOrganName());

    GOOrganController *const ctrl
      = dynamic_cast<GOOrganController *>(p_OrganModel);

    if (ctrl) {
      const wxString odf = ctrl->GetODFFilename();

      summary += wxString::Format(wxT("ODF: %s"), ctrl->GetOrganPathInfo());

      const wxFileName odfFile(odf);

      if (odfFile.FileExists())
        summary += wxString::Format(
          wxT(" (modified %s)"),
          odfFile.GetModificationTime().Format(wxT("%Y-%m-%d %H:%M")));
      summary += wxT("\n");
    }
  }
  summary += wxString::Format(
    wxT("Crossfade mode: %s\n"),
    CrossfadeModeName(GOAudioParams::GetCrossfadeMode()));
  /* WHICH build produced this report - with several deployed exe variants
   * (A/B experiments), reports from different exes are otherwise
   * indistinguishable (cost guesswork three times, 2026-07-15/-16/-17). */
  summary += wxString::Format(
    wxT("Executable: %s\n"), wxStandardPaths::Get().GetExecutablePath());
  summary += wxString::Format(
    wxT("Polyphony limit: %u\n"), engine.GetHardPolyphony());
  summary += wxString::Format(
    wxT("Worker threads: %u\n"), engine.GetWorkerThreadCount());
  summary += wxString::Format(
    wxT("Interpolation: %s\n"),
    engine.GetInterpolationType()
        == GOSoundResample::GO_POLYPHASE_INTERPOLATION
      ? wxT("polyphase")
      : wxT("linear"));
  summary += wxString::Format(
    wxT("Prefetch distance: %d\n"), engine.GetPrefetchDistance());
  summary += wxString::Format(
    wxT("Release align mode: %s\n"),
    GOAudioParams::GetReleaseAlignMode() == GOReleaseAlignMode::Correlation
      ? wxT("correlation")
      : wxT("legacy"));
  /* Address probes (2026-07-16): the 24k ceiling is decided PER PROCESS
   * START and never changes while running - some per-start allocation
   * lottery. These three addresses let reports correlate good/bad runs:
   *   image  - binary base (constant 0x14000... = no-ASLR patch works;
   *            varying = Windows overrides the header bit)
   *   heap   - engine object (heap randomization, independent of image)
   *   mmap   - virtual base of the 46GB sample cache mapping
   * L2/L3 are PHYSICALLY indexed; physical page assignment happens once
   * at load (touchMemory) and is invisible from user space - if all three
   * probes stay constant across good AND bad runs, the lottery is physical. */
  {
    static int s_imageProbe; // lives in the image -> tracks the image base
    const GOOrganController *const pCtrl
      = dynamic_cast<GOOrganController *>(p_OrganModel);
    const void *cacheStart = nullptr;
    if (pCtrl)
      cacheStart
        = const_cast<GOOrganController *>(pCtrl)->GetMemoryPool().GetCacheStart();
    summary += wxString::Format(
      wxT("Address probes: image %p, heap %p, cache mmap %p\n"),
      (void *)&s_imageProbe,
      (void *)&engine,
      cacheStart);
    /* Arena probes (2026-07-17): samplers and voice blocks live in
     * contiguous line-aligned arenas with a set-coprime slot stride -
     * bases must end in 0x00 (64-aligned) and slot line counts must be
     * odd, on EVERY start; slots are in bytes. */
    const GOSoundSamplerPool &pool = engine.GetSamplerPool();
    summary += wxString::Format(
      wxT("Arena probes: samplers %p slot %u, voice blocks %p slot %u, "
          "large pages %u/%u"),
      (const void *)pool.GetSamplerArenaBase(),
      (unsigned)GOSoundSamplerPool::GetSamplerSlotSize(),
      (const void *)pool.GetVoiceBlockArenaBase(),
      (unsigned)pool.GetVoiceBlockSlotSize(),
      pool.GetLargePageChunkCount(),
      pool.GetArenaChunkCount());
    if (pool.GetLargePageFailError() != 0)
      summary += wxString::Format(
        wxT(" (fallback err %u%s)"),
        pool.GetLargePageFailError(),
        pool.GetLargePageFailError() == 1314 ? wxT(" = privilege not held")
                                             : wxT(""));
    summary += wxT("\n");
#ifdef __linux__
    /* Ground truth for 2MB pages on Linux: the chunk counter above only
     * says the MADV_HUGEPAGE advice was accepted; whether THP actually
     * backed the pool (GOMemoryPool) and the arenas with huge pages is
     * what the kernel reports here. */
    {
      std::ifstream rollup("/proc/self/smaps_rollup");
      std::string line, anonHuge;

      while (std::getline(rollup, line))
        if (line.compare(0, 14, "AnonHugePages:") == 0) {
          anonHuge = line.substr(14);
          anonHuge.erase(0, anonHuge.find_first_not_of(' '));
          break;
        }

      std::ifstream thp("/sys/kernel/mm/transparent_hugepage/enabled");
      std::string thpMode;

      std::getline(thp, thpMode);
      summary += wxString::Format(
        wxT("Huge pages (THP): AnonHugePages %s, mode: %s\n"),
        anonHuge.empty() ? wxT("unavailable")
                         : wxString::FromUTF8(anonHuge.c_str()),
        thpMode.empty() ? wxT("unavailable")
                        : wxString::FromUTF8(thpMode.c_str()));
    }
#endif
    /* Load/fault probes (2026-07-17, executable-alternation effect): the
     * cache phase runs seconds when the 46GB file is warm in the OS
     * standby cache and minutes from disk, and the fault counter scales
     * with how much had to be (re)mapped - together they show per start
     * whether this process inherited a warm or a wrecked file cache. */
    if (pCtrl)
      summary += wxString::Format(
        wxT("Load timings: cache %.1f s, total %.1f s\n"),
        pCtrl->GetCacheLoadMs() / 1000.0,
        pCtrl->GetTotalLoadMs() / 1000.0);
#ifdef _WIN32
    {
      PROCESS_MEMORY_COUNTERS pmc;
      if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        summary += wxString::Format(
          wxT("Process memory: page faults %lu, working set %llu MB ")
          wxT("(peak %llu MB)\n"),
          (unsigned long)pmc.PageFaultCount,
          (unsigned long long)(pmc.WorkingSetSize >> 20),
          (unsigned long long)(pmc.PeakWorkingSetSize >> 20));
    }
#endif
  }
  /* Which V3 predictor actually produced the loaded cache. Without this a
   * report cannot be compared against another build at all: a different
   * order means a different residual stream, hence different cache size and
   * DRAM traffic - which is what the ceiling is bound by. (Cost us an
   * afternoon of guessing on 2026-07-15.) */
  summary += wxString::Format(
    wxT("V3 sample compression: %s"),
    r_SoundSystem.GetSettings().UseV3SampleCompression() ? wxT("on")
                                                         : wxT("off"));
  // Mode identity for measurement reports: Variable (0) is not an order.
  if (GOSoundAudioSection::IsVariablePredictorOrder())
    summary += wxT(", predictor: variable (per section)");
  else
    summary += wxString::Format(
      wxT(", predictor: fixed order %u"),
      (unsigned)GOSoundAudioSection::GetPredictorMode());
  {
    /* Compression ratio of the loaded organ (raw PCM vs. stored bytes,
     * aggregated over all live sections - see RegisterStorageStats). The
     * user had to derive this by hand from cache file sizes before;
     * measurement rule: every compared figure belongs in the report. */
    uint64_t raw, stored;
    GOSoundAudioSection::GetCompressionTotals(raw, stored);
    if (stored)
      summary += wxString::Format(
        wxT(", ratio %.3f (%.2f -> %.2f GB)"),
        (double)raw / (double)stored,
        (double)raw / 1073741824.0,
        (double)stored / 1073741824.0);
  }
  /* The section cache version identifies the codec generation that produced
   * the loaded cache: a cache whose version differs from this build's is
   * rejected and rebuilt (GOSoundAudioSection::LoadCache), so this constant
   * IS the loaded cache's version - no need to read it back per section. */
  summary += wxString::Format(
    wxT(", cache v%u\n"), (unsigned)GO_SOUND_AUDIO_SECTION_CACHE_VERSION);

  // Audio thread scheduling info (detected on the first callback).
  const int schedInfo = engine.GetAudioThreadSchedInfo();
  if (schedInfo < 0) {
    summary
      += wxT("Audio thread priority: not yet detected (no audio playing?)\n");
  } else {
#ifdef __linux__
    if (schedInfo == SCHED_FIFO)
      summary += wxT("Audio thread priority: SCHED_FIFO (RT) ✓\n");
    else if (schedInfo == SCHED_RR)
      summary += wxT("Audio thread priority: SCHED_RR (RT) ✓\n");
    else
      summary += wxString::Format(
        wxT("Audio thread priority: normal (sched policy %d) — SCHED_FIFO "
            "elevation failed; grant rtprio (/etc/security/limits.d) or run "
            "via a realtime JACK server\n"),
        schedInfo);
#elif defined(_WIN32)
    // WASAPI/ASIO set HIGHEST (2) via MMCSS "Pro Audio" or explicit
    // SetThreadPriority - that is effectively RT on Windows and is normal.
    // DirectSound/WMME set TIME_CRITICAL (15). Both are fine; NORMAL (0)
    // or below would indicate the backend did not elevate the thread.
    if (schedInfo >= THREAD_PRIORITY_HIGHEST)
      summary += wxString::Format(
        wxT("Audio thread priority: %d (%s) ✓\n"),
        schedInfo,
        schedInfo >= THREAD_PRIORITY_TIME_CRITICAL
          ? wxT("TIME_CRITICAL")
          : wxT("HIGHEST - normal for WASAPI/ASIO"));
    else
      summary += wxString::Format(
        wxT("Audio thread priority: %d (NORMAL or below - may cause "
            "glitches; expected HIGHEST or TIME_CRITICAL from the backend)\n"),
        schedInfo);
#else
    summary += wxString::Format(
      wxT("Audio thread priority: %d (platform-specific value)\n"), schedInfo);
#endif
  }

  // Worker thread RT elevation (attempted at thread start via
  // MMCSS/QoS/SCHED_FIFO).
  {
    using ES = GOSoundThread::ElevationStatus;
    const ES es = engine.GetWorkerElevationStatus();
    wxString workerLine = wxT("Worker thread elevation: ");
    switch (es) {
    case ES::Pending:
      workerLine += wxT("not yet known (threads not started?)\n");
      break;
    case ES::MmcssSucceeded:
      workerLine += wxT("MMCSS 'Pro Audio' ✓\n");
      break;
    case ES::FallbackHighest:
      workerLine += wxT("MMCSS unavailable — THREAD_PRIORITY_HIGHEST set\n");
      break;
    case ES::QosSucceeded:
      workerLine += wxT("QOS_CLASS_USER_INTERACTIVE ✓\n");
      break;
    case ES::SchedFifoSucceeded:
      workerLine += wxT("SCHED_FIFO priority 60 ✓\n");
      break;
    case ES::NotElevated:
      workerLine += wxT("not elevated (permission denied or unsupported) "
                        "— grant rtprio or run as admin\n");
      break;
    }
    summary += workerLine;

    // Census over ALL workers - the line above samples only worker 0 and
    // hides partial MMCSS exhaustion (the suspected carrier of the binary
    // FAST/SLOW process-start lottery; see CountWorkerElevations).
    {
      unsigned nMmcss = 0, nFallback = 0, nOther = 0;

      engine.CountWorkerElevations(nMmcss, nFallback, nOther);
      if (nMmcss + nFallback + nOther > 0)
        summary += wxString::Format(
          wxT("Worker elevation census: %u MMCSS / %u fallback / %u other "
              "of %u workers%s\n"),
          nMmcss,
          nFallback,
          nOther,
          nMmcss + nFallback + nOther,
          nFallback + nOther ? wxT(" ⚠ PARTIAL - suspect for SLOW state")
                             : wxT(""));

      // Raw GetThreadPriority() range across ALL workers: if min != max the
      // pool did not all land at the same priority.
      int minP = 0, maxP = 0;

      if (engine.GetWorkerPriorityRange(minP, maxP))
        summary += wxString::Format(
          wxT("Worker priority range: %d..%d%s\n"),
          minP,
          maxP,
          minP != maxP ? wxT(" ⚠ NON-UNIFORM") : wxT(""));
    }

    // Active worker-affinity mode - two checkboxes write this shared value
    // (old 'reserve audio core' = 1, new 'Pin 1:1' = 2), so make the winner
    // visible. With mode 2 the per-worker CPUs below should read 0..19 and
    // migration should fall (cumulative counter still averages the pre-pin
    // period in, so restart with it on for a clean 0).
    summary += wxString::Format(
      wxT("Worker affinity mode: %d (0=free, 1=off-core0, 2=pin1:1)\n"),
      GOSoundThread::GetAffinityMode());

    // Denormal mode the worker INHERITED from the OS (before GO forces
    // FTZ/DAZ): shows whether this platform already flushed denormals on
    // worker threads or ran them the slow way. -ffast-math only set the
    // main thread; workers inherit differently per platform.
    {
      const int csr = engine.GetWorkerInheritedCsr();
      if (csr < 0)
        summary += wxT("Worker denormals (inherited): not captured yet\n");
      else
        summary += wxString::Format(
          wxT("Worker denormals (inherited): FTZ=%s DAZ=%s (MXCSR 0x%04x)\n"),
          ((unsigned)csr & GO_MXCSR_FTZ) ? wxT("on") : wxT("OFF"),
          ((unsigned)csr & GO_MXCSR_DAZ) ? wxT("on") : wxT("OFF"),
          (unsigned)csr);
    }

#ifdef _WIN32
    // Whether MMCSS's "Pro Audio" class actually lands a worker at the same
    // GetThreadPriority() reading as the audio backend's own callback
    // thread (m_AudioThreadSchedInfo below) was never previously verified -
    // "the same class" was only a code comment, not a measurement.
    const int workerPrio = engine.GetWorkerThreadPriority();
    if (workerPrio > -1000)
      summary += wxString::Format(
        wxT("Worker thread GetThreadPriority(): %d\n"), workerPrio);
#endif
  }

  /* CPU placement (2026-07-17): passive observation for the per-start
   * lottery hunt - the arena fixed the virtual cache geometry yet starts
   * still spread ~6%, so the remaining candidates are physical page
   * colors and scheduler placement. Per thread: last-seen logical CPU
   * (placement snapshot; on the usual Intel numbering CPUs 2i/2i+1 share
   * physical core i) and migrations/samples (does it wander?). Workers
   * sample per wakeup, the audio thread per period - counters stay 0/0
   * until audio has run. */
  {
    const GOCpuPlacementProbe &audioProbe = engine.GetAudioCpuProbe();

    summary += wxString::Format(
      wxT("CPU placement: audio %d (migr %u/%u); workers cpu(migr/samples):"),
      audioProbe.GetLastCpu(),
      audioProbe.GetMigrations(),
      audioProbe.GetSamples());
    for (unsigned i = 0; i < engine.GetWorkerThreadCount(); i++) {
      const GOCpuPlacementProbe &p = engine.GetWorkerCpuProbe(i);

      summary += wxString::Format(
        wxT(" %d(%u/%u)"), p.GetLastCpu(), p.GetMigrations(), p.GetSamples());
    }
    summary += wxT("\n");
  }

  summary += wxString::Format(wxT("Sample rate: %u Hz\n"), sampleRate);
  summary += wxString::Format(wxT("Buffer size: %u samples"), samplesPerBuffer);
  if (sampleRate > 0)
    summary += wxString::Format(
      wxT(" (%.1f ms)"), 1000.0 * samplesPerBuffer / sampleRate);
  summary += wxT("\n");

  const std::vector<GOAudioDeviceConfig> &devCfgs
    = r_SoundSystem.GetSettings().GetAudioDeviceConfig();
  if (!devCfgs.empty()) {
    summary += wxT("Audio devices:\n");
    for (unsigned i = 0; i < (unsigned)devCfgs.size(); ++i) {
      const auto &d = devCfgs[i];
      wxString devLine
        = wxString::Format(wxT("  [%u] %s"), i, d.GetLogicalName());
      if (!d.GetPortName().IsEmpty())
        devLine += wxString::Format(wxT(" (%s"), d.GetPortName());
      if (!d.GetApiName().IsEmpty())
        devLine += wxString::Format(wxT(" / %s"), d.GetApiName());
      if (!d.GetPortName().IsEmpty())
        devLine += wxT(")");
      if (
        !d.GetPhysicalName().IsEmpty()
        && d.GetPhysicalName() != d.GetLogicalName())
        devLine += wxString::Format(wxT(" -> %s"), d.GetPhysicalName());
      devLine += wxString::Format(
        wxT(" [desired latency: %u ms]\n"), d.GetDesiredLatency());
      summary += devLine;
    }
  }

  summary += wxT("\n=== Counters at snapshot time ===\n");
  summary += counterText;
  summary += wxT("\n");

  // File dialog opens after the snapshot is complete.
  const wxString defaultName = wxString::Format(
    wxT("AudioGlitchReport_%s.zip"),
    wxDateTime::Now().Format(wxT("%Y-%m-%d_%H-%M")));

  /* Open in the last-used report directory so consecutive reports land
   * next to each other. Persisted via wxConfig (registry/file), because
   * a session-static resets on every restart - and during test sessions
   * GrandOrgue is restarted for every binary swap, which sent the dialog
   * back to Documents each time. */
  const wxString lastReportDir
    = wxConfigBase::Get()->Read(wxT("AudioMonitor/LastReportDir"), wxT(""));

  wxFileDialog dlg(
    this,
    _("Save Audio Glitch Report"),
    lastReportDir,
    defaultName,
    _("ZIP archives (*.zip)|*.zip"),
    wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

  if (dlg.ShowModal() != wxID_OK)
    return;
  wxConfigBase::Get()->Write(
    wxT("AudioMonitor/LastReportDir"), wxFileName(dlg.GetPath()).GetPath());
  wxConfigBase::Get()->Flush();

  // --- Write ZIP ---
  wxFFileOutputStream fileOut(dlg.GetPath());
  if (!fileOut.IsOk()) {
    wxMessageBox(
      _("Could not open file for writing."),
      _("Error"),
      wxOK | wxICON_ERROR,
      this);
    return;
  }

  wxZipOutputStream zip(fileOut);

  auto addText = [&](const wxString &name, const wxString &content) {
    zip.PutNextEntry(name);
    const wxScopedCharBuffer utf8 = content.utf8_str();
    zip.Write(utf8.data(), utf8.length());
  };

  addText(wxT("summary.txt"), summary);
  addText(wxT("samples.csv"), csv);
  addText(wxT("sysinfo.txt"), BuildSysInfo());

  {
    wxMemoryOutputStream pngStream;
    if (graphBmp.ConvertToImage().SaveFile(pngStream, wxBITMAP_TYPE_PNG)) {
      zip.PutNextEntry(wxT("graph.png"));
      wxStreamBuffer *buf = pngStream.GetOutputStreamBuffer();
      zip.Write(buf->GetBufferStart(), buf->GetIntPosition());
    }
  }

  zip.Close();
  fileOut.Close();

  wxMessageBox(
    wxString::Format(_("Report saved to:\n%s"), dlg.GetPath()),
    _("Report Saved"),
    wxOK | wxICON_INFORMATION,
    this);
}

void GOAudioMonitorDlg::OnCacheBins(wxCommandEvent &WXUNUSED(event)) {
  const unsigned base = GOSoundSubblockBins::GetBase();
  const unsigned nSteps = GOSoundSubblockBins::GetStepCount();
  wxString text;

  if (base == 0 || nSteps == 0) {
    wxMessageBox(
      _("The V3 render-subblock cache is disabled - no bin ladder is set "
        "up. Enable \"Use V3 subblock cache\" in the options and reload "
        "the organ."),
      _("Cache Bins"),
      wxOK | wxICON_INFORMATION,
      this);
    return;
  }

  // Classify the loaded set's V3 sections on demand - no load-time
  // bookkeeping to go stale (the engine, and with it the ladder, is set
  // up AFTER the organ finished loading, so any counting done during the
  // load would be reset or use the previous organ's ladder).
  std::vector<unsigned> counts(nSteps, 0);
  unsigned optOut = 0;
  unsigned total = 0;
  GOOrganModel *pModel = m_GetOrganModel();

  if (pModel) {
    const auto classify = [&](const GOSoundAudioSection *pSection) {
      if (
        !pSection
        || pSection->GetCompressionCodec()
          != GOSoundAudioSectionCompressionCodec::V3)
        return;

      unsigned rung = 0;

      total++;
      if (GOSoundSubblockBins::ClassifySectionRate(
            pSection->GetSampleRate(), &rung))
        counts[rung]++;
      else
        optOut++;
    };

    for (GOCacheObject *obj : pModel->GetCacheObjects()) {
      const GOSoundingPipe *pipe = obj->AsSoundingPipe();

      if (!pipe)
        continue;
      for (unsigned i = 0; i < pipe->GetAttackCount(); i++)
        classify(pipe->GetAttackSection(i));
      for (unsigned i = 0; i < pipe->GetReleaseCount(); i++)
        classify(pipe->GetReleaseSection(i));
    }
  }

  unsigned maxCount = 0;
  unsigned usedRungs = 0;

  for (const unsigned c : counts) {
    if (c > 0)
      usedRungs++;
    if (c > maxCount)
      maxCount = c;
  }

  text << wxString::Format(
    _("Bin ladder: base %u samples, top rung %u, %u rungs (2%% steps)\n"
      "Loaded V3 sections: %u, occupying %u rung(s)%s\n\n"),
    base,
    GOSoundSubblockBins::GetMaxBin(),
    nSteps,
    total,
    usedRungs,
    !pModel ? _(" - no organ loaded")
      : total == 0
      ? _(" - no V3-compressed sections (was the organ loaded with V3 "
          "compression enabled?)")
      : wxT(""));
  // Only occupied rungs are listed - a typical set uses very few distinct
  // source sample rates, so the full ~70-rung ladder would be noise.
  for (unsigned i = 0; i < nSteps; i++) {
    const unsigned c = counts[i];

    if (c == 0)
      continue;

    const unsigned barLen
      = maxCount ? std::max(1u, (c * 30u) / maxCount) : 0;

    text << wxString::Format(
      wxT("bin %5u (rung %3u): %6u  %s\n"),
      GOSoundSubblockBins::GetStep(i),
      i,
      c,
      wxString(wxT('#'), barLen));
  }
  if (optOut)
    text << wxString::Format(
      _("\nopt-out (> top rung): %u section(s) - these voices always use "
        "the direct V3 path\n"),
      optOut);

  wxDialog dlg(
    this,
    wxID_ANY,
    _("Cache Bins"),
    wxDefaultPosition,
    wxDefaultSize,
    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
  wxTextCtrl *textCtrl = new wxTextCtrl(
    &dlg,
    wxID_ANY,
    text,
    wxDefaultPosition,
    wxSize(560, 400),
    wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);

  textCtrl->SetFont(
    wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
  sizer->Add(textCtrl, 1, wxEXPAND | wxALL, 10);
  sizer->Add(dlg.CreateButtonSizer(wxOK), 0, wxEXPAND | wxALL, 10);
  dlg.SetSizerAndFit(sizer);
  dlg.ShowModal();
}

void GOAudioMonitorDlg::OnCbHelpChanged(wxCommandEvent &event) {
  const int sel = event.GetSelection();

  r_SoundSystem.GetEngine().SetCallbackHelpPolicy(sel);
  wxLogMessage("Callback help policy -> %d", sel);
}

void GOAudioMonitorDlg::OnPrefetchChanged(wxSpinEvent &event) {
  const int val = event.GetValue();
  r_SoundSystem.GetEngine().SetPrefetchDistance(val);
  wxLogMessage("Prefetch distance -> %d", val);
}

void GOAudioMonitorDlg::OnStaggerChanged(wxSpinEvent &event) {
  const int val = event.GetValue();
  GOSoundThread::SetStaggerDelayUs(val);
  wxLogMessage("Worker stagger -> %d us", val);
}

void GOAudioMonitorDlg::OnConcealModeChanged(wxCommandEvent &event) {
  const int mode = event.GetSelection();

  GOSoundConcealment::SetMode(mode);
  wxLogMessage(
    "Concealment mode -> %d (0=off, 1=2k, 2=4k, 3=8k, 4=1k)", mode);
}

void GOAudioMonitorDlg::OnMixGraceChanged(wxSpinEvent &event) {
  const int pct = event.GetValue();

  GOSoundConcealment::SetMixGracePct(pct);
  wxLogMessage(
    "Concealment mix grace -> %d %% (100 = legacy blocking delivery)", pct);
}

void GOAudioMonitorDlg::OnFreqInertiaChanged(wxSpinEvent &event) {
  const float v = event.GetValue() / 100.0f;

  GOSoundConcealment::SetFreqDamping(v);
  wxLogMessage("Concealment frequency damping -> %d %%", event.GetValue());
}

void GOAudioMonitorDlg::OnStragglerTruthToggled(wxCommandEvent &event) {
  GOSoundConcealment::SetStragglerTruth(event.IsChecked());
  wxLogMessage(
    "Straggler truth -> %s", event.IsChecked() ? "ON (late mix feeds history)"
                                               : "off (mix abandoned)");
}

void GOAudioMonitorDlg::OnPronyOrderChanged(wxSpinEvent &event) {
  const int p = event.GetValue();

  GOSoundConcealment::SetOrder(p);
  wxLogMessage(
    "Concealment continuation -> order %d (%s)", p,
    p <= 1 ? "rotator" : "per-bin Prony");
}

void GOAudioMonitorDlg::OnSeamChanged(wxCommandEvent &event) {
  static const char *names[] = {"adaptive", "anchor", "bridge"};
  const int v = event.GetSelection();

  GOSoundConcealment::SetSeam(v);
  wxLogMessage("Concealment seam -> %s", names[std::clamp(v, 0, 2)]);
}

void GOAudioMonitorDlg::OnHybridToggled(wxCommandEvent &event) {
  GOSoundConcealment::SetHybrid(event.IsChecked());
  wxLogMessage(
    "Concealment hybrid 1k -> %s", event.IsChecked() ? "ON" : "off");
}

void GOAudioMonitorDlg::OnHybHandChanged(wxSpinEvent &event) {
  GOSoundConcealment::SetHybHandOrder(event.GetValue());
  wxLogMessage(
    "Concealment hybrid hand-back -> %d rasters", event.GetValue());
}

void GOAudioMonitorDlg::OnHybCapChanged(wxSpinEvent &event) {
  GOSoundConcealment::SetHybShortCap(event.GetValue());
  wxLogMessage("Concealment hybrid 1k order -> %d", event.GetValue());
}

void GOAudioMonitorDlg::OnHybAllBandsToggled(wxCommandEvent &event) {
  GOSoundConcealment::SetHybAllBands(event.IsChecked());
  wxLogMessage(
    "Concealment hybrid all-bands -> %s",
    event.IsChecked() ? "ON" : "off (rising only)");
}

void GOAudioMonitorDlg::OnSafetyCapChanged(wxSpinDoubleEvent &event) {
  GOSoundConcealment::SetSafetyCap((float)event.GetValue());
  wxLogMessage("Concealment safety cap -> %.2f", event.GetValue());
}

void GOAudioMonitorDlg::OnAnchorTauChanged(wxSpinEvent &event) {
  GOSoundConcealment::SetAnchorTau(event.GetValue());
  wxLogMessage("Concealment anchor tau -> %d samples", event.GetValue());
}

void GOAudioMonitorDlg::OnRootClampChanged(wxSpinDoubleEvent &event) {
  GOSoundConcealment::SetRootClamp((float)event.GetValue());
  wxLogMessage("Concealment root clamp -> %.2f", event.GetValue());
}

void GOAudioMonitorDlg::OnResGateChanged(wxSpinDoubleEvent &event) {
  const double v = event.GetValue();

  GOSoundConcealment::SetDetResGate((float)v);
  wxLogMessage("Detector residual gate -> %.3f (0 = off)", v);
}

void GOAudioMonitorDlg::OnReentryFadeChanged(wxSpinEvent &event) {
  const int smp = event.GetValue();

  GOSoundConcealment::SetReentryFade(smp);
  wxLogMessage(
    "Concealment re-entry fade -> %d samples (0 = auto, 3 ms capped at "
    "one period)",
    smp);
}

void GOAudioMonitorDlg::OnV0HindcastToggle(wxCommandEvent &event) {
  GOSoundConcealment::SetV0Hindcast(event.IsChecked());
  wxLogMessage(
    "Seam anchor v0 -> %s",
    event.IsChecked() ? "hindcast (shadow[-1] vs last real sample)"
                      : "Burg one-step prediction");
}

void GOAudioMonitorDlg::OnDetectRhoChanged(wxSpinDoubleEvent &event) {
  GOSoundConcealment::SetDetectRho((float)event.GetValue());
  wxLogMessage("Concealment detector rho -> %.2f", event.GetValue());
}

void GOAudioMonitorDlg::OnNoTrendToggled(wxCommandEvent &event) {
  GOSoundConcealment::SetNoTrend(event.IsChecked());
  wxLogMessage(
    "Concealment level trend -> %s",
    event.IsChecked() ? "FROZEN (no trend)" : "continued");
}

void GOAudioMonitorDlg::OnXrunPatternChanged(wxCommandEvent &event) {
  static const char *names[] = {"single", "x2 pair", "1:1 alternate"};
  const int v = event.GetSelection();

  GOSoundConcealment::SetMissPattern(v);
  wxLogMessage("Forced-xrun pattern -> %s", names[v >= 0 && v < 3 ? v : 0]);
}

void GOAudioMonitorDlg::OnXrunTestChanged(wxSpinEvent &event) {
  const int val = event.GetValue();

  GOSoundConcealment::SetForcedXrunsPerMinute(val);
  wxLogMessage(
    "Forced-xrun concealment test -> %d/min (concealed so far: %llu)",
    val,
    (unsigned long long)GOSoundConcealment::GetForcedCount());
}

void GOAudioMonitorDlg::OnPinWorkersChanged(wxCommandEvent &event) {
  const int mode = event.IsChecked() ? 2 : 0;
  GOSoundThread::SetAffinityMode(mode);
  wxLogMessage("Worker affinity mode -> %d", mode);
}

void GOAudioMonitorDlg::OnDecodeReadAheadChanged(wxCommandEvent &event) {
  const bool enable = event.IsChecked();
  GOSoundStream::SetDecodeReadAhead(enable);
  wxLogMessage("Decode read-ahead -> %s", enable ? "on" : "off");
}

void GOAudioMonitorDlg::OnLatencyProbe(wxCommandEvent &WXUNUSED(event)) {
  const GOOrganController *const pCtrl
    = dynamic_cast<GOOrganController *>(p_OrganModel);

  if (!pCtrl) {
    wxLogMessage("Latency probe: no organ loaded");
    return;
  }

  GOMemoryPool &pool
    = const_cast<GOOrganController *>(pCtrl)->GetMemoryPool();
  const unsigned char *const base
    = (const unsigned char *)pool.GetCacheStart();
  const size_t size = pool.GetMappedSize();

  if (!base || size < (size_t)64 * 1024 * 1024) {
    wxLogMessage(
      "Latency probe: no mapped cache >=64MB to chase (base %p, size %zu) - "
      "compressed cache must be memory-mapped",
      (const void *)base,
      size);
    if (m_LatencyLabel)
      m_LatencyLabel->SetLabel(_("latency: n/a"));
    return;
  }

  // Data-dependent pointer chase: the next line to touch is derived from the
  // 64-bit word just loaded, so each load must complete before the next
  // address is known - this defeats the prefetchers and out-of-order engine
  // and exposes the true load-to-use latency. Over a >L3 span it lands in
  // DRAM, so the measured latency is the physical-placement latency of THIS
  // process's cache mapping (incl. TLB walks - part of the real cost).
  const size_t nLines = size / 64;
  size_t pow2 = 1;

  while (pow2 * 2 <= nLines)
    pow2 *= 2;

  const size_t mask = pow2 - 1;
  const unsigned N = 1u << 18; // 262144 dependent loads (~a few ms .. 50 ms)
  uint64_t sink = 0;
  double minTicks = 1e18; // preemption-immune: fastest uninterrupted chunk
  double meanTicks = 0.0; // includes preempted chunks (shows contamination)

#if defined(__x86_64__) || defined(__i386__)
  // Chunked measurement: time blocks of CHUNK dependent loads separately.
  // A normal-priority probe under full RT render load gets descheduled mid
  // loop, and rdtsc counts that wall-clock gap - so the WHOLE-loop average
  // is mostly preemption, not memory latency (the confound the review
  // flagged). Fix: the MINIMUM per-access time over many chunks is the one
  // chunk that ran uninterrupted -> the true load-to-use latency under the
  // current memory load, immune to scheduling (preemption only ADDS time).
  // Tiny chunks (~16 loads ≈ a few µs at loaded latency) so the MIN window
  // is too short for a DPC/ISR/context-switch to slip in - plus a
  // TIME_CRITICAL boost so a normal-priority GUI thread is not starved by
  // the RT workers for the duration. The two together make the MIN a
  // genuine load-to-use latency, not a scheduling artifact.
  const unsigned CHUNK = 16;
  const unsigned nChunks = N / CHUNK;
  uint64_t state = 88172645463325252ull; // nonzero seed
  double sumTicks = 0.0;

#ifdef _WIN32
  const HANDLE hProbeThread = GetCurrentThread();
  const int oldProbePrio = GetThreadPriority(hProbeThread);

  SetThreadPriority(hProbeThread, THREAD_PRIORITY_TIME_CRITICAL);
#elif defined(__linux__)
  // The GUI thread is SCHED_OTHER; under the SCHED_FIFO-60 render workers it
  // only gets CPU in the inter-period gaps (memory subsystem idle) - it would
  // measure IDLE latency, not loaded. Windows has no such starvation because
  // MMCSS reserves ~20% for non-RT. So raise the probe to SCHED_FIFO 65 -
  // above the workers (60), below the audio callback (70, see GOSoundThread) -
  // for the measurement, so it samples memory WHILE the render loads it while
  // never preempting the callback. Needs rtprio (the host's @audio group has
  // it); on failure we proceed at normal priority (MIN over tiny chunks stays
  // preemption-immune, just more samples land in idle gaps).
  const pthread_t probeThread = pthread_self();
  int oldPolicy = SCHED_OTHER;
  struct sched_param oldParam = {};
  pthread_getschedparam(probeThread, &oldPolicy, &oldParam);
  struct sched_param boostParam = {};
  boostParam.sched_priority = 65;
  const bool probeBoosted
    = pthread_setschedparam(probeThread, SCHED_FIFO, &boostParam) == 0;
#endif

  for (unsigned c = 0; c < nChunks; c++) {
    _mm_lfence();
    const uint64_t c0 = __rdtsc();
    _mm_lfence();

    for (unsigned i = 0; i < CHUNK; i++) {
      const size_t line = state & mask;
      const uint64_t v = *(const volatile uint64_t *)(base + line * 64);

      sink += v;
      // Depends on v (must wait for the load); the LCG term guarantees
      // progress even where v == 0 (silent/zero cache regions).
      state = v ^ (state * 6364136223846793005ull + 1442695040888963407ull);
    }

    _mm_lfence();
    const double per = (double)(__rdtsc() - c0) / CHUNK;

    if (per < minTicks)
      minTicks = per;
    sumTicks += per;
  }
  meanTicks = sumTicks / nChunks;
#ifdef _WIN32
  SetThreadPriority(hProbeThread, oldProbePrio);
#elif defined(__linux__)
  if (probeBoosted)
    pthread_setschedparam(probeThread, oldPolicy, &oldParam);
#endif
#endif

  // TSC nominal 3.7 GHz on this i9-10900K (report shows it); ns = ticks/3.7.
  const double minNs = minTicks / 3.7;
  const double meanNs = meanTicks / 3.7;

  wxLogMessage(
    "Latency probe: MIN %.1f ns/access (preemption-immune) | mean %.1f ns "
    "(incl. preemption) | %u loads over %.1f GB cache, sink=%llu",
    minNs,
    meanNs,
    N,
    (double)size / 1e9,
    (unsigned long long)sink);
  if (m_LatencyLabel)
    m_LatencyLabel->SetLabel(wxString::Format(_("latency: %.1f ns"), minNs));
}

void GOAudioMonitorDlg::OnCloseButton(wxCommandEvent &WXUNUSED(event)) {
  Close();
}

void GOAudioMonitorDlg::OnCloseWindow(wxCloseEvent &WXUNUSED(event)) {
  /* User-initiated close: forget the "reopen on next start" flag (see
   * GOAppWindow::ShowAudioMonitor). An app shutdown destroys this child
   * frame without a close event, deliberately leaving the flag set. */
  r_SoundSystem.GetSettings().AudioMonitorOpen(false);
  m_Timer.Stop();
  StopBenchmark();
  ResetForciblyEngagedStops();
  Destroy();
}

/* Re-flow the legend to the current width. wxStaticText::Wrap() edits the
 * label in place, so the unwrapped original has to be restored first -
 * otherwise every resize would wrap the already-wrapped text and the line
 * breaks would accumulate. The width comparison also keeps this from
 * re-entering: Layout() below triggers another size event. */
/* Der Textbereich nimmt nur, was er braucht - der Rest gehoert dem
 * Graphen (Nutzerwunsch 12.08.: ein halbleeres Textfeld neben einem
 * gequetschten Graphen sieht falsch aus).  Nur SCHRUMPFEN: braucht der
 * Text mehr Platz, als der Trenner hergibt, bleibt die eingestellte
 * Position stehen und der Rollbalken uebernimmt - so bleibt das Ziehen
 * des Trenners die letzte Instanz. */
void GOAudioMonitorDlg::FitCounterPane() {
  if (!m_Split || !m_Split->IsSplit())
    return;

  const int total = m_Split->GetClientSize().y - m_Split->GetSashSize();
  const int want = m_CounterTextSize.y + 6;
  const int bottom = total - m_Split->GetSashPosition();

  if (total < 160 || bottom <= want)
    return;

  int pos = total - want;

  if (pos > total - 40) // ein Rest Textfeld bleibt immer sichtbar
    pos = total - 40;
  if (pos < 60) // ... und ein Rest Graph auch
    pos = 60;
  if (pos != m_Split->GetSashPosition())
    m_Split->SetSashPosition(pos);
}

void GOAudioMonitorDlg::OnDialogResize(wxSizeEvent &event) {
  event.Skip(); // let the sizer do its normal work
  FitCounterPane();

  if (!m_Legend)
    return;

  const int available = GetClientSize().x - 20; // the sizer's wxALL border

  if (available < 100 || available == m_LegendWrapWidth)
    return;
  m_LegendWrapWidth = available;
  m_Legend->SetLabel(m_LegendText);
  m_Legend->Wrap(available);
  Layout();
}

void GOAudioMonitorDlg::OnCharHook(wxKeyEvent &e) {
  wxWindow *pMainAppWin = wxTheApp->GetTopWindow();
  bool isProcessed = false;

  if (pMainAppWin) {
    // Copy the event: SetEventObject() below must not mutate the original e,
    // because e is still routed further via e.Skip() to this window's own
    // controls (e.g. a slider taking a keyboard arrow key) when the key is
    // not a recognized shortcut.
    wxKeyEvent eCopied(e);

    eCopied.SetEventObject(pMainAppWin);
    isProcessed = pMainAppWin->ProcessWindowEvent(eCopied);
  }
  if (!isProcessed)
    e.Skip();
}

void GOAudioMonitorDlg::RefreshOrganAvailability() {
  GOOrganModel *const current = m_GetOrganModel ? m_GetOrganModel() : nullptr;

  if (current != p_OrganModel) {
    // Never call anything on the *old* p_OrganModel here - if it changed
    // because the organ is closing, AbortPlayback() already ran (safely,
    // while it was still valid) and set p_OrganModel to nullptr itself; by
    // the time this poll notices the difference, the old pointer's target
    // may already be fully destroyed. Only ever touch `current`, the
    // freshly-fetched, presumed-valid value.
    //
    // Same rule for the held-pipe handles and the pipe pool: they refer to
    // the OLD organ. Normally AbortPlayback already dropped them; clearing
    // again here (without stopping) covers any path that got us a new
    // model without that hook.
    ClearHeldPipes(false);
    m_PipePool.clear();
    StopBenchmark();
    m_BenchmarkActive->SetValue(false);
    ResetForciblyEngagedStops();
    p_OrganModel = current;
    if (p_OrganModel)
      p_OrganModel->RegisterLifecycleListener(this);

    // Organ changed: treat it as a fresh measurement window - the
    // "worst since ..." figures of the previous organ (or of the empty
    // startup phase) would otherwise stick around and pollute the first
    // report of the new organ.
    m_WorstRenderUs = 0;
    m_WorstCallbackUs = 0;
    m_WorstGapUs = 0;
    if (m_HeldPipesLabel && m_HeldPipesSlider)
      m_HeldPipesLabel->SetLabel(
        wxString::Format(wxT("0 / %d"), m_HeldPipesSlider->GetValue()));
  }

  const unsigned totalKeys = GetTotalAccessibleKeys();

  if (totalKeys != m_LastKnownTotalKeys) {
    m_LastKnownTotalKeys = totalKeys;
    m_HeldSlider->SetRange(0, totalKeys ? (int)totalKeys : 1);
    if (m_HeldSlider->GetValue() > (int)totalKeys)
      m_HeldSlider->SetValue(totalKeys);
    m_HeldLabel->SetLabel(
      wxString::Format(_("%d / %u"), m_HeldSlider->GetValue(), totalKeys));
  }

  const bool haveOrgan = totalKeys > 0;

  m_AllStopsBtn->Enable(haveOrgan);
  m_BenchmarkActive->Enable(haveOrgan);
  m_HeldSlider->Enable(haveOrgan);
  // Enabled state set at construction time is stale for a monitor that was
  // auto-reopened at startup, BEFORE any organ was loaded - this refresh
  // is what brings the controls to life after the load.
  if (m_HeldPipesSlider)
    m_HeldPipesSlider->Enable(haveOrgan);
  if (m_HeldPipesSpin)
    m_HeldPipesSpin->Enable(haveOrgan);
  // Same auto-reopen staleness for the ceiling-sweep checkbox. Stays
  // enabled while a sweep runs even though the organ vanishes during the
  // sweep's own codec reloads - unchecking is the user's abort handle
  // (CeilingRequestStop), and it must not be taken away mid-sweep.
  if (m_CeilCheck)
    m_CeilCheck->Enable(
      m_SetBufferSize && m_SetV3AndReload
      && (haveOrgan || m_CeilPhase != CeilPhase::Idle));
  /* NUR bei Aenderung setzen.  Der unbedingte Aufruf hier war die Ursache
   * dafuer, dass in diesem Dialog NIE ein Tooltip erschien (Nutzerbefund
   * 12.08.): wxGTKs SetToolTip landet in gtk_widget_set_tooltip_text, das
   * intern gtk_widget_trigger_tooltip_query ausloest und damit den
   * Tooltip-Zustand der ganzen Anzeige zurueckstellt - alle 333 ms, waehrend
   * GTK rund 500 ms Verweilzeit braucht.  Kein Regler kam je zum Zug. */
  const int benchTip = haveOrgan ? 1 : 0;

  if (benchTip != m_BenchTipState) {
    m_BenchTipState = benchTip;
    m_BenchmarkActive->SetToolTip(BenchmarkActiveTip(haveOrgan));
  }
}

unsigned GOAudioMonitorDlg::GetTotalAccessibleKeys() const {
  unsigned total = 0;

  if (p_OrganModel)
    for (unsigned i = 0; i < p_OrganModel->GetManualAndPedalCount(); i++) {
      GOManual *manual = p_OrganModel->GetManual(i);

      if (manual)
        total += manual->GetNumberOfAccessibleKeys();
    }
  return total;
}

// Stops/couplers are engaged via SetButtonState() - the same path a click on
// the console control takes - so the benchmark behaves like a real organist
// pulling the drawknobs, not like a combination system injecting a hidden
// named state. Consequences, both intentional: the user can still operate
// every engaged control manually while the benchmark runs (a named
// combination state would lock SetButtonState() out until retracted), and a
// drawstop currently held by a crescendo or other named combination state is
// simply skipped (SetButtonState() refuses while CalculateResultState(false)
// is true), exactly as a manual click would be.

void GOAudioMonitorDlg::OnAllStops(wxCommandEvent &WXUNUSED(event)) {
  if (m_AllStopsLatched) {
    ResetForciblyEngagedStops();
    return;
  }
  if (!p_OrganModel) {
    wxLogMessage("Enable All Stops & Couplers: no organ loaded, nothing to do");
    return;
  }

  unsigned manualsSeen = 0, stopsSeen = 0, stopsForced = 0, stopsStillOff = 0,
           couplersSeen = 0, couplersForced = 0, couplersStillOff = 0,
           switchesSeen = 0, switchesForced = 0;

  // Organ-wide switches first: many AND/OR-computed stops and couplers (see
  // the still-off diagnostics below) are read-only drawstops whose engaged
  // state is calculated from these switches (GODrawstop::m_ControllingDrawstops,
  // wired from GOOrganModel::GetSwitch() in GODrawstop::Load()). Engaging the
  // switches here makes GO's own Update()/SetResultState() cascade recompute
  // and engage any dependent stop/coupler automatically - no need to walk the
  // AND/OR graph ourselves.
  for (unsigned i = 0; i < p_OrganModel->GetSwitchCount(); i++) {
    GOSwitch *sw = p_OrganModel->GetSwitch(i);

    if (!sw)
      continue;
    switchesSeen++;
    if (sw->IsEngaged())
      continue;
    sw->SetButtonState(true);
    if (sw->IsEngaged()) {
      m_ForciblyEngaged.push_back(sw);
      switchesForced++;
    }
  }

  for (unsigned i = 0; i < p_OrganModel->GetManualAndPedalCount(); i++) {
    GOManual *manual = p_OrganModel->GetManual(i);

    if (!manual)
      continue;
    manualsSeen++;
    for (unsigned j = 0; j < manual->GetStopCount(); j++) {
      GOStop *stop = manual->GetStop(j);

      if (!stop)
        continue;
      stopsSeen++;
      if (stop->IsEngaged())
        continue;
      stop->SetButtonState(true);
      if (stop->IsEngaged()) {
        m_ForciblyEngaged.push_back(stop);
        stopsForced++;
      } else {
        // Two legitimate reasons a click-like SetButtonState() refuses:
        // IsReadOnly() - any FunctionType other than FUNCTION_INPUT makes
        // the engaged state a *computed* result of other switches - or an
        // active named combination state (crescendo etc.) currently holding
        // the drawstop, which a real click couldn't override either.
        stopsStillOff++;
        if (stopsStillOff <= 8)
          wxLogWarning(
            "Stop %s/%s still off: IsReadOnly=%d FunctionType=%d",
            manual->GetName().c_str(),
            stop->GetName().c_str(),
            (int)stop->IsReadOnly(),
            (int)stop->GetFunctionType());
      }
    }
    // Only the ODF-defined couplers, not GO's own auto-generated ones
    // (generic Sub/Super octave etc.) appended after them in m_couplers -
    // those aren't part of the organ's own registration.
    for (unsigned j = 0; j < manual->GetODFCouplerCount(); j++) {
      GOCoupler *coupler = manual->GetCoupler(j);

      if (!coupler)
        continue;
      couplersSeen++;
      if (coupler->IsEngaged())
        continue;
      coupler->SetButtonState(true);
      if (coupler->IsEngaged()) {
        m_ForciblyEngaged.push_back(coupler);
        couplersForced++;
      } else {
        couplersStillOff++;
        if (couplersStillOff <= 8)
          wxLogWarning(
            "Coupler %s/%s still off: IsReadOnly=%d FunctionType=%d",
            manual->GetName().c_str(),
            coupler->GetName().c_str(),
            (int)coupler->IsReadOnly(),
            (int)coupler->GetFunctionType());
      }
    }
  }

  m_AllStopsLatched = !m_ForciblyEngaged.empty();
  m_AllStopsBtn->SetLabel(
    m_AllStopsLatched ? _("Restore Previous Registration")
                       : _("Enable All Stops && Couplers"));

  wxLogMessage(
    "Enable All Stops & Couplers: switches %u/%u forced, %u manuals, stops "
    "%u/%u forced (%u still off), couplers %u/%u forced (%u still off)",
    switchesForced,
    switchesSeen,
    manualsSeen,
    stopsForced,
    stopsSeen,
    stopsStillOff,
    couplersForced,
    couplersSeen,
    couplersStillOff);
}

GOAudioMonitorDlg *GOAudioMonitorDlg::s_Instance = nullptr;

void GOAudioMonitorDlg::ResetAnyForcedRegistration() {
  if (s_Instance && s_Instance->m_AllStopsLatched) {
    s_Instance->ResetForciblyEngagedStops();
    wxLogMessage(
      _("Benchmark registration retracted before saving the organ settings "
        "(forced stops/switches must never be persisted)."));
  }
}

void GOAudioMonitorDlg::ResetForciblyEngagedStops() {
  // Retract like an organist pushing the drawknobs back in. If the user
  // manually released one of these during the benchmark, this is a no-op
  // for it; if a crescendo has grabbed it in the meantime, SetButtonState()
  // refuses - just like a real click - and it stays under the crescendo's
  // control.
  for (GODrawstop *drawstop : m_ForciblyEngaged)
    drawstop->SetButtonState(false);
  m_ForciblyEngaged.clear();
  m_AllStopsLatched = false;
  if (m_AllStopsBtn)
    m_AllStopsBtn->SetLabel(_("Enable All Stops && Couplers"));
}

bool GOAudioMonitorDlg::PickRandomKey(NoteKey &out) {
  if (!p_OrganModel)
    return false;

  const unsigned manualCount = p_OrganModel->GetManualAndPedalCount();

  if (!manualCount)
    return false;

  // A handful of attempts is enough in practice (real organs rarely have
  // more than one or two manuals with zero accessible keys, e.g. an unused
  // division), and avoids an unbounded loop if every manual is empty.
  for (int attempt = 0; attempt < 8; attempt++) {
    const unsigned manualIndex = (unsigned)std::rand() % manualCount;
    GOManual *manual = p_OrganModel->GetManual(manualIndex);

    if (!manual)
      continue;

    const unsigned nKeys = manual->GetNumberOfAccessibleKeys();

    if (!nKeys)
      continue;

    out.manualIndex = manualIndex;
    out.midiNote = manual->GetFirstAccessibleKeyMIDINoteNumber()
      + (unsigned)std::rand() % nKeys;
    return true;
  }
  return false;
}

bool GOAudioMonitorDlg::IsHeld(const NoteKey &note) const {
  return std::find(m_HeldNotes.begin(), m_HeldNotes.end(), note)
    != m_HeldNotes.end();
}

bool GOAudioMonitorDlg::IsPendingRelease(const NoteKey &note) const {
  for (const PendingNoteAction &pending : m_PendingActions)
    if (pending.action == PendingNoteAction::RELEASE && pending.note == note)
      return true;
  return false;
}

bool GOAudioMonitorDlg::AddOneHeldNote() {
  if (!p_OrganModel)
    return false;

  // A bounded number of attempts guards against an organ too small/occupied
  // for the requested target (the held slider's max is set to
  // GetTotalAccessibleKeys(), but stay defensive regardless).
  for (int attempt = 0; attempt < 50; attempt++) {
    NoteKey note;

    if (!PickRandomKey(note))
      return false;
    if (IsHeld(note) || IsPendingRelease(note))
      continue;

    GOManual *manual = p_OrganModel->GetManual(note.manualIndex);

    if (!manual)
      continue;
    manual->SetMidiNoteState(note.midiNote, 0x7f);
    m_HeldNotes.push_back(note);
    return true;
  }
  return false;
}

void GOAudioMonitorDlg::AdjustHeldNotes() {
  if (!p_OrganModel)
    return;

  const unsigned target = (unsigned)m_HeldSlider->GetValue();

  // Release the excess (most recently added first - order doesn't matter,
  // it's a simulated chord, not a specific voicing).
  while (m_HeldNotes.size() > target) {
    const NoteKey note = m_HeldNotes.back();

    m_HeldNotes.pop_back();

    GOManual *manual = p_OrganModel->GetManual(note.manualIndex);

    if (manual)
      manual->SetMidiNoteState(note.midiNote, 0);
  }

  while (m_HeldNotes.size() < target)
    if (!AddOneHeldNote())
      break;
}

void GOAudioMonitorDlg::SyncHeldPipesTarget(int target, bool fromSpin) {
  if (fromSpin && m_HeldPipesSlider)
    m_HeldPipesSlider->SetValue(target);
  else if (!fromSpin && m_HeldPipesSpin)
    m_HeldPipesSpin->SetValue(target);
  if (m_HeldPipesLabel)
    m_HeldPipesLabel->SetLabel(
      wxString::Format(wxT("%zu / %d"), m_HeldPipes.size(), target));
}

void GOAudioMonitorDlg::OnHeldPipesSliderChanged(wxCommandEvent &) {
  if (m_HeldPipesSlider)
    SyncHeldPipesTarget(m_HeldPipesSlider->GetValue(), false);
}

void GOAudioMonitorDlg::OnHeldPipesSpin(wxSpinEvent &) {
  if (m_HeldPipesSpin)
    SyncHeldPipesTarget(m_HeldPipesSpin->GetValue(), true);
}

void GOAudioMonitorDlg::AdjustHeldPipes() {
  const unsigned target
    = (m_HeldPipesSlider && m_BenchmarkActive && m_BenchmarkActive->GetValue())
    ? (unsigned)m_HeldPipesSlider->GetValue()
    : 0; // benchmark off (wind-down): ramp everything out

  if (!p_OrganModel) {
    ClearHeldPipes(false);
    m_PipePool.clear();
    return;
  }
  if (m_HeldPipes.size() == target)
    return;

  /* Ramp bound per 10ms tick, both directions (25/tick = 2.5k voices/s; a
   * full 30k ramp takes ~12s). Down: stopping everything at once would BE
   * the release-all stress test rather than a controlled ramp. The bound
   * was 150/tick, but the START WORK of such a burst (pool fetch+memset,
   * attack selection, section touches - all on the GUI thread) audibly
   * disturbed the audio pipeline once per tick on a machine running near
   * budget: a dropout train in the tick grid, quantized to the 2.67ms
   * period grid, is the "metronome with pitch" alternating between 375/3
   * and 375/4 Hz - a perfect fourth. Start-onset jitter did NOT remove
   * that tone (measured 2026-07-11): it tracks the burst's CPU cost, not
   * the voices' sound. */
  constexpr unsigned MAX_STEP_PER_TICK = 25;

  if (m_HeldPipes.size() < target) {
    if (m_PipePool.empty()) {
      for (GOCacheObject *obj : p_OrganModel->GetCacheObjects()) {
        GOSoundingPipe *pipe = obj->AsSoundingPipe();

        // One-shot pipes end on their own - their handle would stay in
        // m_HeldPipes while the voice is long gone, silently inflating
        // the slider's "held" count against the real polyphony.
        //
        // Action-noise pipes are not speaking stops: drawstop/coupler/
        // tremulant noises are modelled as ordinary non-percussive pipes
        // (near-silent sustain, the release IS the "clack") - a wind-down
        // of held pipes played a chorus of those clacks (sounded like
        // crackling; 2026-07-22). Two exclusion criteria, both verified
        // on the benchmark organs (demo + Rotterdam):
        //  - single-pipe ranks: all drawstop/coupler noises (demo 29,
        //    Rotterdam 101) have exactly 1 pipe; real stops have >= 25.
        //  - rank name contains "noise": catches the per-KEY noise ranks
        //    (Rotterdam: 6x "... keys noise" with 32-58 pipes) that pass
        //    the pipe-count test; no real rank carries "noise" in either
        //    set. (Displayed=N is NOT usable instead: the demo hides all
        //    its real stops too. A MIDI-key test neither: noise pipes
        //    declare MIDIKeyNumber.)
        GORank *pRank = pipe ? pipe->GetRank() : nullptr;

        if (
          pipe && !pipe->IsOneshot() && pRank && pRank->GetPipeCount() > 1
          && !pRank->GetName().Lower().Contains(wxT("noise")))
          m_PipePool.push_back(pipe);
      }
      if (m_PipePool.empty())
        return; // nothing sounding in this organ
    }

    /* Starting a mix from zero: re-seed, so the same slider value always
     * holds the same pipes and a measurement is reproducible - see
     * HELD_PIPES_RNG_SEED. Only on the 0 -> N edge; re-seeding on every tick
     * would make each tick redraw the same first pipes. */
    if (m_HeldPipes.empty())
      std::srand(HELD_PIPES_RNG_SEED);

    unsigned toStart
      = std::min((unsigned)(target - m_HeldPipes.size()), MAX_STEP_PER_TICK);

    while (toStart > 0) {
      GOSoundingPipe *pipe = m_PipePool[std::rand() % m_PipePool.size()];
      /* Random start delay: a full-speed ramp starts 150 voices per 10ms
       * tick; without jitter every batch of onsets lands in the same
       * audio callback, and impulse bursts quantized to the period grid
       * are an audible pulse train ("metronome click with pitch" -
       * alternating between 375/3 = 125Hz and 375/4 = 94Hz as the tick
       * phase drifts against the 2.67ms periods). Smearing the starts
       * over ~50ms keeps each render period down to a handful of onsets
       * without meaningfully slowing the ramp. */
      GOSoundSampler *pSampler
        = pipe->StartHeldSample((unsigned)(std::rand() % 48));

      if (!pSampler)
        break; // engine refused (polyphony limit?) - retry next tick
      m_HeldPipes.emplace_back(pipe, pSampler);
      toStart--;
    }
  } else {
    // Stopping only marks the sampler (the release work spreads inside
    // the engine) - 4x the start step keeps a 20k wind-down at ~2s.
    unsigned toStop = std::min(
      (unsigned)(m_HeldPipes.size() - target), 4 * MAX_STEP_PER_TICK);

    while (toStop > 0) {
      m_HeldPipes.back().first->StopHeldSample(m_HeldPipes.back().second);
      m_HeldPipes.pop_back();
      toStop--;
    }
  }
  if (m_HeldPipesLabel)
    m_HeldPipesLabel->SetLabel(
      wxString::Format(wxT("%zu / %u"), m_HeldPipes.size(), target));
}

void GOAudioMonitorDlg::ClearHeldPipes(bool stopVoices) {
  if (stopVoices)
    for (auto &held : m_HeldPipes)
      held.first->StopHeldSample(held.second);
  m_HeldPipes.clear();
  if (m_HeldPipesLabel && m_HeldPipesSlider)
    m_HeldPipesLabel->SetLabel(
      wxString::Format(wxT("0 / %d"), m_HeldPipesSlider->GetValue()));
}

void GOAudioMonitorDlg::StopBenchmark() {
  m_BenchmarkTimer.Stop();
  // The held-pipe VOICES are released, but the slider keeps its position:
  // re-enabling the benchmark ramps straight back to the previous target
  // (repeatable A/B runs without re-dialing).
  ClearHeldPipes(p_OrganModel != nullptr);

  if (p_OrganModel)
    for (const NoteKey &note : m_HeldNotes) {
      GOManual *manual = p_OrganModel->GetManual(note.manualIndex);

      if (manual)
        manual->SetMidiNoteState(note.midiNote, 0);
    }
  m_HeldNotes.clear();

  if (p_OrganModel)
    for (const PendingNoteAction &pending : m_PendingActions) {
      // REATTACK entries belong to notes that were already released as part
      // of the collision blip in OnBenchmarkTimer(); nothing to undo for
      // those. RELEASE entries are still-sounding transient attacks.
      if (pending.action == PendingNoteAction::RELEASE) {
        GOManual *manual = p_OrganModel->GetManual(pending.note.manualIndex);

        if (manual)
          manual->SetMidiNoteState(pending.note.midiNote, 0);
      }
    }
  m_PendingActions.clear();
}

// ===== "Find ceiling" sweep ==================================================
// The controller runs on the 333ms UpdateNow() poll. All health inputs are
// the CUMULATIVE realtime-safe counters, so a window is judged purely by
// deltas against a baseline taken at its start - no interference with the
// GetAndReset* consumers above.

namespace {
// Observation window: a target only counts as clean after this many polls
// (~12s) without a single bad event.
constexpr int CEIL_EVAL_POLLS = 36;
// Extra polls at target before the window starts (voice starts decayed).
constexpr int CEIL_SETTLE_EXTRA_POLLS = 6;
// Give the ramp up to ~50s to reach the target (2.5k voices/s up).
constexpr int CEIL_SETTLE_TIMEOUT_POLLS = 150;
// Waiting for sound/organ after a buffer switch resp. codec reload.
constexpr int CEIL_READY_TIMEOUT_POLLS = 3600; // 20 min (organ loads)
// Fast-forward: while the compute peak stays below this fraction of the
// period budget after CEIL_FAST_POLLS, the ramp does not sit out the full
// window - it leaps ahead proportionally (compute scales ~linearly with
// voices). The strict full window applies only near the edge and during
// bisection, where the actual number is decided.
constexpr int CEIL_FAST_POLLS = 3;         // ~1s per fast probe
// Edge-law margin (user 2026-07-12): grazing just under the compute curve
// is not headroom. A poll proves headroom only when the callback peak
// stayed below 95% of the compute peak, and a window is clean only when
// more than ~10% of its polls show such headroom - hugging the curve 90%
// of the time is saturated.
constexpr int CEIL_HEADROOM_NUM = 19; // callback < compute * 19/20 (95%)
constexpr int CEIL_HEADROOM_DEN = 20;
// The held-pipes slider tops out at 30000; stay below it.
constexpr unsigned CEIL_TARGET_CAP = 29500;
constexpr unsigned CEIL_START_TARGET = 2000;
// Polyphony limit while sweeping - must exceed any reachable ceiling, or
// the limiter's drops falsify the measurement.
constexpr unsigned CEIL_POLYPHONY_LIMIT = 32000;
// The sweep seeds the same RNG the manual slider does - see
// HELD_PIPES_RNG_SEED (AdjustHeldPipes re-seeds on every 0 -> N edge, which
// covers the sweep's ramps too; this seeding stays for the case where a cell
// starts with pipes already held).

wxString ceilCellName(bool v3, unsigned buffer) {
  return wxString::Format(wxT("%s @ %u"), v3 ? wxT("V3") : wxT("legacy"), buffer);
}
} // namespace

void GOAudioMonitorDlg::CeilingStatus(const wxString &text) {
  if (m_CeilStatus && m_CeilStatus->GetLabel() != text)
    m_CeilStatus->SetLabel(text);
}

void GOAudioMonitorDlg::CeilingSetTarget(unsigned target) {
  m_CeilTarget = target;
  m_CeilRetriedTarget = false;
  if (m_HeldPipesSlider)
    m_HeldPipesSlider->SetValue((int)target);
  m_CeilPhase = CeilPhase::Settle;
  m_CeilPollsLeft = CEIL_SETTLE_TIMEOUT_POLLS;
  CeilingStatus(wxString::Format(
    _("%s: probing %u (clean %u / dirty %s)"),
    ceilCellName(
      m_CeilCells[m_CeilCellIdx].v3, m_CeilCells[m_CeilCellIdx].buffer),
    target,
    m_CeilLo,
    m_CeilHi ? wxString::Format(wxT("%u"), m_CeilHi) : wxString(_("none"))));
}

void GOAudioMonitorDlg::CeilingCellDone(unsigned result) {
  const CeilCell &cell = m_CeilCells[m_CeilCellIdx];
  const wxString line = wxString::Format(
    wxT("  %-14s %u voices%s\n"),
    ceilCellName(cell.v3, cell.buffer) + wxT(":"),
    result,
    result >= CEIL_TARGET_CAP ? wxT(" (slider cap reached)") : wxT(""));

  m_CeilResults += line;
  wxLogMessage(
    wxT("Ceiling benchmark: %s"), wxString(line).Trim());
  m_CeilCellIdx++;
  m_CeilPhase
    = m_CeilCellIdx >= m_CeilCells.size() ? CeilPhase::Restore : CeilPhase::ApplyCell;
}

void GOAudioMonitorDlg::CeilingStart() {
  if (
    !m_SetBufferSize || !m_SetV3AndReload || !p_OrganModel
    || !GetTotalAccessibleKeys()) {
    if (m_CeilCheck)
      m_CeilCheck->SetValue(false);
    return;
  }

  GOConfig &config = r_SoundSystem.GetSettings();

  m_CeilOrigV3 = config.UseV3SampleCompression();
  m_CeilOrigCache = config.UseV3SubblockCache();
  m_CeilOrigBuffer = config.SamplesPerBuffer();
  m_CeilSavedPolyphonyLimit = config.PolyphonyLimit();
  config.PolyphonyLimit(CEIL_POLYPHONY_LIMIT);
  r_SoundSystem.GetEngine().SetHardPolyphony(CEIL_POLYPHONY_LIMIT);

  // The user's codec first: its three buffer sizes need no organ reload,
  // so results start appearing quickly; one reload to the other codec, one
  // back in the Restore phase.
  m_CeilCells.clear();
  for (int pass = 0; pass < 2; pass++) {
    const bool v3 = pass == 0 ? m_CeilOrigV3 : !m_CeilOrigV3;

    for (unsigned buffer : {128u, 256u, 512u})
      m_CeilCells.push_back({v3, buffer});
  }
  m_CeilCellIdx = 0;
  m_CeilResults.Clear();

  // Objective conditions: pure sustained voices, no transient attacks, no
  // held keys - the same protocol on every machine.
  if (m_RateSlider) {
    m_RateSlider->SetValue(0);
    m_RateLabel->SetLabel(wxString::Format(_("%d/min"), 0));
  }
  if (m_HeldSlider)
    m_HeldSlider->SetValue(0);

  m_CeilRestoreKicked = false;
  m_CeilPhase = CeilPhase::ApplyCell;
  CeilingStatus(_("starting sweep..."));
}

void GOAudioMonitorDlg::CeilingRequestStop() {
  if (m_CeilPhase == CeilPhase::Idle || m_CeilPhase == CeilPhase::Done)
    return;
  m_CeilPhase = CeilPhase::Restore;
  CeilingStatus(_("aborting - restoring your settings..."));
}

void GOAudioMonitorDlg::CeilingFinish() {
  GOConfig &config = r_SoundSystem.GetSettings();

  config.PolyphonyLimit(m_CeilSavedPolyphonyLimit);
  r_SoundSystem.GetEngine().SetHardPolyphony(m_CeilSavedPolyphonyLimit);
  if (m_BenchmarkActive->GetValue())
    // Held pipes are already out (Restore waits for that), so this stops
    // cleanly on the next benchmark tick.
    m_BenchmarkActive->SetValue(false);
  if (m_CeilCheck)
    m_CeilCheck->SetValue(false);
  m_CeilPhase = CeilPhase::Idle;
  CeilingStatus(
    m_CeilResults.empty()
      ? wxString(_("sweep aborted"))
      : wxString::Format(
          _("sweep finished (%zu cells) - results in the counters above"),
          m_CeilCellIdx));
}

void GOAudioMonitorDlg::CeilingTick(
  int64_t computeUs, int64_t callbackUs, unsigned usedPolyphony) {
  if (m_CeilPhase == CeilPhase::Idle || m_CeilPhase == CeilPhase::Done)
    return;
  if (m_CeilInTick)
    return; // reentrant poll while a reload/restart yields events

  struct TickGuard {
    bool &flag;
    ~TickGuard() { flag = false; }
  } guard{(m_CeilInTick = true, m_CeilInTick)};

  GOConfig &config = r_SoundSystem.GetSettings();

  switch (m_CeilPhase) {
  case CeilPhase::ApplyCell: {
    const CeilCell &cell = m_CeilCells[m_CeilCellIdx];
    const bool needCodec = config.UseV3SampleCompression() != cell.v3;
    const bool needBuffer = config.SamplesPerBuffer() != cell.buffer;
    /* V3 cells must measure the PRODUCTION V3 configuration: with the
     * subblock cache off they would silently benchmark the slow uncached
     * decode path. Applied via the device restart below; restored in the
     * Restore phase. Legacy sections never touch the subblock path
     * regardless of the flag (GOSoundStream::InitStream). */
    const bool needCache = cell.v3 && !config.UseV3SubblockCache();

    /* Kill, don't fade, before a switch: note-offs for ~20k voices would
     * first SPAWN thousands of release samples (long tails wasted sweep
     * time and polluted the next cell), while the imminent device
     * restart resp. organ reload ends every voice instantly anyway -
     * GO's own Panic (Escape) is exactly such a close+reopen. So just
     * forget the handles; the switch below does the killing. */
    if ((needCodec || needBuffer || needCache) && !m_HeldPipes.empty()) {
      if (m_HeldPipesSlider)
        m_HeldPipesSlider->SetValue(0);
      ClearHeldPipes(false);
    }

    // At most ONE settings switch per pass; WaitReady leads back here, so
    // codec-then-buffer converges on its own.
    if (needCodec) {
      CeilingStatus(wxString::Format(
        _("%s: switching codec, reloading organ (takes a while)..."),
        ceilCellName(cell.v3, cell.buffer)));
      m_CeilPhase = CeilPhase::WaitReady;
      m_CeilPollsLeft = CEIL_READY_TIMEOUT_POLLS;
      m_SetV3AndReload(cell.v3); // synchronous; AbortPlayback hook cleans up
      break;
    }
    if (needBuffer || needCache) {
      CeilingStatus(wxString::Format(
        _("%s: switching buffer size..."), ceilCellName(cell.v3, cell.buffer)));
      if (needCache)
        config.UseV3SubblockCache(true);
      m_CeilPhase = CeilPhase::WaitReady;
      m_CeilPollsLeft = CEIL_READY_TIMEOUT_POLLS;
      m_SetBufferSize(cell.buffer);
      break;
    }

    // Cell settings are live - arm the measurement machinery.
    if (!m_BenchmarkActive->GetValue()) {
      m_BenchmarkActive->SetValue(true);
      m_NextTriggerDueMs = wxGetLocalTimeMillis();
      m_BenchmarkTimer.Start(BENCHMARK_TICK_MS);
    }
    // Re-apply after every device restart/reload (both reset it from the
    // config, which we did change - belt and braces).
    r_SoundSystem.GetEngine().SetHardPolyphony(CEIL_POLYPHONY_LIMIT);
    std::srand(HELD_PIPES_RNG_SEED);
    m_CeilLo = 0;
    m_CeilHi = 0;
    m_CeilLoStrict = true; // 0 is clean by definition
    m_CeilRampMode = true;
    CeilingSetTarget(CEIL_START_TARGET);
    break;
  }

  case CeilPhase::WaitReady:
    if (
      p_OrganModel && GetTotalAccessibleKeys() > 0
      && r_SoundSystem.GetSampleRate() != 0)
      m_CeilPhase = CeilPhase::ApplyCell; // re-check remaining switches
    else if (--m_CeilPollsLeft <= 0) {
      wxLogWarning(
        _("Ceiling benchmark: sound/organ did not come back after a "
          "settings switch - aborting the sweep."));
      m_CeilPhase = CeilPhase::Restore;
    }
    break;

  case CeilPhase::Settle: {
    const unsigned held = (unsigned)m_HeldPipes.size();
    const unsigned tolerance = std::max(50u, m_CeilTarget / 100);
    /* Ready only when the REAL polyphony has come down to the target
     * too: after a downward step the stopped voices' releases keep
     * sounding (and computing) for seconds - measuring before they
     * decay would blame the lower target for the old load. */
    const bool reached = held + tolerance >= m_CeilTarget
      && held <= m_CeilTarget + tolerance
      && usedPolyphony <= m_CeilTarget + tolerance;

    if (reached)
      // Short stabilization once at target, then open the window.
      m_CeilPollsLeft = std::min(m_CeilPollsLeft, CEIL_SETTLE_EXTRA_POLLS);
    if (--m_CeilPollsLeft > 0)
      break;
    if (!reached) {
      // Could not even reach/hold the target (tiny organ, engine refusing
      // voices) - count that as a dirty probe and bisect downwards.
      m_CeilHi = m_CeilTarget;
      m_CeilRampMode = false;
      if (m_CeilTarget <= m_CeilLo) {
        m_CeilLo = 0;
        m_CeilLoStrict = true;
      }
      if (m_CeilHi - m_CeilLo <= std::max(250u, m_CeilLo / 50)) {
        if (m_CeilLoStrict)
          CeilingCellDone(m_CeilLo);
        else
          CeilingSetTarget(m_CeilLo);
      } else
        CeilingSetTarget((m_CeilLo + m_CeilHi) / 2);
      break;
    }
    m_CeilWinWorstCompute = 0;
    m_CeilBelowPolls = 0;
    m_CeilPhase = CeilPhase::Evaluate;
    m_CeilPollsLeft = CEIL_EVAL_POLLS;
    break;
  }

  case CeilPhase::Evaluate: {
    m_CeilWinWorstCompute = std::max(m_CeilWinWorstCompute, computeUs);

    const unsigned sampleRate = r_SoundSystem.GetSampleRate();
    const int64_t budgetUs = sampleRate
      ? (int64_t)r_SoundSystem.GetSamplesPerBuffer() * 1000000 / sampleRate
      : 0;
    /* Edge law bookkeeping: a poll proves headroom only when the
     * callback peak stayed a real MARGIN below the compute peak (below
     * 95% of it) - the callback returned comfortably before the
     * period's compute would have finished. */
    if (callbackUs * CEIL_HEADROOM_DEN < computeUs * CEIL_HEADROOM_NUM)
      m_CeilBelowPolls++;

    /* SOLE saturation criterion (user 2026-07-11): the window is dirty
     * exactly when the callback did not manage to leave in time - below
     * the compute-time peak - one single time in the whole 12s. Glitch
     * events and compute spikes are deliberately NOT judged: at the
     * absolute edge, inhomogeneous load throws stray events without the
     * operating point being unsustainable, and an external disturbance
     * would have to saturate all 36 polls to fake a dirty verdict.
     * Hard to beat for stability - and it measures GO's INTRINSIC
     * limit, independent of what else the machine happens to do. */
    bool failed = false;
    bool windowDone = false;

    if (!failed) {
      if (
        m_CeilRampMode && budgetUs > 0
        && CEIL_EVAL_POLLS - m_CeilPollsLeft >= CEIL_FAST_POLLS
        && m_CeilWinWorstCompute * 5 < budgetUs * 3) {
        /* Miles below the edge (compute peak < 60% of the budget after
         * ~1s clean): leap instead of sitting out the window. Compute
         * scales roughly linearly with voices, so aim for ~75% budget
         * directly, capped at doubling per step. */
        m_CeilLo = m_CeilTarget;
        m_CeilLoStrict = false; // ~1s probe only - re-verified if decisive

        const uint64_t aimed = (uint64_t)m_CeilTarget
          * (uint64_t)(budgetUs * 3)
          / (uint64_t)(std::max<int64_t>(m_CeilWinWorstCompute, 1) * 4);
        const unsigned leap = (unsigned)std::min<uint64_t>(
          {(uint64_t)CEIL_TARGET_CAP,
           (uint64_t)m_CeilTarget * 2,
           std::max<uint64_t>(aimed, m_CeilTarget + 1000)});

        if (leap > m_CeilTarget) {
          CeilingSetTarget(leap);
          break;
        }
        // No headroom left by the estimate: sit out the strict window.
      }
      windowDone = --m_CeilPollsLeft <= 0;
      if (
        windowDone
        && m_CeilBelowPolls * 10 < CEIL_EVAL_POLLS) // headroom < ~10% of polls
        failed = true; // saturated: callback glued to the compute curve
    }

    if (failed) {
      if (!m_CeilRetriedTarget) {
        // First failure at this target: measure once more before ruling
        // it dirty (Windows background noise must not cap the cell).
        m_CeilRetriedTarget = true;
        m_CeilPhase = CeilPhase::Settle;
        m_CeilPollsLeft = CEIL_SETTLE_TIMEOUT_POLLS;
        CeilingStatus(wxString::Format(
          _("%s: glitch at %u - retrying once"),
          ceilCellName(
            m_CeilCells[m_CeilCellIdx].v3, m_CeilCells[m_CeilCellIdx].buffer),
          m_CeilTarget));
        break;
      }
      m_CeilHi = m_CeilTarget;
      m_CeilRampMode = false;
      if (m_CeilTarget <= m_CeilLo) {
        // A provisional (fast-forwarded) lo failed its strict
        // verification: nothing below hi is proven anymore.
        m_CeilLo = 0;
        m_CeilLoStrict = true;
      }
      if (m_CeilHi - m_CeilLo <= std::max(250u, m_CeilLo / 50)) {
        if (m_CeilLoStrict)
          CeilingCellDone(m_CeilLo);
        else
          CeilingSetTarget(m_CeilLo); // strict verification window
      } else
        CeilingSetTarget((m_CeilLo + m_CeilHi) / 2);
      break;
    }
    if (!windowDone)
      break;
    // Full window passed clean.
    m_CeilLo = m_CeilTarget;
    m_CeilLoStrict = true;
    if (m_CeilLo >= CEIL_TARGET_CAP) {
      CeilingCellDone(m_CeilLo);
      break;
    }
    if (m_CeilRampMode)
      CeilingSetTarget(
        std::min(CEIL_TARGET_CAP, m_CeilTarget + std::max(1000u, m_CeilTarget / 4)));
    else if (m_CeilHi - m_CeilLo <= std::max(250u, m_CeilLo / 50))
      CeilingCellDone(m_CeilLo);
    else
      CeilingSetTarget((m_CeilLo + m_CeilHi) / 2);
    break;
  }

  case CeilPhase::Restore:
    // One step per pass, same self-converging pattern as ApplyCell. The
    // benchmark voices are killed by the restore's own device restart /
    // reload (see ApplyCell) - never faded out via mass note-offs.
    if (!m_HeldPipes.empty()) {
      if (m_HeldPipesSlider)
        m_HeldPipesSlider->SetValue(0);
      ClearHeldPipes(false);
    }
    if (config.UseV3SampleCompression() != m_CeilOrigV3) {
      CeilingStatus(_("restoring your codec, reloading organ..."));
      m_CeilRestoreKicked = true;
      m_SetV3AndReload(m_CeilOrigV3);
      break;
    }
    if (
      config.SamplesPerBuffer() != m_CeilOrigBuffer
      || config.UseV3SubblockCache() != m_CeilOrigCache
      || !m_CeilRestoreKicked) {
      // The restart doubles as the kill for the dropped handles above
      // when codec and buffer happen to need no restoring.
      CeilingStatus(_("restoring your buffer size..."));
      m_CeilRestoreKicked = true;
      config.UseV3SubblockCache(m_CeilOrigCache);
      m_SetBufferSize(m_CeilOrigBuffer);
      break;
    }
    if (r_SoundSystem.GetSampleRate() == 0)
      break; // device still coming back up
    CeilingFinish();
    break;

  default:
    break;
  }
}

void GOAudioMonitorDlg::OnAffinityToggle(wxCommandEvent &event) {
  GOSoundThread::s_AffinityMode.store(
    event.IsChecked() ? 1 : 0, std::memory_order_relaxed);
}

void GOAudioMonitorDlg::OnCeilingToggle(wxCommandEvent &event) {
  if (event.IsChecked())
    CeilingStart();
  else
    CeilingRequestStop();
}

void GOAudioMonitorDlg::OnBenchmarkToggle(wxCommandEvent &event) {
  if (event.IsChecked()) {
    if (!p_OrganModel) {
      m_BenchmarkActive->SetValue(false);
      return;
    }
    m_NextTriggerDueMs = wxGetLocalTimeMillis();
    AdjustHeldNotes();
    m_BenchmarkTimer.Start(BENCHMARK_TICK_MS);
  } else if (m_HeldPipes.empty()) {
    StopBenchmark();
  } else {
    /* Wind-down instead of stopping 20k held pipes in ONE synchronous
     * GUI-thread loop (which froze GO at full load - StopBenchmark's
     * ClearHeldPipes bypassed the ramp bound entirely and hit the engine
     * with an instant mass release). The timer keeps running with target
     * 0; OnBenchmarkTimer completes the stop once the list is empty. */
    ;
  }
}

void GOAudioMonitorDlg::OnRateSliderChanged(wxCommandEvent &WXUNUSED(event)) {
  m_RateLabel->SetLabel(
    wxString::Format(_("%d/min"), m_RateSlider->GetValue()));
}

void GOAudioMonitorDlg::OnHeldSliderChanged(wxCommandEvent &WXUNUSED(event)) {
  m_HeldLabel->SetLabel(wxString::Format(
    _("%d / %u"), m_HeldSlider->GetValue(), GetTotalAccessibleKeys()));
  if (m_BenchmarkActive->GetValue())
    AdjustHeldNotes();
}

void GOAudioMonitorDlg::OnBenchmarkTimer(wxTimerEvent &WXUNUSED(event)) {
  // This timer fires every BENCHMARK_TICK_MS (50ms), much more often than
  // m_Timer (333ms) drives UpdateNow()/RefreshOrganAvailability(). Without
  // this call here too, a new/unloaded organ between two m_Timer ticks would
  // leave p_OrganModel dangling for up to ~6 benchmark ticks - a real
  // use-after-free (loading a new organ, or exiting GO, destroys the old
  // GOOrganController synchronously; this cache must never be more than one
  // of *this* timer's own ticks behind that).
  RefreshOrganAvailability();
  if (!p_OrganModel) {
    StopBenchmark();
    m_BenchmarkActive->SetValue(false);
    return;
  }

  AdjustHeldPipes();

  if (!m_BenchmarkActive->GetValue()) {
    // Winding down after a toggle-off: only the held-pipes ramp keeps
    // running (toward 0); everything else stops once it drained.
    if (m_HeldPipes.empty())
      StopBenchmark();
    return;
  }

  const wxLongLong now = wxGetLocalTimeMillis();
  const int kpm = m_RateSlider->GetValue();

  // Two passes: first fire every due action and erase it, collecting
  // REATTACK notes (with their refill flag) instead of scheduling their
  // follow-up RELEASE immediately - push_back() on m_PendingActions would
  // otherwise risk invalidating the iterator this loop is using.
  struct ReattackFollowUp {
    NoteKey note;
    bool refillHeldOnRelease;
  };
  std::vector<ReattackFollowUp> reattacked;
  unsigned refillsNeeded = 0;

  for (auto it = m_PendingActions.begin(); it != m_PendingActions.end();) {
    if (now >= it->dueTimeMs) {
      GOManual *manual = p_OrganModel->GetManual(it->note.manualIndex);

      if (it->action == PendingNoteAction::RELEASE) {
        if (manual)
          manual->SetMidiNoteState(it->note.midiNote, 0);
        if (it->refillHeldOnRelease)
          refillsNeeded++;
      } else {
        if (manual)
          manual->SetMidiNoteState(it->note.midiNote, 0x7f);
        reattacked.push_back({it->note, it->refillHeldOnRelease});
      }
      it = m_PendingActions.erase(it);
    } else {
      ++it;
    }
  }

  // A reattack (fresh transient, or a held note interrupted by a collision)
  // always gets its own eventual release scheduled, exactly like a normal
  // transient - the refill flag travels with it, so a collision's borrowed
  // held slot is only refilled once this full attack+release cycle
  // completes ("later", not immediately).
  for (const ReattackFollowUp &r : reattacked)
    m_PendingActions.push_back(
      {r.note,
       now + RandomTransientHoldMs(kpm),
       PendingNoteAction::RELEASE,
       r.refillHeldOnRelease});

  for (unsigned i = 0; i < refillsNeeded; i++)
    AddOneHeldNote();

  if (kpm > 0) {
    const wxLongLong intervalMs = 60000 / kpm;

    // If we've fallen far behind (benchmark just started/resumed, rate was
    // just raised a lot, or this GUI thread was briefly stalled), don't
    // fire a huge burst of catch-up notes all at once - just resume from
    // now instead of draining a long backlog in one go.
    if (m_NextTriggerDueMs + intervalMs * 20 < now)
      m_NextTriggerDueMs = now;

    // Catch-up loop, not a single "if": stepping from the previously
    // *scheduled* time (not from "now") and firing every interval that has
    // become due - not just one - is what makes the actual rate match the
    // slider. A plain "if now >= due" (checked only once per
    // BENCHMARK_TICK_MS=50ms tick) silently caps throughput at 1200/min
    // however high the slider goes, and rebasing off "now" after a late
    // tick would silently skip whichever intervals fell inside the delay -
    // both are exactly what produced the audible gaps at higher rates.
    while (now >= m_NextTriggerDueMs) {
      m_NextTriggerDueMs += intervalMs;

      NoteKey note;

      if (!PickRandomKey(note))
        continue;

      GOManual *manual = p_OrganModel->GetManual(note.manualIndex);

      if (!manual)
        continue;

      if (IsHeld(note)) {
        // Borrow the note from the held set: interrupt it, then let it go
        // through a full attack+release like any other transient, instead
        // of just blipping and staying held. One held slot gets refilled
        // once that release fires (not necessarily with the same note).
        m_HeldNotes.erase(
          std::remove(m_HeldNotes.begin(), m_HeldNotes.end(), note),
          m_HeldNotes.end());
        manual->SetMidiNoteState(note.midiNote, 0);
        m_PendingActions.push_back(
          {note,
           now + COLLISION_REATTACK_DELAY_MS,
           PendingNoteAction::REATTACK,
           /*refillHeldOnRelease=*/true});
      } else if (IsPendingRelease(note)) {
        // Collision with a note that's already an active transient from a
        // previous iteration/tick: interrupt it and let it reattack, same
        // as a held collision, instead of wasting this pick entirely. Drop
        // its existing scheduled release first, otherwise it could fire
        // after the reattack and kill the freshly re-triggered note.
        m_PendingActions.erase(
          std::remove_if(
            m_PendingActions.begin(),
            m_PendingActions.end(),
            [&](const PendingNoteAction &p) {
              return p.action == PendingNoteAction::RELEASE && p.note == note;
            }),
          m_PendingActions.end());
        manual->SetMidiNoteState(note.midiNote, 0);
        m_PendingActions.push_back(
          {note,
           now + COLLISION_REATTACK_DELAY_MS,
           PendingNoteAction::REATTACK,
           /*refillHeldOnRelease=*/false});
      } else {
        manual->SetMidiNoteState(note.midiNote, 0x7f);
        m_PendingActions.push_back(
          {note,
           now + RandomTransientHoldMs(kpm),
           PendingNoteAction::RELEASE,
           false});
      }
    }
  }
}

