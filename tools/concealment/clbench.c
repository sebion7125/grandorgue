/* Run the Xorcist solve kernel on a real OpenCL device and compare it,
 * value for value and microsecond for microsecond, with the CPU path.
 *
 * The point is not the absolute time on POCL (that IS the CPU) but two
 * things a device can prove and a C-mode compile cannot: that the kernel
 * compiles and runs under a real OpenCL compiler, and that its results
 * agree with the reference across every bin. */
#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void ref_solve(
  const double *pP, const double *pQ, unsigned order, unsigned N,
  float *pCoefRe, float *pCoefIm, float *pRootRe, float *pRootIm);
/* Die beiden Bibliotheks-Statiken, die SolveBin liest - make_ref macht
 * Globale daraus, damit hier BEIDE Pfade geprueft werden koennen. */
extern double ref_clamp_t;
extern double ref_det_rho;
bool ref_rho_test(
  const double *ar, const double *ai, unsigned order, double rho);

static double now_us(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e6 + ts.tv_nsec * 1e-3;
}

static char *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");

  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  *len = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);

  char *b = malloc(*len + 1);

  if (fread(b, 1, *len, f) != *len) {
    fclose(f);
    free(b);
    return NULL;
  }
  b[*len] = 0;
  fclose(f);
  return b;
}

int main(int argc, char **argv) {
  const int order = argc > 1 ? atoi(argv[1]) : 8;
  const int nbins = argc > 2 ? atoi(argv[2]) : 2049 * 6; /* 6 channels */
  const char *src_path = argc > 3 ? argv[3] : "src/xorcist/xorcist_kernels.cl";
  /* XO_FP64=1 in the environment switches the kernel to double, for a
   * card with real FP64 throughput or a bit-exactness check. */
  /* XO_FP32=1 in the environment switches the kernel to single - opt-in,
   * because the elimination is not a float32 problem at higher orders
   * (see the precision note in the kernel source). */
  /* Per-stage precision, so the sensitive stage can be found instead of
   * guessed: XO_FP32 sets all three, XO_FP32_STORE / _SOLVE / _ROOT
   * override individually. */
  const int all32 = getenv("XO_FP32") && atoi(getenv("XO_FP32"));
  const char *e;
  const int f_store = (e = getenv("XO_FP32_STORE")) ? atoi(e) : all32;
  const int f_solve = (e = getenv("XO_FP32_SOLVE")) ? atoi(e) : all32;
  const int f_root = (e = getenv("XO_FP32_ROOT")) ? atoi(e) : all32;
  /* XO_SCHUR=1 replaces Durand-Kerner by the root-free reflection-
   * coefficient clamp - no iteration, hence no fp32 fragility. */
  const int schur = (e = getenv("XO_SCHUR")) ? atoi(e) : 0;
  const size_t rsz = f_store ? sizeof(float) : sizeof(double);
  const int fp32 = f_store;

  cl_platform_id plat;
  cl_device_id dev;
  cl_int err;

  if (clGetPlatformIDs(1, &plat, NULL) != CL_SUCCESS) {
    printf("keine OpenCL-Plattform\n");
    return 1;
  }
  if (clGetDeviceIDs(plat, CL_DEVICE_TYPE_ALL, 1, &dev, NULL) != CL_SUCCESS) {
    printf("kein Device\n");
    return 1;
  }

  char dname[256] = {0};
  cl_uint cus = 0;

  clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(dname), dname, NULL);
  clGetDeviceInfo(dev, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cus), &cus, NULL);

  cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err);
  cl_command_queue q = clCreateCommandQueueWithProperties(ctx, dev, NULL, &err);
  size_t slen = 0;
  char *src = slurp(src_path, &slen);

  if (!src) {
    printf("Kernelquelle nicht lesbar: %s\n", src_path);
    return 1;
  }

  cl_program prog
    = clCreateProgramWithSource(ctx, 1, (const char **)&src, &slen, &err);

  char opts[128];

  snprintf(
    opts, sizeof(opts),
    "-cl-std=CL1.2 -DXO_ORDER_MAX=%d -DXO_FP32_STORE=%d -DXO_FP32_SOLVE=%d "
    "-DXO_FP32_ROOT=%d -DXO_SCHUR=%d",
    order, f_store, f_solve, f_root, schur);
  if (clBuildProgram(prog, 1, &dev, opts, NULL, NULL) != CL_SUCCESS) {
    size_t n = 0;

    clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &n);

    char *log = malloc(n + 1);

    clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, n, log, NULL);
    log[n] = 0;
    printf("Kernel-Uebersetzung fehlgeschlagen:\n%s\n", log);
    return 1;
  }

  cl_kernel k = clCreateKernel(prog, "xo_solve", &err);

  /* deterministic synthetic accumulators, same recipe as the C-mode test */
  double *P = calloc((size_t)nbins * order * order * 2, sizeof(double));
  double *Q = calloc((size_t)nbins * order * 2, sizeof(double));
  unsigned s = 12345;

  for (int bin = 0; bin < nbins; bin++) {
    double *Pb = P + (size_t)bin * order * order * 2;
    double *Qb = Q + (size_t)bin * order * 2;

    for (int i = 0; i < order; i++) {
      for (int j = 0; j < order; j++) {
        s = s * 1664525u + 1013904223u;

        const double u = ((s >> 8) % 2000) / 1000.0 - 1.0;

        s = s * 1664525u + 1013904223u;

        const double v = ((s >> 8) % 2000) / 1000.0 - 1.0;

        /* keep it Hermitian and diagonally dominant, like a real fit */
        Pb[2 * (i * order + j)] = (i == j) ? 8.0 + u : u;
        Pb[2 * (i * order + j) + 1] = (i == j) ? 0.0 : v;
      }
      s = s * 1664525u + 1013904223u;
      Qb[2 * i] = ((s >> 8) % 2000) / 1000.0 - 1.0;
      s = s * 1664525u + 1013904223u;
      Qb[2 * i + 1] = ((s >> 8) % 2000) / 1000.0 - 1.0;
    }
    for (int i = 0; i < order; i++)
      for (int j = i + 1; j < order; j++) {
        Pb[2 * (j * order + i)] = Pb[2 * (i * order + j)];
        Pb[2 * (j * order + i) + 1] = -Pb[2 * (i * order + j) + 1];
      }
  }

  const size_t pB = (size_t)nbins * order * order * 2 * rsz;
  const size_t qB = (size_t)nbins * order * 2 * rsz;
  void *Pd = malloc(pB), *Qd = malloc(qB);

  for (size_t i = 0; i < (size_t)nbins * order * order * 2; i++)
    if (!f_store)
      ((double *)Pd)[i] = P[i];
    else
      ((float *)Pd)[i] = (float)P[i];
  for (size_t i = 0; i < (size_t)nbins * order * 2; i++)
    if (!f_store)
      ((double *)Qd)[i] = Q[i];
    else
      ((float *)Qd)[i] = (float)Q[i];
  const size_t cB = (size_t)nbins * order * 2 * sizeof(float);
  cl_mem dP = clCreateBuffer(
    ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, pB, Pd, &err);
  cl_mem dQ = clCreateBuffer(
    ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, qB, Qd, &err);
  cl_mem dC = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, cB, NULL, &err);
  cl_mem dR = clCreateBuffer(ctx, CL_MEM_READ_WRITE, cB, NULL, &err);
  const int zero = 0;

  clSetKernelArg(k, 0, sizeof(dP), &dP);
  clSetKernelArg(k, 1, sizeof(dQ), &dQ);
  clSetKernelArg(k, 2, sizeof(dC), &dC);
  clSetKernelArg(k, 3, sizeof(dR), &dR);
  clSetKernelArg(k, 4, sizeof(int), &nbins);
  clSetKernelArg(k, 5, sizeof(int), &order);
  clSetKernelArg(k, 6, sizeof(int), &zero);
  clSetKernelArg(k, 7, sizeof(int), &nbins);

  const float clampT = 0.9f; /* Engine-Vorgabe */
  /* DETEKTOR-MODUS: XO_DET_RHO=1.02 prueft den Zweig, den der Kernel
   * seit dem 07.08. selbst kann - rho-Schur-Test auf den rohen
   * Gauss-Koeffizienten, bestanden = roh durchreichen, durchgefallen =
   * Durand-Kerner und dann auf rho DECKELN statt auf 1.0.  Referenz und
   * Kernel bekommen denselben Wert; die Abweichung muss 0 bleiben. */
  const char *detEnv = getenv("XO_DET_RHO");
  const float detRho = detEnv ? (float)atof(detEnv) : 0.0f;

  ref_clamp_t = clampT;
  ref_det_rho = detRho;
  clSetKernelArg(k, 8, sizeof(float), &clampT);
  clSetKernelArg(k, 9, sizeof(float), &detRho);

  float *zeros = calloc((size_t)nbins * order * 2, sizeof(float));

  clEnqueueWriteBuffer(q, dR, CL_TRUE, 0, cB, zeros, 0, NULL, NULL);

  size_t gws = (size_t)nbins;

  /* warm-up (kernel JIT, buffer residency) */
  clEnqueueNDRangeKernel(q, k, 1, NULL, &gws, NULL, 0, NULL, NULL);
  clFinish(q);

  const int reps = 20;
  const double t0 = now_us();

  for (int r = 0; r < reps; r++)
    clEnqueueNDRangeKernel(q, k, 1, NULL, &gws, NULL, 0, NULL, NULL);
  clFinish(q);

  const double gpu_us = (now_us() - t0) / reps;
  float *coefK = malloc(cB);
  float *rootK = malloc(cB);

  clEnqueueReadBuffer(q, dC, CL_TRUE, 0, cB, coefK, 0, NULL, NULL);
  clEnqueueReadBuffer(q, dR, CL_TRUE, 0, cB, rootK, 0, NULL, NULL);

  /* reference on the CPU, same accumulators */
  float *crf = malloc((size_t)order * sizeof(float));
  float *cif = malloc((size_t)order * sizeof(float));
  const double t1 = now_us();

  for (int bin = 0; bin < nbins; bin++)
    ref_solve(
      P + (size_t)bin * order * order * 2, Q + (size_t)bin * order * 2,
      (unsigned)order, (unsigned)order, crf, cif, NULL, NULL);

  const double cpu_us = now_us() - t1;
  /* For the Schur path the two metrics above are meaningless: it
   * computes no roots (that is the point), so the angle comparison reads
   * an unwritten buffer, and its coefficients are a DIFFERENT method's
   * answer rather than an approximation of the same one.  What it does
   * promise is an invariant, and that is checkable: every root of the
   * returned polynomial inside or on the unit circle. */
  double worstMod = 0.0;

  if (schur) {
    for (int bin = 0; bin < nbins; bin++) {
      double zr[64], zi[64], ar2[64], ai2[64];

      for (int i = 0; i < order; i++) {
        ar2[i] = coefK[(size_t)bin * order * 2 + 2 * i];
        ai2[i] = coefK[(size_t)bin * order * 2 + 2 * i + 1];
      }
      for (int i = 0; i < order; i++) {
        const double a = 2.0 * M_PI * i / order + 0.35;

        zr[i] = 0.9 * cos(a);
        zi[i] = 0.9 * sin(a);
      }
      for (int it = 0; it < 200; it++)
        for (int k = 0; k < order; k++) {
          double vr = 1.0, vi = 0.0, dr = 1.0, di = 0.0;

          for (int j = 0; j < order; j++) {
            const double tr = vr * zr[k] - vi * zi[k];
            const double ti = vr * zi[k] + vi * zr[k];

            vr = tr - ar2[j];
            vi = ti - ai2[j];
          }
          for (int j = 0; j < order; j++) {
            if (j == k)
              continue;

            const double er = zr[k] - zr[j], ei = zi[k] - zi[j];
            const double tr = dr * er - di * ei;

            di = dr * ei + di * er;
            dr = tr;
          }

          const double den = dr * dr + di * di;

          if (den < 1e-300)
            continue;
          zr[k] -= (vr * dr + vi * di) / den;
          zi[k] -= (vi * dr - vr * di) / den;
        }
      for (int i = 0; i < order; i++) {
        const double m = sqrt(zr[i] * zr[i] + zi[i] * zi[i]);

        if (m > worstMod)
          worstMod = m;
      }
    }
    printf(
      "%-46s CUs %2u  [store %s | solve %s | Schur-Cohn]\n"
      "  Ordnung %2d, %5d Bins: Device %8.1f us | CPU (1 Kern) %8.1f us | "
      "Faktor %5.2fx\n    Stabilitaet: groesster Wurzelbetrag %.6f "
      "(muss <= 1 sein)\n",
      dname, cus, f_store ? "32" : "64", f_solve ? "32" : "64", order,
      nbins, gpu_us, cpu_us, cpu_us / gpu_us, worstMod);
    return 0;
  }

  /* Im DETEKTOR-MODUS berechnen bestandene Bins ueberhaupt keine
   * Wurzeln - beide Seiten lassen den Puffer unberuehrt, ein
   * Winkelvergleich laese also zweimal uninitialisierten Speicher (er
   * lieferte 5.7e10 Grad, was kein Befund ist, sondern Muell).  Wie
   * beim Schur-Pfad zaehlt hier allein der Koeffizientenvergleich; die
   * Passierquote sagt, wie viele Bins den teuren Weg gespart haben. */
  int rhoPass = 0;

  if (detRho > 0.0f && order >= 2)
    for (int bin = 0; bin < nbins; bin++) {
      double ar2[64], ai2[64];
      double mm[64 * 64 * 2];
      (void)mm;

      /* Die rohen Koeffizienten stehen nach ref_solve nicht mehr bereit;
       * der Test laeuft deshalb auf der Referenzloesung selbst - das ist
       * dieselbe Groesse, weil bestandene Bins roh durchgereicht werden. */
      for (int i = 0; i < order; i++) {
        ar2[i] = coefK[(size_t)bin * order * 2 + 2 * i];
        ai2[i] = coefK[(size_t)bin * order * 2 + 2 * i + 1];
      }
      if (ref_rho_test(ar2, ai2, (unsigned)order, detRho))
        rhoPass++;
    }

  double worst = 0.0, worstAngDeg = 0.0, sumAng = 0.0;
  long nAng = 0;
  float *rrf = malloc((size_t)order * sizeof(float));
  float *rif = malloc((size_t)order * sizeof(float));

  for (int bin = 0; bin < nbins; bin++) {
    ref_solve(
      P + (size_t)bin * order * order * 2, Q + (size_t)bin * order * 2,
      (unsigned)order, (unsigned)order, crf, cif, rrf, rif);
    for (int i = 0; i < order; i++) {
      const double dr = crf[i] - coefK[(size_t)bin * order * 2 + 2 * i];
      const double di = cif[i] - coefK[(size_t)bin * order * 2 + 2 * i + 1];
      const double d = sqrt(dr * dr + di * di);

      if (d > worst)
        worst = d;
    }
    /* What actually survives to the audio is the ROOT SET, and of a root
     * only its angle: the angle is the component's frequency, the
     * modulus is normalised by the clamp anyway.  Roots come out in no
     * particular order, so match each reference root to its nearest
     * kernel root by angle before measuring. */
    for (int i = 0; i < order; i++) {
      const double refA = atan2(rif[i], rrf[i]);
      const double refM = sqrt((double)rrf[i] * rrf[i] + (double)rif[i] * rif[i]);
      double best = 1e9;

      if (refM < 0.5)
        continue; /* decaying shadow root: dies over the horizon anyway */
      for (int j = 0; j < order; j++) {
        const double kr = rootK[(size_t)bin * order * 2 + 2 * j];
        const double ki = rootK[(size_t)bin * order * 2 + 2 * j + 1];
        double da = fabs(atan2(ki, kr) - refA);

        if (da > M_PI)
          da = 2.0 * M_PI - da;
        if (da < best)
          best = da;
      }
      best *= 180.0 / M_PI;
      if (best > worstAngDeg)
        worstAngDeg = best;
      sumAng += best;
      nAng++;
    }
  }
  char modeBuf[64];

  if (detRho > 0.0f)
    snprintf(modeBuf, sizeof(modeBuf), "Detektor rho %.2f", detRho);
  else
    snprintf(modeBuf, sizeof(modeBuf), "%s",
             schur ? "Schur-Cohn" : "Durand-Kerner");
  printf(
    "%-46s CUs %2u  [store %s | solve %s | root %s | %s]\n"
    "  Ordnung %2d, %5d Bins: Device %8.1f us | CPU (1 Kern) %8.1f us | "
    "Faktor %5.2fx\n",
    dname, cus, f_store ? "32" : "64", f_solve ? "32" : "64",
    f_root ? "32" : "64", modeBuf, order,
    nbins, gpu_us, cpu_us, cpu_us / gpu_us);
  if (detRho > 0.0f)
    printf(
      "    Koeffizient: max %.2e     roh durchgereicht: %d von %d "
      "(%.1f %%), Wurzeln daher nicht vergleichbar\n",
      worst, rhoPass, nbins, 100.0 * rhoPass / nbins);
  else
    printf(
      "    Koeffizient: max %.2e     Wurzelwinkel: max %.4f grad, "
      "mittel %.5f grad\n",
      worst, worstAngDeg, nAng ? sumAng / nAng : 0.0);

  /* ---- xo_rls_update und xo_advance -----------------------------------
   * Der Bit-Beweis existierte bisher nur fuer xo_solve.  Referenz ist
   * dieselbe Formel in double bzw. float, in identischer Operations-
   * reihenfolge wie Kernel UND Engine (Konvention q += X0*conj(Xk),
   * P += Xk*conj(Xl) - gegen xorcist.cpp gelesen, siehe RLS-Block und
   * AdvanceBins). */
  {
    cl_kernel kR = clCreateKernel(prog, "xo_rls_update", &err);
    cl_kernel kA = clCreateKernel(prog, "xo_advance", &err);
    const int J = 24; /* Fit-Rahmen, wie der Prototyp */
    const double lam = 1.0 - 1.0 / (4.0 * order + 6.0);
    const int T = J + order + 1;
    float *hist = malloc((size_t)T * nbins * 2 * sizeof(float));
    unsigned s2 = 777u;

    for (size_t i = 0; i < (size_t)T * nbins * 2; i++) {
      s2 = s2 * 1664525u + 1013904223u;
      hist[i] = (float)(((s2 >> 8) % 2000) / 1000.0 - 1.0);
    }

    const size_t specN = (size_t)(order + 1) * nbins * 2;
    float *spec = malloc(specN * sizeof(float));
    double *Pr = calloc((size_t)nbins * order * order * 2, sizeof(double));
    double *Qr = calloc((size_t)nbins * order * 2, sizeof(double));
    void *Pz = calloc((size_t)nbins * order * order * 2, rsz);
    void *Qz = calloc((size_t)nbins * order * 2, rsz);
    cl_mem dPa = clCreateBuffer(
      ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, pB, Pz, &err);
    cl_mem dQa = clCreateBuffer(
      ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, qB, Qz, &err);
    cl_mem dS = clCreateBuffer(
      ctx, CL_MEM_READ_ONLY, specN * sizeof(float), NULL, &err);

    clSetKernelArg(kR, 0, sizeof(dPa), &dPa);
    clSetKernelArg(kR, 1, sizeof(dQa), &dQa);
    clSetKernelArg(kR, 2, sizeof(dS), &dS);
    clSetKernelArg(kR, 3, sizeof(int), &nbins);
    clSetKernelArg(kR, 4, sizeof(int), &order);
    if (f_solve) {
      const float lf = (float)lam;

      clSetKernelArg(kR, 5, sizeof(float), &lf);
    } else
      clSetKernelArg(kR, 5, sizeof(double), &lam);

    double t_rls = 0.0;

    for (int j = 0; j < J; j++) {
      for (int k2 = 0; k2 <= order; k2++)
        memcpy(
          spec + (size_t)k2 * nbins * 2,
          hist + (size_t)(j + order - k2) * nbins * 2,
          (size_t)nbins * 2 * sizeof(float));
      clEnqueueWriteBuffer(
        q, dS, CL_TRUE, 0, specN * sizeof(float), spec, 0, NULL, NULL);

      const double tt = now_us();

      clEnqueueNDRangeKernel(q, kR, 1, NULL, &gws, NULL, 0, NULL, NULL);
      clFinish(q);
      t_rls += now_us() - tt;
      for (int bin = 0; bin < nbins; bin++) {
        double *Pb = Pr + (size_t)bin * order * order * 2;
        double *Qb = Qr + (size_t)bin * order * 2;
        const double x0r = spec[(size_t)(0 * nbins + bin) * 2];
        const double x0i = spec[(size_t)(0 * nbins + bin) * 2 + 1];

        for (int k2 = 0; k2 < order; k2++) {
          const double xkr = spec[(size_t)((k2 + 1) * nbins + bin) * 2];
          const double xki = spec[(size_t)((k2 + 1) * nbins + bin) * 2 + 1];

          Qb[2 * k2] = lam * Qb[2 * k2] + x0r * xkr + x0i * xki;
          Qb[2 * k2 + 1] = lam * Qb[2 * k2 + 1] + x0i * xkr - x0r * xki;
          for (int l = 0; l < order; l++) {
            const double xlr = spec[(size_t)((l + 1) * nbins + bin) * 2];
            const double xli = spec[(size_t)((l + 1) * nbins + bin) * 2 + 1];
            const int o = 2 * (k2 * order + l);

            Pb[o] = lam * Pb[o] + xkr * xlr + xki * xli;
            Pb[o + 1] = lam * Pb[o + 1] + xki * xlr - xkr * xli;
          }
        }
      }
    }

    void *Pk = malloc(pB), *Qk = malloc(qB);

    clEnqueueReadBuffer(q, dPa, CL_TRUE, 0, pB, Pk, 0, NULL, NULL);
    clEnqueueReadBuffer(q, dQa, CL_TRUE, 0, qB, Qk, 0, NULL, NULL);

    double scale = 0.0, dmax = 0.0;

    for (size_t i = 0; i < (size_t)nbins * order * order * 2; i++) {
      const double rv = Pr[i];
      const double kv = f_store ? (double)((float *)Pk)[i] : ((double *)Pk)[i];

      if (fabs(rv) > scale)
        scale = fabs(rv);
      if (fabs(rv - kv) > dmax)
        dmax = fabs(rv - kv);
    }
    for (size_t i = 0; i < (size_t)nbins * order * 2; i++) {
      const double rv = Qr[i];
      const double kv = f_store ? (double)((float *)Qk)[i] : ((double *)Qk)[i];

      if (fabs(rv) > scale)
        scale = fabs(rv);
      if (fabs(rv - kv) > dmax)
        dmax = fabs(rv - kv);
    }
    printf(
      "    xo_rls_update: %2d Schritte a %7.1f us, max |Delta| %.2e "
      "(rel %.2e)\n",
      J, t_rls / J, dmax, dmax / (scale > 0.0 ? scale : 1.0));

    /* ---- xo_advance ---------------------------------------------------- */
    const size_t stN = (size_t)order * nbins * 2;
    float *st0 = malloc(stN * sizeof(float));
    float *coefA = malloc(cB);

    for (size_t i = 0; i < stN; i++) {
      s2 = s2 * 1664525u + 1013904223u;
      st0[i] = (float)(((s2 >> 8) % 2000) / 1000.0 - 1.0);
    }
    for (size_t i = 0; i < (size_t)nbins * order * 2; i++) {
      s2 = s2 * 1664525u + 1013904223u;
      /* moderat skaliert - Stabilitaet ist fuer 26 Schritte egal */
      coefA[i] = (float)((((s2 >> 8) % 2000) / 1000.0 - 1.0) * 0.6 / order);
    }

    cl_mem dCf = clCreateBuffer(
      ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, cB, coefA, &err);
    cl_mem dSt = clCreateBuffer(
      ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, stN * sizeof(float), st0,
      &err);

    clSetKernelArg(kA, 0, sizeof(dCf), &dCf);
    clSetKernelArg(kA, 1, sizeof(dSt), &dSt);
    clSetKernelArg(kA, 2, sizeof(int), &nbins);
    clSetKernelArg(kA, 3, sizeof(int), &order);

    const int NS = 26; /* Schritte bis zum Horizontende, wie die Engine */
    const double ta = now_us();

    for (int st = 0; st < NS; st++)
      clEnqueueNDRangeKernel(q, kA, 1, NULL, &gws, NULL, 0, NULL, NULL);
    clFinish(q);

    const double t_adv = (now_us() - ta) / NS;
    /* Referenz in float, identische Reihenfolge wie Kernel und Engine */
    float *sr_ = malloc(stN * sizeof(float));

    memcpy(sr_, st0, stN * sizeof(float));
    for (int st = 0; st < NS; st++)
      for (int bin = 0; bin < nbins; bin++) {
        float re = 0.0f, im = 0.0f;

        for (int k2 = 0; k2 < order; k2++) {
          const float aR = coefA[(size_t)bin * order * 2 + 2 * k2];
          const float aI = coefA[(size_t)bin * order * 2 + 2 * k2 + 1];
          const float sR = sr_[((size_t)k2 * nbins + bin) * 2];
          const float sI = sr_[((size_t)k2 * nbins + bin) * 2 + 1];

          re += aR * sR - aI * sI;
          im += aR * sI + aI * sR;
        }
        for (int k2 = order - 1; k2 > 0; k2--) {
          sr_[((size_t)k2 * nbins + bin) * 2]
            = sr_[((size_t)(k2 - 1) * nbins + bin) * 2];
          sr_[((size_t)k2 * nbins + bin) * 2 + 1]
            = sr_[((size_t)(k2 - 1) * nbins + bin) * 2 + 1];
        }
        sr_[(size_t)bin * 2] = re;
        sr_[(size_t)bin * 2 + 1] = im;
      }

    float *stK = malloc(stN * sizeof(float));

    clEnqueueReadBuffer(
      q, dSt, CL_TRUE, 0, stN * sizeof(float), stK, 0, NULL, NULL);

    double amax = 0.0;

    for (size_t i = 0; i < stN; i++)
      if (fabs((double)stK[i] - (double)sr_[i]) > amax)
        amax = fabs((double)stK[i] - (double)sr_[i]);
    printf(
      "    xo_advance:    %2d Schritte a %7.1f us, max |Delta| %.2e\n", NS,
      t_adv, amax);
  }
  return 0;
}
