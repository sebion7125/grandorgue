# GrandOrgue LUT-Parität – korrigierter Implementationsplan

Stand: Python ist die algorithmische Referenz.  
Ziel: C++/GrandOrgue muss dieselben LUT-Punktlisten erzeugen wie `analyze_lut.py`.

Wichtige Grundregel:

- Python bleibt algorithmisch maßgeblich.
- C++ wird an Python angepasst.
- `--cpp-numerics` darf in Python nur numerische C++-Eigenschaften emulieren:
  - float32 statt float64,
  - C++-Rundung,
  - Integer-Skalierung,
  - Akkumulationsreihenfolge,
  - aber keine algorithmische Sonderlogik, nur um C++ nachzubilden.

Der vorherige Plan enthielt an einer Stelle die falsche Richtung: Beim Downsampling-Off-by-one darf nicht Python C++-gleich gemacht werden. Wenn Python-Referenz und C++ abweichen, muss C++ an Python angepasst werden oder Python nur dann geändert werden, wenn Python selbst objektiv inkonsistent zu seiner eigenen Referenzlogik ist.

---

## v230r – echte LUT-Paritäts-Hygiene, aber C++ folgt Python

### 1. Versionsstand synchronisieren

Beide Seiten müssen denselben Stand melden:

```text
Python TOOL_VERSION = v230r
GO align_version    = v230r
```

Das ist keine Algorithmusänderung, sondern Diagnose-Hygiene.

---

### 2. Downsampling-Off-by-one prüfen und C++ an Python anpassen

Python-Referenzverhalten:

```python
loop_seg = attack_mono[:loop_needed:ds]
release_ds = release_mono[:r_max + window_len + 1 : ds]
```

Für `arr[:N:ds]` gilt:

```text
verwendete Originalindizes: 0, ds, 2*ds, ... < N
Länge = ceil(N / ds)
```

C++ darf daher nicht stillschweigend einen zusätzlichen Downsample-Punkt bei Originalindex `N` einbeziehen, falls `N % ds == 0`.

Verdächtiges C++-Muster:

```cpp
loop_needed_d = loop_needed / ds + 1;
```

Das entspricht Python nur dann sicher, wenn `loop_needed` nicht exakt durch `ds` teilbar ist. Wenn `loop_needed % ds == 0`, enthält C++ potenziell einen zusätzlichen Punkt.

Referenzgleiche C++-Formel für Python-Slice `[:loop_needed:ds]`:

```cpp
// ceil(loop_needed / ds) for positive integers
const unsigned loop_needed_d = (loop_needed + ds - 1) / ds;
```

Für `release_mono[:r_max + window_len + 1 : ds]` entsprechend:

```cpp
const unsigned rel_needed = r_max + window_len + 1;
const unsigned rel_needed_d = (rel_needed + ds - 1) / ds;
```

Wichtig: Danach alle Guards gegen die tatsächlich erzeugte downsampled Länge prüfen:

```cpp
if (cs_d + window_len_d > loop_seg_d.size()) ...
if (r_d + window_len_d > release_ds.size()) ...
```

Akzeptanztest:

- Vorher/nachher Full Verify.
- Besonders Pipes prüfen, bei denen `loop_needed % ds == 0` oder `(r_max + window_len + 1) % ds == 0`.
- Wenn bisher 0 MISMATCH bestand, darf diese Änderung keine neuen Mismatches erzeugen. Falls doch, zeigt das, dass C++ vorher nicht Python-Referenz war.

---

### 3. `(scores == 0).all()` nicht blind entfernen, sondern Referenzentscheidung klären

Hier war die vorherige Empfehlung zu grob.

Aktuell ist Python Referenz. Wenn Python in `_compute_corr_scores()` bei einem komplett nulligen Scorevektor aussteigt, ist das zunächst Referenzverhalten – auch wenn es algorithmisch fragwürdig wirkt.

Python-Code sinngemäß:

```python
if r_max == 0 or (scores == 0).all():
    return None, 0
```

Daraus folgt:

- Nicht Python entfernen, nur weil C++ anders ist.
- Entweder C++ bekommt dieselbe Sonderfallentscheidung,
- oder man entscheidet bewusst, dass Python-Referenz hier korrigiert wird. Dann ist es aber eine Referenzänderung und muss als solche behandelt werden.

Empfohlene konservative Lösung für Parität:

```cpp
bool all_scores_zero = true;
for (float s : scores) {
    if (s != 0.0f) {
        all_scores_zero = false;
        break;
    }
}
if (r_max == 0 || all_scores_zero) {
    // same behavior as Python reference: no valid score vector
}
```

Bewertung:

- Praktisch vermutlich irrelevant für echte Pfeifen.
- Für formale Parität aber C++ an Python angleichen, nicht umgekehrt.

---

### 4. Peak-Sortierung: C++ muss Python-stabile Sortierung nachbilden

Python-Verhalten:

```python
peaks.sort(key=lambda idx: float(scores[idx]), reverse=True)
```

Python-Sort ist stabil. Bei exakt gleichem Score bleibt die ursprüngliche Scan-Reihenfolge erhalten. Da die Peaks in aufsteigendem `r` gesammelt werden, gewinnt bei Gleichstand der kleinere `r`-Index.

C++ muss das explizit tun, weil `std::sort` nicht stabil ist.

Empfohlener C++-Fix:

```cpp
std::sort(peaks.begin(), peaks.end(),
          [&](unsigned a, unsigned b) {
              const float sa = scores[a];
              const float sb = scores[b];
              if (sa != sb)
                  return sa > sb;
              return a < b; // Python stable-sort equivalent
          });
```

Alternativ:

```cpp
std::stable_sort(peaks.begin(), peaks.end(),
                 [&](unsigned a, unsigned b) {
                     return scores[a] > scores[b];
                 });
```

Die explizite Variante mit `return a < b` ist besser dokumentierbar und unabhängig von vorheriger Container-Reihenfolge.

---

### 5. Tote EPS-Konstante entfernen oder klar als deprecated markieren

Wenn `V2_BEAM_SORT_EPS` aktuell auf beiden Seiten ungenutzt ist, ist es kein Paritätsfehler. Trotzdem ist es riskant, weil EPS-Heuristiken in diesem Projekt bereits falsche Diffs erzeugt haben.

Empfehlung:

```python
# Deprecated: do not use for beam ordering.
# Beam ordering is exact-score first, then beam_id, then r_d.
V2_BEAM_SORT_EPS = 0.02
```

Besser langfristig: komplett entfernen, wenn wirklich kein Codepfad darauf zugreift.

---

## v230s – Verify-/Sim-Parität

### 6. Runtime-/Sim-Rundung: C++ oder Python-Referenz je nach Ziel trennen

Für LUT-Erzeugung gilt Python-Referenz.

Für die `sim`-CSV ist die Frage anders: Wenn `verify_lut.py` die C++-Runtime-Simulation prüfen soll, muss Python dort C++-Runtime-Verhalten nachbilden. Das ist keine LUT-Algorithmusänderung, sondern ein Verify-Modell.

C++ `std::round()`:

```text
half away from zero
```

Python `round()`:

```text
banker's rounding / half to even
```

Daher im Verify-Sim-Pfad nicht Python-`round()` verwenden, sondern eine explizite C++-Rundung:

```python
def _cpp_round(x: float) -> int:
    return int(math.floor(float(x) + 0.5))
```

Aber nicht verwechseln mit TrackStepV2, wo C++ inzwischen bewusst `RoundHalfEven()` nutzt, um `np.round()` nachzubilden.

---

## v230t – theoretische Robustheit

### 7. Mehrkanal-Mono-Mix nur falls C++ wirklich mehrkanalige WAVs in diesem Pfad nutzt

Wenn die Orgel effektiv Stereo-WAVs nutzt, ist das praktisch irrelevant.

Falls `nch >= 3` im LUT-Pfad vorkommt, muss C++ an Python oder Python-`--cpp-numerics` sauber an C++-Numerik angepasst werden. Da Python Referenz ist, gilt:

- Normaler Python-Pfad bleibt Referenz.
- `--cpp-numerics` darf C++-Akkumulation emulieren, solange keine Algorithmik verändert wird.

Wichtig ist nur, dass `verify_lut.py --cpp-numerics` nicht versehentlich andere mathematische Entscheidungen trifft.

---

### 8. `is_jump` / `approach_up` nach Pruning überprüfen

Hier ist zu klären, was Python aktuell wirklich als Referenz macht.

Regel:

- Wenn Python Flags vor dem Pruning setzt und danach nicht neu berechnet, muss C++ dasselbe tun.
- Wenn das mathematisch falsch erscheint, muss zuerst Python-Referenz bewusst geändert werden. Danach C++ anpassen.

Nicht einfach C++-Logik als Maßstab nehmen.

Review-Frage:

```text
Wann setzt Python final approach_up/is_jump?
Vor oder nach _prune_lut_points?
Wird nach Punktlöschung neu berechnet?
```

Dann C++ exakt entsprechend einstellen.

---

## Testreihenfolge

### 1. Versionscheck

```bash
python analyze_lut.py --version
# oder Ausgabe im GUI/CLI prüfen
```

GO-CSV-Header muss enthalten:

```text
align_version=v230r
```

---

### 2. Zieltests auf bekannte Problemfälle

```bash
python verify_lut.py go_lut_verify.csv organ.organ --cpp-numerics --filter "ped subbass 16|midi=55" --verbose
python verify_lut.py go_lut_verify.csv organ.organ --cpp-numerics --filter "ped subbass 16|midi=43" --verbose
python verify_lut.py go_lut_verify.csv organ.organ --cpp-numerics --filter "ped posaunen bass 16|midi=53" --verbose
```

---

### 3. Full Verify

```bash
python verify_lut.py go_lut_verify.csv organ.organ --cpp-numerics
```

Akzeptanz:

```text
MISMATCH=0
ERR=0
NOT_FOUND nur bekannte Noise/ODF-Fälle
```

---

## Priorität

1. C++ Peak-Sort-Tie-Break Python-stabil machen.
2. C++ Downsample-Längen an Python-Slice-Semantik angleichen.
3. C++ `(all scores zero)`-Sonderfall an Python angleichen oder bewusst als Referenzänderung in Python entfernen.
4. Versionen synchronisieren.
5. Sim-Rundung im Verify-Pfad trennen.
6. Mehrkanal-Mix und Flag-Recompute nur nach Bedarf prüfen.

