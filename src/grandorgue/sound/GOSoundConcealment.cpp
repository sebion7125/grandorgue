/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundConcealment.h"

#include <algorithm>

#include <thread>

#include <chrono>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#endif

#include "GOFlushToZero.h" 

#include "sound/buffer/GOSoundBufferMutable.h"
#include "xorcist.h"

std::atomic<int> GOSoundConcealment::s_ForcedPerMinute{0};
std::atomic<int> GOSoundConcealment::s_MissPattern{0};
// PV 2k als Vorgabe (Nutzer 08.08.): Basis des Siegermodells.
std::atomic<int> GOSoundConcealment::s_Mode{GOSoundConcealment::MODE_PV_2K};
// 25 % Geduld (Nutzerentscheid 12.08.): ~670 us fuer echte Nachzuegler,
// und die Folgeperiode behaelt 2000 us Vorlauf - Luft selbst fuer die
// teuerste Verfahrenswahl (2k/Ordnung 8, Runde bis 813 us) PLUS Mix.
std::atomic<int> GOSoundConcealment::s_MixGracePct{25};
std::atomic<float> GOSoundConcealment::s_FreqDamping{0.0f};
// Ordnung 8 als Vorgabe (Nutzer 08.08.): Siegermodell 2k/8; passt
// zu den Vorgaben PV 2k + Anker + no trend (bei Ordnung 1 braeuchte
// ein Schweller den Trend).
std::atomic<int> GOSoundConcealment::s_Order{8};
std::atomic<bool> GOSoundConcealment::s_Hybrid{false};
std::atomic<bool> GOSoundConcealment::s_HybAllBands{true};
std::atomic<int> GOSoundConcealment::s_HybHandOrder{8};
std::atomic<int> GOSoundConcealment::s_HybShortCap{6};
std::atomic<float> GOSoundConcealment::s_SafetyCap{1.2f};
std::atomic<int> GOSoundConcealment::s_AnchorTau{0};
std::atomic<float> GOSoundConcealment::s_RootClamp{1.0f};
// 0.99 statt 1.02 (Nutzergehoer 10.08.2026, nachgemessen).  Ueber 1.0
// darf jede Mode je Hop wachsen; solange der Ring ueberwiegend Wahrheit
// enthaelt, bricht die naechste Analyse das ab.  Ab etwa halber
// Lueckendichte (1:1-Muster, 12000/min) ist der Ring aber zur Haelfte die
// eigene Vorhersage - dann schliesst sich die Schleife, und ein einzelner
// Bin schaukelt sich auf.  Gemessen am D-Dur-Material im 1:1-Muster, nur
// oberhalb 200 Hz, groesster schmalbandiger Ueberschuss gegen den
// Eingang: rho 0.99 +58.4 dB, 1.00 +64.4, 1.01 +72.6, 1.02 +72.4 - also
// 14 dB Fiepen fuer nichts, denn der mittlere Fehler ist bei 0.99 und
// 1.02 dort praktisch gleich (-11.22 gegen -11.44 dB).  Der Nutzer hat
// es als metallisches Fiepen gehoert, bevor es jemand gemessen hat.
// PREIS, ehrlich: bei MITTLERER Dichte (6000/min) kostet 0.99 3.6 dB im
// Mittel (-17.99 gegen -21.57) - dort gibt es dafuer auch nichts zu
// gewinnen, der schmalbandige Ueberschuss ist bei allen rho gleich.
std::atomic<float> GOSoundConcealment::s_DetectRho{0.99f};
// Anker + kein Trend als Vorgabe (Nutzer 08.08.): beide Kalibrierungen
// der adaptiven Naht und des Trends stammen aus der Ordnung-1-Zeit;
// ab Ordnung 2 kostet der Trend doppelt gebuchte Huellkurven und die
// Bruecke verliert gegen den Anker.  ACHTUNG Wechselwirkung: bei
// Ordnung 1 braucht ein Schweller den Trend - wer auf Ordnung 1
// zurueckstellt, sollte no trend abhaken.
std::atomic<int> GOSoundConcealment::s_Seam{1};
std::atomic<bool> GOSoundConcealment::s_NoTrend{true};
std::atomic<int64_t> GOSoundConcealment::s_ShadowLastUs{0};
std::atomic<int64_t> GOSoundConcealment::s_ShadowMaxUs{0};
std::atomic<int> GOSoundConcealment::s_EffectiveOrder{0};
std::atomic<int> GOSoundConcealment::s_EffectiveMode{
  GOSoundConcealment::MODE_PV_2K};
// Vorgabe AUS: die Messung sagt +0,2 dB im Mittel und den schlimmsten Fall
// zurueck auf den Wert ohne Hybrid, aber sie kostet zwei von sechs
// Einsaetzen - das gehoert gehoert, bevor es Vorgabe wird.
std::atomic<float> GOSoundConcealment::s_DetResGate{0.0f};
std::atomic<float> GOSoundConcealment::s_DetResDecay{0.85f};
std::atomic<bool> GOSoundConcealment::s_V0Hindcast{false};
std::atomic<int> GOSoundConcealment::s_ReentryFade{0}; // 0 = Auto (3 ms)
std::atomic<bool> GOSoundConcealment::s_StragglerTruth{false};
std::atomic<uint64_t> GOSoundConcealment::s_ForcedCount{0};
std::atomic<uint64_t> GOSoundConcealment::s_SkipCount{0};
std::atomic<uint64_t> GOSoundConcealment::s_CatchupCount{0};
std::atomic<uint64_t> GOSoundConcealment::s_LateRealCount{0};
std::atomic<uint64_t> GOSoundConcealment::s_StaleDiscards{0};
std::atomic<uint64_t> GOSoundConcealment::s_LateSlices{0};
std::atomic<uint64_t> GOSoundConcealment::s_OnsetCount{0};
std::atomic<uint32_t> GOSoundConcealment::s_OnsetDelta{0};
std::atomic<int64_t> GOSoundConcealment::s_MaxWaitUs{0};
std::atomic<uint64_t>
  GOSoundConcealment::s_OpenGapHist[GOSoundConcealment::TIMING_BUCKETS]{};
std::atomic<uint64_t>
  GOSoundConcealment::s_SkipGapHist[GOSoundConcealment::TIMING_BUCKETS]{};
std::atomic<uint64_t>
  GOSoundConcealment::s_RoundDurHist[GOSoundConcealment::TIMING_BUCKETS]{};
std::atomic<uint64_t> GOSoundConcealment::s_SkipAfterBail{0};
std::atomic<uint64_t> GOSoundConcealment::s_LateAfterSkip{0};
std::atomic<uint64_t> GOSoundConcealment::s_LateSliceMax{0};
std::atomic<uint64_t> GOSoundConcealment::s_PhaseLateSlices{0};
std::atomic<uint32_t> GOSoundConcealment::s_TimingPhaseKey{0};
std::atomic<uint64_t>
  GOSoundConcealment::s_CompPostBailHist[GOSoundConcealment::TIMING_BUCKETS]{};
std::atomic<uint64_t>
  GOSoundConcealment::s_CompNormalHist[GOSoundConcealment::TIMING_BUCKETS]{};
std::atomic<uint64_t> GOSoundConcealment::s_OpenByAudio{0};
std::atomic<uint64_t> GOSoundConcealment::s_SkipPrevAudio{0};
std::atomic<bool> GOSoundConcealment::s_PrevDeliveryBail{false};
std::atomic<bool> GOSoundConcealment::s_PrevPrevDeliveryBail{false};

// Kein Regelpfad haengt an diesen Zahlen - sie klassifizieren nur.  Die
// Kanten sind auf die Hypothese zugeschnitten: Epilogabstaende klumpen
// entweder um die Periode (normal) oder beim Bail->Pass-Uebergang um die
// Reserve - und die feinen Toepfe unten trennen "um die Reserve" von
// "noch enger als die Runde selbst".
// EINE Leiter fuer alle vier Histogramme (12.08.): nur so stehen
// Epilogabstand und Rundenlaufzeit im Monitor in denselben Spalten
// uebereinander, und das Kollisionskriterium (Runde > Abstand) ist direkt
// ablesbar statt zeilenweise nachzurechnen.
static constexpr int64_t GAP_EDGES_US[] = {50, 100, 150, 200, 300, 600};
static constexpr int64_t DUR_EDGES_US[] = {50, 100, 150, 200, 300, 600};
static_assert(
  sizeof(GAP_EDGES_US) / sizeof(GAP_EDGES_US[0])
      == GOSoundConcealment::TIMING_BUCKETS - 1
    && sizeof(DUR_EDGES_US) / sizeof(DUR_EDGES_US[0])
      == GOSoundConcealment::TIMING_BUCKETS - 1,
  "bucket edges must match TIMING_BUCKETS");

static inline int64_t XoNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
           std::chrono::steady_clock::now().time_since_epoch())
    .count();
}

static inline void XoAddToHist(
  std::atomic<uint64_t> *pHist, const int64_t *edges, int64_t v) {
  int i = 0;

  while (i < GOSoundConcealment::TIMING_BUCKETS - 1 && v >= edges[i])
    i++;
  pHist[i].fetch_add(1, std::memory_order_relaxed);
}

GOSoundConcealment::TimingStats GOSoundConcealment::GetTimingStats() {
  TimingStats t;

  for (int i = 0; i < TIMING_BUCKETS; i++) {
    t.openGap[i] = s_OpenGapHist[i].load(std::memory_order_relaxed);
    t.skipGap[i] = s_SkipGapHist[i].load(std::memory_order_relaxed);
    t.roundDur[i] = s_RoundDurHist[i].load(std::memory_order_relaxed);
  }
  for (int i = 0; i < TIMING_BUCKETS; i++) {
    t.compPostBail[i] = s_CompPostBailHist[i].load(std::memory_order_relaxed);
    t.compNormal[i] = s_CompNormalHist[i].load(std::memory_order_relaxed);
  }
  t.skipAfterBail = s_SkipAfterBail.load(std::memory_order_relaxed);
  t.lateAfterSkip = s_LateAfterSkip.load(std::memory_order_relaxed);
  t.lateSliceMax = s_LateSliceMax.load(std::memory_order_relaxed);
  t.phaseLateSlices = s_PhaseLateSlices.load(std::memory_order_relaxed);
  t.openByAudio = s_OpenByAudio.load(std::memory_order_relaxed);
  t.skipPrevAudio = s_SkipPrevAudio.load(std::memory_order_relaxed);
  return t;
}

void GOSoundConcealment::RecordMixCompletion(int64_t us) {
  if (us < 0)
    return;
  XoAddToHist(
    s_PrevPrevDeliveryBail.load(std::memory_order_relaxed)
      ? s_CompPostBailHist
      : s_CompNormalHist,
    DUR_EDGES_US, us);
}

void GOSoundConcealment::ResetTimingStats() {
  for (int i = 0; i < TIMING_BUCKETS; i++) {
    s_OpenGapHist[i].store(0, std::memory_order_relaxed);
    s_SkipGapHist[i].store(0, std::memory_order_relaxed);
    s_RoundDurHist[i].store(0, std::memory_order_relaxed);
  }
  for (int i = 0; i < TIMING_BUCKETS; i++) {
    s_CompPostBailHist[i].store(0, std::memory_order_relaxed);
    s_CompNormalHist[i].store(0, std::memory_order_relaxed);
  }
  s_SkipAfterBail.store(0, std::memory_order_relaxed);
  s_LateAfterSkip.store(0, std::memory_order_relaxed);
  s_LateSliceMax.store(0, std::memory_order_relaxed);
  s_PhaseLateSlices.store(0, std::memory_order_relaxed);
  s_OpenByAudio.store(0, std::memory_order_relaxed);
  s_SkipPrevAudio.store(0, std::memory_order_relaxed);
}

// The engine's mode constants ARE the library's - keep them that way, the
// Audio Monitor dropdown indexes both.
static_assert(
  GOSoundConcealment::MODE_OFF == XORCIST_OFF
    && GOSoundConcealment::MODE_PV_2K == XORCIST_PV_2K
    && GOSoundConcealment::MODE_PV_4K == XORCIST_PV_4K
    && GOSoundConcealment::MODE_PV_8K == XORCIST_PV_8K
    && GOSoundConcealment::MODE_PV_1K == XORCIST_PV_1K,
  "concealment mode constants must match libxorcist");

GOSoundConcealment::GOSoundConcealment(
  unsigned nChannels,
  unsigned samplesPerBuffer,
  unsigned sampleRate,
  const char *wisdomPath)
  : m_X(Xorcist_new(nChannels, samplesPerBuffer, sampleRate, wisdomPath)),
    m_NChannels(nChannels),
    m_SamplesPerBuffer(samplesPerBuffer),
    m_SampleRate(sampleRate),
    // Per instance and per run: the forced-miss dice must not repeat the
    // same pattern on every device.
    m_RandState(
      0x9E3779B97F4A7C15ull ^ ((uint64_t)samplesPerBuffer << 32)
      ^ (uint64_t)(uintptr_t)this),
    m_InjectRemaining(0),
    m_InjectCooldown(0),
    m_AltPhase(false),
    m_AltSkipFirst(false) {
  // Start the shadow threads last, when every member they read exists.
  // SetupConcealment() replaces instances only while the sound system is
  // stopped, so construction and destruction never race the audio path.
  for (unsigned c = 0; c < nChannels; c++)
    m_ShadowThreads.emplace_back(
      &GOSoundConcealment::ShadowThreadMain, this, c);
}

GOSoundConcealment::~GOSoundConcealment() {
  {
    std::lock_guard<std::mutex> lock(m_ShadowMutex);

    m_ShadowRun = false;
  }
  m_ShadowCv.notify_all();
  for (auto &t : m_ShadowThreads)
    if (t.joinable())
      t.join();
  Xorcist_free(m_X);
}

void GOSoundConcealment::ShadowThreadMain(unsigned channel) {
#ifdef __linux__
  // The net is rigged before the acrobatics: above the render workers
  // (SCHED_FIFO 60), below the audio callback (70) and PipeWire's data
  // loops (88+).  With plain SCHED_OTHER the shadow starves exactly under
  // sustained full load - the one situation it exists for.  Clamp to the
  // rtprio rlimit if 61 is not permitted; without any RT permission run
  // at normal priority (better a fair-weather net than none).
  {
    struct sched_param sp = {};

    sp.sched_priority = 61;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
      struct rlimit rl;

      if (
        getrlimit(RLIMIT_RTPRIO, &rl) == 0 && rl.rlim_cur > 0
        && rl.rlim_cur < 61) {
        sp.sched_priority = (int)rl.rlim_cur;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
      }
    }
  }
#endif
  GOEnableFlushToZero(); // renders spectra like the workers do

  uint64_t done = 0;

  for (;;) {
    {
      std::unique_lock<std::mutex> lock(m_ShadowMutex);

      m_ShadowCv.wait(
        lock, [&] { return !m_ShadowRun || m_ShadowSeq != done; });
      if (!m_ShadowRun)
        return;
      done = m_ShadowSeq;
    }
    // The round was OPENED by the epilog (predictBegin already ran
    // there), so all that is left here is this thread's own channel.
    Xorcist_predictChannel(m_X, channel);
    if (
      m_ShadowOutstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      // Last channel: measure BEFORE publishing - predictEnd releases
      // the busy guard, after which the next epilog may already
      // overwrite m_ShadowT0 under us.  The wall time since the epilog
      // is "when does the net stand", the number the one-period budget
      // is spent on.
      const int64_t us
        = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_ShadowT0)
            .count();

      Xorcist_predictEnd(m_X);

      // Moeglichkeit 3: die waehrend dieser Runde am Besetzt-Waechter
      // gescheiterte Oeffnung SOFORT nachholen, jetzt wo der Platz frei
      // ist - nicht erst beim naechsten Epilog, eine Periode spaeter.
      if (m_OpenPending.exchange(false, std::memory_order_acq_rel))
        TryOpenRound(false, true);

      XoAddToHist(s_RoundDurHist, DUR_EDGES_US, us);
      s_ShadowLastUs.store(us, std::memory_order_relaxed);
      for (int64_t old = s_ShadowMaxUs.load(std::memory_order_relaxed);
           us > old
           && !s_ShadowMaxUs.compare_exchange_weak(
             old, us, std::memory_order_relaxed);)
        ;
    }
  }
}

void GOSoundConcealment::SyncMode() {
  if (!m_X)
    return;

  int mode = GetMode();

  // 1k klemmen statt hoffen: der Analyse-Hop ist nfft/4 = 256 und muss ein
  // Vielfaches der Puffergroesse sein.  Die Bibliothek prueft das nicht -
  // sie rechnete stillschweigend Unsinn -, also faellt der Modus hier
  // sichtbar auf 2k zurueck, und GetEffectiveMode() sagt es dem Monitor.
  if (mode == MODE_PV_1K && (256u % m_SamplesPerBuffer) != 0)
    mode = MODE_PV_2K;
  s_EffectiveMode.store(mode, std::memory_order_relaxed);

  if (Xorcist_getMode(m_X) != mode)
    Xorcist_setMode(m_X, mode);

  const float damping = GetFreqDamping();
  const int order = GetOrder();
  const int seam = GetSeam();
  const int noTrend = GetNoTrend() ? 1 : 0;
  const int hybrid = GetHybrid() ? 1 : 0;
  const int hybAll = GetHybAllBands() ? 1 : 0;
  const int hybHand = GetHybHandOrder();
  const int hybCap = GetHybShortCap();
  // Siegermodell-Konstanten der Lab-Messreihen (06.-08.08.), keine
  // Regler: Backfill (sonst laeuft hohe Ordnung bei Clustern heimlich
  // als Ordnung 1), Cap nur ueber Luecke+Fade (sonst drosselt ein
  // unhoerbarer Schattenschwanz die gespielten Samples), Wurzelkappe
  // 1.0 (nur echte Divergenz), Detektor-Modus rho 1.02 (Huellkurven
  // bleiben, Deckeln statt Heben).  Hybrid-Nebenwahlen fest: nur der
  // 4k-Simpel-Detektor, Vollbetrieb der kurzen Kette, keine Vorstufe.
  const int capSpan
    = (int)(m_SamplesPerBuffer + std::min(m_SamplesPerBuffer, 144u));
  const float safetyCap = GetSafetyCap();
  const int anchorTau = GetAnchorTau();
  const float rootClamp = GetRootClamp();
  const float detectRho = GetDetectRho();
  const float resGate = GetDetResGate();
  const int v0Hindcast = GetV0Hindcast() ? 1 : 0;
  // getTuning liefert die WIRKSAME Blendenlaenge, nicht die Einstellung -
  // fuer einen idempotenten Abgleich rechnet der Wirt die Auto-Vorgabe
  // (3 ms, hoechstens eine Periode, mindestens 16) selbst und setzt immer
  // explizit.  Muss der Formel in Xorcist::Xorcist (m_ReentryFade)
  // entsprechen.
  const int fadeAuto = (int)std::min(
    m_SamplesPerBuffer, std::max(16u, (unsigned)(0.003 * m_SampleRate)));
  const int fadeWanted = GetReentryFade();
  const int fade = fadeWanted > 0 ? std::min(fadeWanted, fadeAuto) : fadeAuto;
  XorcistTuning t;

  Xorcist_getTuning(m_X, &t);
  if (
    t.freqDamping != damping || t.order != order || t.seam != seam
    || t.noTrend != noTrend || t.hybrid != hybrid
    || t.hybAllBands != hybAll || t.hybHandOrder != hybHand
    || t.hybShortCap != hybCap || t.backfill != 1 || t.capSpan != capSpan
    || t.safetyCap != safetyCap || t.anchorTau != anchorTau
    || t.rootClamp != rootClamp || t.detectRho != detectRho
    || t.detResGate != resGate || t.v0Hindcast != v0Hindcast
    || t.reentryFade != fade) {
    t.freqDamping = damping;
    t.order = order;
    t.seam = seam;
    t.noTrend = noTrend;
    t.hybrid = hybrid;
    t.hybSimple = hybrid; // im Produkt nur der zuverlaessige 4k-Detektor
    t.hybAllBands = hybAll;
    t.hybHandOrder = hybHand;
    t.hybShortCap = hybCap;
    t.hybWarmOrder = 0; // Vollbetrieb: die Leiter verliert (08.08.)
    t.hybPre = 0;
    t.hybChooser = 0;
    t.hybReset = 0;
    t.patchTransients = 0;
    t.backfill = 1;
    t.capSpan = capSpan;
    t.safetyCap = safetyCap;
    t.anchorTau = anchorTau;
    t.rootClamp = rootClamp;
    t.detectRho = detectRho;
    t.gpu = 0;
    t.detResGate = resGate;
    t.detResDecay = s_DetResDecay.load(std::memory_order_relaxed);
    t.v0Hindcast = v0Hindcast;
    t.reentryFade = fade;
    Xorcist_setTuning(m_X, &t);
    // Read BACK what the library actually accepted - the effective-order
    // meter must show the library's truth, not our wish.
    Xorcist_getTuning(m_X, &t);
  }
  s_EffectiveOrder.store(t.order, std::memory_order_relaxed);

  // Phasenreine Timing-Statistik: Modus- oder Ordnungswechsel beginnt
  // eine neue Messphase.  Kumulative Toepfe ueber eine Umschaltung hinweg
  // waren unlesbar (2k-Vorspann verfaelschte die 1k-Aussage).
  {
    const uint32_t key = ((uint32_t)mode << 8) | (uint32_t)(t.order & 0xff);

    if (s_TimingPhaseKey.exchange(key, std::memory_order_relaxed) != key)
      ResetTimingStats();
  }
}

void GOSoundConcealment::ReplaceConcealedWithReal(const float *pInterleaved) {
  if (m_X)
    Xorcist_replaceLast(m_X, pInterleaved);
}

void GOSoundConcealment::SnapshotMix(const float *pInterleaved) {
  if (m_X)
    Xorcist_push(m_X, pInterleaved);
}

void GOSoundConcealment::OpenShadow(bool byAudioThread) {
  if (!m_X)
    return;
  SyncMode();

  // Einmal je Periode einsammeln (die Bibliothek setzt beim Lesen zurueck),
  // damit der Monitor Gesamtzahlen zeigt.  Hier statt im Monitor, weil ein
  // geschlossener Dialog sonst nichts abholen wuerde und die erste
  // Anzeige eine Stunde Betrieb auf einmal ausspuckt.
  {
    uint64_t stale = 0, late = 0;

    Xorcist_raceStats(&stale, &late);
    if (stale)
      s_StaleDiscards.fetch_add(stale, std::memory_order_relaxed);
    if (late) {
      s_LateSlices.fetch_add(late, std::memory_order_relaxed);
      s_PhaseLateSlices.fetch_add(late, std::memory_order_relaxed);
    }
  }
  {
    uint64_t onsets = 0;

    Xorcist_onsetStats(&onsets, nullptr);
    if (onsets) {
      s_OnsetCount.fetch_add(onsets, std::memory_order_relaxed);
      s_OnsetDelta.fetch_add((uint32_t)onsets, std::memory_order_relaxed);
    }
  }
  {
    uint64_t afterSkip = 0, maxSlice = 0;

    Xorcist_lateStats(&afterSkip, &maxSlice);
    if (afterSkip)
      s_LateAfterSkip.fetch_add(afterSkip, std::memory_order_relaxed);
    for (uint64_t old = s_LateSliceMax.load(std::memory_order_relaxed);
         maxSlice > old
         && !s_LateSliceMax.compare_exchange_weak(
           old, maxSlice, std::memory_order_relaxed);)
      ;
  }

  TryOpenRound(byAudioThread);
}

// Oeffnen und die Kanalthreads wecken.  Einziger Aufrufer ist der
// Mix-Epilog: eine Oeffnung, die auf eine laufende Runde trifft, faellt
// ersatzlos aus, und die naechste Chance kommt erst mit dem naechsten Mix.
//
// Genau hier sitzt die Spaete-Scheiben-Frage (siehe Header): die
// Zeitstempel unten messen, in welchem Abstand die Oeffnungen wirklich
// kommen, welche davon auf "besetzt" laufen, und ob diese Besetzt-Faelle
// binnen einer Periode auf einen Bail folgen.  Erst wenn diese Verteilung
// die Ursache zeigt, wird am Verhalten etwas geaendert.
void GOSoundConcealment::TryOpenRound(bool byAudioThread, bool isRetry) {
  int64_t gap = -1;
  bool prevAudio = false;

  // Der Nachhol-Pfad laesst die Epilogstatistik unberuehrt: er ist kein
  // Epilog, und seine winzigen Abstaende wuerden die Verteilung
  // verfaelschen, mit der die Kollisionsursache diagnostiziert wird.
  if (!isRetry) {
    const int64_t now = XoNowUs();
    const int64_t prev
      = m_LastAttemptUs.exchange(now, std::memory_order_relaxed);

    gap = prev < 0 ? -1 : now - prev;
    prevAudio
      = m_LastAttemptAudio.exchange(byAudioThread, std::memory_order_relaxed);
    if (byAudioThread)
      s_OpenByAudio.fetch_add(1, std::memory_order_relaxed);
    if (gap >= 0)
      XoAddToHist(s_OpenGapHist, GAP_EDGES_US, gap);
  }

  const int r = Xorcist_predictBegin(m_X);

  if (r != 1) {
    // Busy (previous round still in flight) or nothing to predict: the
    // sequence is NOT bumped, the threads stay asleep, the period goes
    // to the slice mechanism.  This is the entire backpressure story -
    // no aborts, no waiting.
    if (r == 3 && !isRetry) { // besetzt - seit 11.08. von idle getrennt
      s_SkipCount.fetch_add(1, std::memory_order_relaxed);
      // Vormerken: der zuletzt fertige Kanalthread holt den Start nach
      // predictEnd nach.  Trifft der Nachhol-Versuch selbst auf besetzt,
      // hat inzwischen ein Epilog frischer geoeffnet - nichts zu tun.
      m_OpenPending.store(true, std::memory_order_release);
      if (gap >= 0)
        XoAddToHist(s_SkipGapHist, GAP_EDGES_US, gap);
      if (prevAudio)
        s_SkipPrevAudio.fetch_add(1, std::memory_order_relaxed);

      const int64_t bail = m_LastBailUs.load(std::memory_order_relaxed);
      const int64_t periodUs
        = (int64_t)m_SamplesPerBuffer * 1000000 / m_SampleRate;
      const int64_t now2 = m_LastAttemptUs.load(std::memory_order_relaxed);

      if (bail >= 0 && now2 - bail <= periodUs)
        s_SkipAfterBail.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  if (isRetry)
    s_CatchupCount.fetch_add(1, std::memory_order_relaxed);
  else
    // Ein erfolgreicher Epilogstart macht jede aeltere Vormerkung
    // gegenstandslos - der Anker ist mit ihm aktuell.
    m_OpenPending.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(m_ShadowMutex);

    m_ShadowT0 = std::chrono::steady_clock::now();
    // release-store AFTER t0: the last channel's acq_rel fetch_sub makes
    // t0 visible to whichever thread ends up publishing.
    m_ShadowOutstanding.store((int)m_NChannels, std::memory_order_release);
    m_ShadowSeq++;
  }
  m_ShadowCv.notify_all();
}

void GOSoundConcealment::DeliverBail(GOSoundBufferMutable &outBuffer) {
  s_ForcedCount.fetch_add(1, std::memory_order_relaxed);
  m_LastBailUs.store(XoNowUs(), std::memory_order_relaxed);
  s_PrevPrevDeliveryBail.store(
    s_PrevDeliveryBail.load(std::memory_order_relaxed),
    std::memory_order_relaxed);
  s_PrevDeliveryBail.store(true, std::memory_order_relaxed);
  if (!m_X) {
    outBuffer.FillWithSilence();
    return;
  }
  SyncMode();
  Xorcist_conceal(m_X, outBuffer.GetData());
}

void GOSoundConcealment::Deliver(
  GOSoundBufferMutable &outBuffer, const GOSoundBuffer &real) {
  s_PrevPrevDeliveryBail.store(
    s_PrevDeliveryBail.load(std::memory_order_relaxed),
    std::memory_order_relaxed);
  s_PrevDeliveryBail.store(false, std::memory_order_relaxed);
  if (!m_X) {
    outBuffer.CopyFrom(real);
    return;
  }
  // Xorcist_pass() copies real -> out itself and applies the re-entry
  // crossfade when the previous period was concealed.
  Xorcist_pass(m_X, outBuffer.GetData(), real.GetData());
}

double GOSoundConcealment::NextRand() {
  uint64_t x = m_RandState;

  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  m_RandState = x;
  return (double)((x * 0x2545F4914F6CDD1Dull) >> 11)
    / (double)(1ull << 53);
}

void GOSoundConcealment::MaybeInjectDelay(const std::atomic_bool &abandon) {
  const int ratePerMinute = GetForcedXrunsPerMinute();

  if (ratePerMinute <= 0) {
    m_InjectRemaining = 0;
    m_InjectCooldown = 0;
    m_AltPhase = false;
    return;
  }

  const int pattern = GetMissPattern();

  if (pattern == MISS_ALTERNATE) {
    // Half the periods are late, but WHICH one of each pair is decided by
    // a coin toss rather than strict alternation.  Same 50 % load, 3.5 dB
    // less error (measured: -7.3 -> -10.8 dB steady state on a held
    // chord), and it costs nothing: a strictly periodic miss pattern
    // makes the history's "invented" mask periodic too, and that mask's
    // transform is a comb - at 128 frames a 187.5 Hz one - whose
    // sidebands around every partial reinforce each other coherently
    // from analysis to analysis.  A random pattern spreads the same
    // pollution into a noise floor instead.  Applies to a real
    // frame-skipping scheduler just as much as to this test mode.
    if (m_AltPhase) {        // second half of the pair: honour the toss
      m_AltPhase = false;
      if (!m_AltSkipFirst)
        return;
    } else {                 // first half: toss for this pair
      m_AltPhase = true;
      m_AltSkipFirst = NextRand() < 0.5;
      if (m_AltSkipFirst)
        return;
    }
  } else if (m_InjectRemaining == 0) {
    // Cooldown first: in pair mode injections must never follow each
    // other directly - the point of the listening test is "two shadow
    // buffers, then guaranteed real audio".
    if (m_InjectCooldown > 0) {
      m_InjectCooldown--;
      return;
    }

    const double periodsPerMinute
      = 60.0 * m_SampleRate / (double)m_SamplesPerBuffer;

    // Saturates on its own: at rate >= periodsPerMinute every period is
    // delayed, which is the hardest test of the self-feeding degradation.
    if (NextRand() >= (double)ratePerMinute / periodsPerMinute)
      return;
    if (pattern == MISS_PAIR) {
      m_InjectRemaining = 2;
      // Real periods between pairs: at least 8 (so each pair is heard in
      // isolation and the degradation envelope has reset via Pass()),
      // but RANDOMISED on top.  A fixed gap turns the test itself into a
      // periodic pattern - 2 + 8 periods at 128 frames repeats at 37 Hz,
      // squarely in the range the ear reads as flutter, and that
      // roughness would be blamed on the concealment instead of on the
      // metronome driving it.
      m_InjectCooldown = 8 + (unsigned)(NextRand() * 24.0);
    } else
      m_InjectRemaining = 1;
  }
  if (pattern != MISS_ALTERNATE)
    m_InjectRemaining--;

  // The cap must span the FULL distance to the bail deadline: the mix for
  // period P runs about one period BEFORE P's delivery callback, whose
  // bail fires another (budget - reserve) after ITS entry.  A mere 1.25x
  // budget from mix start ended BEFORE that deadline - the "delayed" mix
  // then finished comfortably, the real buffer went out, and the
  // injection never triggered a single bail (found by listening: GO
  // sounded normal even in hard-xrun mode).  3x budget covers the lead
  // safely; the abandon poll ends the sleep ~50 us after the bail anyway.
  const int64_t budgetUs
    = (int64_t)m_SamplesPerBuffer * 1000000 / (int64_t)m_SampleRate;
  const auto until = std::chrono::steady_clock::now()
    + std::chrono::microseconds(budgetUs * 3);

  while (!abandon.load(std::memory_order_relaxed)
         && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(std::chrono::microseconds(50));
}
