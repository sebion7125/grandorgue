# RepoMap (feingranular) – Index und Konventionen

Ziel: Schnelle Navigation mit minimalen Leseoperationen. Diese Maps dokumentieren, wo was wie läuft und wie Komponenten ineinandergreifen.

Nutzen:
- Vor dem breiten Lesen zuerst diese Maps sichten
- Gezielt nachpflegen, wenn neue Zusammenhänge klar werden
- Mit Links auf Dateien/Zeilen arbeiten, statt große Codeblöcke zu zitieren

Pflegehinweis (siehe auch .cline/context.md):
- Bei jeder Arbeit am Repo kurz die passenden Maps aktualisieren (Dateien, Kernklassen/-funktionen, Datenflüsse, Aufrufbeziehungen)
- Einträge knapp halten; offene Fragen als TODO notieren

## Struktur für jede Map
- Überblick
- Schlüsseldateien/-verzeichnisse
- Wichtige Klassen/Funktionen
- Datenflüsse und Zuständigkeiten
- Typische Aufrufketten (Call-Graph Skizze, ggf. Pseudocode)
- Open Questions / TODO
- Change-Log (Datum – kurze Notiz, Link auf Diff/Commit optional)

Zur einheitlichen Pflege gibt es eine Vorlage: `_TEMPLATE.md`.

## Index (Bereichs-Maps)
- Sound-Engine: [SoundEngine.md](./SoundEngine.md)
- GUI: [GUI.md](./GUI.md)
- Settings/Preferences: [Settings.md](./Settings.md)
- ODF Loading (*.organ): [ODFLoading.md](./ODFLoading.md)
- Model/Pipes: [ModelPipes.md](./ModelPipes.md)
- MIDI: [MIDI.md](./MIDI.md)
- Archive/Packaging: [ArchivePackaging.md](./ArchivePackaging.md)
- Controller/Events: [ControllerEvents.md](./ControllerEvents.md)

## Querverweise
- Meta-Logbuch: `docs/GrandOrgue-Odyssey.md`
- Gesamtüberblick: `docs/RepoMap.md`
- Build/Run: `docs/BuildCheatsheet.md`
- Kontext/Arbeitsanweisung: `.cline/context.md`
