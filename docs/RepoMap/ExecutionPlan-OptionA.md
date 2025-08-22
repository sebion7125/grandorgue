# Execution Plan — Option A (Clean/Local Setup, sicher & schrittweise)

Ziel: Deinen aktuellen Stand lokal sichern, den Branch in „local“ überführen und einen sauberen PR‑Basis‑Branch „clean“ von origin/master anlegen. Keine Remote‑Änderungen. Alles gemäß Policy „Erst Plan, dann Freigabe, dann Aktion“.

Wichtige Hinweise
- Keine destruktiven Befehle.
- Keine Rewrites der Historie anderer Branches.
- Keine Pushes, keine Remote‑Änderungen.
- Der vorbereitete pre‑push‑Hook verhindert versehentliche Pushes von „local“.

Voraussetzungen (gegeben)
- Remotes: `origin` und `upstream` zeigen auf https://github.com/GrandOrgue/grandorgue.git
- Aktueller Branch: `cline-experimente` mit uncommitted Änderungen

Schritt 0 — Read‑only Checks (bereits durchgeführt)
```bash
git remote -v
git status -sb
git branch -vv
```

Schritt 1 — WIP‑Snapshot als lokaler Sicherungspunkt
- Zweck: Deinen Arbeitsstand versionieren, damit nichts verloren geht; später leicht per `cherry-pick` in PR‑Zweige übernehmbar.
- Wirkung: Ein zusätzlicher lokaler Commit auf dem aktuellen Branch.
- Befehle:
```bash
git add -A
git commit -m "WIP: snapshot before clean/local setup"
```
Rollback (falls unerwünscht):
```bash
git reset --soft HEAD^   # behält Änderungen im Arbeitsbaum
# oder
git reset --mixed HEAD^  # markiert Dateien wieder als geändert
```

Schritt 2 — Branch in „local“ umbenennen (bleibt rein lokal)
- Zweck: „local“ wird dein persönlicher, vollständiger Verlauf (inkl. WIP, Tools, Notizen).
- Wirkung: Nur Namensänderung des aktuellen Branches.
- Befehle:
```bash
git branch -m local
```
Rollback:
```bash
git branch -m cline-experimente
```

Schritt 3 — Sauberen PR‑Basis‑Branch „clean“ anlegen
- Basis: origin/master (Upstream‑Stand)
- Wirkung: „clean“ spiegelt den Projektstand für PRs, ohne lokale Zusätze.
- Befehle:
```bash
git fetch origin
git checkout -B clean origin/master
```
Rollback:
```bash
git checkout local
git branch -D clean   # löscht clean (nur lokal)
```

Schritt 4 — Pre‑push‑Hook aktivieren (Schutz vor Push von „local“)
- Der Hook liegt bereits vor: `.githooks/pre-push`
- Befehle:
```bash
git config core.hooksPath .githooks
chmod +x .githooks/pre-push
```
Rollback:
```bash
git config --unset core.hooksPath
```

Schritt 5 — Verifikation (read‑only)
```bash
git branch -vv                  # Erwartet: * clean (tracking auf origin/master), local (ohne Tracking)
git config --get core.hooksPath # Erwartet: .githooks
ls -l .githooks/pre-push        # Erwartet: ausführbar
```

Wie geht’s danach weiter? (PR‑Ablauf)
- PR‑Zweig immer von clean:
```bash
git checkout -b feature/xyz clean
# „Saubere“ Änderungen neu implementieren
# oder gezielt aus local übernehmen:
git cherry-pick <COMMIT_HASH>
git push origin feature/xyz
```
- PR öffnen gegen das Hauptrepo (oder Fork, sobald konfiguriert).

FAQ
- Warum WIP‑Commit statt Stash? WIP‑Commit bleibt im Verlauf sichtbar und ist einfach cherry‑pick‑bar. Genau passend für „lokal alles versioniert“.
- Warum origin/master als Basis? In diesem Repo zeigen `origin` und `upstream` auf denselben URL; `master` ist der veröffentlichte Projektstand. Falls künftig ein Fork genutzt wird, kann „clean“ auf `upstream/master` umgestellt werden.

Sicherheitsnetz
- Jeder Schritt ist reversibel (siehe Rollbacks).
- Keine Befehle machen Remote‑Änderungen oder zerstören Historie.
- Pre‑push‑Hook schützt vor versehentlichem Push von „local“.

Genehmigung
- Bitte freigeben: „Option A ausführen“.
- Nach Freigabe führe ich die Schritte 1–4 aus und danach Schritt 5 (Verifikation).
