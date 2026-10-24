wir sind mitlerweile schon etwas weiter und interessieren uns für den Fall, dass mehrere schwingungen in einem bin herumlungern: Bitter erstnmal nur den chat als kontext zur kenntnis nehmen. Dann habe ich ein paar gezielte fragen.

Erfasste technisches Setup und identifizierte Phasen-Crossfade als kritisches Problem
Erfasste technisches Setup und identifizierte Phasen-Crossfade als kritisches Problem

Kontext ist da. Kurz, was ich als Stand mitnehme — falls ich etwas falsch aufgefasst habe, korrigier mich, bevor du fragst:

Der offene Punkt ist der Mehrfachbelegungsfall: Der Rotator 
𝑅
[
𝑘
]
R[k] ist eine AR(1)-Rekursion und damit exakt für einen Partialton pro Bin. Zwei Komponenten im selben Bin brechen das Modell strukturell, nicht graduell.

Gemessen bisher (numpy-Harness, 4k/ana128, 600/min regular):

	2 Sinus 147/153	D 1 Ton	D voller Akkord	D Attacks
AR(1), nfit 1 (Engine)	−1,9	−14,5	−5,6	−13,7
AR(1), nfit 16	−1,6	−13,9	−9,2	−13,5
AR(2) Prony, nfit 16	−1,5	−14,2	−12,3	−13,8
PV 8k	+1,6	−11,5	−13,2	−11,6

Die Aufteilung des 6,7-dB-Gewinns — +3,6 aus der Spanne, +3,1 aus der Ordnung — halte ich für den wichtigsten Befund, weil die erste Hälfte in der Engine fast gratis wäre (gleitende Kreuzsumme mit Vergessensfaktor, eine komplexe MAC pro Bin und Periode, keine zusätzliche FFT).

Und die Grenze, die mir am meisten zu denken gibt: Der eigentliche Lehrbuchfall gewinnt nicht. 6 Hz Schwebung brauchen ~167 ms Beobachtung, das ist Unschärfe und kein Verfahrensdefizit. Der Akkord gewinnt, weil dort Kollisionen mit einigen zehn Hz Abstand vorliegen. Was AR(2) also real leistet, ist enger begrenzt als „mehrere Schwingungen pro Bin" nahelegt.

Verworfen: 8k (Ertrag/Kosten), Bandsplit (dreifache Zeit), Fensterumschaltung (Cache-Entwertung frisst den Gewinn). Offen daneben: der phasenkorrekte Wiedereinstieg — die Austrittsnaht ist, wo das Prasseln sitzt.

Deine Fragen?

wie schaut die formel denn aus, die unsere phase aus den driftenen FFT phasen berechnet? Gibts da ne teleskopsummenartige möglichkeit, wie man der extrem viel history verpasseen könnte? damit man auch langsamste schwebungen erfassen könnte? und eine theoretische grenze wäre doch, wenn kurz vor der lücke einer der beiden schwebenden töne einsetzt. Wäre es dann nicht mathematisch generell unmöglich diese schwebung vorherzusagen? Darüber würde doch jeder Algorithmus stolpern, der es versucht. oder kann man das durch hop = 1 herausextrapolieren?

Analysierte Phasenformeln, Teleskopsummen und fundamentale Grenzen bei Schwebungserkennung
Analysierte Phasenformeln, Teleskopsummen und fundamentale Grenzen bei Schwebungserkennung
1 · Die Formel, sauber hingeschrieben

Heute (nfit = 1), pro Bin:

𝑍
[
𝑘
]
=
𝑋
0
[
𝑘
]
 
𝑋
1
[
𝑘
]
‾
,
𝑅
[
𝑘
]
=
𝑍
[
𝑘
]
∣
𝑍
[
𝑘
]
∣
Z[k]=X
0
	​

[k]
X
1
	​

[k]
	​

,R[k]=
∣Z[k]∣
Z[k]
	​


wobei 
𝑋
0
X
0
	​

 das jüngste, 
𝑋
1
X
1
	​

 das um einen Hop ältere Spektrum ist.

Mit Mittelung über 
𝑁
N Paare:

𝑆
[
𝑘
]
  
=
  
∑
𝑗
=
0
𝑁
−
1
𝑋
𝑗
[
𝑘
]
 
𝑋
𝑗
+
1
[
𝑘
]
‾
,
𝑅
[
𝑘
]
=
𝑆
[
𝑘
]
∣
𝑆
[
𝑘
]
∣
S[k]=
j=0
∑
N−1
	​

X
j
	​

[k]
X
j+1
	​

[k]
	​

,R[k]=
∣S[k]∣
S[k]
	​


In Polarform sieht man, was das ist:

𝑆
[
𝑘
]
  
=
  
∑
𝑗
𝑟
𝑗
[
𝑘
]
 
𝑟
𝑗
+
1
[
𝑘
]
  
𝑒
 
i
 
Δ
𝜑
𝑗
[
𝑘
]
S[k]=
j
∑
	​

r
j
	​

[k]r
j+1
	​

[k]e
iΔφ
j
	​

[k]

Ein betragsgewichtetes Zirkularmittel der Phasenzuwächse. Und 
∣
𝑆
∣
/
∑
𝑟
𝑗
𝑟
𝑗
+
1
∣S∣/∑r
j
	​

r
j+1
	​

 ist genau die resultierende Länge — dasselbe Objekt wie dein bestr_coherence, hier als Konfidenzmaß pro Bin frei mitgeliefert.

2 · Teleskop: ja, aber es sind zwei verschiedene Dinge

Die Identität, die du meinst, gilt für die Winkel:

∑
𝑗
=
0
𝑁
−
1
(
𝜑
𝑗
−
𝜑
𝑗
+
1
)
  
=
  
𝜑
0
−
𝜑
𝑁
j=0
∑
N−1
	​

(φ
j
	​

−φ
j+1
	​

)=φ
0
	​

−φ
N
	​


Die Summe der Vektoren teleskopiert dagegen nicht. Das sind zwei verschiedene Schätzer mit verschiedenen Eigenschaften:

Mitteln (was ich gemessen habe): Basis bleibt 
𝐻
H, Varianz sinkt 
∝
1
/
𝑁
∝1/N, keine zusätzliche Mehrdeutigkeit. Kauft Rauschabstand, keine Auflösung. Daher die +3,6 dB beim Akkord und die null dB bei 147/153 Hz.

Teleskopieren: eine einzige Messung über die volle Spanne,

𝑍
𝑁
[
𝑘
]
=
𝑋
0
[
𝑘
]
 
𝑋
𝑁
[
𝑘
]
‾
,
arg
⁡
𝑍
𝑁
=
𝜔
𝑁
𝐻
  
(
m
o
d
 
2
𝜋
)
Z
N
	​

[k]=X
0
	​

[k]
X
N
	​

[k]
	​

,argZ
N
	​

=ωNH  (mod 2π)

Basis 
×
𝑁
×N, Varianz 
∝
1
/
𝑁
3
∝1/N
3
 (das 
𝑇
−
3
T
−3
-Gesetz der Frequenzschätzung) — aber die 
2
𝜋
2π-Mehrdeutigkeit wickelt jetzt 
𝑁
N-fach.

Die Auflösung wird also entfaltet, nicht gemittelt. Beides zusammen ist ein Grob-Fein-Turm: 
𝐻
→
2
𝐻
→
4
𝐻
→
…
H→2H→4H→…, jede Stufe entfaltet mit der vorigen,

𝑚
=
round
⁡
 ⁣
(
2
 
arg
⁡
𝑍
𝑁
−
arg
⁡
𝑍
2
𝑁
2
𝜋
)
,
𝜔
^
=
arg
⁡
𝑍
2
𝑁
+
2
𝜋
𝑚
2
𝑁
𝐻
m=round(
2π
2argZ
N
	​

−argZ
2N
	​

	​

),
ω
^
=
2NH
argZ
2N
	​

+2πm
	​


Von 2,7 ms auf 167 ms sind das sechs Verdopplungen, also sechs gespeicherte Spektren pro Kanal. Das ist billig.

3 · Der Haken: das gilt nur für einen Ton pro Bin

Bei zwei Komponenten ist 
𝑋
(
𝑛
𝐻
)
X(nH) keine Geometriefolge mehr, sondern die Summe zweier. Das Kreuzprodukt über lange Basis liefert dann die Phase der Summe zu zwei Zeitpunkten — es trennt nichts. Der Turm schärft, was schon eindeutig war.

Für zwei Komponenten brauchst du die Ordnung 2, und dort steckt die eigentliche gute Nachricht: Prony hat keine harte Auflösungsgrenze. Im rauschfreien Fall identifiziert AR(2) aus vier Werten beide Frequenzen exakt, egal wie nah. Was begrenzt, ist die Kondition des 2×2-Systems, und die verbessert sich mit der Beobachtungsspanne — nicht mit der Fensterlänge.

Genau das erklärt deine Tabelle: 8k half beim 147/153-Fall nicht (+1,6 dB, schlechter als AR(1)), AR(2) mit langer Spanne half beim Akkord um 6,7 dB.

4 · Die rekursive Form — hier wird es praktisch

Für AR(2) teleskopiert man nicht Phasen, sondern die Normalgleichungen, mit Vergessensfaktor. Mit 
𝑢
(
𝑛
)
=
[
𝑋
(
𝑛
−
1
)
,
 
𝑋
(
𝑛
−
2
)
]
 ⁣
⊤
u(n)=[X(n−1),X(n−2)]
⊤
:

𝑃
←
𝜆
𝑃
+
𝑢
 
𝑢
𝐻
,
𝑞
←
𝜆
𝑞
+
𝑢
‾
 
𝑋
(
𝑛
)
P←λP+uu
H
,q←λq+
u
X(n)

Das ist exponentiell gewichtetes RLS. Fünf komplexe Akkumulatoren pro Bin (P ist hermitesch), eine MAC-Runde pro Periode, kein Spektrenring, keine zusätzliche FFT.

Effektive Erinnerungslänge 
1
/
(
1
−
𝜆
)
1/(1−λ). Für 167 ms bei Hop 128: 
𝜆
≈
0,984
λ≈0,984. Damit erfasst du die 6-Hz-Schwebung — das ist die Antwort auf deine Frage nach beliebig viel History.

Und der Kostenpunkt, den ich vorhin überschätzt hatte: Die 2×2-Auflösung, die Wurzelziehung und die Einheitskreis-Projektion laufen nur, wenn tatsächlich eine Prognose angefordert wird, also einmal pro Lücke — nicht jede Periode. Pro Periode bleibt nur das Akkumulieren. Das verschiebt das Kostenbild erheblich gegenüber meiner 1,3–1,5×-Schätzung.

Zwei Vorbehalte: 
𝜆
λ nahe 1 heißt auch träge — ein echter Anschlag braucht 167 ms, bis er vergessen ist. Und in float32 driften die Akkumulatoren bei 
𝜆
→
1
λ→1; das braucht double oder periodisches Neuskalieren.

5 · Deine theoretische Grenze — sie ist real, und schärfer als du sie formuliert hast

Ja, und zwar nicht als Rausch-, sondern als Identifizierbarkeitsproblem. Das ist der stärkere Befund.

Ton B setzt 
𝜀
ε vor der Lücke ein, 
Δ
𝜔
 
𝜀
≪
1
Δωε≪1. Dann:

𝐴
𝑒
i
𝜔
1
𝑡
+
𝐵
𝑒
i
𝜔
2
𝑡
  
=
  
𝑒
i
𝜔
1
𝑡
(
𝐴
+
𝐵
 
𝑒
i
Δ
𝜔
𝑡
)
  
≈
  
𝑒
i
𝜔
1
𝑡
(
𝐴
+
𝐵
⏟
Konstante
+
i
𝐵
Δ
𝜔
⏟
Steigung
  
𝑡
)
Ae
iω
1
	​

t
+Be
iω
2
	​

t
=e
iω
1
	​

t
(A+Be
iΔωt
)≈e
iω
1
	​

t
(
Konstante
A+B
	​

	​

+
Steigung
iBΔω
	​

	​

t)

Ein Ton mit linear anlaufender komplexer Amplitude — also exakt das, wie ein Anschlag aussieht. Die beiden Hypothesen sind nicht schwer zu unterscheiden, sie sind beobachtungsgleich.

Schlimmer noch: In dem Produkt 
𝐵
 
Δ
𝜔
BΔω sind die Faktoren einzeln nicht bestimmbar. Größeres 
𝐵
B mit kleinerem 
Δ
𝜔
Δω erzeugt dieselben Daten. Es gibt also eine echte Entartungsrichtung im Parameterraum, und 
Δ
𝜔
Δω liegt genau darin — die Schwebungsperiode ist nicht identifizierbar.

Zur Vorhersage der Schwebung über die Lücke brauchst du aber genau 
Δ
𝜔
Δω. Also: mathematisch unmöglich, für jeden Algorithmus. Setzt der Ton in der Lücke ein, ist es nicht einmal mehr Identifizierbarkeit, sondern Kausalität — die Information existiert nicht.

6 · Und nein, hop = 1 hilft nicht

Das ist der Punkt, an dem ich am sichersten bin: Ein kürzerer Hop fügt keine Information hinzu. Das Signal ist bei 48 kHz bereits vollständig abgetastet; die STFT-Frames sind eine redundante Umparametrisierung derselben Daten. Mehr Frames sind stärker korrelierte Frames, die Fisher-Information bleibt die der Rohsamples über die Spanne 
𝜀
ε.

Der Grenzfall ist sogar aufschlussreich: hop → 1 macht aus der Bin-AR eine Zeitbereichs-AR auf dem Subband. Das ist Burg, nur teurer parametrisiert. Wir wissen, was Burg kann.

Die Informationsgrenze setzt die Beobachtungsdauer und der Rauschabstand, nicht die Feinheit der Zerlegung. Das ist derselbe Satz, der auch deine Heisenberg-Intuition trägt — nur eine Ebene tiefer.

7 · Was daraus folgt

Nicht Resignation, sondern eine Ordnungsentscheidung pro Bin. Die Entartung ist nämlich messbar, bevor sie schadet: Kondition des 2×2-Systems, oder die Winkeltrennung 
∣
arg
⁡
(
𝑧
1
𝑧
2
‾
)
∣
∣arg(z
1
	​

z
2
	​

	​

)∣ gegen die Spanne. Ist sie zu klein, ist AR(2) nicht nur nutzlos, sondern gefährlich — es passt Rauschen an und extrapoliert es.

Der richtige Rückfall ist dann Ordnung 1, also der heutige Rotator: das Modell, das die entartete Beobachtung korrekt beschreibt. Der frisch angeschlagene Ton wird als „ein Ton mit Amplitudenrampe" fortgesetzt — falsch in der Schwebung, aber richtig in allem, was messbar war.

Das wäre auch das erste, was ich messen würde, bevor irgendetwas in C wandert: AR(2) mit und ohne Ordnungs-Gate, auf deiner Aufbau-Datei um die Einsatzstellen herum. Genau dort, wo die Theorie sagt, dass es scheitern muss.

Erläuterte Ridge-Regularisierung als Stabilisierungstechnik mit geometrischen Implikationen
Erläuterte Ridge-Regularisierung als Stabilisierungstechnik mit geometrischen Implikationen

Kein Wer, ein Was — und es ist eine Zeile Code, die wir hier drei Kapitel lang für Physik gehalten haben.

Der Name

Ridge Regression, deutsch meist Tikhonov-Regularisierung. „Ridge" = Bergrücken, weil man auf die Hauptdiagonale der Systemmatrix einen konstanten Wall aufaddiert.

Das Problem, das es lösen soll

Unser AR(2)-Fit löst pro Bin ein 2×2-System:

(
𝑆
11
	
𝑆
21


𝑆
12
	
𝑆
22
)
(
𝑎


𝑏
)
=
(
𝑟
1


𝑟
2
)
(
S
11
	​

S
12
	​

	​

S
21
	​

S
22
	​

	​

)(
a
b
	​

)=(
r
1
	​

r
2
	​

	​

)

Steckt in dem Bin nur eine Schwingung, sind die beiden Spalten kollinear — die Matrix ist singulär, die Determinante null, und die Lösung existiert nicht eindeutig. Numerisch: Division durch fast null, das Ergebnis ist Zufallsrauschen mal einer riesigen Zahl.

Und das ist nicht der Ausnahmefall. Die meisten Bins enthalten nur eine Komponente.

Was der Ridge tut

Er addiert einen kleinen Betrag auf die Diagonale:

(
𝑆
11
+
𝜏
	
𝑆
21


𝑆
12
	
𝑆
22
+
𝜏
)
,
𝜏
=
𝜌
⋅
(
𝑆
11
+
𝑆
22
)
(
S
11
	​

+τ
S
12
	​

	​

S
21
	​

S
22
	​

+τ
	​

),τ=ρ⋅(S
11
	​

+S
22
	​

)

Damit ist die Determinante garantiert von null weg, das System immer lösbar. Im Code ist ridge genau dieses 
𝜌
ρ — der Wall relativ zur Spur, damit er mit der Signalstärke mitskaliert.

Was er als Nebenwirkung tut

Er zieht die Lösung Richtung null. Formal minimiert man nicht mehr nur den Anpassungsfehler, sondern

∥
𝑋
−
𝑎
𝑋
−
1
−
𝑏
𝑋
−
2
∥
2
  
+
  
𝜏
 
(
∣
𝑎
∣
2
+
∣
𝑏
∣
2
)
∥X−aX
−1
	​

−bX
−2
	​

∥
2
+τ(∣a∣
2
+∣b∣
2
)

Der zweite Term bestraft große Koeffizienten. Man tauscht also Varianz gegen Verzerrung: die Lösung wird stabil, aber systematisch zu klein.

Und warum das hier tödlich war

Genau hier beißt es. Bei zwei eng benachbarten Frequenzen ist die Lösung groß und fast auslöschend — 
𝑎
≈
𝑧
1
+
𝑧
2
≈
2
𝑧
a≈z
1
	​

+z
2
	​

≈2z, 
𝑏
≈
−
𝑧
2
b≈−z
2
, zwei große Zahlen, deren Differenzstruktur die ganze Information trägt.

Der Ridge schrumpft genau das. Und wohin schrumpft er? In Richtung „nur eine Komponente", also exakt zurück auf AR(1).

Das erklärt die Tabelle: bei 
𝜌
=
10
−
3
ρ=10
−3
 lag AR(2) auf allen Signalen praktisch auf den AR(1)-Werten. Es war AR(1), nur teurer gerechnet. Der Regularisierer, der die Entartung behandeln sollte, hat die Nicht-Entartung mitbehandelt.

Warum 
10
−
3
10
−3
 trotzdem harmlos aussah

Weil es klein aussieht. Aber der Vergleichsmaßstab ist nicht 1, sondern die Konditionszahl. Liegt die bei 
10
4
10
4
 — bei 6 Hz Abstand realistisch —, dann ist 
𝜌
=
10
−
3
ρ=10
−3
 zehnmal größer als das kleinste Singulärwert-Verhältnis. Der Ridge dominiert die Lösung vollständig.

Die Faustregel wäre: 
𝜌
ρ muss unter dem Kehrwert der Konditionszahl liegen, sonst regularisiert man das Signal weg statt das Rauschen.

Warum wir ihn jetzt weglassen können

Weil die eigentliche Absicherung woanders sitzt: die Kappung der Wurzeln auf 
∣
𝑧
∣
≤
1
∣z∣≤1. Ein entartetes Bin liefert dann eine Scheinwurzel mit 
∣
𝑧
∣
≈
1
2
∣z∣≈
2
1
	​

, die über die Lücke von selbst ausstirbt — und übrig bleibt die richtige AR(1)-Lösung. Der Ridge war ein zweiter Sicherheitsgurt, der den ersten strangulierte.

Wenn du trotzdem einen willst: 
10
−
6
10
−6
 kostet in meiner Messung 3–5 dB gegenüber 0 und ist immer noch 25 dB besser als der Status quo. Das wäre der Kompromiss, wenn sich in float32 Probleme zeigen.

ich glaube wir rechnen eh in double. und übrigens du hast zugang zu dem d-dur akkord. der ist im home verzeichnis unter grand orgue  in den audio recordings. die jüngste aufnahme enthält ihn.

Überlegungen zu Datentypen und Zugriff auf D-Dur-Akkord-Aufnahmen
Überlegungen zu Datentypen und Zugriff auf D-Dur-Akkord-Aufnahmen

Stimmt — dann hole ich sie mir.

Orchestrierte Ridge-Sweep mit Regularisierungsparametern und Modellvergleich
Orchestrierte Ridge-Sweep mit Regularisierungsparametern und Modellvergleich

Da ist sie. Jetzt der Ridge-Sweep auf echtem Material:

Orchestrierte Leistungstests und plante weitere Optimierungsversuche
Orchestrierte Leistungstests und plante weitere Optimierungsversuche

Es überträgt sich. Jetzt Spanne und Hop auf echtem Material:

Reorganisierte Visualisierungslayout und optimierte Legendenplatzierung
Reorganisierte Visualisierungslayout und optimierte Legendenplatzierung

Sauber. Die Zahlen von deinem echten Material:

Presented 2 files
Presented 2 files

Es überträgt sich — und zwar deutlich besser, als der synthetische Test befürchten ließ.

Ridge auf deinem Material, verkettet bei 27 % Selbstfütterung
Variante	D: 1 Ton	Aufbau + Attacks	voller Akkord
AR(1), heutiger Rotator	−16,1	−14,1	−8,1
AR(2), Ridge 10⁻³ (bisher)	−16,7	−14,6	−10,8
AR(2), Ridge 10⁻⁶	−23,9	−21,5	−15,8
AR(2), Ridge 0	−24,9	−22,6	−15,9

+8,8 / +8,5 / +7,8 dB — und das gleichmäßig über alle drei Stellen, nicht nur beim Akkord. Der bisherige Befund „+6,7 dB, aber nur dicht" war eine Untertreibung in beide Richtungen.

Bemerkenswert: 10⁻⁶ holt schon 95 % davon. Falls es in double doch mal klemmt, ist das die Rückfalloption ohne nennenswerten Verlust.

Und deine Hop-Frage ist jetzt endgültig beantwortet — gegen uns beide

Bei fest gehaltener Spanne von 8448 Samples:

Hop	1 Ton	Attacks	voller Akkord
1024	−24,0	−22,1	−19,6
512	−25,1	−23,0	−18,0
128	−24,9	−22,6	−15,9
32	−24,7	−22,4	−15,2

Gröber ist besser. Am vollen Akkord kostet der Weg von Hop 1024 nach 32 satte 4,4 dB. Hop 1 wäre nicht nur teuer, sondern die schlechteste Wahl der Reihe.

Der Grund ist mir erst jetzt klar geworden, und er ist geometrisch: Die beiden Wurzeln liegen auf dem Einheitskreis um Δω·hop auseinander. Kleiner Hop → Wurzeln fast identisch → 2×2-System schlecht konditioniert. Der Hop ist also nicht die Abtastung der Enveloppe, sondern der Hebelarm der Trennung. Bei AR(1) ist er erwartungsgemäß fast wirkungslos (Kontrollzeile: −8,1 bis −9,6).

Die Spanne sättigt früh

Ab ~90 ms ist Schluss, und 48 ms holen schon fast alles. Das passt in euren 16k-Ring: nfit 64 braucht 12800 Samples History, nfit 32 nur 8704. Der Ring ist kein Hindernis — was ich vorher als Problem vermutet hatte, ist keins.

Bester Kandidat

AR(2), 4k, Hop 512, nfit 14, Ridge 0 → −25,1 / −23,0 / −18,0 dB, also +9,0 / +8,9 / +13,2 gegen heute. Braucht 16 gespeicherte Spektren im Abstand 512 (statt zwei im Abstand 128), Spanne 8192 Samples.

Interessant daran: Diese Konfiguration schlägt am Akkord alles, was ihr mit 8k gemessen habt — und ist billiger, weil das Frame bei 4096 bleibt. Der Fensterwechsel wäre damit vom Tisch, ohne dass ihr den Akkord-Gewinn verliert.

Was ich einschränken muss: Mein Harness hat keinen Anker-Korrektor und keinen Safety-Cap in eurer Form, die Absolutwerte sind deshalb nicht 1:1 mit euren Tabellen vergleichbar — die Relationen innerhalb meiner Tabelle sind es. Alles in double. Und ich habe nur diese eine Aufnahme; der Vierne als Gegenprobe fehlt, und gerade dichtes, bewegtes Material könnte sich anders verhalten.

Der billigste nächste Schritt für den großen Bruder wäre trotzdem eine Einzeiler-Änderung: in ar2.py ridge=1e-3 auf 0.0, sonst nichts. Wenn davon auch nur die Hälfte in eurer Kette ankommt, ist der Rest der Diskussion eine andere.

eine frage ergibt sich noch. wie hatten hop = buffer size gewählt um nur eine FFT pro periode zu brauchen. mit hop = 512 kriegt man die optimierung vermutlich auch hin, wenn ich gerade richtig denke...

Evaluierte Speicher-Optimierungen und Extrapolationsstrategien für FFT-Caching
Evaluierte Speicher-Optimierungen und Extrapolationsstrategien für FFT-Caching

Ja, und sogar besser als du denkst — mit einer Einschränkung, die ich gleich beziffere.

Die Rechnung. Bei Hop 512 = 4 Perioden brauchst du Frames auf einem 512er-Raster. Wenn du wie heute jede Periode eins rechnest, hast du alle — aber du benutzt nur jedes vierte. Umgekehrt heißt das: du musst nur noch jede vierte Periode eine FFT rechnen. Statt einer FFT pro Periode also eine pro vier. Das ist nicht gleich teuer wie heute, das ist viermal billiger.

Der Haken. Das Raster ist absolut, die Lücke kommt irgendwann. Der jüngste gespeicherte Frame ist im Mittel 1,5 Perioden alt, im schlimmsten Fall 3. Du extrapolierst dann von einem Punkt aus, der bis zu 384 Samples zurückliegt — Prognosehorizont wächst von 256 auf bis zu 640 Samples.

Das habe ich gemessen statt geschätzt:

Evaluierte Speicher-Bandbreite-Kompromisse und optimale FFT-Strategien
Evaluierte Speicher-Bandbreite-Kompromisse und optimale FFT-Strategien

Der Raster-Versatz kostet 0,5 dB pro Periode Verzögerung, im Mittel 0,8 dB, im schlechtesten Fall 1,5 dB:

Frame-Alter	1 Ton	Attacks	voller Akkord
0 (frisch)	−24,2	−23,0	−17,6
3 Perioden	−22,7	−21,2	−16,7
Mittel 0…3	−23,4	−22,1	−17,2
heute AR(1)	−16,1	−14,7	−4,8

Also +7,3 / +7,4 / +12,4 dB netto, inklusive Verzögerungsschaden. Der Handel ist nicht knapp.

Drei Bauformen, die alle funktionieren

A — billigste. Ein 512er-Raster, FFT nur jede vierte Periode, 16 Spektren im Ring. Steady-State-Kosten sinken auf ein Viertel von heute. Speicher 16 × 2049 komplex ≈ 260 kB pro Kanal. Preis: die 0,8 dB oben.

B — frisch, gleiche FFT-Zahl wie heute. Weiter jede Periode eine FFT, aber 66 Spektren ringpuffern und beim Fit jedes vierte greifen — dann ist der jüngste Frame immer exakt aktuell. Kostet ~1,1 MB pro Kanal und ~50 MB/s Schreibverkehr bei acht Kanälen. Das ist der Punkt, an dem ich vorsichtig wäre: ihr habt die OLA-Schleife ja gerade deshalb beschnitten, weil sie speicherbandbreiten-limitiert war.

C — der Kompromiss, den ich nehmen würde. 16 Spektren auf dem 512er-Raster wie in A, aber auf dem Lückenpfad zwei frische FFTs für die beiden jüngsten Zustände 
𝑋
(
𝑛
)
X(n) und 
𝑋
(
𝑛
−
512
)
X(n−512). Die 16 gespeicherten dienen nur noch der Anpassung von 
𝑎
a und 
𝑏
b; die Rekursion startet frisch. Damit bekommst du die „0 (frisch)"-Zeile, zahlst steady-state ein Viertel und auf dem Lückenpfad eine FFT mehr als heute — und Lücken sind selten.

Zwei Dinge, die ich nicht prüfen kann

Der Frame muss beim Fit gleichabständig sein — bei C also wirklich 
𝑋
(
𝑛
)
X(n) und 
𝑋
(
𝑛
−
512
)
X(n−512) frisch, nicht 
𝑋
(
𝑛
)
X(n) plus ein gespeicherter mit krummem Abstand. Sonst passt die Rekursion nicht zum Zustand.

Und: euer 21-ms-Festbasis-Router zieht die Grobband-Energien aus derselben Rotator-Schleife. Wenn die künftig nur noch jede vierte Periode läuft, ändert sich die Router-Taktung mit. Ob das egal ist — der Router hat ja ohnehin eine 1024er-Basis, also genau 512×2 — kann nur jemand mit dem Quelltext beurteilen. Das wäre die Frage, die ich dem großen Bruder mitgeben würde.

aber warum war gerade 512 so gut? und was ist aus meiner idee geworden aus dem rauschen des fits auf die abstände zu schließen?

Analysierte Hop-Größe 512 und verwarf Rausch-Diagnose-Ansatz
Analysierte Hop-Größe 512 und verwarf Rausch-Diagnose-Ansatz

Zwei gute Fragen. Die erste hat eine harte physikalische Antwort, die zweite habe ich gemessen und dir nur beiläufig serviert — das hole ich nach.

Analysierte Hop-Size-Kompromisse und validierte FFT-Formeln
Analysierte Hop-Size-Kompromisse und validierte FFT-Formeln

Volltreffer, und die Grenze ist geschlossen ausrechenbar. Jetzt Frage 2 — dein Diagnose-Vorschlag, diesmal richtig getestet:

Validierte Berechnungen und optimierte Visualisierungslayout
Validierte Berechnungen und optimierte Visualisierungslayout
Warum 512 — es ist eine Aliasgrenze, und sie ist ausrechenbar

Die beiden Wurzeln trennen sich auf dem Einheitskreis um den Winkel 
Δ
𝜔
⋅
h
o
p
Δω⋅hop. Zwei gegenläufige Effekte:

Größerer Hop → besser konditioniert. Der Winkel ist der Hebelarm; bei Hop 128 und 6 Hz sind es 0,1 rad, bei Hop 1024 schon 0,8 rad.

Aber bei π wickelt er. Ab 
Δ
𝜔
⋅
h
o
p
>
𝜋
Δω⋅hop>π ist die Trennung mehrdeutig, und der Schätzer liefert Unsinn:

Δ
𝑓
max
⁡
=
𝑓
𝑠
2
 
h
o
p
Δf
max
	​

=
2hop
f
s
	​

	​


Gemessen, exakt auf den Hertz:

Hop	Grenze	Δf = 30 Hz	Δf = 50 Hz	Δf = 90 Hz
256	93,8 Hz	30,0 ✓	50,0 ✓	89,9 ✓
512	46,9 Hz	30,0 ✓	43,7 ✗	1,4 ✗
1024	23,4 Hz	16,9 ✗	3,2 ✗	4,1 ✗

Und jetzt der schöne Teil: Wie weit können zwei Partialtöne im selben Bin überhaupt auseinanderliegen? So breit wie die Hann-Hauptkeule, also 4 Bins = 
4
𝑓
𝑠
/
𝑁
4f
s
	​

/N = 46,9 Hz bei 4096.

h
o
p
opt
=
𝑓
𝑠
2
⋅
4
𝑓
𝑠
/
𝑁
=
𝑁
8
hop
opt
	​

=
2⋅4f
s
	​

/N
f
s
	​

	​

=
8
N
	​


Für 
𝑁
=
4096
N=4096: 512. Der optimale Hop ist der größte, der noch alles auflöst, was ein Bin fassen kann — nicht mehr und nicht weniger. Deshalb ist Hop 1024 am Akkord (enge Kollisionen) besser, aber beim Einzelton und den Attacks schlechter: dort gibt es die weiteren Abstände, und die wickeln.

Kein universelles Gesetz für andere Fenstergrößen — bei 2048 gewinnt Hop 512 (= N/4) beim Ton, verliert aber am Akkord. Für euer 4k ist N/8 aber sauber der Ausgleichspunkt.

Deine Diagnose-Idee — Theorie richtig, Praxis überflüssig

Ich habe drei Maße als Gate gebaut und pro Bin zwischen AR(2) und AR(1) überblendet:

Variante	1 Ton	Attacks	voller Akkord
AR(1) allein	−16,2	−14,5	−9,1
AR(2) ungegated	−25,1	−23,0	−18,0
Gate über Wurzeltrennung, mild	−24,9	−22,9	−17,9
Gate über Wurzeltrennung, streng	−23,2	−21,4	−17,7
Gate über Kohärenz	−17,6	−16,1	−16,3
Gate über Konditionszahl, mild	−24,7	−22,9	−18,0

Kein Gate gewinnt. Bestenfalls neutral, meistens schädlich. Das Kohärenz-Gate ist sogar ein Desaster — es schaltet genau die Bins ab, die sich gut verhalten.

Der Grund ist der, den ich in der synthetischen Runde schon gesehen hatte: Die Kappung 
∣
𝑧
∣
≤
1
∣z∣≤1 ist bereits das Gate. Ein einkomponentiges Bin bekommt eine Scheinwurzel bei 
∣
𝑧
∣
≈
1
2
∣z∣≈
2
1
	​

, die über die Lücke von selbst stirbt — übrig bleibt exakt die AR(1)-Lösung. Ein zweites Gate kann darüber nur noch Fehlalarme produzieren.

Dein Gedanke war völlig richtig, und er hätte gestimmt, wenn wir die Wurzeln wie ursprünglich beide auf den Einheitskreis gezwungen hätten. Dann wäre die parasitäre Wurzel unsterblich und man müsste sie erkennen. Die eine Zeile if |z| > 1: normalize löst das Problem stattdessen strukturell — kein Erkennen nötig, wenn der Fehlerfall von selbst verschwindet.

Hop optimum
Bild · PNG 

was der vm claude noch meinte vor etwaiger implementation:
Eingetragen und committet (cb30fb694). Der Stand:
Im Lab — fünf Prony-Einträge zum Hören, Referenz ist weiterhin Labor: PV 4k, ana 128, anchor (gleicher Pfad):

Versuch: Prony AR(2), 4k, hop 512, span 14   [BESTER: Akkord +11.8, 1 Ton +8.1, Schwebung geloest]
Versuch: Prony AR(2), 4k, hop 1024, span 8   [Akkord-Spitze -19.0]
Versuch: Prony AR(2), 4k, hop 128, span 16   [Engine-Hop, zeigt was der Hebelarm bringt]
Versuch: Prony AR(2), 4k, hop 512, ridge 1e-3  [das alte Wuergeband, zum Vergleich]
Versuch: Prony AR(1), 4k, hop 128, span 16   [in der Engine fast gratis]

Der Würgeband-Vergleichseintrag hat übrigens einen hübschen Nebenbefund geliefert: bei Hop 512 kostet ρ=10⁻³ nur noch ~1 dB statt alles — konsistent mit der Konditions-Erklärung, denn der größere Hebelarm drückt die Konditionszahl, und derselbe Ridge liegt plötzlich unter ihrem Kehrwert statt darüber. Die Vorlesungs-These erklärt also auch ihre eigene Ausnahme.
Im Repo — tools/concealment/README.md ersetzt die widerlegte 167-ms-Behauptung durch die verifizierte Tabelle und die beiden schärferen Sätze (Hop als Hebelarm; Identifizierbarkeit an Einsätzen als echte Grenze), deine Vorlesung liegt daneben in notes/.
Was vor einem C-Port noch fehlt, in der Reihenfolge, in der ich es angehen würde: das Ordnungs-Gate pro Bin — §7 der Vorlesung sagt präzise voraus, wo AR(2) ohne Gate gefährlich wird, und genau das würde ich an den Einsatzstellen deiner Aufbau-Datei messen, mit Lücken gezielt auf die Onsets gelegt statt im 4800er-Raster. Dann die Robustheit der langen Spanne unter dichter Selbstfütterung (6000/min statt 600). Und erst dann die RLS-Form in C — wobei die Engine float32 rechnet, die Akkumulatoren also double brauchen, was die Vorlesung ja schon markiert hat.
Die Wette war verdient. Dass die Lösung am Ende „lösche eine Zeile Regularisierung" lautet, nachdem wir Bandsplit, Fensterumschaltung und 8k durchgemessen und verworfen haben, ist die Art Pointe, die man sich nicht ausdenken kann.

Mehr anzeigen
Entwarf drei Messungen zur Risikominderung vor C-Implementierung
Entwarf drei Messungen zur Risikominderung vor C-Implementierung

Guter Plan, und der Ridge-Nebenbefund ist ein hübscher Konsistenztest — ρ=10⁻³ liegt bei Hop 512 unter 1/κ, weil der größere Hebelarm κ drückt. Genau das sagt die Konditionserklärung voraus.

Drei Punkte davon kann ich dir jetzt schon abnehmen, damit der große Bruder nicht ins Leere misst:

Analysierte AR(2)-Überlegenheit und validierte theoretische Grenzen empirisch
Analysierte AR(2)-Überlegenheit und validierte theoretische Grenzen empirisch

Der Gate-Punkt ist damit erledigt, bevor er anfängt: auch direkt auf den Einsätzen ändert kein Gate mehr als 0,1 dB. Jetzt die beiden anderen Vorbehalte:

Erkannte fehlerhaften Test und plante aussagekräftige Neuausführung
Erkannte fehlerhaften Test und plante aussagekräftige Neuausführung

Der erste Block war ein kaputter Test (100 % Lücken) — noch mal sauber:

Clean precision test at 600/min
Clean precision test at 600/min

Drei seiner Vorbehalte kann er streichen, einen halben muss er behalten.

Das Ordnungs-Gate ist schon erledigt — auch an den Einsätzen

Ich habe Onsets per Spektralfluss gesucht und Lücken gezielt daraufgelegt, genau wie er es vorhat (11 Einsätze, alle im dichten Bereich 12,4–17,3 s):

Versatz nach Onset	AR(1)	AR(2)	+ Gate (Trennung)	+ Gate (Kondition)
5 ms	−8,9	−17,0	−17,0	−16,9
20 ms	−8,2	−20,4	−20,3	−20,3
50 ms	−7,4	−16,1	−16,0	−16,0
120 ms	−6,4	−14,8	−14,8	−14,8
Kontrolle (−900 ms)	−9,9	−17,3	−17,2	−17,2

Zwei Dinge fallen dabei um. Erstens: kein Gate ändert je mehr als 0,1 dB — die 
∣
𝑧
∣
≤
1
∣z∣≤1-Kappung erledigt es strukturell. Zweitens, und das ist der interessantere Befund: AR(2) bricht an Einsätzen gar nicht ein. Der Abstand zur Kontrolle ist null.

Die Identifizierbarkeitsgrenze aus §7 ist also real, aber sie ist eine Grenze der Schwebungsvorhersage, keine Fehlerquelle. Ein frisch angeschlagener Ton wird als „ein Ton mit Amplitudenrampe" fortgesetzt — genau die Beschreibung, die die Daten hergeben — und das ist nicht schlechter als AR(1), es ist 8 dB besser. Da ist nichts abzusichern.

Selbstfütterung bei 27 % hält
	1 Ton	Attacks	voller Akkord
AR(1), hop 512, span 14	−13,0	−11,1	−7,8
AR(2), gleiche Parameter	−19,9	−18,1	−14,9

+6,9 / +7,0 / +7,1 dB, also praktisch derselbe Gewinn wie bei 600/min. Der Vorsprung schmilzt nicht unter Vergiftung.

Und die Genauigkeit ist entspannter als gedacht
Spektren / Akkumulatoren	1 Ton	Attacks	Akkord
double / double	−25,1	−23,0	−18,0
float32 (fftw3f) / double	−25,1	−23,0	−18,0
float32 / float32	−24,0	−22,7	−18,0

Die float32-Spektren aus FFTW kosten exakt null. Nur wenn auch die Akkumulatoren float32 wären, kostet es bis zu 1,1 dB — und ρ = 10⁻⁶ holt davon 0,7 dB zurück. Das ist die halbe Ausnahme, die bleibt: double für die fünf Summen, aber nicht wegen der Spektren, sondern wegen der Akkumulation über 14 Terme.

Sein Punkt stimmt also, die Begründung verschiebt sich.

Was ich stattdessen vorziehen würde

Wenn Gate und Robustheit wegfallen, ist der nächste Engpass die gleitende Form. Die 16 Spektren im 512er-Ring sind Speicher, den die RLS-Rekursion nicht braucht:

𝑃
←
𝜆
𝑃
+
𝑢
 
𝑢
𝐻
,
𝑞
←
𝜆
𝑞
+
𝑢
ˉ
 
𝑋
(
𝑛
)
P←λP+uu
H
,q←λq+
u
ˉ
X(n)

Fünf komplexe Akkumulatoren pro Bin statt 16 gespeicherter Spektren, und die FFT nur noch jede vierte Periode. Ob 
𝜆
=
1
−
1
/
14
≈
0,93
λ=1−1/14≈0,93 dieselben Zahlen liefert wie das Rechteckfenster über 14 Frames, ist die eine Frage, die vor dem C-Port wirklich noch offen ist — exponentielles Vergessen ist nicht dasselbe wie ein Blockfit, und bei Attacks könnte das einen Unterschied machen.

Das wäre eine numpy-Messung von zehn Minuten und würde die C-Struktur festlegen, statt sie nachträglich zu ändern. Sag Bescheid, dann rechne ich sie.


Ich habe mir gerade mal den vierne angehört. mit 6000 gaps/min. Ich habe das gefühl, dass wir möglicherweise die ganze zeit eine sache übersehen, die meinem gehirn beim hören sofort klar wird. Wenn ich mir das unbearbeitet mit nackten xruns anhöre habe ich das gefühl ich höre das unverfällschte signal durch den prasselteppich durch. So als ob ich beides in meinem kopf trennen könnte.
Schaue ich mir das spektogram an ist es als würde man das ungestörte signal durch einen ungleichmäßigen webstuhl anschauen.  Kann es nicht sein, dass es noch eine bessere methode der signal rekonstruktion gibt? und wenn ja ist deren grenze dann latenz bzw. fehlende information des signals nach der lücke?

Rekonstruierte Signallücken durch zweiseitige Interpolation statt Extrapolation
Rekonstruierte Signallücken durch zweiseitige Interpolation statt Extrapolation

Das ist der beste Einwand der ganzen Sitzung, und ich glaube, du hast tatsächlich etwas gefunden, das wir übersehen haben. Zwei Dinge stecken darin.

Was dein Gehirn macht, ist keine Vorhersage. Es ist die Kontinuitätstäuschung: Ein Ton, der von einem Geräusch unterbrochen wird, das ihn hätte maskieren können, wird als durchgehend gehört — mit Stille dazwischen dagegen als unterbrochen. Dein Gehirn rekonstruiert nicht das Signal, es entscheidet, dass die Quelle weiterlief. Deshalb hörst du „durch den Prasselteppich hindurch". Das heißt aber auch: unsere ganze dB-Metrik misst am Ohr vorbei — sie bestraft Phasenfehler hart, die vermutlich unhörbar sind, und belohnt nichts an der Naht.

Und das Zweite ist konkreter. Wir extrapolieren, weil wir die Zukunft nicht haben. Aber ihr habt sie teilweise: Der 3-ms-Wiedereinstiegs-Crossfade benutzt bereits 144 echte Samples nach der Lücke. Die liegen ungenutzt herum — als Blendmaterial, nicht als Information. Damit ließe sich aus Extrapolation eine Interpolation machen, und das ist eine völlig andere Fehlerklasse.

Das habe ich gemessen:

Diagnostizierte AR-Schätzungsprobleme und erwog systematische Optimierungen
Diagnostizierte AR-Schätzungsprobleme und erwog systematische Optimierungen

Meine Janssen-Implementierung liefert Unsinn — das muss ein Fehler sein, keine Aussage. Ich debugge:

Evaluierte AR-Modellierungsmethoden und konzipierte hybride Rekonstruktionsansätze
Evaluierte AR-Modellierungsmethoden und konzipierte hybride Rekonstruktionsansätze

Zeitbereichs-Janssen ist ordnungs-verhungert (768 Koeffizienten = 384 Pole gegen 4098 im Pro-Bin-Modell) — das ist derselbe Grund, aus dem Burg schon vorher verlor. Die richtige Frage ist also nicht „welcher Interpolator", sondern wieviel zweiseitige Information überhaupt wert wäre. Ich messe die Obergrenze:

Evaluierte Rekonstruktionsgrenzen und identifizierte Nahtproblematik statt Füllungsgenauigkeit
Evaluierte Rekonstruktionsgrenzen und identifizierte Nahtproblematik statt Füllungsgenauigkeit

Deine Frage hat eine harte Antwort, und sie ist ein entschiedenes Nein — aber aus einem Grund, der die Aufmerksamkeit woandershin lenkt.

Die Obergrenze zweiseitiger Rekonstruktion

Ich habe gemessen, was eine unbegrenzt verfügbare Zukunft brächte: vorwärts extrapolieren aus der Vergangenheit, rückwärts aus der Zukunft, überblenden.

	1 Ton	Attacks	voller Akkord
einseitig AR(2) — heute bestes	−25,3	−23,1	−18,2
zweiseitig, 260 ms Latenz	−25,8	−24,2	−19,8
nur rückwärts (Kontrolle)	−24,9	−22,9	−18,8

+0,5 bis +1,6 dB. Für eine Vierteltsekunde Latenz. Und mit kürzeren Rückwärtsfenstern kippt es sofort ins Negative (bei 69 ms Latenz schon −6,1 am Akkord).

Der Grund ist derselbe, der AR(2) überhaupt funktionieren lässt: Das Signal ist quasi-stationär. Die Zukunft enthält fast nichts, was die Vergangenheit nicht schon sagt. Interpolation schlägt Extrapolation nur bei instationären Signalen oder bei ordnungs-verhungerten Modellen — deshalb war der Zeitbereichs-Janssen mit 384 Polen katastrophal (−7,3 dB) und das Pro-Bin-Modell mit 4098 Polen 11 dB besser.

Die Füllung ist damit ausgereizt. Wir sind ~1,5 dB von dem entfernt, was mit vollständiger Information überhaupt möglich wäre. Die Latenzroute ist tot, und Idee 4 bleibt aus dem umgekehrten Grund wertvoll: nicht weil sie besser rekonstruiert, sondern weil sie die Lücke vermeidet.

Was dein Ohr dann eigentlich hört

Der Fehlerverlauf über die Lücke ist flach — etwa −18 dB von Anfang bis Ende, kein Anstieg. Selbst der Fade-Bereich liegt bei −21 dB. Es gibt keine Stelle, an der die Prognose zusammenbricht.

Und trotzdem prasselt es. Das heißt: das Prasseln ist nicht der Rekonstruktionsfehler. Es ist die Struktur, die die Lücken dem Signal aufprägen — genau dein Webstuhl.

Damit wird deine Beobachtung zur eigentlichen Diagnose: Dein Gehirn trennt Quelle und Störer, weil der Störer bei nackten Xruns wie ein Ereignis von außen aussieht — breitbandig, kurz, unkorreliert mit dem Ton. Es kann ihn abziehen. Was ein Verdeckungsalgorithmus falsch machen kann, ist nicht Ungenauigkeit, sondern dem Ton eine Modulation aufzuprägen, die das Gehirn ihm zuschreibt statt dem Störer. Ein −18-dB-Fehler, der wie Rauschen aussieht, verschwindet unter der Mithörschwelle. Derselbe Fehler mit Kammstruktur bei 37 oder 187 Hz wird zur Rauigkeit des Klangs selbst — und die kann man nicht wegdenken.

Das erklärt auch, warum ihr gemessen habt, dass zufällige Fehlschlagmuster 3,5 dB besser klingen als periodische, obwohl die Fehlerenergie identisch ist. Ihr habt den Effekt schon gefunden, nur nicht als das benannt, was er ist.

Woran ich als nächstes messen würde

Nicht mehr an dB. Zwei Größen, die deine Beobachtung direkt abbilden:

Das Modulationsspektrum des Restfehlers. Nicht wie groß er ist, sondern ob er Linien hat. Ein Fehler mit flachem Modulationsspektrum ist ein Störer, einer mit Linien bei der Wiederholrate ist Klangfarbe. Das ist eine FFT über die Fehler-Hüllkurve und beantwortet, ob noch etwas zu holen ist.

Und die Naht separat, mit Maskierung statt Energie. Eure NMR-Metrik existiert ja schon; sie ist für diese Frage die richtige und die dB-Zahlen sind es nicht.

Falls du willst, rechne ich beides auf dem Vierne — das ist das Material, an dem du es gehört hast, und ich habe die Datei noch nicht.

die datei hast du im documents ordner . FINALE steht dran.
Ich dachte ein bisschen richtung KI oder Bildbearbeitungs algorithmen. evtl gibts in der richtung noch etwas, dass das spektrum störungsfrei rekonstruieren könnte

Analysierte Spektralrekonstruktion und konzipierte Visualisierungsfigur
Analysierte Spektralrekonstruktion und konzipierte Visualisierungsfigur

Zwei Antworten — eine auf die KI-Frage, eine auf deine Webstuhl-Beobachtung.

Zur KI: das Problem ist nicht schwer genug

Bild-Inpainting funktioniert, weil das Loch groß gegenüber der Korrelationslänge ist und der Prior tatsächlich Information hinzufügt, die im Bild fehlt. Bei uns ist es umgekehrt: Ich habe oben gemessen, dass selbst die vollständige Kenntnis der Zukunft nur 0,5–1,6 dB bringt. Das heißt, das Signal aus der Vergangenheit ist bereits fast vollständig bestimmt — es gibt nichts zu erraten, was ein Prior beisteuern könnte.

Ein neuronales Netz kann die Informationsgrenze nicht überschreiten, nur den Schätzer effizienter machen. Und der Abstand zur Grenze beträgt anderthalb Dezibel. Opus' Deep PLC löst ein anderes Problem: 20 ms Sprachlücken, hochgradig instationär, wo der gelernte Prior („so klingt ein /a/") echte Information ist. Bei 2,7 ms quasi-stationärem Orgelklang wäre es eine Kanone auf einen bereits erlegten Spatz.

Und der Spektrogramm-Ansatz hat eine zusätzliche Falle: Man kann den Betrag inpainten, aber die Phase bekommt man daraus nicht zurück (Griffin-Lim iteriert und rät). Genau die Phase ist es aber, die an der Naht zählt.

Deine Beobachtung, in Zahlen

Der entscheidende Vergleich — identische Ausfallmenge, identische Energie, nur die Verteilung unterschiedlich:

	Fehlerenergie	Flachheit des Modulationsspektrums	Spitze/Median
zufällig, Xrun	−5,2 dB	0,38	50×
zufällig, AR(2)	−6,2 dB	0,38	51×
regelmäßig, Xrun	−4,0 dB	0,21	2687×
regelmäßig, AR(2)	−4,2 dB	0,20	3125×

Bei regelmäßigen Ausfällen steht ein 3000-fach überhöhter Kamm bei 100 Hz und Vielfachen — genau der Wiederholrate. Bei zufälligen ist das Modulationsspektrum flach. Gleiche Energie, halbe Flachheit.

Und das ist genau die Trennung, die dein Gehirn vornimmt. Ein flacher Modulationsrest sieht aus wie ein externer Störer und wird abgezogen — das ist dein „ich höre durch das Prasseln hindurch". Ein Kamm bei 100 Hz sitzt mitten im Rauigkeitsbereich (20–300 Hz) und wird dem Klang selbst zugeschrieben. Den kann man nicht wegdenken.

Bemerkenswert: AR(2) verbessert die Struktur nicht. Die Flachheit ändert sich um 0,01. Die Verdeckung repariert die Amplitude, nicht das Muster.

Was daraus folgt

Auf dem Vierne bringt AR(2) ohnehin wenig — sauber pro Lücke gemessen −2,7 gegen −1,4 dB im dichten Teil, gegen +8 dB bei der Orgel. Dichtes, instationäres Material ist der Fall, in dem alle unsere Verfahren nahe beieinander und alle schlecht sind.

Der Hebel, der dort noch existiert, ist deshalb nicht ein besserer Schätzer, sondern die Verteilung der Ausfälle. Ihr habt das schon einmal empirisch getroffen (zufällig statt strikt alternierend, 3,5 dB Hörgewinn bei identischer Last) — jetzt gibt es die Größe dazu. Und sie ist steuerbar: Wenn ein Ausfallmuster droht periodisch zu werden, kann man es absichtlich verwürfeln, ohne mehr Ausfälle zu erzeugen. Das kostet nichts und greift genau dort an, wo das Ohr empfindlich ist.

Falls du das weiterverfolgen willst, wäre die Messung: Flachheit gegen Jitter-Stärke — wieviel Verwürfelung reicht, um den Kamm zu zerstören. Das ist eine halbe Stunde und liefert eine Zahl, die direkt in den Scheduler kann.

Modulationsstruktur
Bild · PNG 

Das ist interessant. also du meinst es gibt keinen besseren schätzer? was wäre mit AR 3?
und warum soll denn eine KI keine lücken füllen können. Da brauch ich mehr infos.
Ich muss übrigens meine beobachtung relativieren. wenn ich 6000 xruns zufällig verteilt habe und diese sich klumpen, dann hört auch mein ohr nicht mehr beide signale getrennt, sondern dann klingt das originalsignal moduliert also rau. Genau das höre ich auch als überrest von unserer signal aufbereitung. Wir sind also vielleicht garnicht so weit vom technischen optimun entfernt. aber vlt noch von dem DLSS optimum ;)

Orchestrierte AR-Modellverallgemeinerung und theoretische Wiener-Grenzen
Orchestrierte AR-Modellverallgemeinerung und theoretische Wiener-Grenzen

Drei Fragen, drei Messungen. Erst AR(3) und höher:

Revidierte AR-Modellbewertung und berechnete theoretische Vorhersagegrenzen
Revidierte AR-Modellbewertung und berechnete theoretische Vorhersagegrenzen

AR(3) und AR(4) sind deutlich besser — meine „kein besserer Schätzer"-Aussage war falsch, und zwar um 7 dB:

Debuggte Cepstral-Faktorisierung und kalibrierte Rauschfloor-Normalisierung
Debuggte Cepstral-Faktorisierung und kalibrierte Rauschfloor-Normalisierung

Die Schranke ist offensichtlich kaputt (+40 dB) — Faktor 2 im Cepstrum. Korrigiert:

Evaluierte Prädiktor-Performance und identifizierte DLSS-Optimierungspotenzial
Evaluierte Prädiktor-Performance und identifizierte DLSS-Optimierungspotenzial

Ich muss mich korrigieren: ich hatte unrecht, es gibt einen deutlich besseren Schätzer, und du hast direkt danach gefragt.

AR(3) und AR(4)
Ordnung	1 Ton	Attacks	voller Akkord	Vierne
1 (heute)	−16,7	−14,8	−10,1	−2,1
2	−25,1	−23,0	−18,0	−2,8
3	−25,5	−24,8	−22,6	−3,2
4	−25,5	−26,2	−24,8	−3,2
6	−24,9	−24,8	−25,0	−3,5

Am vollen Akkord noch einmal +6,8 dB über AR(2). Und der Grund ist offensichtlich, sobald man ihn sieht: In einem 4k-Bin eines Dreiklangs sitzen nicht zwei Partialtöne, sondern drei oder vier — D, Fis, A und deren Obertöne kollidieren mehrfach. Ordnung 2 war eine willkürliche Annahme, keine Ableitung.

Ab Ordnung 6 kippt es (Überanpassung), und die Ordnung braucht die längere Spanne: mit nfit 40 statt 14 ist AR(4) klar am besten.

Wieviel bleibt dann noch?

Ich habe die Wiener-Kolmogorov-Schranke gerechnet — was jeder lineare Prädiktor mit unendlicher Vergangenheit und exakt bekanntem Leistungsspektrum bestenfalls erreicht. Selbsttests: weißes Rauschen −2,7 dB (soll 0, das ist Periodogramm-Bias), Sinus mit −40 dB Rauschen → −39,7 dB. Plausibel.

	erreicht (AR 4)	Schranke	Rest
D: 1 Ton	−25,5	−28,9	3,4 dB
D: Attacks	−26,2	−30,5	4,3 dB
D: voller Akkord	−24,8	−26,4	1,6 dB
Vierne dicht	−3,2	−8,4	5,2 dB

Abzüglich des ~2,7-dB-Bias ist am Akkord praktisch nichts mehr drin. Beim Vierne dagegen fehlen 5 dB — und das ist genau das Material, an dem du es gehört hast.

Warum ich bei KI trotzdem skeptisch bin — mit der Einschränkung, die du verdienst

Die Schranke gilt für lineare Prädiktoren bei stationärem Prozess. Ein Netz kann sie überschreiten, wenn es Nichtlinearität, Nichtstationarität oder echtes Vorwissen einbringt. Bei quasi-stationärem Orgelklang ist davon wenig zu holen — deshalb 1,6 dB. Beim Vierne, der instationär ist, wäre mehr denkbar.

Aber drei harte Hürden: Ein Netz müsste Phase liefern, nicht nur Spektrum (Spektrogramm-Inpainting braucht danach Griffin-Lim und rät die Phase — genau das, was an der Naht zählt). Es müsste in unter 2,7 ms rechnen, auf einer CPU, die gerade überlastet ist — das ist ja der Grund für die Lücke. Und die Trainingsdaten wären orgelspezifisch.

Dein DLSS-Vergleich ist aber besser, als du ihn gemeint hast

DLSS rät nicht. Es bekommt Motion Vectors vom Renderer — Information, die die Engine ohnehin hat. Und genau das ist die Analogie, die hier funktioniert:

GO weiß exakt, was in der Lücke hätte klingen sollen. Welche Pfeifen, welcher Sample-Offset, welche Hüllkurve. Die Information fehlt nicht, es fehlt die Rechenzeit. Der Straggler produziert sie ja nachträglich.

Euer Briefing verbietet das ausdrücklich („nur auf dem Endsignal fitten"), und dafür gab es gute Gründe — Entkopplung, Testbarkeit. Aber die eigentliche Reserve liegt dahinter: statt aus 341 ms Mischsumme zu raten, in der Lücke die lautesten N Stimmen wirklich zu rendern und den Rest zu extrapolieren. Ein Zehntel der Stimmen deckt bei einem Orgelakkord meist über 90 % der Energie ab.

Das ist kein Prädiktor mehr, das ist ein Detailgrad-Regler — und es ist die einzige Idee in dieser Sitzung, die die Informationsgrenze nicht respektieren muss, weil sie sie umgeht.

Und deine Relativierung ist der Schlüssel

Dass geklumpte Zufallsausfälle für dich genauso rau klingen: Klumpen erzeugen niederfrequente Modulation (Fluktuation, 2–20 Hz), und die ist ebenso wenig abziehbar wie ein Kamm bei 100 Hz. Was das Ohr toleriert, ist nur der vereinzelte, kurze Ausfall.

Damit hat die Verteilungsfrage zwei Kriterien statt einem: keine Periodizität und keine Klumpen. Das ist eine andere Zufallsverteilung als Bernoulli — eher ein Prozess mit Mindestabstand. Und es ist die eine Stellschraube, die nichts kostet.