Kurzzusammenfassung (für eine andere KI)
Ziel
- Ladezeit großer ODFs reduzieren; insbesondere lange Pausen während "Parsing odf" / Modellaufbau / Audio‑Vorbereitung vermeiden.
- Vorgehen: erst instrumentieren + messen, dann gezielt minimal-invasive Optimierungen.

Wichtigste Befunde aus bisherigen Runs
- Parser (Read/Decompress/Convert/Parse) insgesamt: typ. ~0.8–1.0 s (früher lauter Messwerte, Start‑Logs wurden inzwischen entfernt).
- Modellaufbau:
  - ModelPhase 'ranks' (Summe aller Rank-Ladevorgänge): ≈ 20.7 s — größter Single-Hotspot.
  - ModelPhase 'manuals': ≈ 1.6 s.
  - Viele Stops sind sehr schnell (0–2 ms), einzelne Stops zeigen Peaks (~100–140 ms).
- Cache-Deserialisierung (LoadFromCacheWithoutExc über viele Objekte): früher gemessene ~16.4 s — großer Hotspot beim Warm‑Run (Audiodaten aus Cache rekonstruieren).
- UI/Panel-Initialisierung: unklare Lücke (ca. 10–12 s) zwischen ElementCreators.Load und CreatePanels — mögliche Kandidaten: MainWindowData.Load, DialogSizes.Load, viele Panel::Load-Aufrufe, Bitmap‑Ladevorgänge.

Was bereits instrumentiert / verändert wurde (iterativ, minimal)
- Parser: feingranulare Messpunkte (read/decompress/convert/parse). Start‑Logs entfernt (reduziert Log‑Lärm).
- GOOrganController::Load:
  - Modell-Fortschrittsmapping (0..40 für Modellphase).
  - Timing für ResolveReferences, Prepare audio objects, Cache open/read.
  - Temporäre Aggregationstests im Cache-Pfad (per‑class/Title) wurden ausprobiert und wieder zurückgenommen; Controller wurde auf stabilen Zustand zurückgesetzt.
- Model/Rank/Stop: Phase‑Timings; Stop‑Totals mit wenigen Peaks sichtbar.
- GOConfigFileReader: laute Start‑Timing‑Logs entfernt.

Empfehlung / Priorität der nächsten Instrumentierungen (minimal-invasiv)
1) Kurz und sicher (geringe Log‑Menge) — dringend:
   - Messpunkte hinzufügen (aggregiert, 1 Log/Zeile):
     - m_MainWindowData.Load(...) → Gesamtms
     - m_config.m_DialogSizes.Load(...) → Gesamtms
   Rationale: diese Aufrufe liegen exakt in der beschriebenen Lücke; schnell testbar.

2) Mittlere Tiefe (nur falls 1 nicht reicht):
   - Pro‑Panel aggregated timings:
     - GOGUIPanel::Load total ms (für jedes Panel)
     - Optional: aggregated LoadBitmap- oder CreateControls‑Zeiten pro Panel (falls Panels teuer erscheinen).

3) Tiefer (Cache‑Pfad, falls Warm‑Run teuer):
   - Aggregiert nach Objekt‑Title/Klasse in LoadFromCacheWithoutExc: count, total_ms, avg_ms.
   - Falls schweres Deserializing, untersuchen: Dekompression vs. Deserializing vs. Allokationen.
   - Mögliche Optimierungen: Batching, parallelisieren des Deserializing, memory‑pool reuse, lazy allocation.

Welche Log‑Zeilen / Metriken die KI ansehen soll
- Parser-Phasen: Read file into buffer / Decompress buffer / Convert to wxString / Parse lines and build maps (wenn vorhanden).
- ModelPhase * (start, ranks-start, ranks-complete, manuals, complete).
- GOStop Load totals / created Rank Load entries (erkennen von Peaks).
- Cache: Cache open / Cache load start / Cache load complete / (aggregates per title/class falls aktiv).
- GUI: ElementCreators Load total, MainWindowData.Load ms, DialogSizes.Load ms, Panel.Load per panel, CreatePanels total, Panels Layout total.

Konkrete kleine Patches (Empfohlen)
- Patch A (sicher): Messpunkte für m_MainWindowData.Load und m_config.m_DialogSizes.Load (eine Zeile je Aufruf).
- Patch B (moderater Aufwand): Pro‑Panel Load logging (einzeilige, aggregierte Logs).
- Patch C (nur falls nötig): Aggregated cache deserialization logging (per title/class, emitting single summary after cache loop).

Wie die KI vorgehen sollte (operativer Ablauf)
1. Prüfe Logs für die o.g. Timing-Zeilen; markiere die größten Einzelzeiten.
2. Falls ranks >> others → instrumentiere GORank intern (nur aggregiert) oder untersuche, ob Rank‑Erzeugung/Allokation/IO schwer ist.
3. Falls Cache >> others → instrumentiere LoadFromCacheWithoutExc aggregiert und prüfe Kompression/allocs.
4. Falls GUI gap >> others → fokussiere auf MainWindowData/DialogSizes/Panels.

Quick checklist (für die andere KI)
- [ ] Hole neueste Logs (warm run) nach Entfernung der Start‑Logs.
- [ ] Prüfe ModelPhase 'ranks' + per‑rank Summen.
- [ ] Prüfe Cache load (Start/Complete) und ggf. aggregierte Werte.
- [ ] Prüfe Panel‑Load/WindowData/DialogSizes.
- [ ] Empfehle kleine, priorisierte Optimierungen (zuerst: CPU‑/alloc‑Hotspots, dann IO).

Kontakt / Übergabe
- Wenn du willst, implementiere ich Patch A (MainWindowData + DialogSizes timing) und schreibe die Änderungen in das Repo; danach bitte einen Warm‑Run und sende Logs. Ich analysiere sofort die neuen Messwerte und gebe konkrete Code‑Änderungen (z. B. lazy init, pooling, thread‑parallel deserializing) mit minimalem Invasionsgrad.
