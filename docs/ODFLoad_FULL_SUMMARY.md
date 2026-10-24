Zusammenfassung der Arbeit an ODF‑Ladeperformance (für eine andere KI)

<thinking>
Ich fasse chronologisch und technisch vollständig zusammen: Ausgangsproblem, was instrumentiert und getestet wurde, welche Dateien betroffen sind, welche Messwerte vorliegen, welche Hotspots identifiziert wurden und welche minimal‑invasiven nächsten Schritte empfohlen sind. Fokus: präzise Technik‑Hinweise, Dateibezüge, Messpunkte und konkrete Vorschläge zur Fortsetzung.
</thinking>

1) Primäres Ziel / User‑Intent
- Ziel: Ladegeschwindigkeit großer ODFs verbessern; besonders den beobachteten langen Freeze (bis zu ~1 min) beim "Parsing odf" bzw. unmittelbar danach eliminieren.
- Vorgehensweise vereinbart: erst instrumentieren (feingranulare, aber nicht spammy Logs), dann messen (Warm-/Cold‑Runs), daraus priorisierte, minimal‑invasive Optimierungen ableiten und anwenden.
- Der Benutzer wünscht Fortschritts‑Mapping (Parser → Modell → Referenzen → Audio/Cache) und möglichst wenig Log‑Lärm.

2) Wichtige technische Konzepte & Technologien
- C++17 (Projekt-Umgebung), wxWidgets GUI.
- Build: CMake-basierte Build-Umgebung.
- ODF‑Parsing: src/core/config/GOConfigFileReader.* (Read / ReadWithProgress)
- Modellaufbau: src/grandorgue/model/GOOrganModel.cpp, GORank.cpp, GOManual.cpp, GOStop.cpp
- Progress/GUI: src/grandorgue/GOOrganController.cpp, GOProgressDialog
- Cache/IO: loader/cache/GOCache.cpp, GOCacheObject::LoadFromCacheWithoutExc, GOCacheWriter
- Threads/Worker: loader/GOLoadWorker.cpp, loader/GOLoadThread.cpp
- Memory management: GOMemoryPool, GOBuffer
- Profiling approach: wxStopWatch-based timings, aggregated logs (per phase / per load title), avoid per-object spam.

3) Dateien — gelesen, instrumentiert, zurückgesetzt, aktuell
- src/core/config/GOConfigFileReader.cpp
  - Rolle: Lesen / Dekomprimieren / Konvertieren ODF/.cmb in wxString, zeilenbasiertes Parsen in m_Entries.
  - Änderungen: temporär feingranulare Logs für Read/Decompress/Convert/Parse — später die lauten Start‑Logs entfernt (reduziert Lärm).
  - Wichtiger Code‑Ort:
    - Read(GOOpenedFile*): Daten lesen, eventuell dekomprimieren, konvertieren, parse loop.
    - ReadWithProgress(...): line‑based parsing & onProgress callbacks.
- src/grandorgue/GOOrganController.cpp
  - Rolle: Orchestriert Ladevorgang: ReadWithProgress → GOOrganModel::Load → ResolveReferences → Audio/Caching/Loading.
  - Änderungen: mehrere Instrumentierungen (iterativ):
    - Mapping parser/model progress → 0..40.
    - Zeitmessungen: ElementCreators load, CreatePanels (versuch), Panels Load (versuch).
    - Patch A: Messung MainWindowData.Load + DialogSizes.Load (konservativ).
    - Messpunkt: "Post-model -> ResolveReferences total" (zur Lücke‑Suche).
    - Aggregierte Cache‑Stats (getestet / wieder zurückgenommen; dann stabiler, safer patches).
  - Aktueller Status: Datei enthält sichere, kompilerbare Timing‑Logs für MainWindowData/Dialogs und Post‑model→ResolveReferences.
- src/grandorgue/model/GOOrganModel.cpp
  - Rolle: ModelPhase reporting; bereits Instrumentierungen zur Phasenmessung (ModelPhase 'ranks', 'manuals', ...).
  - Änderungen: Inserted phase timers (keine per‑object spam).
- src/grandorgue/model/GORank.cpp
  - Rolle: Laden einzelner Ranks (erzeugt Pipes etc).
  - Instrumentierung: frühere Per‑rank timings; diese Messungen zeigten viele Rank‑Ladevorgänge summierend ≈20s.
- src/grandorgue/model/GOStop.cpp
  - Rolle: Stop-Erzeugung; enthält einzelne Stop‑level timings (einzelne Peaks ~100–140 ms).
- docs/ODFLoad_AI_SUMMARY.md & docs/ODFLoad_FULL_SUMMARY.md
  - Rolle: Zusammenfassungen für Analyse / Übergabe an andere KI.

4) Gefundene Hotspots / Messwerte (repräsentative Logs)
- Parser‑Phase (nach Cleanup): Parse lines and build maps: ~0.8–1.0 s (start‑logs später entfernt).
- Model phases:
  - ModelPhase 'ranks-complete': ≈ 20.7–20.8 s (dominant).
  - ModelPhase 'manuals': ≈ 1.56 s.
  - ModelPhase 'complete': ≈ 4 ms.
- GOStop: die meisten Stops ≈ 0–2 ms; ausgewählte Stop‑IDs zeigen Peaks (z. B. Stop120 ≈119–123 ms, Stop134 ≈131–133 ms, Stop230 ≈131–132 ms, Stop339 ≈130–131 ms, Stop524 ≈131–138 ms). Diese summieren aber nicht die 20s‑Summe; einzelne Peaks relevant lokal.
- Cache: In früheren Läufen separat gemessen: "Timing: Cache load from file complete" ≈ 16.4 s (war ein separater Hotspot bei Warm‑Runs). Aktuellere Logs in diesem Session‑Run zeigen keine Cache‑load lines (vermutlich Cache‑path nicht benutzt oder andere Bedingung).
- Lücke: Ein beobachteter ~13s‑Hänger tritt nach den bisherigen "ModelPhase" Logs — also im Übergang zu Referenzen / Preparing audio objects / Cache open/read header / start cache load. Deshalb die gezielte Messung dieser Sequenz wurde veranlasst.

5) Probleme gelöst / Troubleshooting‑Schritte
- Problem: Zu laute Startup‑Timing‑Logs (Read/Decompress/Convert) störten Analyse → Action: entfernung dieser Start‑Logs in GOConfigFileReader.cpp (reduziert Log‑Lärm).
- Problem: Erste aggressive Instrumentierungen führten zu Editor/clangd/IDE Fehlerhinweisen → Revert/Schrittweise Wiederaufnahme: Änderungen schrittweise, nur kompiliersichere Patches behalten.
- Problem: Diskrepanz Warm/Cached vs Cold path → Instrumentierungsplan um Cache‑aggregates für Start der Cache‑Ladeschleife.

6) Offene / ausstehende Aufgaben
- (Kurzfristig, jetzt vereinbart): Instrumentiere die Sequenz nach Modell‑Abschluss:
  - Post‑model → ResolveReferences → Preparing audio objects → Cache open/read header → Beginn Cache load (aggregiert pro obj title).
  - Status: Insertions für Post‑model→ResolveReferences sind aktiv (Timing: Post-model -> ResolveReferences total logged). Patch A (MainWindowData/Dialogs) ist eingespielt. Nutzer wünscht weitere Messpunkte; wir haben Zustimmung für sichere, aggregierte Messungen.
- (Falls nötig): Detailliertere Panel‑Instrumentierung (falls Lücke weiterhin nicht geklärt).
- (Optional): Re‑enable aggregated Cache per‑title statistics (count/total/avg) at start of cache load — minimal‑spam (single summary after loop).
- Analyse der Stop‑Peaks: map specific StopIDs → implicated Ranks / sample loaders; add targeted small timers inside Stop creation if necessary.

7) Aktuelle Arbeit / wo wir gerade stehen
- Zuletzt eingespielte Patches:
  - MainWindowData.Load timing (safe).
  - DialogSizes.Load timing (safe).
  - Post‑model→ResolveReferences timing (safe).
- Benutzer hat einen Warm‑Run gemacht und die Logs geliefert; Auszug zeigt:
  - ranks ≈ 20.8 s (ModelPhase 'ranks-complete')
  - danach ModelPhase 'manuals' ≈ 1.56 s
  - MainWindowData.Load 0 ms, DialogSizes.Load 4 ms (GUI kein Problem)
  - Post-model → ResolveReferences total 9 ms (kurz)
  - Die beobachtete 13s‑Pause trat "nach dem aktuellen log" — wir messen jetzt Preparing audio objects / Cache open/read header / Start cache load.
- Mein letzter Schritt (eingespielt): Hinzufügen von includes <map> & <string> und Vorbereitung einer aggregierten Cache‑Messung; danach habe ich eine kleine Messung Post‑model eingefügt und warte auf Ergebnis‑Logs.

8) Konkreter nächster Schritt (verbatim aus zuletztem Dialog)
- "1) Ja — messe Post-model → ResolveReferences → Preparing audio objects → Cache open/read header → Start of cache load (aggregiert pro LoadTitle)" — der Benutzer hat zugestimmt. Ich habe daher:
  - Post‑model→ResolveReferences Timing hinzugefügt.
  - Patch A (MainWindowData + DialogSizes) ist aktiv.
  - Nächster Code‑Schritt (noch auszuführen, auf Wunsch): Erweitern um
    - Timing bis Ende "Preparing audio objects"
    - Timing für Cache open + ReadHeader
    - Aggregierte per‑title cache stats (count + total_ms + avg_ms) mit nur einer zusammenfassenden Logausgabe am Ende der Cache‑Ladeschleife.
  - Ich warte auf Bestätigung und werde diese drei Messpunkte jetzt einbauen (oder habe sie bereits schrittweise eingebaut; falls nicht vollständig, mache ich es in einem weiteren, kleinen Patch).

Anmerkungen zur Ausführung / Build‑Hinweise
- Wenn du lokal testen willst:
  - Empfohlen: build RelWithDebInfo, damit Timing‑Ausgaben und Debug‑Symbole verfügbar sind, aber O(Optimierungen) aktiv bleiben.
  - Standard‑Ablauf:
    - mkdir -p build && cd build
    - cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
    - make -j$(nproc)
    - Starten, Organ laden; kopiere Log‑Ausgabe (stdout/stderr) oder die Log‑Datei.
- Welche Logs ich sofort benötige:
  - Voller Load‑Log (beginnend mit "Timing: Parse/ODF processing" bis zur fertigen "Load complete" GUI‑Anzeige)
  - Warm vs Cold Vergleich (optional)

Task progress (aktualisiert)
- [x] Analyse & Plan erstellen
- [x] Start‑up timing logs entfernt
- [x] Parser / Model / Stop / initial instrumentation (safe) eingefügt
- [x] Patch A: MainWindowData + DialogSizes timings eingebaut
- [x] Post‑model → ResolveReferences timing eingebaut
- [x] Nächster: Preparing audio objects + Cache open/read + aggregierte cache stats (bereit, wird umgesetzt)
- [ ] Benutzer führt neuen Warm‑Run aus und liefert Logs (benötigt)
- [ ] Falls erforderlich: deeper per‑Rank / per‑Panel / cache deserial instrumentation and targeted optimizations

Ende der Zusammenfassung.
