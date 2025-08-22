# Git Basics: Stash und Hook (Kurz & Praxisnah)

Ziel: Begriffe schnell klären, damit wir den Clean/Local/Feature‑Workflow sicher aufsetzen.

## 1) Was ist ein Stash?
- „Temporärer Zwischenparkplatz“ für noch nicht committete Änderungen.
- Du legst deinen aktuellen Arbeitsstand weg, ohne ihn zu committen. Arbeitsbaum wird sauber.
- Später kannst du den Stash zurückholen (ganz oder teilweise).

Wichtige Befehle:
```bash
# Änderungen (auch untracked) weglegen, mit Notiz:
git stash push -u -m "Kurzbeschreibung (z. B. vor Clean-Setup)"

# Liste vorhandener Stashes
git stash list

# Obersten Stash wiederherstellen UND aus der Liste entfernen
git stash pop

# Stash anwenden (behalten in Liste, z. B. zum Testen):
git stash apply stash@{0}

# Stash löschen (wenn nicht mehr gebraucht)
git stash drop stash@{0}
```

Wann Stash sinnvoll ist:
- Du willst nichts committen (z. B. experimentell, unvollständig).
- Du brauchst kurz einen sauberen Zustand (Branch wechseln, rebasen, Clean-Branch anlegen).

## 2) Was ist ein Hook?
- Ein lokales Skript, das Git zu bestimmten Ereignissen automatisch ausführt (clientseitig).
- Beispiele: `pre-commit`, `pre-push`, `commit-msg` …
- Wir nutzen einen `pre-push`‑Hook, um versehentliche Pushes von `local` zu verhindern.

Ablage/Struktur:
- Standard: `.git/hooks/` (Repo‑intern, aber nicht versioniert).
- Besser: eigenes Hooks‑Verzeichnis (versioniert) + Konfiguration:
  ```bash
  git config core.hooksPath .githooks
  ```
- Hook-Dateien müssen ausführbar sein (`chmod +x`).

Beispiel (pre‑push Hook, bereits im Repo hinterlegt):
```
.githooks/pre-push
```
- Verhindert `git push` auf Branch `local` oder `local/*`.
- Aktivierung:
  ```bash
  git config core.hooksPath .githooks
  chmod +x .githooks/pre-push
  ```

## 3) WIP‑Commit vs. Stash – wann nutze ich was?

- WIP = „Work In Progress“ (Zwischenstands‑Commit)
  - Vorteile: 
    - Im Verlauf sichtbar, später leicht per `git cherry-pick` übernehmbar.
    - Besser für „lokal alles versioniert“ (History bleibt vollständig).
  - Nachteile: 
    - Ein zusätzlicher Commit (später ggf. „squashen“/umbenennen).

- Stash:
  - Vorteile:
    - Keine Commits nötig, schnell „sauber“.
  - Nachteile:
    - Nicht im Commit‑Verlauf sichtbar, weniger „greifbar“ zum Cherry‑Pick.

Empfehlung für unseren Workflow:
- WIP‑Commit, wenn der Stand später selektiv in PRs einfließen könnte.
- Stash, wenn du nur kurz etwas weglegen musst und daraus voraussichtlich nichts in PRs wandert.

## 4) Bezug zum Clean/Local/Feature‑Workflow
- `local` enthält deine vollständige, private Historie (inkl. WIP‑Commits).
- `clean` spiegelt den Upstream‑Stand (PR‑Basis).
- `feature/*` wird von `clean` abgezweigt; aus `local` werden nur explizit ausgewählte Commits per `cherry-pick` übernommen (Allow‑List).

Kurzanleitung (Option A, zusammengefasst):
```bash
# A1) WIP-Snapshot (alles sichern)
git add -A
git commit -m "WIP: snapshot before clean/local setup"

# A2) Aktuellen Branch in 'local' umbenennen (lokal, kein Push)
git branch -m local

# A3) Clean-Branch von origin/master (PR-Basis)
git fetch origin
git checkout -B clean origin/master

# A4) (Optional) Hook aktivieren, um Push von local/* zu blockieren
git config core.hooksPath .githooks
chmod +x .githooks/pre-push
```

Rollback‑Hinweise:
```bash
# WIP-Commit auf 'local' zurücknehmen (behalte Änderungen im Arbeitsbaum):
git checkout local
git reset --soft HEAD^    # oder --mixed, wenn Dateien wieder als geändert markiert sein sollen

# Branch-Namensänderung rückgängig:
git branch -m cline-experimente
