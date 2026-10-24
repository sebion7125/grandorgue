/* Verfolgt Durand-Kerner ueber eine ZEITENTWICKLUNG statt auf statischen
 * Akkumulatoren - die Frage des Nutzers vom 06.08.2026: findet ein WARM
 * gestarteter Durand-Kerner in fp32 dieselben Loesungen?
 *
 * Die gemessenen 174.6 grad Winkelfehler von fp32 stammen aus KALTEM
 * Start auf statischen Daten: vom Kreis-Seed aus muss die Iteration die
 * Wurzeln erst sortieren, und beim Sortieren kollabieren in float die
 * Schritte zwischen nahen Wurzeln.  Im Betrieb startet sie aber am
 * konvergierten Ergebnis der Vorperiode, waehrend die Akkumulatoren nur
 * langsam wandern - der Start sitzt also schon im richtigen
 * Einzugsbereich.  Hier wird genau das nachgestellt: pro Bin p/2
 * driftende Komponenten (ein Viertel der Bins als ENGES Paar, der
 * Gefahrenfall der fast-entarteten Doppelwurzel), exponentiell
 * vergessene Normalgleichungen wie in der Engine, ein Solve pro Schritt.
 *
 * Ein Binary = eine Genauigkeit (die Kernel-Typen sind Compile-Zeit):
 *     gcc -O2 -o /tmp/track64 track_bench.c -lm
 *     gcc -O2 -DXO_FP32=1 -o /tmp/track32 track_bench.c -lm
 * Jedes schreibt seine Wurzel-Trajektorie als Binaerdatei; der Vergleich
 * ist run_track.sh.  Aufruf: track <order> <nbins> <steps> <warm01> <out>
 */
#define XO_HOST_C 1
#ifndef XO_ORDER_MAX
#define XO_ORDER_MAX 16
#endif
#include "../../src/xorcist/xorcist_kernels.cl"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  const int order = argc > 1 ? atoi(argv[1]) : 12;
  const int nbins = argc > 2 ? atoi(argv[2]) : 512;
  const int steps = argc > 3 ? atoi(argv[3]) : 200;
  const int warm = argc > 4 ? atoi(argv[4]) : 1;
  const char *out = argc > 5 ? argv[5] : "/tmp/track.bin";
  const int nc = order / 2 > 1 ? order / 2 : 1; /* Komponenten pro Bin */
  const double lam = 1.0 - 1.0 / (4.0 * order + 6.0);

  /* Komponenten: Winkel, Drift, Amplitude - deterministisch je Bin.
   * Jeder vierte Bin bekommt ein ENGES Paar (Abstand 2e-3 rad ~ 15 Hz
   * bei hop 512): der Doppelwurzel-Gefahrenfall der Transienten. */
  double *ang = malloc((size_t)nbins * nc * sizeof(double));
  double *drf = malloc((size_t)nbins * nc * sizeof(double));
  double *amp = malloc((size_t)nbins * nc * sizeof(double));
  unsigned s = 424242u;

  for (int b = 0; b < nbins; b++)
    for (int c = 0; c < nc; c++) {
      s = s * 1664525u + 1013904223u;
      ang[b * nc + c] = 2.0 * M_PI * ((s >> 8) % 10000) / 10000.0;
      s = s * 1664525u + 1013904223u;
      drf[b * nc + c] = (((s >> 8) % 2000) / 1000.0 - 1.0) * 2e-4;
      s = s * 1664525u + 1013904223u;
      amp[b * nc + c] = 0.3 + 0.7 * ((s >> 8) % 1000) / 1000.0;
      if (b % 4 == 0 && c == 1) {
        ang[b * nc + 1] = ang[b * nc] + 2e-3; /* enges Paar */
        amp[b * nc + 1] = amp[b * nc] * 0.8;
      }
    }

  /* Signalhistorie X(t) je Bin, daraus RLS-Akkumulatoren im
   * STORE-Typ des Kernels - exakt die Engine-Formel. */
  const int T = steps + order + 1;
  float *hist = malloc((size_t)T * nbins * 2 * sizeof(float));

  for (int t = 0; t < T; t++)
    for (int b = 0; b < nbins; b++) {
      double re = 0.0, im = 0.0;

      for (int c = 0; c < nc; c++) {
        const double a = ang[b * nc + c] + drf[b * nc + c] * t;

        re += amp[b * nc + c] * cos(a * (t + 1));
        im += amp[b * nc + c] * sin(a * (t + 1));
      }
      hist[((size_t)t * nbins + b) * 2] = (float)re;
      hist[((size_t)t * nbins + b) * 2 + 1] = (float)im;
    }

  xo_store *P = calloc((size_t)nbins * order * order * 2, sizeof(xo_store));
  xo_store *Q = calloc((size_t)nbins * order * 2, sizeof(xo_store));
  float *spec = malloc((size_t)(order + 1) * nbins * 2 * sizeof(float));
  float *coef = calloc((size_t)nbins * order * 2, sizeof(float));
  float *roots = calloc((size_t)nbins * order * 2, sizeof(float));
  FILE *f = fopen(out, "wb");

  if (!f)
    return 1;
  for (int st = 0; st < steps; st++) {
    for (int k = 0; k <= order; k++)
      memcpy(
        spec + (size_t)k * nbins * 2,
        hist + (size_t)(st + order - k) * nbins * 2,
        (size_t)nbins * 2 * sizeof(float));
    for (xo_gid = 0; xo_gid < nbins; xo_gid++)
      xo_rls_update(P, Q, spec, nbins, order, (xo_real)lam);
    if (st >= order) { /* Rang aufbauen lassen, wie die Engine */
      if (!warm)
        memset(roots, 0, (size_t)nbins * order * 2 * sizeof(float));
      for (xo_gid = 0; xo_gid < nbins; xo_gid++)
        xo_solve(P, Q, coef, roots, nbins, order, 0, nbins, 0.9f);
      fwrite(roots, sizeof(float), (size_t)nbins * order * 2, f);
    }
  }
  fclose(f);
  printf(
    "%s: order %d, %d bins (%d mit engem Paar), %d Schritte, %s\n", out,
    order, nbins, nbins / 4, steps, warm ? "WARM" : "KALT");
  return 0;
}
