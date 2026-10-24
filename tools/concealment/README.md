# Concealment / Xorcist — Arbeitsverzeichnis

Prosa, Quellen und Fremdmaterial zur Deadline-Miss-Verdeckung. Der Code
liegt in `src/xorcist/` (eigenständige C-Bibliothek) und
`src/grandorgue/sound/GOSoundConcealment.*` (dünner Engine-Adapter), das
Hörlabor unter `pwdsp/experiments/gap_lab.py`.

Hier hinein gehört alles, was **nicht** Code ist: Herleitungen,
Vorlesungsnotizen, Gesprächsmitschriften, Papers, Skizzen. Getrackt, also
branchfest und in jedem Worktree sichtbar — anders als `docs/`, das über
`.git/info/exclude` bewusst außerhalb der Versionierung liegt.

## Ablagekonvention

    tools/concealment/
      README.md                  dieses Dokument
      notes/<datum>-<thema>.md   Mitschriften, Herleitungen, Gespräche
      papers/                    PDFs und externe Quellen

Bei Mitschriften vorne kurz vermerken, woher sie stammen und was der
Kernbefund ist — dann muss man beim Suchen nicht das ganze Dokument lesen.

## Prony pro Bin — Stand nach der Mathevorlesung (04.08.2026)

Der Phasenvocoder schreibt jeden Bin mit einer Rotation der Ordnung 1
fort, `X(n) = z·X(n−1)` mit `|z| = 1`. Das ist für **eine** Schwingung
pro Bin exakt und für zwei strukturell falsch. Die Verallgemeinerung ist
eine Rekursion der Ordnung M, deren Wurzeln die Rotationen der M
Teilschwingungen sind (Prony pro Bin, Prototyp in `gap_lab.prony_cont`).

Die erste Fassung des Prototyps trug einen Ridge ρ = 10⁻³ und maß damit
kaum besser als Ordnung 1. Die Vorlesung
([notes/mathevorlesung.md](notes/mathevorlesung.md)) hat das seziert:
bei zwei nahen Frequenzen ist die Lösung ein Paar großer, fast
auslöschender Koeffizienten (a ≈ 2z, b ≈ −z²), deren *Differenz* die
Information trägt — und genau die schrumpft der Ridge, zurück auf
AR(1). ρ muss unter dem Kehrwert der Konditionszahl liegen (~10⁻⁴ bei
6 Hz Abstand); 10⁻³ hat das Signal wegregularisiert. Die eigentliche
Absicherung ist die Wurzelbehandlung (nur Wurzeln nahe dem Einheitskreis
werden daraufgeschnappt, Scheinwurzeln sterben aus).

Nachgemessen in der vollen Kette (Anker-Korrektor, Safety-Cap,
selbstfütternder Ring, 600 Fehlschläge/min):

|                            | 2 Sinus | 1 Ton | voller Akkord | Attacks | Vierne |
|----------------------------|--------:|------:|--------------:|--------:|-------:|
| AR(1)-Rotator (Engine)     |    −2,1 | −14,5 |          −5,6 |   −13,7 |   −3,0 |
| AR(2) Hop 128/n16, ρ=10⁻³  |    −1,7 | −14,2 |         −12,3 |   −13,8 |   −2,3 |
| AR(2) Hop 128/n16, ρ=0     |   −66,1 | −21,6 |         −14,5 |   −20,8 |   −3,2 |
| **AR(2) Hop 512/n14, ρ=0** |   −95,8 | −22,6 |         −17,4 |   −22,0 |   −3,8 |
| AR(2) Hop 1024/n8, ρ=0     |  −104,1 | −21,8 |         −19,0 |   −21,3 |   −3,4 |

**Der Lehrbuchfall (147/153 Hz in einem Bin) ist gelöst**, nicht
verbessert. Die frühere Begründung — „eine 6-Hz-Schwebung braucht
~167 ms Beobachtung, unabhängig vom Verfahren" — ist damit widerlegt;
sie beschrieb den Ridge-Bias, keine Informationsgrenze. Zwei weitere
Resultate der Vorlesung, beide in der Kette bestätigt:

- **Der Analyse-Hop ist bei Ordnung 2 der Hebelarm der Trennung** (die
  Wurzeln liegen Δω·Hop auseinander): grober Hop konditioniert besser.
  Bei Ordnung 1 gilt das Gegenteil — dort bleibt Hop 128 richtig.
- **Die echte Grenze ist Identifizierbarkeit an Einsätzen:** ein Ton,
  der ε vor der Lücke einsetzt, ist beobachtungsgleich mit einer
  Amplitudenrampe; im Produkt B·Δω sind die Faktoren einzeln nicht
  bestimmbar. Keine Methode sagt diese Schwebung vorher.

Offen vor einem C-Port: das **Ordnungs-Gate pro Bin** (Rückfall auf
AR(1), wo das 2×2-System entartet ist — sonst wird Rauschen angepasst
und extrapoliert), die RLS-Form mit Vergessensfaktor λ ≈ 0,984 für die
Engine (fünf komplexe Akkumulatoren pro Bin, Lösen nur pro Lücke;
float32 braucht double-Akkumulatoren oder Neuskalierung), und die Frage,
wie sich die lange Spanne unter dichter Selbstfütterung jenseits von
600/min hält.
