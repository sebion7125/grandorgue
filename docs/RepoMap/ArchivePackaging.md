# Archive / Packaging – RepoMap

Kurzbeschreibung:
- Verwaltung von Orgel-Paketen (.orgue) inkl. Index (`organindex.ini`), Installation/Registrierung und Erzeugung neuer Pakete. Bindeglied zwischen ODFs, Ressourcen und Cache. Paketierung/Distribution via CMake/CPack und Build-Skripte.

## Überblick
- Hauptverantwortungen
  - Lesen/Indexieren von .orgue-Archiven, Abhängigkeitsprüfung
  - Installation/Registrierung in lokale Archive-Liste
  - Zugriff auf ODFs/Dateien innerhalb eines Pakets
  - Erstellen von Paketen inkl. Index/Metadaten
- Abhängigkeiten (ein-/ausgehend)
  - Eingehend: Controller/Loader (z. B. beim ODF-Laden)
  - Ausgehend: GOOrganList (registrierte Organe/Archive), FileStore (Dateizugriff), Config/Parser

## Schlüsselverzeichnisse/-dateien
- Archive-Manager/Files:
  - `src/core/archive/GOArchiveManager.*` – Öffnen/Indexieren/Installieren von Paketen
  - `src/core/archive/GOArchiveFile.*` – Repräsentiert installierte/registrierte Pakete
  - `src/core/archive/GOArchiveReader.cpp` / `...Writer.h` – Low-Level Lese-/Schreib-Helfer
  - `src/core/archive/GOArchiveCreator.*` – Erzeugt neue .orgue-Pakete + `organindex.ini`
- Integration mit Organ-Liste:
  - `src/core/GOOrganList.*` – Sammlung von `GOOrgan` und `GOArchiveFile`
  - `src/core/GOOrgan.*` – ODF-Pfad, Metadaten, Hash, Archive-ID/Path
- Namen/Patterns:
  - `src/core/files/GOStdFileName.*` – Hash-/Pattern-Funktionen für Organs/Index/Settings/Cache
- Build/Packaging:
  - Linux: `build-scripts/for-linux/build-on-linux.sh` (cpack Source RPM)
  - Win64: `build-scripts/for-win64/make-windows-package.sh` (Packaging), `build-on-linux.sh`
  - CMake/CPack: Top-Level `CMakeLists.txt`, `resources/GrandOrgue.appdata.xml.in`, `resources/*.in`

## Wichtige Klassen/Funktionen (Ausschnitt)
- `GOArchiveManager`
  - `ReadIndex(...)` – liest `organindex.ini`, extrahiert Organ-Liste/Dependencies, registriert in `GOOrganList`
  - `InstallPackage(...)` – installiert/registriert Paket unter Cache/Install-Pfad
  - `OpenArchive(...)` – Zugriffs-Factory auf Archiv
- `GOArchiveFile`
  - Metadaten + Zustandsprüfung (`IsUsable`, `IsComplete` gegen Dependencies)
- `GOArchiveCreator`
  - `AddPackage(path)` / `RegisterPackage(path)` – Paketdatei hinzufügen/indexieren
  - `AddOrgan(path)` – ODF/Organ-Dateipfad vormerken
  - `FinishPackage()` – schreibt Inhalte + `organindex.ini` (inkl. `OrganCount`, `Filename`, `ChurchName`, `OrganBuilder`, `RecordingDetails`)
  - `addOrganData(...)` – parst ODF, validiert Pflichtfelder
- `GOOrganList`
  - `AddArchive(...)`, `AddOrgan(...)`, LRU/Usability-Filter, Deduplikation/Update

## Datenflüsse und Zuständigkeiten
- Installation/Nutzung:
  - Pfad → `GOArchiveManager::Register/InstallPackage` → `ReadIndex` → `GOOrganList::AddArchive/AddOrgan`
  - Controller (ODF Load) fragt Liste/Archive ab → `GOFileStore` öffnet Datei aus Archiv oder FS
- Paket-Erzeugung:
  - Tool/Code fügt Organ-Datei(en) hinzu → `GOArchiveCreator::FinishPackage` → schreibt `organindex.ini` + Inhalte → neues `.orgue`
- Validierung:
  - Dependencies/Usability über `GOArchiveFile::IsComplete` und `GOOrganList` geprüft

## Typische Aufrufketten (Skizze)
- Paket lesen:
  - `GOArchiveManager::OpenArchive` → `ReadIndex(archive, InstallOrgans?)` → `GOOrganList::AddArchive/AddOrgan`
- Paket bauen:
  - `GOArchiveCreator::AddPackage` → `AddOrgan`(ODF) → `FinishPackage` → erzeugt `.orgue` + `organindex.ini`
- ODF-Zugriff aus Archiv:
  - Controller/Loader → `GOFileStore.LoadArchives` → `FindArchiveContaining(odf)` → `OpenFile`

## Integration mit anderen Bereichen
- ODF Loading: liefert Organ-Liste/ODF-Pfade über Index; `GOFileStore` nutzt Archive zur Dateibereitstellung
- Settings: speichert Pfade/Caches/Install-Orte
- GUI: Organ-Selektor/Properties zeigen Archive/Organs an
- Packaging/Distribution: CPack/Build-Skripte (DEB/RPM/Win-Pakete)

## Open Questions / TODO
- Datei:Zeile-Links via clangd ergänzen:
  - `GOArchiveManager::ReadIndex` (organindex.ini Parsing)
  - `GOArchiveCreator::FinishPackage`/`addOrganData`
  - `GOOrganList::AddArchive/AddOrgan`
- Dokumentieren: exakte Struktur von `organindex.ini` (Schlüssel, Gruppen) anhand Code
- Pfadnormalisierung in `go_path.h`/`go_normalize_path` verlinken

## Clangd-Index-Hinweise
- Suche nach „organindex.ini“ Referenzen und referenzierte Call-Sites
- „depend on organ package“ Strings in Creator/Manager nutzen, um Abhängigkeitscode zu markieren

## Packaging via Build-Skripte
- Linux: `build-scripts/for-linux/build-on-linux.sh` ruft CPack auf (Source RPM)
- Win64: `build-scripts/for-win64/make-windows-package.sh` für Windows-Paket
- App-Metadaten/Launcher: `resources/*.in`, Desktop/AppData/Manifest

## Change-Log
- 2025-08-22: Erste Fassung basierend auf Archiv-/Manager-/Creator-Dateien; Zeilenverweise noch offen
