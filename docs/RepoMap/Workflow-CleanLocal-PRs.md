# Workflow: Lokal alles versionieren – PRs bleiben „clean“

Ziel: In deinem lokalen Repo darf alles versioniert sein (auch Skripte/Notizen/Experimente). In Pull Requests an das Upstream-Projekt landet nur das, was „sauber“ und projektweit sinnvoll ist.

In Kürze:
- clean (Tracking-Branch von upstream/main) = PR-Basis, keine privaten Commits
- local (nur lokal) = deine privaten/experimentellen Commits, niemals pushen
- feature/* (vom clean abgezweigt) = PR-Zweige; nur ausgewählte Änderungen (ggf. via `cherry-pick` aus `local`)

Wer entscheidet, was in clean gehört?
- Du entscheidest bewusst per „Allow-List“: Nur was du aktiv von `local` in `feature/*` übernimmst (Commit neu erstellen oder gezielt `cherry-pick`), gehört in PRs.
- Richtlinie: 
  - In clean/PR gehört: allgemein nützlicher Code, reproduzierbare Builds, dokumentierte Änderungen, keine Secrets, keine maschinenspezifischen Pfade.
  - Nicht in PR: private Skripte, lokale Build-/IDE-Dateien, Credentials, große Binärdaten, WIP/Notizen, Maschinenpfade/absolute Pfade.

---

## 1) Einmaliges Setup

Voraussetzung: `origin` = dein Fork, `upstream` = Hauptrepo (schon konfiguriert).

```bash
# clean anlegen und auf Upstream setzen
git fetch upstream
git checkout -B clean upstream/main

# local von clean abzweigen (nur lokal, nie pushen)
git checkout -B local clean
# ... hier deine privaten Commits, Tools, Notizen etc.

# optional: Hooks-Verzeichnis verwenden (siehe unten)
git config core.hooksPath .githooks
```

Optional (komfortabel mit parallelen Arbeitsbäumen):
```bash
# Worktrees: sauberen Arbeitsbaum + privater Arbeitsbaum
git worktree add ../repo-clean clean    # sauberes Arbeitsverzeichnis
git worktree add ../repo-local local    # privates Arbeitsverzeichnis
```

---

## 2) Täglicher Ablauf

Updates holen:
```bash
# clean auf aktuellem Upstream halten:
git checkout clean
git fetch upstream
git reset --hard upstream/main

# local auf clean rebasen (private Historie sauber halten)
git checkout local
git rebase clean
```

Neues Feature als PR-Zweig:
```bash
git checkout -b feature/xyz clean
# Variante A: Änderungen neu und „clean“ implementieren
# Variante B: gezielt aus local übernehmen:
#  - commit(s) aus 'git log local' wählen (HASH notieren)
git cherry-pick <HASH1> [<HASH2> ...]
#  - Konflikte lösen, testen, dokumentieren

# an deinen Fork pushen und PR aufmachen
git push origin feature/xyz
```

Wichtig: `local` nie pushen. Nur `feature/*` von `clean` geht zu `origin` und als PR zu `upstream`.

---

## 3) Entscheidungs-Checkliste (gehört das in PR/clean?)

Ja, wenn:
- Es ist allgemein wiederverwendbar (Team/Upstream profitiert).
- Keine Secrets/Token/Schlüssel enthalten.
- Keine maschinenspezifischen Pfade/Abhängigkeiten.
- Build/Test laufen auch bei anderen.
- Die Änderung ist in der Doku/Commit-Message nachvollziehbar.

Nein (nur `local`), wenn:
- Persönliche Tools/Notizen/Workarounds (nur für deine Maschine).
- IDE-/Editor-Cache, generierte Files, temporäre Artefakte.
- Private Daten/Assets, Lizenzunklares Material.
- Experiment/WIP ohne Review-Reife.

Tipp: Die Entscheidung passiert praktisch durch „Explizites Übernehmen“ (neu committen oder cherry-picken). Alles, was du nicht aktiv übernimmst, bleibt automatisch draußen.

---

## 4) Schutzmaßnahmen

A) pre-push Hook (blockiert Push von `local`-Zweigen)

Speicher diese Datei später als `.githooks/pre-push` (ausführbar machen: `chmod +x .githooks/pre-push`), nachdem du `git config core.hooksPath .githooks` gesetzt hast:

```bash
#!/usr/bin/env bash
# Blockt Push von 'local' oder 'local/*' Branches
while read local_ref local_sha remote_ref remote_sha; do
  # Beispielzeile: local_ref="refs/heads/local"
  case "$local_ref" in
    refs/heads/local|refs/heads/local/*)
      echo "Push blockiert: '$local_ref' ist ein lokaler Privat-Branch."
      exit 1
      ;;
  esac
done
exit 0
```

B) .gitignore / export-ignore
- `.gitignore` sorgt dafür, dass generierte/ temporäre Dateien gar nicht erst ins VCS kommen.
- Für Release-Archive (nicht PRs) kann `.gitattributes` mit `export-ignore` genutzt werden, z. B.:
  ```
  docs/local/** export-ignore
  tools/private/** export-ignore
  ```
  Das wirkt bei `git archive`/Release-Paketen, nicht auf PR-Inhalte.

C) Remotes
- Für `local` bewusst keinen Remote eintragen (Versehenspushing vermeiden).
- Optional `push.default=current`, um versehentliche Pushes auf falsche Branches zu reduzieren:
  ```bash
  git config push.default current
  ```

---

## 5) Kurze „Cheat‑Sheet“ Befehle

- Clean aktualisieren:
  ```bash
  git checkout clean
  git fetch upstream
  git reset --hard upstream/main
  ```

- Local auf Clean rebasen:
  ```bash
  git checkout local
  git rebase clean
  ```

- Feature‑PR starten (vom clean):
  ```bash
  git checkout -b feature/xyz clean
  # dann Code schreiben ODER Commits aus local holen:
  git cherry-pick <HASH>
  git push origin feature/xyz
  # PR öffnen
  ```

- Pre‑Push‑Hook aktivieren:
  ```bash
  git config core.hooksPath .githooks
  # Datei .githooks/pre-push anlegen und chmod +x
  ```

---

## 6) Governance (kompakt)

- Entscheider: Du (Repo-Owner) wählst explizit durch „Übernahme“ (Commit neu/cherry-pick), was in PRs/clean darf.
- Review: PRs gegen `upstream` folgen den dortigen Maintainer-Regeln (CI, Code-Review, Style).
- Dokumentation: PR-Beschreibung erklärt, warum die Änderung upstream-tauglich ist; alles weitere bleibt in `local`.

Damit ist „alles versioniert“ (in `local`) möglich, ohne Upstream mit lokalem Kram zu belasten (PRs nur aus `feature/*` von `clean`).
