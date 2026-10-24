# Warum der numpy-Prototyp und die Bibliothek auseinanderlaufen

> **GELOEST, siehe Abschnitt 8.**  Abschnitt 7 war nah dran, aber
> ebenfalls noch nicht die Ursache.  Die Abschnitte 1 bis 6
> sind der Weg dorthin und bleiben als Ausschlussliste stehen - sie
> beantworten die falsche Frage (warum ist der MITTELWERT schlechter),
> und genau das war das Problem.

Stand 06.08.2026.  Alle Zahlen: mittlerer NMR-Ueberschuss in dB, kleiner
ist besser, > 0 dB ist hoerbar.  Material `2026-08-04-08-52-07.473.wav`
(17,69 s, D-Dur-Akkord), Luecken 128 Samples, Fade 3 ms, random spacing,
Seed 0xC0FFEE, Metrik auf <= 150 Luecken gestichprobt.

Verglichen werden `Versuch: Prony AR(4), 4k, hop 512, span 20` (numpy)
und `GO-1knob: frame 4k, PRONY order 4, ANKER, kein Trend` (Bibliothek).


## 1. Der Unterschied ist ZWEI Unterschiede, und einer wechselt das Vorzeichen

|                            | 600/min (2,5 %) | 6000/min (23,7 %) |
|----------------------------|----------------:|------------------:|
| numpy AR(4), pro Luecke    |          -10,91 |             +2,47 |
| Bibliothek, pro Luecke     |          -12,22 |             +4,48 |
| Bibliothek, verkettet      |           -8,97 |             +6,94 |
| **Aufbau-Anteil**          |       **-3,25** |         **-2,46** |
| **Algorithmus-Anteil**     |       **+1,31** |         **-2,01** |

Der **Aufbau-Anteil** ist der Abstand zwischen verkettetem und
Pro-Luecke-Betrieb bei IDENTISCHER Bibliothek.  Er ist bei beiden
Dichten etwa gleich gross und geht immer zugunsten des Pro-Luecke-Pfads.

Der **Algorithmus-Anteil** ist der Rest.  Bei 2,5 % Verdeckung ist die
Bibliothek dem Prototyp um 1,31 dB UEBERLEGEN; erst bei 23,7 % dreht es
sich auf 2,01 dB Rueckstand.  Es gibt also keinen generellen
Verfahrensnachteil - er entsteht erst mit der Dichte.

Zur Einordnung: bei 6000/min ist ALLES hoerbar, auch der beste Wert
(+2,47).  Der Bereich, in dem die Verdeckung wirklich traegt, ist
600/min mit -9 bis -12 dB.  6000/min ist ein Belastungstest, kein
Betriebspunkt - 23,7 % aller Perioden fehlen.


## 2. Was gemessen und ausgeschlossen ist

| Verdaechtiger | Ergebnis | Beleg |
|---|---|---|
| Safety-Cap zu aggressiv | **0,0 dB**; Ausloesequote flach 14,1/14,9/14,6/14,3 % bei Ordnung 4/8/12/16, also NICHT ordnungsabhaengig | Commit 202c5cbfc |
| Durand-Kerner Warm Start | ausgeschlossen: Schur haelt keine persistenten Wurzeln, hat keinen Warm Start, und bricht bei hoher Ordnung im Gleichschritt ein | Commit 7b79bd487 |
| Verschmutzte Fit-Updates (Akkumulatoren) | Gaten macht es **0,9 bis 3,1 dB SCHLECHTER**, Ueberspringquote ~60 % | Commit 9ae779710 |
| Pegel-Trend bei Ordnung >= 2 | 0,01 bis 0,13 dB; Vorzeichenwechsel bei Ordnung 2 wie theoretisch verlangt, Betrag im Rauschen | Messung 06.08. |
| Schur statt Wurzelkappung | verliert an jedem rho, auch gegen das Nichtstun; rho = 1,0 war die Singularitaet (179,88 Grad Winkelfehler) | Commit 7b79bd487 |
| `freqDamping` | +-0,5 dB ueber 0 bis 0,95, ohne Richtung | Messung 06.08. |
| Analyse aufschieben (`staleWindow`) | bis **7 dB SCHLECHTER** - Drift ist teurer als Verschmutzung | Messung 06.08. |
| Compiler-Flags | bis Ordnung 12 deckungsgleich; ab 16 entscheidet die Rundungsreihenfolge (12 dB bei Ordnung 20) | Commit 59855245e |
| Lab schummelt (echtes Signal in der Historie) | **nein** - `hist = x[:pos]` aus dem mitwachsenden Array; die einzige Stelle mit Zugriff auf das Original ist `phase-align re-entry (warp)`, und der Haken ist aus | gap_lab.py:1352, 1370 |


## 3. Was positiv feststeht

Ein Orakel, das dem Ring nachtraeglich die WAHRHEIT unterschiebt,
waehrend die gespielte Ausgabe die Vorhersage bleibt:

| | Ring = eigene Vorhersage | Ring = Wahrheit | Differenz |
|---|---:|---:|---:|
| 600/min, Ordnung 4 | -8,97 | -12,53 | **3,56** |
| 600/min, Ordnung 12 | -5,45 | -13,34 | **7,90** |
| 6000/min, Ordnung 4 | +6,94 | -3,31 | **10,24** |
| 6000/min, Ordnung 12 | +6,58 | -4,29 | **10,86** |

Daraus folgt zweierlei.

**Die Selbstfuetterung ueber den Analyse-Ring ist der groesste Posten
im ganzen System** - groesser als Aufbau, Algorithmus, Cap und Trend
zusammen.

**Der Hochordnungs-Einbruch ist NICHTS ANDERES als diese Rueckkopplung.**
Mit sauberem Ring liegt Ordnung 12 vor Ordnung 4, bei beiden Dichten.
Mit verschmutztem dahinter.  Hoehere Ordnung ist nicht defekt, sie ist
empfindlicher - mehr Freiheitsgrade heisst mehr Moeglichkeiten, sich an
die eigene Ausgabe statt an das Signal anzupassen.

Und die Verschmutzung ist **korreliert, nicht zufaellig**: was in den
Ring geht, ist die Ausgabe genau des Modells, das gleich wieder
analysiert.  Die Analyse bestaetigt die eigenen Fehler, statt sie
auszumitteln.  Deshalb kosten schon 2,5 % Verdeckung 3,6 dB - der
Schaden steht in keinem Verhaeltnis zum Anteil im Fenster.


## 4. Was NICHT erklaert ist

**Der Aufbau-Anteil von 2,46 dB bei 23,7 %.**  Bei 600/min gibt es eine
Erklaerung: die Luecken liegen im Mittel 39 Perioden auseinander, das
Analysefenster ist 32 Perioden lang, also sind die Fenster des
Pro-Luecke-Pfads meist sauber, waehrend der verkettete Pfad den Schatten
JEDE Periode neu baut, auch direkt nach einer Luecke, wenn das Fenster am
schmutzigsten ist.  Bei 6000/min ist das Fenster fuer beide schmutzig -
und der Abstand bleibt trotzdem.  Die Erklaerung ist also unvollstaendig.

**Der Algorithmus-Anteil von 2,01 dB, der erst bei hoher Dichte
auftritt.**  Drei benannte Unterschiede kommen in Frage: rechteckiges
Fitfenster ueber 20 Hops gegen exponentielles Vergessen mit Gedaechtnis
22; kaltes `eigvals` gegen Warm Start; und `AR_SOLVE_EVERY = 4`, also
Wurzeln, die bis zu vier Gitterpunkte alt sind, gegen frisches Loesen
bei jeder Luecke.


## 5. Strategie - drei Experimente, dann ist Schluss

Nicht weiter sweepen.  Drei kontrollierte Versuche, jeder schliesst
einen grossen Ast aus, mit Abbruchregel.

**A. Aufbau-Anteil, ein Freiheitsgrad.**  `engine_run` so aendern, dass
`predict()` nur an denselben Stellen laeuft wie im Pro-Luecke-Pfad (bzw.
umgekehrt).  Damit unterscheiden sich die Pfade nur noch in der
Haeufigkeit des Schattenbaus.  Ein Lauf, zwei Zahlen.  Erklaert das die
2,46 dB, ist der Aufbau-Anteil verstanden und der Vergleich endlich
fair.

**B. Algorithmus-Anteil, ein Freiheitsgrad.**  `AR_SOLVE_EVERY` als
Knopf, 1 gegen 4.  Das ist der einzige der drei genannten Unterschiede,
der mit der Dichte skaliert: bei vielen Luecken sind veraltete Wurzeln
oefter veraltet.  Ein Lauf.

**C. Wenn A und B zusammen unter 1 dB erklaeren: direkt vergleichen.**
Die per-Bin-Koeffizienten beider Verfahren zum selben Zeitpunkt
ausgeben und gegeneinanderlegen.  Das beendet das Raten, weil es die
gefitteten MODELLE vergleicht statt ihrer Ausgaben.  Der Haken
`XO_DUMP_BIN` existiert bereits.

**Abbruchregel:** nach C wird nicht weiter nach der Ursache gesucht.
Bleibt dann noch ein Rest, ist er kleiner als die 3,6 bis 10,9 dB der
Rueckkopplung und damit nicht die lohnende Baustelle.


## 6. Die eigentliche Baustelle

Die Rueckkopplung kostet 3,6 bis 10,9 dB und ist **nicht durch Wegsehen
behebbar** - drei Versuche in diese Richtung (Fit gaten, Analyse
aufschieben, Frequenz daempfen) haben es alle schlechter gemacht.
Zweimal derselbe Befund: veraltete oder fehlende Daten sind schlimmer
als verschmutzte.

Wenn die Verschmutzung korreliert ist, waere der Hebel nicht Vermeiden
sondern **Dekorrelieren**: dafuer sorgen, dass aufeinanderfolgende
Analysen den eigenen Output nicht in derselben Phasenlage wiedersehen.
Das ist bisher eine These ohne Messung und steht bewusst NACH A/B/C.


## 7. Die Aufloesung: vergiftete Startwerte der Rekursion

Der Mittelwert war die falsche Kennzahl.  Ueber ALLE Luecken gemessen
(nicht die 150er-Stichprobe der GUI), Fehlerspitze je Luecke, 600/min:

| | Median | p90 | max | >0,05 | >0,10 | >0,20 |
|---|---:|---:|---:|---:|---:|---:|
| numpy AR(4) | 0,0172 | 0,0554 | 0,1437 | 11,0 % | 3,1 % | **0,0 %** |
| Bibliothek order 4 | 0,0165 | 0,0640 | **0,3291** | 16,8 % | 3,1 % | **1,6 %** |

Im Normalfall ist die Bibliothek gleichauf, im Median sogar besser.  Der
Unterschied sind SELTENE GROBE AUSREISSER - 1,6 % gegen 0,0 %.  Bei 600
Luecken je Minute sind das rund zehn hoerbare Knacke pro Minute, und
genau die sieht man als vertikale Striche im Spektrogramm.  Ein Mittel
ueber 191 Luecken verduennt drei Ausreisser bis zur Unsichtbarkeit; die
Ohren des Nutzers waren durchgehend das genauere Instrument.

**Die Ausreisser sitzen ausschliesslich in Lueckenclustern** (Hinweis des
Nutzers).  Fehlerspitze nach Abstand zur vorigen Luecke, 600/min:

| Abstand | numpy | Bibliothek |
|---|---:|---:|
| 1-2 Perioden | 0,0174 | 0,0183 |
| 3-4 Perioden | 0,0224 | **0,0467** |
| 5-16 Perioden | 0,0269 | **0,0402** |
| > 16 (isoliert) | 0,0243 | **0,0225** |

Bei ISOLIERTEN Luecken ist die Bibliothek gleichauf oder besser.  Der
gesamte Unterschied entsteht, wenn eine Luecke einer anderen folgt - und
deshalb wuchs der Abstand von 2 auf 4,5 dB beim Wechsel von 600 auf
6000/min: nicht das Verfahren wird schlechter, der Clusteranteil steigt
von 40 auf 99 %.

### Der Ort

Die Rekursion braucht P Startwerte.  Sie werden aus dem SPEKTRENRING
gelesen, an den Stellen `n - k*arStride`, k = 1..P, mit arStride = 4
Perioden bei 4k.  Bei Ordnung 4 also 4, 8, 12 und 16 Perioden zurueck -
deckungsgleich mit dem gemessenen Schadensbereich.  Eine Luecke in
diesem Abstand trifft punktgenau eine dieser Stuetzstellen.

Vorhersage daraus: der Schadensbereich muss mit der Ordnung wachsen.
Fehlerspitze relativ zu isolierten Luecken (1,00 = kein Schaden):

| Ordnung | 1-2 | 3-8 | 9-16 | 17-32 | 33-48 | >48 | erwartet |
|---|---:|---:|---:|---:|---:|---:|---:|
| 2 | 0,73 | 1,48 | 1,28 | 1,02 | 0,91 | 1,00 | 8 |
| 4 | 1,00 | 2,35 | 2,06 | 1,45 | 1,12 | 1,00 | 16 |
| 12 | 1,78 | 3,19 | 2,45 | 1,82 | 2,70 | 1,00 | 48 |

Ordnung 2 ist ab Periode 17 sauber, Ordnung 4 ab 33, Ordnung 12 bei
33-48 noch bei 2,70.  Bestaetigt.

**Der Hochordnungs-Einbruch IST dieser Effekt.**  Hohe Ordnung ist nicht
instabil, schlecht konditioniert oder ueberangepasst - sie greift weiter
zurueck und sammelt mehr vergiftete Stuetzstellen ein.  Alle Kandidaten
aus Abschnitt 2 liegen HINTER dieser Stelle und konnten es deshalb nie
sein.

### Was daraus fuer eine Reparatur folgt

Nicht ueberspringen - das ist zweimal gescheitert (Abschnitt 2).  Die
Stuetzstellen sind unverzichtbar, es sind genau P Stueck.

Der naheliegende Weg: den ganzen Satz um m Perioden nach HINTEN
verschieben, bis alle P Stuetzstellen in sauberem Material liegen, und
die Rekursion dafuer m Schritte weiter vorlaufen lassen.  Das verwirft
keine Daten, es verschiebt nur den Ansatzpunkt - und Vorlaufen ist
genau das, wofuer ein Extrapolator gebaut ist.  Nicht zu verwechseln mit
`staleWindow`: dort wurde der fertige SCHATTEN behalten und eine immer
spaetere Scheibe daraus gelesen, die das inzwischen gespielte echte
Audio nicht kennt (bis 7 dB schlechter).  Hier wird der Schatten JETZT
gebaut, mit korrekter Naht auf die aktuelle Historie, nur die
Startwerte der Rekursion kommen aus einem aelteren sauberen Abschnitt.

**Gemessen wird die Reparatur an der Ausreisserquote (>0,20), nicht am
Mittelwert.**  Der hat heute einen ganzen Tag lang in die Irre gefuehrt.


## 8. Die wirkliche Ursache: der AR-Pfad lief gar nicht

Der Spektrenring wird NUR beim Schattenbau beschrieben.  Waehrend einer
Lueckenkette wird kein Schatten gebaut (das ist die Optimierung, die
verhindert, dass die Bibliothek ihren eigenen Output analysiert) - also
reisst dort ein LOCH.  Und die Freigabe des AR-Pfads verlangt, dass
jede Stuetzstelle im Ring steht:

    useAr = ...;
    for (k = 0; k < P && useAr; k++)
        if (m_ArRingPeriod[...] != want) useAr = false;

Fehlt eine einzige, faellt der GESAMTE AR-Pfad aus und die Luecke laeuft
mit dem Ordnung-1-Rotator.  Nicht graduell - komplett.  Gemessen:

| Rate | Ordnung | Abstuerze auf Ordnung 1 |
|---|---|---:|
| 600 | 2 | 5,7 % |
| 600 | 4 | 11,1 % |
| 600 | 12 | 30,6 % |
| 6000 | 2 | 46,0 % |
| 6000 | 4 | 71,2 % |
| 6000 | 12 | **97,5 %** |

Bei 6000/min und Ordnung 12 lief die Bibliothek in 97,5 % aller
Schattenbauten als Ordnung 1.  Die Einstellung "Ordnung 12" rechnete
dort praktisch nie Ordnung 12 - und das erklaert auch die Beobachtung
des Nutzers, dass die Ordnungen IN GO kaum unterscheidbar klangen.

Damit ist auch klar, warum jeder Verdaechtige aus Abschnitt 2 unschuldig
war: Cap, Fit-Verschmutzung, Warm Start, Schur, Wurzelkappung und
Koeffizientenalter sitzen alle INNERHALB des AR-Pfads, der in diesen
Momenten nicht ausgefuehrt wurde.

### Der Fix

Die Samples fehlen nicht - `Conceal()` schreibt die gespielte Vorhersage
in den Historienring, die verdeckten Perioden sind vollstaendig da.  Nur
die FFT wurde nie genommen.  Also nachrechnen, genau wie der
Offline-Prototyp, der keinen Cache haelt und deshalb kein Loch bilden
kann.  Laeuft nur NACH einem Cluster, nie waehrend eines.

| Rate | Ordn | Backfill | Abstuerze | >0,20 | max | NMR |
|---|---|---|---:|---:|---:|---:|
| 600 | 4 | aus | 11,1 % | 1,6 % | 0,3291 | -8,55 |
| 600 | 4 | **AN** | 0,0 % | 0,0 % | 0,1516 | **-12,36** |
| 600 | 12 | aus | 30,6 % | 2,6 % | 0,3683 | -4,03 |
| 600 | 12 | **AN** | 0,0 % | 0,0 % | 0,1570 | **-12,27** |
| 6000 | 4 | aus | 71,2 % | 13,3 % | 0,5507 | +5,28 |
| 6000 | 4 | **AN** | 0,0 % | 0,1 % | 0,2216 | **-1,05** |
| 6000 | 12 | aus | 97,5 % | 20,3 % | 0,5752 | +5,40 |
| 6000 | 12 | **AN** | 0,0 % | 0,5 % | 0,2847 | **-1,13** |

Der numpy-Prototyp liegt bei -10,52 (600/min) und +2,47 (6000/min).  Die
Bibliothek schlaegt ihn jetzt um 1,8 bzw. 3,5 dB.  Ordnung 12 gewinnt
8,2 dB und liegt gleichauf mit Ordnung 4 statt acht dB dahinter - der
gesamte "Hochordnungs-Einbruch" war diese Abschaltung.

Und es ist dieselbe Lehre wie beim Fit-Gate und bei `staleWindow`, nur
in ihrer extremsten Form: VERSCHMUTZTE DATEN SCHLAGEN FEHLENDE DATEN.
Das ganze Modell wegzuwerfen, weil eine Stuetzstelle unbequem ist, war
der teuerste Weg, verschmutzte Daten zu vermeiden.
