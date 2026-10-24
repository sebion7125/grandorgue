# Xorcist — Übergabe an VM-Claude

Stand nach der Mathematik-Sitzung. Alle Zahlen unten stammen aus einem
numpy-Harness ohne Anker-Korrektor, ohne Naht-Router und ohne euren
Safety-Cap in seiner Engine-Form; **die Relationen sind belastbar, die
Absolutwerte nicht mit Engine-Zahlen vergleichbar.** Material: die D-Dur-Datei
(2026-08-04-08-52-07), die Benchmark-Rampe (2026-08-04-23-07-01) und der
Vierne.

---

## 0 Korrekturen an früheren Aussagen aus diesem Chat

Damit nichts Falsches weitergetragen wird:

- **„dB korreliert schlecht mit dem Höreindruck" war eine Überverallgemeinerung.**
  Der Hörtest hat Ordnung 4 klar von Ordnung 2 unterschieden. Die Diskrepanz
  lag an Naht-Router und Pegel-Trend auf Ordnung-1-Werten, nicht an der Metrik.
- **Die gelockerte Wurzelkappung $|z| \le 1{,}15$ ist gemessen schlechter.**
  An Toneinsätzen $-3{,}6$ statt $-4{,}3$ dB, Nahtenergie steigt. Nicht
  umsetzen.
- **Mein erster Onset-Befund („kein Sonderfall") war methodisch falsch** —
  die Einsätze lagen in dichtem Klang und die Metrik normierte auf die
  Gesamtenergie. Korrigiert siehe §4.

---

## 1 Sofort umsetzbar, hoher Ertrag

### 1.1 Ordnung an die Dichte koppeln

Die optimale Ordnung wandert monoton mit der Polyphonie. Gemessen auf der
Benchmark-Rampe, 600 Lücken/min, hop 512, Spanne 40, Ridge 0:

| Spektralgipfel < 2 kHz | beste Ordnung | Gewinn gegen Ordnung 1 |
|---|---|---|
| 10 | 3 | +2,3 dB |
| 23 | 6 | +7,9 |
| 50 | 6 | +12,5 |
| 58 | 12 | +13,2 |
| 70 | 16 | +12,9 |

Grob **eine Ordnung je vier bis fünf zusätzliche Gipfel**. Wichtig: Bei
dünnem Material ist eine zu hohe Ordnung *schädlich* — bei 10 Gipfeln liegt
Ordnung 8 bei $-20{,}6$ dB gegen $-21{,}6$ für Ordnung 3.

**Aufgabe.** Den 21-ms-Festbasis-Dichtewert des Naht-Routers gegen die
gemessene beste Ordnung auftragen, fein über die ganze Rampe. Ergebnis ist
eine Kalibrierkurve statt geratener Schwellen. Auf der GPU kostet der Sweep
praktisch nichts.

*Vorsicht:* Ordnungswechsel entwertet Spektrum-Cache und Rotator-Keep. Beim
Fenstergrößen-Umschalten hatte das den ganzen Adaptionsgewinn gefressen.
Erst prüfen, ob das hier auch gilt — falls ja, Umschaltung nur an
Streak-Grenzen oder mit Hysterese.

### 1.2 Trend und Naht nach Ordnung und Dichte staffeln

Der Pegel-Trend ist bei Ordnung 1 eine Krücke, ab Ordnung 2 eine
Doppelzählung — **aber nur bei sauberer Historie**. Bei 27 % Selbstfütterung
hat der Fit die Lautstärkeentwicklung verloren und der Trend ist bei *jeder*
Ordnung 36–39 dB wert (Schweller-Testsignal).

**Aufgabe.** Trend-Stärke als Funktion von (Ordnung, Verdeckungsdichte)
vermessen statt schalten. Die Dichte ist über `m_RecentPredMask` bereits
verfügbar — der Anteil vorhergesagter Perioden im Fit-Fenster ist genau die
gesuchte Größe.

---

## 2 Numerik: Durand-Kerner ersetzen

Die Wurzelsuche ist die einzige Iteration und die einzige fp32-fragile Stufe
(gemessen: 174,6° Winkelabweichung, während Gauß und Speicherung bei 0,0000°
liegen). Sie lässt sich ersatzlos streichen.

**Schur-Cohn-Kappung**, Herleitung in §6.4 des Papers:

```
1  alpha -> k      Abstieg, O(p^2), endlich, nur Grundrechenarten
2  fuer jedes l:   falls |k[l]| >= 1:  k[l] <- k[l] * (1-delta)/|k[l]|
3  k -> alpha      Aufstieg, O(p^2), endlich
```

Das Ergebnis ist **per Konstruktion** stabil, nicht näherungsweise: Jeder
Aufstiegsschritt erhält die Kreisscheiben-Eigenschaft (Paper §6.4.6), und der
Aufstieg ist die exakte Umkehrung des Abstiegs (numerisch auf 1e-16 geprüft).

Zwei Fallstricke:

- Der Nenner $1-|k_\ell|^2$ wird bei stationären Tönen sehr klein
  ($|k| \to 1$ ist dort der Normalfall). Untergrenze nötig.
- Die Methode kappt **alle** Moden, die an einem verletzten $k_\ell$ hängen,
  nicht nur die instabilen. Ob sie mit der selektiven Wurzelkappung mithält,
  ist **nicht gemessen** — das ist die erste Messung dazu.

Vergleichsbasis aus meinem Harness (Ordnung 6, D-Dur-Akkord / Bench 23 / Bench 53):

| Variante | Akkord | Bench 23 | Bench 53 |
|---|---|---|---|
| selektive Wurzelkappung | −25,2 | −23,9 | −21,0 |
| uniformes Schrumpfen | −24,2 | −23,1 | −20,7 |
| gar keine Kappung | −25,4 | −20,6 | −20,9 |

Uniformes Schrumpfen kostet 0,3 bis 1,0 dB. Wenn die $k$-Kappung in dieser
Spanne landet, ist der Tausch klar.

*Zur Häufigkeit:* Die Kappung ist kein Notfall. Auf dem D-Dur-Akkord liegen
bei Ordnung 6 rund **81 % der Bin-Energie** in Bins mit $\max|z| > 1$ — weil
stationäre Töne Nullstellen *auf* dem Kreis haben und jede Schätzstörung sie
mit gleicher Wahrscheinlichkeit nach außen wie nach innen schiebt.

---

## 3 GPU

Die Arbeit ist klein: Ordnung 6, acht Kanäle, 2049 Bins ergeben rund
123 GFlop/s Dauerlast gegen etwa 20 000 GFlop/s der Karte. Der Transfer ist
bei GPU-residentem Zustand vernachlässigbar (9 MB/s). **Der Engpass ist
Kernel-Start und Jitter, nicht Durchsatz** — CUDA Graphs bzw. das
OpenCL-Äquivalent sind dafür gemacht.

Zur Schatten-Veralterung, falls der Puffer voraus rechnet:

| Alter | Füllung | Naht > 4 kHz | Mod-Flachheit |
|---|---|---|---|
| 0 | −20,9 | −0,0 | 0,366 |
| 1 Periode | −20,8 | 0,0 | 0,366 |
| 4 | −19,0 | −0,0 | 0,365 |
| 8 | −17,6 | −0,0 | 0,365 |
| 16 | −13,7 | −0,1 | — |

Die Füllung altert, **Naht und Modulationsstruktur nicht**. Bei einer Periode
Alter kostet es 0,6 dB — irrelevant gegenüber dem Ordnungsgewinn.

Das eigentliche Risiko ist die geteilte GPU: Ein Compositor-Frame kann die
Warteschlange für zig Millisekunden blockieren. Genau dagegen macht der
Vorlauf das System tolerant.

---

## 4 Toneinsätze — offener Befund

Der Nutzer hört an Toneinsätzen ein Kratzen. Bandaufgelöst, an **exponierten**
Einsätzen der Benchmark-Rampe (dünner Bereich, Ordnung 4, m = 14):

| Versatz nach Einsatz | < 500 Hz | 0,5–2 k | 2–4 k | > 4 kHz |
|---|---|---|---|---|
| 0 ms | −13,0 | −14,3 | −14,1 | −12,7 |
| 20 ms | −16,6 | −14,1 | **−10,0** | −10,5 |
| 100 ms | −17,8 | −15,1 | −10,7 | −13,5 |
| 300 ms | −23,0 | −25,4 | −23,0 | −21,4 |
| Kontrolle (−500 ms) | −23,7 | −27,1 | −26,3 | −20,1 |

Bis zu **16 dB Abstand zur Kontrolle** im Band 2–4 kHz, Erholung erst nach
etwa 300 ms. Das passt zum Höreindruck.

Reale Pfeifen chirpen beim Einschwingen messbar: Spitzenrate rund
**3300 Hz/s**, Frequenzabweichung bis 10,6 Hz — also etwa eine Binbreite.
Das ist 140-mal schneller als das schnellste Testglissando.

**Aber:** Eine kürzere Anpassungsspanne hilft am Einsatz *nicht* (85 ms
gegen 469 ms: 2 dB Streuung, im kritischen Band sogar schlechter), während
sie beim reinen Glissando 8 dB brachte. Die Chirp-Erklärung trägt also
höchstens teilweise.

**Aufgabe.** Ursachentrennung an synthetischem Material: dasselbe Signal
einmal mit entdrifteter Frequenz (Hüllkurve original) und einmal mit
geglätteter Hüllkurve (Drift original). Was den Fehler senkt, ist die
Ursache. Dritter Kandidat, den keine Rekursion modellieren kann: der
breitbandige Anblasgeräusch-Anteil.

*Zu bedenken:* Das Analysefenster ist mit 85 ms **länger als der
Einschwingvorgang** (10–50 ms). Ein Frame enthält dann Stille davor und
Transiente danach in einem — ein Zustand, den keine Rekursion mit konstanten
Koeffizienten beschreibt, unabhängig von der Frequenzdrift.

---

## 5 Was nicht weiterverfolgt werden sollte

Alles gemessen und verworfen:

| Idee | Befund |
|---|---|
| Frame 8k statt 4k | +8,8 dB am Akkord, −3,3 am Einzelton, dreifache Kosten |
| Bandsplit 8k unten / 4k oben | +2,0 dB für dreifache Rechenzeit |
| Fenstergröße pro Lücke umschalten | Cache-Entwertung frisst den Gewinn vollständig |
| Zweiseitige Interpolation (Latenz) | Obergrenze +0,5 bis +1,6 dB bei 260 ms Latenz |
| Zeitbereichs-Janssen | ordnungs-verhungert, −7,3 dB gegen −18 |
| Ordnungs-Gate pro Bin | drei Varianten getestet, alle neutral bis schädlich |
| Wurzelkappung bei 1,15 | schlechter, siehe §0 |
| Neuronale Verdeckung | Wiener-Schranke lässt nur 1,6–5 dB Luft; Phase und Latenz ungelöst |

---

## 6 Der Hebel außerhalb des Prädiktors

Der verbleibende hörbare Rest ist nach den Messungen **kein Schätzproblem**,
sondern die Modulationsstruktur der Ausfälle. Bei identischer Fehlerenergie:

| Ausfallmuster | spektrale Flachheit des Fehlers | Spitze/Median |
|---|---|---|
| zufällig | 0,38 | 50× |
| regelmäßig | 0,21 | 2687× |

Die Ordnung ändert die Flachheit um 0,03, das Ausfallmuster um 0,18 — Faktor
sechs im Hebel. Und der Nutzer berichtet, dass auch *geklumpte* Zufallsmuster
rau klingen, was zu niederfrequenter Modulation passt.

**Kriterium ist damit nicht nur Aperiodizität, sondern auch Klumpenfreiheit** —
also ein Prozess mit Mindestabstand statt Bernoulli. In der Injektion frei
wählbar und sofort hörtestbar; bei echter Überlast nur begrenzt steuerbar.

---

## 7 Reihenfolge, die ich vorschlagen würde

1. Schur-Cohn-Kappung gegen Wurzelkappung messen (§2) — löst die fp32-Frage
   und ist billig.
2. Kalibrierkurve Dichte gegen Ordnung (§1.1) — größter unerschlossener
   Gewinn, mit GPU praktisch kostenlos.
3. Trend-Staffelung vermessen (§1.2) — behebt die letzte Ordnung-1-Altlast.
4. Ursachentrennung am Einsatz (§4) — adressiert den konkret gehörten Fehler.
5. Injektion mit Mindestabstand, Hörtest (§6).
