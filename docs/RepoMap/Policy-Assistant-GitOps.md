# Policy: „Erst Plan, dann Freigabe, dann Aktion“ für Git-/Repo‑Operationen

Gültig für alle von der Assistenz vorgeschlagenen oder ausgeführten Befehle (Git, Dateien, Build, CI).

1) Plan vor Aktion
- Die Assistenz beschreibt vorab:
  - Ziel / erwartetes Ergebnis
  - konkrete Befehle (inkl. Flags) in Ausführungsreihenfolge
  - Auswirkungen/Risiken und Rückfallplan (Rollback/Revert)
- Ohne ausdrückliche Freigabe werden keine Befehle ausgeführt.

2) Read‑only zuerst
- Zunächst nur lesende Prüfungen (z. B. git status, git remote -v, git branch -vv).
- Ergebnisse werden gezeigt; der nächste Schritt wird erneut bestätigt.

3) Schrittweise, minimalinvasiv
- Änderungen in kleinen, überprüfbaren Schritten.
- Nach jedem Schritt Stopp und Bestätigung einholen.

4) Keine destruktiven Aktionen ohne Rollback
- Befehle wie reset --hard, rebase, clean, rm etc. nur nach expliziter Freigabe und mit beschriebenem Rollback (z. B. Reflog/Backupbranch).

5) Branch‑Modell: clean / local / feature/*
- Einrichtung/Änderung dieses Modells erfolgt nur nach Freigabe.
- local bleibt rein lokal (kein Push).
- PRs entstehen aus feature/*, abgezweigt von clean.

6) Hooks/Configs
- Änderungen an git config, Hooks, .gitignore/.gitattributes erfolgen nur nach Freigabe.
- Standard: pre‑push‑Hook gegen Push von local/* (wenn genehmigt).

7) Geheimnisse & Pfade
- Keine Secrets/Token anfassen oder in Logs zeigen.
- Keine maschinenspezifischen Pfade in PR‑Branches einführen.

8) Langläufer
- Länger laufende Prozesse (Build/Tests) werden angekündigt; Start nur nach Zustimmung.

9) Dokumentation
- Relevante Prozess‑/Governance‑Änderungen werden unter docs/RepoMap gepflegt (z. B. Workflow‑CleanLocal‑PRs.md).

10) Abweichungen
- Jede Abweichung von dieser Policy wird vorab begründet und separat genehmigt.

Erster Standard‑Schritt bei Git‑Hilfe:
- Read‑only Sichtprüfung: `git remote -v`, `git status`, `git branch -vv`.
