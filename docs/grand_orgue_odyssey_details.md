# GrandOrgue-Odyssey-Details.md

> Version: 1.0 • Stand: 2025‑08‑22 • Autor: KI-Assistent (auf Basis der gemeinsamen GO‑Odyssee)
>
> Zweck: **Extrem detaillierte Knowledge Base**. Aus dieser Datei sollen KI‑Assistenten (Cline/Agent/API) automatisiert präzise Ergänzungen in die RepoMap einpflegen können. Enthält **konkrete Kommandos, Code‑Snippets, Fehlermeldungen, Fixes, Algorithmen, Prüfpläne** und **Einfüge‑Marker** für zielgerichtete Updates.

---

## Leitlinien & Struktur für Automatisierung

- **Zieldateien (RepoMap)**: `BuildCheatsheet.md`, `SoundEngine.md`, `SoundEngine-Full.md`, `CrossfadeIntegration.md`, `ODFLoading.md`, `ModelPipes.md`, `RepoMap.md`.
- **Einfüge‑Marker** (damit eine API gezielt patchen kann):
  - `<!-- INSERT:BUILD:TOOLCHAIN -->` in `BuildCheatsheet.md`
  - `<!-- INSERT:SOUND:FADES -->` in `SoundEngine.md`
  - `<!-- INSERT:SOUND:BUGFIXES -->` in `SoundEngine-Full.md`
  - `<!-- INSERT:CROSSFADE:THEORY -->` in `CrossfadeIntegration.md`
  - `<!-- INSERT:ODF:DRY_WET_COMBINE -->` in `ODFLoading.md`
  - `<!-- INSERT:ATTACK:ANALYSIS -->` in `SoundEngine-Full.md`
- **Commit‑Vorlage**: `docs: sync Odyssey details → <Zieldatei> [section=<MARKER>]`
- **Konfidenz‑Flag**: `[confidence: high|medium|low]` an jedem Patch mitführen.

---

## Build & Toolchain (sehr detailliert)

### Zielplattformen
- Host: Linux (Debian/Ubuntu, x86_64)
- Target: Windows (x86_64) via MinGW‑w64 (Cross‑Compile)
- IDE: VS Code mit clangd (Navigation)

### Voraussetzungen (Pakete)
```bash
sudo apt update
sudo apt install -y git cmake ninja-build autoconf automake libtool \
  g++ make pkg-config python3 python3-venv python3-pip \
  mingw-w64 mingw-w64-tools binutils-mingw-w64-x86-64 wine-stable
sudo apt install -y libjack-jackd2-dev libasound2-dev libpulse-dev   # optional
```

### Toolchain-Datei (MinGW) unter /home/ubuntu/mingw-toolchain.cmake
```cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

### wxWidgets 3.2.3 (Windows‑Crossbuild)
**Quelle:** offizielles wxWidgets Release. **Hinweis:** Autotools‑Variante mit --enable-static wird ignoriert, daher CMake bevorzugen.

**Build (Monolith, Shared, ohne Samples/Tests)**
```bash
export WX_PREFIX=$HOME/wxWidgets-mingw/install
mkdir -p "$WX_PREFIX"

git clone --depth 1 --branch v3.2.3 https://github.com/wxWidgets/wxWidgets.git
cd wxWidgets
mkdir build-mingw && cd build-mingw

cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/home/ubuntu/mingw-toolchain.cmake \
  -DCMAKE_INSTALL_PREFIX=${WX_PREFIX} \
  -DwxBUILD_SHARED=ON -DwxBUILD_MONOLITHIC=ON \
  -DwxBUILD_TOOLKIT=msw -DwxBUILD_SAMPLES=OFF -DwxBUILD_TESTS=OFF \
  ..

ninja
ninja install
```

**Scintilla‑Fix** in src/stc/AutoComplete.cxx
```diff
-    return Scintilla::CompareNCaseInsensitive(s1, len, s2) == 0;
+    return CompareNCaseInsensitive(s1, len, s2) == 0;
```
[reason] Fehlender Namespace im Cross‑Build. [confidence: high]

### Audio Backends
- PortAudio (WASAPI): historisch Problem **PROPVARIANT undeclared**.
  - Cross‑Build Workaround: **RtAudio** bevorzugen oder WASAPI deaktivieren; JACK optional. [confidence: medium]
- ASIO: Pfad **konfigurierbar** halten (`-DASIO_SDK_PATH=<pfad>`) und nicht auf /usr/local/asio-sdk fest verdrahten. [confidence: high]

### CMake‑Patches (wxWidgets‑Erkennung im Crossbuild)
Problem: `find_package(wxWidgets)` scheitert häufig im Cross.

**Workaround (vor cmake ausführen)**
```bash
sed -i 's/find_package(wxWidgets .*$/# replaced by custom wx config/g' CMakeLists.txt
cat >> CMakeLists.txt <<'EOF'
# BEGIN: custom wx setup (cross)
set(wxWidgets_FOUND ON)
set(wxWidgets_LIBRARIES wxmsw32u)
set(wxWidgets_INCLUDE_DIRS "${WX_PREFIX}/include;${WX_PREFIX}/lib/gcc_x64_lib/mswud")
include_directories(${wxWidgets_INCLUDE_DIRS})
# END: custom wx setup
EOF
```

### VS Code / clangd Navigation
Problem: `wx/setup.h` nicht gefunden.

**Lösung: compile_commands.json patchen**
```python
#!/usr/bin/env python3
import json, pathlib
CC = pathlib.Path('build/win64/compile_commands.json')
INC = [
    '-isystem', '/home/ubuntu/wxWidgets-mingw/install/lib/gcc_x64_lib/mswud',
    '-isystem', '/home/ubuntu/wxWidgets-mingw/install/include',
]

data = json.loads(CC.read_text(encoding='utf-8'))
changed = False
for cmd in data:
    cmd_str = cmd.get('command') or ' '
    if 'wx/setup.h' not in cmd_str and 'gcc_x64_lib' not in cmd_str:
        cmd['command'] = cmd_str.strip() + ' ' + ' '.join(INC)
        changed = True
if changed:
    CC.write_text(json.dumps(data, indent=2), encoding='utf-8')
    print('[patched] compile_commands.json updated')
else:
    print('[patched] no changes')
```
[confidence: high]

### Referenz‑Buildskript (auszugsweise)
```bash
#!/usr/bin/env bash
set -euo pipefail
WX_PREFIX=${WX_PREFIX:-$HOME/wxWidgets-mingw/install}
ASIO_SDK_PATH=${ASIO_SDK_PATH:-$HOME/sdk/asio}
TC_FILE=/home/ubuntu/mingw-toolchain.cmake
BUILD_DIR=build/win64
[[ "${1:-}" == "--clean" ]] && rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR"
cmake -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$TC_FILE \
  -DCMAKE_PREFIX_PATH=$WX_PREFIX \
  -DASIO_SDK_PATH=$ASIO_SDK_PATH \
  -DwxWidgets_CONFIG_EXECUTABLE=$WX_PREFIX/bin/wx-config \
  ../..
ninja -v GrandOrgue
python3 ../../build-scripts/patch_compile_commands.py || true
```

---

## Sound Engine & DSP (Dateistellen, Algorithmen, Fixes)

### Relevante Dateien/Klassen
- `src/grandorgue/sound/GOSoundFader.h/.cpp` – Fades, Crossfades
- `src/grandorgue/sound/GOSoundingPipe.cpp` – Sample‑Trigger, Release‑Handling
- `src/grandorgue/sound/GO_Attack_Parameters.cpp` – Tabellen für Attack/Curvature

### Fade‑Varianten (Referenz)
**Linear**
```cpp
inline float fade_linear(unsigned frame, unsigned nFrames) {
  if (nFrames == 0) return 1.0f; // Guard
  return static_cast<float>(frame) / static_cast<float>(nFrames);
}
```
**Sinus (sin², „glatt“)**
```cpp
inline float fade_sin2(unsigned frame, unsigned nFrames) {
  if (nFrames == 0) return 1.0f; // Guard
  const float x = static_cast<float>(frame) / static_cast<float>(nFrames);
  const float s = sinf(0.5f * (float)M_PI * x);
  return s * s; // sin^2
}
```
**Cosinus (cos²)**
```cpp
inline float fade_cos2(unsigned frame, unsigned nFrames) {
  if (nFrames == 0) return 1.0f;
  const float x = static_cast<float>(frame) / static_cast<float>(nFrames);
  const float c = cosf(0.5f * (float)M_PI * x);
  return c * c; // cos^2
}
```
**Equal‑Power Crossfade (Theorie)**
```
w_s(t) = cos^2(pi t / 2);   w_r(t) = sin^2(pi t / 2)
# Energieerhaltung für unkorrelierte Signale; bei Phasenkohärenz Pegelberge möglich.
```

### Guard‑Cases & bekannte Bugs
- `nFrames == 0` ohne Guard → Stille bei Release‑Start.
```cpp
if (nFrames == 0) fadeFactor = 1.0f; // in allen Fadern sicherstellen
```
- Sehr kurze Tastenanschläge: künstlicher Fade‑Out.
  - Workaround: `decay = 0` im Release‑Zweig; Release‑Sample übernimmt Abklingen. [confidence: high]

### Crossfade‑Gewichte (Klicks vermeiden)
> Beim Übergang Attack ↔ Release ist die korrekte Mischung:
```
mix = Attack * w_release + Release * w_attack
```
Empirisch bessere Phasennähte. [confidence: high]

### Test‑Matrix (Hörtests)
| Szenario | Samples | Erwartung | Beobachtung | Ergebnis |
|---|---|---|---|---|
| kurzer Key | 060‑C rel200 vs rel99999 | nahtlos | sin² „Schluckauf“ | linear/angepasst bevorzugen |
| mittlerer Key | rel400 | stabil | ok | ok |
| langer Key | rel99999 | glatter Sustain→Release | ok | ok |

---

## ODF (.organ) – Parsing & Dry/Wet‑Merge (Text‑basiert)

### Grundsätze
- `.organ` ist **kein XML**, sondern INI‑ähnliches Textformat mit `[SectionNNN]` und `Key=Value`.
- Parser **rein textuell** (Regex/State‑Machine), keine XML‑Libs verwenden. [confidence: high]

### Minimal‑Grammatik (Gedankenmodell)
```ini
[SectionNameNNN]
Key=Value
Key2=Value2
; Kommentarzeilen mit Semikolon
```

### Merge‑Ziel (Dry → Wet erweitern)
- Dry‑Ranks importieren, neue freie IDs vergeben
- Zugehörige WindchestGroups übernehmen
- Neue Enclosure „DryRanks“ anlegen und im `[Organ]` referenzieren (`NumberOfEnclosures`, Enclosure001..N)
- Stops im Wet‑Preset um `DryRankID=RankNNN` ergänzen

### Invarianten
- Keine ID‑Kollisionen, konsistente Referenzen, gültige Pipe‑Pfade

### Beispiel‑Transformation
**Vorher (Wet)**
```ini
[Stop012]
Name=Gedackt 8'
RankID=Rank034
```
**Nachher (Dry ergänzt)**
```ini
[Stop012]
Name=Gedackt 8'
RankID=Rank034
DryRankID=Rank057
```
**Neue Enclosure**
```ini
[Enclosure003]
Name=DryRanks
Gain=0.0
```
**Organ‑Block**
```ini
NumberOfEnclosures=3
Enclosure001=Enclosure001
Enclosure002=Enclosure002
Enclosure003=Enclosure003
```

### Python‑Parser (Skelett, robust)
```python
#!/usr/bin/env python3
import re
HEADER = re.compile(r"^\[(?:[A-Za-z]+)(?:[0-9]{3})\]$")
KV     = re.compile(r"^[A-Za-z0-9_]+=.*$")
```
> Hinweis: Vollparser im Projekt verwenden; hier nur Outline. [confidence: high]

---

## Attack & Release – Analyse & Tabellen

### Envelope‑Optionen
- Standard: `gauss_abs` (Gauß‑Fenster auf |Signal|)
- Alternativen: `rms`, `hilbert`
- Glättung: 50 ms Moving‑Average oder Savitzky–Golay

### Fit‑Fenster
- Start bei 0.1 s, Ende am ersten Erreichen von 95 % des Maximums innerhalb 1 s

### t_max‑Methoden
- **Slope**: Steigungsmaximum → Abfall unter 20 % → Extrapolation bis Steigung ~ 0
- **Plateau**: Zeitpunkt 95 %, darf nicht vor dem Steigungsmaximum liegen
- **Top‑10 Maxima + Parabelfit**: Kandidaten via Ableitung, Auswahl per MSE

### Parabel‑Krümmung
- `a = -p / t_max^2`, p in [0, 1]

### Robuster g0‑Fit (linear, Huber‑Gewichte)
```python
def huber_fit_g0_with_constraint(t, y, t_max, delta=0.02, iters=60):
    tmax = float(max(t_max, 1e-12))
    x = t / tmax
    a = (1.0 - x)
    b = x
    num = float((a * (y - b)).sum())
    den = float((a * a).sum()) + 1e-12
    g0 = max(0.0, min(1.0, num / den))
    for _ in range(iters):
        r = a * g0 + b - y
        w = 1.0 / (1.0 + (r / delta)**2)
        num = float((w * a * (y - b)).sum())
        den = float((w * a * a).sum()) + 1e-12
        g0 = max(0.0, min(1.0, num / den))
    return g0
```

### CSV‑Schema
```csv
file,midi,mode,tmax_s,g0_norm,a_ms2,env,rmse,score
036-C.wav,36,fixed_plateau_huber,0.18,0.00,-3.086e-05,gauss_abs,0.05527,0.91
```

### C++‑Export
```cpp
// GO_Attack_Parameters.cpp
using f32 = float;
const f32 curvature_by_midi[128]   = { /* auto‑gen */ };
const f32 attack_time_by_midi[128] = { /* auto‑gen */ };
```

### Plot‑Richtlinien
- Referenzlinie bei t = t_max
- Sicherstellen: Gerade endet bei g(t_max)=1 und verläuft danach horizontal

---

## Known Issues (ausführlich)

### Build
- PortAudio WASAPI: PROPVARIANT undeclared → Cross: RtAudio bevorzugen oder WASAPI ausblenden
- find_package(wxWidgets) scheitert → Custom wx‑Setup
- clangd: wx/setup.h fehlt → compile_commands.json patchen

### Sound
- nFrames==0 ohne Guard → Stille
- sin² kann Pegelberg verursachen
- extrem kurze Keys: Fade‑Out vermeiden, Release‑Samples nutzen

### ODF
- kein XML‑Parser verwenden, rein textuell

---

## API/Agent‑Integrationsplan (automatisch patchen)

### Mapping (Marker → Ziel → Quelle)
| Marker | Zieldatei | Quelle |
|---|---|---|
| BUILD:TOOLCHAIN | BuildCheatsheet.md | Build & Toolchain |
| SOUND:FADES | SoundEngine.md | Sound Engine, Fades |
| SOUND:BUGFIXES | SoundEngine-Full.md | Guard‑Cases, Bugs |
| CROSSFADE:THEORY | CrossfadeIntegration.md | Equal‑Power etc. |
| ODF:DRY_WET_COMBINE | ODFLoading.md | ODF‑Merge |
| ATTACK:ANALYSIS | SoundEngine-Full.md | Attack‑Analyse |

### Patch‑Policy
1. Idempotent: Mehrfachläufe müssen stabil sein
2. Konfidenz: nur high automatisch mergen
3. Diff‑Prüfung: bestehende Abschnitte nur ersetzen, wenn Hash geändert

### PR‑Template (Kurz)
```md
### Zweck
Sync Odyssey‑Details → <Datei> [section=<MARKER>]

### Änderung
- Inhalt aus Odyssey v1.0, Abschnitt <…>

### Test
- Cross mingw Build ok
- clangd Navigation ok

[confidence: high]
```

---

## Validierungs‑Checkliste
- [ ] Cross‑Build erzeugt GrandOrgue.exe
- [ ] wxWidgets‑Includes in compile_commands.json
- [ ] Guard‑Case nFrames==0 in allen Fadern
- [ ] Hörtests kurz/mittel/lang bestanden
- [ ] ODF‑Merge ohne ID‑Kollisionen, Enclosure korrekt
- [ ] GO_Attack_Parameters.cpp: 128 Werte je Array

---

## Anhang: typische Fehler & Diagnose

**AttributeError (CLI‑Kompatibilität, Python)**
```
AttributeError: 'Namespace' object has no attribute 'fit'
Ursache: Option --fit-hop-ms falsch geparst.
Fix: CLI‑Alias akzeptieren, beide Schreibweisen erlauben.
```

**TypeError (API‑Signatur geändert)**
```
choose_tmax_m2_vs_plateau() got an unexpected keyword argument 'peak_consistency_level'
Ursache: Aufrufer nutzt neuere Signatur als Implementierung.
Fix: Wrapper akzeptiert optionale Parameter via kwargs.
```

**CMake Cache (Versionstag)**
```
GO_VERSION_EXTRA:UNINITIALIZED=XFadeDemo
Hinweis: Tag ist nicht standardisiert; am einfachsten via About‑Dialog oder CLI‑Flag einblenden.
```

---

> Ende. Datei enthält bewusst **Markierungen** und präzise Blöcke, damit Assistenten Updates automatisch in die RepoMap übernehmen können. [confidence: high]

