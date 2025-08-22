# GUI – RepoMap

Kurzbeschreibung:
- wxWidgets-basierte Oberfläche: Frames, Panels, Dialoge und custom Controls für Bedienung, Visualisierung und Parametrisierung.

## Überblick
- Hauptverantwortungen
  - Hauptfenster/Frames (Start, Menü, Log, Stops)
  - Panels für Orgelbedienung (Manuale, Stops, Koppeln, Sequencer/Recorder, Metering)
  - Dialoge (Einstellungen, Organ-Auswahl, Fortschritt, Eigenschaften)
  - Custom wxControls (Grids, Gauges, File/Dir-Picker etc.)
- Abhängigkeiten (ein-/ausgehend)
  - Eingehend: Model-/Controller-Events (z. B. Änderungen im Orgelmodell)
  - Ausgehend: Aufrufe an `GOOrganController` (Laden/Ändern), ggf. Audio-Ansichten (Pegel)

## Schlüsselverzeichnisse
- `src/grandorgue/gui/frames/`
  - `GOFrame.h/.cpp` – Hauptfenster, Menü/Toolbar, Orgel-Ladepfad, zentrale Event-Verteilung
  - `GOStopsWindow.*`, `GOLogWindow.*`, `GOMainWindowData.*` – Teilfenster/Logik
- `src/grandorgue/gui/panels/`
  - Umfangreiche Panel-Sammlung (Manuale, Coupler, Crescendo, Enclosures, Master, Recorder/Sequencer, Anzeige-Metriken)
  - `primitives/` – Zeichen- und Font-Primitiven (`GOBitmap.*`, `GODC.*`, `GOFont.*`)
- `src/grandorgue/gui/dialogs/`
  - Allgemeine Dialoge (`GOSelectOrganDialog.*`, `GOProgressDialog.*`, `GOPropertiesDialog.*`, `GOSplash.*`, Message-Boxes)
  - `midi-event/` – MIDI-Event-Dialoge/Tabs
  - `organ-settings/` – Organ-spezifische Settings-Tabs (Pipes, Enclosures)
  - `settings/` – Applikationsweite Settings (Audio, MIDI, Device-Matching, Paths, Reverb, Temperaments, Organs, Options)
- `src/grandorgue/gui/wxcontrols/`
  - Custom Controls (`GOAudioGauge.*`, `GOGrid.*`, Picker-Controls, Choice)
- `src/grandorgue/gui/size/`
  - Größen-/Layout-Helfer (Resizable, SizeKeeper, LogicalRect)

## Wichtige Klassen/Funktionen (Auswahl)
- Frames
  - `GOFrame` (frames/GOFrame.*): Einstiegspunkt der GUI; Menühandlungen (z. B. Organ laden, Reset), Einbettung der Panel-Ansichten, Status/Log
  - `GOStopsWindow`, `GOLogWindow`, `GOMainWindowData`
- Panels
  - `GOGUIMasterPanel`, `GOGUICouplerPanel`, `GOGUIEnclosure`, `GOGUIManual`, `GOGUIRecorderPanel`, `GOGUISequencerPanel`, `GOGUIDisplayMetrics`, `GOGUIBankedGeneralsPanel`, …
  - Layout/Engine: `GOGUILayoutEngine.*`
- Dialoge – Organ
  - `GOSelectOrganDialog` – ODF/Package-Auswahl (Integration mit ODF Loading)
  - `GOPropertiesDialog` – Eigenschaften der geladenen Orgel
  - Progress/UI: `GOProgressDialog`, `GOSplash`, `go-message-boxes`
- Dialoge – Settings
  - `GOSettingsDialog` – Sammler für Tabs: Audio, MIDI, Paths, Organs, Reverb, Temperaments, Options, DeviceMatch
  - Applikationsweite Einstellungen; organ-spezifische unter `organ-settings/`
- Controls
  - `GOAudioGauge` – Pegelanzeige
  - `GOGrid` – Tabellenanzeige
  - Picker/Choice – Dateiauswahl, Pfadwahl

## Datenflüsse und Zuständigkeiten
- Benutzeraktion (Menü/Panel) → `GOFrame`/Panel-Handler → `GOOrganController` → Modelländerung/Load → Rückmeldung/Events → UI aktualisiert
- Organ-Laden:
  - `GOSelectOrganDialog` liefert Auswahl → Controller startet ODF-/Package-Load → `GOProgressDialog` zeigt Fortschritt → Panels/Frames re-binden Daten
- Settings:
  - `GOSettingsDialog` schreibt via Controller/Config in Persistenz → Neustart/Neuinitialisierung beeinflusst Audio-/MIDI-/Pfad-bezogene Komponenten

## Typische Aufrufketten (vereinfachte Skizze)
- Organ öffnen:
  - `GOFrame::OnOpenOrgan` → `GOSelectOrganDialog` → Auswahl → `GOOrganController::Load(...)` → ProgressDialog → Rebuild UI
- Organ Reset zu Defaults:
  - `GOFrame` Menu → Warnung (MessageBox mit Hinweis auf “organ definition file”) → Controller reset/reload → UI refresh
- Settings ändern:
  - `GOSettingsDialog` (Tab) → Apply → Controller/Config Update → ggf. Audio Reinit → UI-Status aktualisiert

## Integration mit anderen Bereichen
- ODF Loading: Auswahl-Dialog und Progress steuern den Ladevorgang; Ergebnisse erscheinen in Panels
- Sound-Engine: Anzeige von Pegeln (`GOAudioGauge`), Start/Stop-States, ggf. Latenz/Buffer-Infos in Settings-Ansichten
- Settings: `GOSettings*`-Tabs liefern Parameter in Config/Persistenz; Device-Matching-Sichten
- Model: Panels spiegeln Model-States (Stops, Coupler, Divisionals etc.)

## Open Questions / TODO
- Exakte Handler-Namen in `GOFrame` für Open/Reset referenzieren (Zeilenlink via clangd ergänzen)
- Panel-Lebenszyklus/Owner-Beziehungen dokumentieren (Wer hält wen?)
- Event-/Observer-Mechanik zwischen Controller↔GUI präzisieren (Signals/Custom Events?)
- Welche Panels sind lazily created vs. persistent?

## Change-Log
- 2025-08-22: Erste Struktur angelegt (High-Level, ohne Zeilenreferenzen)
