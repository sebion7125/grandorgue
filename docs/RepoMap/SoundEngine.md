# Sound-Engine – RepoMap

Kurzbeschreibung:
- DSP-/Audio-Pipeline für Echtzeit-Klangerzeugung, Stimmen-/Pipe-Verwaltung, Fades/Crossfades, Übergabe an Audio-IO.

## Überblick
- Hauptverantwortungen
  - Stimmenverwaltung (pro Pipe/Note), Mischen in Ausgabepuffer
  - Hüllkurven/Fades (z. B. Attack, Crossfade)
  - Pull-basiertes Bereitstellen von Samples für das Audio-Backend
- Abhängigkeiten (ein-/ausgehend)
  - Eingehend: Modell/Ereignisse (z. B. Note On/Off über Controller/Organ)
  - Ausgehend: Audio-IO (PortAudio/RtAudio Adapter), ggf. Faltung (ZitaConvolver) – siehe `submodules/`
  - Interne Parameter/Settings (Latenz, Puffergrößen, Crossfade-Mode/-Param)

## Schlüsseldateien/-verzeichnisse
- `src/grandorgue/sound/GOSoundEngine.h/.cpp` – Zentrale Sound-Engine (Initialisierung, Render-Loop/Callback, Stimmenmix)
- `src/grandorgue/sound/GOSoundProvider.h/.cpp` – Quelle/Provider für Audiodaten je Stimme/Pipe
- `src/grandorgue/sound/GOSoundFader.h/.cpp` – Fader/Hüllkurvenlogik (u. a. Ein-/Ausblendungen)
- `src/grandorgue/sound/GOCrossfadeMode.h` – Crossfade-Modi
- `src/grandorgue/sound/GOCrossfadeParam.h` – Parameter für Crossfades
- `src/grandorgue/sound/GO_Attack_Parameters.h/.cpp` – Attack-/Envelope-Parameter
- `src/grandorgue/sound/GOSoundAudioSection.cpp` – Segmentierung/Abschnitte im Audiopfad
- `src/grandorgue/sound/GO_DebugRelease.h` – Buildabhängige Schalter (ggf. Logging/Checks)
- Integration (nur Verweis):
  - Audio-Backend/Adapter: `src/portaudio/` (Binding zum Systemaudio)
  - Faltung/Hall: `submodules/ZitaConvolver/` (falls im Einsatz)

## Wichtige Klassen/Funktionen
- `GOSoundEngine` (…/GOSoundEngine.*): zentrale Verwaltung
  - Init/Start/Stop, Konfiguration von Pufferformaten/-größen
  - Render-/Process-Funktion (wird vom Audio-Backend im Callback-Kontext aufgerufen)
  - Stimmenlebenszyklus: Anlegen (NoteOn), Aktualisieren (pro Block), Freigeben (NoteOff/Release)
- `GOSoundProvider` (…/GOSoundProvider.*):
  - Liefert Rohdaten/Frames je Stimme (z. B. Sample Pull), ggf. Interpolation, Looping
- `GOSoundFader` (…/GOSoundFader.*):
  - Parameterisieren von Fades (Zielpegel/Zeit), pro Block anwenden
- `GOCrossfadeMode/Param`:
  - Ausprägung der Überblendung (Kurven, Zeiten), Feintuning
- `GO_Attack_Parameters`:
  - Einstellgrößen für Attack/Envelope, Interaktion mit Fader/Provider

## Datenflüsse und Zuständigkeiten
- Ereignisse (Note On/Off, Parameteränderungen) kommen aus dem Modell/Controller → Engine updated interne Voice-States
- Pro Audio-Callback:
  - Engine fordert Samples von Provider(n) je aktiver Stimme an
  - Hüllkurven/Fader anwenden, Mischen in Output-Puffer
  - Übergabe an Audio-Backend (PortAudio)
- Crossfade/Attack wirken auf Gain/Envelope je Stimme/Sektion

## Typische Aufrufketten (Call-Graph Skizze)
- Ereignisweg (vereinfacht):
  - Controller/Model → [NoteOn/Off] → `GOSoundEngine::{startVoice/stopVoice}` → Voice registriert → im nächsten Render-Block aktiv
- Audiopfad (vereinfacht):
  - AudioBackend Callback → `GOSoundEngine::renderBlock` → for voices: `GOSoundProvider::getSamples` → `GOSoundFader::apply` → Summe → Output

## Integration mit anderen Bereichen
- Modell: `src/grandorgue/model/` (z. B. `GOSoundingPipe.*`, `GOPipe.h`) liefert semantische Ereignisse/Zuordnung Pipe↔Voice
- GUI: Parameter-/Preset-Steuerung (z. B. Crossfade-Mode), Sicht auf Pegel/Last
- Settings: Latenz/Puffergrößen, Qualitäts-/Kurvenparameter
- ODF Loading: legt pro Pipe/Stimme Sample-Quellen/Parameter fest

## Open Questions / TODO
- Exakte Signatur und Ort des Audio-Callbacks in PortAudio-Schicht prüfen und verlinken
- Threading/Locking-Strategie im Renderpfad dokumentieren (XRuns vermeiden)
- Wo werden Latenz/Buffer-Settings gesetzt? (Settings-Map ergänzen und verlinken)
- Referenzen auf konkrete Funktions-/Zeilennummern ergänzen, sobald verifiziert

## Change-Log
- 2025-08-22: Erste Struktur angelegt (hohe Ebene, noch ohne Zeilenreferenzen)
