# Projektinfos für Cline
- Dies ist ein GrandOrgue-Fork.
- Code Kommentare sind in Englisch zu halten. Sollten deutsche Kommentare gefunden werden, frage denn Benutzer, ob du es übersetzen sollst.
- Achtung: *.organ Dateien sind kein XML, sondern proprietäres Textformat.
- Kompiliert wird via MinGW Cross-Build unter Linux (Toolchain: mingw-toolchain.cmake). Mit den Scripten /home/vboxuser/grandorgue/build-scripts/for-win64/update-build-on-linux.sh und /home/vboxuser/grandorgue/build-scripts/for-win64/build-on-linux.sh; Das Ergebnis landet im build Unterordner bei den skripten.
- es kann auch für linux kompilert werden: /home/vboxuser/grandorgue/build-scripts/for-linux/build-on-linux.sh das ergebnis landet dann aber in /home/vboxuser/grandorgue/build-scripts/for-linux/build  und nicht im Ordner neben dem Skript.
- Behandle diese Datei und die in ihr(auch rekurvi)
- Thema "Crossfade": Es gibt zwei Arten. Der eine arbeitet mit den Loops und wird im cache vorberechnet. Der andere faded zwischen sustain und release. Bitte immer genau unterscheiden und im zweifel den user fragen, welchen fade er meint.

!Gedächtnis: Wenn du vom Benutzer aufgefordert wirst dir etwas zu merken, oder etwas für erinnernswert hälst, dann kannst du diese Information je nach Bezug und Wichtigkeit entweder in dieser Datei oder an einer geeigneten Stelle in den Repo‑Maps (docs/RepoMap/) speichern. Weise den Benutzer in deiner Antwort aktiv darauf hin, dass du dir die Information gemerkt hast.

Vorgehensweise für Gedächtnis‑Anfragen (Kurzfassung)
- 1) Bestätigen: Antworte dem Benutzer kurz, dass du die Anweisung verstanden hast und welche Datei du verwenden wirst (z. B. `.cline/context.md` oder `docs/RepoMap/<Thema>.md`).
- 2) Speichern: Füge eine knappe Notiz hinzu (eine Zeile) mit Metadaten: Datum, Quelle (Benutzeranfrage), Schlüsselwort/Tag, kurze Beschreibung, und optional ein Link/Verweis auf betroffene Dateien. Beispiel-Format:
  - [2025-08-22] memory: "Build-Flag X aktivieren" — stored-in: .cline/context.md — tag: build, note: Flag für Cross-Build.
- 3) Indexieren: Falls relevant für spätere Suche, ergänze `docs/GO_Conversation_Titles.json`-kompatible Metadaten oder eine RepoMap‑Notiz (Datei:Zeile oder Regex), damit future lookups leicht sind.
- 4) Zugriffsschutz: Speichere niemals Geheimnisse, Passwörter, API‑Keys oder persönliche Daten im Repo. Hinweise, die als vertraulich gelten, nur in der Konversation belassen und den Benutzer auf sichere Ablage hinweisen.
- 5) Nutzung: Bei späteren Anfragen zuerst `.cline/context.md` und die passenden RepoMaps prüfen. Für große Dateien gilt folgende HARTE POLICY (kein Voll-Lesen):
  - 5.1) Index nutzen: `docs/GO_Conversation_Titles.json` als Titel‑Index verwenden, Top‑N relevante Titel auswählen.
  - 5.2) Chunking: Pro Titel nur kleine Fenster laden (~1–2KB), maximal 3–5 Treffer.
  - 5.3) Zugriff:
    - bevorzugt `search_files` mit `file_pattern: 'docs/GO_Project_Chats.md'` und titelbezogenem Regex; liefert kontextbegrenzte Treffer ohne Voll-Laden.
    - alternativ CLI‑Streaming via `rg/grep/sed/head/tail` oder `.cline/chunked_reader.py` für NDJSON‑Chunks; niemals `read_file` auf die gesamte Datei (z. B. `docs/GO_Project_Chats.md`).
  - 5.4) Verarbeitung: Treffer zusammenfassen, keine Vollzitate; bei Bedarf weitere kleine Fenster nachladen.
  - 5.5) Limits: Gesamtlesevolumen pro Anfrage begrenzen (z. B. ≤ 10KB) und früh abbrechen, sobald genügend Kontext vorliegt.
- 6) Bestätigung an Benutzer: Nach dem Speichern automatisch eine kurze Bestätigung zurückgeben (Datum, Speicherort, Kurzinhalt).

Weitere Hinweise
- Standard‑Ablage: Kurz, maschinenlesbar, Datum + Tag + Kurzbeschreibung. Längere Erinnerungen oder thematische Sammlungen bitte in `docs/RepoMap/<Thema>_NOTES.md`.
- Referenz: Vollständige Richtlinie in `docs/Assistant_Memory_Instructions.md`.

Es folgen generelle Infos über den Aufbau des Repos, falls der Promt nicht genug Infos liefert:

Navigationshilfen:
- docs/RepoMap.md – Repo-Karte und Orientierung
- docs/GrandOrgue-Odyssey.md – Meta-Logbuch (Erkenntnisse, Fixes, Querverweise)
- docs/GO_Project_Chats.md – Konversationsmitschnitte (allgemeine Themen, Tools)
- docs/GO_API_Context_Bundle.zip – API/Projekt-Kontext-Bundle
- docs/BuildCheatsheet.md – Build-/Run-Übersicht
- VSCode-Tasks: .vscode/tasks.json – Build und Run per Task

Arbeitsanweisung zur Pflege der Repo Maps:
- Trage bei jeder Arbeit am Repo neue Erkenntnisse (wo was wie läuft, wer wen aufruft, wichtige Datenflüsse) in die feineren Repo-Maps unter docs/RepoMap/ ein.
- Bevor du Quellcode breitflächig liest, konsultiere zuerst die Repo-Maps und aktualisiere sie gezielt. Ziel: mit minimalen Leseoperationen schnell zum richtigen Ort gelangen.
- Halte Einträge knapp (Dateipfade, Kernklassen/-funktionen, kurze Beziehungs-Notizen, ggf. Call-Graph-Skizzen), verlinke auf Dateien/Zeilen statt große Codeblöcke zu zitieren.
- Wenn Strukturen unklar sind, notiere “Open Questions/TODO” direkt in der jeweiligen Map-Datei für spätere Klärung.

Standard-Arbeitsmodus: Map-first, targeted reads
- Konsultiere zuerst die Repo-Maps (docs/RepoMap.md und Unterseiten) als Index.
- Nutze Schlüsselwort-/Symbolsuche und clangd-Indices, um nur relevante Abschnitte gezielt zu lesen (keine Voll-Leseoperationen der Maps/Quelltexte).
- Lade Code punktuell (nur Funktionen/Klassen/Abschnitte) und aktualisiere bei Bedarf die Maps mit kurzen Datei:Zeile-Sprungmarken.
- Halte den Tokenverbrauch minimal; Voll-Lesen nur bei Map-Überarbeitung oder Strukturänderungen.

- [2025-08-22] memory: "RepoMap-first + Cline Context: Zuerst Titel-Index nutzen, danach nur gezielte, kleine Chunks aus großen Dateien (kein Voll-Lesen) für Erinnerungsabfragen." — stored-in: .cline/context.md — tag: memory, note: Gespräch 'RepoMap & Cline Context' bestätigt.

#END Of Context Window
