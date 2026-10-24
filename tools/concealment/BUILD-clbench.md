# clbench — den Xorcist-Solve-Kernel auf einem OpenCL-Device messen

Vergleicht `xo_solve` aus `src/xorcist/xorcist_kernels.cl` gegen die
CPU-Referenz: Wert für Wert und Mikrosekunde für Mikrosekunde.

Die Referenz ist **nicht eingecheckt**, sondern wird bei jedem Bauen aus
`src/xorcist/xorcist.cpp` erzeugt. Das ist Absicht: verglichen werden
soll der Kernel gegen den Code, den die Engine wirklich ausführt, und
eine eingecheckte Kopie wäre bei der ersten Änderung an `SolveBin`
veraltet, ohne dass es jemand merkt.

## Voraussetzungen

Header und ICD-Loader:

```bash
sudo apt install -y opencl-headers ocl-icd-opencl-dev
```

Dazu eine Laufzeit für das Zielgerät:

| Gerät | Paket |
|---|---|
| NVIDIA (RTX 3070 &c.) | nichts — steckt im Treiber |
| AMD | `mesa-opencl-icd` |
| Intel iGPU | `intel-opencl-icd` |
| CPU (zum Gegenprüfen) | `pocl-opencl-icd` |

Mit `clinfo` prüfen, dass das gewünschte Gerät auftaucht. Sind mehrere
Plattformen installiert, nimmt `clbench` **die erste** — dann entweder
die anderen ICD-Dateien in `/etc/OpenCL/vendors/` kurz wegschieben oder
`OCL_ICD_VENDORS` auf ein Verzeichnis mit nur der gewünschten setzen.

## Der kurze Weg

```bash
bash tools/concealment/run_clbench.sh            # 12294 Bins, Ordnungen 2..12
bash tools/concealment/run_clbench.sh 4098 4 8   # eigene Bin-Zahl/Ordnungen
```

Erzeugt die Referenz, baut, listet die gefundenen Geräte und misst.

## Wo bauen, wo laufen lassen

**Auf dem Host, nicht in der VM** — und zwar nicht wegen des Ablageorts
der Dateien, sondern wegen des Compilers: ein in der VM gebautes Binary
trägt deren glibc und deren ICD-Loader. Die Quellen dürfen ruhig über
einen sshfs-Mount liegen; `gcc` und die OpenCL-Laufzeit müssen die der
Maschine sein, auf der gemessen wird.

Ausgeführt wird ohnehin vom Host-Kernel, das Binary sieht also die echte
GPU. Nur wenn der Mount mit `noexec` eingehängt ist, muss das fertige
Binary vorher woandershin kopiert werden.

Große Bauvorgänge über sshfs sind zäh (jeder Compileraufruf macht viele
kleine Dateizugriffe); für `clbench` mit seinen zwei Dateien spielt das
keine Rolle.

## Der lange Weg, Schritt für Schritt

Aus dem Repository-Wurzelverzeichnis:

```bash
python3 tools/concealment/make_ref.py > /tmp/ref.c
gcc -O2 -o /tmp/clbench tools/concealment/clbench.c /tmp/ref.c -lOpenCL -lm
/tmp/clbench <ordnung> <bins> src/xorcist/xorcist_kernels.cl
```

`bins` = 2049 × Kanalzahl, für 5.1 also **12294**. Beispiel:

```bash
for o in 4 8 12; do /tmp/clbench $o 12294 src/xorcist/xorcist_kernels.cl; done
```

Ausgegeben werden Gerätename, Recheneinheiten, Device-Zeit, CPU-Zeit auf
**einem** Kern, der Faktor und die größte Abweichung gegen die Referenz.
Die Abweichung muss `0.00e+00` sein; alles andere ist ein Befund.

## Wie die Zahlen zu lesen sind

Der Test löst **alle** Bins in einem Rutsch. Die Engine löst pro Periode
nur **1/16** davon: die Koeffizienten stammen aus einem ~20-Frame-Gedächt-
nis und werden rollierend erneuert (Zyklus = `AR_SOLVE_EVERY` 4 Raster-
punkte × `arStride` 4 Perioden = 16 Perioden). Jeder Bin ist dadurch
genauso frisch wie vorher, die Last liegt nur flach statt als Schwall alle
16 Perioden — was vorher als Spitze von 1,5 bis 2,1 ms im Monitor auftauchte.

Für den Echtzeit-Anteil also durch 16 teilen; `run_clbench.sh` rechnet das
selbst aus.

## Gemessen: RTX 3070 gegen POCL (12 294 Bins = 5.1)

| Ordnung | 3070, alle Bins | 3070 pro Periode | von 2667 µs | POCL (CPU, 10 Kerne) | CPU, 1 Kern |
|---|---|---|---|---|---|
| 2 | 42,5 µs | 2,7 µs | 0,1 % | — | 905 µs |
| 4 | 297,6 µs | 18,6 µs | 0,7 % | 808 µs | 7 381 µs |
| 6 | 731,4 µs | 45,7 µs | 1,7 % | — | 15 101 µs |
| 8 | 1 725,8 µs | 107,9 µs | 4,0 % | 3 378 µs | 28 800 µs |
| 12 | 5 526,7 µs | 345,4 µs | **13 %** | 7 978 µs | 80 088 µs |

Ordnung 12 kostet auf einem CPU-Kern 5 005 µs pro Periode — das Doppelte
des gesamten Budgets, also unmöglich. Auf der 3070 sind es 13 %.

Die maximale Abweichung ist über alle Ordnungen **exakt null**: die Karte
rechnet bitgleich zur CPU-Referenz, trotz anderer Hardware, anderem
Compiler und anderer Ausführungsreihenfolge.

Der Faktor gegenüber einem CPU-Kern *fällt* mit der Ordnung (21× bei 2,
14,5× bei 12). Bei Ordnung 12 ist die Scratch-Matrix 12×26 doubles, also
~2,5 kB privat pro Thread — dort beginnt die 3070 aus dem Registerfile zu
spillen. Derselbe Effekt, der POCL bei `XO_ORDER_MAX = 20` zum Absturz
brachte, nur milder.

## fp32: das Tempo ist da, die Genauigkeit noch nicht

`XO_FP32=1` auf derselben Karte, 12 294 Bins:

| Ordnung | Device | pro Periode | von 2667 µs | Faktor gg. 1 CPU-Kern | max. Abweichung |
|---|---|---|---|---|---|
| 2 | 13,8 µs | 0,9 µs | 0,0 % | 66× | 2,4e−05 |
| 4 | 118,1 µs | 7,4 µs | 0,3 % | 59× | **1,7e−07** |
| 6 | 291,0 µs | 18,2 µs | 0,7 % | 51× | **3,8e−07** |
| 8 | 573,3 µs | 35,8 µs | 1,3 % | 48× | 1,5e−01 |
| 12 | 1 601,9 µs | 100,1 µs | 3,8 % | 48× | 4,1e−01 |

Gegenüber fp64 ist die Karte 3,4-mal schneller — Ordnung 12 kostet 100 µs
pro Periode statt 345.

Die Abweichung springt zwischen Ordnung 6 und 8 um sechs Größenordnungen,
und das Muster sagt, wo es klemmt: **nicht** bei der Wurzelsuche.
Durand-Kerner ist iterativ und korrigiert sich selbst, deshalb liegen
Ordnung 4 und 6 bei 1e−07 — es findet die exakten Wurzeln eines
ungenauen Polynoms. Ungenau wird das Polynom bei der **Gauß-Elimination**:
die Lösung besteht aus großen, fast auslöschenden Koeffizienten, und
genau deren Differenzstruktur verliert single precision.

Ordnung 2 fällt mit 2,4e−05 heraus, weil sie die geschlossene Formel
nutzt (komplexe Wurzel aus der Diskriminante) statt der selbstkorrigie-
renden Iteration.

**Brauchbar ist fp32 damit heute bis Ordnung 6.** Darüber gehört fp64
genommen — oder die Elimination repariert. Der Lehrbuchweg dafür ist
iterative Nachkorrektur (in float lösen, Residuum mit `fma` bilden,
Korrektur nachlösen, addieren); ein erster Versuch machte es auf 2,7
schlechter statt besser und wurde zurückgenommen, der Fehler ist noch
nicht gefunden. Und ungemessen bleibt weiterhin, was 1,5e−01 auf echtem
Material überhaupt in dB kostet — die Wurzelkappung dahinter zwingt
ohnehin alles auf den Einheitskreis.

**Noch nicht gemessen: die Umlaufzeit.** Kernel-Start kostet auf NVIDIA
typisch 5–20 µs, und pro Periode bräuchte es drei davon (RLS, Solve,
Advance) plus Transfers. Bei Ordnung 4 mit 18,6 µs Rechenzeit wäre der
Start also teurer als die Arbeit — die GPU lohnt ab etwa Ordnung 6 bis 8,
und genau dort kann die CPU nicht mehr mit.

## Fallstrick, der beim ersten Lauf zuschlug

Die Scratch-Matrix des Lösers ist `order × (2·order+2)` **privater**
Speicher pro Work-Item. Auf `XO_ORDER_MAX` dimensioniert waren das bei 20
rund 6,7 kB je Work-Item — POCL stürzte ab, und eine NVIDIA-GPU würde aus
dem Registerfile in Local Memory spillen und den gesamten
Parallelitätsgewinn zurückgeben. `clbench` übersetzt das Programm deshalb
mit `-DXO_ORDER_MAX=<tatsächliche Ordnung>`; ein späterer Host sollte das
genauso tun und die Kernel-Builds cachen, denn die Ordnung ändert sich
höchstens, wenn jemand am Regler dreht.

## Schur-Cohn statt Wurzelsuche

`XO_SCHUR=1` ersetzt Durand-Kerner durch die wurzelfreie Stabilisierung
über die Reflexionskoeffizienten (Abstieg α → k, Kastenbedingung, Levinson-
Aufstieg). Beide Richtungen sind **endliche** O(p²)-Operationsfolgen — damit
entfällt die einzige fp32-anfällige Stufe.

Die Prüfgröße ist dort eine andere: Koeffizientenabstand und Wurzelwinkel
sind sinnlos, weil das Verfahren gar keine Wurzeln berechnet und ein
*anderes* Ergebnis liefert als die selektive Kappung, keine Näherung
desselben. Geprüft wird die **Invariante**: liegt jede Nullstelle des
zurückgegebenen Polynoms im Einheitskreis. Der Bench rechnet die Wurzeln
dafür nachträglich auf dem Host.

```bash
XO_SCHUR=1 XO_FP32=1 bash tools/concealment/run_clbench.sh
```

### Gemessen: RTX 3070, 12 294 Bins (5.1)

| Ordnung | Schur-Cohn fp32 | pro Periode | Durand-Kerner fp64 | pro Periode | Gewinn | max \|z\| |
|---|---|---|---|---|---|---|
| 4 | 35,5 µs | 2,2 µs | 298,4 µs | 18,6 µs | **8,4×** | 0,854928 |
| 6 | 109,8 µs | 6,9 µs | 704,4 µs | 44,0 µs | **6,4×** | 0,974948 |
| 8 | 264,0 µs | 16,5 µs | 1 577,1 µs | 98,6 µs | **6,0×** | 0,999992 |
| 12 | 926,6 µs | 57,9 µs | 4 484,8 µs | 280,3 µs | **4,8×** | 1,000001 |

Gegen **einen** CPU-Kern sind das 195× bei Ordnung 4 und 83× bei Ordnung 12.
Ordnung 12 kostet damit 2,2 % des Periodenbudgets statt 10,5 %.

Die Invariante hält auch unter NVIDIAs Compiler (1,000001 bei Ordnung 12
ist die Genauigkeit des host-seitigen Wurzelfinders, nicht eine Verletzung)
— das war keine Formsache, weil andere Compiler Gleitkommaoperationen
anders umordnen und die Kappung genau davon lebt.

Der Gewinn **fällt** mit der Ordnung (8,4× auf 4,8×), weil Durand-Kerner
bei kleiner Ordnung anteilig mehr Zeit frisst, während Schur-Cohn immer
zwei O(p²)-Durchläufe braucht. Beide Verfahren wachsen mit p³ durch die
Elimination.

Die Formeln lassen sich außerdem gegen die Referenzspuren der Herleitung
prüfen, ohne jedes Device:

```bash
gcc -O2 -I src/xorcist -o /tmp/schur_test tools/concealment/schur_test.c -lm
/tmp/schur_test
```

Genau dieser Test hat zwei Fehler gefunden, die die Invariante nur knapp
verletzten (max |z| = 1,0028 statt 2,5 wie bei groben Konventionsfehlern):
der Nennerschutz `den > eps` statt `fabs(den) > eps` — bei |k| > 1 ist
1 − |k|² negativ, und genau dieser Fall stieg aus und gab das instabile
Polynom unverändert zurück — und ein Aliasing im Abstieg, weil Schritt i
auf α_{l−i} zugreift, das ein In-Place-Schreiben für i > l/2 bereits
ersetzt hat.

## Warum überhaupt GPU

Weil auf der CPU nichts mehr liegt. `libxorcist` enthält null
`ymm`-Instruktionen (kein `-march`, keine Intrinsics — anders als GOs
Sampler-Pfad, der AVX2 mit `__builtin_cpu_supports`-Dispatch nutzt), aber
ein Neubau mit `-march=x86-64-v3` bringt 7 % bei Ordnung 1 und nichts bei
2 bis 4. Der heiße Pfad ist Gauß-Elimination mit Pivotsuche plus
Durand-Kerner, pro Bin — skalar und verzweigt. Dagegen helfen keine
breiteren Vektoren, sondern nur mehr unabhängige Bahnen.
