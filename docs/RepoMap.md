# RepoMap – Überblick und Navigationshilfe

Ziel: Schnelles Auffinden relevanter Bereiche (Build, Audio-Engine, GUI, Modell) und Verstehen der Struktur dieses GrandOrgue-Forks.

## Top-Level
- `CMakeLists.txt` – Projektweite CMake-Konfiguration
- `README.md`, `BUILD.md`, `INSTALL.md`, `CHANGELOG.md`, `LICENSE`
- `index.yaml`, `version.txt`
- `.clang-format`, `.pre-commit-config.yaml` – Code-Style und Hooks
- `.cline/context.md` – Cline-spezifischer Kontext (Hinweise und Konventionen)

## Build-Skripte und Artefakte
- `build-scripts/for-win64/`
  - `build-on-linux.sh` – Cross-Build für Windows (MinGW)
  - `update-build-on-linux.sh` – Toolchain/Deps aktualisieren und bauen
  - Ergebnis: `build-scripts/for-win64/build/` (Cross-Build-Artefakte)
- `build-scripts/for-linux/`
  - `build-on-linux.sh` – Build nativ für Linux
  - `build-for-tests.sh`, `do-tests.sh` – Test-Builds/Tests
  - Ergebnis: `build-scripts/for-linux/build/`
- Weitere Targets: `for-osx/`, `for-linux-aarch64/`, `for-linux-armhf/`, `for-appimage-x86_64/`
- Clangd/IntelliSense:
  - `.vscode/settings.json` verweist auf `--compile-commands-dir=build/win64`
  - Falls Du Cross-Build verwendest, achte darauf, dass `compile_commands.json` in `build/win64/` landet bzw. symlinked wird

## Quellcode – Hauptbäume
- `src/`
  - `grandorgue/` – Anwendungsspezifische Logik
    - `sound/` – Audio-Engine, DSP, Provider, Fader
      - Kern-Dateien (Auswahl):
        - `GOSoundEngine.h/.cpp` – Zentrale Sound-Engine
        - `GOSoundProvider.h/.cpp` – Bereitstellung von Audiodaten
        - `GOSoundFader.h/.cpp` – Fading/Überblendungen
        - `GOSoundAudioSection.cpp` – Audio-Sektion/Blöcke
        - `GOCrossfadeMode.h`, `GOCrossfadeParam.h` – Crossfade-Modi/Parameter
        - `GO_Attack_Parameters.h/.cpp` – Attack-Parameter der Klangerzeugung
        - `GO_DebugRelease.h` – Build-abhängige Schalter
    - `model/` – Orgelmodell, Pfeifen, Konfiguration
      - `GOPipe.h` – Basisklasse Rohr/Pipe
      - `GOSoundingPipe.h/.cpp` – Klang-erzeugende Pfeife
      - `pipe-config/GOPipeConfigNode.h` – Konfigurationsknoten
      - `GOOrganController.cpp` – Orgelsteuerung
    - `gui/`
      - `frames/GOFrame.h/.cpp` – GUI-Rahmen/Hauptfenster
    - `GOApp.cpp` – App-Einstieg/Anwendungslogik
  - `core/`, `rt/` – Laufzeit-/Echtzeitunterstützung
  - `portaudio/` – PortAudio-Integration/Adapter
  - `images/` – Ressourcen (Bilder/Icons)
  - `tests/`, `tools/` – Tests und Hilfsprogramme
- `resources/` – App-Ressourcen, `.rc`, `.xml` für App-Metadaten
- `help/` – Hilfeinhalte (XML, XSL, Bilder) und Übersetzungen
- `po/` – i18n-Übersetzungen (`.po`, `LINGUAS`)
- `docs/` – Projekt-Dokumentation
  - `CrossfadeIntegration.md` – Details zur Crossfade-Integration
  - `GrandOrgue-Odyssey.md` – Meta-Logbuch (Erkenntnisse, Fixes, Querverweise)
  - `GO_Project_Chats.md` – Konversationsmitschnitte (allgemeine Themen, Tools)
  - `GO_API_Context_Bundle.zip` – API/Projekt-Kontext-Bundle
  - `RepoMap.md` – diese Datei
- `packages/` – Beispiel-/Demo-Orgelpakete
- `perftests/` – Performance-Testdaten (`.wav` etc.)
- `submodules/` – Drittmodule (PortAudio, RtAudio, RtMidi, ZitaConvolver)
- `sounds/` – zusätzliche Klang-/Metronom-Daten

## Häufig gesuchte Dateien (Direktlinks)
- Build:
  - `build-scripts/for-win64/build-on-linux.sh`
  - `build-scripts/for-win64/update-build-on-linux.sh`
  - `build-scripts/for-linux/build-on-linux.sh`
- Audio Engine:
  - `src/grandorgue/sound/GOSoundEngine.h`
  - `src/grandorgue/sound/GOSoundEngine.cpp`
  - `src/grandorgue/sound/GOSoundProvider.h`
  - `src/grandorgue/sound/GOSoundProvider.cpp`
  - `src/grandorgue/sound/GOSoundFader.h`
  - `src/grandorgue/sound/GOSoundFader.cpp`
- Modell:
  - `src/grandorgue/model/GOPipe.h`
  - `src/grandorgue/model/GOSoundingPipe.h`
  - `src/grandorgue/model/GOSoundingPipe.cpp`
  - `src/grandorgue/model/pipe-config/GOPipeConfigNode.h`
- GUI/App:
  - `src/grandorgue/gui/frames/GOFrame.h`
  - `src/grandorgue/gui/frames/GOFrame.cpp`
  - `src/grandorgue/GOApp.cpp`

## Hinweise / Konventionen
- Code-Kommentare: Englisch (falls deutsche Kommentare im Code, bitte Nutzer fragen, ob Übersetzung gewünscht ist)
- Achtung: `*.organ` ist proprietäres Textformat, kein XML
- Cross-Build: MinGW unter Linux, Toolchain `mingw-toolchain.cmake` wird über Skripte referenziert
- Build-Artefakte:
  - Win64: `build-scripts/for-win64/build/`
  - Linux: `build-scripts/for-linux/build/`

## Bereichs-Maps (feingranular)
- docs/RepoMap/README.md – Index/Konventionen
- docs/RepoMap/SoundEngine.md – Audio-Pipeline, Voices, Fades, Cache-Integration (high-level)
- docs/RepoMap/SoundEngine-Full.md – Sound-Engine Vollanalyse (Scheduler, Sampler, Provider, I/O, Hotspots)
- docs/RepoMap/GUI.md – Frames, Panels, Dialoge, Controls
- docs/RepoMap/Settings.md – Konfigurationssystem, Parser/Writer, GUI-Tabs
- docs/RepoMap/ODFLoading.md – ODF/CMB/Archive-Load, Cache, Threads
- docs/RepoMap/ModelPipes.md – Orgelmodell, Ranks/Pipes, Switches/Enclosures/Tremulants (high-level)
- docs/RepoMap/ModelFull.md – Model Vollanalyse (Pipes, Ranks, Manuals, Stops, Couplers, Cache, Voice Lifecycle)
- docs/RepoMap/MIDI.md – MIDI-IO, Mapping, Player/Recorder, Ports
- docs/RepoMap/ArchivePackaging.md – .orgue Archive, Index, Installation, Paketbau
- docs/RepoMap/ControllerEvents.md – Event-Verteilung, Handler-Listen, Timer/Phasen

## Nächste Schritte
- Siehe Build-/Run-Cheatsheet: `docs/BuildCheatsheet.md`
- VSCode-Tasks verwenden (falls vorhanden): `.vscode/tasks.json`
