/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDCONCEALMENT_H
#define GOSOUNDCONCEALMENT_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>



class GOSoundBuffer;
class GOSoundBufferMutable;

/**
 * Deadline-miss concealment - GrandOrgue's binding to libxorcist.
 *
 * The DSP lives in src/xorcist (a self-contained C library): a
 * phase-vocoder spectral freeze with an adaptive entry seam and a level
 * trend, developed and listening-tested here and then extracted, so the
 * engine and the offline lab cannot drift apart.  What remains in this
 * class is everything that is GrandOrgue's business and not the
 * algorithm's:
 *   - the buffer types the sound engine passes around,
 *   - the process-wide settings the Audio Monitor edits (method, bail
 *     reserve, forced-miss rate) and the counters it displays,
 *   - the forced-miss TEST MODE, which delays a real mix past its
 *     deadline so the genuine budget-watch/bail path runs.
 *
 * Threading (one instance per GOSoundOutputTask):
 * - SnapshotMix(): the thread that mixed the period, while it still holds
 *   the OutputTask mutex - a plain history-ring append.
 * - OpenShadow(): the mix epilog (still under the OutputTask mutex, a
 *   sequence bump plus one notify) - it WAKES the shadow thread instead
 *   of computing anything.
 * - the shadow THREADS: one per channel, owned by this instance, each at
 *   SCHED_FIFO 61 - above the render workers (60), below the audio
 *   callback (70) and PipeWire's data loops (88+).  The shadow is the
 *   safety net; the net is rigged before the acrobatics, so under full
 *   load it PREEMPTS workers for its runtime rather than starving - a
 *   net that only opens in fair weather protects nothing.  One thread
 *   per CHANNEL because one period is all there ever is: the epilog may
 *   land late in the period under load, and serially an order-4 shadow
 *   at 5.1 costs 1.46 ms of a 2.67 ms period - parallel it is the
 *   per-channel ~250 us, so the net stands even when the mix finishes
 *   at 80 % of the budget.  Scheduler tasks could not express any of
 *   this: the task list dispatches on "handed out", not on "finished",
 *   so the gap task fired early, did nothing, and NextPeriod()'s Exec()
 *   then ran the whole shadow ON THE CALLBACK THREAD - the green curve
 *   in the monitor was the shadow, one to one.
 * - Deliver()/DeliverBail(): the device's audio callback.
 */
class GOSoundConcealment {
public:
  /** Name of the FFTW wisdom cache inside the organ cache directory.
   * GOCacheCleaner knows this name so it does not report it as a stray
   * file (it is a derived, regenerable artifact and belongs there). */
  static constexpr const char *WISDOM_FILE_NAME = "fftw-wisdomf.dat";

  /**
   * @param wisdomPath  file the measured FFTW plans are cached in (may be
   *   null/empty to disable).  FFTW_MEASURE times the actual codelets on
   *   THIS machine, which is why it cannot be precomputed at build time -
   *   but its result can be persisted, so the ~0.9 s of measuring is paid
   *   once per machine instead of once per program start.
   */
  GOSoundConcealment(
    unsigned nChannels,
    unsigned samplesPerBuffer,
    unsigned sampleRate,
    const char *wisdomPath = nullptr);
  ~GOSoundConcealment();

  GOSoundConcealment(const GOSoundConcealment &) = delete;
  GOSoundConcealment &operator=(const GOSoundConcealment &) = delete;

  /** Mixer thread, OutputTask mutex held: append the finished mix (
   * interleaved, one period) to the per-channel history rings. */
  void SnapshotMix(const float *pInterleaved);

  /** Same thread, mutex released: predict the next period into the inactive
   * shadow buffer and publish it. */
  /** Open this period's prediction and wake the channel threads.  Call
   * ONCE per period from the mix epilog, right after the history
   * snapshot: predictBegin (a handful of atomics), arm the channel
   * counter, bump the sequence, notify - nothing heavier runs on the
   * caller.  Each thread computes its own channel, the last one
   * publishes.  If the previous round is still in flight, predictBegin
   * skips (the library's busy guard), the sequence is NOT bumped and the
   * period is simply lost to the slice mechanism - no abort protocol,
   * nothing waits anywhere.
   *
   * byAudioThread: Diagnose-Etikett - lief dieser Epilog auf dem
   * Callback-Thread (TryHelpMix-Gewinn) statt auf einem Worker?
   */
  void OpenShadow(bool byAudioThread);

  /** WALL microseconds from the epilog's OpenShadow() to the moment the
   * last channel thread published, and the maximum since the last read.
   * This is the answer to "when does the net stand", which is what the
   * one-period budget is spent on; the speculative prediction runs every
   * period, misses or none, so it gets its own meter instead of hiding
   * in the callback curve. */
  static int64_t GetShadowLastUs() {
    return s_ShadowLastUs.load(std::memory_order_relaxed);
  }
  static int64_t FetchShadowMaxUs() {
    return s_ShadowMaxUs.exchange(0, std::memory_order_relaxed);
  }

  /** The AR order as the LIBRARY reports it, read back after every sync
   * (i.e. every period the shadow is active).  This is the proof the
   * monitor shows next to the spin control's wish: if the two differ, the
   * plumbing is broken; 0 means the shadow has never synced at all. */
  static int GetEffectiveOrder() {
    return s_EffectiveOrder.load(std::memory_order_relaxed);
  }


  /** Audio callback, normal delivery: copy the real buffer into outBuffer,
   * applying the re-entry crossfade if the previous period was concealed.
   * The shadow published by the shadow thread is the
   * correct one for THIS period by construction - no latching needed. */
  void Deliver(GOSoundBufferMutable &outBuffer, const GOSoundBuffer &real);

  /** Audio callback, budget missed (real or injected): deliver the shadow
   * (PV modes) or hard silence (mode off / no shadow / degraded), handle
   * the consecutive-miss streak, and self-feed the ring with what was
   * actually played so the next prediction stays time-aligned. */
  void DeliverBail(GOSoundBufferMutable &outBuffer);

  /** Mixer thread (mutex held), start of the mix: test mode - with
   * probability from the configured rate, sleep past the period budget so
   * the mix is GENUINELY late and the real bail path runs.  Polls abandon
   * so the callback's bail releases the mutex within ~50 us. */
  void MaybeInjectDelay(const std::atomic_bool &abandon);

  // ---- forced-xrun test mode (Audio Monitor) ----
  static void SetForcedXrunsPerMinute(int v) {
    s_ForcedPerMinute.store(v, std::memory_order_relaxed);
  }
  static int GetForcedXrunsPerMinute() {
    return s_ForcedPerMinute.load(std::memory_order_relaxed);
  }
  static bool IsArmed() { return GetForcedXrunsPerMinute() > 0; }

  /** Injection pattern for the forced-miss test.
   * SINGLE: independent dice per period (natural clusters included).
   * PAIR: every miss spans TWO consecutive periods, pairs never follow
   *   each other directly - previews frame-skipping after a late buffer.
   * ALTERNATE: strictly every second period is missed, for as long as
   *   the rate spin is armed - the sustained 1:1 limit case (50 %
   *   synthetic signal), measured steady state ~-7 dB on a held chord.
   *   The rate value is ignored in this mode; > 0 just arms it. */
  static constexpr int MISS_SINGLE = 0;
  static constexpr int MISS_PAIR = 1;
  static constexpr int MISS_ALTERNATE = 2;
  static void SetMissPattern(int v) {
    s_MissPattern.store(v, std::memory_order_relaxed);
  }
  static int GetMissPattern() {
    return s_MissPattern.load(std::memory_order_relaxed);
  }
  static uint64_t GetForcedCount() {
    return s_ForcedCount.load(std::memory_order_relaxed);
  }
  static uint64_t GetShadowSkipCount() {
    return s_SkipCount.load(std::memory_order_relaxed);
  }
  /** Davon nachgeholt: Rundenstarts, die am Besetzt-Waechter scheiterten
   * und direkt nach predictEnd doch noch liefen.  Skips und Catch-ups im
   * Gleichschritt heisst: kein Anker-Rueckstand ueber eine Rundenlaufzeit
   * hinaus. */
  static uint64_t GetCatchupCount() {
    return s_CatchupCount.load(std::memory_order_relaxed);
  }

  /** Nebenlaeufigkeits-Nachweis (09.08.2026).  Der Verdacht war, dass der
   * Rueckruf mit seiner Selbstfuetterung in eine laufende Schattenrunde
   * hineinschreibt - im einthreadigen Labor unmoeglich.  Gezaehlt statt
   * gestritten, und die Zaehler liefen sofort: der Verdacht stimmte.
   *
   * Seit dem Einfrieren des Rundenfensters (10.08.2026, m_RoundHead /
   * RoundPeriod in xorcist.cpp) ist es KEIN Schaden mehr - die Runde
   * rechnet mit ihrem eigenen Fenster zu Ende und wird veroeffentlicht,
   * statt verworfen zu werden.  Die Zahlen messen jetzt nur noch, wie
   * stark sich Mix, Rueckruf und Schattenrunden ueberlappen; eine
   * steigende "late slices" ist der Hinweis, dass die Rundenlaufzeit an
   * die Periodendauer stoesst. */
  static uint64_t GetStaleDiscards() {
    return s_StaleDiscards.load(std::memory_order_relaxed);
  }
  static uint64_t GetLateSlices() {
    return s_LateSlices.load(std::memory_order_relaxed);
  }

  /** Zuendungen des Transientendetektors, gesamt seit Start.  Auf
   * gehaltenem Klang gehoert hier eine Null hin - das Kriterium ist ein
   * reines Verhaeltnis und damit pegelunabhaengig (ueber 100 dB
   * nachgemessen).  Was ihn bei hoher Lueckendichte trotzdem taeuscht,
   * sind die eigenen Wiedereinstiege. */
  static uint64_t GetOnsetCount() {
    return s_OnsetCount.load(std::memory_order_relaxed);
  }
  /** Seit dem letzten Aufruf - fuer die Marken im Verlaufsgraphen. */
  static uint32_t FetchOnsetDelta() {
    return s_OnsetDelta.exchange(0, std::memory_order_relaxed);
  }

  /** Delivery-path telemetry (audio thread; shown in the Audio Monitor so
   * "sounds fine" claims can be checked against what actually happened):
   * how often the callback had to WAIT at all, how often the real mix
   * still made it before the bail deadline (late-real), and the longest
   * wait seen. */
  static void RecordDeliveryWait(int64_t waitedUs, bool bailed) {
    if (!bailed)
      s_LateRealCount.fetch_add(1, std::memory_order_relaxed);
    for (int64_t old = s_MaxWaitUs.load(std::memory_order_relaxed);
         waitedUs > old
         && !s_MaxWaitUs.compare_exchange_weak(
           old, waitedUs, std::memory_order_relaxed);)
      ;
  }
  static uint64_t GetLateRealCount() {
    return s_LateRealCount.load(std::memory_order_relaxed);
  }
  static int64_t GetMaxWaitUs() {
    return s_MaxWaitUs.load(std::memory_order_relaxed);
  }

  // Concealment method (Audio Monitor dropdown): 0 = off (forced misses
  // play HARD SILENCE - the demo of what an unconcealed xrun sounds
  // like), 1/2/3 = phase-vocoder freeze with NFFT 2048/4096/8192.  4096
  // was the listening/NMR sweet spot on dense chords (23 Hz bins at 2048
  // merge neighbouring partials into one bin frequency; 8192 goes stale
  // over its 170 ms span) - switchable to hear exactly that.
  static constexpr int MODE_OFF = 0;
  static constexpr int MODE_PV_2K = 1;
  static constexpr int MODE_PV_4K = 2;
  static constexpr int MODE_PV_8K = 3;
  // 4 ist ANGEHAENGT, nicht einsortiert: 0..3 stecken in gespeicherten
  // Einstellungen.  1024er Rahmen = 21 ms, 47 Hz Bins - zu grob, um die
  // Teiltoene eines Akkords zu trennen, dafuer die vierfache
  // Zeitaufloesung.  Das ist genau die kurze Kette, die der Hybrid nur
  // waehrend eines Anschlags einblendet; hier laeuft sie als EINZIGE
  // Kette, ohne zweite Analyse und ohne Detektor.  Gedacht fuer schwache
  // Maschinen, die nur einzelne Aussetzer unhoerbar machen wollen.
  // BRAUCHT einen Puffer, der den Analyse-Hop nfft/4 = 256 teilt (128 oder
  // 256).  Die Bibliothek prueft das NICHT - sie wuerde stillschweigend
  // Unsinn rechnen -, also klemmt SyncMode() den Modus ab und meldet ueber
  // GetEffectiveMode(), was wirklich laeuft.  Stille Wirkungslosigkeit war
  // in diesem Projekt schon zu oft die Fehlerursache.
  static constexpr int MODE_PV_1K = 4;

  /** Residuen-Tor des Transientendetektors: ein Band zaehlt nur als
   * steigend, wenn sein eigener Vorhersagefehler darueber liegt.  0 = aus.
   * Trennt Schwebung und Ausklang von echten Anschlaegen - was der Pegel
   * nicht kann, weil beide im selben Pegelbereich wohnen. */
  static float GetDetResGate() {
    return s_DetResGate.load(std::memory_order_relaxed);
  }
  static void SetDetResGate(float v) {
    s_DetResGate.store(v, std::memory_order_relaxed);
  }

  /** Hindcast-Anker: Nahtversatz als Schatten[-1] gegen das letzte echte
   * Sample MESSEN statt ihn per Burg vorherzusagen.  Gemessen 12.08.:
   * ohne Hybrid identisch, mit Hybrid Einsaetze identisch und 0,14 dB
   * Rest im Plateau; spart den Burg-Fit (4,3 us je Runde).  Vorerst
   * Hoertest-Schalter, Vorgabe AUS (= Burg). */
  static bool GetV0Hindcast() {
    return s_V0Hindcast.load(std::memory_order_relaxed);
  }
  static void SetV0Hindcast(bool v) {
    s_V0Hindcast.store(v, std::memory_order_relaxed);
  }

  /** Wiedereinstiegs-Blende (Vorhersage -> echtes Signal) in Samples.
   * 0 = Auto: min(Pufferlaenge, 3 ms) - die Lab-Vorgabe, bei 128er-
   * Puffern also 128 Samples = 2,67 ms.  Werte oberhalb der Auto-Laenge
   * klemmt die Bibliothek (die Blende muss in die erste echte Periode
   * passen und die Puffer sind darauf dimensioniert); kuerzer ist frei
   * waehlbar, die Messreihe im Lab lief bis 0,2 ms hinab. */
  static void SetReentryFade(int samples) {
    s_ReentryFade.store(samples, std::memory_order_relaxed);
  }
  static int GetReentryFade() {
    return s_ReentryFade.load(std::memory_order_relaxed);
  }

  /** Der Modus, den die Bibliothek nach der Puffer-Pruefung wirklich
   * faehrt - kann von GetMode() abweichen. */
  static int GetEffectiveMode() {
    return s_EffectiveMode.load(std::memory_order_relaxed);
  }
  static void SetMode(int m) { s_Mode.store(m, std::memory_order_relaxed); }
  static int GetMode() { return s_Mode.load(std::memory_order_relaxed); }
  /** Shadow pipeline (ring snapshots + gap-task prediction) active? */
  static bool IsShadowActive() { return GetMode() != MODE_OFF; }

  /** Damping of the predictor's frequency estimate (0 = off, 1 = the
   * frequencies are frozen after the first analysis).  Attacks the
   * positive feedback that appears once the history contains the
   * predictor's own output: a sluggish frequency estimate cannot re-fit
   * its own error.  It only bites where that estimate actually
   * fluctuates - measured at 50 % concealment, PV 4k: a held real organ
   * tone gains +0.1 dB (nothing), the Vierne finale +1.5, a heavily
   * detuned synthetic chord +2.9. */
  /** Order of the per-bin continuation: 1 = the classic phase-vocoder
   * rotator, 2 = per-bin Prony.  The rotator is an order-1 recursion and
   * is EXACT for one partial per bin; a chord puts several into the same
   * bin, where the model is structurally wrong rather than merely
   * imprecise.  Order 2 fits X(n) = a X(n-1) + b X(n-2) from
   * exponentially forgotten normal equations and runs on its own analysis
   * hop of nfft/8 - the largest hop whose root separation still resolves
   * the widest pair one bin can hold.
   *
   * Measured in the engine at PV 4k, 600 misses/min, order 1 -> 4:
   * single organ tone -15.7 -> -25.3 dB, chord buildup with attacks
   * -13.8 -> -25.2, full D major chord -4.9 -> -16.5, dense Vierne
   * -2.4 -> -3.5.  Order 6 collapses into overfitting, so 4 is the
   * ceiling.  Costs 64 / 112 / 166 / 214 us per channel and period for
   * orders 1..4 - every period, because the shadow is speculative and
   * the work cannot wait for a miss. */
  /** Seam repair: 0 = adaptive (density picks), 1 = anchor, 2 = bridge.
   * The density discriminator was calibrated for the ORDER-1 rotator.
   * At order 4 the anchor is so good that blending in even 13 % of the
   * Burg bridge costs 1.0 dB on a dense chord - measured 600/min, PV 4k:
   * -16.5 adaptive vs -17.5 forced anchor. */
  static void SetSeam(int v) { s_Seam.store(v, std::memory_order_relaxed); }
  static int GetSeam() { return s_Seam.load(std::memory_order_relaxed); }

  /** Freeze the level instead of continuing its trend.
   *
   * The global level trend exists because the order-1 rotator has
   * unit-modulus roots and therefore CONSTANT amplitude - without it a
   * swell dies in the gap.  A higher-order recursion does not need that
   * crutch: a linearly growing sinusoid satisfies an order-4 recursion
   * exactly (double root on the unit circle), so at order 4 the trend
   * double-counts and actively destroys what the model got right -
   * measured on a crescendo at 600/min: -127.6 dB with the trend OFF
   * against -40.4 with it on, and +7.0 dB on a dense chord together with
   * the forced anchor.
   *
   * But it cannot simply be switched off: at 27 % concealment the
   * self-fed history has LOST the level development, and there the trend
   * is worth 36-39 dB on the same crescendo at every order, order 4
   * included.  Hence a switch and no default change - the right answer
   * is presumably a trend whose strength follows both the order and the
   * concealment density, and that needs measuring, not guessing. */
  static void SetNoTrend(bool v) {
    s_NoTrend.store(v, std::memory_order_relaxed);
  }
  static bool GetNoTrend() { return s_NoTrend.load(std::memory_order_relaxed); }

  static void SetOrder(int v) {
    // 8 statt 4: GOs libxorcist baut mit XO_AR_ORDER_MAX 8, und das
    // Siegermodell der Lab-Messreihen (08.08.) ist 2k mit Ordnung 8.
    s_Order.store(v < 1 ? 1 : (v > 8 ? 8 : v), std::memory_order_relaxed);
  }
  static int GetOrder() { return s_Order.load(std::memory_order_relaxed); }

  /** ZWEI-KETTEN-HYBRID (Lab-Siegermodell 08.08.): eine kurze 1k-Kette
   * laeuft dauerhaft warm mit; erkennt der 4k-Detektor einen Anschlag,
   * spielen die betroffenen Baender die kurze Kette und gehen nach
   * GetHybHandOrder Rasterpunkten an die lange zurueck.  Bei 2k-Basis
   * und Ordnung 8: Einsatz-Luecken -12.1 statt -10.8 dB NMR, Schwebung
   * unveraendert.  Kostet dauerhaft (~2x der reinen Kette, CPU-Messung
   * VM: 176 -> 326 us/Periode bei 2 Kanaelen).  Braucht eine
   * Puffergroesse, die 256 teilt - bei 512+ schaltet die Bibliothek
   * ihn STILL ab. */
  static void SetHybrid(bool v) {
    s_Hybrid.store(v, std::memory_order_relaxed);
  }
  static bool GetHybrid() { return s_Hybrid.load(std::memory_order_relaxed); }

  /** Beim Einsatz ALLE Baender schalten statt nur der steigenden - die
   * Teilauswahl schneidet Teiltoene zwischen den Ketten durch
   * (gemessen +16 dB NMR im Haltetest). */
  static void SetHybAllBands(bool v) {
    s_HybAllBands.store(v, std::memory_order_relaxed);
  }
  static bool GetHybAllBands() {
    return s_HybAllBands.load(std::memory_order_relaxed);
  }

  /** Rueckgabe an die lange Kette (X+2) Rasterpunkte nach dem Anschlag;
   * bei 2k ist ein Rasterpunkt 2 Perioden.  8 = 53 ms. */
  static void SetHybHandOrder(int v) {
    s_HybHandOrder.store(
      v < 1 ? 1 : (v > 40 ? 40 : v), std::memory_order_relaxed);
  }
  static int GetHybHandOrder() {
    return s_HybHandOrder.load(std::memory_order_relaxed);
  }

  /** Deckelordnung der kurzen 1k-Kette (Vorgabe 6).  Unter 4 leidet
   * der Einsatz IN stehenden Klang: alter Ton + neuer Anschlag brauchen
   * zusammen mehr als zwei Wurzeln (gemessen -5.5 gegen -13.3 dB). */
  static void SetHybShortCap(int v) {
    s_HybShortCap.store(
      v < 1 ? 1 : (v > 8 ? 8 : v), std::memory_order_relaxed);
  }
  static int GetHybShortCap() {
    return s_HybShortCap.load(std::memory_order_relaxed);
  }

  /** Sicherheits-Cap: Schatten nie lauter als juengste Historie x Cap.
   * 1.2 = Lab-Stand; noch nicht endgueltig durchgetestet. */
  static void SetSafetyCap(float v) {
    s_SafetyCap.store(v < 0.0f ? 0.0f : v, std::memory_order_relaxed);
  }
  static float GetSafetyCap() {
    return s_SafetyCap.load(std::memory_order_relaxed);
  }

  /** Zeitkonstante des Naht-Ankers in Samples; 0 = eingebaut (Rate/3000,
   * 16 bei 48 kHz).  Breitbandig - tau tauscht Bandbreite gegen Dauer. */
  static void SetAnchorTau(int v) {
    s_AnchorTau.store(
      v < 0 ? 0 : (v > 512 ? 512 : v), std::memory_order_relaxed);
  }
  static int GetAnchorTau() {
    return s_AnchorTau.load(std::memory_order_relaxed);
  }

  /** Ausloeseschwelle der Wurzelkappung (Ziel immer 1.0).  1.0 =
   * Lab-Stand (nur echte Divergenz); 0.90 war der alte Einbau, der
   * Transienten-Moden zu ewigen Oszillatoren einfror.  Ab Ordnung 2
   * nur wirksam, wenn rho = 0 (sonst deckelt der Detektor-Modus). */
  static void SetRootClamp(float v) {
    s_RootClamp.store(
      v < 0.0f ? 0.0f : (v > 1.2f ? 1.2f : v), std::memory_order_relaxed);
  }
  static float GetRootClamp() {
    return s_RootClamp.load(std::memory_order_relaxed);
  }

  /** Detektor-Modus: rho-Schur-Test auf den rohen Koeffizienten;
   * bestanden = roh (Huellkurven bleiben), sonst Deckeln auf rho.
   * 1.02 = Lab-Stand; 0 = klassischer Pfad (z-Kappe). */
  static void SetDetectRho(float v) {
    s_DetectRho.store(
      v < 0.0f ? 0.0f : (v > 1.2f ? 1.2f : v), std::memory_order_relaxed);
  }
  static float GetDetectRho() {
    return s_DetectRho.load(std::memory_order_relaxed);
  }

  static void SetFreqDamping(float v) {
    s_FreqDamping.store(v, std::memory_order_relaxed);
  }
  static float GetFreqDamping() {
    return s_FreqDamping.load(std::memory_order_relaxed);
  }

  /** Let a straggling mix run to COMPLETION after a bail and feed its
   * result into the history in place of the prediction.  Its audio is
   * useless for playback (the slot is gone) but it is the truth for the
   * analysis, which removes the feedback at its source: -10.8 -> -15.1 dB
   * measured.  Costs the render time the bail would otherwise have saved,
   * so it is a quality/throughput trade, not a free win. */
  static void SetStragglerTruth(bool v) {
    s_StragglerTruth.store(v, std::memory_order_relaxed);
  }
  static bool GetStragglerTruth() {
    return s_StragglerTruth.load(std::memory_order_relaxed);
  }

  /** Mixer thread: the late mix finished after its period was already
   * concealed - hand its output to the history instead of appending. */
  void ReplaceConcealedWithReal(const float *pInterleaved);

  /** Mix grace, als Prozent der Periode (0..100): wie lange der Rueckruf
   * einen verspaeteten Mix duldet, bevor er die Verdeckung liefert.
   * bailAt = Eintritt + Periode * grace / 100.
   *
   * Bis 12.08.2026 hiess dieselbe Frist "Reserve" und war vom FALSCHEN
   * Ende der Zeitachse benannt (Abstand zum naechsten Rueckruf, in us):
   * die Vorgabe 200 us klang wie ein Sicherheitsabstand, bedeutete aber
   * "dulde bis zu 93 % der Periode Phasenversatz".  Denn was der Rueckruf
   * an Geduld verbraucht, fehlt der NAECHSTEN Periode als Vorlauf -
   * NextPeriod/WakeupThreads laufen erst nach der Wartschleife, der
   * naechste Mix und dessen Schattenrunde starten also erst beim
   * Rueckruf-Ende (Ursachenanalyse 12.08., Spaete-Scheiben-Papier).
   *
   * Dimensionierung: der verbleibende Vorlauf (100 - grace) muss
   * max(Zustellkette, Mixdauer, Rundenlaufzeit) tragen.  Vorgabe 25 %
   * (Nutzerentscheid 12.08.): ~670 us Geduld reichen fuer echte
   * Nachzuegler, und die Folgeperiode behaelt ~2000 us Vorlauf - Luft
   * selbst fuer 2k/Ordnung 8 (Runde bis 813 us) PLUS Mix.  Ein trotzdem
   * verpasster Rundenstart wird nachgeholt.
   *
   * 100 = LEGACY-Zustellung (blockierendes Finish inkl. Rueckruf-Hilfe,
   * keine Verdeckung) - die A/B-Referenz der Ceiling-Benchmarks. */
  static void SetMixGracePct(int pct) {
    s_MixGracePct.store(pct, std::memory_order_relaxed);
  }
  static int GetMixGracePct() {
    return s_MixGracePct.load(std::memory_order_relaxed);
  }

private:
  // Process-wide because one dialog drives every device.  The library
  // keeps these per instance; the wrapper pushes the current value in.
  static std::atomic<int> s_ForcedPerMinute;
  static std::atomic<int> s_MissPattern;
  static std::atomic<int> s_Mode;
  static std::atomic<int> s_MixGracePct;
  static std::atomic<float> s_FreqDamping;
  static std::atomic<int> s_Order;
  static std::atomic<bool> s_Hybrid;
  static std::atomic<bool> s_HybAllBands;
  static std::atomic<int> s_HybHandOrder;
  static std::atomic<int> s_HybShortCap;
  static std::atomic<float> s_SafetyCap;
  static std::atomic<int> s_AnchorTau;
  static std::atomic<float> s_RootClamp;
  static std::atomic<float> s_DetectRho;
  static std::atomic<int> s_Seam;
  static std::atomic<bool> s_NoTrend;
  static std::atomic<int64_t> s_ShadowLastUs;
  static std::atomic<int64_t> s_ShadowMaxUs;
  static std::atomic<int> s_EffectiveOrder;
  static std::atomic<int> s_EffectiveMode;
  void TryOpenRound(bool byAudioThread, bool isRetry = false);
  // Moeglichkeit 3 der Spaete-Scheiben-Analyse (12.08.): ein am
  // Besetzt-Waechter gescheiterter Rundenstart wird vorgemerkt und vom
  // zuletzt fertigen Kanalthread direkt nach predictEnd nachgeholt -
  // der Anker haengt dann hoechstens eine Rundenlaufzeit zurueck statt
  // einer Periode.  Noetig als Auffangnetz, wenn jemand die Mix grace
  // so gross stellt, dass der Vorlauf unter die Rundenlaufzeit faellt.
  std::atomic<bool> m_OpenPending{false};

  static std::atomic<float> s_DetResGate;
  static std::atomic<float> s_DetResDecay;
  static std::atomic<bool> s_V0Hindcast;
  static std::atomic<int> s_ReentryFade;
  // Shadow threads, one per channel: woken by OpenShadow(), exit when
  // m_ShadowRun clears.  m_ShadowSeq counts successfully OPENED rounds;
  // each thread keeps its own done-counter on its stack, so after a
  // successful open every thread runs exactly once - a second open
  // cannot happen before the last channel called predictEnd, because
  // until then the library's busy guard makes predictBegin skip.
  std::vector<std::thread> m_ShadowThreads;
  std::mutex m_ShadowMutex;
  std::condition_variable m_ShadowCv;
  uint64_t m_ShadowSeq = 0;
  bool m_ShadowRun = true;
  std::atomic<int> m_ShadowOutstanding{0};
  std::chrono::steady_clock::time_point m_ShadowT0;
  void ShadowThreadMain(unsigned channel);
  static std::atomic<bool> s_StragglerTruth;
  static std::atomic<uint64_t> s_ForcedCount;   // bails delivered
  static std::atomic<uint64_t> s_SkipCount;     // shadow updates skipped (busy)
  static std::atomic<uint64_t> s_CatchupCount;  // skipped starts caught up
  // Aus der Bibliothek eingesammelt (deren Zaehler setzen beim Lesen
  // zurueck), damit der Monitor Gesamtzahlen zeigt wie alle anderen.
  static std::atomic<uint64_t> s_StaleDiscards; // Runden wegen Ringlauf verworfen
  static std::atomic<uint64_t> s_LateSlices;    // Scheibe > 0 ohne Fehlschlagkette
  static std::atomic<uint64_t> s_OnsetCount;    // Detektorzuendungen gesamt
  static std::atomic<uint32_t> s_OnsetDelta;    // seit dem letzten Graphen-Tick
  static std::atomic<uint64_t> s_LateRealCount; // real mix arrived after wait
  static std::atomic<int64_t> s_MaxWaitUs;      // longest delivery wait

  // ---- Spaete-Scheiben-Diagnose (11.08.2026, reine Messung) ----------
  // Hypothese: nach einem Bail endet der Rueckruf erst an der Frist, und
  // NextPeriod (GOSoundSystem: GetAudioOutput steht davor) startet den
  // naechsten Mix deshalb erst am Periodenanfang.  Dessen Epilog-Oeffnung
  // und die des Folge-Mixes liegen dann nur noch etwa die RESERVE
  // auseinander; ist die Runde laenger als die Reserve, trifft die zweite
  // Oeffnung auf "besetzt" und faellt aus - der Anker bleibt eine Periode
  // zurueck.  Die Histogramme unten entscheiden das: klumpen die
  // Besetzt-Abstaende bei der Reserve und folgen sie auf Bails, stimmt
  // die Kette; tun sie es nicht, ist die Ursache eine andere.
public:
  // 7 Toepfe, unten fein aufgeloest: die 1k-Runde dauert ~105 us, und ob
  // Kollisionen DARUNTER liegen, entscheidet zwischen "Reserve-Abstand
  // trifft lange Runde" und "Oeffnungspaare enger als die Reserve".
  // Die Statistik setzt sich bei Modus-/Ordnungswechsel selbst zurueck -
  // kumulative Toepfe ueber eine Umschaltung hinweg sind unlesbar
  // (User-Einwand 11.08.), und phasenrein ist der Vergleich erst damit.
  static constexpr int TIMING_BUCKETS = 7;
  struct TimingStats {
    uint64_t openGap[TIMING_BUCKETS];
    uint64_t skipGap[TIMING_BUCKETS];
    uint64_t roundDur[TIMING_BUCKETS];
    uint64_t compPostBail[TIMING_BUCKETS]; // Mix-Fertigstellung nach Bail
    uint64_t compNormal[TIMING_BUCKETS];   // dito nach normalem Pass
    uint64_t skipAfterBail;
    uint64_t lateAfterSkip;
    uint64_t lateSliceMax;
    uint64_t phaseLateSlices; // late slices seit dem letzten Reset
    uint64_t openByAudio;     // Epiloge, die der Audio-Thread lief
    uint64_t skipPrevAudio;   // Besetzt-Faelle direkt nach so einem
  };
  static TimingStats GetTimingStats();

  /** Vom Callback-Thread (NextPeriod): Fertigstellungslatenz der soeben
   * gelieferten Periode, WakeupThreads bis Output-Task fertig.  Wird nach
   * "Vorlieferung war Bail" vs. "war Pass" getrennt - die Weckasymmetrie
   * der Worker (lang geparkt nach Bail, heiss nach Pass) ist der
   * Hauptverdaechtige fuer die engen Epilogpaare. */
  static void RecordMixCompletion(int64_t us);

private:
  static void ResetTimingStats();
  static std::atomic<uint32_t> s_TimingPhaseKey; // (mode<<8)|order
  std::atomic<int64_t> m_LastAttemptUs{-1}; // letzter Epilog (steady, us)
  std::atomic<int64_t> m_LastBailUs{-1};    // letzter Bail (steady, us)
  std::atomic<bool> m_LastAttemptAudio{false}; // lief er auf dem Audio-Thread?
  static std::atomic<uint64_t> s_OpenGapHist[TIMING_BUCKETS]; // Epilogabstand
  static std::atomic<uint64_t> s_SkipGapHist[TIMING_BUCKETS]; // nur Besetzt
  static std::atomic<uint64_t> s_RoundDurHist[TIMING_BUCKETS]; // Rundendauer
  static std::atomic<uint64_t> s_CompPostBailHist[TIMING_BUCKETS];
  static std::atomic<uint64_t> s_CompNormalHist[TIMING_BUCKETS];
  static std::atomic<uint64_t> s_SkipAfterBail; // Besetzt binnen Periode nach Bail
  static std::atomic<uint64_t> s_LateAfterSkip; // aus der Bibliothek gesammelt
  static std::atomic<uint64_t> s_LateSliceMax;  // groesste spaete Scheibe
  static std::atomic<uint64_t> s_PhaseLateSlices; // late slices der Phase
  static std::atomic<uint64_t> s_OpenByAudio;   // Epiloge auf dem Audio-Thread
  static std::atomic<uint64_t> s_SkipPrevAudio; // Besetzt direkt danach
  // Lieferungs-Schieberegister (nur Audio-Thread, eine Instanz je Geraet):
  // s_PrevPrevDeliveryBail gehoert zur Lieferung VOR der aktuellen - genau
  // der Kontext, in dem der Mix der aktuellen Lieferung geweckt wurde.
  static std::atomic<bool> s_PrevDeliveryBail;
  static std::atomic<bool> s_PrevPrevDeliveryBail;

  struct Xorcist *m_X;
  const unsigned m_NChannels;
  const unsigned m_SamplesPerBuffer;
  const unsigned m_SampleRate;

  uint64_t m_RandState; // xorshift64*, forced-miss dice
  unsigned m_InjectRemaining; // periods this injection still spans
  unsigned m_InjectCooldown;  // periods to wait before the next dice roll
  bool m_AltPhase;     // ALTERNATE: which half of the current pair we are in
  bool m_AltSkipFirst; // ALTERNATE: coin toss - miss the first or the second
  double NextRand();

  /** Push the dialog's current method into the instance. */
  void SyncMode();
};

#endif
