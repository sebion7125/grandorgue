# Pro-Bin-Fortsetzung durch lineare Rekursion

Herleitung des Verfahrens hinter Xorcist, von der Modellannahme bis zur Zahlenwahl. Alle Symbole werden vor ihrer ersten Verwendung definiert.

---

## 1 Notation und Ausgangslage

### 1.1 Signal und Kurzzeit-Spektrum

Sei

- $x : \mathbb{Z} \to \mathbb{R}$ das Audiosignal (die fertige Mischsumme eines Kanals),
- $N \in \mathbb{N}$ die Fensterlänge in Samples,
- $H \in \mathbb{N}$ der Analyse-Hop in Samples,
- $w := (w_\nu)_{\nu = 0,\dots,N-1}$ mit $w_\nu \in \mathbb{R}$ das Hann-Fenster in periodischer Form, $w_\nu = \tfrac12 (1 - \cos(2\pi\nu/N))$,
- $K := N/2$, sodass $k \in \{0,\dots,K\}$ die Bin-Indizes durchläuft,
- $t_0 \in \mathbb{Z}$ ein fester Bezugszeitpunkt, das Ende des jüngsten Frames.

Für $k \in \{0,\dots,K\}$ und $n \in \mathbb{Z}$ definieren wir den **Bin-Wert**

$$
X_k(n) \;:=\; \sum_{\nu=0}^{N-1} w_\nu \; x(t_0 + nH - N + \nu) \; e^{-2\pi \mathrm{i} k \nu / N} \;\in\; \mathbb{C},
$$

wobei $\mathrm{i}$ die imaginäre Einheit bezeichnet.

Der Index $n$ zählt **Frames**, nicht Samples: Frame $n$ endet bei Sample $t_0 + nH$, Frame $n-1$ einen Hop früher. Frame $n = 0$ ist der jüngste vollständig verfügbare.

Für festes $k$ heißt die Folge

$$
(X_k(n))_{n \in \mathbb{Z}} \;\in\; \mathbb{C}^{\mathbb{Z}}
$$

die **Bin-Trajektorie** von Bin $k$.

### 1.2 Der zentrale Aufbau

Das Verfahren behandelt jedes Bin **unabhängig**. Alles Folgende gilt bei festem $k$; wo keine Verwechslung möglich ist, schreiben wir $X(n)$ statt $X_k(n)$. In der Implementierung laufen $K+1$ solcher Rechnungen nebeneinander.

### 1.3 Aufgabe

Ab Frame $n = 0$ liegen keine echten Samples mehr vor, das ist die Lücke. Gesucht sind Fortsetzungswerte $X(1), X(2), \dots$, aus denen per inverser Transformation und Overlap-Add Ersatz-Samples entstehen.

---

## 2 Das Modell

### 2.1 Modellannahme

Sei $p \in \mathbb{N}$ die **Ordnung**. Wir nehmen an, es gebe Koeffizienten

$$
a \;:=\; (a_1,\dots,a_p)^{\mathsf T} \;\in\; \mathbb{C}^{p}
$$

mit

$$
X(n) \;=\; \sum_{\kappa=1}^{p} a_\kappa\, X(n-\kappa) \tag{2.1}
$$

für alle betrachteten $n$. Der Index $\kappa \in \{1,\dots,p\}$ heißt **Rückblicktiefe**: $a_\kappa$ gewichtet den um $\kappa$ Hops zurückliegenden Wert.

### 2.2 Abgrenzung

Gleichung (2.1) ist **kein Polynom-Fit**. Ein Ansatz der Form

$$
X(n) \;\approx\; \sum_{\kappa=1}^{p} c_\kappa\, n^{\kappa-1}
$$

modelliert Drift, aber keine Schwingung. Die Lösungen von (2.1) sind dagegen Überlagerungen komplexer Exponentialfolgen (§5) und daher schwingungsfähig.

Polynom-Extrapolation ist der Sonderfall von (2.1), in dem alle Nullstellen des charakteristischen Polynoms (Definition 5.3) gleich $1$ sind. Für $p=2$ lautet er $X(n) = 2X(n-1) - X(n-2)$, also lineare Extrapolation.

### 2.3 Gültigkeitsbereich

Gleichung (2.1) ist erfüllbar, wenn die Bin-Trajektorie eine endliche Überlagerung von Komponenten konstanter Frequenz mit polynomialer Amplitudenhüllkurve ist.

Sie ist **nicht** erfüllbar für Komponenten mit veränderlicher Frequenz: Ein linearer Chirp erfüllt keine Rekursion mit konstanten Koeffizienten beliebiger endlicher Ordnung. Der Restfehler (Definition 3.3) misst, wie gut die Annahme im Einzelfall trägt.

---

## 3 Schätzung der Koeffizienten

### 3.1 Überbestimmtes System

Sei $m \in \mathbb{N}$ mit $m \ge p$ die Zahl der **Beobachtungen**, also wie oft (2.1) an verschiedenen Zeitstellen gefordert wird. Für $j \in \{0,\dots,m-1\}$ lautet die $j$-te Forderung

$$
X(-j) \;=\; \sum_{\kappa=1}^{p} a_\kappa\, X(-j-\kappa).
$$

Benötigt werden damit die Frames $n = 0, -1, \dots, -(m-1+p)$, also $m + p$ Stück.

**Definition 3.1.** Der Beobachtungsvektor ist

$$
b \;:=\; (X(-j))_{j=0,\dots,m-1} \;\in\; \mathbb{C}^{m}.
$$

**Definition 3.2.** Die Beobachtungsmatrix $A \in \mathbb{C}^{m \times p}$ hat die Einträge

$$
A_{j\kappa} \;:=\; X(-j-\kappa), \qquad j = 0,\dots,m-1, \quad \kappa = 1,\dots,p.
$$

*Bedeutung.* Zeile $j$ ist eine Beobachtung: ein Zeitpunkt, an dem die Regel geprüft wird. Spalte $\kappa$ ist die um $\kappa$ Hops verzögerte Kopie derselben Trajektorie.

Da $A_{j\kappa}$ nur von $j + \kappa$ abhängt, ist $A$ eine Hankel-Matrix — auf jeder Gegendiagonale steht derselbe Wert. In der Implementierung muss $A$ deshalb nicht materialisiert werden.

Ausgeschrieben lautet das zu erfüllende System

$$
\underbrace{\begin{pmatrix} X(0) \\ X(-1) \\ X(-2) \\ \vdots \\ X(-(m-1)) \end{pmatrix}}_{b}
\;=\;
\underbrace{\begin{pmatrix}
X(-1) & X(-2) & \cdots & X(-p) \\
X(-2) & X(-3) & \cdots & X(-p-1) \\
X(-3) & X(-4) & \cdots & X(-p-2) \\
\vdots & \vdots & \ddots & \vdots \\
X(-m) & X(-m-1) & \cdots & X(-(m-1+p))
\end{pmatrix}}_{A}
\;
\underbrace{\begin{pmatrix} a_1 \\ a_2 \\ \vdots \\ a_p \end{pmatrix}}_{a}
\tag{3.0}
$$

Jede Zeile ist eine Instanz der Modellgleichung (2.1), verankert an einem anderen Zeitpunkt. Beim Übergang von einer Zeile zur nächsten rutscht das gesamte Fenster um einen Hop nach hinten; deshalb wiederholen sich die Einträge diagonal.

Da (3.0) für $m > p$ im Allgemeinen nicht exakt erfüllbar ist, misst man den Fehlbetrag jeder einzelnen Zeile.

**Definition 3.3.** Für $a \in \mathbb{C}^p$ heißt

$$
r(a) \;:=\; b - A\,a \;\in\; \mathbb{C}^{m}
$$

das **Residuum**. Seine Komponenten sind

$$
r_j(a) \;=\; X(-j) - \sum_{\kappa=1}^{p} a_\kappa\, X(-j-\kappa), \qquad j = 0,\dots,m-1.
$$

Für $m > p$ ist $Aa = b$ im Allgemeinen unlösbar. Wir minimieren stattdessen

$$
J(a) \;:=\; \lVert r(a)\rVert_2^2 \;=\; r(a)^{\mathsf H} r(a) \;=\; \sum_{j=0}^{m-1} \lvert r_j(a)\rvert^2 , \tag{3.1}
$$

wobei $(\cdot)^{\mathsf H}$ die konjugiert Transponierte bezeichnet, also $(M^{\mathsf H})_{\kappa j} := \overline{M_{j\kappa}}$.

### 3.2 Warum die Quadratsumme

Drei unabhängige Begründungen fallen zusammen:

1. **Geometrisch.** Die $2$-Norm stammt von einem Skalarprodukt. Nur dann existiert eine Orthogonalprojektion und damit eine geschlossene Lösung.
2. **Statistisch.** Bei additivem, unabhängigem, komplex-gaußschem Rauschen gleicher Varianz ist der Minimierer von (3.1) der Maximum-Likelihood-Schätzer.
3. **Algebraisch.** $J$ ist quadratisch in $a$, also ist die Optimalitätsbedingung linear.

### 3.3 Die Idee: Orthogonalprojektion

Bevor gerechnet wird, die geometrische Vorstellung — sie liefert die Lösung praktisch von selbst.

Die Menge aller erreichbaren rechten Seiten ist der **Spaltenraum**

$$
\operatorname{im} A \;=\; \{\, Aa \;:\; a \in \mathbb{C}^p \,\} \;\subseteq\; \mathbb{C}^m ,
$$

ein Unterraum der Dimension $\operatorname{rang} A \le p$. Der Beobachtungsvektor $b$ liegt für $m > p$ im Allgemeinen **nicht** darin: Genau deshalb ist (3.0) unlösbar.

Gesucht ist folglich derjenige Punkt des Spaltenraums, der $b$ am nächsten liegt. Das ist die **Orthogonalprojektion** von $b$ auf $\operatorname{im} A$, und ihre kennzeichnende Eigenschaft ist die kürzeste Verbindung: Der Restvektor steht senkrecht auf dem Unterraum.

Senkrecht auf einem Unterraum zu stehen heißt, senkrecht auf jedem seiner Erzeuger zu stehen. Die Erzeuger sind die Spalten von $A$, also lautet die Bedingung

$$
\langle \text{Spalte } \kappa,\; \hat r \rangle \;=\; 0 \qquad \text{für alle } \kappa = 1,\dots,p ,
$$

und diese $p$ Gleichungen zusammengefasst sind nichts anderes als

$$
A^{\mathsf H}\,\hat r \;=\; 0 . \tag{3.3}
$$

**Die Multiplikation mit $A^{\mathsf H}$ ist damit die formale Fassung der Orthogonalitätsbedingung.** Setzt man $\hat r = b - A\hat a$ ein, folgt unmittelbar $A^{\mathsf H}A\,\hat a = A^{\mathsf H}b$.

Die Konjugation ist dabei wesentlich: Das Skalarprodukt auf $\mathbb{C}^m$ ist sesquilinear, $\langle u,v\rangle = v^{\mathsf H}u$. Mit $A^{\mathsf T}$ statt $A^{\mathsf H}$ würde eine Bilinearform verwendet, die keine Norm induziert; das Ergebnis minimierte nichts.

*Alternativer Zugang.* Man kann stattdessen $\nabla J = 0$ fordern. Da $J$ reellwertig und damit nicht holomorph ist, geht das nicht über den komplexen Differenzenquotienten, sondern über den Wirtinger-Operator $\partial/\partial\bar a$, der $a$ und $\bar a$ formal als unabhängig behandelt. Aus

$$
J(a) \;=\; b^{\mathsf H}b - b^{\mathsf H}Aa - a^{\mathsf H}A^{\mathsf H}b + a^{\mathsf H}A^{\mathsf H}Aa
$$

folgt $\partial J/\partial\bar a = A^{\mathsf H}Aa - A^{\mathsf H}b$, also dieselbe Bedingung. Dieser Weg liefert allerdings nur Stationarität; für ein Minimum braucht er zusätzlich die positive Semidefinitheit von $A^{\mathsf H}A$. Der folgende Beweis kommt ohne beides aus.

### 3.4 Normalgleichungen

**Definition 3.4.** Wir setzen

$$
P \;:=\; A^{\mathsf H} A \;\in\; \mathbb{C}^{p \times p}, \qquad q \;:=\; A^{\mathsf H} b \;\in\; \mathbb{C}^{p}.
$$

$P$ ist die **Gram-Matrix** der Spalten von $A$, also die Tabelle ihrer paarweisen Skalarprodukte. Sie ist hermitesch und positiv semidefinit. Ihre Größe hängt allein von $p$ ab, nicht von $m$.

**Satz 3.5.** Ein Vektor $\hat a \in \mathbb{C}^p$ minimiert $J$ genau dann, wenn

$$
P\,\hat a \;=\; q. \tag{3.2}
$$

**Beweis.**

*Hinlänglichkeit.* Sei $\hat a$ eine Lösung von (3.2) und $\hat r := r(\hat a)$. Dann gilt

$$
A^{\mathsf H}\hat r \;=\; A^{\mathsf H} b - A^{\mathsf H}A\hat a \;=\; q - P\hat a \;=\; 0,
$$

also gerade die Orthogonalitätsbedingung (3.3). Sei $a \in \mathbb{C}^p$ beliebig und $d := \hat a - a$. Dann ist

$$
r(a) \;=\; b - Aa \;=\; (b - A\hat a) + A(\hat a - a) \;=\; \hat r + Ad,
$$

also

$$
J(a) \;=\; (\hat r + Ad)^{\mathsf H}(\hat r + Ad) \;=\; \hat r^{\mathsf H}\hat r + \hat r^{\mathsf H} A d + d^{\mathsf H} A^{\mathsf H}\hat r + d^{\mathsf H} A^{\mathsf H} A d .
$$

Wegen $A^{\mathsf H}\hat r = 0$ verschwindet $d^{\mathsf H}A^{\mathsf H}\hat r$, und ebenso $\hat r^{\mathsf H} A d = (A^{\mathsf H}\hat r)^{\mathsf H} d = 0$. Es bleibt

$$
J(a) \;=\; \lVert \hat r\rVert^2 + \lVert Ad\rVert^2 \;\ge\; J(\hat a),
$$

mit Gleichheit genau für $Ad = 0$. Das ist der Satz des Pythagoras für die Zerlegung von $r(a)$ in einen Anteil senkrecht zum Spaltenraum und einen darin.

*Notwendigkeit.* Sei $\tilde a$ keine Lösung von (3.2), also $g := q - P\tilde a = A^{\mathsf H} r(\tilde a) \neq 0$. Für $a_\varepsilon := \tilde a + \varepsilon g$ mit $\varepsilon > 0$ liefert dieselbe Zerlegung

$$
J(a_\varepsilon) \;=\; J(\tilde a) - 2\varepsilon \lVert g\rVert^2 + \varepsilon^2 \lVert Ag\rVert^2 ,
$$

was für hinreichend kleines $\varepsilon$ echt kleiner als $J(\tilde a)$ ist. Also ist $\tilde a$ kein Minimierer. $\square$

**Korollar 3.6.** Der Minimierer ist eindeutig genau dann, wenn $\operatorname{rang} A = p$ gilt, äquivalent $\det P \neq 0$.

### 3.5 Abgrenzung: das Residuum verschwindet nicht

Die Bedingung $A^{\mathsf H}\hat r = 0$ ist strikt schwächer als $\hat r = 0$: Erstere sind $p$ Gleichungen, Letztere $m$. Bei vollem Rang liegt $\hat r$ im orthogonalen Komplement des Spaltenraums, das die Dimension $m - p$ hat, und ist dort im Allgemeinen von null verschieden.

Es gilt $\hat r = 0$ genau dann, wenn $b \in \operatorname{im} A$, also wenn (3.0) exakt lösbar ist. Das ist der Fall

- bei $m = p$ und vollem Rang, da der Spaltenraum dann ganz $\mathbb{C}^m$ ist (klassische Prony-Interpolation, ohne Mittelung und entsprechend rauschempfindlich), oder
- wenn das Modell exakt zutrifft, die Bin-Trajektorie also wirklich eine Überlagerung von $p$ Exponentialfolgen ist.

Auf realem Material trifft beides nicht zu. Dort ist $\lVert\hat r\rVert$ ein Maß dafür, wie gut die Modellannahme aus §2.3 trägt: Je größer, desto mehr steckt im Bin, was keine endliche Rekursion erfüllt — veränderliche Frequenzen, Nichtstationarität, Rauschen.

*Zur Ordnungswahl.* $\lVert\hat r\rVert$ fällt monoton mit wachsendem $p$, weil der Spaltenraum größer wird. Ein kleineres Residuum bedeutet daher nicht notwendig ein besseres Modell; es kann Überanpassung sein. Die Ordnung wird deshalb am Vorhersagefehler außerhalb der Anpassungsdaten gewählt, nicht am Residuum.

---

## 4 Fortsetzung

Sei $\hat a$ die Lösung von (3.2). Die Fortsetzung ist die Iteration von (2.1):

$$
X(n) \;:=\; \sum_{\kappa=1}^{p} \hat a_\kappa\, X(n-\kappa), \qquad n = 1, 2, \dots \tag{4.1}
$$

Der Zustand besteht aus den letzten $p$ Werten. Pro Schritt fallen $p$ komplexe Multiplikationen und $p-1$ Additionen an.

**Hiermit ist die Fortsetzung vollständig bestimmt.** Die folgenden Abschnitte dienen der Interpretation (§5) und der Absicherung (§6), nicht der Berechnung.

---

## 5 Modaldarstellung

> **Vorbemerkung: Dieses Kapitel wird nicht gerechnet.** Die Fortsetzung ist mit §4 vollständig bestimmt. Was hier folgt, erklärt, *warum* die Rekursion tut, was sie tut, und liefert die Begriffe für die Stabilitätsprüfung in §6. Von den hier eingeführten Objekten wird im Algorithmus einzig die Nullstellenmenge von $\chi$ benutzt — Begleitmatrix, Vandermonde-Matrix, Jordanblöcke und die Modalzerlegung kommen im Code nicht vor.

### 5.1 Die Rekursion als lineare Abbildung

Die Fortsetzung (4.1) ist eine Iteration. Um zu verstehen, was sie über viele Schritte tut, schreibt man sie als wiederholte Anwendung einer festen Matrix.

**Definition 5.1.** Der Zustandsvektor ist

$$
s(n) \;:=\; (X(n), X(n-1),\dots,X(n-p+1))^{\mathsf T} \;\in\; \mathbb{C}^p .
$$

**Definition 5.2.** Die Begleitmatrix $C \in \mathbb{C}^{p\times p}$ ist

$$
C \;:=\; \begin{pmatrix} \hat a_1 & \hat a_2 & \cdots & \hat a_{p-1} & \hat a_p \\ 1 & 0 & \cdots & 0 & 0 \\ 0 & 1 & \cdots & 0 & 0 \\ \vdots & & \ddots & & \vdots \\ 0 & 0 & \cdots & 1 & 0 \end{pmatrix}.
$$

Die erste Zeile ist die Rekursion selbst; die Einsen darunter schieben den Zustand um eine Stelle weiter. Damit ist (4.1) äquivalent zu

$$
s(n) \;=\; C\, s(n-1), \qquad\text{also}\qquad s(n) \;=\; C^{\,n}\, s(0). \tag{5.1}
$$

**Die ganze Fortsetzung steckt damit in den Potenzen von $C$.** Und Potenzen einer Matrix sind genau dann leicht zu verstehen, wenn man ihre **Eigenwerte** kennt: Ist $C v = z v$, so ist $C^{\,n} v = z^{\,n} v$. Zerlegt man $s(0)$ in Eigenvektoren, wird aus der Matrixpotenz eine Summe von Zahlenpotenzen.

Das Eigenwertproblem für $C$ ist also nicht ein weiteres Thema, sondern **die** Frage, auf die (5.1) hinausläuft.

### 5.2 Das charakteristische Polynom

Eigenwerte bestimmt man wie üblich über die Singularitätsbedingung.

**Definition 5.3.** Das charakteristische Polynom von $C$ ist $\det(C - z\,\mathrm{Id})$. Ausgerechnet ergibt es bis auf das Vorzeichen $(-1)^p$

$$
\chi(z) \;:=\; z^{p} - \sum_{\kappa=1}^{p} \hat a_\kappa\, z^{\,p-\kappa}, \qquad z \in \mathbb{C}.
$$

Die Eigenwerte von $C$ sind somit genau die Nullstellen von $\chi$.

**Die Eigenvektoren.** Zu einem Eigenwert $z \neq 0$ gehört

$$
v_z \;:=\; (z^{\,p-1}, z^{\,p-2},\dots,z,\,1)^{\mathsf T} .
$$

Probe: Die Zeilen $2$ bis $p$ von $C v_z = z v_z$ lauten $z^{\,p-\mu} = z \cdot z^{\,p-\mu-1}$ und sind trivial erfüllt; die erste Zeile lautet $\sum_\kappa \hat a_\kappa z^{\,p-\kappa} = z^{\,p}$ und ist genau $\chi(z) = 0$.

**Derselbe Sachverhalt von der Folgenseite.** Statt Eigenvektoren in $\mathbb{C}^p$ kann man direkt nach Lösungsfolgen von (2.1) suchen. Der Ansatz

$$
u_z \;:=\; (z^{\,n})_{n \in \mathbb{Z}}, \qquad z \in \mathbb{C}\setminus\{0\},
$$

eingesetzt in (2.1) liefert

$$
z^{\,n} \;=\; \sum_{\kappa=1}^{p} \hat a_\kappa\, z^{\,n-\kappa};
$$

Division durch $z^{\,n-p}$ ergibt wieder $\chi(z) = 0$.

Beide Wege sind dasselbe: Der Zustandsvektor der Folge $u_z$ ist $s(n) = z^{\,n-p+1} v_z$, also stets ein Vielfaches des Eigenvektors.

Der Ansatz ist gerechtfertigt, weil (2.1) verschiebungsinvariant ist und $u_z$ die Eigenfolge des Verschiebungsoperators zum Eigenwert $z$ ist. Er ist **keine Annahme über den Zustand**: Eine konkrete Trajektorie ist im Allgemeinen eine Überlagerung, kein einzelnes $u_z$.

**$P$ und $C$ sind verschiedene Objekte.** $P$ entsteht aus den Messdaten und wird gelöst; $C$ entsteht aus dem Ergebnis und wird diagonalisiert. $C$ steht hinter $P$ in der Kette, nicht daneben.

### 5.3 Lösungsraum

Sei

$$
L \;:=\; \left\{\, y = (y_n)_{n\in\mathbb{Z}} \in \mathbb{C}^{\mathbb{Z}} \;\middle|\; y_n = \sum_{\kappa=1}^{p} \hat a_\kappa\, y_{n-\kappa} \ \text{ für alle } n \,\right\}.
$$

Die Abbildung $y \mapsto (y_0,\dots,y_{p-1})$ ist ein Isomorphismus von $L$ nach $\mathbb{C}^p$; folglich ist $\dim L = p$.

**Fall 1: paarweise verschiedene Nullstellen.** Seien $z_1,\dots,z_p \in \mathbb{C}\setminus\{0\}$ die Nullstellen von $\chi$, paarweise verschieden. Die Folgen $u_{z_1},\dots,u_{z_p}$ liegen in $L$, und die Matrix ihrer Anfangswerte ist die Vandermonde-Matrix

$$
V \;:=\; (z_\lambda^{\,\mu})_{\mu=0,\dots,p-1;\ \lambda=1,\dots,p}, \qquad \det V \;=\; \prod_{\lambda < \lambda'} (z_{\lambda'} - z_\lambda).
$$

Für paarweise verschiedene Nullstellen ist $\det V \neq 0$. Ihre Spalten sind gerade die Anfangswerte der Eigenfolgen, in Matrixsprache also die Eigenvektoren $v_{z_\lambda}$ aus §5.2 in umgekehrter Zeilenreihenfolge; $\det V \neq 0$ bedeutet somit genau, dass $C$ diagonalisierbar ist. Damit bilden die $u_{z_\lambda}$ eine Basis von $L$, und jede Lösung besitzt eine eindeutige Darstellung

$$
y_n \;=\; \sum_{\lambda=1}^{p} c_\lambda\, z_\lambda^{\,n},
$$

wobei $c \in \mathbb{C}^p$ aus den Anfangswerten folgt.

**Fall 2: mehrfache Nullstelle.** Ist $z_0$ eine Nullstelle der Vielfachheit $\mu \ge 2$, so fallen $\mu$ der obigen Kandidaten zusammen und $\det V = 0$. Die Menge $\{u_{z_\lambda}\}$ ist dann **keine** Basis mehr. Zu ergänzen sind die Folgen

$$
v^{(\sigma)} \;:=\; (n^{\sigma}\, z_0^{\,n})_{n\in\mathbb{Z}}, \qquad \sigma = 1,\dots,\mu-1 .
$$

Nachweis für $\mu = 2$ und $\sigma = 1$: Mit $\chi(z) = (z-z_0)^2$ ist $\hat a_1 = 2z_0$ und $\hat a_2 = -z_0^2$, also

$$
\hat a_1 (n-1) z_0^{\,n-1} + \hat a_2 (n-2) z_0^{\,n-2} \;=\; 2(n-1)z_0^{\,n} - (n-2)z_0^{\,n} \;=\; n\,z_0^{\,n}.
$$

Die Folgen $u_{z_0}$ und $v^{(1)}$ sind linear unabhängig: Aus $n z_0^{\,n} = c\, z_0^{\,n}$ für alle $n$ folgte $n = c$ für alle $n$.

*Anmerkung.* Die Folge $v^{(\sigma)}$ entsteht auch als Grenzwert. Für $z_1 \neq z_2$ ist der Ausdruck $(z_1^{\,n} - z_2^{\,n})/(z_1 - z_2)$ eine Linearkombination der alten Basis, also nichts Neues; für $z_2 \to z_1 = z_0$ liefert dieser Differenzenquotient $n z_0^{\,n-1}$. Die Ergänzung ist somit die stetige Fortsetzung, keine Zusatzannahme.

Äquivalent in Matrixsprache: Bei mehrfacher Nullstelle ist $C$ nicht diagonalisierbar. Der zugehörige Jordanblock der Größe $2$ erfüllt

$$
\begin{pmatrix} z_0 & 1 \\ 0 & z_0 \end{pmatrix}^{n} \;=\; \begin{pmatrix} z_0^{\,n} & n z_0^{\,n-1} \\ 0 & z_0^{\,n} \end{pmatrix},
$$

trägt den Faktor $n$ also explizit in der Nebendiagonale.

### 5.4 Physikalische Deutung

Jede Nullstelle $z_\lambda$ beschreibt eine Komponente im Bin:

| Größe | Bedeutung |
|---|---|
| $\arg z_\lambda$ | Phasenvorschub pro Hop, also Frequenz |
| $\lvert z_\lambda\rvert$ | Amplitudenfaktor pro Hop |
| Vielfachheit $\mu$ | Grad des Amplitudenpolynoms, nämlich $\mu - 1$ |

Insbesondere gilt:

- $\lvert z\rvert = 1$, einfach: konstante Amplitude,
- $\lvert z\rvert = 1$, doppelt: **linear** wachsende Amplitude bei konstanter Frequenz, also ein Crescendo ohne exponentielles Wachstum,
- $\lvert z\rvert < 1$: exponentielles Abklingen,
- $\lvert z\rvert > 1$: exponentielles Wachstum.

Setzt man $X(n) = z_0^{\,n}\, Y(n)$, wechselt also in ein mitrotierendes Bezugssystem, so wird die Rekursion mit Doppelwurzel bei $z_0$ zu $Y(n) = 2Y(n-1) - Y(n-2)$. Das ist **lineare Extrapolation der Amplitude auf einem drehenden Träger**.

### 5.5 Aliasgrenze der Trennung

Zwei Komponenten mit Kreisfrequenzen $\omega_1$ und $\omega_2$ in rad pro Sample liefern Nullstellen mit dem Winkelabstand $\lvert\omega_1 - \omega_2\rvert \cdot H$. Eindeutig auflösbar ist das nur für

$$
\lvert\omega_1 - \omega_2\rvert\, H \;<\; \pi \qquad\Longleftrightarrow\qquad \lvert f_1 - f_2\rvert \;<\; \frac{f_s}{2H},
$$

mit der Abtastrate $f_s$. Andererseits wächst der Winkelabstand, und damit die Kondition, mit $H$.

Der größte Frequenzabstand, den ein Bin überhaupt enthalten kann, ist die Breite der Hann-Hauptkeule, also $4 f_s / N$. Gleichsetzen liefert

$$
H_{\text{opt}} \;=\; \frac{N}{8}.
$$

---

## 6 Stabilisierung

Der Fit kann Nullstellen mit $\lvert z\rvert > 1$ liefern; deren Beitrag wächst über die Lücke exponentiell. Das ist nicht der Ausnahmefall: Ein stationärer Ton hat Nullstellen *auf* dem Einheitskreis, und jede Schätzstörung verschiebt sie mit etwa gleicher Wahrscheinlichkeit nach außen wie nach innen. Gemessen auf einem Orgelakkord lagen 81 % der Bin-Energie in Bins mit $\max\lvert z\rvert > 1$.

### 6.1 Selektive Kappung — der Ablauf

```
Eingabe:  a[1..p]                      Koeffizienten aus (3.2)

1  Begleitmatrix C aufstellen          (Definition 5.2)
2  Eigenwerte bestimmen  ->  z[1..p]   praktisch: Durand-Kerner auf chi
3  fuer jedes lambda:
       falls |z[lambda]| > 1:  z[lambda] <- z[lambda] / |z[lambda]|
4  c <- [1]
   fuer jedes lambda:  c <- Faltung(c, [1, -z[lambda]])
   a[kappa] <- -c[kappa]               fuer kappa = 1..p

Ausgabe:  korrigiertes a[1..p]  ->  weiter mit (4.1)
```

Der Zustandsvektor wird dabei **nicht** angefasst. Geändert wird das System, nicht sein Zustand; deshalb ist keine Zerlegung in Eigenräume nötig.

### 6.2 Warum Schritt 4 so aussieht

Die Brücke von den Nullstellen zurück zu den Koeffizienten führt nicht über die Matrix, sondern über das Polynom — dasselbe Objekt in beiden Rollen.

Nach dem Fundamentalsatz der Algebra zerfällt das normierte Polynom $\chi$ vollständig in Linearfaktoren:

$$
\chi(z) \;=\; \prod_{\lambda=1}^{p} (z - z_\lambda),
$$

wobei mehrfache Nullstellen entsprechend oft auftreten. Ausmultipliziert ergibt das

$$
\chi(z) \;=\; z^{p} + \sum_{\kappa=1}^{p} (-1)^{\kappa}\, e_\kappa(z_1,\dots,z_p)\; z^{\,p-\kappa},
$$

mit den elementarsymmetrischen Funktionen $e_1 = \sum_\lambda z_\lambda$, $e_2 = \sum_{\lambda<\lambda'} z_\lambda z_{\lambda'}$, und so weiter bis $e_p = \prod_\lambda z_\lambda$.

Koeffizientenvergleich mit Definition 5.3, also mit $\chi(z) = z^{p} - \sum_\kappa \hat a_\kappa z^{\,p-\kappa}$, liefert

$$
\hat a_\kappa \;=\; (-1)^{\kappa+1}\, e_\kappa(z_1,\dots,z_p). \tag{6.1}
$$

Für $p = 2$ ist das $\hat a_1 = z_1 + z_2$ und $\hat a_2 = -z_1 z_2$.

Praktisch berechnet man (6.1) nicht über die Summenformeln, sondern durch schrittweises Ausmultiplizieren: Das Produkt zweier Polynome ist die Faltung ihrer Koeffizientenvektoren, und jeder Linearfaktor $(z - z_\lambda)$ hat den Koeffizientenvektor $[1, -z_\lambda]$. Das sind $p$ Faltungen mit einem zweigliedrigen Vektor, insgesamt $O(p^2)$.

### 6.3 Warum mehrfache Nullstellen keinen Sonderfall brauchen

Die Schritte 3 und 4 arbeiten auf der **Multimenge** der Nullstellen, nicht auf einer Basis des Lösungsraums. Zwei zusammenfallende Nullstellen erhalten in Schritt 3 denselben Skalierungsfaktor und bleiben damit zusammenfallend; Schritt 4 multipliziert den Linearfaktor entsprechend zweifach ein. Das Produkt $\prod_\lambda (z - z_\lambda)$ ist für Multimengen wohldefiniert.

Die Ergänzung um $(n^\sigma z_0^{\,n})_n$ aus §5.3 wird nur gebraucht, wenn man die Lösung **explizit als Linearkombination** hinschreiben will. Das tut der Algorithmus nirgends — er ändert Koeffizienten und iteriert (4.1), und die Rekursion kennt keinen Unterschied zwischen einfachen und mehrfachen Nullstellen.

*Numerische Anmerkung.* Der Weg Koeffizienten, Nullstellen, Kappung, Koeffizienten ist insgesamt gutartig, obwohl der Zwischenschritt es nicht ist: Bei fast doppelter Nullstelle wandern die einzelnen Nullstellen mit $\sqrt{\varepsilon}$, ihre elementarsymmetrischen Funktionen jedoch nur mit $\varepsilon$. Schritt 4 kürzt den Fehler aus Schritt 2 also teilweise wieder heraus.

### 6.4 Der Schur-Cohn-Test

Schritt 2 des Ablaufs ist die einzige Iteration und die einzige numerisch fragile Stufe der ganzen Kette. Es gibt einen endlichen Weg, Stabilität zu entscheiden, **ohne eine einzige Nullstelle zu berechnen**. Da er auch die Grundlage der Alternativen in §6.5 ist, hier vollständig.

#### 6.4.1 Grundlage: $z^{-1}$ als Verzögerung

Die folgende Darstellung benutzt die in der Signalverarbeitung übliche Schreibweise mit negativen Potenzen. Sie ist eine reine Umnotation und geht auf die z-Transformation zurück:

$$
\hat X(z) \;:=\; \sum_{n} X(n)\, z^{-n}
$$

Das Dach bezeichnet hier und im Folgenden die Transformierte, der nackte Buchstabe die Folge selbst. (In Kapitel 3 markiert das Dach die Schätzung, etwa $\hat a$ und $\hat r$; eine Verwechslung ist ausgeschlossen, da dort keine Transformierten vorkommen und hier keine Schätzgrößen mit Dach neu eingeführt werden.) Ist $(Y(n))_n$ die um einen Schritt verzögerte Folge mit $Y(n) := X(n-1)$ und $\hat Y$ ihre Transformierte, so gilt

$$
\hat Y(z) \;=\; \sum_n X(n-1)\, z^{-n} \;=\; \sum_{\nu} X(\nu)\, z^{-\nu-1} \;=\; z^{-1}\,\hat X(z).
$$

**Die Multiplikation mit $z^{-1}$ ist also genau die Verzögerung um einen Hop.** Die negativen Potenzen zählen Verzögerungsstufen.

Angewandt auf den Vorhersagefehler

$$
e(n) \;:=\; X(n) - \sum_{\kappa=1}^{p} \hat a_\kappa\, X(n-\kappa)
$$

wird daraus, mit dem Verschiebungssatz auf jeden Summanden und der Linearität der Transformation,

$$
\hat E(z) \;=\; \hat X(z) - \sum_{\kappa=1}^{p} \hat a_\kappa\, z^{-\kappa}\,\hat X(z) \;=\; \hat X(z)\,\hat\alpha(z)
$$

mit

$$
\hat\alpha(z) \;:=\; 1 - \sum_{\kappa=1}^{p} \hat a_\kappa\, z^{-\kappa} .
$$

Das ist der **Faltungssatz** der z-Transformation im endlichen Fall. Damit die Faltung überhaupt definiert ist, wird die Koeffizientenfolge als Funktion auf ganz $\mathbb{Z}$ aufgefasst, außerhalb ihres Trägers durch null fortgesetzt:

$$
\alpha : \mathbb{Z} \to \mathbb{C}, \qquad \alpha(\kappa) := \begin{cases} 1 & \kappa = 0\\ -\hat a_\kappa & 1 \le \kappa \le p\\ 0 & \text{sonst.}\end{cases}
$$

Mit $\alpha(0) = 1$ ist der freistehende Term $X(n)$ gerade der Summand $\kappa = 0$, sodass

$$
e(n) \;=\; \alpha(0)\,X(n) + \sum_{\kappa=1}^{p} \alpha(\kappa)\,X(n-\kappa) \;=\; \sum_{\kappa \in \mathbb{Z}} \alpha(\kappa)\, X(n-\kappa) \;=\; (\alpha * X)(n)
$$

vollständig als Faltung geschrieben ist; die Summe über $\mathbb{Z}$ kollabiert dabei auf $p+1$ Terme. Faltung im Zeitbereich wird zum Produkt im Bildbereich, und $\hat\alpha$ ist die z-Transformierte von $\alpha$ im selben Sinn, in dem $\hat X$ die von $(X(n))_n$ ist. Hier folgt das unmittelbar aus Verschiebungssatz und Linearität, weil die Summe endlich ist.

$\hat\alpha$ heißt **Prädiktorfehlerfilter**: Er bildet das Signal auf den Vorhersagefehler ab. Seine Koeffizienten stehen in derselben Reihenfolge wie in der Rekursion, mit $\alpha(0) = 1$ für den Gegenwartswert.

*Zur Bezeichnung.* Das Dach hat hier dieselbe Bedeutung wie bei $\hat X$ und $\hat E$: $\hat\alpha$ ist die z-Transformierte der Folge $\alpha$. In der Literatur heißt dieses Polynom meist $A(z)$; der Buchstabe $A$ ist im vorliegenden Text durch die Beobachtungsmatrix aus §3.1 belegt.

Der Zusammenhang zum charakteristischen Polynom ist eine Multiplikation mit $z^{p}$:

$$
z^{p}\,\hat\alpha(z) \;=\; z^{p} - \sum_{\kappa=1}^{p} \hat a_\kappa\, z^{\,p-\kappa} \;=\; \chi(z),
$$

beide haben also dieselben Nullstellen außer $z=0$. Für den Schur-Cohn-Test ist die Form $\hat\alpha$ die praktischere: Die Normierung sitzt auf dem konstanten Term und bleibt beim Grad-Abstieg erhalten, und die in §6.4.3 benötigte Spiegelung reduziert sich in dieser Schreibweise auf das Umkehren des Koeffizientenvektors.

#### 6.4.2 Umformulierung

Der Test wird auf $\hat\alpha$ formuliert statt auf $\chi$. Gesucht ist: liegen alle Nullstellen von $\hat\alpha$ in der offenen Einheitskreisscheibe?

Dabei sind zwei Objekte auseinanderzuhalten:

$$
\alpha^{(p)} \;:=\; (\alpha(0),\, \alpha(1),\, \dots,\, \alpha(p)) \;\in\; \mathbb{C}^{p+1}, \qquad \alpha(0) = 1,
$$

der **Koeffizientenvektor**, und die skalare Funktion

$$
\hat\alpha : \mathbb{C}\setminus\{0\} \to \mathbb{C}, \qquad \hat\alpha(z) = \sum_{\kappa=0}^{p} \alpha^{(p)}_\kappa\, z^{-\kappa},
$$

das zugehörige **Polynom**. Auf Polynomen vom Grad höchstens $p$ ist diese Zuordnung bijektiv, beide Sichten sind also austauschbar — aber nicht dasselbe Objekt. Die Sätze dieses Abschnitts werden über die Funktion formuliert, weil der Beweis in §6.4.5 Funktionswerte auf dem Einheitskreis auswertet; gerechnet wird in der Implementierung ausschließlich auf dem Vektor, siehe (6.3) und (6.4).

Der obere Index bezeichnet den Grad. Das Verfahren steigt von $\ell = p$ schrittweise auf $\ell = 0$ herunter; dabei entstehen Vektoren $\alpha^{(\ell)} \in \mathbb{C}^{\ell+1}$ mit $\alpha^{(\ell)}_0 = 1$ und die zugehörigen Polynome $\hat\alpha_\ell$, beginnend bei $\alpha^{(p)}$ und $\hat\alpha_p := \hat\alpha$.

#### 6.4.3 Das gespiegelte Polynom

**Was gebraucht wird.** Der Abstieg in §6.4.4 zieht von $\hat\alpha_\ell$ ein Vielfaches eines zweiten Polynoms ab, um den Grad zu senken. Damit dabei die Nullstellenbilanz kontrollierbar bleibt, muss dieses zweite Polynom zwei Dinge leisten: Es soll auf dem Einheitskreis **denselben Betrag** haben wie $\hat\alpha_\ell$ — dann greift im Beweis der Satz von Rouché — und es soll den Grad $\ell$ mit **führendem Koeffizienten $1$** haben, damit sich der höchste Term wegheben kann.

**Die Konstruktion.** Beides leistet die Umkehrung der Koeffizientenreihenfolge, verbunden mit Konjugation.

**Definition 6.1.** Zu $\alpha^{(\ell)} \in \mathbb{C}^{\ell+1}$ ist der **gespiegelte Koeffizientenvektor** $\alpha^{(\ell),R} \in \mathbb{C}^{\ell+1}$ definiert durch

$$
\alpha^{(\ell),R}_j \;:=\; \overline{\alpha^{(\ell)}_{\ell-j}}, \qquad j = 0,\dots,\ell,
$$

und $\hat\alpha_\ell^{R}$ ist das zugehörige Polynom, also $\hat\alpha_\ell^{R}(z) = \sum_{j=0}^{\ell} \alpha^{(\ell),R}_j\, z^{-j}$.

Der erste Eintrag ist $\alpha^{(\ell),R}_0 = \overline{\alpha^{(\ell)}_\ell}$, der letzte ist $\alpha^{(\ell),R}_\ell = \overline{\alpha^{(\ell)}_0} = 1$. Vorne und hinten sind also vertauscht: Wo $\hat\alpha_\ell$ die $1$ trägt, steht bei $\hat\alpha_\ell^{R}$ der letzte Koeffizient, und umgekehrt.

**Geschlossene Form.** Dieselbe Operation lässt sich ohne Indizes schreiben. Aus $\hat\alpha_\ell(w) = \sum_\kappa \alpha^{(\ell)}_\kappa w^{-\kappa}$ folgt mit $w = 1/\bar z$

$$
\overline{\hat\alpha_\ell(1/\bar z)} \;=\; \overline{\sum_\kappa \alpha^{(\ell)}_\kappa\, \bar z^{\,\kappa}} \;=\; \sum_\kappa \overline{\alpha^{(\ell)}_\kappa}\, z^{\kappa},
$$

und Multiplikation mit $z^{-\ell}$ verschiebt die Exponenten von $\kappa$ auf $\kappa - \ell$. Mit $j := \ell - \kappa$ ist das genau Definition 6.1:

$$
\hat\alpha_\ell^{R}(z) \;=\; z^{-\ell}\,\overline{\hat\alpha_\ell(1/\bar z)} . \tag{6.6}
$$

Der Faktor $z^{-\ell}$ dient also nur dazu, die durch die Umkehrung nach oben gewanderten Exponenten wieder in den Bereich $z^{0}$ bis $z^{-\ell}$ zu holen. Trotz der Konjugation ist $\hat\alpha_\ell^{R}$ ein gewöhnliches Polynom in $z^{-1}$ — konjugiert werden nur die Koeffizienten, nicht die Variable.

**Beispiel $\ell = 2$.** Aus $\alpha^{(2)} = (1,\, \alpha_1,\, \alpha_2)$ wird $\alpha^{(2),R} = (\overline{\alpha_2},\, \overline{\alpha_1},\, 1)$, also

$$
\hat\alpha_2(z) = 1 + \alpha_1 z^{-1} + \alpha_2 z^{-2}, \qquad \hat\alpha_2^{R}(z) = \overline{\alpha_2} + \overline{\alpha_1}\, z^{-1} + z^{-2}.
$$

**(S1) Die Nullstellen werden am Einheitskreis gespiegelt.** Ist $z_0 \neq 0$ eine Nullstelle von $\hat\alpha_\ell$, so ist $1/\bar z_0$ eine Nullstelle von $\hat\alpha_\ell^{R}$.

*Beweis.* Einsetzen von $z = 1/\bar z_0$ in (6.6) ergibt $1/\bar z = z_0$, also $\hat\alpha_\ell^{R}(1/\bar z_0) = (1/\bar z_0)^{-\ell}\,\overline{\hat\alpha_\ell(z_0)} = 0$. $\square$

Wegen $\lvert 1/\bar z_0\rvert = 1/\lvert z_0\rvert$ wird dabei aus innen außen und aus außen innen; Nullstellen auf dem Kreis bleiben darauf.

**(S2) Auf dem Einheitskreis stimmen die Beträge überein.** Für $\lvert z\rvert = 1$ gilt $\lvert \hat\alpha_\ell^{R}(z)\rvert = \lvert \hat\alpha_\ell(z)\rvert$.

*Beweis.* Für $\lvert z\rvert = 1$ ist $1/\bar z = z$ und $\lvert z^{-\ell}\rvert = 1$, also nach (6.6)

$$
\lvert \hat\alpha_\ell^{R}(z)\rvert \;=\; \lvert z^{-\ell}\rvert \cdot \left\lvert \overline{\hat\alpha_\ell(z)} \right\rvert \;=\; \lvert \hat\alpha_\ell(z)\rvert . \qquad\square
$$

Die beiden Eigenschaften sind der Grund für die Konstruktion: (S2) liefert die Betragsschranke, die Rouché in §6.4.5 benötigt, und (S1) erklärt, warum der Abzug eines Vielfachen von $\hat\alpha_\ell^{R}$ die Nullstellenbilanz gerade in der gewünschten Weise verändert.

#### 6.4.4 Der Abstiegsschritt

**Definition 6.2.** Der $\ell$-te **Reflexionskoeffizient** ist der letzte Koeffizient,

$$
k_\ell \;:=\; \alpha^{(\ell)}_\ell ,
$$

und das um einen Grad reduzierte Polynom ist

$$
\hat\alpha_{\ell-1}(z) \;:=\; \frac{\hat\alpha_\ell(z) - k_\ell\,\hat\alpha_\ell^{R}(z)}{1 - \lvert k_\ell\rvert^2} . \tag{6.2}
$$

Sein Koeffizientenvektor $\alpha^{(\ell-1)} \in \mathbb{C}^{\ell}$ ergibt sich komponentenweise zu

$$
\alpha^{(\ell-1)}_i \;=\; \frac{\alpha^{(\ell)}_i - k_\ell\,\overline{\alpha^{(\ell)}_{\ell-i}}}{1 - \lvert k_\ell\rvert^2}, \qquad i = 1,\dots,\ell-1, \tag{6.3}
$$

mit $\alpha^{(\ell-1)}_0 = 1$.

*Der Grad sinkt.* Der Koeffizient zu $z^{-\ell}$ ist in $\hat\alpha_\ell$ gerade $\alpha^{(\ell)}_\ell = k_\ell$ und in $\hat\alpha_\ell^{R}$ gerade $\alpha^{(\ell),R}_\ell = 1$; in der Differenz also $k_\ell - k_\ell\cdot 1 = 0$.

*Die Normierung bleibt.* Der Koeffizient zu $z^{0}$ ist $(1 - k_\ell\overline{\alpha^{(\ell)}_\ell})/(1-\lvert k_\ell\rvert^2) = (1-\lvert k_\ell\rvert^2)/(1-\lvert k_\ell\rvert^2) = 1$.

**Wichtig: die Nullstellen bleiben nicht erhalten.** Der Abstieg ist keine Abspaltung eines Linearfaktors. $\hat\alpha_{\ell-1}$ hat im Allgemeinen völlig andere Nullstellen als $\hat\alpha_\ell$ — erhalten bleibt nur die Eigenschaft „alle liegen innerhalb des Einheitskreises". Warum das so ist, zeigt der nächste Abschnitt.

#### 6.4.5 Warum $\lvert k_\ell\rvert < 1$ das Kriterium ist

Für das Zählen von Nullstellen innerhalb einer Kurve ist die gewöhnliche Polynomschreibweise handlicher. Setze deshalb

$$
P_\ell(z) \;:=\; z^{\ell}\,\hat\alpha_\ell(z) \;=\; \sum_{\kappa=0}^{\ell} \alpha^{(\ell)}_\kappa\, z^{\,\ell-\kappa},
\qquad
P_\ell^{*}(z) \;:=\; z^{\ell}\,\hat\alpha_\ell^{R}(z) .
$$

$P_\ell$ ist normiert vom Grad $\ell$ mit konstantem Term $\alpha^{(\ell)}_\ell = k_\ell$; die Nullstellen von $P_\ell$ und $\hat\alpha_\ell$ stimmen überein, abgesehen von $z=0$. Nach (S2) gilt weiterhin $\lvert P_\ell^{*}(z)\rvert = \lvert P_\ell(z)\rvert$ für $\lvert z\rvert = 1$.

**Satz 6.3.** Sei $\lvert k_\ell\rvert < 1$. Dann gilt: Alle Nullstellen von $\hat\alpha_\ell$ liegen genau dann in der offenen Einheitskreisscheibe, wenn alle Nullstellen von $\hat\alpha_{\ell-1}$ dort liegen.

**Beweis.** Bezeichne $N(\cdot)$ die Anzahl der Nullstellen in der offenen Einheitskreisscheibe, mit Vielfachheit gezählt.

*Schritt 1 (Rouché).* Auf $\lvert z\rvert = 1$ ist

$$
\lvert k_\ell\, P_\ell^{*}(z)\rvert \;=\; \lvert k_\ell\rvert\cdot\lvert P_\ell(z)\rvert \;<\; \lvert P_\ell(z)\rvert ,
$$

da $\lvert k_\ell\rvert < 1$ und $P_\ell$ auf dem Kreis nullstellenfrei ist (andernfalls stünde dort Gleichheit von $0$ und $0$; dieser Randfall wird unten behandelt). Nach dem Satz von Rouché haben $P_\ell$ und $P_\ell - k_\ell P_\ell^{*}$ innerhalb des Kreises gleich viele Nullstellen:

$$
N(P_\ell - k_\ell P_\ell^{*}) \;=\; N(P_\ell).
$$

*Schritt 2 (die künstliche Nullstelle bei null).* Der konstante Term von $P_\ell - k_\ell P_\ell^{*}$ ist $k_\ell - k_\ell\cdot 1 = 0$. Es gilt also

$$
P_\ell(z) - k_\ell P_\ell^{*}(z) \;=\; (1-\lvert k_\ell\rvert^2)\; z\; P_{\ell-1}(z),
$$

wobei $P_{\ell-1}(z) = z^{\ell-1}\hat\alpha_{\ell-1}(z)$ normiert vom Grad $\ell-1$ ist. Die Nullstelle bei $z=0$ liegt innerhalb des Kreises, folglich

$$
N(P_{\ell-1}) \;=\; N(P_\ell) - 1 .
$$

*Schritt 3 (Abgleich mit dem Grad).* „Alle Nullstellen innen" heißt $N = \deg$. Wegen $\deg P_{\ell-1} = \ell - 1 = \deg P_\ell - 1$ ist

$$
N(P_\ell) = \ell \iff N(P_{\ell-1}) = \ell - 1 ,
$$

also genau die Behauptung. $\square$

*Randfall.* Hat $P_\ell$ eine Nullstelle auf dem Einheitskreis, greift Rouché in dieser Form nicht; dieser Fall wird in §6.4.7 behandelt.

Induktiv von $\ell = p$ bis $\ell = 1$ heruntergestiegen und mit $\hat\alpha_0 = 1$ als trivialem Anfang ergibt das:

**Korollar 6.4 (Schur-Cohn).** Alle Nullstellen von $\hat\alpha$ liegen genau dann in der offenen Einheitskreisscheibe, wenn

$$
\lvert k_\ell\rvert \;<\; 1 \qquad\text{für alle } \ell = 1,\dots,p .
$$

Bricht die Rekursion vorher ab, weil ein $\lvert k_\ell\rvert = 1$ auftritt, greift §6.4.7.

*Zur Lesart.* Ein einzelnes $\lvert k_\ell\rvert < 1$ sagt für sich genommen nichts über die Stabilität — erst die Aussage für **alle** $\ell$ trägt. Ein Polynom mit einer Nullstelle außerhalb kann durchaus mit einem betragskleinen $k_p$ beginnen; die Verletzung zeigt sich dann erst in einem späteren Schritt.

#### 6.4.6 Die strukturelle Fassung: der Schur-Algorithmus

Der Beweis über Rouché zählt Nullstellen. Es gibt eine zweite Fassung, die statt zu zählen erklärt, warum die Eigenschaft überhaupt vererbt wird — und die zugleich die Rückrichtung liefert.

**Die tragende Größe.** Setze

$$
B_\ell(z) \;:=\; \frac{P_\ell(z)}{P_\ell^{*}(z)} .
$$

Nach (S2) ist $\lvert B_\ell\rvert = 1$ auf dem Einheitskreis. Liegen alle Nullstellen von $P_\ell$ innen, so liegen die von $P_\ell^{*}$ nach (S1) alle außen; $B_\ell$ ist dann in der Kreisscheibe polfrei, also holomorph, und nach dem **Maximumprinzip** gilt $\lvert B_\ell\rvert \le 1$ im Inneren. Umgekehrt erzeugt eine Nullstelle von $P_\ell$ außerhalb einen Pol von $B_\ell$ innerhalb. Also

$$
\hat\alpha_\ell \text{ stabil} \iff B_\ell \text{ bildet } \mathbb{D} \text{ in sich ab.} \tag{6.7}
$$

Insbesondere ist $B_\ell(0) = P_\ell(0)/P_\ell^{*}(0) = \alpha^{(\ell)}_\ell = k_\ell$: **Der Reflexionskoeffizient ist der Wert von $B_\ell$ im Ursprung.** Daraus folgt $\lvert k_\ell\rvert \le 1$ unmittelbar aus dem Maximumprinzip.

**Der Abstieg als Funktionentransformation.** In dieser Größe lautet (6.2)

$$
B_{\ell-1}(z) \;=\; \frac{1}{z}\cdot\frac{B_\ell(z) - k_\ell}{1 - \overline{k_\ell}\,B_\ell(z)} \;=\; \frac{1}{z}\,\varphi_{k_\ell}\!\left(B_\ell(z)\right),
\qquad \varphi_k(w) := \frac{w-k}{1-\bar k\,w}. \tag{6.8}
$$

Beide Faktoren erhalten Selbstabbildungen der Kreisscheibe:

- Für $\lvert k\rvert < 1$ ist $\varphi_k$ ein **Automorphismus** von $\mathbb{D}$, der $k$ auf $0$ abbildet. Wegen $B_\ell(0) = k_\ell$ verschwindet $\varphi_{k_\ell}(B_\ell)$ im Ursprung.
- Nach dem **Schwarzschen Lemma** gilt für holomorphes $f:\mathbb{D}\to\mathbb{D}$ mit $f(0)=0$ die Abschätzung $\lvert f(z)\rvert \le \lvert z\rvert$; also ist $f(z)/z$ wieder eine Selbstabbildung.

Die Nullstelle im Ursprung, die in Schritt 2 des Rouché-Beweises als „künstlich" erscheint, wird hier **gezielt erzeugt**, damit die Division durch $z$ zulässig ist.

**Der Aufstieg.** Löst man (6.8) nach $B_\ell$ auf, ergibt sich

$$
B_\ell(z) \;=\; \varphi_{k_\ell}^{-1}\!\left(z\,B_{\ell-1}(z)\right), \qquad \varphi_k^{-1}(u) = \frac{u+k}{1+\bar k\,u}. \tag{6.9}
$$

Ist $B_{\ell-1}$ eine Selbstabbildung und $\lvert k_\ell\rvert < 1$, so ist $\lvert z\,B_{\ell-1}(z)\rvert < 1$ für $\lvert z\rvert < 1$, und $\varphi_{k_\ell}^{-1}$ führt nicht aus $\mathbb{D}$ heraus. Also ist auch $B_\ell$ eine Selbstabbildung.

**Damit sind beide Richtungen strukturell.** Abwärts: Automorphismus plus Schwarzsches Lemma. Aufwärts: Multiplikation mit $z$ plus Umkehrautomorphismus. Die Bedingung $\lvert k_\ell\rvert < 1$ wird jeweils genau dort gebraucht, wo $\varphi$ ein Automorphismus sein muss.

Mit dem Induktionsanfang $\hat\alpha_0 = 1$ — Grad $0$, keine Nullstellen, die Bedingung gilt leer — folgt Korollar 6.4 auch in der Richtung „hinreichend".

**Folgerung für die Kappung.** Setzt man alle $k_\ell$ mit $\lvert k_\ell\rvert \ge 1$ auf einen Betrag kleiner als $1$ und rekonstruiert mit (6.4), so ist das Ergebnis **per Konstruktion** stabil, nicht näherungsweise: Jeder Aufstiegsschritt erhält die Eigenschaft, und der Anfang hat sie. Zugleich ist (6.4) die exakte Umkehrung von (6.3), das rekonstruierte Polynom gehört also wirklich zu den gewählten $k_\ell$.

*Zur Namensgebung.* Die Iteration (6.8) ist der **Schur-Algorithmus** der Funktionentheorie, und die $k_\ell$ heißen dort **Schur-Parameter**.

#### 6.4.7 Der Randfall $\lvert k_\ell\rvert = 1$

Rechnerisch verschwindet der Nenner in (6.2) und die Rekursion bricht ab. Die Bedeutung ist präzise fassbar.

$B_\ell$ ist eine Selbstabbildung der abgeschlossenen Kreisscheibe, die im **inneren** Punkt $z=0$ den Betrag $1$ annimmt. Nach dem Maximumprinzip ist $B_\ell$ dann konstant, also $P_\ell = c\,P_\ell^{*}$ mit $\lvert c\rvert = 1$. Solche Polynome heißen **selbstinversiv**; ihre Nullstellenmenge ist unter Spiegelung am Einheitskreis invariant.

Daraus folgt sofort: Es liegen **nicht** alle Nullstellen strikt innen, denn sonst lägen die gespiegelten alle außen und die Menge wäre nicht invariant. Die strikte Frage ist also mit „nein" beantwortet.

**Was der Test dabei nicht leistet:** Er unterscheidet nicht zwischen den beiden selbstinversiven Fällen.

| Nullstellen | $\lvert k_p\rvert$ | selbstinversiv | Fortsetzung |
|---|---|---|---|
| alle auf dem Einheitskreis | 1 | ja | beschränkt |
| Paar $z_0$ und $1/\bar z_0$ mit $\lvert z_0\rvert > 1$ | 1 | ja | divergiert |

Beide liefern denselben Wert. Für die Unterscheidung braucht es die klassische Sonderbehandlung des Jury-Verfahrens, das im Singulärfall auf die Ableitung des Polynoms übergeht.

*Praktische Bedeutung.* Ein stationärer Ton hat Nullstellen auf dem Einheitskreis, liegt also in unmittelbarer Nähe dieses Randfalls; in Gleitkomma wird $\lvert k_\ell\rvert = 1$ zu $1 \pm \varepsilon$ mit zufälligem Vorzeichen. Für einen reinen **Test** wäre das ein ernstes Problem. Für die **Kappung** ist es keines, da ohnehin auf $\lvert k_\ell\rvert \le 1-\delta$ gezogen wird und es nicht darauf ankommt, auf welcher Seite der Ausgangswert lag. Der Nenner $1 - \lvert k_\ell\rvert^2$ wird dabei allerdings sehr klein und braucht in einer Implementierung eine Untergrenze.

#### 6.4.8 Der Aufstieg

Die Rekursion ist umkehrbar. Aus $\hat\alpha_{\ell-1}$ und $k_\ell$ gewinnt man $\hat\alpha_\ell$ zurück über

$$
\alpha^{(\ell)}_i \;=\; \alpha^{(\ell-1)}_i + k_\ell\,\overline{\alpha^{(\ell-1)}_{\ell-i}}, \quad i = 1,\dots,\ell-1, \qquad \alpha^{(\ell)}_\ell = k_\ell . \tag{6.4}
$$

Das ist die **Levinson-Rekursion**. $(k_1,\dots,k_p)$ und $(\alpha_1,\dots,\alpha_p)$ sind damit zwei gleichwertige Parametrisierungen desselben Modells, wechselseitig in $O(p^2)$ ineinander überführbar.

#### 6.4.9 Beispiel $p = 2$

Mit $\alpha^{(2)} = (1,\alpha_1,\alpha_2)$ ist $k_2 = \alpha_2$, und (6.3) mit $i=1$ liefert

$$
\alpha^{(1)}_1 \;=\; \frac{\alpha_1 - \alpha_2\,\overline{\alpha_1}}{1 - \lvert\alpha_2\rvert^2}, \qquad k_1 = \alpha^{(1)}_1 .
$$

Für reelle Koeffizienten kürzt sich das zu $k_1 = \alpha_1/(1+\alpha_2)$, und das Kriterium lautet

$$
\lvert \alpha_2\rvert < 1 \quad\text{und}\quad \lvert \alpha_1\rvert < 1 + \alpha_2 .
$$

Das ist das bekannte **Stabilitätsdreieck** der Ordnung 2 — ohne jede Nullstellenberechnung aus zwei Ungleichungen ablesbar.

#### 6.4.10 Was die $k_\ell$ bedeuten

Die Reflexionskoeffizienten sind keine Hilfsgrößen, sondern haben zwei anschauliche Bedeutungen.

**Energiebilanz.** Ist $\sigma_\ell^2$ die mittlere Restleistung des Prädiktors der Ordnung $\ell$, so gilt

$$
\sigma_\ell^2 \;=\; \sigma_{\ell-1}^2\,(1 - \lvert k_\ell\rvert^2). \tag{6.5}
$$

Damit liest sich das Kriterium als Leistungsaussage: $\lvert k_\ell\rvert < 1$ heißt, dass die Restleistung sinkt und **positiv** bleibt. $\lvert k_\ell\rvert = 1$ heißt Restleistung null, das Signal wäre perfekt vorhersagbar — genau der Fall einer Nullstelle exakt auf dem Kreis. $\lvert k_\ell\rvert > 1$ ergäbe negative Leistung und ist für ein reales Signal unmöglich.

**Akustische Deutung.** Ein AR-Modell der Ordnung $p$ ist äquivalent zu einer Röhre aus $p$ aneinandergesetzten Abschnitten unterschiedlichen Querschnitts. An jedem Übergang wird ein Teil der Welle reflektiert, und $k_\ell$ ist genau dieser Reflexionsanteil — daher der Name. $\lvert k_\ell\rvert < 1$ heißt: Jeder Übergang gibt weniger zurück, als ankommt, die Röhre ist passiv. $\lvert k_\ell\rvert > 1$ hieße, ein Übergang erzeugt Energie.

Für eine Orgelpfeife ist das keine Analogie, sondern das Modell selbst: Der Bohrungsverlauf einer konischen Pfeife ist eine solche Abschnittsfolge.

**Bezug zu Burg.** Das Burg-Verfahren schätzt die $k_\ell$ direkt und konstruiert sie mit $\lvert k_\ell\rvert < 1$. Das ist der Ursprung seiner Stabilitätsgarantie — und zugleich seine Grenze: Es kann eine Nullstelle nie exakt auf den Kreis legen, einen stationären Ton also nie exakt treffen.

### 6.5 Wurzelfreie Stabilisierung

Aus dem Test lassen sich zwei Korrekturverfahren bauen, die Schritt 2 des Ablaufs ersetzen.

**Uniformes Schrumpfen.** Die Skalierung $\hat a_\kappa \mapsto \hat a_\kappa\,\rho^{\kappa}$ bildet nach (6.1) alle Nullstellen auf $\rho z_\lambda$ ab. Man testet mit Schur-Cohn und verkleinert $\rho$ per Bisektion, bis das Kriterium erfüllt ist. Wenige Tests genügen für drei Dezimalstellen. Preis: Auch gutartige Moden werden gedämpft.

**Kappung der Reflexionskoeffizienten.** Man geht in die $k$-Parametrisierung, erzwingt dort die Kastenbedingung und geht zurück:

```
1  alpha -> k    Abstieg (6.3), O(p^2), endlich
2  fuer jedes l:  falls |k[l]| >= 1:  k[l] <- k[l] * (1-delta)/|k[l]|
3  k -> alpha    Aufstieg (6.4), O(p^2), endlich
```

Beide Verfahren bestehen ausschließlich aus endlichen Operationsfolgen — derselben Kategorie wie die Gauß-Elimination, für die in Messungen kein Genauigkeitsproblem auftrat. Die Fixpunktiteration mit ihrem Risiko, auf eine benachbarte Nullstelle zu konvergieren, entfällt.

**Zulassen begrenzten Wachstums.** Die Kastenbedingung lässt sich auf einen beliebigen Radius $\rho > 0$ verschieben. Die Skalierung

$$
\alpha_\kappa \;\longmapsto\; \alpha_\kappa\,\rho^{-\kappa}
$$

bildet jede Nullstelle $z$ auf $z/\rho$ ab. Führt man den Test auf den skalierten Koeffizienten aus, entscheidet er über „alle Nullstellen innerhalb des Radius $\rho$"; kappt man dort und skaliert zurück, ist genau das garantiert. Verifiziert an je 400 Zufallspolynomen der Grade 2 bis 6: Der größte Betrag nach der Kappung war exakt $\rho$, für $\rho = 1{,}00$, $1{,}15$ und $1{,}40$.

Der Zweck wäre, Bins zuzulassen, in die ein Teilton hineinwandert und die daher echtes Wachstum zeigen (Glissandi, Einschwingvorgänge). Gemessen auf realem Material hat eine gelockerte Schranke bisher allerdings **geschadet**: An Toneinsätzen fiel die Füllgenauigkeit von $-4{,}3$ auf $-3{,}6$ dB und die Nahtenergie stieg. Der Grund ist vermutlich, dass ein globales $\rho$ das Wachstum überall freigibt, auch dort, wo es nur Rauschanpassung ist. Eine gezielte Freigabe müsste an den Nachbarbins erkennen, ob tatsächlich Energie wandert — das bräche allerdings die Bin-Unabhängigkeit des Verfahrens.

*Nicht gemessen.* Wie sich die $k$-Kappung gegenüber der selektiven Nullstellenkappung auf realem Material schlägt, ist offen. Sie ist nicht dasselbe Verfahren: Sie verändert alle Moden, die an einem verletzten $k_\ell$ hängen, nicht nur die instabilen.

## 7 Der Sonderfall $p = 1$

Für $p = 1$ und die zusätzliche Normierung $\lvert \hat a_1 \rvert = 1$ ist die Fortsetzung eine reine Drehung; der Betrag jedes Bins bleibt konstant. Das ist der klassische Phasenvocoder-Freeze.

Mit $m$ Beobachtungen und anschließender Normierung gilt

$$
\hat a_1 \;=\; \frac{S}{\lvert S\rvert}, \qquad S \;:=\; \sum_{j=0}^{m-1} X(-j)\,\overline{X(-j-1)} ,
$$

also ein betragsgewichtetes Zirkularmittel der Phasenzuwächse. Für $m = 1$ reduziert sich das auf die Normierung von $X(0)\,\overline{X(-1)}$.

Die übliche Phasenvocoder-Formel mit Bin-Mittenfrequenz und Hauptwertfaltung liefert dasselbe Ergebnis, sofern Synthese-Hop gleich Analyse-Hop ist. Mit $\omega_k := 2\pi k/N$, dem gemessenen Phasenzuwachs $\Delta\varphi$ und $d := \operatorname{princarg}(\Delta\varphi - \omega_k H)$ gilt nämlich

$$
e^{\mathrm{i}(\omega_k H + d)} \;=\; e^{\mathrm{i}\omega_k H}\, e^{\mathrm{i}(\Delta\varphi - \omega_k H)} \;=\; e^{\mathrm{i}\Delta\varphi},
$$

weil $\operatorname{princarg}$ nur ganzzahlige Vielfache von $2\pi$ abzieht. Bei abweichendem Synthese-Hop gilt die Identität **nicht**.

Ordnung $1$ kann keine Amplitudenänderung darstellen: Eine $1\times1$-Matrix ist stets diagonalisierbar, ein Jordanblock der Größe $\ge 2$ existiert nicht. Der in Ordnung-1-Systemen übliche globale Pegel-Trend ist der Ausgleich dafür — und ab $p \ge 2$ eine Doppelzählung, weil das Modell die Amplitudenänderung dann selbst trägt.

---

## 8 Numerische Hinweise

**Konditionsquadrierung.** Es gilt $\kappa_2(P) = \kappa_2(A)^2$. Die Normalgleichungen sind numerisch ungünstiger als eine QR-Zerlegung direkt auf $A$. Sie werden trotzdem verwendet, weil $P$ die Größe $p \times p$ unabhängig von $m$ hat und sich rekursiv fortschreiben lässt, etwa als $P \leftarrow \lambda P + u\,u^{\mathsf H}$.

**Regularisierung.** Ein Ridge-Term $P \mapsto P + \tau\,\mathrm{Id}$ mit $\tau = \rho\,\operatorname{tr}(P) / p$ zieht die Lösung Richtung null, also Richtung „nur eine Komponente" — das heißt in Richtung des degenerierten Falls, den man gerade auflösen will. Er darf $1/\kappa_2(P)$ nicht überschreiten, sonst dominiert er die Lösung vollständig.

Erschwerend kommt hinzu: Bei Autoregression enthalten auch die Regressoren Rauschen (errors-in-variables). Der Kleinste-Quadrate-Schätzer ist dann nicht erwartungstreu, sondern zieht die Nullstellen bereits nach innen; der Ridge verstärkt dieselbe Verzerrung.

**Genauigkeitsstufen.** Getrennt messbar sind drei Stufen: Speicherung von $P$ und $q$, Lösung von (3.2), und die Nullstellensuche. Die ersten beiden sind endliche Operationsfolgen mit beschränktem Fehler. Die Nullstellensuche ist ein Fixpunktverfahren, dessen Grenzwert bei zu grober Arithmetik auf eine benachbarte Nullstelle springen kann — das ist kein Rundungsfehler, sondern ein anderes Ergebnis. Die Abbruchschranke muss zur Arbeitsgenauigkeit passen.

---

## 9 Parameterwahl

Gemessen auf realem Orgelmaterial bei Lücke $256$ Samples, verketteter Selbstfütterung, $N = 4096$ und $f_s = 48\,000$:

| Parameter | Wert | Begründung |
|---|---|---|
| $N$ | 4096 | Kompromiss aus Frequenz- und Zeitauflösung |
| $H$ | 512, also $N/8$ | Aliasgrenze der Nullstellentrennung, §5.5 |
| $m$ | 14 bis 40 | Sättigung ab etwa 90 ms Beobachtungsspanne |
| $p$ | 4 bis 6 | wächst mit der Polyphonie, siehe unten |
| $\rho$ | 0 | jeder positive Wert kostet messbar |

Die optimale Ordnung steigt etwa linear mit der Zahl gleichzeitig klingender Teiltöne; gemessen wurde eine Ordnung je vier bis fünf zusätzliche Spektralgipfel unter 2 kHz. Bei dünnem Material ist eine zu hohe Ordnung schädlich, weil überzählige Nullstellen Rauschen anpassen.

---

## 10 Zusammenfassung der Rechenkette

| Schritt | Objekt | Dimension | Zweck |
|---|---|---|---|
| 1 | $(X_k(n))_n$ | $K+1$ Folgen | Datenquelle |
| 2 | $A$, $b$ | $m\times p$ und $m$ | Beobachtungen, Hankel, implizit |
| 3 | $P = A^{\mathsf H}A$, $q = A^{\mathsf H}b$ | $p\times p$ und $p$ | Normalgleichungen |
| 4 | $\hat a$ | $p$ | Lösung von (3.2), Fortsetzung bestimmt |
| 5 | $C$ und die Nullstellen von $\chi$ | $p\times p$ und $p$ | nur Stabilitätsprüfung |
| 6 | $X(1), X(2), \dots$ | | Iteration von (4.1) |
| 7 | inverse Transformation, Overlap-Add | | zurück in Samples |

Die Schritte 1 bis 4 sowie 6 und 7 genügen für die Fortsetzung. Schritt 5 ist Absicherung.
