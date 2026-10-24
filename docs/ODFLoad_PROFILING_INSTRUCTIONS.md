Profiling‑Anleitung für ODF‑Load (Deutsch) — kurze, praktische Schritte zum Selberbauen und Timings sammeln

Kurzfassung
- Ziel: exakte Phase‑Timings (Parser, ResolveReferences, Preparing audio objects, Audio load) sammeln.
- Voraussetzung: In der aktuellen Quelle ist die Timing‑Ausgabe hinter dem Makro GO_PROFILE_ODFLOAD vorhanden. Stelle sicher, dass dieses Makro definiert ist (es wurde in deiner lokalen Kopie bereits in GOOrganController.cpp gesetzt).
- Ergebnis: eine Logdatei mit "Timing:"‑Zeilen und "Building:"‑Meldungen, die wir auswerten können.

1) Vorbereitungen
- Stelle sicher, dass du in deinem Arbeitsbaum die aktuellen Änderungen gespeichert/committed hast (du hast das zuletzt getan).
- Prüfe, dass in src/grandorgue/GOOrganController.cpp das Makro GO_PROFILE_ODFLOAD definiert ist. (Wenn nicht, alternativ beim CMake-Build die entsprechende Option setzen.)

2) Bauen
- Für Win64‑Cross‑Build (wie bisher benutzt):
  - Benutze dein gewohntes Skript zum Cross‑Build (z. B. build-scripts/for-win64/update-build-on-linux.sh).
  - Das Skript erzeugt die Binärdateien unter build-scripts/for-win64/build/win64/bin/.
- Für Linux‑Native:
  - Benutze build-scripts/for-linux/build-on-linux.sh (oder deine übliche Build‑Sequenz mit cmake && make).
- Hinweis: Du baust lokal — ich habe die Timing‑Makros bereits in den Quellen aktiviert, falls du das anders möchtest, sag Bescheid.

3) Smoke‑Test (Ausführen & Logs sammeln)
- Wähle die Binary (für Cross‑Build ist das die exe in build/.../bin; für native die ausführbare Datei).
- Empfohlene Vorgehensweise (Terminal, Arbeitsverzeichnis repository root):
  1) Lege ein Verzeichnis für Logs an:
     - logs/
  2) Starte die Anwendung so, dass Konsole/Log in eine Datei umgeleitet wird (Beispiel, passe Pfad zur Binary an):
     - PATH_TO_BINARY="/pfad/zur/GrandOrgue.exe"   # oder ./bin/GrandOrgue (native)
     - LOGFILE="logs/odfload_$(date +%Y%m%d-%H%M%S).log"
     - $PATH_TO_BINARY > $LOGFILE 2>&1
     (Wichtig: die App öffnet ein GUI — die Konsolen‑wxLogMessage-Ausgaben sollen in der umgeleiteten Logdatei landen)
  3) In der App: lade das ODF (z. B. packages/demo-...orgue). Warte bis Ladeprozess fertig ist.
  4) Wiederhole ggf. zwei Läufe:
     - Mit vorhandenem Cache (falls vorhanden).
     - Ohne Cache: lösche ggf. das Cachefile (z. B. rm build/.../organ.cache oder per UI/Settings) und führe erneut.

4) Timing‑Extraktion (einfach)
- Nachdem die Laufdatei fertig ist:
  - Extrahiere Timing‑Zeilen:
    - grep "Timing:" logs/odfload_*.log > logs/timing_summary.txt
  - Extrahiere Progress‑Zeilen:
    - grep -E "Building:|Parsing|Loading audio" logs/odfload_*.log > logs/progress_sequence.txt
- Die Datei timing_summary.txt enthält z. B.:
  - Timing: Parse/ODF processing: 1234 ms
  - Timing: ResolveReferences: 4567 ms
  - Timing: Preparing audio objects… (Marker)
- Die Datei progress_sequence.txt zeigt die sichtbaren Fortschrittsmeldungen mit Reihenfolge. Wenn du die Logs mit Zeitstempeln möchtest, starte die App aus einem Terminal mit `ts` (from moreutils) oder prefixe die Logfile‑Zeilen beim Ausführen: e.g. use `stdbuf -oL` and `ts` — wenn du das brauchst, kann ich Beispielbefehle liefern.

5) Was du mir schicken kannst (empfohlen)
- Die timing_summary.txt (oder der gesamte Logfile) plus progress_sequence.txt.
- Angabe: Cache‑Status (cache vorhanden / gelöscht), verwendetes Binary (Pfad), Build‑Methode (Win64 cross / native Linux).
- Optional: Kurze Beschreibung, welche UI‑Progress‑Meldung am längsten stand.

6) Was ich aus den Logs ableiten werde
- Wenn "Parse/ODF processing" groß ist → Parser / ReadWithProgress noch langsam.
- Wenn "ResolveReferences" groß ist → Referenzauflösung (z. B. in GetReferencingObjects/ResolveReferences) ist der Hotspot.
- Wenn "Preparing audio objects" bis "Loading audio data" groß ist → Audio‑Vorbereitung / Pipe‑Enumerierung oder GOWave‑Initialisierung sind teuer.
- Wenn innerhalb "Building: manual X stops" ein einzelner Stop lange dauert → wir schauen in GOStop::Load und GORank::Load nach Pipes‑/Rank‑Ladeoperationen, die zusätzlichen I/O/Work anstoßen.

7) Optional: zusätzliche kleine Messhilfen (falls du willst)
- Falls du in mehreren Läufen vergleichen willst, notiere Start‑Endzeiten manuell (oder verwende `date +%s%3N` vor/nach dem Klick).
- Wenn du willst, kann ich kleine Änderungen vorschlagen, die sehr niedriges Risiko haben (z. B. zusätzliche pre‑report Aufrufe), danach wieder builden und erneut messen.

Wenn du möchtest, gebe ich jetzt die exakten Shell‑Befehle mit konkreten Beispielen (z. B. für deinen Cross‑Build + Start + grep) — sag "zeige die Befehle".
