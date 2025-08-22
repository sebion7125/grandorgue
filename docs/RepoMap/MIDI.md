# MIDI – RepoMap

Kurzbeschreibung:
- MIDI-Ein-/Ausgabe, Mapping, Playback/Recording. Ereignisse werden normalisiert, gemappt und an Controller/Model weitergeleitet oder aus Dateien abgespielt. Ports kapseln RtMidi/Backend.

## Überblick
- Hauptverantwortungen
  - Eingang: Mehrere MIDI-Quellen mergen, Normalisierung in interne Events
  - Mapping: Zuordnung externer Messages auf interne Aktionen/Objekte
  - Ausgabe: Senden von MIDI (Playback, Feedback, Recorder)
  - Dateiformate: MIDI-File-Lesen für Playback/Import
- Abhängigkeiten (ein-/ausgehend)
  - Eingehend: OS-/RtMidi-Ports
  - Ausgehend: `GOOrganController`/`GOEventDistributor` (Spielaktionen), GUI (Status), ggf. Recorder/Player

## Schlüsselverzeichnisse/-dateien
- Core-Objekte:
  - `src/grandorgue/midi/GOMidi.*` – zentrale MIDI-Steuerung/Facade
  - `src/grandorgue/midi/GOMidiMap.*` – Mapping-Definitionen
  - `src/grandorgue/midi/GOMidiListener.*` – Listener/Callback-Ebene
  - Merger: `GOMidiInputMerger.*`, `GOMidiOutputMerger.*`
- Playback/Recording:
  - `GOMidiPlayer.*`, `GOMidiPlayerContent.*` – Abspielen
  - `GOMidiRecorder.*` – Aufzeichnen, SampleSet-IDs, Preconfig
- Events (Normalisierung/Pattern):
  - `events/GOMidiEvent.*`, `GOMidiCallback.h`
  - Pattern/Match: `GOMidiEventPattern.*`, `GOMidiReceiverEventPattern.*`, `GOMidiSenderEventPattern.*`, `GOMidiShortcutPattern.*` inkl. Typ-/Message-Enums
  - WX-Events: `GOMidiWXEvent.*`
  - Rodgers-spezifisch: `events/GORodgers.*` (Hersteller-spezial)
- Elemente/Objekte:
  - `elements/GOMidiReceiver.*`, `elements/GOMidiSender.*`, `elements/GOMidiSendProxy.*`, `elements/GOMidiShortcutReceiver.*`
  - `objects/GOMidiObject*.{h,cpp}` – Basisklassen und Varianten (WithDivision, WithShortcut, Playing, ReceivingSending, Sending)
- Ports / Backend:
  - `ports/GOMidiPort.*`, `GOMidiInPort.*`, `GOMidiOutPort.*`, `GOMidiPortFactory.*`
  - RtMidi: `ports/GOMidiRt*.{h,cpp}` – konkrete Port-Implementierungen
- Dateien:
  - `files/GOMidiFileReader.*`, `files/GOMidiFile.*` – Lesen/Parsen von MIDI-Dateien

## Wichtige Klassen/Funktionen (Auswahl)
- `GOMidi` – orchestriert Ports/Listener/Merger, bindet Player/Recorder
- `GOMidiMap` – definiert Zuordnung von eingehenden Events → Aktionen/Objekte
- `GOMidiPlayer`/`GOMidiRecorder` – Playback/Recording-Pipeline
- `GOMidiEvent` – internes Eventformat inkl. Gerät/Kanal/Typ
- Ports/Factories – Backends (RtMidi) anlegen/öffnen, IO-Kanal

## Datenflüsse und Zuständigkeiten
- Input:
  - OS/RtMidi → `GOMidiInPort` → `GOMidiInputMerger` → `GOMidiListener` → Mapping → `GOEventDistributor::SendMidi` → Controller/Model
- Playback:
  - `GOMidiPlayer` lädt Datei → schedult Events → über Out-Port/Distributor an Engine/Model
- Recording:
  - `GOMidiRecorder` registriert Mapping/IDs → schreibt Events/Spuren (ggf. in GO-spezifisches Format)
- Mapping:
  - Patterns (Receiver/Sender/Shortcut) matchen Roh-Events → lösen interne Aktionen aus (Stops, Keys, Couplers etc.)

## Typische Aufrufketten (Skizze)
- Live-Eingang:
  - RtMidi Callback → `GOMidiInPort` → `GOMidiInputMerger` → `GOMidiListener::OnEvent` → `GOMidiMap::Dispatch` → `GOEventDistributor::SendMidi(event)`
- Playback:
  - `GOOrganController::PreparePlayback` setzt `m_midi`/Recorder → `GOMidiPlayer::Setup` → Timer/Loop → Events an Out-Port/Distributor
- Recording:
  - `GOMidiRecorder::PreconfigureMapping` (IDs/Manuale) → bei Events mitschreiben → Export

## Integration mit anderen Bereichen
- Controller/Events: `GOEventDistributor` ist Brücke von MIDI zu Model/Engine
- GUI: MIDI-Konfig-Dialoge (siehe GUI-Maps) steuern Ports/Mapping
- Settings: Ports/Geräte/Pfade/Prioritäten persistieren; Recorder-Ausgabeziele
- Sound-Engine: indirekt über Controller/Distributor; direkte Audio-IO ist separat

## Open Questions / TODO
- Zentrale Registrierungs-/Bindestelle Port↔Listener verlinken (Datei:Zeile)
- Detailliertes Mapping-Beispiel mit realem Pattern ergänzen
- Recorder-Format/Exportpfade mit Settings verknüpfen und referenzieren

## Clangd-Index-Hinweise
- Referenzen auf:
  - `GOOrganController::PreparePlayback` (setzt MIDI/Recorder, Start)
  - `GOMidiListener::OnEvent`/`GOMidiMap::Dispatch`
  - `GOMidiPlayer::LoadFile/Setup`
  - PortFactory/Port-Open-Pfade
- Datei:Zeile an den o. g. Stellen in diese Map einpflegen

## Change-Log
- 2025-08-22: Erste Fassung; konkrete Zeilenverweise via clangd nachtragen
