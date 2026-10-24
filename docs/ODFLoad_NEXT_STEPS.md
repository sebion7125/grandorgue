ODF Load — Next Steps (Kurzplan, safe / minimal-invasiv)
--------------------------------------------------------

Ziel
----
Sicheres, experimentelles Deferred‑Loading der sekundären Panels implementieren,
ohne das bestehende cfg‑Lebenszeitmodell zu brechen. Priorität: sichtbare GUI
schneller machen (weniger Freeze beim Parsing/Panel‑Phase), minimaler Risiko‑Eingriff.

Kontext (bereits erledigt)
- Panels werden aktuell vollständig synchron geladen (GOGUIPanel::Load).
- Ich habe bereits eine Layout‑Deferral‑Änderung in `GOOrganController.cpp`:
  - Wenn `LAZY_SECONDARY_PANELS` definiert ist, wird nur `m_panels[0]->Layout()`
    synchron ausgeführt; sekundäre Panels erhalten `wxCallAfter([p](){ p->Layout(); })`.
  - Das reduziert UI‑Blockierung beim Layout, jedoch bleibt `Load` synchron.
- Projekt baut erfolgreich nach dieser Änderung.

Warum LoadDeferred heikel ist
----------------------------
- `GOGUIPanel::Load(GOConfigReader &cfg, const wxString &group)` benötigt
  `GOConfigReader` (wrapper um `GOConfigReaderDB`) — dieses Objekt ist nicht
  trivially copyable (es hält eine Referenz auf DB).
- `ReadOrganFile`/`Load` erzeugen lokale `GOConfigReader`/`GOConfigFileReader`
  Instanzen; wenn wir `m_panels[i]->Load(...)` in `wxCallAfter` verschieben,
  würde der übergebene `cfg` nicht mehr gültig sein wenn die Callback läuft.
- Sichere Lösungen benötigen entweder:
  1) Eine schlanke Serialisierung des für jedes Panel relevanten Config‑Slices,
     die in heap allozierten Objekten gespeichert und später in einer temporären
     GOConfigReader-Instanz benutzt wird; oder
  2) Eine neue, explizite LoadDeferred-API, die nur die minimal benötigten Werte
     übernimmt (z. B. Panel‑Groupname + explizite Parameter wie Booleans/Counts),
     und intern die Controls/Elemente nachbaut ohne direkten Zugriff auf die
     originalen cfg-Objekte; oder
  3) Die Lebenszeit von cfg/ini so verlängern, dass deferred callbacks noch
     gültigen Zugriff haben (riskant und nicht empfohlen).

Vorgeschlagener, sicherer Implementierungsweg (empfohlen)
--------------------------------------------------------
Schritt A — Experimentelle, minimale Variante (Iterativ, risikoarm)
  1. Keep current behaviour: `m_panels[i]->Load(cfg, name)` weiterhin synchron.
  2. Defer only `Layout()` (bereits implementiert) — Messung, A/B Test.
  3. Messen: Warm/Cold runs, Logs filtern (siehe Test-Anleitung unten).
  4. Wenn Layout deferral signifikant hilft, proceed to Step B.

Schritt B — Safe LoadDeferred (implementieren wenn Step A nicht ausreicht)
  1. Definiere eine schlanke Snapshot-Struct `GOGUIPanel::LoadSnapshot`:
     - Enthält nur die für das Panel notwendige, stabil serialisierbare Daten:
       e.g. group name, group prefix, panel flags (is_main_panel), pre-read
       integers/booleans: NumberOfImages, NumberOfLabels, NumberOfManuals, etc.
     - Keine Referenzen auf GOConfigReader/DB; alle Strings/Primitives heap-allokiert.
  2. Führe vor Return aus `ReadOrganFile` für jeden Panel (zusätzlich zu Sync Load)
     optional eine Snapshot-Erzeugung durch:
       - Lese die wenigen benötigten keys via `cfg.ReadInteger/ReadBoolean/ReadString`
         und packe sie in `LoadSnapshot`.
       - Speicher `std::shared_ptr<LoadSnapshot>` in `m_panels[i]` oder in einem
         temporären container, je nach Design.
  3. Implementiere `GOGUIPanel::LoadFromSnapshot(std::shared_ptr<LoadSnapshot>)`
     das die heavy-control-Konstruktion (new GOGUIButton, new GOGUIManual, ...)
     genauso baut, aber aus den Werten der Snapshot-Struktur statt durch
     direkten Zugriff auf `cfg`.
     - Vorteil: keine Abhängigkeit mehr zu `GOConfigReader` während deferred
       Callback ausgeführt wird.
     - Nachbau braucht etwas Arbeit: für kontrollzählende Loops nutzt LoadSnapshot
       statt cfg.ReadInteger.
  4. In `GOOrganController`, replace (optional) second-phase:
     - `m_panels[i]->Load(cfg, buffer)` (synchron) kann bleiben optional;
       stattdessen ruft man:
         wxCallAfter([p, snap](){ p->LoadFromSnapshot(snap); p->Layout(); });
     - Alternativ: keep sync Load for critical panels, deferred LoadFromSnapshot for
       secondary panels (configurable per panel index / group).
  5. Tests & Logging: mirror existing `wxLogMessage("GUI.Panel.Load ...")` in
     LoadFromSnapshot — zusätzlich markiere "deferred=true" im Log.

Schritt C — Optional (komplizierter)
  - Vollständige Serialisierung der relevanten `GOConfigFileReader`-Gruppen
    in einen temporären INI-Block (string), auf Heap speichern, und beim
    deferred callback wieder in ein temporäres `GOConfigReader` einlesen.
  - Besser für 1:1 Kompatibilität, aber aufwändiger und fehleranfälliger.

Konkrete Code-Änderungen (Dateien / Orte)
-----------------------------------------
- src/grandorgue/GOOrganController.cpp
  - (bereits) Panels.Layout Deferral.
  - Ergänzen: Snapshot-Erzeugung vor Show/Return (siehe Schritt B.2). Optional: nur für panels index >= N.
- src/grandorgue/gui/panels/GOGUIPanel.h / GOGUIPanel.cpp
  - Neue Typen: `struct LoadSnapshot { /* Strings + counts + flags */ };`
  - Neue Methode: `void LoadFromSnapshot(std::shared_ptr<LoadSnapshot> snap);`
  - Ergänze Logging: "GUI.Panel.Load deferred=true name=... total_ms=..."
- docs/ODFLoad_PROFILING_INSTRUCTIONS.md
  - (siehe Test-Anleitung unten) – ggf. ergänzen.

Test-Anleitung (Messkampagne)
-----------------------------
- Build-Varianten:
  1. Baseline: no LAZY_SECONDARY_PANELS.
  2. Layout-Deferral: compile with `-DLAZY_SECONDARY_PANELS`.
  3. LoadDeferred (nach Implementation): compile & run.

- Was zu sammeln:
  - Filtered logs (grep/rg) für:
    - GUI.Panel.Load
    - GUI.Panels.Layout
    - GUI.CreatePanels
    - GUI.Bitmap.Load
    - Timing: Cache.byTitle
  - Messgrößen:
    - Time-to-first-UI-visible (manuell messen oder Screenshot/observer)
    - Time until GUI.CreatePanels completed
    - Total load time
    - Top-10 slow cache objects / slow bitmap decodes

Dev-Notes / Risiken
-------------------
- Snapshot-Ansatz erfordert, dass alle Values, die Load benötigt, vorher gelesen werden.
  Für komplexe ODF-Varianten könnte dies umfangreich sein; deshalb:
  - Implementiere Snapshot schrittweise: zuerst nur die obvious counts/flags,
    später erweitern.
- Logging wichtig: jede deferred Load sollte ein `deferred=true` Flag in Log tragen.
- Keep feature behind compile-time define and/or runtime setting (config toggle).

TODO-Checkliste (aktueller Stand)
- [x] Analyse & Plan erstellen
- [x] Instrumentation: GUI.ElementCreators / CreatePanels / Panels.Layout
- [x] ReadOrganFile: optional dlg param + pass dlg from Load
- [x] GUI.Panels.Load per-panel timing (non-intrusive)
- [x] Progress-Dialog: "Loading panel: NAME (i/n)" integration
- [x] Bitmap timing instrumentation (guarded)
- [x] Implement deferred secondary panels (Layout deferral) — done
- [ ] Implement LoadSnapshot struct and GOGUIPanel::LoadFromSnapshot
- [ ] Use wxCallAfter to call LoadFromSnapshot for secondary panels
- [ ] Build + Warm/Cold profiling run
- [ ] Analyse logs + propose targeted optimizations (bitmap lazy decode etc.)

Nächster Implementationsschritt (konkret, wenn gewünscht)
- Ich implementiere die `LoadSnapshot` + `LoadFromSnapshot` schrittweise:
  - Änderung in `GOGUIPanel.h/.cpp` (neue struct + Methode).
  - Änderung in `GOOrganController.cpp` um Snapshots zu erzeugen und
    `wxCallAfter([p,snap]{ p->LoadFromSnapshot(snap); p->Layout(); });`.
- Ich kann direkt mit dem ersten Patch anfangen (erzeugt LoadSnapshot mit:
  - group name (string)
  - is_main_panel (bool)
  - NumberOfImages (int)
  - NumberOfGUIElements (int)
  - NumberOfManuals / NumberOfLabels / NumberOfStops (int)
  - evtl. pre-read manual/display booleans)
  und implementiere LoadFromSnapshot für diese subset.
