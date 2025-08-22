# Settings/Configuration – RepoMap

Kurzbeschreibung:
- Persistente Applikations- und Organ-bezogene Einstellungen (Audio, MIDI, Paths, Organs, Reverb/Temperaments, Optionen). GUI-Dialoge schreiben/lesen über Konfig-API, Parser/Writer kümmern sich um Datei-Formate.

## Überblick
- Hauptverantwortungen
  - Laden/Speichern globaler Settings (Audio-Backend, Geräte, Puffer, MIDI, Pfade, Optionen)
  - Organ-registrierung/Verwaltung (Liste bekannter Organe/Packages)
  - Schreiben/Lesen von Konfig-/INI-artigen Dateien (inkl. ODF-Unterstützung)
- Abhängigkeiten (ein-/ausgehend)
  - Eingehend: GUI-Dialoge (Settings-Tabs) → setzen/lesen Werte
  - Ausgehend: Audio-/MIDI-Subsysteme (z. B. Gerätezuordnung), Controller/Model (Organ-Registrierungen)
  - Parser/Writer im Core lesen/schreiben Textdateien (INI/ähnlich, proprietär für ODF)

## Schlüsselverzeichnisse/-dateien
- Applikationsnahe Configs: `src/grandorgue/config/`
  - `GOConfig.h/.cpp` – Zentrale Konfigurationszugriffe (App-weite Settings)
  - `GOAudioDeviceConfig.*`, `GOAudioDeviceNode.*` – Audio-Geräte/Nodes
  - `GOMidiDeviceConfig.*`, `GOMidiDeviceConfigList.*` – MIDI-Geräte/Listen
  - `GOPortsConfig.*`, `GOPortFactory.*` – Port-/Device-Mapping
  - `GODeviceNamePattern.*` – Gerätedetektion via Namensmuster
  - `GORegisteredOrgan.*` – Verwaltung registrierter Orgeln/Packages auf App-Ebene
- Parser/Writer: `src/core/config/`
  - `GOConfigReader.*`, `GOConfigWriter.*` – generische Lese-/Schreib-API
  - `GOConfigFileReader.*`, `GOConfigFileWriter.*` – dateibasierte Reader/Writer (INI-artig)
  - `GOConfigEnum.*`, `GOConfigReaderDB.*` – Hilfen/DB-gestützte Reader, Enums

- GUI-Settings-Dialoge: `src/grandorgue/gui/dialogs/settings/`
  - `GOSettingsDialog.*` – hostet Tabs
  - Tabs: `GOSettingsAudio.*`, `GOSettingsMidiDevices.*`, `GOSettingsPaths.*`, `GOSettingsOrgans.*`, `GOSettingsReverb.*`, `GOSettingsTemperaments.*`, `GOSettingsOptions.*`, `GOSettingsDeviceMatchDialog.*`, `GOSettingsPorts.*`, `GOSettingsMidiInitial.*`, `GOSettingsMidiDeviceList.*`

## Wichtige Klassen/Funktionen (Auswahl)
- `GOConfig` (…/grandorgue/config/GOConfig.*)
  - Zentrale Schnittstelle für App-Settings; bietet get/set und Persistenz-Anbindung
- Audio/MIDI
  - `GOAudioDeviceConfig`, `GOAudioDeviceNode` – Auswahl/Topologie von Audio-Geräten
  - `GOMidiDeviceConfig`, `GOMidiDeviceConfigList` – MIDI-Gerätekonfiguration und Listenverwaltung
  - `GOPortsConfig`, `GOPortFactory` – Ports/Zuordnung/Erzeugung
- Parser/Writer (Core)
  - `GOConfigReader/Writer` – Abstraktion für Konfig-Zugriff
  - `GOConfigFileReader/Writer` – Datei-IO (INI-artig) inkl. ODF-Unterstützung
- Registrierungen
  - `GORegisteredOrgan` – Eintrag/Metadaten registrierter Orgeln (Verknüpfung mit Archive/Packages)

## Datenflüsse und Zuständigkeiten
- GUI-Änderungen:
  - `GOSettingsDialog` (Tab X) → ruft Setter in `GOConfig`/spezifischen Config-Klassen
  - Persistenz: `GOConfigWriter`/`GOConfigFileWriter` schreibt auf Platte (Format/Ort projekt-/plattformabhängig)
- App-Start:
  - `GOConfigReader`/`GOConfigFileReader` lädt bestehende Settings → initialisiert Audio/MIDI/Paths
- Organ-Registrierung:
  - `GOSettingsOrgans`/Controller pflegt Registrierungen → `GORegisteredOrgan` + Archive-Index

## Typische Aufrufketten (Skizze)
- Settings ändern/speichern:
  - `GOSettingsDialog::OnApply` → `GOConfig::Set…` (oder spezifische Config-Klasse) → `GOConfigWriter::Write(...)` → Datei aktualisiert
- Laden beim Start:
  - App init → `GOConfigReader::Read(...)` → Felder setzen → Audio/MIDI/Paths reinitialisieren
- Ports/Device-Match:
  - Dialog öffnet DeviceMatch → Benutzer ordnet zu → `GOPortsConfig`/`GOPortFactory` aktualisiert → wirksam nach Apply/Restart bestimmter Subsysteme

## Integration mit anderen Bereichen
- GUI: Settings-Dialoge sind Frontend
- Sound-Engine: liest Audio-Parameter (Samplerate, Buffer, Device) aus Config
- ODF Loading/Archive: Pfade/Registrierungen werden in Settings verwaltet/gespeichert
- Model/Controller: nutzt Settings für Laufzeitentscheidungen (z. B. aktive Orgeln, Pfade)

## Open Questions / TODO
- Exakte Persistenzpfade je Plattform (User/AppData vs. Projektverzeichnis) verlinken
- Welche Settings erfordern Live-Reinit vs. App-Neustart? (Audio/MIDI)
- Konkrete Setter/Getter-Namen in `GOConfig` und Tab-Handler via clangd-Referenzen ergänzen
- DeviceMatch-Fluss: Welche Klassen lösen tatsächliches Binding aus?

## Clangd-Index-Hinweise
- Nutze “Gehe zu Definition/Referenz” auf:
  - `GOSettingsDialog::OnApply` (und vergleichbare Apply/OK-Handler der Tabs)
  - `GOConfig::Set...`/`Get...` Methoden
  - `GOConfigWriter::Write`, `GOConfigReader::Read`
- In die Map nachpflegen: Datei:Zeile für zentrale Übergabepunkte

## Change-Log
- 2025-08-22: Erste Struktur anhand Dateiliste erstellt; Zeilenlinks noch zu ergänzen
