/* Referenzspuren aus der Herleitung, Zeile fuer Zeile nachgerechnet. */
#define XO_HOST_C 1
#define XO_ORDER_MAX 20
#include "xorcist_kernels.cl"
#include <stdio.h>

static int fail;
static void chk(const char *what, double got, double want, double tol) {
  const int bad = fabs(got - want) > tol;

  if (bad)
    fail = 1;
  printf("  %-42s %12.6f  soll %12.6f  %s\n", what, got, want,
         bad ? "FEHLER" : "ok");
}

int main(void) {
  /* --- Aufstieg: k -> alpha ---------------------------------------- */
  {
    xo_root kr[3] = {0.3, -0.2, 0.5}, ki[3] = {0.1, 0.4, -0.15};
    xo_root ur[4], ui[4];

    ur[0] = 1;
    ui[0] = 0;
    for (int l = 1; l <= 3; l++) {
      xo_root nr[4], ni[4];

      for (int i = 1; i < l; i++) {
        nr[i] = ur[i] + (kr[l - 1] * ur[l - i] + ki[l - 1] * ui[l - i]);
        ni[i] = ui[i] + (ki[l - 1] * ur[l - i] - kr[l - 1] * ui[l - i]);
      }
      for (int i = 1; i < l; i++) {
        ur[i] = nr[i];
        ui[i] = ni[i];
      }
      ur[l] = kr[l - 1];
      ui[l] = ki[l - 1];
    }
    puts("Aufstieg k -> alpha, Endzustand nach k3:");
    chk("alpha_1 Re", ur[1], 0.120000, 1e-5);
    chk("alpha_1 Im", ui[1], 0.070000, 1e-5);
    chk("alpha_2 Re", ur[2], -0.096000, 1e-5);
    chk("alpha_2 Im", ui[2], 0.238000, 1e-5);
    chk("alpha_3 Re", ur[3], 0.500000, 1e-5);
    chk("alpha_3 Im", ui[3], -0.150000, 1e-5);
  }
  /* --- Abstieg: alpha -> k, mit |k1| > 1 ---------------------------- */
  {
    /* a = Rekursionskoeffizienten, alpha = -a */
    xo_root ar[3] = {1.4, -0.55, 0.18}, ai[3] = {0.2, -0.3, 0.11};
    xo_root ur[4], ui[4], kr[3], ki[3];

    ur[0] = 1;
    ui[0] = 0;
    for (int i = 1; i <= 3; i++) {
      ur[i] = -ar[i - 1];
      ui[i] = -ai[i - 1];
    }
    for (int l = 3; l >= 1; l--) {
      kr[l - 1] = ur[l];
      ki[l - 1] = ui[l];

      const xo_root km = kr[l - 1] * kr[l - 1] + ki[l - 1] * ki[l - 1];
      const xo_root den = (xo_root)1 - km;

      if (l == 3)
        chk("k3 Re", kr[2], -0.180000, 1e-5),
          chk("k3 Nenner", den, 0.955500, 1e-5);
      if (fabs(den) < 1e-6)
        break;
      /* Two passes, not one: step i reads alpha_{l-i}, which an in-place
       * write has already replaced for i > l/2.  The ascent got this
       * right by accident (it uses scratch arrays); the descent did not,
       * and it showed up as alpha(2)_2 = 0.302307 against the reference
       * trace's 0.288854 while alpha(2)_1 was exact. */
      xo_root nr[XO_ORDER_MAX + 1], ni[XO_ORDER_MAX + 1];

      for (int i = 1; i < l; i++) {
        nr[i] = (ur[i] - (kr[l - 1] * ur[l - i] + ki[l - 1] * ui[l - i])) / den;
        ni[i] = (ui[i] - (ki[l - 1] * ur[l - i] - kr[l - 1] * ui[l - i])) / den;
      }
      for (int i = 1; i < l; i++) {
        ur[i] = nr[i];
        ui[i] = ni[i];
      }
      if (l == 3)
        chk("alpha(2)_1 Re", ur[1], -1.327054, 1e-5),
          chk("alpha(2)_2 Re", ur[2], 0.288854, 1e-5);
      if (l == 2)
        chk("alpha(1)_1 Re", ur[1], -1.028256, 1e-5),
          chk("alpha(1)_1 Im", ui[1], -0.009356, 1e-5);
    }
    chk("|k1| (muss > 1 sein)", sqrt(kr[0] * kr[0] + ki[0] * ki[0]),
        1.028299, 1e-5);
  }
  puts(fail ? "\nTEST FEHLGESCHLAGEN" : "\nAlle Referenzspuren getroffen");
  return fail;
}
