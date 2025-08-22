# Memory Log — Kurznotizen der Assistenz
Quelle: automatisch erzeugt  
Erstellt: 2025-08-22

- [2025-08-22] memory: "Vorgehensweise für Gedächtnis-Anfragen (Bestätigen → Speichern einzeilig mit Datum/Tag → Indexierungshinweis → Zugriffsschutz → Nutzung: Titel-Index → Top-N → 3–5 Nachrichten → Chunking ~1–2KB)" — stored-in: .cline/context.md — tag: memory, note: Kurze Richtlinie für zukünftige Gedächtnis‑Anfragen.  
- Referenzen:
  - docs/Assistant_Memory_Instructions.md (ausführliche Richtlinie)
  - docs/GO_Project_Chats_SUMMARY.md (erste Zusammenfassung der Chat‑Exportdatei)
  - docs/GO_Conversation_Titles.json (Titel‑Index für gezielte Suche)

Warnhinweis:
- Keine Geheimnisse, Keys oder sensible personenbezogene Daten in dieses Repo schreiben. Falls der Benutzer vertrauliche Daten speichern möchte, darauf hinweisen, sichere Ablage zu verwenden.

Kurzstatus:
- Vorgehensweise gespeichert in .cline/context.md und protokolliert in dieser Datei.
- [2025-08-22] policy: "HARTE POLICY: große Dateien nie voll lesen; Index (docs/GO_Conversation_Titles.json) → Top‑N; nur kleine Chunks nachladen (≤10KB gesamt) via search_files oder .cline/chunked_reader.py; kein read_file auf docs/GO_Project_Chats.md" — stored-in: .cline/context.md — tag: memory-policy, note: eingefrorene Sitzungen vermeiden
- [2025-08-22] user-confirmation: "Benutzer bestätigt: allgemeine Vorgehensweise merken" — stored-in: .cline/context.md — tag: memory, note: Benutzerbestätigung.
