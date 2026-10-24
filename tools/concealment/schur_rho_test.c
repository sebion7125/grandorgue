/* Winkeltreue der Schur-Kappung als Funktion von rho.
 *
 * Der Verdacht aus der Vorlesung (06.08.2026): ein stationaerer Ton hat
 * Wurzeln exakt auf dem Einheitskreis, das Polynom ist damit
 * selbstinversiv, und daraus folgt |k_p| = 1 EXAKT.  Bei rho = 1.0
 * greift also schon der erste Abstiegsschritt in die Singularitaet -
 * nicht als Randfall, sondern im Normalbetrieb.
 *
 * Der Test rechnet gegen Wurzeln, deren Winkel wir vorher wissen, und
 * stellt zwei Varianten des Abstiegs gegeneinander:
 *
 *   IST    kappt k NACH dem Abstieg; der Abstieg selbst dividiert durch
 *          den = 1 - |k|^2 und faengt das nur ueber einen Floor 1e-6 ab.
 *          Bei |k| = 1 wird damit mit 1e6 multipliziert.
 *   NEU    kappt k VOR dem Abstieg.  Dann ist den >= 2*delta - delta^2,
 *          bei delta = 1e-3 also 2e-3 statt 1e-6, und der Sonderfall
 *          entfaellt ersatzlos.
 *
 * Bauen:  gcc -O2 -o /tmp/schur_rho tools/concealment/schur_rho_test.c -lm
 */
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PMAX 24

/* ---- Abstieg/Aufstieg, wortgleich zu xorcist.cpp ------------------- */
static void descend(const double *ar, const double *ai, unsigned order,
                    double *kr, double *ki, int clamp_first, double delta) {
  double ur[PMAX + 1], ui[PMAX + 1], nr[PMAX + 1], ni[PMAX + 1];

  ur[0] = 1.0;
  ui[0] = 0.0;
  for (unsigned i = 1; i <= order; i++) {
    ur[i] = -ar[i - 1];
    ui[i] = -ai[i - 1];
  }
  for (unsigned l = order; l >= 1; l--) {
    kr[l - 1] = ur[l];
    ki[l - 1] = ui[l];

    /* NEU: erst kappen, dann rechnen - der Nenner kann nicht mehr
     * kollabieren, weil |k| <= 1 - delta garantiert ist. */
    if (clamp_first) {
      const double m2 = kr[l - 1] * kr[l - 1] + ki[l - 1] * ki[l - 1];

      if (m2 >= (1.0 - delta) * (1.0 - delta)) {
        const double sc = (1.0 - delta) / sqrt(m2);

        kr[l - 1] *= sc;
        ki[l - 1] *= sc;
      }
    }

    double den = 1.0 - (kr[l - 1] * kr[l - 1] + ki[l - 1] * ki[l - 1]);

    if (!clamp_first && fabs(den) < 1e-6)
      den = den < 0.0 ? -1e-6 : 1e-6;
    for (unsigned i = 1; i < l; i++) {
      nr[i] = (ur[i] - (kr[l - 1] * ur[l - i] + ki[l - 1] * ui[l - i])) / den;
      ni[i] = (ui[i] - (ki[l - 1] * ur[l - i] - kr[l - 1] * ui[l - i])) / den;
    }
    for (unsigned i = 1; i < l; i++) {
      ur[i] = nr[i];
      ui[i] = ni[i];
    }
  }
}

static void ascend(const double *kr, const double *ki, unsigned order,
                   double *ar, double *ai) {
  double ur[PMAX + 1], ui[PMAX + 1], nr[PMAX + 1], ni[PMAX + 1];

  ur[0] = 1.0;
  ui[0] = 0.0;
  for (unsigned l = 1; l <= order; l++) {
    for (unsigned i = 1; i < l; i++) {
      nr[i] = ur[i] + (kr[l - 1] * ur[l - i] + ki[l - 1] * ui[l - i]);
      ni[i] = ui[i] + (ki[l - 1] * ur[l - i] - kr[l - 1] * ui[l - i]);
    }
    for (unsigned i = 1; i < l; i++) {
      ur[i] = nr[i];
      ui[i] = ni[i];
    }
    ur[l] = kr[l - 1];
    ui[l] = ki[l - 1];
  }
  for (unsigned i = 1; i <= order; i++) {
    ar[i - 1] = -ur[i];
    ai[i - 1] = -ui[i];
  }
}

static void schur_clamp(double *ar, double *ai, unsigned order, double delta,
                        double rho, int clamp_first) {
  double kr[PMAX], ki[PMAX], f = 1.0;
  unsigned i;

  if (rho != 1.0)
    for (i = 0; i < order; i++) {
      f /= rho;
      ar[i] *= f;
      ai[i] *= f;
    }
  descend(ar, ai, order, kr, ki, clamp_first, delta);
  if (!clamp_first)
    for (unsigned l = 0; l < order; l++) {
      const double m2 = kr[l] * kr[l] + ki[l] * ki[l];

      if (m2 >= 1.0) {
        const double sc = (1.0 - delta) / sqrt(m2);

        kr[l] *= sc;
        ki[l] *= sc;
      }
    }
  ascend(kr, ki, order, ar, ai);
  if (rho != 1.0)
    for (f = 1.0, i = 0; i < order; i++) {
      f *= rho;
      ar[i] *= f;
      ai[i] *= f;
    }
}

/* ---- Wurzeln <-> Koeffizienten ------------------------------------- */
static void roots_to_coef(const double complex *z, unsigned p, double *ar,
                          double *ai) {
  double complex c[PMAX + 1];

  memset(c, 0, sizeof c);
  c[0] = 1.0;                       /* Produkt (x - z_j) */
  for (unsigned j = 0; j < p; j++)
    for (unsigned i = j + 1; i >= 1; i--)
      c[i] -= z[j] * c[i - 1];
  for (unsigned i = 1; i <= p; i++) {  /* x^p - a1 x^(p-1) - ... */
    ar[i - 1] = -creal(c[i]);
    ai[i - 1] = -cimag(c[i]);
  }
}

static void coef_to_roots(const double *ar, const double *ai, unsigned p,
                          double complex *z) {
  double complex c[PMAX + 1];

  c[0] = 1.0;
  for (unsigned i = 1; i <= p; i++)
    c[i] = -(ar[i - 1] + I * ai[i - 1]);
  for (unsigned j = 0; j < p; j++)          /* Durand-Kerner */
    z[j] = cpow(0.4 + 0.9 * I, (double)j + 1.0);
  for (int it = 0; it < 4000; it++) {
    double mv = 0.0;

    for (unsigned j = 0; j < p; j++) {
      double complex num = c[0], den = 1.0;

      for (unsigned i = 1; i <= p; i++)
        num = num * z[j] + c[i];
      for (unsigned m = 0; m < p; m++)
        if (m != j)
          den *= z[j] - z[m];
      double complex d = num / den;

      z[j] -= d;
      if (cabs(d) > mv)
        mv = cabs(d);
    }
    if (mv < 1e-14)
      break;
  }
}

static int by_angle(const void *a, const void *b) {
  const double x = carg(*(const double complex *)a);
  const double y = carg(*(const double complex *)b);

  return x < y ? -1 : x > y ? 1 : 0;
}

/* Groesster Winkelfehler in Grad, Wurzeln nach Winkel gepaart. */
static double angle_error(unsigned p, double rho, int clamp_first,
                          double *max_radius) {
  double complex z0[PMAX], z1[PMAX];
  double ar[PMAX], ai[PMAX], worst = 0.0;

  for (unsigned j = 0; j < p; j++)     /* exakt auf dem Einheitskreis */
    z0[j] = cexp(I * (0.35 + 2.1 * (double)j / (double)p));
  roots_to_coef(z0, p, ar, ai);
  schur_clamp(ar, ai, p, 1e-3, rho, clamp_first);
  coef_to_roots(ar, ai, p, z1);
  qsort(z0, p, sizeof z0[0], by_angle);
  qsort(z1, p, sizeof z1[0], by_angle);
  *max_radius = 0.0;
  for (unsigned j = 0; j < p; j++) {
    double d = fabs(carg(z1[j]) - carg(z0[j])) * 180.0 / M_PI;

    if (d > 180.0)
      d = 360.0 - d;
    if (d > worst)
      worst = d;
    if (cabs(z1[j]) > *max_radius)
      *max_radius = cabs(z1[j]);
  }
  return worst;
}

int main(void) {
  const double rhos[] = {1.0, 1.001, 1.002, 1.005, 1.02, 1.05};
  const unsigned orders[] = {4, 8, 12};

  for (int variant = 0; variant < 2; variant++) {
    printf("\n=== %s ===\n",
           variant ? "NEU: k VOR dem Abstieg gekappt"
                   : "IST: k nach dem Abstieg, Nenner-Floor 1e-6");
    printf("%-8s", "Ordnung");
    for (unsigned r = 0; r < sizeof rhos / sizeof rhos[0]; r++)
      printf(" rho=%-9.3f", rhos[r]);
    printf("\n");
    for (unsigned o = 0; o < sizeof orders / sizeof orders[0]; o++) {
      printf("%-8u", orders[o]);
      for (unsigned r = 0; r < sizeof rhos / sizeof rhos[0]; r++) {
        double mr, e = angle_error(orders[o], rhos[r], variant, &mr);

        printf(" %9.3f deg", e);
      }
      printf("\n");
    }
  }
  return 0;
}
