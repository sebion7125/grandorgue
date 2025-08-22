# Assistant Memory Instructions (Chunked Reading Policy)

Ziel: Einfrieren beim Erinnern verhindern, indem große Dateien nie vollständig gelesen werden. Stattdessen werden Indexe und kleine Chunks verwendet.

Hard Policy (verbindlich)
- Nie `read_file` auf sehr große Dateien wie `docs/GO_Project_Chats.md`.
- Zuerst den Titel-Index `docs/GO_Conversation_Titles.json` nutzen, um relevante Themen/Abschnitte einzugrenzen.
- Pro Anfrage nur kleine Textfenster laden (~1–2KB) und maximal 3–5 Treffer verarbeiten.
- Gesamtlesevolumen pro Anfrage begrenzen (z. B. ≤ 10KB) und früh abbrechen, sobald genügend Kontext vorliegt.

Vorgehen (Schritt-für-Schritt)
1) Index verwenden:
   - Lese `docs/GO_Conversation_Titles.json` (klein, schnell).
   - Wähle Top-N relevante Titel basierend auf der Anfrage.

2) Kontext gezielt suchen (bevorzugt Tool-gestützt):
   - Verwende `search_files` mit `file_pattern: 'docs/GO_Project_Chats.md'` und einem Regex, der auf die gewählten Titel oder charakteristische Phrasen passt.
   - Vorteil: Es werden nur kontextnahe Ausschnitte geliefert; kein Voll-Laden der Datei.

3) Falls `search_files` nicht ausreicht (optional):
   - Streamen per CLI oder Hilfsskript:
     - `.cline/chunked_reader.py` gibt NDJSON-Chunks aus, z. B.:
       ```
       python3 .cline/chunked_reader.py --file docs/GO_Project_Chats.md --chunk-size 2048 --max-chunks 10
       ```
     - Die Ausgabe kann extern gefiltert werden (z. B. per `jq`, `grep`) um nur relevante Chunks zu betrachten.
   - Alternativ klassische Tools: `rg/grep/sed/head/tail` mit eng gefassten Mustern und kleinen Fenstern.

4) Verarbeitung:
   - Treffer zusammenfassen, keine Vollzitate und keine großen Blöcke.
   - Bei Bedarf weitere kleine Fenster selektiv nachladen.

5) Sicherheits-/Compliance-Hinweise:
   - Keine Geheimnisse, Keys oder personenbezogene Daten ins Repo schreiben.
   - Sensible Inhalte ausschließlich in der Konversation behandeln und auf sichere Ablage verweisen.

Konkrete Beispiele

A) Suche via search_files (konzeptionell)
- Input: Titel aus `docs/GO_Conversation_Titles.json`, z. B. "Audio Engine Init"
- Suche: Regex mit Titel- oder Schlüsselwortfragment in `docs/GO_Project_Chats.md`
- Ergebnis: Kleine Auszüge mit Treffer-Kontext (toolseitig begrenzt)

B) Chunk-Streaming via Hilfsskript
- Erste 10 Chunks (je 2KB):
  ```
  python3 .cline/chunked_reader.py --file docs/GO_Project_Chats.md --chunk-size 2048 --max-chunks 10
  ```
- Mit Offsets (zur späteren Byte-Range-Referenz):
  ```
  python3 .cline/chunked_reader.py --file docs/GO_Project_Chats.md --chunk-size 2048 --max-chunks 5 --show-offsets
  ```

Verweise
- Index: `docs/GO_Conversation_Titles.json`
- Große Datei (nicht voll lesen): `docs/GO_Project_Chats.md`
- Richtlinien verankert in: `.cline/context.md` (HARTE POLICY)
- Kurznotizen/Log: `.cline/memory_log.md`
- Hilfsskript: `.cline/chunked_reader.py`

Kurzfazit
- Index → gezielte Suche → kleine Chunks → strikte Volumen-Limits.
- Kein Voll-Lesen großer Dateien. So bleibt der Assistent reaktionsschnell und friert nicht ein.
