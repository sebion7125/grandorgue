# ODF Loading (*.organ) – RepoMap

Kurzbeschreibung:
- Laden und Parsen von ODFs (proprietäres .organ-Textformat), optional kombiniert mit .cmb (gespeicherten Einstellungen) und/oder Orgel-Paketen (.orgue). Verteilen der Ladeobjekte, paralleles Laden, optionales Cache-Handling.

## Überblick
- Hauptverantwortungen
  - ODF (.organ) + optional .cmb einlesen, Mergen relevanter Sektionen
  - Aus Archiven (.orgue) lesen, inkl. `organindex.ini`
  - Initialisierung des Orgelmodells und GUI-Panels
  - Laden/Erzeugen von Cache-Dateien für schnelleren Start
- Abhängigkeiten (ein-/ausgehend)
  - Eingehend: GUI/Controller (z. B. `GOFrame` → `GOOrganController::Load`)
  - Ausgehend: Model (Pipes/Ranks etc.), Sound-Engine (später), Cache (rw)
  - Core-Config Parser/Writer; Archive-Manager; Loader/Worker/Threads

## Schlüsseldateien/-verzeichnisse
- Controller/Eintrittspunkt:
  - `src/grandorgue/GOOrganController.cpp`
    - `GOOrganController::Load(...)` – zentraler Einstieg in den ODF-Ladevorgang
    - `GOOrganController::ReadOrganFile(GOConfigReader &cfg)` – Basismetadaten
- Loader/Dateizugriff/Parallelisierung:
  - `src/grandorgue/loader/GOFileStore.*` – Quelle (Verzeichnis vs. Archiv), Datei-IO
  - `src/grandorgue/loader/GOLoaderFilename.*` – Pfadauflösung (relativ/absolut, Archive)
  - `src/grandorgue/loader/GOCacheObjectDistributor.h`, `GOObjectDistributor.h`
  - `src/grandorgue/loader/GOLoadWorker.*`, `GOLoadThread.*` – serielles/paralleles Laden von Objekten
  - Cache: `src/grandorgue/loader/cache/GOCache.*`, `GOCacheWriter.*`, `GOCacheCleaner.*`
- Archive-Verwaltung:
  - `src/core/archive/GOArchiveManager.*`, `GOArchiveFile.*`, `GOArchiveCreator.*`
    - Paketformat `.orgue` inkl. `organindex.ini`, Abhängigkeiten, Registrierung
- Parser/Config:
  - `src/core/config/GOConfigFileReader.*`, `GOConfigReaderDB.*`, `GOConfigReader.*`
  - `src/core/config/GOConfigFileWriter.*`, `GOConfigWriter.*` (Export/Speichern)
- Dateinamen/Patterns:
  - `src/core/files/GOStdFileName.*` – Hash-basierte Namen, Patterns (organ/index/setting/cache)

## Wichtige Klassen/Funktionen (Ausschnitt)
- `GOOrganController::Load(...)`
  - Entscheidet: Archiv (.orgue) vs. Datei-System; nutzt `GOFileStore`
  - Liest `.organ` via `GOConfigFileReader` → `GOConfigReaderDB` → `GOConfigReader`
  - Merget optional `.cmb` (gespeicherte Einstellungen) in CMB-Sektion
  - Initialisiert Model/GUI (Panels, Setter, Labels), optional startet Objektladung
  - Steuert Cache-Workflow (prüfen/lesen/aktualisieren) und Threads
- `GOConfigFileReader`, `GOConfigReaderDB`, `GOConfigReader`
  - Kaskade zum Parsen/Normalisieren von ODF/CMB-Inhalten, Zugriff auf Sektionen/Schlüssel
- `GOArchiveManager::ReadIndex(...)`
  - Liest `organindex.ini` aus `.orgue`, registriert Organe/Abhängigkeiten
- `GOLoadWorker`/`GOLoadThread`
  - Laden von Samples/Objekten (Pipes etc.) parallelisiert über `GOCacheObjectDistributor`

## Datenflüsse und Zuständigkeiten
- Organ wählen (GUI) → `GOOrganController::Load(dlg, organ, ...)`
  - Archiv? → `GOFileStore.LoadArchives(...)` + `GOArchiveManager` indiziert
  - ODF lesen (`GOConfigFileReader`) + optional `.cmb` mergen
  - `GOOrganController::ReadOrganFile(cfg)` liest Metadaten u. GUI-relevante Felder
  - Model laden (`GOOrganModel::Load(cfg)`), Panels/Setter/Recorder etc. aufbauen
- Optionales Audio/Caching:
  - Objektliste via `GOCacheObjectDistributor(GetCacheObjects())`
  - Cache prüfen/lesen (`GOCache`), bei Bedarf laden (`GOLoadWorker` + `GOLoadThread`)
  - Nach erfolgreichem Laden optional Cache schreiben (`GOCacheWriter`)
- Export/Speichern:
  - `GOOrganController::Export(...)` schreibt `.cmb` via `GOConfigWriter`/`GOConfigFileWriter`

## Typische Aufrufkette (vereinfachte Skizze)
- GUI → Controller:
  - `GOFrame::OnOpenOrgan` → `GOOrganController::Load(...)`
- Laden/Mergen:
  - `.organ` lesen (ODF) → optional `.cmb` lesen → `GOConfigReaderDB::ReadData`
  - `GOConfigReader` als vereinheitlichte Sicht
- Model/GUI:
  - `GOOrganModel::Load(cfg)` → `m_panelcreators[i]->CreatePanels(cfg)` → `GOGUIPanel::Load(...)`
- Cache/Samples:
  - Prüfe Cache-Datei → wenn gültig, aus Cache lesen; sonst:
    - `GOLoadWorker::LoadNextObject` + N×`GOLoadThread::Run` (parallel)
    - Fortschritt via `GOProgressDialog`
    - Bei Erfolg: `GOOrganController::UpdateCache(...)`

## Integration mit anderen Bereichen
- Settings/Config: Pfade, ODFCheck/HW1Check-Flags, Preset-Nummern, Cache-Policy
- GUI: Fortschritt/Dialogs, Organ-Auswahldialog, Panels rekonstruiert
- Sound-Engine: wird nach erfolgreichem Model/ODF-Laden vorbereitet (separater Schritt)
- Archive: `.orgue`-Pakete, Abhängigkeiten, Index-Datei

## Open Questions / TODO
- Zeilen-/Symbol-Referenzen (clangd) in diese Map einpflegen:
  - `GOOrganController::Load`, `ReadOrganFile`
  - `GOArchiveManager::ReadIndex`
  - `GOLoadWorker::LoadNextObject`, `GOLoadThread::Run`
- Detaillierte Liste der von `GetCacheObjects()` gelieferten Objekttypen ergänzen
- Threading-/Exception-Strategie dokumentieren (siehe `GOLoadAborted`, OOM-Handling)

## Clangd-Index-Hinweise
- Nutze “Gehe zu Definition/Referenzen” für die oben genannten Symbole und füge Datei:Zeile in diese Map ein
- Prüfe Aufrufer-Ketten von `GOOrganController::Load` (GUI/Events) und `GOLoadWorker`

## Change-Log
- 2025-08-22: Erste Fassung anhand Controller/Loader/Archive/Config-Quelltext erstellt; Zeilenlinks offen
