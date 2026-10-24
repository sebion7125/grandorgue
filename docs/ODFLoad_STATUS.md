ODF Load — aktueller Status (Kurz)
----------------------------------

Stand:
- Experimentelle Maßnahmen zur Reduktion der UI‑Freeze‑Zeit beim ODF‑Laden wurden implementiert.
  - Deferred secondary panels: Sekundäre Panels werden jetzt per Snapshot + wxCallAfter rekonstruiert (minimaler, sicherer Ansatz). Primäres Panel bleibt synchron.
  - Deferred Layout: Layout sekundärer Panels erfolgt per Event‑Loop, sodass die erste sichtbare UI schneller erscheint.
  - Instrumentierung bereits vorhanden: Logs für GUI.Panel.Load, GUI.Panels.Load, GUI.Panels.Layout, GUI.Bitmap.Load, Cache.byTitle Top‑N.
- Build: Lokaler Build wurde bereits erfolgreich durchgeführt (der Nutzer übernimmt weitere Builds).

Was noch offen / nächste Schritte:
- Vollständige Implementierung von LoadFromSnapshot (nahezu‑1:1 Rekonstruktion aus Snapshot).
- Optional: Erweiterte Snapshot‑Felder (wenn nötig).
- Messkampagne: Warm-/Cold‑Runs durchführen, Logs filtern und Top‑Offender analysieren.
- Zieloptimierungen: lazy bitmap decode, shared fonts/brushes, gezielte Cache‑Verbesserungen.

Anleitung für dich (empfohlen):
1. Build lokal (du übernimmst das).
2. Testfälle:
   - Cold run: Starten, vollständiges Laden eines großen ODFs, Log speichern.
   - Warm run: Starten direkt danach, Log speichern.
3. Log‑Filter (Beispiele):
   - grep -E "GUI\.Panel\.Load|GUI\.Panels\.Layout|GUI\.Panels\.Load|GUI\.Bitmap\.Load|Timing: Cache.byTitle" logfile
4. Sende mir die gefilterten Log‑Zeilen oder lade sie hier hoch — ich analysiere sofort Top‑Offender und mache konkrete Code‑Vorschläge.

Kurz-Checklist (aktueller Stand)
- [x] Per‑Panel Timing (GUI.Panel.Load) instrumentiert
- [x] Bitmap‑decode timing (guarded) eingebaut
- [x] ProgressDialog Updates während Panels.Load integriert
- [x] Deferred secondary panels: Layout deferral (wxCallAfter) implementiert
- [x] Snapshot-Erzeugung (CreateLoadSnapshot) und LoadFromSnapshot (stub) implementiert
- [ ] Vollständige LoadFromSnapshot‑Rekonstruktion implementieren
- [ ] Warm & Cold Runs durchführen und Logs sammeln
- [ ] Analyse durchführen und gezielte Optimierungen einarbeiten

Falls du willst, implementiere ich jetzt die vollständige LoadFromSnapshot‑Rekonstruktion (schrittweise), ansonsten warte ich auf deine Logs/Build‑Ergebnisse.
