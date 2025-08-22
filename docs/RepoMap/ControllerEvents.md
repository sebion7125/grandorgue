# Controller / Events – RepoMap

Kurzbeschreibung:
- Ereignisverteilung und Lebenszyklen: MIDI-/UI-Ereignisse, Steuer- und Speicherobjekte, Sound-State-Handler. Zentrale Drehscheibe ist `GOEventDistributor` in Kombination mit `GOEventHandlerList`. Zeitgesteuerte Abläufe via `GOTimer`.

## Überblick
- Hauptverantwortungen
  - Sammeln und Verteilen von Ereignissen (MIDI, Tasten, States) an registrierte Handler
  - Verwaltung von speicherbaren Objekten, Cache-Objekten und Sound-Status-Handlern
  - Orchestrierung von Play/Record-Phasen (Prepare/Start/Abort) und Persistenz (Save/Read)
  - Zeitbasierte Callbacks für Player/Recorder/Metronom über `GOTimer`
- Abhängigkeiten (ein-/ausgehend)
  - Eingehend: GUI (z. B. `GOFrame`), MIDI (siehe MIDI-Map), ODF/Settings (Laden/Speichern)
  - Ausgehend: Model (Switches/Manuals/etc.), Sound-Engine (Start/Stop/Prepare), GUI-Labels/Status

## Schlüsseldateien/-verzeichnisse
- Verteiler/Listen:
  - `src/grandorgue/control/GOEventDistributor.h/.cpp` – zentrale Verteilung und Phasensteuerung
  - `src/grandorgue/model/GOEventHandlerList.h/.cpp` – Sammlung registrierter Handler/Objekte:
    - MIDI-Event-Handler, Sound-State-Handler, SaveableObjects, CacheObjects, ControlChangedHandlers
- Referenzen/Objekthilfen:
  - `src/grandorgue/model/GOReferencingObject.h/.cpp` – referenzierte Objekte, Auflösungsstatus
- Timer:
  - `src/core/GOTimer.h/.cpp`, `src/core/GOTimerCallback.h` – wiederverwendbarer Timer, Callback-Interface
- Integration im Controller:
  - `src/grandorgue/GOOrganController.cpp` – nutzt Distributor/HandlerList, setzt Timer/Player/Recorder

## Wichtige Klassen/Funktionen (Auswahl)
- `GOEventDistributor`
  - `SendMidi(const GOMidiEvent&)` – verteilt MIDI an registrierte MIDI-Handler
  - `HandleKey(int)` – verteilt Tastendrücke
  - `ReadCombinations(GOConfigReader&)` / `Save(GOConfigWriter&)` – Persistenz der SaveableObjects
  - `ResolveReferences()` / `UpdateHash(GOHash&)` – Auflösung und Fingerabdruck der CacheObjects
  - Playback/Record-Phasen:
    - `PreparePlayback(GOSoundEngine*)`, `StartPlayback()`, `AbortPlayback()`, `PrepareRecording()`
- `GOEventHandlerList`
  - Liefert Vektoren/Container der registrierten Objekte (MIDI-Handler, SoundStateHandler, Saveable/CacheObjects, Controls…)
  - `Cleanup()` – räumt u. a. Cache-Objekte frei
  - `SendControlChanged(GOControl*)` – benachrichtigt Control-Änderungen
- `GOTimer` / `GOTimerCallback`
  - Setzt relative/absolute Timer, aktualisiert Intervalle, verwaltet Callback-Liste
  - Wird u. a. von `GOMetronome`, `GOMidiPlayer`, `GOMidiRecorder`, `GOAudioRecorder` verwendet

## Datenflüsse und Zuständigkeiten
- Eventfluss (MIDI):
  - MIDI-In → `GOEventDistributor::SendMidi` → Iterate über `GetMidiEventHandlers()` → Model/Controls reagieren
- Steuer-/Speicherfluss:
  - Laden: `GOEventDistributor::ReadCombinations(cfg)` → Iterate über `GetSaveableObjects()`
  - Speichern: `GOEventDistributor::Save(cfg)` → Iterate über `GetSaveableObjects()`
- Cache-/Referenzen:
  - `ResolveReferences()` / `UpdateHash(hash)` → Iterate über `GetCacheObjects()` (z. B. Pipes/Ressourcen)
- Playback/Recording:
  - `PreparePlayback(engine)` → Iterate über `GetSoundStateHandlers()` (z. B. Engine/Provider) → `StartPlayback()`
  - `AbortPlayback()` stoppt Systematisch alle SoundStateHandler
  - `PrepareRecording()` initialisiert Recorder-Seite

## Typische Aufrufketten (Skizze)
- Start Wiedergabe:
  - Controller: `PreparePlayback(engine, midi, recorder)` → `GOEventDistributor::PreparePlayback(engine)` → `StartPlayback()` → MIDI-Player Setup → ggf. Timer-Events treiben Playback
- Laden/Speichern:
  - Laden: `GOEventDistributor::ReadCombinations(cfg)` → SaveableObjects laden → `OnCombinationsLoaded(...)` im Setter
  - Speichern: `GOEventDistributor::Save(cfg)` → SaveableObjects persistieren
- MIDI-Live:
  - RtMidi Callback → (siehe MIDI-Map) → `GOEventDistributor::SendMidi(event)` → Handlers reagieren (Switches/Manuals/…)
- Timer:
  - `GOTimer::Notify()` → ruft registrierte `GOTimerCallback`s → z. B. Player/Recorder/Metronome Ticks

## Integration mit anderen Bereichen
- GUI: triggert Events (Menü, Shortcuts), empfängt Status (Labels/Progress)
- MIDI: liefert Events; Player/Recorder nutzen Timer über Controller
- Model: liefert Handler-Listen; reagiert auf verteilte Ereignisse
- Sound-Engine: SoundStateHandler für Start/Abort/Prepare; tatsächliche Audio-Verarbeitung ist separat (siehe SoundEngine-Map)
- ODF/Settings: Read/Save/Resolve flow über Config-Reader/Writer

## Open Questions / TODO
- Datei:Zeile-Referenzen via clangd ergänzen:
  - `GOEventDistributor::{SendMidi,PreparePlayback,StartPlayback,AbortPlayback,Save,ReadCombinations,ResolveReferences,UpdateHash}`
  - `GOEventHandlerList::{GetMidiEventHandlers,GetSoundStateHandlers,GetSaveableObjects,GetCacheObjects,Cleanup}`
- Lebenszyklus/Besitzverhältnisse: wer registriert/deregistriert Handler genau? (Controller/Model)
- Threading-Aspekte: Aufrufkontext der Handler (GUI-Thread vs. Audio/MIDI-Callback), Locking-Strategie

## Clangd-Index-Hinweise
- Startpunkte:
  - Suche nach `class GOEventDistributor` und “for (auto handler …)” Schleifen
  - Referenzen auf `GOEventHandlerList` in `GOOrganModel.h` (vererbt `GOEventHandlerList`)
  - Fundstellen von `GOTimerCallback`-Implementoren (`GOMetronome`, `GOMidiPlayer`, `GOMidiRecorder`, `GOAudioRecorder`)
- Datei:Zeile nachtragen, um Sprungpunkte direkt in die Map zu setzen

## Change-Log
- 2025-08-22: Erste Fassung (High-Level), konkrete Zeilenverweise via clangd ausstehend
