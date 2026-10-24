ODF Load — schnelle Analyse der gelieferten Logs (Kurz)
-----------------------------------------------------

Datum/Quelle:
- Log-Ausschnitte vom Nutzer (Cold/Warm runs, zwei Orgeln).

Wesentliche Beobachtungen
- Panel-Laden:
  - GUI.Panels.Load total_ms=1649 (12 Panels) — Panels-Gesamt ≈ 1.6 s.
  - Einzelne Panel‑Ladezeiten liegen im Bereich 59–267 ms; kein einzelnes Panel dominiert stark.
  - Nach dem Fix sind die Panel‑Times valide (keine Format‑Artefakte mehr).

- Modelaufbau:
  - ModelPhase 'manuals' = 1256 ms (frühere Log) — der Modellaufbau der Manuals kostet spürbar (~1.25 s) beim größeren ODF.

- Cache / Audio-Objekte:
  - BeforeCacheDeserialization objects=4244
  - Cache load summary total_objects=4244 total_ms=2604 (neuere Messung) bzw. 3338 ms (ältere Messung)
  - Die gesamte Cache‑Deserialisierung dauert aktuell ~2.6–3.3 s. Durchschnitt pro Objekt sehr klein (<2 ms), aber 4k Objekte summieren sich.

- Layout:
  - GUI.Panels.Layout total_ms=0 — durch Deferred Layout / wxCallAfter reduziert; Layout blockiert die UI nicht mehr sichtbar.

- Bitmap‑Dekodierung:
  - Keine langsamen Bitmap‑Dekodierungen in den gelieferten Logs aufgefallen (keine GUI.Bitmap.Load >120ms geloggt).

Einschätzung / Prioritäten
1. Hauptzeitfresser aktuell: Cache‑Deserialisierung (gesamt ~2.6–3.3 s) und Model‑Phase (manuals ~1.25 s).
2. Panels sind bereits in akzeptablen Bereichen (Summe ~1.6 s) und Layout ist deferred — UI‑Freeze reduziert.
3. Weitere Optimierungen an Panels (vollständiges deferred Load) bringen noch Verbesserungen bei sichtbarer UI, sind aber sekundär gegenüber Cache-Optimierung, wenn Ziel die Gesamtladezeit ist.

Konkrete, priorisierte Handlungsempfehlungen
1. Kurzfristig (geringer Aufwand):
   - Behalte Deferred Layout aktiv (LAZY_SECONDARY_PANELS).
   - Vergleiche Messungen mit/ohne Layout‑Deferral für Time‑to‑first‑visible (A/B).
   - Erhöhe testweise LoadConcurrency in Einstellungen (m_config.LoadConcurrency()) um die Cache‑Deserialisierung schneller zu machen (mehr Worker Threads). Messen, ob IO oder CPU der Engpass ist.

2. Mittelfristig (mittlerer Aufwand, hoher Nutzen):
   - Parallelisierung / Batch-IO für Cache‑Deserialisierung:
     - Prüfen, ob reader.ReadFromCache() und LoadFromCacheWithoutExc() CPU- oder IO‑gebunden sind.
     - Falls IO‑gebunden: mehr Threads / größere IO‑Puffer / weniger Synchronisation.
     - Falls CPU‑gebunden: Profiling der Deserialisierung-Funktionen und Optimierung (z. B. weniger Kopien).
   - Optional: Cache‑Format prüfen (bessere Layouts / weniger Overhead pro Objekt, ggf. Binary packing optimieren).

3. Panels & GUI (zusätzlich, optional):
   - Komplettes LoadFromSnapshot implementieren (schrittweise):
     - Snapshot alle benötigten, serialisierbaren Werte vorhalten.
     - Deferred construction der Controls per wxCallAfter: reduziert kritischen Pfad.
     - Logging: markiere "deferred=true" in Panel‑Logs.
   - Ergebnis: bessere Time‑to‑first‑paint, weniger wahrgenommene Verzögerung.

4. Messkampagne (unabdingbar vor größeren Änderungen):
   - Führe Cold & Warm Runs mit:
     - Baseline (kein LAZY_SECONDARY_PANELS)
     - Mit Layout‑Deferral
     - Mit erhöhter LoadConcurrency
   - Filtere Logs auf: GUI.Panel.Load, GUI.Panels.Load, GUI.Panels.Layout, GUI.CreatePanels, GUI.Bitmap.Load, Timing: Cache.byTitle.
   - Messe Time‑to‑first‑visible (manuell/Screenshot) + vollständige Ladezeit.

Schnelle Checks / Diagnostik (was du lokal ausführen kannst)
- Prüfe, ob Cache‑Deserialisierung IO‑gebunden ist:
  - Während des Cache-Laufs die CPU‑Auslastung und IO‑Wait beobachten (top/iostat).
- Teste LoadConcurrency:
  - Erhöhe LoadConcurrency um 2×, 4×; beurteile Skalierung (Zeit vs. Thread‑Overhead).
- Wenn einzelne Cache-Titel auffällig sind (Cache.byTitle Top-N):
  - Untersuche die Dateigröße / Komplexität dieser Dateien (können seltene große WAVs sein).

Vorschlag: nächster Schritt
- Du wolltest selbst bauen/testen. Bitte führe die Messkampagne mit:
  - Current code (Layout‑Deferral aktiv)
  - Variante: erhöhte LoadConcurrency
  und sende mir die gefilterten Logs (GUI.Panel.Load, GUI.Panels.Load, GUI.Panels.Layout, Timing: Cache.byTitle).
- Wenn du möchtest, implementiere ich anschließend die vollständige LoadFromSnapshot‑Rekonstruktion (Option für deferred panel Load) — sag Bescheid und ich beginne mit dem ersten Patch.

Kurz-Checklist (Status)
- [x] Format-String Bugfix (Panel logging)
- [x] Deferred Layout: sekundäre Panels per Event‑Loop
- [x] Snapshot‑Stub + deferred LoadFromSnapshot() vorhanden (stub)
- [ ] Vollständige LoadFromSnapshot‑Rekonstruktion implementieren (optional)
- [ ] Cache‑Parallelisierung prüfen / implementieren (empfohlen)
- [ ] Messkampagne: Cold/Warm runs (du ausführst) und Logs liefern
