#!/usr/bin/env python3
"""
GrandOrgue LUT Analysis Tool
Analysiert die Korrelations-LUT-Qualität für alle Pfeifen eines Sample-Sets.

Erfordert: Python 3.8+, tkinter, matplotlib, numpy
Aufruf:    python3 analyze_lut.py [pfad/zur/orgel.organ]
"""

import os
import re
import sys
import struct
import threading
import concurrent.futures
import os as _os
import queue
import math
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from dataclasses import dataclass, field
from typing import Optional
import numpy as np

TOOL_VERSION = "v146-go-default-crossfade"

# v115: Exhaustive DP debug disabled by default; it was useful for diagnosis
# but is too expensive for full-set scans.
ENABLE_EXHAUSTIVE_DP_DEBUG = False

try:
    import matplotlib
    matplotlib.use("TkAgg")
    import matplotlib.pyplot as plt
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
    from matplotlib.figure import Figure
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False


# ─── Konstanten (aus GOSoundReleaseAlignTable.cpp) ────────────────────────────
DENSE_STEP  = 6
MAX_DENSE_N = 100
STABLE_WIN  = 4
N_SPARSE    = 10
MAX_TOTAL   = 64
# Removed: CORR_MIXTURE_HARMONIC_THRESHOLD = 48
# Now using CorrIsOctaveStop logic: only power-of-2 HarmonicNumbers are pure
# octave stops (8=8', 16=4', 32=2', 4=16', ...) and skip autocorrelation.
# Non-power-of-2 (24=2⅔', 40=1⅗', 48=1⅓', mixtures) always use autocorr.
def corr_is_octave_stop(harmonic_number: int) -> bool:
    return harmonic_number > 0 and (harmonic_number & (harmonic_number - 1)) == 0
SCORE_WARN  = 0.35  # Warnung erst bei deutlich schwacher, aber nicht katastrophaler Korrelation
SCORE_BAD   = 0.25
SCORE_LOW_FRACTION_WARN = 0.30  # Warnung nur, wenn relevante Minderheit schwacher Qualitaetspunkte betroffen ist

# v112: Exportiert zusaetzlich den ungeprunten finalen Viterbi-DP-Pfad
#       direkt nach dem Backtracking, vor Rebuild/Folding/Pruning.
#       Damit laesst sich pruefen, ob der korrekte Pfad wirklich im DP verloren
#       hat oder nur der Debug-/LUT-Export Zustandsraeume vermischt.
# v114: Kink-Penalty gegen Gap-Jitter abgesichert. Gap-nahe Kinks werden
#       skaliert (BRANCH_KINK_GAP_SCALE) und danach gedeckelt
#       (BRANCH_KINK_PENALTY_CAP), damit dn=1-Rauschen keinen ganzen Ast tötet.
# v103: DP-Full-Trace: Pro Kandidat wird minimaler globaler DP-Zustand gespeichert.
#       Neue CSV-Felder: cost_prev (akkum. Vorgaengerpfadkosten), total_cost
#       (= cost_prev + local_step_cost, das was DP wirklich minimiert hat),
#       prev_raw_r/prev_track_r (welcher Vorgaenger optimal war), is_final_path.
#       Damit sieht man ob raw=35 lokal billiger war (Penalty-Problem) oder ob
#       sein bester Vorgaengerpfad teurer war (Pfadhistorie-Problem). Kein
#       Algorithmus geaendert, nur Tracing.
# v111: Midpoint-Kohärenz raw-space-korrekt: innerhalb [0,r_max) keine T-Copy im Midpoint-Test; Debug-Felder erweitert.
# v109: Midpoint-Kohärenz-Penalty im globalen Viterbi. Für jeden DP-Übergang
#       (pi-1 → pi) wird geprüft ob der Midpoint-Frame (n zwischen den beiden)
#       einen Peak in der Nähe der interpolierten Track-Position hat. Ist der
#       nächste Peak weit vom erwarteten Midpoint entfernt, bekommt der Übergang
#       eine Penalty proportional zu (Abstand/allowed)^2. Ersetzt v104's hartes
#       Blockieren; legitime T-Copies (Midpoint kohärent) werden kaum bestraft.
# v99: Branch-Debug: globale DP speichert pro LUT-Punkt Kandidaten-Diagnose
#      (raw_r, score, track_r, pred, err, penalties). Korrelationslandschaft
#      kann interne Kandidaten und gewaehlten raw_r ueberlagern; Debug-CSV-Export.
# v98: Korrelationslandschaft nutzt jetzt dieselbe NDP-Fensterlaenge wie compute_lut():
#      window_len = crossfade_len_samples. 2*T/r_search_max ist nur das r-Suchfenster,
#      nicht die NDP-Laenge. Dadurch stimmen Heatmap-Farben und LUT-Scores wieder ueberein.
# v97: Raw-r Branch-Switch-Penalty im globalen Viterbi. closest_branch_copy machte
#      T-Kopien im Track-Raum kostenfrei; Spruenge im rohen r-Raum > T/2 werden
#      jetzt fuer alle Pfeifen bestraft, auch in den Startpaaren.
# v95: Erklaerungstext pro Release (status_text Property). Weniger falsche Warnungen:
#      score_p10 statt score_min, phase3_count-Bedingung entfernt, stable_at_n>60→>100.
#      Status-Spalte auf 400px verbreitert, linksbündig.
# v94: v90-v93 revertiert. closest_branch_copy in _global_branch_path wiederhergestellt.
#      Natuerliche T-Uebergaenge (Drift durch T-Grenze) kostenlos; artificielle Spruenge
#      werden durch smooth_pen+kink_pen bestraft. Stabiles Verhalten fuer alle Pfeifen.
# v93: Dreistufiger Ansatz. Stufe3: Copy-DP ueber copy_id=raw_r//T verhindert
#      Alternieren (54→5→54) wenn Scores zwischen T-Kopien schwanken.
#      BRANCH_COPY_SWITCH_PENALTY=0.20; bester konsistenter T-Copy gewaehlt.
# v92: Zweistufen-zirkulaerer DP. Stufe1: DP in [0,T) Phasenraum (kein T-Fenster-
#      Alternieren). Stufe2: phase_r→raw_r mit bestem Score (korrekte Kopplung).
#      Loest v91-Problem (freies Alternieren zwischen r und r+T).
# v91: v90-Fix: Kandidaten bleiben bei raw_r (Score/raw_r-Kopplung korrekt).
#      Nur DP-Kosten zirkulaer: err=circ_delta(pred%T, raw_r%T, T),
#      slope=circ_delta(prev%T, cur%T, T)/dn. Fold bleibt _try_fold()-Entscheidung.
# v90: _global_branch_path zirkulaerer Modus fuer search_periods==2: Kandidaten
#      auf [0,T) gefaltet, Distanzen zirkulaer. T-Spruenge (r→r+kT) kostenlos.
#      Diagnose: Knick war search_periods==2-Pfad der T-Sprung als err=T bestrafte.
# v89: Kink-Penalty-Fix: max_slope_change war T_int/allowed/dn_cur → kollabiert
#      zu smooth_pen. Jetzt feste Referenz T_int/100 Samples/Periode. Damit misst
#      kink_pen wirklich Steigungsaenderung (2. Ableitung). HARD_KINK_PENALTY 8→20.
# v88: Kink-/Beschleunigungsstrafe in _global_branch_path(): kink_pen bestraft
#      Steigungsaenderung (quadratisch), hard_kink_pen addiert Pauschalstrafe bei
#      err > 2*allowed. Linearer Drift bleibt billig; Astwechsel mit Knick teuer.
# v87: HN-Periodenkorrektur: T_float = smpl_T * HN/8 (Tastennoten-Periode).
#      smpl gibt physikalische Pfeifenschwingung; für HN!=8 war T_float zu klein.
#      Autocorr-Suchbereich [0.5*T_hn, 2.0*T_hn], expected_period = hn_T_float.
#      Neues Feld pa.hn_T_float; T<16-Guard prüft nun T_hn statt T_smpl.
# v86: Small-T-only Kostenterme in _global_branch_path (search_periods > 2):
#      BRANCH_SWITCH_PENALTY (Fenster-Wechsel), BRANCH_ANCHOR_PENALTY (Pseudo-Drift),
#      BRANCH_JUMP_PENALTY (Endpunkt-Ruecksprung). Fuer T>=16 unveraendert.
# v85: CorrLandschaft+Crossfade-Simulation auf pa.lut_r_search_max umgestellt;
#      Fensterkandidaten in _global_branch_path nach Top-K-Schnitt erneut gesichert.
# v84: Adaptives Suchfenster: T<16 → 4T statt 2T; Kandidaten pro Periodenfenster
#      gesichert; Plot zeigt T/2T/3T/4T-Linien; r_search_max im Detailpanel.
# v83: Review-Fixes: Fold-Schwelle best_sc-0.01, MAX_PRUNE_GAP_N, Gap-Pruning
#      nur bei echtem Sprung, Zoom-Button-Fix, ALLOW_SHORT_PERIOD=True.
# v82: Lokales Folding entfernt: best_corr_vectorized(), unwrap_phase_points(),
#      fit_linear_drift() und FOLD_*-Konstanten geloescht (toter Code).
#      Gap-Detection verwendet track_r statt best_r % T_int.
# v81: Periodenerkennung fuer Mixturen: schwache fruehe YIN-Senken werden
#      durch deutlich bessere spaetere lokale Senken ersetzt.

# v77: Eindeutige Versionsanzeige im Fenstertitel/Detailpanel und CLI-Selbsttest
#      --test-wav <file.wav>; Submultiple-Guard sichtbar verifizierbar.
# v76: Perioden-Debug im Detailpanel: smpl_T, autocorr_T, min_p/max_p und Attack-Pfad.
#      Submultiple-Guard verschaerft: bei eindeutig gutem smpl-T wird T/k nicht akzeptiert.
# v75: Submultiple-Guard fuer Mixturen: wenn YIN T/k findet, aber smpl-T im
#      selben Fenster deutlich plausibler ist, wird auf smpl-T korrigiert.
# v74: Simulationsfenster-Defaults: Crossfade-Kurve standardmaessig Sin2;
#      beim Oeffnen nur Attack und Release NDP (interpoliert) sichtbar.
# v73: Simulationsfenster: Crossfade-Laenge kollabiert am rechten Rand der
#      Attack-WAV nicht mehr; safe_slice() darf wie vorgesehen zero-padden.
# v72: Globales Branch-Path-Tracking (Viterbi-artig): lokale Maxima werden
#      ueber alle LUT-Punkte zu einem glatten Gesamtpfad verbunden. Verhindert
#      isolierte Astspruenge; Drift selbst ist erlaubt, abrupte Knicke werden
#      bestraft. Bad-LUT-Heuristik fuer Driftpfade entschaerft.
# v71: Korrelationslandschaft: Colorbar horizontal unter dem Plot, damit sie
#      die Legende rechts nicht mehr verdeckt.
# v70: Crossfade-Simulation: Attack-Slider reicht bis zur Release-Gueltigkeit
#      bzw. beim langen Release bis zum Ende der Attack-WAV.
# v69: Branch-Lock/Full-Coverage: zurueck auf v67-Strategie mit groesserem
#      Punktbudget; Sparse-Punkte werden staerker am vorhergesagten Ast gehalten
#      und nachtraeglich gegen Astwechsel repariert.
# v68: Experimentelles progressives Branch-Tracking: vom letzten sicheren Punkt
#      aus schrittweise weitergehen, bei Abweichung Schrittweite halbieren.
#      Verwarf sich praktisch teilweise, weil viele Punkte frueh verbraucht wurden.
# v67: Anzeige-Konsistenz in der Korrelationslandschaft: effektive LUT-Punkte
#      als best_r % T anzeigen; raw_r nur als Zusatzmarkierung.
# v66: Adaptives Branch-Tracking: laengere Dense-Warmup-Phase und rekursive
#      Zwischenpunkte bei unsicherer Sparse-Fortsetzung.
# v65: Branch-Tracking ueber lokale Korrelationsmaxima statt lokalem argmax;
#      mehrere Kandidaten pro n werden gegen einen vorhergesagten Ast bewertet.
# v64: YIN-CMNDF-Refinement nutzt globales ndp_all, vermeidet Integer-Rundung am Suchrand.
# v63: YIN-CMNDF normalisiert ab Lag 1, verhindert 2T-Schätzung bei Oktav-Stops.
# v57: DC-Entfernung vor Hann-Fenster in estimate_period_by_autocorr.
#      Ohne DC-Removal blaehte der Gleichanteil alle NDP-Werte Richtung 1
#      auf → YIN fand T/2 statt T. Entspricht ChatGPT-Referenzskript.
#      Diagnose-Felder cmndf_at_T_half/T/2T im Detail-Panel angezeigt.
# v56: YIN-CMNDF Periodendetektion: findet erste starke Periodizitaet statt
#      globalem NDP-Maximum — loest T/2-Problem fuer Mixturen mit gerader Harmonik.
#      Fallback auf Lag-Penalty-NDP wenn kein YIN-Valley gefunden.
# v55: Hann-Fenster in estimate_period_by_autocorr (Seitenkeulendaempfung).
#      Score-Worker crossfade window = crossfade_len_samples (wie compute_lut).
# v54: Lag-Strafterm (alpha=0.10) statt Sub-Harmonischen-Check: NDP(lag) *= (1 - 0.10*lag/max_p).
#      Bevorzugt kuerze Perioden wenn NDP(T) ≈ NDP(2T), ohne Mixturen faelschlich zu halbieren.
# v52: NDP-Fenster = crossfade_len (statt min(crossfade,2*T)).
#      estimate_period_by_autocorr: Sub-Harmonischen-Check (div 2/3/4, 94%-Schwelle)
#      verhindert 2*T-Fehlbestimmung bei gerader Harmonik.
# v51: Aliquote/Mixturen: Autokorrelations-Suchbereich unabhängig vom smpl-Chunk.
#      max_p = sr//20 (deckt bis 20 Hz ab), kein HarmonicNumber/smpl-Pitch-Einfluss.
# v50: Bugfix: Loop-Punkte werden jetzt VOR der Autokorrelation aus dem smpl-Chunk
#      gelesen. Vorher: loop_start=0 (Default) → Autokorrelation lief im Anblase-
#      Transient → falsches T bei Aliquoten/Mixturen (z.B. T=96 statt ~177).
# v49: Downsampling-Checkbox in der Toolbar (entspricht GOSettingsOptions::CorrLutDownsampling,
#      Default ON). Toter Code nach return in compute_lut() entfernt.
# v46: Zusätzliche Qualitätskriterien für Legacy-Fallback.
# Eine formal stabilisierte LUT kann trotzdem unbrauchbar sein, wenn
# sie nur durch viele Gap-Fill-Punkte zusammengeflickt wird, schlechte
# Scores hat oder die best_r-Werte kreisstatistisch breit streuen.
LEGACY_SCORE_MIN_THRESHOLD = 0.25
ALLOW_SHORT_PERIOD = True   # Experiment: T<16-Legacy-Sperre deaktiviert
LEGACY_COHERENCE_THRESHOLD = 0.75
LEGACY_PHASE3_MAX          = 8

# v13: Drift-Erkennung für Mixturen/Sesquialtera
DRIFT_FIT_WIN = 12
DRIFT_MAX_RESID_FACTOR = 1.0 # Residuum <= stable_thresh * Faktor

# v65: Branch-Tracking-Parameter
# Statt pro n blind das globale Maximum zu nehmen, werden mehrere lokale Maxima
# betrachtet und der Kandidat gewählt, der zum vorhergesagten Ast passt.
BRANCH_TOP_K             = 24     # v142: high aliquots need more simultaneous branch candidates
BRANCH_SCORE_MARGIN      = 0.40   # Kandidaten bis best_score - margin behalten
BRANCH_PREDICT_PENALTY   = 0.35   # Score-Abzug bei Abstand zum vorhergesagten Ast
BRANCH_MIN_PEAK_DISTANCE = 3      # Mindestabstand lokaler Maxima in r-Samples
BRANCH_PHASE_SEPARATION_FACTOR = 1.0 / 16.0  # v142: 1'/high aliquots can expose ~16 branches per 2T
BRANCH_FIT_WIN           = 8      # letzte Punkte fuer lokale lineare Vorhersage
BRANCH_STABLE_RESID_FACTOR = 1.0
BRANCH_WARMUP_POINTS     = 12     # dichte Startmessungen vor Sparse-Phase
BRANCH_ADAPT_MAX_DEPTH   = 8      # rekursive Zwischenmessungen pro Sparse-Ziel
BRANCH_ADAPT_ERR_FACTOR  = 0.75   # erlaubter Fehler = allowed * Faktor
BRANCH_HARD_LOCK_FACTOR  = 0.75   # Kandidaten in dieser Naehe zum Predicted-Ast haben Vorrang
BRANCH_GLOBAL_SMOOTH_PENALTY = 0.70 # Strafe fuer Knicke im globalen Astpfad
BRANCH_GLOBAL_ALLOWED_FACTOR = 0.10  # erlaubter Knickfehler relativ zu T
MAX_PRUNE_GAP_N = 50                 # max. n-Abstand zwischen Nachbarn beim Pruning (Perioden)
# v118: Gap-Fill darf nicht Rauschen bis auf dn=1 herunter subdividieren.
# n ist Periodenindex; Werte darunter liefern meist nur Peak-Quantisierung/Jitter,
# aber kosten viele zusaetzliche Korrelationen und koennen den DP durch Rauschpunkte belasten.
GAP_FILL_MIN_DN = 8
GAP_FILL_MAX_INSERTS = 12
BRANCH_COPY_SWITCH_PENALTY = 0.20   # Kosten fuer T-Copy-Wechsel in der Copy-DP (Stufe 3)

# v86: Small-T-only Kostenterme (nur aktiv wenn search_periods > 2)
BRANCH_SWITCH_PENALTY  = 1.50  # Kosten pro Periodenfenster-Wechsel (v139: erhoeht von 0.50)
BRANCH_ANCHOR_PENALTY  = 0.40  # Kosten fuer Abweichung vom Start-Ast-Anker
BRANCH_JUMP_PENALTY    = 0.30  # Kosten fuer harte Spruenge zwischen aufeinanderfolgenden Punkten

# v142: Zusätzliche Phasen-Vorhersage-Penalty gegen späte Branch-Jumps.
# Smooth/Kink-Strafen reichen bei hohen Aliquoten und Sparse-Abständen nicht
# immer, weil ein falscher Parallelast lokal einen etwas besseren Score haben
# kann. Diese Penalty hat eine kleine Totzone und bestraft erst deutliche
# Abweichungen der Kandidatenphase von der vorhergesagten Trackphase.
BRANCH_PHASE_PRED_PENALTY = 0.0  # v143: disabled; duplicated smooth_pen and destabilized noisy high-HN cases
BRANCH_DP_SCORE_WEIGHT = 0.35  # v144: local score differences are noisy; geometry should dominate branch tracking
BRANCH_PHASE_PRED_THRESH_HIGH_HN = 1.0 / 8.0
BRANCH_PHASE_PRED_THRESH_DEFAULT = 1.0 / 6.0

# v88/v89: Kink-/Beschleunigungsstrafe — allgemein, nicht small-T-spezifisch
# Referenz: T_int / 100.0 Samples/Periode als "natuerliche" Steigungsaenderung.
# NICHT durch dn_cur teilen — sonst kollabiert kink_pen algebraisch zu smooth_pen.
BRANCH_KINK_PENALTY      = 4.0   # Strafe fuer echte Steigungsaenderung (Samples/Periode)
BRANCH_KINK_REF_DIVISOR  = 100.0 # max_slope_change = T_int / REF_DIVISOR
# v114: Gap-Fill erzeugt oft dn=1-Punkte. Einzelnes ±1-Sample-Jitter darf
# nicht als echter Kurswechsel gewertet werden. Deshalb: Kink an Gap-Punkten
# stark reduzieren und den lokalen Kink-Term hart deckeln.
BRANCH_KINK_GAP_SCALE    = 0.10
BRANCH_KINK_PENALTY_CAP  = 3.0
# v139: Score-basierte Kink-Toleranz fuer instabile Einschwingphase.
# Niedriger Score = verrauschtes Signal = natuerliches Wandern des Astes.
# Kink-Penalty wird mit dem lokalen Score skaliert, damit der DP dem
# wandernden Ast folgt statt auf einen glatteren (falschen) Ast zu wechseln.
BRANCH_KINK_SCORE_SCALE_MIN = 0.25  # untere Grenze der Score-Skalierung
BRANCH_KINK_SCORE_N_LIMIT   = 30    # Score-Toleranz nur bis Periode n (Einschwingphase)
BRANCH_HARD_KINK_FACTOR  = 2.0   # err > Faktor*allowed → harter Knick
BRANCH_HARD_KINK_PENALTY = 20.0  # additive Strafe bei hartem Knick (gross genug gg. Score)
BRANCH_MIDPT_COHERENCE_PENALTY = 5.0  # v109-v111: Strafe wenn Midpoint-Frame kein Peak bei erwartetem r hat
BRANCH_MIDPT_SCORE_PENALTY      = 1.5  # v111: Zusatzstrafe wenn der naechste Midpoint-Peak deutlich schwach ist

# v84: Adaptives Suchfenster für kleine Perioden
SMALL_T_THRESHOLD      = 16   # T_int < Schwelle → erweitertes Suchfenster
SMALL_T_SEARCH_PERIODS = 4    # Anzahl Perioden im Suchfenster bei kleinem T
DEFAULT_SEARCH_PERIODS = 2    # Standardfall


# ─── Datenklassen ─────────────────────────────────────────────────────────────

@dataclass
class LutPoint:
    n:          int     # Periodenindex
    loop_pos:   int     # Sample-Position im Attack
    best_r:     int     # Effektiver Release-Offset / Phase
    best_score: float   # Korrelationsscore beim besten r
    phase:      str     # "dense", "sparse", "gap", "drift"
    raw_r:      int   = 0
    raw_score:  float = 0.0
    folded:     bool  = False
    fold_ratio: float = 1.0
    # Richtung des Segments vom vorherigen LUT-Punkt zu diesem Punkt.
    # Wird aus track_r berechnet. Relevant fuer gefaltete LUTs nach Pruning:
    # dann ist nicht mehr zwingend der kuerzeste Kreisweg der richtige Weg.
    approach_up: bool = True


@dataclass
class PipeAnalysis:
    organ_base:    str
    rank_name:     str
    midi_note:     int
    perspective:   str
    release_type:  str
    attack_path:   str
    release_path:  str
    harmonic_number: int = 8

    # Ergebnisse
    T_float:       float = 0.0
    T_int:         int   = 0
    sample_rate:   int   = 0
    loop_start:    int   = 0
    loop_end:      int   = 0
    loop_len:      int   = 0
    release_len:   int   = 0
    smpl_T_float:  float = 0.0   # Rohperiode aus smpl-Chunk (physikalische Pfeifenschwingung)
    hn_T_float:    float = 0.0   # HN-korrigierte Tastennoten-Periode: smpl_T * HN/8
    autocorr_T_float: float = 0.0
    autocorr_min_p: int = 0
    autocorr_max_p: int = 0
    n_total:       int   = 0
    crossfade_len_samples: int = 0
    crossfade_auto: bool = False   # True = GO-Default (get_fader_length), nicht explizit im ODF

    min_key_press_ms: Optional[int] = None
    max_key_press_ms: Optional[int] = None
    is_shortest_release: bool = False  # True für das Release mit kleinster max_key_press_ms
    min_sample:   int   = 0
    max_sample:   Optional[int] = None

    # YIN diagnostics: CMNDF values at T/2, T, 2T (NaN if not computed)
    cmndf_at_T_half: float = float('nan')
    cmndf_at_T:      float = float('nan')
    cmndf_at_2T:     float = float('nan')

    drift_mode: bool = False
    drift_per_period: float = 0.0
    drift_residual: float = 0.0
    max_interp_gap_n: int = 0
    dense_step_used: int = DENSE_STEP

    lut_points:    list = field(default_factory=list)   # List[LutPoint]
    lut_folded:    bool = True    # True = Pfad auf [0,T) gefaltet
    lut_points_raw_count: int = 0  # Anzahl Punkte vor Pruning
    lut_score_p10: float = 0.0   # 10. Perzentil der LUT-Scores (nur Diagnose)
    lut_score_median: float = 0.0 # Median der Qualitaets-Scores vor Pruning
    lut_low_score_fraction: float = 0.0 # Anteil Scores < SCORE_BAD in Qualitaets-Punkten
    lut_quality_score_count: int = 0 # Anzahl fuer Qualitaetsbewertung verwendeter Punkte
    lut_fold_reason: str = ""    # Grund fuer Fold-Entscheidung
    lut_r_search_max: int = 0    # r_max des Suchfensters in Samples
    lut_search_periods: int = 2  # Anzahl Perioden im Suchfenster
    stable_at_n:   Optional[int] = None
    stabilized:    bool = False
    phase3_count:  int  = 0
    atk_rms:       float = 0.0
    rel_rms:       float = 0.0
    amplitude_ratio: float = 0.0
    error:         Optional[str] = None
    legacy_fallback: bool = False  # GO nutzt Legacy statt Korrelations-LUT
    legacy_reason: str = ""          # "short_period", "drift", "instabil", "bad_lut"

    # Erweiterte Metriken fuer Datenanalyse
    n_dense:       int   = 0    # Anzahl Phase-1-Punkte
    n_sparse:      int   = 0    # Anzahl Phase-2-Punkte
    n_gap:         int   = 0    # Anzahl Phase-3-Punkte
    score_mean:    float = 0.0  # Durchschnittsscore steady-state
    score_min:     float = 0.0  # Schlechtester Score steady-state
    score_max:     float = 0.0  # Bester Score steady-state
    bestr_coherence: float = 0.0  # Kreisstatistik R ∈ [0,1]: 1=stabil ein Ast, 0=instabil/mehrere Äste
    bestr_mean:    float = 0.0  # Mittelwert best_r (steady-state)
    transient_end_n: int = 0    # n ab dem steady-state beginnt
    freq_hz:       float = 0.0  # berechnete Pfeifenfrequenz
    midi_note_str: str   = ""   # z.B. "GIS3"
    atk_frames:    int   = 0    # Laenge Attack-WAV in Samples
    rel_frames:    int   = 0    # Laenge Release-WAV in Samples

    @property
    def severity(self) -> int:
        """0=ok, 1=warn, 2=bad"""
        if self.legacy_fallback:
            return 0
        if self.error:
            return 2
        # Linearer Drift (drift_mode=True) ist kein Fehler — Branch-Tracker folgt ihm.
        if not self.stabilized and not self.drift_mode:
            return 2
        # Score-Warnungen nicht mehr aus Minimum/p10 der geprunten LUT ableiten:
        # einzelne Transientenpunkte koennen zwangslaufig schlecht korrelieren.
        # Bewertet wird der typische Score nach Stabilisierung, vor Pruning.
        q_count = int(getattr(self, "lut_quality_score_count", 0) or 0)
        score_median = float(getattr(self, "lut_score_median", 0.0) or 0.0)
        low_frac = float(getattr(self, "lut_low_score_fraction", 0.0) or 0.0)
        if q_count >= 4 and score_median > 0.0 and score_median < SCORE_BAD:
            return 2
        if (q_count >= 4 and score_median > 0.0 and score_median < SCORE_WARN
                and low_frac > SCORE_LOW_FRACTION_WARN):
            return 1
        # Spaete Stabilisierung nur fuer kuerzestes Release und erst ab n>100 warnen.
        if self.is_shortest_release and self.stable_at_n is not None and self.stable_at_n > 100:
            return 1
        return 0

    @property
    def status_text(self) -> str:
        """Erklaerungstext fuer Status — eine Zeile, warum Warn/Error/Legacy."""
        if self.error:
            return f"❌ {self.error[:60]}"
        if self.legacy_fallback:
            reason_map = {
                "drift":        f"⚡ Legacy Drift — {self.drift_per_period:.4f} smp/Prd",
                "instabil":     "⚡ Legacy — Korrelation instabil",
                "short_period": "⚡ Legacy — Periode T<16 (Grenzfall)",
            }
            if self.legacy_reason in reason_map:
                return reason_map[self.legacy_reason]
            if self.legacy_reason.startswith("bad_lut:"):
                detail = self.legacy_reason.split(":", 1)[1]
                return f"⚡ Legacy — LUT schwach: {detail}"
            return f"⚡ Legacy ({self.legacy_reason})"
        if not self.stabilized and self.drift_mode:
            n_pts = len(self.lut_points)
            return f"↗ Drift verfolgt  drift={self.drift_per_period:.4f}  {n_pts}Pkt"
        if not self.stabilized:
            return f"❌ Nicht stabilisiert  n_total={self.n_total}"
        # Warn-Gruende sammeln
        warn_parts = []
        score_median = float(getattr(self, "lut_score_median", 0.0) or 0.0)
        low_frac = float(getattr(self, "lut_low_score_fraction", 0.0) or 0.0)
        q_count = int(getattr(self, "lut_quality_score_count", 0) or 0)
        if (q_count >= 4 and score_median > 0.0 and score_median < SCORE_WARN
                and low_frac > SCORE_LOW_FRACTION_WARN):
            warn_parts.append(f"score_med={score_median:.2f}  low={low_frac*100:.0f}%")
        if self.is_shortest_release and self.stable_at_n and self.stable_at_n > 100:
            warn_parts.append(f"stabil erst n={self.stable_at_n}")
        # OK-Zeile
        n_pts = len(self.lut_points)
        score_str = f"  med={score_median:.2f}" if score_median > 0 else ""
        drift_str = f"  drift={self.drift_per_period:.3f}" if abs(self.drift_per_period) > 0.001 else ""
        base = f"n={self.stable_at_n}  {n_pts}Pkt{score_str}{drift_str}"
        if warn_parts:
            return "⚠ " + "  ".join(warn_parts) + "  " + base
        return "✓ " + base

    @property
    def severity_label(self) -> str:
        if self.legacy_fallback and self.legacy_reason == "drift":
            return "Legacy Drift"
        if self.legacy_fallback and self.legacy_reason == "instabil":
            return "Legacy instabil"
        if self.legacy_fallback and self.legacy_reason.startswith("bad_lut"):
            return "Legacy schlechte LUT"
        if self.legacy_fallback and self.legacy_reason == "short_period":
            return "Legacy T<16"
        if self.legacy_fallback:
            return "Legacy"
        return ["OK", "Warn", "Fehler"][self.severity]

    @property
    def severity_tag(self) -> str:
        """Tag-Name für tkinter Treeview Zeilenfarbe."""
        if self.error:
            return "sev_error"
        if self.legacy_fallback and self.legacy_reason == "drift":
            return "sev_drift"
        if self.legacy_fallback and self.legacy_reason == "instabil":
            return "sev_legacy_warn"
        if self.legacy_fallback and self.legacy_reason.startswith("bad_lut"):
            return "sev_legacy_badlut"
        if self.legacy_fallback:
            return "sev_legacy"
        return ["sev_ok", "sev_warn", "sev_bad"][self.severity]


# ─── WAV-Hilfsfunktionen ──────────────────────────────────────────────────────

def _parse_wav_chunks(path: str) -> dict:
    """
    Parst WAV-Datei direkt (ohne wave.open), toleriert fmt_size > 16.
    Gibt dict mit: nch, sr, sw (bytes/sample), data_bytes, n_frames zurück.
    Unterstützt PCM (format=1) und IEEE Float (format=3).
    """
    with open(path, "rb") as f:
        raw = f.read()

    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"Keine gültige WAV-Datei: {path}")

    result = {}
    pos = 12
    while pos < len(raw) - 8:
        cid  = raw[pos:pos+4]
        size = struct.unpack_from("<I", raw, pos+4)[0]
        chunk_data = raw[pos+8 : pos+8+size]

        if cid == b"fmt ":
            fmt_code = struct.unpack_from("<H", chunk_data, 0)[0]
            result["fmt_code"] = fmt_code          # 1=PCM, 3=float, 65534=extensible
            result["nch"] = struct.unpack_from("<H", chunk_data, 2)[0]
            result["sr"]  = struct.unpack_from("<I", chunk_data, 4)[0]
            result["bps"] = struct.unpack_from("<H", chunk_data, 14)[0]  # bits per sample
            result["sw"]  = result["bps"] // 8                           # bytes per sample
            # Extensible: echter Sub-Format steckt ab Offset 24
            if fmt_code == 65534 and size >= 40:
                sub = struct.unpack_from("<H", chunk_data, 24)[0]
                result["fmt_code"] = sub

        elif cid == b"data":
            result["data_offset"] = pos + 8
            result["data_size"]   = size

        pos += 8 + size + (size % 2)

    if "nch" not in result or "data_offset" not in result:
        raise ValueError(f"fmt oder data chunk fehlt: {path}")

    nch   = result["nch"]
    sw    = result["sw"]
    nf    = result["data_size"] // (nch * sw)
    result["n_frames"]  = nf
    result["raw_bytes"] = raw  # komplettes File im Speicher
    return result


def read_wav_mono_float(path: str, max_samples: int = None) -> tuple:
    """Liest WAV, gibt (mono_float32_array, sample_rate, n_frames, n_channels) zurück.
    Toleriert fmt_size != 16 (z.B. 18 oder 20 bei manchen 24-bit WAVs)."""
    r   = _parse_wav_chunks(path)
    nch = r["nch"]
    sr  = r["sr"]
    sw  = r["sw"]
    nf  = r["n_frames"]
    fmt = r["fmt_code"]

    if max_samples:
        nf = min(nf, max_samples)

    byte_start = r["data_offset"]
    byte_end   = byte_start + nf * nch * sw
    raw        = r["raw_bytes"][byte_start:byte_end]

    if fmt == 3:
        # IEEE Float
        if sw == 4:
            s = np.frombuffer(raw, dtype=np.float32).copy()
        elif sw == 8:
            s = np.frombuffer(raw, dtype=np.float64).astype(np.float32)
        else:
            raise ValueError(f"Unbekannte Float-Breite {sw*8} bit: {path}")
        norm = 1.0
    elif sw == 3:
        # 24-bit PCM
        arr = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        s = (arr[:,0].astype(np.int32)
             | (arr[:,1].astype(np.int32) << 8)
             | (arr[:,2].astype(np.int32) << 16))
        s = np.where(s >= 2**23, s - 2**24, s).astype(np.float32)
        norm = float(2**23)
    elif sw == 2:
        s = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
        norm = float(2**15)
    elif sw == 4:
        s = np.frombuffer(raw, dtype=np.int32).astype(np.float32)
        norm = float(2**31)
    elif sw == 1:
        s = np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0
        norm = 128.0
    else:
        raise ValueError(f"Unbekannte Sample-Breite {sw*8} bit: {path}")

    # Mono-Mix aller Kanäle
    s    = s.reshape(-1, nch)
    mono = s.mean(axis=1) / norm
    return mono, sr, nf, nch


def parse_smpl_chunk(path: str) -> dict:
    """Liest smpl-Chunk aus WAV: midi_note, pitch_frac, loops."""
    result = {"midi_note": None, "pitch_frac": 0, "loops": []}
    try:
        with open(path, "rb") as f:
            data = f.read()
        pos = 12
        while pos < len(data) - 8:
            cid  = data[pos:pos+4]
            size = struct.unpack_from("<I", data, pos+4)[0]
            if cid == b"smpl" and size >= 36:
                result["midi_note"]  = struct.unpack_from("<I", data, pos+20)[0]
                result["pitch_frac"] = struct.unpack_from("<I", data, pos+24)[0]
                n_loops = struct.unpack_from("<I", data, pos+36)[0]
                for i in range(min(n_loops, 8)):
                    off = pos + 44 + i * 24
                    if off + 16 <= len(data):
                        s = struct.unpack_from("<I", data, off+8)[0]
                        e = struct.unpack_from("<I", data, off+12)[0]
                        result["loops"].append((s, e))
                break
            pos += 8 + size + (size % 2)
    except Exception:
        pass
    return result


def normalized_dot_product(a: np.ndarray, b: np.ndarray,
                            offset: int, window: int) -> float:
    """Pearson-Korrelation zwischen a[0:window] und b[offset:offset+window]."""
    if offset + window > len(b):
        return -2.0
    aw = a[:window]
    bw = b[offset:offset+window]
    na = np.linalg.norm(aw)
    nb = np.linalg.norm(bw)
    if na < 1e-12 or nb < 1e-12:
        return 0.0
    return float(np.dot(aw / na, bw / nb))


def _local_maxima_indices(scores: np.ndarray, min_distance: int = 3) -> list:
    """Lokale Maxima in einer 1D-Scorekurve, sortiert nach Score absteigend."""
    if len(scores) == 0:
        return []
    peaks = []
    for i in range(len(scores)):
        left_ok  = (i == 0 or scores[i] >= scores[i - 1])
        right_ok = (i == len(scores) - 1 or scores[i] >= scores[i + 1])
        if left_ok and right_ok:
            peaks.append(i)
    peaks.sort(key=lambda idx: float(scores[idx]), reverse=True)

    # einfache Non-Maximum-Suppression, damit ein breiter Peak nicht mehrere
    # fast identische Kandidaten liefert.
    selected = []
    for idx in peaks:
        if all(abs(idx - j) >= min_distance for j in selected):
            selected.append(idx)
    return selected


def _compute_corr_scores(lw: np.ndarray, release_mono: np.ndarray,
                          r_max: int, window_len: int):
    """Berechnet den vollen Korrelations-Score-Vektor [0, r_max).
    Rückgabe: (scores: np.ndarray, r_max_actual: int) oder (None, 0) bei Fehler."""
    if r_max + window_len > len(release_mono):
        r_max = max(1, len(release_mono) - window_len)
    na = np.linalg.norm(lw)
    if na < 1e-12 or r_max <= 0:
        return None, 0
    lw_n = lw / na
    from numpy.lib.stride_tricks import as_strided
    s = release_mono.strides[0]
    rel_mat = as_strided(release_mono, shape=(r_max, window_len), strides=(s, s))
    norms = np.linalg.norm(rel_mat, axis=1)
    norms = np.where(norms < 1e-12, 1.0, norms)
    return rel_mat.dot(lw_n) / norms, r_max


def _scores_to_candidates(scores: np.ndarray,
                           top_k: int = BRANCH_TOP_K,
                           score_margin: float = BRANCH_SCORE_MARGIN,
                           min_peak_distance: int = BRANCH_MIN_PEAK_DISTANCE) -> list:
    """Extrahiert Top-K Kandidaten aus einem Score-Vektor."""
    peak_idx = _local_maxima_indices(scores, min_peak_distance)
    if not peak_idx:
        peak_idx = [int(np.argmax(scores))]
    best_score = float(scores[peak_idx[0]])
    candidates = []
    for idx in peak_idx:
        sc = float(scores[idx])
        if len(candidates) >= top_k:
            break
        if sc < best_score - score_margin and len(candidates) > 0:
            continue
        candidates.append((int(idx), sc))
    if not candidates:
        candidates.append((int(np.argmax(scores)), float(np.max(scores))))
    return candidates


def _corr_scores_and_candidates(lw: np.ndarray, release_mono: np.ndarray,
                                 r_max: int, window_len: int,
                                 top_k: int = BRANCH_TOP_K,
                                 score_margin: float = BRANCH_SCORE_MARGIN,
                                 min_peak_distance: int = BRANCH_MIN_PEAK_DISTANCE):
    """Gibt (candidates, scores_array) zurück. scores_array wird für
    _ensure_per_window_candidates benötigt."""
    scores, r_actual = _compute_corr_scores(lw, release_mono, r_max, window_len)
    if scores is None:
        return [(0, 0.0)], np.zeros(1)
    cands = _scores_to_candidates(scores, top_k, score_margin, min_peak_distance)
    return cands, scores


def corr_candidates_vectorized(lw: np.ndarray, release_mono: np.ndarray,
                               r_max: int, window_len: int,
                               top_k: int = BRANCH_TOP_K,
                               score_margin: float = BRANCH_SCORE_MARGIN,
                               min_peak_distance: int = BRANCH_MIN_PEAK_DISTANCE) -> list:
    """
    Berechnet alle relevanten lokalen Korrelationsmaxima für ein Attack-Fenster.

    Rückgabe: Liste von Tupeln (raw_r, score), nach Score absteigend.
    raw_r liegt im realen r-Suchraum [0, r_max). Keine T-Faltung.
    """
    cands, _ = _corr_scores_and_candidates(
        lw, release_mono, r_max, window_len, top_k, score_margin, min_peak_distance)
    return cands


def _ensure_per_window_candidates(candidates: list, scores: np.ndarray,
                                   T_int_d: int, search_periods: int) -> list:
    """Stellt sicher dass aus jedem Periodenfenster [k*T, (k+1)*T) mindestens
    ein Kandidat enthalten ist. Wichtig bei search_periods > 2 (kleines T),
    damit nicht alle Top-K aus demselben Ast kommen.
    candidates: Liste von (idx, score) aus corr_candidates_vectorized (downsampled).
    Rückgabe: ergänzte und deduplizierte Liste."""
    if T_int_d <= 0 or search_periods <= 2:
        return candidates
    by_raw = {int(r): float(sc) for r, sc in candidates}
    for k in range(search_periods):
        lo = k * T_int_d
        hi = min((k + 1) * T_int_d, len(scores))
        if lo >= hi:
            continue
        # Schon ein Kandidat aus diesem Fenster?
        if any(lo <= r < hi for r in by_raw):
            continue
        # Besten lokalen Kandidaten aus diesem Fenster holen.
        window_best = int(lo + np.argmax(scores[lo:hi]))
        sc = float(scores[window_best])
        by_raw[window_best] = sc
    return sorted(by_raw.items(), key=lambda x: x[1], reverse=True)


def closest_branch_copy(raw_r: int, predicted_r: float, T_int: int) -> float:
    """
    Wählt die um k*T verschobene Kopie eines Kandidaten, die am nächsten an
    der vorhergesagten unwrapped Astposition liegt.

    Das ist nur zum Tracking gedacht. Der reale Release-Offset bleibt raw_r.
    """
    if T_int <= 0:
        return float(raw_r)
    best = float(raw_r)
    best_d = abs(best - predicted_r)
    for k in range(-8, 9):
        cand = float(raw_r + k * T_int)
        d = abs(cand - predicted_r)
        if d < best_d:
            best = cand
            best_d = d
    return best

def circ_dist(a: int, b: int, T: int) -> int:
    d = abs(int(a) - int(b))
    if d > T // 2:
        d = T - d
    return d


def shortest_circular_delta(a: float, b: float, T: int) -> float:
    """Kürzeste gerichtete Differenz b-a auf dem Kreis [0,T)."""
    d = float(b) - float(a)
    half = T / 2.0
    while d > half:
        d -= T
    while d < -half:
        d += T
    return d


def _phase_separated_candidates(candidates: list, T_int: int, max_k: int = BRANCH_TOP_K,
                                  phase_sep_factor: float = None) -> list:
    """Reduziert Kandidaten auf starke, phasengetrennte Maxima.

    Die DP braucht nicht 32 nahezu phasengleiche Peaks. Wir nehmen daher
    greedy immer das hoechste verbleibende Maximum und akzeptieren weitere
    Maxima nur, wenn ihre Phase mindestens ca. T/8 von bereits gewaehlten
    Kandidaten entfernt ist. Dadurch bleiben verschiedene Aeste erhalten,
    aber breite/rauschige Peak-Cluster kosten nicht mehr massiv DP-Zeit.
    """
    if not candidates or T_int <= 0 or max_k <= 0:
        return candidates[:max_k]
    if phase_sep_factor is None:
        phase_sep_factor = BRANCH_PHASE_SEPARATION_FACTOR
    min_sep = max(1, int(round(T_int * phase_sep_factor)))
    selected = []
    for raw, sc in sorted(candidates, key=lambda it: it[1], reverse=True):
        raw_i = int(raw)
        phase_i = raw_i % T_int
        if all(circ_dist(phase_i, int(sel_raw) % T_int, T_int) >= min_sep
               for sel_raw, _ in selected):
            selected.append((raw_i, float(sc)))
            if len(selected) >= max_k:
                break
    if not selected:
        selected = [max(candidates, key=lambda it: it[1])]
    return selected


def refine_peak_parabolic(y: np.ndarray, i: int) -> float:
    """Subsample-Position eines Maximums per Parabelinterpolation."""
    if i <= 0 or i >= len(y) - 1:
        return float(i)
    ym1 = float(y[i - 1])
    y0  = float(y[i])
    yp1 = float(y[i + 1])
    denom = ym1 - 2.0 * y0 + yp1
    if abs(denom) < 1e-12:
        return float(i)
    delta = 0.5 * (ym1 - yp1) / denom
    if abs(delta) > 1.0:
        return float(i)
    return float(i) + delta


def estimate_period_by_autocorr(samples: np.ndarray,
                                  min_period: int,
                                  max_period: int,
                                  yin_threshold: float = 0.15,
                                  expected_period: Optional[float] = None) -> tuple:
    """YIN-style CMNDF period estimator (Nachbau EstimatePeriodByAutocorr).

    Finds the FIRST lag where the cumulative-mean normalised difference dips
    below yin_threshold — the true fundamental, not T/2 or 2T.
    Falls back to penalised NDP if no YIN valley is found.
    """
    N = len(samples)
    if N < max_period * 2:
        return float(min_period), {'cmndf_half': float('nan'), 'cmndf_T': float('nan'), 'cmndf_2T': float('nan')}

    # DC removal then Hann window (matches ChatGPT reference: seg -= mean before FFT).
    # Removing DC prevents the mean offset from inflating sub-harmonic ACF peaks.
    s    = samples.astype(np.float32) - float(np.mean(samples))
    hann = 0.5 * (1.0 - np.cos(2.0 * np.pi * np.arange(N) / (N - 1)))
    x    = s * hann.astype(np.float32)
    W    = N - max_period
    ref_n = x[:W] / (np.linalg.norm(x[:W]) + 1e-12)

    lags_all = np.arange(1, max_period + 1)
    ndp_all  = np.empty(len(lags_all), dtype=np.float64)
    for i, lag in enumerate(lags_all):
        sh = x[lag:lag + W]
        nrm = np.linalg.norm(sh)
        ndp_all[i] = float(np.dot(ref_n, sh / nrm)) if nrm > 1e-12 else 0.0

    # YIN CMNDF: d'[i]=1-NDP[i]; cmndf[i]=d'[i]/mean(d'[0..i]) ab Lag 1
    d_prime_all = 1.0 - ndp_all
    cumsum_all  = np.cumsum(d_prime_all)
    cmndf_all   = np.where(
        cumsum_all > 0,
        d_prime_all * np.arange(1, len(lags_all) + 1) / cumsum_all,
        1.0,
    )

    search_start = max(1, min_period)
    search_stop  = max_period
    lags    = np.arange(search_start, search_stop + 1)
    ndp_arr = ndp_all[search_start - 1:search_stop]
    cmndf   = cmndf_all[search_start - 1:search_stop]

    # YIN-Variante:
    # Klassisches YIN nimmt die erste Senke unter yin_threshold. Bei Mixturen
    # kann aber ein hoher Teilton eine fruehe, nur maessig gute Senke erzeugen
    # (z.B. T/3), waehrend die gemeinsame Periode T spaeter eine viel tiefere
    # Senke hat. Deshalb sammeln wir lokale CMNDF-Senken und ersetzen eine
    # schwache fruehe Senke durch eine deutlich bessere spaetere Senke.
    valley_candidates = []  # (cmndf, lag, ndp)
    for i in range(1, len(cmndf) - 1):
        c = float(cmndf[i])
        if c < yin_threshold and c <= float(cmndf[i - 1]) and c <= float(cmndf[i + 1]):
            valley_candidates.append((c, int(lags[i]), float(ndp_arr[i])))

    # Fallback falls die erste/letzte Stelle selbst die Senke bildet.
    if not valley_candidates:
        in_valley  = False
        best_cmndf = 2.0
        yin_lag    = 0
        for i in range(len(cmndf)):
            c = float(cmndf[i])
            if c < yin_threshold:
                if not in_valley or c < best_cmndf:
                    best_cmndf = c;  yin_lag = int(lags[i]);  in_valley = True
                else:
                    break
            elif in_valley:
                break
        if in_valley:
            valley_candidates.append((best_cmndf, yin_lag, float(ndp_all[yin_lag - 1])))

    if valley_candidates:
        # Erste akzeptable YIN-Senke bleibt Default.
        chosen_c, chosen_lag, chosen_ndp = valley_candidates[0]

        # Aber: wenn eine spaetere Senke sehr viel besser ist, dann war die
        # erste Senke wahrscheinlich nur ein Teilton-/Mixtur-Artefakt.
        # Beispiel Problempfeife: lag16 cmndf=0.091, lag49 cmndf=0.0055.
        for c, lag, ndp in valley_candidates[1:]:
            much_deeper = c <= max(0.050, chosen_c * 0.35)
            much_better_ndp = ndp >= chosen_ndp + 0.03
            near_integer_multiple = False
            if chosen_lag > 0:
                ratio = lag / float(chosen_lag)
                nearest = round(ratio)
                near_integer_multiple = nearest >= 2 and abs(ratio - nearest) < 0.18
            if much_deeper and (much_better_ndp or near_integer_multiple):
                chosen_c, chosen_lag, chosen_ndp = c, lag, ndp

        abs_idx = int(chosen_lag - 1)
        refined_abs_idx = refine_peak_parabolic(ndp_all, abs_idx)
        result = 1.0 + refined_abs_idx
    else:
        # Fallback: penalised NDP fuer Auswahl, echtes NDP fuer Subsample-Refinement
        weighted = ndp_arr * (1.0 - 0.10 * lags / max_period)
        best_idx = int(np.argmax(weighted))
        abs_idx = int((search_start - 1) + best_idx)
        refined_abs_idx = refine_peak_parabolic(ndp_all, abs_idx)
        result = 1.0 + refined_abs_idx

    # v75/v76: Submultiple-Guard.
    # Bei Mixturen kann die erste CMNDF-Senke auf einem starken Teilton liegen
    # (z.B. T/3). Der smpl-Chunk liefert fuer WAV-Dateien aber oft die reale
    # Sample-Periode. Wenn das gefundene Ergebnis ein ganzzahliges Submultiple
    # von expected_period ist und expected_period selbst im selben Fenster eine
    # sehr gute Periodizitaet zeigt, wird auf expected_period korrigiert.
    if expected_period is not None and expected_period > 0:
        exp_i = int(round(expected_period))
        res_i = int(round(result))
        if 1 <= exp_i <= max_period and 1 <= res_i <= max_period and result > 0:
            ratio = float(expected_period) / float(result)
            k = int(round(ratio))
            if 2 <= k <= 8 and abs(ratio - k) <= 0.18:
                cm_res = float(cmndf_all[res_i - 1])
                cm_exp = float(cmndf_all[exp_i - 1])
                ndp_res = float(ndp_all[res_i - 1])
                ndp_exp = float(ndp_all[exp_i - 1])

                # Konservativ, aber nicht zu streng: Bei der Problemdatei ist
                # T/3 harmonisch relevant, aber smpl_T ist extrem eindeutig.
                exp_good = (cm_exp <= 0.03) or (ndp_exp >= 0.96)
                res_suspicious = result < expected_period * 0.70
                exp_not_worse = (cm_exp <= cm_res * 1.25) or (ndp_exp >= ndp_res - 0.03)
                exp_clearly_better = (cm_exp < cm_res * 0.50) or (ndp_exp > ndp_res + 0.03)

                if exp_good and res_suspicious and (exp_not_worse or exp_clearly_better or (cm_exp <= 0.05 and ndp_exp >= 0.90)):
                    refined_exp_idx = refine_peak_parabolic(ndp_all, exp_i - 1)
                    result = 1.0 + refined_exp_idx

    # Diagnostic: CMNDF values at T/2, T, 2T for the RETURNED period.
    def _cmndf_at(lag: int) -> float:
        if lag < 1 or lag > max_period:
            return float('nan')
        i = lag - 1
        return float(cmndf_all[i]) if 0 <= i < len(cmndf_all) else float('nan')

    result_i = int(round(result))
    diag = {
        'cmndf_half': _cmndf_at(result_i // 2),
        'cmndf_T':    _cmndf_at(result_i),
        'cmndf_2T':   _cmndf_at(result_i * 2),
    }
    return result, diag


def _go_default_crossfade_ms(midi_note: int) -> int:
    """GO-Default-Crossfade-Laenge wenn ReleaseCrossfadeLength nicht im ODF steht.

    Entspricht GOSoundProviderWave::get_fader_length() — notenbabhaengig aus
    dem smpl-Chunk der WAV-Datei (m_MidiKeyNumber).
    Bereich: 184 ms (Bass) .. 6 ms (Diskant).
    """
    if midi_note < 42:
        return 184
    if midi_note > 86:
        return 6
    return max(6, 184 - int(((midi_note - 42.0) / 44.0) * 178.0))


# ─── LUT-Algorithmus (Python-Nachbau) ────────────────────────────────────────

def _run_global_branch_dp(points_in: list, T_int: int, r_max: int, search_periods: int,
                           harmonic_number: int = 8, params: dict = None) -> list:
    """Parametrisierter Viterbi-Branch-Tracking-DP.

    Wird sowohl von compute_lut() (ueber den inneren _global_branch_path-Wrapper)
    als auch vom interaktiven Branch Tuning Lab fuer On-the-fly-Neuberechnungen
    genutzt.  params-Dict ueberschreibt globale Konstanten:
      top_k, phase_sep_factor, smooth_penalty, kink_penalty, switch_penalty,
      kink_n_limit, kink_gap_scale, kink_penalty_cap, kink_score_min, score_weight
    """
    _pr = params or {}
    _top_k        = int(_pr.get('top_k',            BRANCH_TOP_K))
    _phase_sep    = float(_pr.get('phase_sep_factor', BRANCH_PHASE_SEPARATION_FACTOR))
    _smooth_pen   = float(_pr.get('smooth_penalty',   BRANCH_GLOBAL_SMOOTH_PENALTY))
    _kink_pen     = float(_pr.get('kink_penalty',     BRANCH_KINK_PENALTY))
    _switch_pen   = float(_pr.get('switch_penalty',   BRANCH_SWITCH_PENALTY))
    _kink_n_limit = int(_pr.get('kink_n_limit',       BRANCH_KINK_SCORE_N_LIMIT))
    _kink_gap_sc  = float(_pr.get('kink_gap_scale',   BRANCH_KINK_GAP_SCALE))
    _kink_cap     = float(_pr.get('kink_penalty_cap', BRANCH_KINK_PENALTY_CAP))
    _kink_sc_min  = float(_pr.get('kink_score_min',   BRANCH_KINK_SCORE_SCALE_MIN))
    _score_w      = float(_pr.get('score_weight',     BRANCH_DP_SCORE_WEIGHT))

    pts = [p for p in points_in if p.best_score > -1.5]
    if len(pts) < 3:
        return points_in

    cand_lists = []
    for p in pts:
        cands = list(getattr(p, "candidates", []) or [])
        if not cands:
            cands = [(int(getattr(p, "raw_r", p.best_r)), float(p.best_score))]
        cur = (int(getattr(p, "raw_r", p.best_r)), float(p.best_score))
        if all(raw != cur[0] for raw, _ in cands):
            cands.append(cur)
        by_raw = {}
        for raw, sc in cands:
            raw = int(raw); sc = float(sc)
            if raw not in by_raw or sc > by_raw[raw]:
                by_raw[raw] = sc
        cands = _phase_separated_candidates(
            sorted(by_raw.items(), key=lambda it: it[1], reverse=True),
            T_int, _top_k, _phase_sep)
        if search_periods > 2:
            top_k_set = dict(cands)
            for k in range(search_periods):
                lo, hi = k * T_int, (k + 1) * T_int
                if not any(lo <= r < hi for r in top_k_set):
                    window_best = max(
                        ((r, sc) for r, sc in by_raw.items() if lo <= r < hi),
                        key=lambda x: x[1], default=None)
                    if window_best:
                        top_k_set[window_best[0]] = window_best[1]
            cands = sorted(top_k_set.items(), key=lambda x: x[1], reverse=True)
        cand_lists.append(cands)

    allowed = max(2.0, T_int * BRANCH_GLOBAL_ALLOWED_FACTOR)

    anchor_r = None
    max_abs_drift = None
    if search_periods > 2:
        dense_for_anchor = [p for p in pts if getattr(p, "phase", "") == "dense"
                            and p.best_score > -1.5][:8]
        if dense_for_anchor:
            anchor_r = float(np.median([float(p.best_r) for p in dense_for_anchor]))
            max_abs_drift = max(float(T_int) * 0.75, 8.0)

    states = {}
    for i0, (raw0, sc0) in enumerate(cand_lists[0]):
        tr0 = float(raw0)
        for i1, (raw1, sc1) in enumerate(cand_lists[1]):
            tr1 = closest_branch_copy(int(raw1), tr0, T_int)
            dn01 = max(1, pts[1].n - pts[0].n)
            slope = (tr1 - tr0) / dn01
            raw_jump_init = abs(int(raw1) - int(raw0))
            init_pen = _switch_pen * max(0.0, raw_jump_init - T_int * 0.5) / T_int
            cost = -_score_w * (float(sc0) + float(sc1)) + init_pen
            states[(i0, i1)] = (cost, tr0, tr1, slope, None)

    all_back = []
    for pi in range(2, len(pts)):
        new_states = {}
        back = {}
        n_prev1 = pts[pi - 1].n
        n_cur   = pts[pi].n
        dn_cur  = max(1, n_cur - n_prev1)
        for (i_prev2, i_prev1), (cost_prev, tr_prev2, tr_prev1, slope_prev, _) in states.items():
            pred = tr_prev1 + slope_prev * dn_cur
            for i_cur, (raw_cur, sc_cur) in enumerate(cand_lists[pi]):
                tr_cur = closest_branch_copy(int(raw_cur), pred, T_int)
                err    = tr_cur - pred
                smooth_pen = _smooth_pen * (err / allowed) ** 2

                # phase_pred_pen: currently 0.0 (BRANCH_PHASE_PRED_PENALTY disabled since v143)
                phase_err = abs(shortest_circular_delta(float(pred) % T_int,
                                                        float(raw_cur) % T_int, T_int))
                phase_thresh = max(1.5, float(T_int) * (
                    BRANCH_PHASE_PRED_THRESH_HIGH_HN if harmonic_number >= 64
                    else BRANCH_PHASE_PRED_THRESH_DEFAULT))
                phase_pred_pen = (BRANCH_PHASE_PRED_PENALTY
                                  * ((phase_err - phase_thresh) / phase_thresh) ** 2
                                  if phase_err > phase_thresh else 0.0)

                slope_cur_tent = (tr_cur - tr_prev1) / dn_cur
                slope_change   = slope_cur_tent - slope_prev
                max_slope_ch   = max(0.05, float(T_int) / BRANCH_KINK_REF_DIVISOR)
                kink_pen_raw   = _kink_pen * (slope_change / max_slope_ch) ** 2
                if (getattr(pts[pi],     "phase", "") == "gap"
                        or getattr(pts[pi - 1], "phase", "") == "gap"
                        or getattr(pts[pi - 2], "phase", "") == "gap"):
                    kink_pen_scaled = kink_pen_raw * _kink_gap_sc
                else:
                    kink_pen_scaled = kink_pen_raw
                if n_cur <= _kink_n_limit:
                    _sc_cur = max(0.0, float(getattr(pts[pi], "best_score", 1.0)))
                    kink_pen_scaled *= max(_kink_sc_min, min(1.0, _sc_cur))
                kink_pen = min(kink_pen_scaled, _kink_cap)
                hard_kink_pen = (BRANCH_HARD_KINK_PENALTY
                                 if abs(err) > BRANCH_HARD_KINK_FACTOR * allowed else 0.0)

                raw_prev_r = cand_lists[pi - 1][i_prev1][0]
                raw_jump   = abs(int(raw_cur) - int(raw_prev_r))
                branch_sw_pen = _switch_pen * max(0.0, raw_jump - T_int * 0.5) / T_int
                anchor_pen = (BRANCH_ANCHOR_PENALTY * (abs(tr_cur - anchor_r) / max_abs_drift) ** 2
                              if search_periods > 2 and anchor_r is not None else 0.0)

                cost = (cost_prev - _score_w * float(sc_cur)
                        + smooth_pen + kink_pen + hard_kink_pen
                        + branch_sw_pen + anchor_pen + phase_pred_pen)
                key = (i_prev1, i_cur)
                if key not in new_states or cost < new_states[key][0]:
                    new_states[key] = (cost, tr_prev1, tr_cur, slope_cur_tent, (i_prev2, i_prev1))
                    back[key] = (i_prev2, i_prev1)

        if not new_states:
            return points_in
        all_back.append(back)
        states = new_states

    end_key = min(states, key=lambda k: states[k][0])
    idx_path = [None] * len(pts)
    idx_path[-2], idx_path[-1] = end_key
    cur_key = end_key
    for pi in range(len(pts) - 1, 1, -1):
        prev_key = all_back[pi - 2].get(cur_key)
        if prev_key is None:
            break
        idx_path[pi - 2] = prev_key[0]
        cur_key = prev_key

    if any(i is None for i in idx_path):
        return points_in

    out = []
    prev_track = prevprev_track = prev_n = prevprev_n = None
    for pt, cands, ci in zip(pts, cand_lists, idx_path):
        raw, sc = cands[int(ci)]
        if prev_track is None:
            pred_r = None;  track = float(raw)
        elif prevprev_track is None:
            pred_r = prev_track
            track  = closest_branch_copy(int(raw), pred_r, T_int)
        else:
            slope  = (prev_track - prevprev_track) / max(1, prev_n - prevprev_n)
            pred_r = prev_track + slope * max(1, pt.n - prev_n)
            track  = closest_branch_copy(int(raw), pred_r, T_int)

        best_r_out = _project_track_to_release_window(track, T_int, r_max)
        npnt = LutPoint(n=pt.n, loop_pos=pt.loop_pos,
                        best_r=best_r_out, best_score=float(sc), phase=pt.phase,
                        raw_r=int(raw), raw_score=float(sc),
                        folded=False, fold_ratio=1.0)
        npnt.track_r       = float(track)
        npnt.predicted_r   = None if pred_r is None else float(pred_r)
        npnt.pred_error    = 0.0 if pred_r is None else abs(float(track) - float(pred_r))
        npnt.candidates    = cands
        npnt.all_candidates = list(getattr(pt, 'all_candidates', None) or cands)
        npnt.debug_chosen_raw   = int(raw)
        npnt.debug_chosen_index = int(ci)
        npnt.debug_candidates   = [{"raw": r, "score": float(s)} for r, s in cands]
        npnt.dp_full_trace = []
        out.append(npnt)
        prevprev_track, prev_track = prev_track, track
        prevprev_n,     prev_n     = prev_n, pt.n

    return out


def compute_lut(attack_mono: np.ndarray, release_mono: np.ndarray,
                T_float: float, T_int: int,
                crossfade_len_samples: int,
                harmonic_number: int,
                loop_start: int, loop_end: int,
                min_sample: int = 0,
                max_sample: Optional[int] = None,
                downsampling: bool = True) -> tuple:
    """
    Nachbau von ComputeCorrelationLut. Rückgabe: (points, meta)
    v24: Klassischer Dense-Loop für alle Pfeifen.
         Drift-Erkennung nur als Diagnostik — bei Drift: leere LUT, Legacy-Fallback.
    """
    loop_len    = loop_end - loop_start + 1
    release_len = len(release_mono)
    window_len  = crossfade_len_samples

    meta = {
        "stabilized": False, "stable_at_n": None,
        "drift_mode": False, "drift_per_period": 0.0,
        "drift_residual": 0.0, "max_interp_gap_n": 0,
        "dense_step_used": DENSE_STEP,
    }

    if window_len < 4 or loop_len < window_len or release_len < window_len:
        return [], meta
    search_periods = SMALL_T_SEARCH_PERIODS if T_int < SMALL_T_THRESHOLD else DEFAULT_SEARCH_PERIODS
    r_max = min(search_periods * T_int, release_len - window_len)
    meta["r_search_max"]    = r_max
    meta["search_periods"]  = search_periods
    if r_max == 0 or int(loop_len / T_float) < 4:
        return [], meta

    ds           = (min(4, T_int // 500) if T_int >= 500 else 1) if downsampling else 1
    atk_full_len = len(attack_mono)
    n_total      = max(1, int((atk_full_len - window_len) / T_float))

    min_sample = max(0, min_sample)
    if max_sample is not None:
        max_sample = max(min_sample + 1, max_sample)
    n_start = max(1, int(math.ceil(min_sample / T_float)))
    n_end   = n_total if max_sample is None else min(n_total, int(math.ceil(max_sample / T_float)) + 2)
    if n_start >= n_end:
        n_start, n_end = 1, n_total

    loop_needed  = min(int(round((n_end - 1) * T_float)) + window_len, atk_full_len)
    loop_seg     = attack_mono[:loop_needed:ds].astype(np.float32)
    release_ds   = release_mono[:r_max + window_len + 1 : ds].astype(np.float32)
    window_len_d = max(4, window_len // ds)
    r_max_d      = max(1, r_max // ds)

    def _fit_track(points_subset):
        pts = [p for p in points_subset if getattr(p, "track_r", None) is not None
               and p.best_score > -1.5]
        if len(pts) < 2:
            return 0.0, float(getattr(pts[-1], "track_r", 0.0)) if pts else 0.0, float("inf")
        ns_fit = np.array([p.n for p in pts], dtype=float)
        rs_fit = np.array([float(p.track_r) for p in pts], dtype=float)
        a, b = np.polyfit(ns_fit, rs_fit, 1)
        resid = float(np.max(np.abs(rs_fit - (a * ns_fit + b))))
        return float(a), float(b), resid

    def _predict_track_r(points_subset, n: int):
        pts = [p for p in points_subset if getattr(p, "track_r", None) is not None
               and p.best_score > -1.5]
        if not pts:
            return None
        if len(pts) == 1:
            return float(pts[-1].track_r)
        fit_pts = pts[-BRANCH_FIT_WIN:]
        a, b, _ = _fit_track(fit_pts)
        return a * float(n) + b

    def corr_at(n: int, phase: str, predicted_r: Optional[float] = None) -> LutPoint:
        if n < n_start or n >= n_end:
            return LutPoint(n=n, loop_pos=0, best_r=0, best_score=-2.0, phase=phase)
        cs   = int(round(n * T_float))
        cs_d = cs // ds
        if cs_d + window_len_d > len(loop_seg):
            return LutPoint(n=n, loop_pos=cs, best_r=0, best_score=-2.0, phase=phase)
        lw = loop_seg[cs_d:cs_d + window_len_d]

        raw_cands_d, _scores_d = _corr_scores_and_candidates(
            lw, release_ds, r_max_d, window_len_d)
        candidates = _ensure_per_window_candidates(
            raw_cands_d, _scores_d, max(1, T_int // ds), search_periods)

        # Downsampling zurück auf echte Sample-Offsets.
        candidates = [(int(r_d * ds), float(sc)) for r_d, sc in candidates]
        candidates_all = list(candidates)   # vor Phase-Trennung für Tuning Lab
        candidates = _phase_separated_candidates(candidates, T_int, BRANCH_TOP_K)
        best_raw, best_score = max(candidates, key=lambda item: item[1])

        if predicted_r is None:
            chosen_raw = best_raw
            chosen_score = best_score
            chosen_track = float(chosen_raw)
            pred_error = 0.0
        else:
            # Kandidaten nach Score UND Nähe zum vorhergesagten Ast bewerten.
            # v69: Wenn ein Kandidat im engen Korridor um den vorhergesagten
            # Ast liegt, hat dieser Korridor Vorrang. Das verhindert den
            # typischen End-of-WAV-Astwechsel, bei dem ein Nachbarast lokal
            # minimal besser ist, obwohl der bisherige Ast linear stabil war.
            # Hinweis: closest_branch_copy ist hier nur Vorhersagehilfe im
            # lokalen Vorwärts-Pass. Der globale Pfad (_global_branch_path)
            # verwendet keine T-Kopien mehr (realer [0,2T)-Raum).
            allowed = max(2.0, T_int / 10.0)
            enriched = []
            for raw_r, sc in candidates:
                track_r = closest_branch_copy(raw_r, predicted_r, T_int)
                dist = abs(track_r - predicted_r)
                enriched.append((dist, raw_r, sc, track_r))

            hard_limit = allowed * BRANCH_HARD_LOCK_FACTOR
            near = [e for e in enriched if e[0] <= hard_limit]
            if near:
                # Innerhalb des richtigen Ast-Korridors entscheidet wieder der Score.
                dist, chosen_raw, chosen_score, chosen_track = max(near, key=lambda e: e[2])
                pred_error = dist
            else:
                best_eff = -1e30
                chosen_raw = best_raw
                chosen_score = best_score
                chosen_track = closest_branch_copy(best_raw, predicted_r, T_int)
                pred_error = abs(chosen_track - predicted_r)
                for dist, raw_r, sc, track_r in enriched:
                    eff = sc - BRANCH_PREDICT_PENALTY * (dist / allowed) ** 2
                    if eff > best_eff:
                        best_eff = eff
                        chosen_raw = raw_r
                        chosen_score = sc
                        chosen_track = track_r
                        pred_error = dist

        pt = LutPoint(n=n, loop_pos=cs, best_r=int(chosen_raw),
                      best_score=float(chosen_score), phase=phase,
                      raw_r=int(chosen_raw), raw_score=float(best_score),
                      folded=False, fold_ratio=1.0)
        # Nicht Teil der Dataclass, nur interne Diagnose/Tracking-Information.
        pt.track_r = float(chosen_track)
        pt.predicted_r = None if predicted_r is None else float(predicted_r)
        pt.pred_error = float(pred_error)
        pt.candidates = candidates
        pt.all_candidates = candidates_all
        return pt

    def _global_branch_path(points_in: list) -> list:
        """Thin wrapper — delegates to module-level _run_global_branch_dp."""
        return _run_global_branch_dp(points_in, T_int, r_max, search_periods, harmonic_number)

    def _assign_approach_flags(points_in: list) -> None:
        """Setzt pro Punkt die Segmentrichtung vom Vorgänger zu diesem Punkt.

        Die Richtung wird aus dem entfalteten track_r abgeleitet und bleibt auch
        nach Folding/Pruning erhalten. Dadurch kann eine gefaltete LUT später
        eindeutig interpoliert werden, ohne auf den kuerzesten Kreisweg raten zu
        muessen. Fuer Punkt 0 ist das Flag bedeutungslos.
        """
        if not points_in:
            return
        pts = sorted(points_in, key=lambda p: p.loop_pos)
        pts[0].approach_up = True
        for a, b in zip(pts, pts[1:]):
            ta = float(getattr(a, "track_r", a.best_r))
            tb = float(getattr(b, "track_r", b.best_r))
            b.approach_up = (tb - ta) >= 0.0

    def _try_fold(points_in: list) -> tuple:
        """Prueft, ob der entfaltete track_r-Pfad als gefaltete LUT
        mit einem Richtungsbit pro Segment eindeutig darstellbar ist.

        v137: Die alte Score-Drop-Pruefung gegen raw_r % T ist hier falsch
        geworden, weil approach_up die Segmentrichtung konserviert. Entscheidend
        ist nicht, ob ein gleicher Score bei der ersten T-Kopie existiert,
        sondern ob die geprunte Kurve im gefalteten Raum mit genau einem
        gerichteten Umlauf pro Segment rekonstruierbar bleibt.
        """
        if not points_in:
            return False, "no_points"
        if T_int <= 0:
            return False, "bad_T"
        pts_f = sorted(points_in, key=lambda p: p.loop_pos)
        if len(pts_f) < 2:
            return True, "single_point"

        tol = max(1.5, float(T_int) / 8.0)
        for a, b in zip(pts_f, pts_f[1:]):
            ta = float(getattr(a, "track_r", a.best_r))
            tb = float(getattr(b, "track_r", b.best_r))
            track_delta = tb - ta

            # Ein einzelnes approach_up-Bit kann nur einen gerichteten Weg
            # innerhalb einer Periode beschreiben. Wenn Pruning ein Segment mit
            # mehr als einem Umlauf erzeugt, darf die Kurve nicht gefaltet werden.
            if abs(track_delta) > float(T_int) + tol:
                return False, "multiwrap_segment"

            r0 = int(round(ta)) % T_int
            r1 = int(round(tb)) % T_int
            up = track_delta >= 0.0
            folded_delta = _directed_delta_folded(r0, r1, up, T_int)
            if abs(folded_delta - track_delta) > tol:
                return False, "directed_mismatch"

        return True, "directed_fold_ok"

    # Tighter tolerance for non-octave stops (aliquots/mixtures), matching GO.
    stable_thresh = max(T_int // (8 if corr_is_octave_stop(harmonic_number) else 6), 4)
    points: list  = []

    # ── Kurzer Loop ───────────────────────────────────────────────────────────
    if n_total <= 30:
        span = max(0, n_end - n_start)
        for i in range(min(10, span)):
            p = n_start + i * max(1, (span - 1) // 9)
            if p < n_end:
                pred = _predict_track_r(points, p)
                points.append(corr_at(p, "dense", pred))
        points = [p for p in points if p.best_score > -1.5]
        meta["stabilized"] = True
        return points, meta

    # ── Phase 1: Dense ────────────────────────────────────────────────────────
    dense_start = max(n_start, 5)
    # dense_step dynamisch: so dass mindestens STABLE_WIN+1 Punkte in den
    # verfügbaren Bereich passen. Bei kurzen Releases (rel00220 mit wenig n)
    # kann DENSE_STEP=6 zu wenige Punkte liefern.
    available_n = max(1, n_end - dense_start)
    dense_step  = min(DENSE_STEP, max(1, available_n // (STABLE_WIN + 1)))
    meta["dense_step_used"] = dense_step

    dense_stop  = min(n_end, dense_start + dense_step * BRANCH_WARMUP_POINTS)
    stable_at_n = None

    for n in range(dense_start, dense_stop, dense_step):
        pred = _predict_track_r(points, n)
        pt = corr_at(n, "dense", pred)
        if pt.best_score > -1.5:
            points.append(pt)
        if len(points) >= STABLE_WIN:
            _, _, resid = _fit_track(points[-STABLE_WIN:])
            ok = resid <= stable_thresh * BRANCH_STABLE_RESID_FACTOR
            if ok and stable_at_n is None:
                stable_at_n = n
                # Nicht abbrechen: fuer Branch-Tracking brauchen wir mehr als
                # nur STABLE_WIN Punkte. Sonst ist die Sparse-Extrapolation
                # ueber tausende Perioden zu schlecht bestimmt.

    if not points and n_start < n_end:
        points.append(corr_at(min(n_end - 1, dense_start), "dense"))

    # ── Drift-Diagnose (nur Diagnostik, kein Einfluss auf LUT-Berechnung) ────
    # Für die ersten DRIFT_FIT_WIN Dense-Punkte: linearer Fit auf unwrapped best_r
    drift_slope, drift_resid = 0.0, 0.0
    diag_pts = [p for p in points if p.best_score > -1.5][:DRIFT_FIT_WIN]
    if len(diag_pts) >= 4:
        a, b, resid = _fit_track(diag_pts)
        drift_slope = a
        drift_resid = resid

    # Drift ist problematisch wenn r sich innerhalb von STABLE_WIN * DENSE_STEP
    # Perioden um mehr als T/4 bewegt — dann kann die normale LUT nicht folgen.
    # slope_thresh = T / (4 * STABLE_WIN * DENSE_STEP)
    drift_slope_thresh = T_int / (4.0 * STABLE_WIN * DENSE_STEP)
    drift_detected = (abs(drift_slope) >= drift_slope_thresh and
                      drift_resid <= stable_thresh * DRIFT_MAX_RESID_FACTOR)

    if drift_detected:
        # Drift erkannt → LUT-Punkte trotzdem behalten für Visualisierung,
        # aber drift_mode=True signalisiert GO/analyze_pipe: Legacy verwenden.
        meta["drift_mode"]       = True
        meta["drift_per_period"] = drift_slope
        meta["drift_residual"]   = drift_resid
        meta["stabilized"]       = False
        meta["stable_at_n"]      = None
        # Weiter mit Phase 2+3 für vollständige Visualisierung

    meta["stabilized"]       = stable_at_n is not None
    meta["stable_at_n"]      = stable_at_n
    meta["drift_per_period"] = drift_slope
    meta["drift_residual"]   = drift_resid

    # ── Phase 2: Adaptive Sparse ───────────────────────────────────────────────
    def _append_adaptive_target(n_target: int, phase: str, depth: int = 0):
        """Fuegt n_target ein, aber nur wenn der Kandidat zum vorhergesagten
        Ast passt. Bei grosser Abweichung wird zuerst der Mittelpunkt gemessen.

        Das ist der entscheidende Unterschied zum alten Sparse-Verfahren:
        grosse n-Spruenge werden nicht blind akzeptiert. Dadurch bleiben die
        lokalen Maxima auf demselben Ast, auch wenn Nachbaraeste lokal aehnliche
        oder minimal bessere Scores haben.
        """
        nonlocal points
        if len(points) >= MAX_TOTAL or n_target <= 0 or n_target >= n_end:
            return
        if points and n_target <= points[-1].n:
            return

        pred = _predict_track_r(points, n_target)
        pt = corr_at(n_target, phase, pred)

        need_mid = False
        if pred is not None and getattr(pt, "track_r", None) is not None and points:
            allowed = max(2.0, T_int / 10.0)
            err = abs(float(pt.track_r) - float(pred))
            dn = n_target - points[-1].n
            if err > allowed * BRANCH_ADAPT_ERR_FACTOR and dn > 1:
                need_mid = True

        if need_mid and depth < BRANCH_ADAPT_MAX_DEPTH and len(points) < MAX_TOTAL - 1:
            nm = (points[-1].n + n_target) // 2
            if nm > points[-1].n and nm < n_target:
                _append_adaptive_target(nm, "gap", depth + 1)
                if len(points) >= MAX_TOTAL:
                    return
                # Nach dem Zwischenpunkt ist die Vorhersage besser; Ziel neu messen.
                pred = _predict_track_r(points, n_target)
                pt = corr_at(n_target, phase, pred)

        if pt.best_score > -1.5:
            points.append(pt)

    last_n = int(round(points[-1].loop_pos / T_float)) if points else dense_start
    if last_n + 1 < n_end and len(points) < MAX_TOTAL:
        n_rem = min(N_SPARSE, MAX_TOTAL - len(points))
        targets = []
        for i in range(1, n_rem + 1):
            n = last_n + i * (n_end - 1 - last_n) // n_rem
            if n > last_n and n < n_end:
                targets.append(n)
        # Monoton und ohne Duplikate.
        for n in sorted(set(targets)):
            if len(points) >= MAX_TOTAL:
                break
            _append_adaptive_target(n, "sparse", 0)

    # ── Phase 3: Gap-Fill ─────────────────────────────────────────────────────
    # v118: Gap-Fill gedrosselt. Die alte Schleife konnte bei verrauschten
    # lokalen Peaks einen grossen Sparse-Abstand bis auf dn=1 herunterteilen.
    # Das ist teuer und liefert meistens nur Quantisierungs-/Korrelationsrauschen.
    # Deshalb:
    #   - kein Gap-Fill mehr unter GAP_FILL_MIN_DN Perioden Abstand,
    #   - globales Insert-Limit,
    #   - raw_gap allein triggert nur am Anfang; sonst entscheidet die
    #     Abweichung vom vorhergesagten Track (track_gap).
    gap_thresh = T_int // 4
    gap_err_thresh = max(2.0, T_int / 10.0)
    idx = 0
    gap_inserts = 0
    while idx + 1 < len(points) and len(points) < MAX_TOTAL and gap_inserts < GAP_FILL_MAX_INSERTS:
        # v82: Gap-Erkennung im echten [0, 2T)-Raum via track_r.
        # Kein % T_int mehr — track_r ist der branch-konsistente Pfadwert.
        dn_pair = max(1, points[idx + 1].n - points[idx].n)
        if dn_pair < GAP_FILL_MIN_DN:
            idx += 1
            continue

        track_r_a = getattr(points[idx],     "track_r", float(points[idx].best_r))
        track_r_b = getattr(points[idx + 1], "track_r", float(points[idx + 1].best_r))
        raw_gap = abs(track_r_b - track_r_a)
        track_gap = 0.0
        have_prediction = False
        if idx >= 1:
            track_r_prev = getattr(points[idx - 1], "track_r", None)
            if track_r_prev is not None:
                dn0 = max(1, points[idx].n - points[idx - 1].n)
                slope0 = (track_r_a - float(track_r_prev)) / dn0
                pred_next = track_r_a + slope0 * dn_pair
                track_gap = abs(track_r_b - pred_next)
                have_prediction = True

        should_fill = (track_gap > gap_err_thresh) if have_prediction else (raw_gap > gap_thresh)
        if should_fill:
            na = int(round(points[idx].loop_pos / T_float))
            nb = int(round(points[idx+1].loop_pos / T_float))
            nm = (na + nb) // 2
            if nm > na and nm < nb:
                pred = _predict_track_r(points, nm)
                pt = corr_at(nm, "gap", pred)
                if pt.best_score > -1.5:
                    points.insert(idx + 1, pt)
                    gap_inserts += 1
                    continue
        idx += 1

    # ── Phase 4: Branch-Lock-Reparatur ───────────────────────────────────────
    # Nach Sparse+Gap kann besonders am Ende ein falscher Ast die Vorhersage
    # kapern. Daher messen wir vorhandene Punkte noch einmal in Reihenfolge und
    # zwingen sie an den aus den vorherigen Punkten vorhergesagten Ast, falls
    # sie deutlich danebenliegen. Es werden keine neuen Punkte verbraucht; der
    # Punkt wird nur durch den besten Kandidaten auf dem bestehenden Ast ersetzt.
    repaired = []
    for pt in points:
        if len(repaired) >= max(3, min(BRANCH_FIT_WIN, len(points))):
            pred = _predict_track_r(repaired, pt.n)
            if pred is not None:
                allowed = max(2.0, T_int / 10.0)
                old_err = abs(float(getattr(pt, "track_r", pt.raw_r)) - float(pred))
                if old_err > allowed * BRANCH_ADAPT_ERR_FACTOR:
                    trial = corr_at(pt.n, pt.phase, pred)
                    new_err = abs(float(getattr(trial, "track_r", trial.raw_r)) - float(pred))
                    # Ersatz nur, wenn er klar naeher am Ast liegt und der Score
                    # nicht voellig einbricht. Bei vielen Mixturen ist der richtige
                    # Ast lokal etwas schwaecher; das ist erlaubt.
                    if new_err < old_err and trial.best_score >= pt.best_score - BRANCH_SCORE_MARGIN:
                        pt = trial
        repaired.append(pt)
    points = repaired

    # v72: globale Branch-Pfad-Nachbearbeitung. Diese Stufe korrigiert
    # Astspruenge, die lokal plausibel aussehen, global aber einen Knick im
    # ansonsten glatten Pfad erzeugen.
    points = _global_branch_path(points)

    # v137: Folding erst nach Pruning entscheiden.
    # Vorher nur ungueltige Punkte entfernen. Die Qualitaetsbewertung passiert
    # weiterhin vor Pruning, aber die Fold-Entscheidung muss auf der finalen
    # geprunten Kurve stattfinden, weil Pruning sonst die noetigen Wrap-
    # Zwischenpunkte entfernen und damit die Segmentrichtung veraendern kann.
    points = [p for p in points if p.best_score > -1.5]

    # v121: Qualitaets-Scores vor Pruning bestimmen. Minimum/p10 einzelner
    # Transientenpunkte sind als Warnkriterium zu hart; Median und Anteil
    # schwacher Punkte nach Stabilisierung sind robuster.
    if stable_at_n is not None:
        quality_pts = [p for p in points if p.n >= stable_at_n]
    else:
        # Fallback: dichte Startphase ignorieren, weil sie den Einschwingvorgang
        # enthaelt und nicht zwingend gut korrelieren muss.
        quality_pts = [p for p in points if p.phase in ("sparse", "gap")]
        if not quality_pts:
            quality_pts = list(points)
    quality_scores = [float(p.best_score) for p in quality_pts if p.best_score > -1.5]
    if quality_scores:
        q_sorted = sorted(quality_scores)
        p10_idx = max(0, int(len(q_sorted) * 0.10) - 1)
        meta["score_p10"] = float(q_sorted[p10_idx])
        meta["score_median"] = float(np.median(q_sorted))
        meta["low_score_fraction"] = float(sum(1 for sc in q_sorted if sc < SCORE_BAD) / len(q_sorted))
        meta["quality_score_count"] = int(len(q_sorted))
    else:
        meta["score_p10"] = 0.0
        meta["score_median"] = 0.0
        meta["low_score_fraction"] = 0.0
        meta["quality_score_count"] = 0

    # v82: LUT-Pruning: redundante Punkte entfernen.
    def _prune_lut(pts: list) -> list:
        if len(pts) <= 2:
            return pts
        all_scores = [p.best_score for p in pts]
        mean_score = sum(all_scores) / len(all_scores) if all_scores else 0.0
        keep = [True] * len(pts)
        track_rs_prune = [float(getattr(p, "track_r", p.best_r)) for p in pts]
        ns_prune = [p.n for p in pts]
        for i in range(1, len(pts) - 1):
            n_prev = ns_prune[i - 1]
            n_next = ns_prune[i + 1]
            n_cur  = ns_prune[i]
            dn = max(1, n_next - n_prev)
            t_frac = (n_cur - n_prev) / dn
            r_interp = track_rs_prune[i - 1] + t_frac * (track_rs_prune[i + 1] - track_rs_prune[i - 1])
            tol = max(1.5, T_int / 200.0)
            if abs(track_rs_prune[i] - r_interp) > tol:
                continue
            pt = pts[i]
            if pt.best_score < mean_score - 0.15:
                continue
            # Gap-Punkte nur behalten wenn sie einen echten Sprung abdecken.
            if pt.phase == "gap":
                span = abs(track_rs_prune[i + 1] - track_rs_prune[i - 1])
                if span > T_int / 4.0:
                    continue
            if (n_cur - ns_prune[i - 1]) > MAX_PRUNE_GAP_N or (ns_prune[i + 1] - n_cur) > MAX_PRUNE_GAP_N:
                continue
            if abs(track_rs_prune[i] - track_rs_prune[i - 1]) > T_int / 4.0:
                continue
            if abs(track_rs_prune[i + 1] - track_rs_prune[i]) > T_int / 4.0:
                continue
            keep[i] = False
        return [pts[i] for i in range(len(pts)) if keep[i]]

    meta["pruned_count"] = 0
    if len(points) > 2:
        n_before_prune = len(points)
        points = _prune_lut(points)
        meta["pruned_count"] = n_before_prune - len(points)

    # v125/v137: Nach dem Pruning die Segmentrichtung neu aus dem erhaltenen
    # entfalteten track_r ableiten. Das ist noetig, weil entfernte
    # Zwischenpunkte sonst bei gefalteten LUTs die Wrap-Richtung verlieren.
    _assign_approach_flags(points)

    # v137: Fold-Entscheidung jetzt auf der finalen geprunten Kurve und anhand
    # der gerichteten Segmentgeometrie, nicht anhand eines Score-Drops bei
    # raw_r % T. Ein gefalteter Punkt speichert nur die Phase; approach_up
    # speichert den Weg vom Vorgaenger zu diesem Punkt.
    can_fold, fold_reason = _try_fold(points)
    if can_fold:
        for pt in points:
            if not hasattr(pt, "debug_chosen_raw"):
                pt.debug_chosen_raw = int(pt.best_r)
            pt.debug_best_r_before_fold = int(pt.best_r)
            pt.best_r = int(pt.best_r) % T_int
            pt.raw_r  = int(pt.raw_r) % T_int
            pt.folded = True
            pt.fold_ratio = 1.0
        meta["folded"] = True
        meta["fold_reason"] = fold_reason
    else:
        for pt in points:
            pt.folded = False
            pt.fold_ratio = 1.0
        meta["folded"] = False
        meta["fold_reason"] = fold_reason

    # Flags nach dem eventuellen best_r-Modulo nochmals setzen; track_r bleibt
    # unveraendert und ist die Quelle der Richtung.
    _assign_approach_flags(points)

    if len(points) >= 4:
        a, b, resid = _fit_track(points)
        meta["drift_per_period"] = a
        meta["drift_residual"] = resid
        # Stabilisierung bedeutet ab v72: verwertbarer glatter Pfad, nicht
        # zwingend konstante Phase. Linearer Drift ist erlaubt.
        meta["stabilized"] = resid <= max(2.0, T_int / 8.0)
        meta["stable_at_n"] = points[min(len(points)-1, STABLE_WIN-1)].n
    return points, meta


# ─── ODF-Parser ──────────────────────────────────────────────────────────────

def parse_organ_file(organ_path: str) -> list:
    """
    Parst .organ-Datei und gibt Liste von Pipe-Deskriptoren zurück.
    Jeder Eintrag: dict mit rank_name, midi_note, attack_path, releases,
                   harmonic_number, crossfade_len_ms, perspective
    """
    organ_dir = os.path.dirname(os.path.abspath(organ_path))
    pipes = []

    try:
        with open(organ_path, encoding="utf-8", errors="replace") as f:
            content = f.read()
    except Exception as e:
        return []

    # Finde alle Rank-Sektionen
    sections = {}
    current_section = None
    for line in content.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            current_section = line[1:-1]
            sections[current_section] = {}
        elif "=" in line and current_section:
            k, _, v = line.partition("=")
            sections[current_section][k.strip()] = v.strip()

    # Finde Rank-Sektionen
    rank_pattern = re.compile(r"^Rank\d+$")
    for sec_name, sec_data in sections.items():
        if not rank_pattern.match(sec_name):
            continue

        rank_name = sec_data.get("Name", sec_name)
        # Globaler HarmonicNumber-Default (kann per Pipe ueberschrieben werden)
        global_harmonic = int(sec_data.get("HarmonicNumber", "8"))
        xfade_ms  = int(sec_data.get("ReleaseCrossfadeLength", "0"))
        first_key = int(sec_data.get("FirstMidiNoteNumber",
                        sec_data.get("FirstAccessibleKeyMIDINoteNumber", "36")))
        pipe_count = int(sec_data.get("NumberOfLogicalPipes", "0"))
        if pipe_count == 0:
            pipe_count = sum(1 for k in sec_data if re.match(r"^Pipe\d+$", k))

        for pipe_idx in range(1, pipe_count + 1):
            pipe_key = f"Pipe{pipe_idx:03d}"
            attack_rel = sec_data.get(pipe_key)
            if not attack_rel:
                continue
            midi_note = first_key + pipe_idx - 1
            # Per-Pipe HarmonicNumber hat Vorrang vor globalem Wert
            harmonic = int(sec_data.get(f"{pipe_key}HarmonicNumber",
                                        str(global_harmonic)))

            # Releases sammeln
            releases = {}
            for k, v in sec_data.items():
                m = re.match(rf"^{pipe_key}Release(\d+)$", k)
                if m:
                    rel_idx = int(m.group(1))
                    releases[rel_idx] = v

            # Pfad auflösen
            def resolve(rel_path):
                rel_norm = rel_path.replace("\\", "/").lstrip("/")
                full = os.path.normpath(os.path.join(organ_dir, rel_norm.replace("/", os.sep)))
                return full if os.path.isfile(full) else None

            atk_path = resolve(attack_rel)
            if not atk_path:
                continue

            # Perspektive aus Pfad ableiten
            parts = attack_rel.replace("\\", "/").split("/")
            perspective = "default"
            for p in parts:
                if re.match(r"^\d{2}-", p):
                    perspective = p
                    break

            release_infos = []
            for rel_idx, rel_path in releases.items():
                rel_full = resolve(rel_path)
                if not rel_full:
                    continue
                rel_parent = os.path.basename(os.path.dirname(rel_full))
                mkpt_entry = sec_data.get(
                    f"{pipe_key}Release{rel_idx:03d}MaxKeyPressTime", "")
                if mkpt_entry:
                    max_key_ms = int(mkpt_entry)
                else:
                    m = re.match(r"rel(\d+)", rel_parent)
                    max_key_ms = int(m.group(1)) if m else None
                # GO: MaxKeyPressTime=-1 bedeutet "kein Limit" (laengster Release).
                # Ebenso werden sehr grosse Werte (>=99999) als "kein Limit" behandelt.
                if max_key_ms is not None and (max_key_ms < 0 or max_key_ms >= 99999):
                    max_key_ms = None
                release_infos.append({
                    "rel_idx": rel_idx,
                    "rel_path": rel_full,
                    "rel_type": rel_parent,
                    "max_key_ms": max_key_ms,
                })

            release_infos.sort(key=lambda info: info["max_key_ms"]
                               if info["max_key_ms"] is not None else float("inf"))
            prev_max_ms = 0
            for rel_idx_loop, info in enumerate(release_infos):
                rel_full = info["rel_path"]
                rel_parent = info["rel_type"]
                max_key_ms = info["max_key_ms"]
                min_key_ms = prev_max_ms
                if max_key_ms is not None:
                    prev_max_ms = max_key_ms

                pipes.append({
                    "rank_name":   rank_name,
                    "midi_note":   midi_note,
                    "perspective": perspective,
                    "release_type": rel_parent,
                    "attack_path": atk_path,
                    "release_path": rel_full,
                    "harmonic_number": harmonic,
                    "crossfade_len_ms": int(sec_data.get(
                        f"{pipe_key}ReleaseCrossfadeLength",
                        str(xfade_ms))),
                    "min_key_press_ms": min_key_ms,
                    "max_key_press_ms": max_key_ms,
                    "is_shortest_release": rel_idx_loop == 0,
                    "organ_base":  organ_dir,
                })
                if max_key_ms is None:
                    break

    return pipes


# ─── Analyse-Worker ───────────────────────────────────────────────────────────

def analyze_pipe(desc: dict) -> PipeAnalysis:
    pa = PipeAnalysis(
        organ_base=desc["organ_base"],
        rank_name=desc["rank_name"],
        midi_note=desc["midi_note"],
        perspective=desc["perspective"],
        release_type=desc["release_type"],
        attack_path=desc["attack_path"],
        release_path=desc["release_path"],
        harmonic_number=desc["harmonic_number"],
    )

    try:
        # smpl-Chunk lesen
        smpl = parse_smpl_chunk(pa.attack_path)
        if smpl["midi_note"] is None:
            pa.error = "Kein smpl-Chunk"
            return pa

        midi_note  = smpl["midi_note"]
        pitch_frac = smpl["pitch_frac"] / 2**32  # in Semitones
        # smpl-Chunk gibt die physikalische Schwingungsfrequenz der aufgenommenen
        # Pfeife. Für HN=8 ist das identisch mit der Tastennoten-Frequenz.
        # Für andere HN (z.B. HN=24, 2⅔'-Quinte) ist die Pfeife jedoch bei
        # HN/8-fachem der Tastenfrequenz gestimmt, weshalb die korrekte
        # Arbeitsperiode für die LUT-Korrelation lautet:
        #   T_hn = T_smpl * HN / 8
        # Nur T_hn gibt die Periodik an, bei der GrandOrgue den Crossfade ausrichtet.
        freq_hz    = 440.0 * 2**((midi_note + pitch_frac - 69) / 12)

        # WAV laden
        atk_mono, sr, atk_frames, atk_ch = read_wav_mono_float(pa.attack_path)
        rel_mono, _,  rel_frames, rel_ch  = read_wav_mono_float(pa.release_path)

        pa.sample_rate  = sr
        smpl_T          = sr / freq_hz
        pa.smpl_T_float = smpl_T
        hn_factor       = pa.harmonic_number / 8.0
        pa.hn_T_float   = smpl_T * hn_factor
        # T_float = HN-korrigierte Tastennoten-Periode (Arbeitsperiode für LUT)
        pa.T_float      = pa.hn_T_float
        pa.T_int        = int(round(pa.T_float))
        pa.release_len  = len(rel_mono)

        if pa.T_int < 16 and not ALLOW_SHORT_PERIOD:
            pa.legacy_fallback = True
            pa.legacy_reason = "short_period"
            pa.error = None
            return pa

        # Loop-Punkte (muss vor Autokorrelation stehen — loop_mid braucht loop_start)
        loops = smpl["loops"]
        if loops:
            pa.loop_start = loops[0][0]
            pa.loop_end   = loops[0][1]
        else:
            pa.loop_start = 0
            pa.loop_end   = len(atk_mono) - 1
        pa.loop_len = pa.loop_end - pa.loop_start + 1

        # Periodenbestimmung per Autokorrelation.
        # Suchbereich [0.5*T_hn, 1.5*T_hn] um die HN-korrigierte Tastennoten-Periode.
        # T_hn ist bereits in pa.T_float — der Suchraum deckt ±50 % ab, was sowohl
        # leichte Verstimmung als auch Oktavmehrdeutigkeit (T/2, 2T) abfängt.
        T_hn_int = pa.T_int  # = round(pa.T_float) = round(T_hn)
        min_p = max(16, int(round(0.5  * T_hn_int)))
        max_p = min(sr // 20, int(round(2.0 * T_hn_int)))

        pa.autocorr_min_p = int(min_p)
        pa.autocorr_max_p = int(max_p)

        if max_p >= min_p * 2 and T_hn_int >= 16:
            loop_mid = pa.loop_start + pa.loop_len // 2
            ac_window = max_p * 8
            autocorr_region = atk_mono[loop_mid:loop_mid + ac_window]
            if len(autocorr_region) >= max_p * 2:
                t_est, diag = estimate_period_by_autocorr(
                    autocorr_region, min_p, max_p, expected_period=pa.hn_T_float)
                pa.autocorr_T_float = float(t_est)
                pa.T_float         = float(t_est)
                pa.T_int           = int(round(pa.T_float))
                pa.cmndf_at_T_half = diag['cmndf_half']
                pa.cmndf_at_T      = diag['cmndf_T']
                pa.cmndf_at_2T     = diag['cmndf_2T']

        pa.n_total  = int(pa.loop_len / pa.T_float)

        # Crossfade-Länge — entspricht GOSoundProviderWave::LoadFromOneFile:
        # releaseCrossfadeLength ? releaseCrossfadeLength : get_fader_length(midiKeyNumber)
        xfade_ms = desc.get("crossfade_len_ms", 0)
        if xfade_ms > 0:
            pa.crossfade_len_samples = int(xfade_ms * sr / 1000)
            pa.crossfade_auto = False
        else:
            auto_ms = _go_default_crossfade_ms(midi_note)
            pa.crossfade_len_samples = int(auto_ms * sr / 1000)
            pa.crossfade_auto = True
        if pa.crossfade_len_samples < 4:
            pa.crossfade_len_samples = 2 * pa.T_int

        # Amplitudenverhältnis (Steady-State)
        ss_start = pa.loop_start
        ss_end   = min(ss_start + 50 * pa.T_int, len(atk_mono))
        pa.atk_rms = float(np.sqrt(np.mean(atk_mono[ss_start:ss_end]**2))) if ss_end > ss_start else 0.0
        rel_early  = rel_mono[:min(2000, len(rel_mono))]
        pa.rel_rms = float(np.sqrt(np.mean(rel_early**2))) if len(rel_early) > 0 else 0.0
        pa.amplitude_ratio = (pa.atk_rms / pa.rel_rms) if pa.rel_rms > 1e-9 else 0.0

        # LUT berechnen
        pa.min_key_press_ms = desc.get("min_key_press_ms")
        pa.max_key_press_ms = desc.get("max_key_press_ms")
        pa.is_shortest_release = bool(desc.get("is_shortest_release", False))
        pa.min_sample = int(pa.min_key_press_ms * sr / 1000) if pa.min_key_press_ms is not None else 0
        pa.max_sample = (int(pa.max_key_press_ms * sr / 1000)
                         if pa.max_key_press_ms is not None else None)

        pa.lut_points, lut_meta = compute_lut(
            attack_mono=atk_mono,
            release_mono=rel_mono,
            T_float=pa.T_float,
            T_int=pa.T_int,
            crossfade_len_samples=pa.crossfade_len_samples,
            harmonic_number=pa.harmonic_number,
            loop_start=pa.loop_start,
            loop_end=pa.loop_end,
            min_sample=pa.min_sample,
            max_sample=pa.max_sample,
            downsampling=desc.get("downsampling", True),
        )
        pa.stabilized = bool(lut_meta.get("stabilized", False))
        pa.stable_at_n = lut_meta.get("stable_at_n")
        pa.drift_mode = bool(lut_meta.get("drift_mode", False))
        pa.drift_per_period = float(lut_meta.get("drift_per_period", 0.0))
        pa.drift_residual = float(lut_meta.get("drift_residual", 0.0))
        pa.max_interp_gap_n = int(lut_meta.get("max_interp_gap_n", 0) or 0)
        pa.dense_step_used = int(lut_meta.get("dense_step_used", DENSE_STEP) or DENSE_STEP)
        pa.lut_folded = bool(lut_meta.get("folded", True))
        pa.lut_fold_reason = str(lut_meta.get("fold_reason", ""))
        pruned_count = int(lut_meta.get("pruned_count", 0) or 0)
        pa.lut_points_raw_count = len(pa.lut_points) + pruned_count
        pa.lut_r_search_max  = int(lut_meta.get("r_search_max",  2 * pa.T_int))
        pa.lut_search_periods = int(lut_meta.get("search_periods", 2))
        pa.lut_score_p10 = float(lut_meta.get("score_p10", 0.0) or 0.0)
        pa.lut_score_median = float(lut_meta.get("score_median", 0.0) or 0.0)
        pa.lut_low_score_fraction = float(lut_meta.get("low_score_fraction", 0.0) or 0.0)
        pa.lut_quality_score_count = int(lut_meta.get("quality_score_count", 0) or 0)

        if not pa.lut_points:
            pa.error = "Keine LUT-Punkte"
            return pa

        # Stabilisierung/Drift wurde bereits in compute_lut ausgewertet.
        dense_pts = [p for p in pa.lut_points if p.phase == "dense"]
        pa.phase3_count = sum(1 for p in pa.lut_points if p.phase == "gap")

        # ── Erweiterte Metriken ────────────────────────────────────────────
        pa.n_dense  = sum(1 for p in pa.lut_points if p.phase == "dense")
        pa.n_sparse = sum(1 for p in pa.lut_points if p.phase == "sparse")
        pa.n_gap    = pa.phase3_count
        pa.freq_hz  = freq_hz
        pa.atk_frames = atk_frames
        pa.rel_frames = rel_frames
        pa.midi_note_str = midi_to_name(pa.midi_note)

        # Steady-state Punkte: sparse + gap (nach Stabilisierung)
        ss_pts = [p for p in pa.lut_points if p.phase in ("sparse", "gap")]
        if ss_pts:
            scores_ss = [p.best_score for p in ss_pts]
            pa.score_mean = float(np.mean(scores_ss))
            pa.score_min  = float(np.min(scores_ss))
            pa.score_max  = float(np.max(scores_ss))

            # Standardabweichung von best_r (Zwei-Ast-Indikator)
            # Kreisstatistik: circ_mean dann circ_std
            rs = np.array([p.best_r for p in ss_pts], dtype=float)
            T  = pa.T_int
            # Kreismittelwert via sin/cos
            angles = rs / T * 2 * np.pi
            mean_sin = np.mean(np.sin(angles))
            mean_cos = np.mean(np.cos(angles))
            pa.bestr_mean = float(np.arctan2(mean_sin, mean_cos) % (2*np.pi) / (2*np.pi) * T)
            # Kreisstreuung: 0 = perfekt konzentriert, T/2 = maximal gestreut
            R = float(np.clip(np.sqrt(mean_sin**2 + mean_cos**2), 0.0, 1.0))
            pa.bestr_coherence = R  # 1=stabil, 0=instabil/mehrere Äste

        # Transient-Ende: n des letzten dense-Punkts
        if dense_pts:
            pa.transient_end_n = dense_pts[-1].n

        # v46: Legacy-Fallback nicht nur bei Drift/Instabilität, sondern auch
        # bei formal stabilisierter, aber qualitativ schlechter LUT.
        # Wichtig: erst NACH Berechnung von score_min, R und phase3_count.
        # v65: Linearer/leicht gekruemmter Drift ist mit Branch-Tracking kein
        # automatischer Legacy-Grund mehr. Legacy nur noch, wenn keine stabile
        # Punktfolge oder eine qualitativ schlechte LUT entsteht.
        # v96: drift_mode=True bedeutet linearer Drift — Branch-Tracker folgt ihm.
        # Kein Legacy-Trigger, auch wenn stable_at_n=None.
        if not pa.stabilized and not pa.drift_mode:
            pa.legacy_fallback = True
            pa.legacy_reason = "instabil"
        else:
            bad_reasons = []
            # v121: Legacy wegen schlechter Score-Qualitaet nur, wenn der typische
            # Score nach Stabilisierung schlecht ist. Einzelne Transientenpunkte
            # duerfen keine Legacy-/Warnlawine ausloesen.
            if (pa.lut_quality_score_count >= 4 and pa.lut_score_median > 0.0
                    and pa.lut_score_median < LEGACY_SCORE_MIN_THRESHOLD):
                bad_reasons.append(f"score_median<{LEGACY_SCORE_MIN_THRESHOLD:.2f}")
            # v72: R und Anzahl der Gap-Punkte sind fuer branch-getrackte,
            # linear driftende Pfade keine verlaesslichen Fehlerkriterien mehr.
            # Ein sauberer linearer Drift verteilt best_r % T ueber den Kreis
            # und kann deshalb ein kleines R erzeugen. Viele Gap-Punkte koennen
            # schlicht notwendige adaptive Stuetzpunkte sein.
            if pa.drift_residual and pa.drift_residual > max(4.0, pa.T_int / 4.0):
                bad_reasons.append("track_residual_high")
            if bad_reasons:
                pa.legacy_fallback = True
                pa.legacy_reason = "bad_lut:" + ",".join(bad_reasons)

    except Exception as e:
        pa.error = str(e)

    return pa


def circ_interp(r_a: int, r_b: int, t: float, T: int) -> int:
    """Kreislineare Interpolation zwischen r_a und r_b auf [0,T). Kürzester Weg."""
    delta = shortest_circular_delta(float(r_a), float(r_b), T)
    return int(round(r_a + delta * t)) % T


def _directed_delta_folded(r0: int, r1: int, approach_up: bool, T: int) -> float:
    """Gerichtete Phasendifferenz r0 -> r1 im gefalteten Raum.

    approach_up=True  : positive Richtung über 0/T falls nötig.
    approach_up=False : negative Richtung über 0/T falls nötig.
    """
    if T <= 0:
        return float(r1 - r0)
    r0 = int(r0) % T
    r1 = int(r1) % T
    if approach_up:
        return float((r1 - r0) % T)
    return -float((r0 - r1) % T)


def _project_track_to_release_window(track_r: float, T: int, r_max: Optional[int] = None) -> int:
    """Projiziert einen entfalteten Track-Wert in den realen Release-Suchraum.

    Der DP-Track darf unter 0 oder ueber r_max laufen. Fuer die tatsaechliche
    Release-Startposition muss derselbe Phasenpunkt in das berechnete Suchfenster
    zurueckgelegt werden. Das ist keine Interpolation im sichtbaren best_r-Raum,
    sondern eine reine Projektion der bereits entschiedenen Track-Position.
    """
    if T <= 0:
        return int(round(track_r))
    y = float(track_r)
    if r_max is None or r_max <= 0:
        r_max = 2 * T
    # Mit Sicherheitslimit gegen pathologische Werte.
    for _ in range(32):
        if y < 0:
            y += T
        elif y >= r_max:
            y -= T
        else:
            break
    # Falls r_max nicht ganzzahliges Vielfaches von T ist, kann ein Wert am Rand
    # uebrig bleiben. Dann hart in den gueltigen Bereich ziehen.
    if y < 0:
        y = 0.0
    if y >= r_max:
        y = float(max(0, r_max - 1))
    ri = int(round(y))
    if ri < 0:
        ri = 0
    if ri >= r_max:
        ri = max(0, int(r_max) - 1)
    return ri


def get_position_for_correlation(loop_pos: int, lut_points: list, T: int,
                                     folded: bool = True,
                                     r_max: Optional[int] = None) -> int:
    """Simuliert die LUT-Interpolation fuer Plot und Crossfade-Simulation.

    v136: Semantik von folded wiederhergestellt.

    - folded=True:
        best_r-Werte sind Phasenwerte im gefalteten Raum. Die Richtung des
        Segments kommt aus approach_up am Zielpunkt. Nur hier ist das
        Richtungsbit bedeutungsvoll.

    - folded=False:
        best_r-Werte liegen im realen Suchraum [0, r_max). Es gibt keinen
        Phasen-Fold und keine Richtungsentscheidung; interpoliert wird direkt
        linear zwischen den gespeicherten Release-Offsets.

    Wenn ein Segment im Plot/Simulation einen Wrap braucht, die Kurve aber
    folded=False ist, dann ist nicht die Interpolation zu reparieren, sondern
    die Fold-Entscheidung bzw. die gespeicherten LUT-Werte sind falsch.
    """
    if not lut_points:
        return 0

    pts = sorted(lut_points, key=lambda p: p.loop_pos)
    T_safe = max(1, int(T))
    r_max_safe = int(r_max) if r_max and r_max > 0 else (T_safe if folded else 2 * T_safe)

    def _r(pt) -> float:
        return float(pt.best_r)

    def _clamp_release(y: float) -> float:
        if r_max_safe <= 0:
            return float(y)
        return max(0.0, min(float(r_max_safe - 1), float(y)))

    def _interp(pa_pt, pb_pt, pos: int) -> float:
        span = max(1, pb_pt.loop_pos - pa_pt.loop_pos)
        t    = (pos - pa_pt.loop_pos) / span
        r0   = _r(pa_pt)
        r1   = _r(pb_pt)

        if folded:
            up    = getattr(pb_pt, "approach_up", True)
            delta = _directed_delta_folded(int(round(r0)), int(round(r1)), up, T_safe)
            return (r0 + t * delta) % T_safe

        # Ungefaltet: direkter Weg im realen Release-Suchraum. Kein approach_up,
        # kein Kreisweg, keine Projektion entlang einer anderen T-Kopie.
        return _clamp_release(r0 + t * (r1 - r0))

    def _project_single(pt) -> float:
        r = _r(pt)
        return (r % T_safe) if folded else _clamp_release(r)

    if len(pts) == 1:
        return int(round(_project_single(pts[0])))

    # Vorlauf: konstant auf erstem Stuetzpunkt.
    if loop_pos <= pts[0].loop_pos:
        return int(round(_project_single(pts[0])))

    # Nachlauf: letzten Trend fortsetzen. Welche Releases das im Plot nutzen,
    # entscheidet _plot_lut ueber den dargestellten n-Bereich.
    if loop_pos >= pts[-1].loop_pos:
        return int(round(_interp(pts[-2], pts[-1], loop_pos)))

    lo, hi = 0, len(pts) - 1
    while lo + 1 < hi:
        mid = (lo + hi) // 2
        if pts[mid].loop_pos <= loop_pos:
            lo = mid
        else:
            hi = mid

    return int(round(_interp(pts[lo], pts[hi], loop_pos)))



# ─── Legacy Release Alignment (Nachbau GOSoundReleaseAlignTable) ─────────────

PHASE_ALIGN_DERIVATIVES  = 32
PHASE_ALIGN_AMPLITUDES   = 32
PHASE_ALIGN_MIN_FREQUENCY = 64   # Hz — bestimmt Fensterlänge


class LegacyAlignTable:
    """
    Nachbau von GOSoundReleaseAlignTable.
    Baut eine 32×32-Tabelle aus dem Release-WAV auf (Amplitude × Ableitung).
    Lookup: letzte 2 Attack-Samples → Release-Startposition.
    """

    def __init__(self, release_mono: np.ndarray, sample_rate: int):
        self._table = {}   # (deriv_bin, amp_bin) → release_start_sample
        self._max_val = 0.0
        self._build(release_mono, sample_rate)

    def _build(self, rel: np.ndarray, sr: int):
        search_len = sr // PHASE_ALIGN_MIN_FREQUENCY
        n = min(search_len, len(rel))
        if n < 2:
            return

        # Finde Max-Amplitude für Normalisierung
        max_val = float(np.max(np.abs(rel[:n]))) if n > 0 else 1.0
        if max_val < 1e-12:
            max_val = 1.0
        self._max_val = max_val

        table = {}
        for i in range(1, n):
            f_cur  = float(rel[i])
            f_prev = float(rel[i-1])
            deriv  = f_cur - f_prev
            amp    = f_cur

            # Quantisierung wie in GO: shift in [0..2*max-1], dann bin
            amp_shifted   = amp   + max_val
            deriv_shifted = deriv + max_val

            amp_bin   = int(amp_shifted   / (2.0 * max_val) * PHASE_ALIGN_AMPLITUDES)
            deriv_bin = int(deriv_shifted / (2.0 * max_val) * PHASE_ALIGN_DERIVATIVES)

            amp_bin   = max(0, min(PHASE_ALIGN_AMPLITUDES   - 1, amp_bin))
            deriv_bin = max(0, min(PHASE_ALIGN_DERIVATIVES  - 1, deriv_bin))

            key = (deriv_bin, amp_bin)
            if key not in table:
                table[key] = i + 1   # frühester Index (wie GO: i+1+start_position)

        # Leere Bins per Nachbarsuche füllen
        all_keys = set((d, a) for d in range(PHASE_ALIGN_DERIVATIVES)
                               for a in range(PHASE_ALIGN_AMPLITUDES))
        empty    = all_keys - set(table.keys())
        radius   = 1
        while empty and radius <= max(PHASE_ALIGN_DERIVATIVES, PHASE_ALIGN_AMPLITUDES):
            filled = set()
            for (d, a) in list(empty):
                for dd in range(-radius, radius+1):
                    for da in range(-radius, radius+1):
                        if abs(dd) != radius and abs(da) != radius:
                            continue
                        nd, na = d+dd, a+da
                        if (nd, na) in table:
                            table[(d, a)] = table[(nd, na)]
                            filled.add((d, a))
                            break
                    if (d, a) in filled:
                        break
            empty -= filled
            radius += 1

        self._table = table

    def get_position_for(self, attack_mono: np.ndarray, t_attack: int) -> int:
        """Gibt Release-Startposition zurück basierend auf letzten 2 Attack-Samples."""
        if t_attack < 2 or len(attack_mono) < 2:
            return 0
        t = min(t_attack, len(attack_mono) - 1)
        f_cur  = float(attack_mono[t])
        f_prev = float(attack_mono[t-1])
        f_mod  = f_cur  + f_prev   # Summe wie in GO (2 Kanäle summiert, hier mono)
        v_mod  = f_cur  - f_prev

        max_val = self._max_val if self._max_val > 1e-12 else 1.0
        amp_shifted   = f_mod + max_val
        deriv_shifted = v_mod + max_val

        amp_bin   = int(amp_shifted   / (2.0 * max_val) * PHASE_ALIGN_AMPLITUDES)
        deriv_bin = int(deriv_shifted / (2.0 * max_val) * PHASE_ALIGN_DERIVATIVES)
        amp_bin   = max(0, min(PHASE_ALIGN_AMPLITUDES   - 1, amp_bin))
        deriv_bin = max(0, min(PHASE_ALIGN_DERIVATIVES  - 1, deriv_bin))

        return self._table.get((deriv_bin, amp_bin), 0)


# ─── Crossfade-Kurven ─────────────────────────────────────────────────────────

def _crossfade_weights(mode: str, n: int) -> tuple:
    """Gibt (a_weights, b_weights) für n Samples zurück. a=Attack, b=Release."""
    t = np.linspace(0.0, 1.0, n, dtype=np.float32)
    half_pi = np.pi / 2.0
    if mode == "Linear":
        return 1.0 - t, t
    elif mode == "SinEqualPower":
        return np.cos(half_pi * t), np.sin(half_pi * t)
    elif mode == "Sin2":
        return np.cos(half_pi * t)**2, np.sin(half_pi * t)**2
    elif mode == "SqrtEqualPower":
        return np.sqrt(1.0 - t), np.sqrt(t)
    elif mode == "X2":
        return 1.0 - t**2, t**2
    else:  # Custom / fallback
        return np.zeros(n), np.ones(n)



class CrossfadeSimWindow:
    """
    Simulationsfenster: zeigt wie GO den Crossfade bei einer bestimmten
    Attack-Dauer durchführen würde.

    Modi:
    - Sofort: r aus LUT-Interpolation + Legacy-Tabelle
    - Nach "Berechnen": zusätzlich r direkt per argmax aus Score-Matrix
    """

    XFADE_MODES = ["Linear", "SinEqualPower", "Sin2", "SqrtEqualPower", "X2"]

    def __init__(self, parent, pa: "PipeAnalysis"):
        self.pa          = pa
        self._atk_mono   = None
        self._rel_mono   = None
        self._legacy_tbl = None
        self._score_mat  = None   # (N, r_max_d) — nach "Berechnen"
        self._score_ns   = None   # n-Werte zur Score-Matrix
        self._computing  = False
        self._stop_event = threading.Event()

        self.win = tk.Toplevel(parent)
        self.win.title(f"Crossfade-Simulation — {pa.rank_name} "
                       f"{midi_to_name(pa.midi_note)} / {pa.release_type}")
        self.win.geometry("1200x750")
        self.win.configure(bg=C_BG)
        self.win.protocol("WM_DELETE_WINDOW", self._on_close)

        self._build_ui()
        # WAVs und Legacy-Tabelle im Hintergrund laden
        threading.Thread(target=self._load_data, daemon=True).start()
        # Warnung bei Legacy-Fallback
        if pa.legacy_fallback:
            self.win.after(100, lambda: self._status.config(
                text="⚠ Legacy-Pfeife — NDP-Kurve zeigt LUT-Visualisierung, kein GO-Korrelations-Alignment",
                fg=C_WARN))

    def _on_close(self):
        self._stop_event.set()
        self.win.destroy()

    def _build_ui(self):
        pa = self.pa

        # ── Toolbar ───────────────────────────────────────────────────────────
        bar = tk.Frame(self.win, bg=C_BG3, pady=5)
        bar.pack(fill=tk.X)

        btn_kw = dict(bg=C_BTN, fg=C_TEXT, relief=tk.RAISED,
                      font=("Consolas", 9), padx=8, pady=3,
                      activebackground=C_BTN_ACT, cursor="hand2")

        # ── Attack-Dauer Controls ──────────────────────────────────────────────
        pa = self.pa
        sr = pa.sample_rate or 48000
        # Slider-Grenzen samplegenau aus der Release-Gültigkeit ableiten.
        # Bisher endete der Slider bei pa.max_key_press_ms oder pauschal 2000 ms.
        # Für das lange Release (max_key_press_ms=None) ist aber das WAV-Ende
        # die natürliche Obergrenze; für kurze/mittlere Releases die ODF-
        # Gültigkeit max_sample. Die eigentlichen Analysewerte wurden in
        # analyze_pipe() bereits als pa.min_sample/pa.max_sample berechnet.
        atk_last_smp = max(0, int(getattr(pa, "atk_frames", 0) or 0) - 1)
        min_smp = int(pa.min_sample) if pa.min_sample is not None else 0
        if pa.max_sample is not None:
            max_smp = min(int(pa.max_sample), atk_last_smp)
        else:
            max_smp = atk_last_smp
        if max_smp < min_smp:
            max_smp = min_smp
        min_ms = min_smp * 1000.0 / sr
        max_ms = max_smp * 1000.0 / sr
        self._sr = sr
        self._min_smp = min_smp
        self._max_smp = max_smp

        # interner Zustand: Sample-genau
        self._cur_smp = tk.IntVar(value=min_smp)

        # Grob-Slider (ms)
        tk.Label(bar, text="Attack:", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=(8,2))

        self._slider = tk.Scale(bar, from_=min_ms, to=max_ms,
                                resolution=1, orient=tk.HORIZONTAL, length=250,
                                bg=C_BG3, fg=C_TEXT, highlightthickness=0,
                                font=("Consolas", 8), showvalue=False)
        self._slider.set(min_ms)
        self._slider.pack(side=tk.LEFT, padx=2)
        self._slider.bind("<Motion>",        self._on_slider_move)
        self._slider.bind("<ButtonRelease>", self._on_slider_move)

        # ms-Feld
        tk.Label(bar, text="ms:", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=(6,1))
        self._ms_var = tk.StringVar(value=f"{min_ms:.3f}")
        ms_entry = tk.Entry(bar, textvariable=self._ms_var, width=9,
                            font=("Consolas", 9))
        ms_entry.pack(side=tk.LEFT, padx=1)
        ms_entry.bind("<Return>", self._on_ms_entry)
        ms_entry.bind("<FocusOut>", self._on_ms_entry)

        # Samples-Feld
        tk.Label(bar, text="smp:", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=(6,1))
        self._smp_var = tk.StringVar(value=str(min_smp))
        smp_entry = tk.Entry(bar, textvariable=self._smp_var, width=8,
                             font=("Consolas", 9))
        smp_entry.pack(side=tk.LEFT, padx=1)
        smp_entry.bind("<Return>", self._on_smp_entry)
        smp_entry.bind("<FocusOut>", self._on_smp_entry)

        # Step-Buttons: ±1 sample, ±10 samples, ±1ms
        btn_kw_step = dict(font=("Consolas", 9), bg=C_BTN, fg=C_TEXT,
                           relief=tk.RAISED, padx=3, pady=2,
                           activebackground=C_BTN_ACT)

        tk.Label(bar, text="Step:", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=(6,2))

        sr_ = pa.sample_rate or 48000
        ms_step = int(round(sr_ / 1000))  # samples per ms

        tk.Button(bar, text="«ms",  command=lambda: self._step_sample(-ms_step),
                  **btn_kw_step).pack(side=tk.LEFT, padx=1)
        tk.Button(bar, text="«10",  command=lambda: self._step_sample(-10),
                  **btn_kw_step).pack(side=tk.LEFT, padx=1)
        tk.Button(bar, text="◀1",   command=lambda: self._step_sample(-1),
                  **btn_kw_step).pack(side=tk.LEFT, padx=1)
        tk.Button(bar, text="▶1",   command=lambda: self._step_sample(+1),
                  **btn_kw_step).pack(side=tk.LEFT, padx=1)
        tk.Button(bar, text="10»",  command=lambda: self._step_sample(+10),
                  **btn_kw_step).pack(side=tk.LEFT, padx=1)
        tk.Button(bar, text="ms»",  command=lambda: self._step_sample(+ms_step),
                  **btn_kw_step).pack(side=tk.LEFT, padx=(1,4))

        # Pfeiltasten binden mit eigenem Key-Repeat (wie Texteditor)
        #
        # Wichtig:
        # Das Betriebssystem erzeugt beim Halten einer Taste selbst wiederholte
        # KeyPress-Events. Die alte Implementierung hat bei jedem solchen
        # KeyPress den after()-Timer gecancelt und wieder auf 600 ms gesetzt.
        # Ergebnis: der eigene Repeat konnte praktisch nie starten.
        #
        # Deshalb ignorieren wir wiederholte KeyPress-Events derselben Taste,
        # solange unser eigener Repeat aktiv ist.
        self._repeat_job = None
        self._repeat_release_job = None
        self._repeat_delta = 0
        self._repeat_key = None
        self._plotting = False
        self._plot_pending = False

        def _cancel_repeat_job():
            if self._repeat_job is not None:
                try:
                    self.win.after_cancel(self._repeat_job)
                except tk.TclError:
                    pass
                self._repeat_job = None

        def _cancel_release_job():
            if self._repeat_release_job is not None:
                try:
                    self.win.after_cancel(self._repeat_release_job)
                except tk.TclError:
                    pass
                self._repeat_release_job = None

        def _repeat_tick():
            # Während matplotlib zeichnet, blockiert der Tk-Mainloop ohnehin.
            # Nach dem Zeichnen läuft dieser Callback weiter. _plot() selbst
            # coalesced Zwischenwerte über _plot_pending.
            if self._repeat_key is None:
                self._repeat_job = None
                return
            self._step_sample(self._repeat_delta)
            self._repeat_job = self.win.after(100, _repeat_tick)

        def _key_press(key, delta):
            _cancel_release_job()

            # OS-Autorepeat: gleicher KeyPress kommt mehrfach. Nicht erneut
            # initialisieren, sonst wird der 600-ms-Delay immer wieder neu
            # gestartet.
            if self._repeat_key == key:
                return "break"

            # Wechsel zwischen Links/Rechts bzw. Modifier-Variante sauber
            # übernehmen.
            _cancel_repeat_job()
            self._repeat_key = key
            self._repeat_delta = delta

            # Erster Tastendruck wirkt sofort.
            self._step_sample(delta)

            # Danach verzögerter Repeat.
            self._repeat_job = self.win.after(600, _repeat_tick)
            return "break"

        def _finish_key_release(key):
            self._repeat_release_job = None
            if self._repeat_key != key:
                return
            _cancel_repeat_job()
            self._repeat_key = None
            self._repeat_delta = 0

        def _key_release(key):
            # Kleiner Delay macht das auch unter X11 robust, wo Autorepeat
            # teils als KeyRelease+KeyPress-Paare sichtbar wird. Kommt direkt
            # wieder ein KeyPress, cancelt _key_press diesen Release-Job.
            _cancel_release_job()
            self._repeat_release_job = self.win.after(
                40, lambda key=key: _finish_key_release(key))
            return "break"

        self.win.bind("<KeyPress-Left>",  lambda e: _key_press("Left", -1))
        self.win.bind("<KeyPress-Right>", lambda e: _key_press("Right", +1))
        self.win.bind("<KeyRelease-Left>",  lambda e: _key_release("Left"))
        self.win.bind("<KeyRelease-Right>", lambda e: _key_release("Right"))
        self.win.bind("<Control-KeyPress-Left>",  lambda e: _key_press("CtrlLeft", -10))
        self.win.bind("<Control-KeyPress-Right>", lambda e: _key_press("CtrlRight", +10))
        self.win.bind("<Control-KeyRelease-Left>",  lambda e: _key_release("CtrlLeft"))
        self.win.bind("<Control-KeyRelease-Right>", lambda e: _key_release("CtrlRight"))
        self.win.bind("<Shift-KeyPress-Left>",  lambda e: _key_press("ShiftLeft", -ms_step))
        self.win.bind("<Shift-KeyPress-Right>", lambda e: _key_press("ShiftRight", +ms_step))
        self.win.bind("<Shift-KeyRelease-Left>",  lambda e: _key_release("ShiftLeft"))
        self.win.bind("<Shift-KeyRelease-Right>", lambda e: _key_release("ShiftRight"))

        # Crossfade-Modus
        tk.Label(bar, text="Xfade:", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=(8,2))
        self._xfade_var = tk.StringVar(value="Sin2")
        xfade_menu = ttk.Combobox(bar, textvariable=self._xfade_var,
                                   values=self.XFADE_MODES, width=14,
                                   state="readonly", font=("Consolas", 9))
        xfade_menu.pack(side=tk.LEFT, padx=2)
        xfade_menu.bind("<<ComboboxSelected>>", lambda _: self._plot())

        # Berechnen-Button
        self._btn_compute = tk.Button(bar, text="▶ Alle N berechnen",
                                       command=self._compute_scores, **btn_kw)
        self._btn_compute.pack(side=tk.LEFT, padx=12)

        self._progress = ttk.Progressbar(bar, length=150, mode="determinate")
        self._progress.pack(side=tk.LEFT, padx=4)
        self._status = tk.Label(bar, text="Lade WAV...", bg=C_BG3,
                                 fg=C_TEXT2, font=("Consolas", 9))
        self._status.pack(side=tk.LEFT, padx=4)

        # ── Kurven-Checkboxen ─────────────────────────────────────────────────
        cb_frame = tk.Frame(self.win, bg=C_BG2, pady=4)
        cb_frame.pack(fill=tk.X, padx=6)

        self._show = {}
        curves = [
            ("attack",       "Attack",                  C_DENSE),
            ("ndp_interp",   "Release NDP (interpoliert)", C_SPARSE),
            ("xfade_interp", "Crossfade NDP (interpoliert)", C_SPARSE),
            ("ndp_direct",   "Release NDP (direkt, alle N)", "#9c27b0"),
            ("xfade_direct", "Crossfade NDP (direkt)",   "#9c27b0"),
            ("legacy",       "Release Legacy",           C_WARN),
            ("xfade_legacy", "Crossfade Legacy",         C_WARN),
        ]
        default_visible = {"attack", "ndp_interp"}
        for key, label, color in curves:
            var = tk.BooleanVar(value=(key in default_visible))
            self._show[key] = var
            cb = tk.Checkbutton(cb_frame, text=label, variable=var,
                                 bg=C_BG2, fg=color, selectcolor=C_BG2,
                                 font=("Consolas", 9),
                                 command=self._plot)
            cb.pack(side=tk.LEFT, padx=6)

        # ── Plot ──────────────────────────────────────────────────────────────
        if HAS_MATPLOTLIB:
            plot_frame = tk.Frame(self.win, bg=C_BG)
            plot_frame.pack(fill=tk.BOTH, expand=True, padx=6, pady=4)
            self._fig = Figure(figsize=(11, 5), facecolor=C_BG)
            from matplotlib.backends.backend_tkagg import NavigationToolbar2Tk
            self._canvas = FigureCanvasTkAgg(self._fig, master=plot_frame)
            toolbar_frame = tk.Frame(plot_frame, bg=C_BG)
            toolbar_frame.pack(fill=tk.X)
            NavigationToolbar2Tk(self._canvas, toolbar_frame)
            self._canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    # ── Datenladen ────────────────────────────────────────────────────────────

    def _load_data(self):
        try:
            atk, sr, _, _ = read_wav_mono_float(self.pa.attack_path)
            rel, _,  _, _ = read_wav_mono_float(self.pa.release_path)
            self._atk_mono = atk
            self._rel_mono = rel
            # Legacy-Tabelle aufbauen
            legacy = LegacyAlignTable(rel, sr)
            self._legacy_tbl = legacy
            self.win.after(0, lambda: self._status.config(
                text="Bereit", fg=C_OK))
            self.win.after(0, self._plot)
        except Exception as e:
            self.win.after(0, lambda: self._status.config(
                text=f"Fehler: {e}", fg=C_BAD))

    # ── Score-Matrix berechnen (Modus 2) ──────────────────────────────────────

    def _compute_scores(self):
        if self._computing or self._atk_mono is None:
            return
        self._computing = True
        self._stop_event.clear()
        self._btn_compute.config(state=tk.DISABLED)
        self._progress["value"] = 0
        self._status.config(text="Berechne Score-Matrix...")
        threading.Thread(target=self._score_worker, daemon=True).start()

    def _score_worker(self):
        pa  = self.pa
        try:
            atk = self._atk_mono
            rel = self._rel_mono
            T   = pa.T_int
            ds  = min(4, T // 500) if T >= 500 else 1
            window_len = pa.crossfade_len_samples if pa.crossfade_len_samples >= 4 else 2 * T
            r_search_max = getattr(pa, "lut_r_search_max", 0) or (2 * T)
            r_max = min(r_search_max, len(rel) - window_len)
            if r_max <= 0:
                return

            window_len_d = max(4, window_len // ds)
            r_max_d      = max(1, r_max // ds)
            rel_ds       = rel[:r_max + window_len + 1 : ds].astype(np.float32)

            # n-Bereich: nur gültige LUT-Perioden
            n_start = max(1, int(pa.min_sample / pa.T_float)) if pa.min_sample else 1
            n_end   = min(int(len(atk) / pa.T_float),
                          int(pa.max_sample / pa.T_float) + 2) \
                      if pa.max_sample else int(len(atk) / pa.T_float)
            ns = list(range(n_start, n_end))
            if not ns:
                return

            from numpy.lib.stride_tricks import as_strided as _ast
            score_mat = np.zeros((len(ns), r_max_d), dtype=np.float32)

            for col_i, n in enumerate(ns):
                if self._stop_event.is_set():
                    return
                cs   = int(round(n * pa.T_float))
                cs_d = cs // ds
                lw   = atk[cs_d*ds : (cs_d+window_len_d)*ds : ds].astype(np.float32)
                if len(lw) < window_len_d:
                    continue
                na = np.linalg.norm(lw)
                if na < 1e-12:
                    continue
                lw_n = lw / na
                s0      = rel_ds.strides[0]
                if r_max_d + window_len_d > len(rel_ds):
                    continue
                rel_mat = _ast(rel_ds, shape=(r_max_d, window_len_d),
                               strides=(s0, s0))
                norms   = np.linalg.norm(rel_mat, axis=1)
                norms   = np.where(norms < 1e-12, 1.0, norms)
                score_mat[col_i] = rel_mat.dot(lw_n) / norms

                pct = 100 * (col_i+1) / len(ns)
                self.win.after(0, lambda p=pct: setattr(self._progress, 'value', p)
                               or self._progress.configure(value=p))

            self._score_mat = score_mat
            self._score_ns  = ns
            self._score_ds  = ds
            self._score_r_max_d = r_max_d
            self.win.after(0, self._on_scores_done)
        except Exception as e:
            self.win.after(0, lambda: self._status.config(
                text=f"Fehler: {e}", fg=C_BAD))
        finally:
            self._computing = False

    def _on_scores_done(self):
        self._btn_compute.config(state=tk.NORMAL)
        self._status.config(text="Score-Matrix fertig — Slider live", fg=C_OK)
        self._progress.configure(value=100)
        self._plot()

    def _set_sample(self, smp: int, update_slider: bool = True):
        """Zentrale Update-Funktion — alle Controls synchronisieren."""
        smp = max(self._min_smp, min(self._max_smp, smp))
        sr  = self._sr
        ms  = smp * 1000.0 / sr
        self._cur_smp.set(smp)
        self._ms_var.set(f"{ms:.3f}")
        self._smp_var.set(str(smp))
        if update_slider:
            self._slider.set(int(round(ms)))
        self._plot()

    def _on_slider_move(self, event=None):
        """Slider-Bewegung → Sample berechnen, aber Slider NICHT zurücksetzen."""
        try:
            ms  = float(self._slider.get())
            smp = int(round(ms * self._sr / 1000))
            smp = max(self._min_smp, min(self._max_smp, smp))
            if smp != self._cur_smp.get():
                self._cur_smp.set(smp)
                self._ms_var.set(f"{ms:.3f}")
                self._smp_var.set(str(smp))
                self._plot()
        except ValueError:
            pass

    def _on_slider(self, val):
        self._on_slider_move()

    def _on_ms_entry(self, event=None):
        try:
            ms  = float(self._ms_var.get().replace(",", "."))
            smp = int(round(ms * self._sr / 1000))
            self._set_sample(smp)
        except ValueError:
            pass

    def _on_smp_entry(self, event=None):
        try:
            smp = int(self._smp_var.get())
            self._set_sample(smp)
        except ValueError:
            pass

    def _step_sample(self, delta: int):
        self._set_sample(self._cur_smp.get() + delta)

    def _on_atk_change(self):
        self._plot()

    def _get_r_interp(self, t_attack_samples: int) -> int:
        """r aus LUT-Interpolation + Offset innerhalb Periode.
        n = floor(t/T) — immer die Periode VOR dem aktuellen Sample."""
        pa = self.pa
        T_float = pa.T_float if pa.T_float > 0 else float(max(1, pa.T_int))
        if not pa.lut_points or T_float <= 0:
            return 0
        n_int  = int(t_attack_samples / T_float)   # floor, kein round
        t_n    = int(round(n_int * T_float))
        offset = t_attack_samples - t_n             # immer >= 0
        r_base = get_position_for_correlation(t_n, pa.lut_points, pa.T_int,
                                                     folded=getattr(pa, "lut_folded", True),
                                                     r_max=getattr(pa, "lut_r_search_max", 2 * pa.T_int))
        if getattr(pa, "lut_folded", True):
            return (r_base + offset) % max(1, pa.T_int)
        else:
            return r_base + offset

    def _get_r_direct(self, t_attack_samples: int) -> int:
        """r direkt per argmax aus Score-Matrix, kein mod-Fold."""
        if self._score_mat is None:
            return None
        pa  = self.pa
        ns  = self._score_ns
        ds  = self._score_ds
        T_float = pa.T_float if pa.T_float > 0 else float(max(1, pa.T_int))
        n_int  = int(t_attack_samples / T_float)   # floor
        t_n    = int(round(n_int * T_float))
        offset = t_attack_samples - t_n             # >= 0

        # Nächstes n in Score-Matrix
        if n_int not in ns:
            diffs = [abs(ni - n_int) for ni in ns]
            n_int = ns[diffs.index(min(diffs))]
        col_i = ns.index(n_int) if n_int in ns else 0
        r_d   = int(np.argmax(self._score_mat[col_i]))
        # Kein % T_int — roher Wert aus Score-Matrix + Offset
        return r_d * ds + offset

    def _get_r_legacy(self, t_attack_samples: int) -> int:
        if self._legacy_tbl is None or self._atk_mono is None:
            return 0
        return self._legacy_tbl.get_position_for(self._atk_mono, t_attack_samples)

    # ── Plot ─────────────────────────────────────────────────────────────────

    def _on_atk_change(self):
        self._plot()

    def _plot(self):
        if not HAS_MATPLOTLIB or self._atk_mono is None or self._rel_mono is None:
            return
        if getattr(self, '_plotting', False):
            # Plot läuft noch — Zielwert merken, nach Fertigstellung neu zeichnen
            self._plot_pending = True
            return
        self._plotting = True
        self._plot_pending = False
        try:
            self._do_plot()
        finally:
            self._plotting = False
            # Wenn während des Plots ein neuer Wert ankam: nochmal zeichnen
            if getattr(self, '_plot_pending', False):
                self.win.after(0, self._plot)

    def _do_plot(self):
        pa          = self.pa
        sr          = pa.sample_rate or 48000
        T           = max(16, pa.T_int)
        T_float     = pa.T_float if pa.T_float > 0 else float(T)
        t_smp       = self._cur_smp.get()
        t_ms        = t_smp * 1000.0 / sr
        xfade_len   = pa.crossfade_len_samples if pa.crossfade_len_samples > 0 else 2 * T
        context     = 2 * T

        if len(self._rel_mono) < 4 or len(self._atk_mono) < 4:
            return
        t_smp = max(0, min(t_smp, len(self._atk_mono) - 1))

        # Display bugfix: do NOT shorten the crossfade window at the right edge
        # of the attack WAV. safe_slice() below already zero-pads out-of-range
        # samples. Shrinking xfade_len here made the simulated crossfade collapse
        # when the slider was moved to the end of the release-valid range.
        # Keep the configured GO crossfade length constant so the visualization
        # stays comparable across the entire slider range.
        xfade_len = max(1, int(xfade_len))

        r_interp = self._get_r_interp(t_smp)
        r_legacy = self._get_r_legacy(t_smp)
        r_direct = self._get_r_direct(t_smp)  # None wenn Score-Matrix fehlt

        # Gemeinsame Zeitachse: [-context, xfade_len + context]
        total = context + xfade_len + context
        x     = np.arange(-context, xfade_len + context)

        def safe_slice(arr, start, length):
            s = max(0, start)
            e = min(len(arr), start + length)
            out = np.zeros(length, dtype=np.float32)
            src_start = s - start
            out[src_start:src_start+(e-s)] = arr[s:e]
            return out

        # Attack-Kurve: zentriert um t_smp
        atk_seg = safe_slice(self._atk_mono, t_smp - context, total)

        # Release-Segmente
        def rel_seg(r):
            return safe_slice(self._rel_mono, r - context, total)

        # Crossfade-Kurven
        a_w, b_w = _crossfade_weights(self._xfade_var.get(), xfade_len)

        def make_xfade(r):
            atk_part = safe_slice(self._atk_mono, t_smp, xfade_len)
            rel_part = safe_slice(self._rel_mono,  r,     xfade_len)
            xf = atk_part * a_w + rel_part * b_w
            out = np.zeros(total, dtype=np.float32)
            out[context:context+xfade_len] = xf
            # Attack vor Crossfade
            out[:context] = safe_slice(self._atk_mono, t_smp - context, context)
            # Release nach Crossfade
            out[context+xfade_len:] = safe_slice(
                self._rel_mono, r + xfade_len, context)
            return out

        self._fig.clear()
        ax = self._fig.add_subplot(111, facecolor=C_BG2)
        self._fig.patch.set_facecolor(C_BG)
        ax.tick_params(colors=C_TEXT2)
        for sp in ax.spines.values():
            sp.set_edgecolor(C_BORDER)

        show = self._show

        if show["attack"].get():
            ax.plot(x, atk_seg, color=C_DENSE, lw=1.5, label="Attack")

        if show["ndp_interp"].get():
            ax.plot(x, rel_seg(r_interp), color=C_SPARSE, lw=1.2,
                    label=f"Release NDP interp (r={r_interp})")
        if show["xfade_interp"].get():
            ax.plot(x, make_xfade(r_interp), color=C_SPARSE, lw=1.5,
                    linestyle="--", label="Crossfade NDP interp")

        if r_direct is not None:
            if show["ndp_direct"].get():
                ax.plot(x, rel_seg(r_direct), color="#9c27b0", lw=1.2,
                        label=f"Release NDP direkt (r={r_direct})")
            if show["xfade_direct"].get():
                ax.plot(x, make_xfade(r_direct), color="#9c27b0", lw=1.5,
                        linestyle="--", label="Crossfade NDP direkt")

        if show["legacy"].get():
            ax.plot(x, rel_seg(r_legacy), color=C_WARN, lw=1.2,
                    label=f"Release Legacy (r={r_legacy})")
        if show["xfade_legacy"].get():
            ax.plot(x, make_xfade(r_legacy), color=C_WARN, lw=1.5,
                    linestyle="--", label="Crossfade Legacy")

        # Vertikale Marker
        ax.axvline(0,          color=C_TEXT2,  lw=0.8, linestyle=":",
                   label="Crossfade Start")
        ax.axvline(xfade_len,  color=C_BORDER, lw=0.8, linestyle=":",
                   label="Crossfade Ende")

        ax.set_xlabel("Samples (0 = Crossfade-Start)", color=C_TEXT2)
        ax.set_ylabel("Amplitude", color=C_TEXT2)
        ax.set_title(
            f"{pa.rank_name} {midi_to_name(pa.midi_note)} / {pa.release_type}"
            f"  —  Attack {t_ms:.3f} ms ({t_smp} smp)  |  T={T}",
            color=C_TEXT, fontsize=10)
        self._fig.tight_layout(rect=[0, 0, 0.78, 1])
        ax.legend(facecolor=C_BG2, edgecolor=C_BORDER, labelcolor=C_TEXT,
                  fontsize=8, loc="upper left",
                  bbox_to_anchor=(1.02, 1), borderaxespad=0)
        self._canvas.draw()


class CorrLandscapeWindow:
    """
    Separates Fenster: NormalizedDotProduct(attack@n, release@r) als 2D-Heatmap.
    X-Achse: Periodenindex n (gesamter Attack)
    Y-Achse: r in [0, 2*T)
    Farbe:   NDP-Wert in [-1, 1]

    Optionen:
    - Fensterfunktion: keine, Hann, Hamming, Blackman
    - CSV-Export der berechneten Matrix
    """

    WINDOWS = {
        "Keine":    None,
        "Hann":     "hanning",
        "Hamming":  "hamming",
        "Blackman": "blackman",
    }

    def __init__(self, parent, pa: "PipeAnalysis"):
        self.pa     = pa
        self.matrix = None   # np.ndarray (n_cols x r_max)
        self.ns     = None   # x-Achse: n-Werte
        self._computing = False
        self._stop_event = threading.Event()
        self._zoom_mode = "lut"   # "lut" = Auto-Zoom auf LUT, "full" = Gesamtansicht
        self._lut_xlim  = None
        self._lut_ylim  = None
        self._debug_var = tk.BooleanVar(value=True)
        self._lab_pts   = None   # Tuning-Lab LUT-Punkte (None = Original pa.lut_points)

        self.win = tk.Toplevel(parent)
        self.win.title(f"Korrelationslandschaft — {pa.rank_name} {midi_to_name(pa.midi_note)} "
                       f"{pa.perspective} / {pa.release_type}")
        self.win.geometry("1730x820")
        self.win.configure(bg=C_BG)
        self.win.protocol("WM_DELETE_WINDOW", self._on_close)

        self._build_ui()

    def _on_close(self):
        self._stop_event.set()
        self.win.destroy()

    def _build_ui(self):
        # Toolbar
        bar = tk.Frame(self.win, bg=C_BG3, pady=5)
        bar.pack(fill=tk.X)

        btn_kw = dict(bg=C_BTN, fg=C_TEXT, relief=tk.RAISED,
                      font=("Consolas", 9), padx=8, pady=3,
                      activebackground=C_BTN_ACT, cursor="hand2")

        # Fensterfunktion
        tk.Label(bar, text="Fensterfunktion:", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=(8,2))
        self._win_var = tk.StringVar(value="Keine")
        for name in self.WINDOWS:
            tk.Radiobutton(bar, text=name, variable=self._win_var, value=name,
                           bg=C_BG3, fg=C_TEXT, selectcolor=C_SEL,
                           font=("Consolas", 9),
                           command=self._on_window_change).pack(side=tk.LEFT, padx=4)

        tk.Button(bar, text="▶  Berechnen",
                  command=self._compute, **btn_kw).pack(side=tk.LEFT, padx=12)

        tk.Button(bar, text="LUT-Zoom",
                  command=self._zoom_to_lut, **btn_kw).pack(side=tk.LEFT, padx=4)
        tk.Button(bar, text="Gesamtansicht",
                  command=self._zoom_to_full, **btn_kw).pack(side=tk.LEFT, padx=4)

        tk.Checkbutton(bar, text="Debug-Kandidaten",
                       variable=self._debug_var, command=self._plot,
                       bg=C_BG3, fg=C_TEXT, selectcolor=C_BG2,
                       activebackground=C_BG3, activeforeground=C_TEXT,
                       font=("Consolas", 9)).pack(side=tk.LEFT, padx=6)

        self._btn_dbg_csv = tk.Button(bar, text="Debug CSV",
                                      command=self._export_debug_csv, **btn_kw)
        self._btn_dbg_csv.pack(side=tk.LEFT, padx=4)

        self._btn_csv = tk.Button(bar, text="💾  CSV exportieren",
                                   command=self._export_csv,
                                   state=tk.DISABLED, **btn_kw)
        self._btn_csv.pack(side=tk.LEFT, padx=4)

        self._progress = ttk.Progressbar(bar, length=180, mode="determinate")
        self._progress.pack(side=tk.LEFT, padx=8)
        self._status = tk.Label(bar, text="", bg=C_BG3, fg=C_TEXT2,
                                 font=("Consolas", 9))
        self._status.pack(side=tk.LEFT, padx=4)

        # Info
        pa = self.pa
        r_info = getattr(pa, "lut_r_search_max", 0) or (2 * pa.T_int)
        xfade_tag = " (auto)" if getattr(pa, "crossfade_auto", False) else ""
        info = (f"T={pa.T_int}  SR={pa.sample_rate}  "
                f"NDP-Fenster={pa.crossfade_len_samples}{xfade_tag}  "
                f"r in [0, {r_info})  "
                f"n_total={len(pa.attack_path) and pa.n_total or '?'}")
        tk.Label(self.win, text=info, bg=C_BG, fg=C_TEXT2,
                 font=("Consolas", 9)).pack(anchor=tk.W, padx=8, pady=(4,0))

        # Hauptbereich: Plot links, Tuning Lab rechts
        main_area = tk.Frame(self.win, bg=C_BG)
        main_area.pack(fill=tk.BOTH, expand=True)

        # Lab-Panel zuerst packen (rechts, feste Breite — schrumpft nicht)
        self._build_lab_panel(main_area)

        # Plot-Bereich
        plot_frame = tk.Frame(main_area, bg=C_BG)
        plot_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        if HAS_MATPLOTLIB:
            self._fig = Figure(figsize=(10, 5), facecolor=C_BG)
            self._canvas = FigureCanvasTkAgg(self._fig, master=plot_frame)
            # Matplotlib NavigationToolbar fuer Zoom/Pan
            from matplotlib.backends.backend_tkagg import NavigationToolbar2Tk
            toolbar_frame = tk.Frame(plot_frame, bg=C_BG)
            toolbar_frame.pack(fill=tk.X, padx=6)
            NavigationToolbar2Tk(self._canvas, toolbar_frame)
            self._canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True, padx=6, pady=4)
            self._canvas.mpl_connect('button_press_event', self._on_plot_click)
        else:
            tk.Label(plot_frame, text="matplotlib nicht verfügbar",
                     bg=C_BG, fg=C_BAD, font=("Consolas", 10)).pack(pady=20)

    def _zoom_to_lut(self):
        self._zoom_mode = "lut"
        self._plot()

    def _zoom_to_full(self):
        self._zoom_mode = "full"
        self._plot()
        self._plot()

    def _on_window_change(self):
        # Wenn Matrix schon berechnet: neu plotten mit neuer Fensterfunktion
        if self.matrix is not None:
            self._plot()

    def _compute(self):
        if self._computing:
            return
        self._stop_event.clear()
        self._computing = True
        self._btn_csv.config(state=tk.DISABLED)
        self._progress["value"] = 0
        self._status.config(text="Lade WAV...")
        # win_name hier lesen (Hauptthread) — nicht im Worker-Thread
        win_name = self._win_var.get()
        threading.Thread(target=self._compute_worker, args=(win_name,), daemon=True).start()

    def _compute_worker(self, win_name: str):
        pa = self.pa
        try:
            atk_mono, sr, _, _ = read_wav_mono_float(pa.attack_path)
            rel_mono, _,  _, _ = read_wav_mono_float(pa.release_path)

            T       = pa.T_int
            T_f     = pa.T_float
            ds      = min(4, T // 500) if T >= 500 else 1
            # v98: NDP-Fensterlaenge muss identisch zu compute_lut() sein.
            # crossfade_len_samples ist die Laenge des Korrelationsfensters.
            # 2*T bzw. pa.lut_r_search_max ist dagegen nur das r-Suchfenster.
            window_len = int(pa.crossfade_len_samples) if pa.crossfade_len_samples >= 4 else 2 * T
            r_search_max = getattr(pa, "lut_r_search_max", 0) or (2 * T)
            r_max        = r_search_max
            # Kuerzen wenn Release zu kurz
            available  = len(rel_mono) - window_len
            if available <= 0:
                window_len = len(rel_mono) // 2
                r_max      = len(rel_mono) - window_len
            elif available < r_max:
                r_max = available
            self._window_len = int(window_len)
            self._r_search_max = int(r_max)
            window_len_d = max(4, window_len // ds)
            r_max_d      = max(1, r_max // ds)

            # Release downgesamplet (muss r_max + window_len abdecken)
            rel_ds = rel_mono[:r_max + window_len + 1 : ds].astype(np.float32)

            # Alle n-Positionen: jede Periode (delta_n=1), damit Feinstrukturen sichtbar werden.
            atk_len = len(atk_mono)
            n_max   = atk_len // T
            ns      = list(range(1, n_max))
            if not ns:
                ns = [1]

            matrix = np.zeros((len(ns), r_max_d), dtype=np.float32)

            for col_idx, n in enumerate(ns):
                if self._stop_event.is_set():
                    return
                cs   = int(round(n * T_f))
                cs_d = cs // ds
                if cs_d + window_len_d > len(atk_mono) // ds:
                    continue
                lw = atk_mono[cs_d * ds : (cs_d + window_len_d) * ds : ds].astype(np.float32)
                if len(lw) < window_len_d:
                    continue

                # Fensterfunktion anwenden — win_name wurde vor Thread-Start gelesen
                win_fn = self.WINDOWS.get(win_name)
                if win_fn:
                    w = getattr(np, win_fn)(len(lw)).astype(np.float32)
                    lw = lw * w
                else:
                    w = None

                na = np.linalg.norm(lw)
                if na < 1e-12:
                    continue
                lw_n = lw / na

                # Alle r auf einmal
                from numpy.lib.stride_tricks import as_strided
                if r_max_d + window_len_d <= len(rel_ds):
                    s0      = rel_ds.strides[0]
                    rel_mat = as_strided(rel_ds,
                                         shape=(r_max_d, window_len_d),
                                         strides=(s0, s0))
                    if w is not None:
                        rel_mat_w = rel_mat * w[np.newaxis, :]
                    else:
                        rel_mat_w = rel_mat
                    norms   = np.linalg.norm(rel_mat_w, axis=1)
                    norms   = np.where(norms < 1e-12, 1.0, norms)
                    scores  = rel_mat_w.dot(lw_n) / norms
                    matrix[col_idx, :len(scores)] = scores

                # Fortschritt ans GUI melden
                pct = 100 * (col_idx + 1) / len(ns)
                self.win.after(0, lambda p=pct, i=col_idx+1, tot=len(ns):
                    self._update_progress(p, i, tot))

            self.matrix = matrix
            self.ns     = ns
            self.r_max_d = r_max_d
            self.ds      = ds
            self.T       = T

            self.win.after(0, self._on_done)

        except Exception as e:
            msg = str(e)
            self.win.after(0, lambda msg=msg: self._status.config(
                text=f"Fehler: {msg}", fg=C_BAD))
        finally:
            self._computing = False

    def _update_progress(self, pct, done, total):
        self._progress["value"] = pct
        self._status.config(text=f"{done}/{total} Positionen")

    def _on_done(self):
        win = getattr(self, "_window_len", None)
        rmax = getattr(self, "_r_search_max", None)
        if win is not None and rmax is not None:
            self._status.config(text=f"Fertig — NDP-Fenster={win}, r_max={rmax}")
        else:
            self._status.config(text="Fertig — zoom mit Mausrad/Toolbar")
        self._btn_csv.config(state=tk.NORMAL)
        self._plot()

    def _plot(self):
        if self.matrix is None or not HAS_MATPLOTLIB:
            return
        self._fig.clear()
        ax = self._fig.add_subplot(111, facecolor=C_BG2)
        self._fig.patch.set_facecolor(C_BG)
        ax.tick_params(colors=C_TEXT2)
        for spine in ax.spines.values():
            spine.set_edgecolor(C_BORDER)

        T   = self.T
        ds  = self.ds
        ns  = self.ns
        mat = self.matrix   # shape: (n_cols, r_max_d)

        # Y-Achse in echten Sample-Offsets (0 .. 2T)
        r_ticks = np.arange(mat.shape[1]) * ds

        im = ax.imshow(
            mat.T,
            aspect="auto",
            origin="lower",
            extent=[ns[0], ns[-1], 0, r_ticks[-1] if len(r_ticks) else 2*T],
            vmin=-1.0, vmax=1.0,
            cmap="RdYlGn",
            interpolation="nearest",
        )
        # Colorbar bewusst unten statt rechts: rechts sitzt die Legende.
        # Sonst kann die NDP-Skala je nach Fensterbreite/DPI die Legende ueberdecken.
        cbar = self._fig.colorbar(
            im, ax=ax, orientation="horizontal",
            fraction=0.055, pad=0.11, aspect=35
        )
        cbar.set_label("NDP-Score", color=C_TEXT2)
        cbar.ax.tick_params(colors=C_TEXT2)

        # Aktive LUT-Punkte: Tuning-Lab-Ergebnis oder Original
        active_pts = self._lab_pts if self._lab_pts is not None else self.pa.lut_points

        # LUT-Punkte & Overlays
        if active_pts:
            pts_sorted = sorted(active_pts, key=lambda p: p.n)

            show_cands = getattr(self, "_show_cand_overlay", None)
            show_cands = show_cands.get() if show_cands is not None else self._debug_var.get()

            if show_cands:
                cand_x, cand_y, cand_s = [], [], []
                chosen_x, chosen_y = [], []
                for p in pts_sorted:
                    for c in getattr(p, "debug_candidates", []):
                        raw_c = c.get("raw", 0) if isinstance(c, dict) else c[0]
                        sc_c  = c.get("score", 0.0) if isinstance(c, dict) else c[1]
                        cand_x.append(p.n)
                        cand_y.append(raw_c)
                        cand_s.append(10 + 30 * max(0.0, min(1.0, float(sc_c))))
                    if hasattr(p, "debug_chosen_raw"):
                        chosen_x.append(p.n)
                        chosen_y.append(int(getattr(p, "debug_chosen_raw")))
                if cand_x:
                    ax.scatter(cand_x, cand_y, c="#666666", marker=".",
                               s=cand_s, alpha=0.45, zorder=5,
                               label="DP-Kandidaten")
                if chosen_x:
                    ax.scatter(chosen_x, chosen_y, c="#ff00ff", marker="s",
                               s=42, alpha=0.9, zorder=7,
                               edgecolors="black", linewidths=0.4,
                               label="DP gewählt raw")

            # LUT-Punkte + DP-Pfad — beide nutzen debug_chosen_raw (= echte Heatmap-Y-Position,
            # ungefaltet, in [0, r_max)).  best_r koennte gefaltet sein und wuerde die Punkte
            # in die untere Haelfte der Landscape schieben.
            show_lut  = getattr(self, "_show_lut_pts",  None)
            show_path = getattr(self, "_show_dp_path",  None)
            show_lut_v  = show_lut  is None or show_lut.get()
            show_path_v = show_path is None or show_path.get()
            if show_lut_v or show_path_v:
                lut_x, lut_y = [], []
                for p in pts_sorted:
                    lut_x.append(p.n)
                    lut_y.append(getattr(p, "debug_chosen_raw", p.best_r))

                if show_path_v:
                    # Linie mit NaN-Brüchen an T-Wrap-Stellen (verhindert chaotische Diagonalen)
                    px, py = [], []
                    prev_y = None
                    for nx, ny in zip(lut_x, lut_y):
                        if prev_y is not None and abs(ny - prev_y) > T * 0.6:
                            px.append(nx); py.append(float("nan"))
                        px.append(nx); py.append(ny)
                        prev_y = ny
                    ax.plot(px, py, color="#ffff00", linewidth=1.5, alpha=0.85,
                            zorder=6, label="DP-Pfad")

                if show_lut_v:
                    ax.scatter(lut_x, lut_y, c="white", marker="o", s=28, zorder=8,
                               edgecolors="black", linewidths=0.6, label="LUT raw_r")

        # Perioden-Linien T, 2T, 3T, 4T je nach Suchfenster
        sp = getattr(self.pa, "lut_search_periods", 2)
        r_sm = getattr(self.pa, "lut_r_search_max", 2 * T)
        for k in range(1, sp + 1):
            r_line = k * T
            if r_line > r_sm + T:
                break
            ax.axhline(r_line, color="cyan" if k == 1 else "deepskyblue",
                        linewidth=0.8 if k == 1 else 0.5,
                        linestyle="--" if k == 1 else ":",
                        alpha=0.7 if k == 1 else 0.45,
                        label=f"{k}T={r_line}")

        # LUT-Zoom-Grenzen berechnen (immer auf Originalpunkte, nicht Lab-Ergebnis)
        zoom_pts = sorted(self.pa.lut_points, key=lambda p: p.n) if self.pa.lut_points else []
        full_xlim = (float(ns[0]),  float(ns[-1]))  if len(ns)      else None
        full_ylim = (0.0, float(r_ticks[-1])) if len(r_ticks) else None
        if zoom_pts:
            ns_lut = [p.n for p in zoom_pts]
            rs_lut = [p.best_r for p in zoom_pts]
            x_margin = max(2, 0.05 * (max(ns_lut) - min(ns_lut)))
            y_margin = max(10, T / 4)
            self._lut_xlim = (min(ns_lut) - x_margin, max(ns_lut) + x_margin)
            self._lut_ylim = (max(0, min(rs_lut) - y_margin),
                              min(r_ticks[-1] if len(r_ticks) else 2*T, max(rs_lut) + y_margin))
        elif not zoom_pts:
            self._lut_xlim = None
            self._lut_ylim = None

        # Zoom anwenden
        zoom = getattr(self, "_zoom_mode", "lut")
        if zoom == "full" and full_xlim:
            ax.set_xlim(full_xlim)
            ax.set_ylim(full_ylim)
        elif self._lut_xlim is not None:
            ax.set_xlim(self._lut_xlim)
            ax.set_ylim(self._lut_ylim)

        win_name = self._win_var.get()
        ax.set_xlabel("Periodenindex n", color=C_TEXT2)
        ax.set_ylabel("r (Sample-Offset in Release)", color=C_TEXT2)
        ax.set_title(
            f"Korrelationslandschaft  [{win_name}]  —  "
            f"{self.pa.rank_name} {midi_to_name(self.pa.midi_note)}",
            color=C_TEXT, fontsize=10)
        ax.legend(facecolor=C_BG2, edgecolor=C_BORDER,
                   labelcolor=C_TEXT, fontsize=8,
                   loc="upper left", bbox_to_anchor=(1.02, 1), borderaxespad=0)

        # Rechts Platz fuer die Legende lassen; unten Platz fuer die horizontale Colorbar.
        self._fig.tight_layout(rect=[0, 0.08, 0.82, 1])
        self._canvas.draw()

    # ─── Branch Tuning Lab ────────────────────────────────────────────────────

    def _build_lab_panel(self, parent):
        """Rechte Seitenleiste: interaktives Branch Tuning Lab."""
        frame = tk.Frame(parent, bg=C_BG3, bd=1, relief=tk.RIDGE, width=275)
        frame.pack(side=tk.RIGHT, fill=tk.Y, padx=(2, 6), pady=4)
        frame.pack_propagate(False)

        lbl_kw  = dict(bg=C_BG3, fg=C_TEXT,  font=("Consolas", 9), anchor=tk.W)
        sec_kw  = dict(bg=C_BG3, fg=C_ACCENT, font=("Consolas", 9, "bold"), anchor=tk.W)
        ent_kw  = dict(bg=C_BG2, fg=C_TEXT,  font=("Consolas", 9), width=7,
                       relief=tk.SUNKEN, bd=1)
        btn_kw  = dict(bg=C_BTN, fg=C_TEXT,  font=("Consolas", 9), relief=tk.RAISED,
                       padx=6, pady=2, activebackground=C_BTN_ACT, cursor="hand2")

        tk.Label(frame, text="Branch Tuning Lab", bg=C_BG3, fg=C_TEXT,
                 font=("Consolas", 10, "bold"), anchor=tk.W).pack(
                 fill=tk.X, padx=6, pady=(8, 4))

        # ── Kandidaten ───────────────────────────────────────────────────────
        tk.Label(frame, text="── Kandidaten ──", **sec_kw).pack(
            fill=tk.X, padx=6, pady=(4, 2))

        def _row(label, var, parent=frame):
            f = tk.Frame(parent, bg=C_BG3)
            f.pack(fill=tk.X, padx=6, pady=1)
            tk.Label(f, text=label, width=20, **lbl_kw).pack(side=tk.LEFT)
            tk.Entry(f, textvariable=var, **ent_kw).pack(side=tk.LEFT)
            return f

        default_phase_sep_div = int(round(1.0 / BRANCH_PHASE_SEPARATION_FACTOR))
        self._lab_topk          = tk.StringVar(value=str(BRANCH_TOP_K))
        self._lab_phase_sep_div = tk.StringVar(value=str(default_phase_sep_div))
        _row("Top-K",          self._lab_topk)
        _row("Phase-Sep (T/)", self._lab_phase_sep_div)

        # ── DP-Parameter ─────────────────────────────────────────────────────
        tk.Label(frame, text="── DP-Parameter ──", **sec_kw).pack(
            fill=tk.X, padx=6, pady=(8, 2))

        dp_defs = [
            ("Kink-Penalty",   "_lab_kink_pen",  str(BRANCH_KINK_PENALTY)),
            ("Switch-Penalty", "_lab_switch_pen", str(BRANCH_SWITCH_PENALTY)),
            ("Smooth-Penalty", "_lab_smooth_pen", str(BRANCH_GLOBAL_SMOOTH_PENALTY)),
            ("Score-Weight",   "_lab_score_w",    str(BRANCH_DP_SCORE_WEIGHT)),
            ("Kink N-Limit",   "_lab_kink_n",     str(BRANCH_KINK_SCORE_N_LIMIT)),
            ("Kink Cap",       "_lab_kink_cap",   str(BRANCH_KINK_PENALTY_CAP)),
        ]
        for label, attr, default in dp_defs:
            var = tk.StringVar(value=default)
            setattr(self, attr, var)
            _row(label, var)

        # ── Buttons ──────────────────────────────────────────────────────────
        tk.Button(frame, text="▶  Recompute", width=22,
                  command=self._on_lab_recompute, **btn_kw).pack(
                  fill=tk.X, padx=6, pady=(10, 2))
        tk.Button(frame, text="↺  Reset (Original)", width=22,
                  command=self._on_lab_reset, **btn_kw).pack(
                  fill=tk.X, padx=6, pady=2)

        # ── Overlays ─────────────────────────────────────────────────────────
        tk.Label(frame, text="── Overlays ──", **sec_kw).pack(
            fill=tk.X, padx=6, pady=(8, 2))

        self._show_dp_path       = tk.BooleanVar(value=True)
        self._show_cand_overlay  = tk.BooleanVar(value=True)
        self._show_lut_pts       = tk.BooleanVar(value=True)
        chk_kw = dict(bg=C_BG3, fg=C_TEXT, selectcolor=C_BG2, font=("Consolas", 9),
                      activebackground=C_BG3, activeforeground=C_TEXT)
        for text, var in [("DP-Pfad (gelb)",     self._show_dp_path),
                          ("Kandidaten (grau)",   self._show_cand_overlay),
                          ("LUT-Punkte (weiß)",   self._show_lut_pts)]:
            tk.Checkbutton(frame, text=text, variable=var,
                           command=self._plot, **chk_kw).pack(anchor=tk.W, padx=8)

        # ── Status ───────────────────────────────────────────────────────────
        tk.Label(frame, text="── Status ──", **sec_kw).pack(
            fill=tk.X, padx=6, pady=(8, 2))
        self._lab_status = tk.Label(frame, text="Original",
                                     bg=C_BG3, fg=C_TEXT2,
                                     font=("Consolas", 8), anchor=tk.W,
                                     justify=tk.LEFT, wraplength=255)
        self._lab_status.pack(fill=tk.X, padx=6)

        # ── Punkt-Info (Klick) ────────────────────────────────────────────────
        tk.Label(frame, text="── Punkt-Info ──", **sec_kw).pack(
            fill=tk.X, padx=6, pady=(8, 2))
        self._lab_info = tk.Text(frame, height=9, bg=C_BG2, fg=C_TEXT2,
                                  font=("Consolas", 8), relief=tk.SUNKEN, bd=1,
                                  state=tk.DISABLED, wrap=tk.NONE)
        self._lab_info.pack(fill=tk.BOTH, padx=6, pady=(0, 6), expand=True)

    def _on_lab_recompute(self):
        """Recompute DP with current Tuning Lab parameters."""
        import copy, time
        pa = self.pa
        if not pa.lut_points:
            self._lab_status.config(text="Keine LUT-Punkte vorhanden.")
            return
        try:
            top_k          = int(self._lab_topk.get())
            phase_sep_div  = max(1.0, float(self._lab_phase_sep_div.get()))
            phase_sep_f    = 1.0 / phase_sep_div
            kink_pen       = float(self._lab_kink_pen.get())
            switch_pen     = float(self._lab_switch_pen.get())
            smooth_pen     = float(self._lab_smooth_pen.get())
            score_w        = float(self._lab_score_w.get())
            kink_n         = int(self._lab_kink_n.get())
            kink_cap       = float(self._lab_kink_cap.get())
        except (ValueError, tk.TclError) as exc:
            self._lab_status.config(text=f"Ungültige Eingabe:\n{exc}")
            return

        params = {
            'top_k':            top_k,
            'phase_sep_factor': phase_sep_f,
            'smooth_penalty':   smooth_pen,
            'kink_penalty':     kink_pen,
            'switch_penalty':   switch_pen,
            'kink_n_limit':     kink_n,
            'kink_penalty_cap': kink_cap,
            'score_weight':     score_w,
        }

        # Kandidaten-Quelle: all_candidates (vor Phase-Trennung) falls vorhanden.
        # Wichtig: pa.lut_points kommen NACH dem Folding, daher ist raw_r ggf.
        # gefaltet (= debug_chosen_raw % T).  Das "ensure current" im DP wuerde
        # dann den falschen (gefalteten) Kandidaten einschleusen.  Deshalb immer
        # debug_chosen_raw (ungefaltet) als raw_r/best_r setzen.
        pts_copy = [copy.copy(p) for p in pa.lut_points]
        for p in pts_copy:
            if hasattr(p, 'debug_chosen_raw'):
                p.raw_r  = int(p.debug_chosen_raw)
                p.best_r = int(p.debug_chosen_raw)
            src = list(getattr(p, 'all_candidates', None)
                       or getattr(p, 'candidates', None)
                       or [])
            p.candidates = src

        T_int          = pa.T_int
        r_max          = getattr(pa, 'lut_r_search_max', 0) or (2 * T_int)
        search_periods = getattr(pa, 'lut_search_periods', 2)
        hn             = getattr(pa, 'harmonic_number', 8)

        t0 = time.perf_counter()
        try:
            new_pts = _run_global_branch_dp(pts_copy, T_int, r_max, search_periods, hn, params)
        except Exception as exc:
            self._lab_status.config(text=f"DP-Fehler:\n{exc}")
            return
        elapsed_ms = int((time.perf_counter() - t0) * 1000)

        self._lab_pts = new_pts
        total_cands = sum(len(getattr(p, 'candidates', [])) for p in new_pts)
        avg_cands   = total_cands // max(1, len(new_pts))
        self._lab_status.config(
            text=(f"DP: {elapsed_ms} ms\n"
                  f"Punkte: {len(new_pts)}\n"
                  f"Ø Kandidaten: {avg_cands}\n"
                  f"Top-K={top_k}  T/{int(phase_sep_div)}"))
        self._plot()

    def _on_lab_reset(self):
        """Verwerfe Tuning-Lab-Ergebnis, zeige Original."""
        self._lab_pts = None
        self._lab_status.config(text="Original")
        self._plot()

    def _on_plot_click(self, event):
        """Klick in die Heatmap: zeige Kandidaten des naechsten LUT-Punkts."""
        if event.inaxes is None:
            return
        pts = self._lab_pts if self._lab_pts is not None else self.pa.lut_points
        if not pts:
            return
        n_click = event.xdata
        if n_click is None:
            return
        closest = min(pts, key=lambda p: abs(p.n - n_click))
        cands_raw = getattr(closest, 'debug_candidates', [])
        if not cands_raw:
            cands_raw = [(r, s) for r, s in (getattr(closest, 'candidates', []) or [])]
        chosen_raw = getattr(closest, 'debug_chosen_raw', closest.best_r)

        lines = [
            f"n={closest.n}  phase={closest.phase}",
            f"chosen r={chosen_raw}  sc={closest.best_score:.4f}",
            f"track_r={getattr(closest, 'track_r', '?'):.1f}" if isinstance(
                getattr(closest, 'track_r', None), float) else "",
            "",
            "Kandidaten:",
        ]
        for c in cands_raw:
            if isinstance(c, dict):
                rc, sc = c.get('raw', '?'), c.get('score', 0.0)
            else:
                rc, sc = c[0], c[1]
            sel = " ←" if rc == chosen_raw else ""
            lines.append(f"  r={rc:6}  sc={float(sc):.4f}{sel}")

        self._lab_info.config(state=tk.NORMAL)
        self._lab_info.delete("1.0", tk.END)
        self._lab_info.insert("1.0", "\n".join(lines))
        self._lab_info.config(state=tk.DISABLED)

    # ─── Export ───────────────────────────────────────────────────────────────

    def _export_debug_csv(self):
        """v115: Exhaustive DP debug export is disabled for normal-speed scans."""
        try:
            self._status.config(text="Debug export disabled in v142-more-candidates-phasepred")
        except Exception:
            pass
        return

    def _export_csv(self):
        if self.matrix is None:
            return
        from tkinter import filedialog
        path = filedialog.asksaveasfilename(
            parent=self.win,
            defaultextension=".csv",
            filetypes=[("CSV", "*.csv"), ("Alle", "*.*")],
            initialfile=(f"corr_landscape_{self.pa.rank_name}_{midi_to_name(self.pa.midi_note)}"
                         f"_{self.pa.perspective}_{self.pa.release_type}.csv").replace(" ", "_"),
        )
        if not path:
            return

        import csv
        ds = self.ds
        with open(path, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            # Header: Metadaten
            w.writerow(["# GrandOrgue Korrelationslandschaft"])
            w.writerow(["# Register", self.pa.rank_name])
            w.writerow(["# Note", midi_to_name(self.pa.midi_note)])
            w.writerow(["# Perspektive", self.pa.perspective])
            w.writerow(["# Release", self.pa.release_type])
            w.writerow(["# T_int", self.T])
            w.writerow(["# ds", ds])
            w.writerow(["# NDP_Window_Samples", getattr(self, "_window_len", "")])
            w.writerow(["# R_Search_Max", getattr(self, "_r_search_max", "")])
            w.writerow(["# Fensterfunktion", self._win_var.get()])
            w.writerow([])
            # Spaltenheader: n-Werte
            w.writerow(["r\n"] + self.ns)
            # Daten: eine Zeile pro r_d
            for r_d in range(self.matrix.shape[1]):
                r_full = r_d * ds
                w.writerow([r_full] + [f"{v:.4f}" for v in self.matrix[:, r_d]])

        self._status.config(text=f"Exportiert: {path}")


# ─── GUI ──────────────────────────────────────────────────────────────────────

SEVERITY_COLOR = ["#1a6b1a", "#8a5c00", "#8b0000"]
SEVERITY_BG    = ["#d4edda", "#fff3cd", "#f8d7da"]
SEVERITY_FG    = ["#155724", "#856404", "#721c24"]

# Helles Theme
C_BG     = "#f5f5f5"
C_BG2    = "#ffffff"
C_BG3    = "#e8e8e8"

# Zeilenfarben für Severity-Tags im Report (Hintergrund / Vordergrund)
SEV_COLORS = {
    "sev_ok":          ("#e8f5e9", "#1b5e20"),   # grün
    "sev_warn":        ("#fff8e1", "#e65100"),   # gelb
    "sev_bad":         ("#ffebee", "#b71c1c"),   # rot
    "sev_error":       ("#fce4ec", "#880e4f"),   # dunkelrot
    "sev_legacy":      ("#f5f5f5", "#555555"),   # grau
    "sev_legacy_warn": ("#fff3e0", "#bf360c"),   # orange
    "sev_legacy_badlut": ("#ffe0b2", "#e65100"), # kräftiges orange
    "sev_drift":       ("#ede7f6", "#4527a0"),   # lila
}
C_BORDER = "#cccccc"
C_TEXT   = "#1a1a1a"
C_TEXT2  = "#555555"
C_ACCENT = "#1565c0"
C_SEL    = "#bbdefb"
C_BTN    = "#e0e0e0"
C_BTN_ACT= "#bdbdbd"
C_RANK   = "#1565c0"
C_NOTE   = "#1a1a1a"
C_OK     = "#1b5e20"
C_WARN   = "#e65100"
C_BAD    = "#b71c1c"
C_DENSE  = "#1565c0"
C_SPARSE = "#2e7d32"
C_GAP    = "#e65100"
C_DRIFT  = "#6a1b9a"

MIDI_NOTE_NAMES = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"]

def midi_to_name(n: int) -> str:
    return f"{MIDI_NOTE_NAMES[n%12]}{n//12-1}"


class LUTAnalyzerApp(tk.Tk):
    def __init__(self, initial_organ: str = None):
        super().__init__()
        self.title(f"GrandOrgue LUT Analyzer — {TOOL_VERSION}")
        self.geometry("1400x900")
        self.configure(bg=C_BG)

        self._analyses: dict = {}   # key -> PipeAnalysis
        self._key_to_item: dict = {}  # key -> tree item_id (O(1) lookup)
        self._organ_path: str = ""
        self._work_queue  = queue.Queue()
        self._result_queue = queue.Queue()
        self._total_work  = 0
        self._done_work   = 0

        self._build_ui()
        self.after(100, self._poll_results)

        if initial_organ:
            self.after(200, lambda: self._load_organ(initial_organ))

    # ── UI-Aufbau ──────────────────────────────────────────────────────────────

    def _build_ui(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("Treeview",
                         background=C_BG2, foreground=C_TEXT,
                         fieldbackground=C_BG2, rowheight=22,
                         font=("Consolas", 10))
        style.configure("Treeview.Heading",
                         background=C_BG3, foreground=C_TEXT,
                         font=("Consolas", 10, "bold"))
        style.map("Treeview", background=[("selected", C_SEL)],
                              foreground=[("selected", C_TEXT)])
        style.configure("TNotebook", background=C_BG, tabmargins=[2,5,0,0])
        style.configure("TNotebook.Tab", background=C_BG3, foreground=C_TEXT,
                         padding=[10,4], font=("Consolas", 10))
        style.map("TNotebook.Tab", background=[("selected", C_BG2)])
        style.configure("TProgressbar", troughcolor=C_BG3, background=C_ACCENT)

        # Toolbar
        toolbar = tk.Frame(self, bg=C_BG3, pady=6)
        toolbar.pack(fill=tk.X)

        btn_kw = dict(bg=C_BTN, fg=C_TEXT, relief=tk.RAISED,
                       font=("Consolas",10), padx=12, pady=4,
                       activebackground=C_BTN_ACT, activeforeground=C_TEXT,
                       cursor="hand2")

        tk.Button(toolbar, text="📂  Organ laden",
                  command=self._open_dialog, **btn_kw).pack(side=tk.LEFT, padx=6)

        self._btn_all = tk.Button(toolbar, text="🔍  Alles analysieren",
                  command=self._analyze_all, state=tk.DISABLED, **btn_kw)
        self._btn_all.pack(side=tk.LEFT, padx=4)

        self._btn_report = tk.Button(toolbar, text="📄  Report speichern",
                  command=self._save_report, state=tk.DISABLED, **btn_kw)
        self._btn_report.pack(side=tk.LEFT, padx=4)

        self._organ_label = tk.Label(toolbar, text="Keine Datei geladen",
                                      bg=C_BG3, fg=C_TEXT2,
                                      font=("Consolas",10))
        self._organ_label.pack(side=tk.LEFT, padx=12)

        # Downsampling-Einstellung — entspricht GOSettingsOptions::CorrLutDownsampling
        self._ds_var = tk.BooleanVar(value=True)
        tk.Checkbutton(toolbar, text="Downsampling",
                       variable=self._ds_var,
                       bg=C_BG3, fg=C_TEXT,
                       selectcolor=C_BG2, activebackground=C_BG3,
                       activeforeground=C_TEXT,
                       font=("Consolas", 10)
                       ).pack(side=tk.LEFT, padx=(12, 4))

        # Fortschrittsbalken
        self._progress_var = tk.DoubleVar()
        self._progress = ttk.Progressbar(toolbar, variable=self._progress_var,
                                          length=200, mode="determinate")
        self._progress.pack(side=tk.RIGHT, padx=12)
        self._progress_label = tk.Label(toolbar, text="", bg=C_BG3,
                                         fg=C_TEXT2, font=("Consolas", 9))
        self._progress_label.pack(side=tk.RIGHT, padx=4)

        # Haupt-Split: Baum links / Detail rechts
        paned = tk.PanedWindow(self, orient=tk.HORIZONTAL,
                                bg=C_BG3, sashwidth=6,
                                sashrelief=tk.RAISED)
        paned.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

        # Linkes Panel — Baum
        left = tk.Frame(paned, bg=C_BG)
        paned.add(left, minsize=280)

        tk.Label(left, text="Register  /  Pfeife  /  Release",
                  bg=C_BG, fg=C_TEXT2,
                  font=("Consolas",9)).pack(anchor=tk.W, padx=4, pady=(4,2))

        tree_frame = tk.Frame(left, bg=C_BG2)
        tree_frame.pack(fill=tk.BOTH, expand=True)

        self._tree = ttk.Treeview(tree_frame, columns=("info",),
                                   show="tree headings", selectmode="browse")
        self._tree.heading("#0",    text="Eintrag")
        self._tree.heading("info",  text="Status / Erklärung")
        self._tree.column("#0",     width=220)
        self._tree.column("info",   width=400, anchor=tk.W)
        vsb = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL,
                             command=self._tree.yview)
        self._tree.configure(yscrollcommand=vsb.set)
        vsb.pack(side=tk.RIGHT, fill=tk.Y)
        self._tree.pack(fill=tk.BOTH, expand=True)
        self._tree.bind("<<TreeviewSelect>>", self._on_select)

        # Rechtes Panel — Tabs
        right = tk.Frame(paned, bg=C_BG)
        paned.add(right, minsize=600)

        self._tabs = ttk.Notebook(right)
        self._tabs.pack(fill=tk.BOTH, expand=True)

        # Tab 1: Detail
        self._tab_detail = tk.Frame(self._tabs, bg=C_BG)
        self._tabs.add(self._tab_detail, text="  Detail  ")
        self._build_detail_tab()

        # Tab 2: Report
        self._tab_report = tk.Frame(self._tabs, bg=C_BG)
        self._tabs.add(self._tab_report, text="  Report  ")
        self._build_report_tab()

    def _build_detail_tab(self):
        f = self._tab_detail

        # Info-Bereich oben
        info_frame = tk.Frame(f, bg=C_BG2, pady=8, padx=12, relief=tk.GROOVE, bd=1)
        info_frame.pack(fill=tk.X, padx=6, pady=6)

        self._detail_title = tk.Label(info_frame, text="Keine Pfeife ausgewählt",
                                       bg=C_BG2, fg=C_ACCENT,
                                       font=("Consolas", 13, "bold"), anchor=tk.W)
        self._detail_title.pack(fill=tk.X)

        self._detail_info = tk.Label(info_frame, text="",
                                      bg=C_BG2, fg=C_TEXT,
                                      font=("Consolas", 10), anchor=tk.W,
                                      justify=tk.LEFT)
        self._detail_info.pack(fill=tk.X, pady=(4,0))

        # LUT-Tabelle
        tbl_frame = tk.Frame(f, bg=C_BG)
        tbl_frame.pack(fill=tk.X, padx=6, pady=(0,4))

        tk.Label(tbl_frame, text="LUT-Stützpunkte",
                  bg=C_BG, fg=C_TEXT2,
                  font=("Consolas",9)).pack(anchor=tk.W, padx=4)

        cols = ("n","loop_pos","best_r","score","phase","raw_r","folded","fold_ratio")
        self._lut_table = ttk.Treeview(tbl_frame, columns=cols,
                                        show="headings", height=8)
        headers = {"n":"n","loop_pos":"Loop-Pos","best_r":"best_r",
                   "score":"Score","phase":"Phase",
                   "raw_r":"raw_r","folded":"Fold?","fold_ratio":"Fold%"}
        widths   = {"n":50,"loop_pos":100,"best_r":80,"score":80,"phase":70,
                    "raw_r":80,"folded":50,"fold_ratio":60}
        for c in cols:
            self._lut_table.heading(c, text=headers[c])
            self._lut_table.column(c, width=widths[c], anchor=tk.CENTER)
        self._lut_table.pack(fill=tk.X, padx=4)
        self._lut_table.bind("<<TreeviewSelect>>", self._on_lut_select)

        # Button: Korrelationslandschaft
        btn_frame = tk.Frame(tbl_frame, bg=C_BG)
        btn_frame.pack(fill=tk.X, padx=4, pady=(4,0))
        btn_kw2 = dict(bg=C_BTN, fg=C_TEXT, relief=tk.RAISED,
                       font=("Consolas",9), padx=8, pady=3,
                       activebackground=C_BTN_ACT, cursor="hand2")
        tk.Button(btn_frame, text="📊  Korrelationslandschaft öffnen",
                  command=self._open_corr_landscape, **btn_kw2).pack(side=tk.LEFT, padx=2)
        tk.Button(btn_frame, text="🎵  Crossfade-Simulation",
                  command=self._open_crossfade_sim, **btn_kw2).pack(side=tk.LEFT, padx=2)

        # Plot-Bereich
        if HAS_MATPLOTLIB:
            plot_frame = tk.Frame(f, bg=C_BG)
            plot_frame.pack(fill=tk.BOTH, expand=True, padx=6, pady=4)

            self._fig = Figure(figsize=(8, 4), facecolor=C_BG)
            self._canvas = FigureCanvasTkAgg(self._fig, master=plot_frame)
            self._canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        else:
            tk.Label(f, text="matplotlib nicht verfügbar — Plots deaktiviert",
                      bg=C_BG, fg=C_BAD,
                      font=("Consolas",10)).pack(pady=20)

    def _build_report_tab(self):
        f = self._tab_report

        tk.Label(f, text="Problematischste Pfeifen (sortiert nach Schwere)",
                  bg=C_BG, fg=C_TEXT2,
                  font=("Consolas",9)).pack(anchor=tk.W, padx=10, pady=(8,2))

        cols = ("sev","rank","note","persp","rel","freq","T","stable_n",
                "transient_n","n_pts","score_mean","score_min","score_max",
                "bestr_coherence","amp_ratio","phase3","atk_dur","rel_dur","error")
        self._report_table = ttk.Treeview(f, columns=cols,
                                           show="headings", height=30)
        headers = {
            "sev":"", "rank":"Register", "note":"Note",
            "persp":"Perspektive", "rel":"Release",
            "freq":"Freq Hz", "T":"T",
            "stable_n":"Stabil n", "transient_n":"Trans.Ende n",
            "n_pts":"LUT d+s+g", "score_mean":"Score Mean",
            "score_min":"Score Min", "score_max":"Score Max",
            "bestr_coherence":"R Streu.", "amp_ratio":"Amp-Ratio",
            "phase3":"Gaps", "atk_dur":"Atk ms", "rel_dur":"Rel ms",
            "error":"Fehler"
        }
        widths = {
            "sev":110, "rank":200, "note":55, "persp":130, "rel":80,
            "freq":70, "T":45, "stable_n":70, "transient_n":95,
            "n_pts":80, "score_mean":85, "score_min":75, "score_max":75,
            "bestr_coherence":80, "amp_ratio":75,
            "phase3":45, "atk_dur":65, "rel_dur":65, "error":200
        }
        for c in cols:
            self._report_table.heading(c, text=headers[c])
            stretch = c in ("rank", "error", "persp")
            self._report_table.column(c, width=widths[c],
                                       minwidth=widths[c]//2,
                                       anchor=tk.W if c in ("rank","error","sev") else tk.CENTER,
                                       stretch=stretch)

        # Zeilenfarben für Severity
        for tag, (bg, fg) in SEV_COLORS.items():
            self._report_table.tag_configure(tag, background=bg, foreground=fg)

        vsb2 = ttk.Scrollbar(f, orient=tk.VERTICAL,
                              command=self._report_table.yview)
        self._report_table.configure(yscrollcommand=vsb2.set)
        vsb2.pack(side=tk.RIGHT, fill=tk.Y, padx=(0,6), pady=6)
        self._report_table.pack(fill=tk.BOTH, expand=True, padx=(6,0), pady=6)
        self._report_table.bind("<<TreeviewSelect>>", self._on_report_select)

    # ── Datei-Dialog ───────────────────────────────────────────────────────────

    def _open_dialog(self):
        path = filedialog.askopenfilename(
            title="Organ-Datei öffnen",
            filetypes=[("Organ files","*.organ"),("Alle Dateien","*.*")])
        if path:
            self._load_organ(path)

    def _load_organ(self, path: str):
        self._organ_path = path
        self._organ_label.config(text=os.path.basename(path), fg=C_TEXT)
        self._analyses.clear()
        self._key_to_item.clear()
        self._tree.delete(*self._tree.get_children())
        self._btn_all.config(state=tk.NORMAL)
        self._btn_report.config(state=tk.DISABLED)

        # Baum aufbauen (ohne Analyse)
        self._pipe_descs = parse_organ_file(path)
        if not self._pipe_descs:
            messagebox.showerror("Fehler",
                "Keine Pfeifen gefunden. Bitte prüfe den Organ-Dateipfad.")
            return

        # Baum: Register → MIDI-Note → Perspektive/Release
        ranks = {}
        for d in self._pipe_descs:
            r = d["rank_name"]
            n = d["midi_note"]
            p = d["perspective"]
            t = d["release_type"]
            ranks.setdefault(r, {}).setdefault(n, []).append(d)

        for rank_name in sorted(ranks):
            rank_id = self._tree.insert("", tk.END,
                                         text=f"♩ {rank_name}",
                                         values=("–",), tags=("rank",))
            self._tree.tag_configure("rank", foreground=C_RANK,
                                      font=("Consolas",10,"bold"))
            for midi in sorted(ranks[rank_name]):
                note_id = self._tree.insert(rank_id, tk.END,
                                             text=f"  {midi_to_name(midi)} ({midi})",
                                             values=("–",), tags=("note",))
                self._tree.tag_configure("note", foreground=C_NOTE)
                for d in ranks[rank_name][midi]:
                    key = f"{rank_name}|{midi}|{d['perspective']}|{d['release_type']}"
                    label = f"    {d['perspective']} / {d['release_type']}"
                    item_id = self._tree.insert(note_id, tk.END,
                                                 text=label,
                                                 values=("⬜ ausstehend",),
                                                 tags=("pipe",))
                    self._tree.tag_configure("pipe", foreground=C_OK)
                    # item_id → key Mapping
                    self._tree.item(item_id, tags=("pipe", key))
                    self._tree.tag_configure(key, foreground=C_OK)
                    self._key_to_item[key] = item_id   # O(1) lookup
                    # Speichere Descriptor im Analyse-Dict (noch ohne Ergebnis)
                    self._analyses[key] = d  # temporär Descriptor

        self._title(f"GrandOrgue LUT Analyzer — {TOOL_VERSION} — {os.path.basename(path)}")

    def _title(self, t):
        self.title(t)

    # ── Analyse ────────────────────────────────────────────────────────────────

    def _analyze_all(self):
        self._btn_all.config(state=tk.DISABLED)
        self._done_work = 0
        self._total_work = len(self._pipe_descs)
        self._progress_var.set(0)
        self._progress_label.config(text=f"0/{self._total_work}")

        def worker():
            # ProcessPoolExecutor umgeht Pythons GIL — echte CPU-Parallelität.
            # analyze_pipe ist eine reine Funktion ohne GUI-Referenzen → picklebar.
            n_workers = min(20, max(2, (_os.cpu_count() or 4)))
            self._progress_label.config(text=f"0/{self._total_work}  ({n_workers} Prozesse)")
            use_ds = self._ds_var.get()
            descs = [{**d, "downsampling": use_ds} for d in self._pipe_descs]
            with concurrent.futures.ProcessPoolExecutor(max_workers=n_workers) as pool:
                futures = {pool.submit(analyze_pipe, d): d
                           for d in descs}
                for fut in concurrent.futures.as_completed(futures):
                    try:
                        result = fut.result()
                    except Exception as e:
                        d = futures[fut]
                        result = PipeAnalysis(
                            organ_base=d.get("organ_base",""),
                            rank_name=d.get("rank_name","?"),
                            midi_note=d.get("midi_note",0),
                            perspective=d.get("perspective","?"),
                            release_type=d.get("release_type","?"),
                            attack_path=d.get("attack_path",""),
                            release_path=d.get("release_path",""),
                            error=str(e),
                        )
                    self._result_queue.put(result)

        threading.Thread(target=worker, daemon=True).start()

    def _poll_results(self):
        # Max 30 Ergebnisse pro Poll-Zyklus — GUI bleibt responsiv
        for _ in range(30):
            try:
                pa: PipeAnalysis = self._result_queue.get_nowait()
            except queue.Empty:
                break
            key = f"{pa.rank_name}|{pa.midi_note}|{pa.perspective}|{pa.release_type}"
            self._analyses[key] = pa
            self._done_work += 1
            self._update_tree_item(key, pa)

            if self._done_work >= self._total_work and self._total_work > 0:
                pct = 100.0
                self._progress_var.set(pct)
                self._progress_label.config(
                    text=f"{self._done_work}/{self._total_work}  ✓")
                self._btn_all.config(state=tk.NORMAL)
                self._btn_report.config(state=tk.NORMAL)
                self._build_report()
                self._tabs.select(self._tab_report)
                self.after(50, self._poll_results)
                return

        # Fortschritt nach dem Batch aktualisieren (ein Aufruf statt N)
        if self._total_work > 0:
            pct = 100 * self._done_work / self._total_work
            self._progress_var.set(pct)
            self._progress_label.config(
                text=f"{self._done_work}/{self._total_work}")
        self.after(50, self._poll_results)

    def _update_tree_item(self, key: str, pa: PipeAnalysis):
        """Aktualisiert Farbe und Status eines Baum-Eintrags (O(1) via dict)."""
        pipe_item = self._key_to_item.get(key)
        if pipe_item is None:
            return
        colors = [C_OK, C_WARN, C_BAD]
        self._tree.item(pipe_item,
                        values=(pa.status_text,),
                        tags=("pipe", key))
        self._tree.tag_configure(key, foreground=colors[pa.severity])

    # ── Selektion ──────────────────────────────────────────────────────────────

    def _on_select(self, event):
        sel = self._tree.selection()
        if not sel:
            return
        tags = self._tree.item(sel[0], "tags")
        # Finde key-Tag (enthält "|")
        key = next((t for t in tags if "|" in t), None)
        if not key:
            return
        pa = self._analyses.get(key)
        if pa is None:
            return

        if isinstance(pa, dict):
            # Noch nicht analysiert — on-demand
            pa = analyze_pipe({**pa, "downsampling": self._ds_var.get()})
            self._analyses[key] = pa
            self._update_tree_item(key, pa)

        self._show_detail(pa)

    def _open_crossfade_sim(self):
        if not self._current_pa:
            return
        pa = self._current_pa
        if pa.error:
            return
        CrossfadeSimWindow(self, pa)
    def _open_corr_landscape(self):
        """Oeffnet separates Fenster mit vollstaendiger Korrelationslandschaft."""
        if not self._current_pa:
            return
        pa = self._current_pa
        if pa.error:
            tk.messagebox.showinfo("Korrelationslandschaft",
                                   f"Keine Daten: {pa.error}", parent=self)
            return
        CorrLandscapeWindow(self, pa)

    def _on_report_select(self, event):
        sel = self._report_table.selection()
        if not sel:
            return
        tags = self._report_table.item(sel[0], "tags")
        key = next((t for t in tags if "|" in t), None)
        if not key:
            return
        pa = self._analyses.get(key)
        if pa and isinstance(pa, PipeAnalysis):
            self._tabs.select(self._tab_detail)
            self._show_detail(pa)

    def _on_lut_select(self, event):
        """Klick auf LUT-Punkt → Waveform-Plot aktualisieren."""
        sel = self._lut_table.selection()
        if not sel or not self._current_pa:
            return
        vals = self._lut_table.item(sel[0], "values")
        try:
            n = int(vals[0])
        except (ValueError, IndexError):
            return
        pt = next((p for p in self._current_pa.lut_points if p.n == n), None)
        if pt:
            self._plot_waveform(self._current_pa, pt)

    # ── Detail-Anzeige ─────────────────────────────────────────────────────────

    _current_pa: Optional[PipeAnalysis] = None

    def _show_detail(self, pa: PipeAnalysis):
        self._current_pa = pa

        # Titel
        note_str = midi_to_name(pa.midi_note)
        self._detail_title.config(
            text=f"{pa.rank_name}  —  {note_str}  —  {pa.perspective} / {pa.release_type}",
            fg=[C_OK, C_WARN, C_BAD][pa.severity])

        # Info-Text
        if pa.error:
            info = f"❌ Fehler: {pa.error}"
        else:
            info_parts = [
                f"Tool-Version: {TOOL_VERSION}",
                f"T_float={pa.T_float:.2f}  T_int={pa.T_int}  SR={pa.sample_rate}Hz"
                + (f"  CMNDF: T/2={pa.cmndf_at_T_half:.3f}  T={pa.cmndf_at_T:.3f}  2T={pa.cmndf_at_2T:.3f}"
                   if not (pa.cmndf_at_T_half != pa.cmndf_at_T_half) else ""),  # nan check
                f"Perioden-Debug: smpl_T={pa.smpl_T_float:.4f}  hn_T={pa.hn_T_float:.4f}  autocorr_T={pa.autocorr_T_float:.4f}  search=[{pa.autocorr_min_p},{pa.autocorr_max_p}]",
                f"Attack: {pa.attack_path}",
                f"Loop: {pa.loop_start}–{pa.loop_end}  ({pa.loop_len} Samples, {pa.n_total} Perioden)",
                f"Stabilisiert: {'Drift bei n=' + str(pa.stable_at_n) if pa.drift_mode else ('ja bei n=' + str(pa.stable_at_n) if pa.stabilized else '⚠ nein')}",
                f"Drift: {pa.drift_per_period:.3f} Samples/Periode  Residuum={pa.drift_residual:.2f}  max_gap_n={pa.max_interp_gap_n}",
                f"Amplitude Attack/Release: {pa.amplitude_ratio:.1f}×",
                f"LUT-Punkte: {len(pa.lut_points)}  (Phase-3-Gaps: {pa.phase3_count}, dense_step={pa.dense_step_used})",
                ("LUT-Raum: [0,T) gefaltet" if getattr(pa, "lut_folded", True)
                 else f"LUT-Raum: [0,{getattr(pa,'lut_r_search_max',2*pa.T_int)}) ungefaltet")
                + f"  search_periods={getattr(pa,'lut_search_periods',2)}"
                + f"  r_max={getattr(pa,'lut_r_search_max',2*pa.T_int)}"
                + f"  Fold: {getattr(pa,'lut_fold_reason','')}",
                f"Punkte: {getattr(pa, 'lut_points_raw_count', len(pa.lut_points))} vor Pruning -> {len(pa.lut_points)} nach Pruning  (entfernt: {getattr(pa, 'lut_points_raw_count', len(pa.lut_points)) - len(pa.lut_points)})",
                f"score_med={getattr(pa, 'lut_score_median', 0.0):.3f}  low<{SCORE_BAD:.2f}={getattr(pa, 'lut_low_score_fraction', 0.0)*100:.0f}%  q_n={getattr(pa, 'lut_quality_score_count', 0)}  score_min={pa.score_min:.3f}  score_mean={pa.score_mean:.3f}",
                f"Legacy-Fallback: {'ja (' + pa.legacy_reason + ')' if pa.legacy_fallback else 'nein'}",
            ]
            info = "\n".join(info_parts)
        self._detail_info.config(text=info)

        # LUT-Tabelle
        self._lut_table.delete(*self._lut_table.get_children())
        phase_colors = {"dense":C_DENSE,"sparse":C_SPARSE,"gap":C_GAP,"drift":C_DRIFT}
        for pt in pa.lut_points:
            score_str = f"{pt.best_score:.3f}" if pt.best_score > -2 else "n/a"
            fold_str  = "✓" if pt.folded else "✗"
            ratio_str = f"{pt.fold_ratio*100:.0f}%" if pt.fold_ratio < 1.5 else "–"
            iid = self._lut_table.insert("", tk.END,
                                          values=(pt.n, pt.loop_pos, pt.best_r,
                                                  score_str, pt.phase,
                                                  pt.raw_r, fold_str, ratio_str),
                                          tags=(pt.phase,))
            self._lut_table.tag_configure(pt.phase,
                                           foreground=phase_colors.get(pt.phase, C_TEXT))

        # Plot
        if HAS_MATPLOTLIB:
            self._plot_lut(pa)

    def _plot_lut(self, pa: PipeAnalysis):
        """Zeigt LUT-Punkte und die Kreis-Interpolation, die GO später verwenden soll."""
        self._fig.clear()
        if not pa.lut_points:
            self._canvas.draw()
            return

        ax = self._fig.add_subplot(111, facecolor=C_BG2)
        self._fig.patch.set_facecolor(C_BG)
        ax.tick_params(colors=C_TEXT2)
        for spine in ax.spines.values():
            spine.set_edgecolor(C_BORDER)

        pts_sorted = sorted(pa.lut_points, key=lambda p: p.n)
        T = pa.T_int
        lut_folded = getattr(pa, "lut_folded", True)

        # Interpolationslinie — v138: analytisches Zeichnen statt Sampling.
        # Jedes LUT-Segment erzeugt exakt 1 oder 2 Plot-Teilsegmente.
        # Das loest den 1-Punkt-Segment-Bug beim engen Doppelwrap.
        interp_segments = []
        visual_phase_wrap = False

        if len(pts_sorted) >= 1:
            # v126: Plot-Gueltigkeitsbereich release-spezifisch.
            n_min_pts = min(p.n for p in pts_sorted)
            n_max_pts = max(p.n for p in pts_sorted)
            first_release = bool(getattr(pa, "is_shortest_release", False))
            longest_release = getattr(pa, "max_key_press_ms", None) is None
            valid_start_n = max(0.0, float(getattr(pa, "min_sample", 0) or 0) / max(pa.T_float, 1e-9))
            n_min = valid_start_n if first_release else float(n_min_pts)
            if first_release:
                n_min = min(n_min, float(n_min_pts))
            n_max = float(getattr(pa, "n_total", 0) or n_max_pts) if longest_release else float(n_max_pts)
            n_max = max(n_max, float(n_max_pts))
            if n_max < n_min:
                n_min, n_max = float(n_min_pts), float(n_max_pts)

            def _add_seg(x0, x1, y0, y1):
                if x1 > x0:
                    interp_segments.append(([x0, x1], [y0, y1]))

            def _add_directed(x0, x1, r0, r1, up):
                """Fuegt 1 oder 2 Teilsegmente fuer ein LUT-Segment hinzu.
                Bei Phasenuebergang (Wrap) wird gesplittet; kein Verbindungsstrich."""
                nonlocal visual_phase_wrap
                r0i = int(round(r0)) % T
                r1i = int(round(r1)) % T
                if lut_folded:
                    needs_wrap = (up and r1i < r0i) or (not up and r1i > r0i)
                    if not needs_wrap:
                        _add_seg(x0, x1, float(r0i), float(r1i))
                    else:
                        visual_phase_wrap = True
                        if up:
                            # Weg: r0 → T (oben raus), dann 0 → r1
                            span = (T - r0i) + r1i  # Gesamtdelta
                            if span <= 0:
                                _add_seg(x0, x1, float(r0i), float(r1i))
                                return
                            t_w = (T - r0i) / float(span)
                            x_w = x0 + t_w * (x1 - x0)
                            _add_seg(x0, x_w, float(r0i), float(T))
                            _add_seg(x_w, x1, 0.0, float(r1i))
                        else:
                            # Weg: r0 → 0 (unten raus), dann T → r1
                            span = r0i + (T - r1i)
                            if span <= 0:
                                _add_seg(x0, x1, float(r0i), float(r1i))
                                return
                            t_w = float(r0i) / float(span)
                            x_w = x0 + t_w * (x1 - x0)
                            _add_seg(x0, x_w, float(r0i), 0.0)
                            _add_seg(x_w, x1, float(T), float(r1i))
                else:
                    # Ungefaltet: direkter linearer Weg
                    _add_seg(x0, x1, float(r0), float(r1))

            # Lead-in: horizontal vor erstem Punkt (nur kuerzestes Release)
            if first_release and n_min < float(n_min_pts):
                p0 = pts_sorted[0]
                r0f = float(p0.best_r % T) if lut_folded else float(p0.best_r)
                _add_seg(n_min, float(n_min_pts), r0f, r0f)

            # Segmente zwischen LUT-Punkten
            for pa_pt, pb_pt in zip(pts_sorted, pts_sorted[1:]):
                _add_directed(
                    float(pa_pt.n), float(pb_pt.n),
                    float(pa_pt.best_r), float(pb_pt.best_r),
                    getattr(pb_pt, "approach_up", True))

            # Lead-out: Trend nach letztem Punkt fortsetzen (nur laengstes Release)
            if longest_release and len(pts_sorted) >= 2 and n_max > float(n_max_pts):
                plast = pts_sorted[-1]
                pprev = pts_sorted[-2]
                ta = float(getattr(pprev, "track_r", pprev.best_r))
                tb = float(getattr(plast, "track_r", plast.best_r))
                slope = (tb - ta) / max(1, plast.n - pprev.n)
                r_end_track = tb + slope * (n_max - plast.n)
                r_end = int(round(r_end_track)) % T if lut_folded else max(0, min(
                    getattr(pa, "lut_r_search_max", 2 * T) - 1, round(r_end_track)))
                up_ext = slope >= 0
                _add_directed(float(n_max_pts), n_max,
                               float(plast.best_r), float(r_end), up_ext)

        label_interp = "GO/Simulation-Interpolation"
        for seg_i, (xs_seg, ys_seg) in enumerate(interp_segments):
            ax.plot(xs_seg, ys_seg, color=C_TEXT, linewidth=1.2,
                    alpha=0.75, zorder=1,
                    label=label_interp if seg_i == 0 else None)

        # Rohpunkte nach Phase/Fold-Status getrennt zeichnen.
        phase_color = {"dense": C_DENSE, "sparse": C_SPARSE, "gap": C_GAP, "drift": C_DRIFT}
        for phase in ["dense", "sparse", "gap", "drift"]:
            pts_fold   = [p for p in pts_sorted if p.phase == phase and p.folded]
            pts_unfold = [p for p in pts_sorted if p.phase == phase and not p.folded]
            col = phase_color[phase]
            if pts_fold:
                ax.scatter([p.n for p in pts_fold], [p.best_r for p in pts_fold],
                           color=col, marker="o", s=36, zorder=3,
                           label=f"{phase} (gefaltet)")
            if pts_unfold:
                ax.scatter([p.n for p in pts_unfold], [p.best_r for p in pts_unfold],
                           color=col, marker="D", s=36, zorder=3,
                           label=f"{phase} (ungefaltet)")

        r_search_max   = getattr(pa, "lut_r_search_max",  2 * T)
        search_periods = getattr(pa, "lut_search_periods", 2)
        ax.axhline(0, color=C_BORDER, linewidth=0.5)
        for k in range(1, search_periods + 1):
            r_line = k * T
            if r_line > r_search_max + T:
                break
            style = "--" if k == 1 else ":"
            color = C_BAD if k == 1 else C_DRIFT
            alpha = 0.6 if k == 1 else 0.4
            ax.axhline(r_line, color=color, linewidth=0.7,
                        linestyle=style, alpha=alpha, label=f"{k}T={r_line}")

        if pa.stable_at_n:
            label = f"Drift n={pa.stable_at_n}" if pa.drift_mode else f"stabil n={pa.stable_at_n}"
            ax.axvline(pa.stable_at_n, color=C_ACCENT, linewidth=1,
                        linestyle=":", alpha=0.8, label=label)

        ax.set_xlabel("Periodenindex n", color=C_TEXT2)
        max_point_y = max([float(p.best_r) for p in pts_sorted] + [0.0])
        max_track_y = max([float(getattr(p, "track_r", p.best_r))
                           for p in pts_sorted] + [0.0])
        _interp_vals_for_ylim = [v for _xs, _ys in interp_segments for v in _ys if not (isinstance(v, float) and math.isnan(v))]
        max_interp_y = max(_interp_vals_for_ylim + [0.0])
        # Die y-Skalierung soll alle sichtbaren Punkte und die tatsaechliche
        # Simulations-/GO-Interpolationslinie zeigen. Falls obere T-Sheets
        # benutzt werden, darf der Plot entsprechend bis r_search_max/2T gehen.
        y_top = max(float(T), float(r_search_max), max_point_y, max_track_y, max_interp_y) + 5.0
        ax.set_ylabel(f"best_r / GO r [0,{int(round(y_top-5))}]", color=C_TEXT2)
        ax.set_ylim(-5, y_top)
        title = "LUT-Stuetzpunkte + GO/Simulation-Interpolation"
        if pa.drift_mode:
            title += f"  (Drift {pa.drift_per_period:.3f} smp/Periode)"
        ax.set_title(title, color=C_TEXT, fontsize=10)
        ax.legend(facecolor=C_BG2, edgecolor=C_BORDER,
                   labelcolor=C_TEXT, fontsize=8,
                   loc="upper left", bbox_to_anchor=(1.02, 1), borderaxespad=0)
        self._fig.tight_layout(rect=[0, 0, 0.82, 1])
        self._canvas.draw()

    def _plot_waveform(self, pa: PipeAnalysis, pt: LutPoint):
        """Zeigt Attack-Loop und Release an einem LUT-Punkt.
        Drei Kurven:
          - Attack @ loop_pos (blau):   was GO am Übergang abspielt
          - Release @ best_r (grün):    korrelationsbasierter Startpunkt
          - Release @ 0 (grau):         Release-Anfang zum Vergleich
        """
        if not HAS_MATPLOTLIB:
            return
        try:
            # Vollständige Dateien laden (nur bis zum benötigten Bereich)
            needed_atk = pa.loop_start + pt.loop_pos + 3 * pa.T_int + 10
            needed_rel = pt.best_r + 3 * pa.T_int + 10
            atk_mono, sr, _, _ = read_wav_mono_float(pa.attack_path,
                                                      max_samples=needed_atk)
            rel_mono, _,  _, _ = read_wav_mono_float(pa.release_path,
                                                      max_samples=needed_rel)
        except Exception as e:
            return

        self._fig.clear()
        ax = self._fig.add_subplot(111, facecolor=C_BG2)
        self._fig.patch.set_facecolor(C_BG)
        ax.tick_params(colors=C_TEXT2)
        for spine in ax.spines.values():
            spine.set_edgecolor(C_BORDER)

        # Fensterlänge: 2 Perioden, max 512 Samples
        win = min(2 * pa.T_int, 512)
        t   = list(range(win))

        # Attack bei loop_pos:
        # pt.loop_pos ist absolute Sample-Position in der Attack-Datei (wie in GO)
        abs_cs = pt.loop_pos
        atk_w  = atk_mono[abs_cs : abs_cs + win]

        # Release ab best_r (korrelierter Startpunkt)
        rel_corr = rel_mono[pt.best_r : pt.best_r + win]

        # Release ab 0 (unkompensiert, zum Vergleich)
        rel_raw  = rel_mono[0 : win]

        if len(atk_w) == win:
            ax.plot(t, atk_w,   color=C_DENSE,  linewidth=1.5,
                    label=f"Attack @ loop_pos={pt.loop_pos}", zorder=3)
        if len(rel_corr) == win:
            ax.plot(t, rel_corr, color=C_SPARSE, linewidth=1.5,
                    label=f"Release @ best_r={pt.best_r}  (score={pt.best_score:.3f})",
                    zorder=2)
        if len(rel_raw) == win:
            ax.plot(t, rel_raw,  color=C_BORDER, linewidth=1.0,
                    linestyle="--", label="Release @ 0 (Anfang)", zorder=1)

        # Vertikale Linie bei T (Ende der ersten Periode)
        ax.axvline(pa.T_int, color=C_TEXT2, linewidth=0.8,
                   linestyle=":", alpha=0.6, label=f"T={pa.T_int}")

        ax.set_xlabel("Sample-Offset ab Startposition", color=C_TEXT2)
        ax.set_ylabel("Amplitude (normiert)", color=C_TEXT2)
        ax.set_title(
            f"Waveform-Vergleich  —  n={pt.n}  loop_pos={pt.loop_pos}  "
            f"best_r={pt.best_r}  score={pt.best_score:.3f}",
            color=C_TEXT, fontsize=10)
        ax.legend(facecolor=C_BG2, edgecolor=C_BORDER,
                   labelcolor=C_TEXT, fontsize=8)
        self._canvas.draw()

    # ── Report ─────────────────────────────────────────────────────────────────

    def _build_report(self):
        self._report_table.delete(*self._report_table.get_children())
        pas = [v for v in self._analyses.values() if isinstance(v, PipeAnalysis)]

        def sort_key(p):
            # 1. Fehler zuerst
            # 2. Rot (Fehler in LUT)
            # 3. Gelb (Warn in LUT)
            # 4. Grün (OK in LUT)
            # 5. Legacy instabil
            # 6. Legacy Drift
            # 7. Legacy (T<16)
            tag = p.severity_tag
            order = {
                "sev_error":       0,
                "sev_bad":         1,
                "sev_warn":        2,
                "sev_ok":          3,
                "sev_legacy_warn": 4,
                "sev_legacy_badlut": 5,
                "sev_drift":       6,
                "sev_legacy":      7,
            }.get(tag, 9)
            return (order, p.rank_name, p.midi_note)

        pas.sort(key=sort_key)

        for pa in pas:
            key = f"{pa.rank_name}|{pa.midi_note}|{pa.perspective}|{pa.release_type}"
            sr_ = pa.sample_rate or 48000
            self._report_table.insert("", tk.END,
                values=(
                    pa.severity_label,
                    pa.rank_name,
                    midi_to_name(pa.midi_note),
                    pa.perspective,
                    pa.release_type,
                    f"{pa.freq_hz:.1f}"    if pa.freq_hz         else "-",
                    str(pa.T_int)           if pa.T_int           else "-",
                    str(pa.stable_at_n)     if pa.stabilized      else "nein",
                    str(pa.transient_end_n) if pa.transient_end_n else "-",
                    f"{pa.n_dense}+{pa.n_sparse}+{pa.n_gap}",
                    f"{pa.score_mean:.3f}"  if pa.score_mean      else "-",
                    f"{pa.score_min:.3f}"   if pa.score_min       else "-",
                    f"{pa.score_max:.3f}"   if pa.score_max       else "-",
                    f"{pa.bestr_coherence:.1f}"   if pa.bestr_coherence       else "-",
                    f"{pa.amplitude_ratio:.1f}x" if pa.amplitude_ratio else "-",
                    str(pa.phase3_count),
                    f"{pa.atk_frames*1000//sr_}" if pa.atk_frames else "-",
                    f"{pa.rel_frames*1000//sr_}" if pa.rel_frames else "-",
                    pa.error or ""
                ),
                tags=(pa.severity_tag, key))

    def _save_report(self):
        path = filedialog.asksaveasfilename(
            defaultextension=".html",
            filetypes=[("HTML","*.html"),("CSV","*.csv"),("Alle","*.*")])
        if not path:
            return

        pas = [v for v in self._analyses.values() if isinstance(v, PipeAnalysis)]
        pas.sort(key=lambda p: (-p.severity, p.rank_name, p.midi_note))

        if path.endswith(".csv"):
            import csv
            with open(path, "w", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                w.writerow(["Severity","Register","MIDI","Note","Perspektive","Release",
                             "Freq_Hz","T_int","T_float",
                             "Stabil_n","Transient_Ende_n","N_Dense","N_Sparse","N_Gap",
                             "Score_Mean","Score_Min","Score_Max","BestR_Std","BestR_Mean",
                             "Amp_Ratio","Atk_ms","Rel_ms","Loop_Start","Loop_End",
                             "HarmonicNumber","Fehler"])
                for pa in pas:
                    sr_ = pa.sample_rate or 48000
                    w.writerow([
                        pa.severity, pa.rank_name, pa.midi_note,
                        midi_to_name(pa.midi_note), pa.perspective, pa.release_type,
                        f"{pa.freq_hz:.3f}", pa.T_int, f"{pa.T_float:.4f}",
                        pa.stable_at_n if pa.stabilized else "nein",
                        pa.transient_end_n, pa.n_dense, pa.n_sparse, pa.n_gap,
                        f"{pa.score_mean:.4f}", f"{pa.score_min:.4f}", f"{pa.score_max:.4f}",
                        f"{pa.bestr_coherence:.2f}", f"{pa.bestr_mean:.2f}",
                        f"{pa.amplitude_ratio:.3f}",
                        pa.atk_frames * 1000 // sr_, pa.rel_frames * 1000 // sr_,
                        pa.loop_start, pa.loop_end,
                        pa.harmonic_number, pa.error or ""
                    ])
        else:
            # HTML
            rows = ""
            for pa in pas:
                scores = [p.best_score for p in pa.lut_points if p.phase in ("sparse","gap")]
                bg = ["#e8f5e8","#fff8e0","#fde8e8"][pa.severity]
                sr_ = pa.sample_rate or 48000
                rows += f"""<tr style="background:{bg}">
                  <td>{pa.severity_label}</td>
                  <td>{pa.rank_name}</td>
                  <td>{midi_to_name(pa.midi_note)}</td>
                  <td>{pa.perspective}</td>
                  <td>{pa.release_type}</td>
                  <td>{pa.freq_hz:.1f}</td>
                  <td>{pa.T_int}</td>
                  <td>{"n="+str(pa.stable_at_n) if pa.stabilized else "<b>nein</b>"}</td>
                  <td>{pa.transient_end_n}</td>
                  <td>{pa.n_dense}+{pa.n_sparse}+{pa.n_gap}</td>
                  <td>{pa.score_mean:.3f}</td>
                  <td>{pa.score_min:.3f}</td>
                  <td>{pa.score_max:.3f}</td>
                  <td>{pa.bestr_coherence:.1f}</td>
                  <td>{pa.amplitude_ratio:.1f}x</td>
                  <td>{pa.phase3_count}</td>
                  <td>{pa.atk_frames*1000//sr_}</td>
                  <td>{pa.rel_frames*1000//sr_}</td>
                  <td>{pa.error or ""}</td>
                </tr>\n"""

            html = f"""<!DOCTYPE html><html><head><meta charset="utf-8">
<title>GrandOrgue LUT Report</title>
<style>
  body {{ font-family: monospace; font-size:13px; background:#f8f8f0; margin:20px; }}
  h1 {{ color:#333; }}
  table {{ border-collapse:collapse; width:100%; }}
  th {{ background:#333; color:#fff; padding:6px 10px; text-align:left; }}
  td {{ padding:5px 10px; border-bottom:1px solid #ddd; }}
</style></head><body>
<h1>GrandOrgue LUT Analyzer — Report</h1>
<p>Organ: {os.path.basename(self._organ_path)}</p>
<table>
<tr><th></th><th>Register</th><th>Note</th><th>Perspektive</th><th>Release</th>
<th>Freq Hz</th><th>T</th><th>Stabil n</th><th>Trans.Ende n</th>
<th>LUT-Pts<br>(d+s+g)</th><th>Score Mean</th><th>Score Min</th><th>Score Max</th>
<th>R Streuung</th><th>Amp-Ratio</th><th>Gaps</th>
<th>Atk ms</th><th>Rel ms</th><th>Fehler</th></tr>
{rows}
</table></body></html>"""

            with open(path, "w", encoding="utf-8") as f:
                f.write(html)

        messagebox.showinfo("Gespeichert", f"Report gespeichert:\n{path}")


# ─── Einstiegspunkt ───────────────────────────────────────────────────────────

def export_csv_batch(organ_path: str, output_path: str = None):
    """
    Batch mode for Phase 8 GO comparison.
    Analyses all releases of an organ file and exports LUT points as CSV:
      release_idx, point_idx, loop_pos, best_r
    Releases with legacy_fallback (drift, instability, bad quality) are omitted —
    matching GO's behaviour (releaseMap[i] = -1 for those).

    Compare with GO output:
      python3 analyze_lut_v57.py organ.organ --export-csv py.csv
      python3 read_golut.py organ.release-align.golut --csv > go.csv
      diff py.csv go.csv
    """
    import concurrent.futures

    print(f"Parsing {organ_path} …", file=sys.stderr)
    pipes = parse_organ_file(organ_path)
    print(f"  {len(pipes)} release descriptors found", file=sys.stderr)

    print("Analysing …", file=sys.stderr)
    max_workers = max(1, (os.cpu_count() or 2) - 1)
    with concurrent.futures.ProcessPoolExecutor(max_workers=max_workers) as ex:
        results = list(ex.map(analyze_pipe, pipes))

    out = open(output_path, 'w', newline='') if output_path else sys.stdout
    try:
        out.write("release_idx,point_idx,loop_pos,best_r\n")
        n_cached = 0
        for release_idx, pa in enumerate(results):
            # Skip releases that GO would mark as no-LUT
            if pa.legacy_fallback or not pa.lut_points:
                continue
            n_cached += 1
            for pt_idx, pt in enumerate(pa.lut_points):
                out.write(f"{release_idx},{pt_idx},{pt.loop_pos},{pt.best_r}\n")
    finally:
        if output_path:
            out.close()

    print(
        f"Exported {n_cached} cached / {len(results)} total releases.",
        file=sys.stderr)
    if output_path:
        print(f"Written to {output_path}", file=sys.stderr)



def test_wav_period(wav_path: str, harmonic_number: int = 8):
    """CLI-Selbsttest fuer die Periodenerkennung an einer einzelnen WAV.

    Nutzung:
      python analyze_lut_v77_unambiguous_period_guard.py --test-wav 082-A#.wav
    """
    smpl = parse_smpl_chunk(wav_path)
    if smpl["midi_note"] is None:
        print(f"{TOOL_VERSION}: Kein smpl-Chunk: {wav_path}")
        return 2
    atk_mono, sr, atk_frames, atk_ch = read_wav_mono_float(wav_path)
    midi_note  = smpl["midi_note"]
    pitch_frac = smpl["pitch_frac"] / 2**32
    freq_hz    = 440.0 * 2**((midi_note + pitch_frac - 69) / 12)
    smpl_T     = sr / freq_hz
    hn_T       = smpl_T * harmonic_number / 8.0
    T_smpl_i   = int(round(smpl_T))
    T_hn_i     = int(round(hn_T))
    loops = smpl["loops"]
    if loops:
        loop_start, loop_end = loops[0]
    else:
        loop_start, loop_end = 0, len(atk_mono) - 1
    # Suchbereich identisch zu analyze_pipe: [0.5*T_hn, 2.0*T_hn]
    min_p = max(16, int(round(0.5 * T_hn_i)))
    max_p = min(sr // 20, int(round(2.0 * T_hn_i)))
    loop_len = loop_end - loop_start + 1
    loop_mid = loop_start + loop_len // 2
    autocorr_region = atk_mono[loop_mid:loop_mid + max_p * 8]
    if len(autocorr_region) < max_p * 2:
        print(f"FEHLER: autocorr_region zu kurz ({len(autocorr_region)} < {max_p * 2}), WAV zu kurz oder loop_mid zu gross.")
        return 1
    t_est, diag = estimate_period_by_autocorr(autocorr_region, min_p, max_p, expected_period=hn_T)
    print(f"TOOL_VERSION={TOOL_VERSION}")
    print(f"wav={wav_path}")
    print(f"sr={sr} frames={atk_frames} channels={atk_ch}")
    print(f"smpl_midi={midi_note} pitch_frac={smpl['pitch_frac']} freq_hz={freq_hz:.6f}")
    print(f"smpl_T={smpl_T:.6f} T_smpl_int={T_smpl_i}")
    print(f"hn_T={hn_T:.6f} T_hn_int={T_hn_i}  (harmonic_number={harmonic_number})")
    print(f"search=[{min_p},{max_p}]")
    print(f"loop={loop_start}-{loop_end} autocorr_region_len={len(autocorr_region)}")
    print(f"T_est={t_est:.6f} T_int={int(round(t_est))}")
    print(f"CMNDF_half={diag['cmndf_half']:.6g} CMNDF_T={diag['cmndf_T']:.6g} CMNDF_2T={diag['cmndf_2T']:.6g}")
    return 0

def main():
    if len(sys.argv) >= 3 and sys.argv[1] == '--test-wav':
        if len(sys.argv) <= 3:
            print("HINWEIS: Kein harmonic_number angegeben, verwende 8 (Oktavregister-Pfad).", file=sys.stderr)
        hn = int(sys.argv[3]) if len(sys.argv) > 3 else 8
        raise SystemExit(test_wav_period(sys.argv[2], hn))

    # CLI batch mode: analyze_lut_v57.py <organ> --export-csv [output.csv]
    if len(sys.argv) >= 3 and sys.argv[2] == '--export-csv':
        out = sys.argv[3] if len(sys.argv) > 3 else None
        export_csv_batch(sys.argv[1], out)
        return

    initial = sys.argv[1] if len(sys.argv) > 1 else None
    app = LUTAnalyzerApp(initial_organ=initial)
    app.mainloop()


if __name__ == "__main__":
    # Nötig für ProcessPoolExecutor unter Windows (spawn-Methode)
    import multiprocessing
    multiprocessing.freeze_support()
    main()
