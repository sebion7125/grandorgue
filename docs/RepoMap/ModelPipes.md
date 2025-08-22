# Model / Pipes – RepoMap

Kurzbeschreibung:
- Orgelmodell: Ranks, Pipes, Enclosures, Switches, Tremulants, Manuals; Zustände und Steuerlogik, die GUI/MIDI in Klangerzeugung überführen.

## Überblick
- Hauptverantwortungen
  - Repräsentation der Orgelstruktur (Manuale, Register/Stops, Ranks, Pipes)
  - Zustandsverwaltung (an/aus, Koppler, Tremulanten, Enclosures)
  - Konfigurationsübernahme aus ODF (Pipe-/Rank-Parameter)
- Abhängigkeiten (ein-/ausgehend)
  - Eingehend: Controller (Ereignisse/Kommandos), ODF-Parser (Parameter)
  - Ausgehend: Sound-Engine (aktive Stimmen/Pipes), GUI (Status/Labels)

## Schlüsseldateien/-verzeichnisse
- Kern-Pipes/Ranks:
  - `src/grandorgue/model/GOPipe.h` – Basisklasse Pipe
  - `src/grandorgue/model/GOSoundingPipe.h/.cpp` – klingende Pfeife (Ton-Erzeugung)
  - `src/grandorgue/model/GORank.h` – Zuordnung Pipes → Rank
- Struktur/Elemente:
  - `src/grandorgue/model/GOManual.h` – Manuale/Pedale
  - `src/grandorgue/model/GOEnclosure.h` – Schwellwerke/Enclosures
  - `src/grandorgue/model/GOSwitch.h` – Schalter/Stops
  - `src/grandorgue/model/GOTremulant.h` – Tremulant
  - Koppler: `src/grandorgue/model/GOCoupler.h`, `src/grandorgue/model/GODivisionalCoupler.h`
- Pipe-Konfiguration:
  - `src/grandorgue/model/pipe-config/GOPipeConfigNode.h` – Konfig-Knoten für Pipes
- Controller-Anbindung:
  - `src/grandorgue/GOOrganController.cpp` – lädt Model aus ODF, erstellt Panels/Elemente

## Wichtige Klassen/Funktionen (Auswahl)
- `GOPipe` – Basisschnittstelle; Zustände/Parameter je Pfeife
- `GOSoundingPipe` – Tonerzeugungsspezifika (Attack/Release, ggf. Samples via Provider)
- `GORank` – Gruppierung/Zuordnung vieler Pipes (Registerlage)
- `GOManual` – Tasten/Key-Routing pro Manual
- `GOEnclosure` – Lautstärke-/Filterbeeinflussung über Hüllkurven/Controller
- `GOSwitch`/`GOCoupler` – (De-)Aktivieren von Klangquellen/Verbindungen
- `GOPipeConfigNode` – strukturierte ODF-Parameter eines Pipe-Baums

## Datenflüsse und Zuständigkeiten
- ODF Load:
  - ODF → `GOOrganController::ReadOrganFile` → `GOOrganModel::Load(cfg)` → erstellt Model-Objekte (Pipes, Ranks, …)
- Laufzeit:
  - GUI/MIDI → Controller → Model (Switch/Coupler/Tremulant/Manual State) → Engine zieht Samples von zugeordneten Stimmen/Pipes
- Parameter:
  - Pipe-/Rank-Parameter aus `GOPipeConfigNode` prägen Tonhöhen, Hüllkurven, Gruppierungen

## Typische Aufrufketten (Skizze)
- Note-On:
  - GUI/MIDI Event → Controller/Model: Manual-Key on → relevante `GOSwitch`/`GOCoupler` setzen → betroffene `GOSoundingPipe` aktiv → Engine mischt Stimme
- Stop aktivieren:
  - GUI Stop → `GOSwitch::Set(true)` → betroffene `GORank`/Pipes werden berücksichtigt

## Integration mit anderen Bereichen
- Sound-Engine: zieht pro aktiver Pipe Samples/Frames (Provider/Fader greifen)
- GUI: zeigt Model-Status (Stops, Enclosures), triggert Umschaltungen
- ODF Loading: initialisiert Struktur/Parameter

## Open Questions / TODO
- Exakte Übergabepunkte Pipe→Engine (Voice-Lifecycle) mit Datei:Zeile verlinken
- Welche Klassen erzeugen/halten Voice-Objekte konkret?
- Detaillierte Koppler-Logik (Divisional/General) mit Referenzen dokumentieren

## Clangd-Index-Hinweise
- “Gehe zu Def/Ref” auf:
  - `GOSoundingPipe` zentrale Methoden (Tonstart/-ende)
  - `GOOrganModel::Load` Erzeugung des Modells
  - Koppler/Enclosure-Setter
  - Controller-Stellen, die Pipes aktivieren/deaktivieren

## Change-Log
- 2025-08-22: Erste Struktur angelegt (High-Level, Zeilenlinks offen)
