// v3_perblock_fit_study.cpp
//
// Question (user, 2026-07-30): the coefficient thread measured GLOBAL fits
// (LS / minimax-IRLS / pattern search -> integers win) and PER-FILE fits
// (LS3 / LPC-4..12 -> -7..-9% at LPC decode cost), but never PER-BLOCK
// *fitted float coefficients*. Per-block fitting is the one fitting variant
// that can attack the real objective (the per-1024-block PEAK residual that
// sets the V3 byte width) directly, so it could beat the per-file numbers.
// Measure it, in the real V3 block-width metric.
//
// Modes (all order-3 taps v[n-1..n-3] unless stated, history continuous
// across block boundaries exactly like the streaming decoder):
//   fixed2         (2,-1)            shipping-class reference
//   fixed3         (3,-3,1)          current default = BASELINE
//   file-LS3       per-file least-squares, FLAC-quantized (15 bit)
//   blk-order      per-block best FIXED order 1..4 (selection; the 2-bit
//                  selector fits in the width byte's spare bits -> free)
//   blk-LS3        per-block least-squares, quantized
//   blk-PEAK3      per-block peak-minimizing fit: LS init, then IRLS with
//                  p ramping 2->16 (weights |r|^(p-2)), keeping the
//                  iteration with the smallest TRUE block byte width
//   blk-BEST       encoder tries {fixed 1..4, blk-LS3, blk-PEAK3} per
//                  block, keeps the smallest width (realistic per-block
//                  encoder; the fitted-coeff overhead is only paid on
//                  blocks where a fitted set actually wins)
// Fitted-per-block modes are reported raw AND with +7 bytes/block coeff
// overhead (3x int16 coeffs + shift byte; selector rides in spare bits).
//
// Build:
//   g++ -O2 -std=gnu++17 -I ../../src/grandorgue  \
//     -I ../../src/v3codec v3_perblock_fit_study.cpp \
//       -lwavpack -o /tmp/claude-1000/perblock
// Run:
//   /tmp/claude-1000/perblock <filelist>   (plain PCM wav or WavPack .wav)

#include <wavpack/wavpack.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GOSoundCompressionCacheV3.h"

static const unsigned BLK = 1024;
static const double COEF_OVERHEAD = 7.0; // 3x int16 + shift byte per fitted block

struct Wav {
  unsigned channels = 0, bits = 0;
  std::vector<std::vector<int32_t>> ch; // native integer scale (16 or 24 bit)
};

// ---------- loaders ----------
static uint32_t rd32(const unsigned char* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }

static bool loadPcmWav(const char* path, Wav& w) {
  FILE* f = fopen(path, "rb");
  if (!f)
    return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 44) {
    fclose(f);
    return false;
  }
  std::vector<unsigned char> b(sz);
  if (fread(b.data(), 1, sz, f) != (size_t)sz) {
    fclose(f);
    return false;
  }
  fclose(f);
  if (memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4))
    return false;
  size_t pos = 12;
  const unsigned char* data = nullptr;
  uint32_t dl = 0, fmt = 0;
  while (pos + 8 <= (size_t)sz) {
    const unsigned char* id = b.data() + pos;
    uint32_t len = rd32(b.data() + pos + 4);
    const unsigned char* body = b.data() + pos + 8;
    if (pos + 8 + len > (size_t)sz)
      len = (uint32_t)sz - (pos + 8);
    if (!memcmp(id, "fmt ", 4) && len >= 16) {
      fmt = rd16(body);
      w.channels = rd16(body + 2);
      w.bits = rd16(body + 14);
    } else if (!memcmp(id, "data", 4)) {
      data = body;
      dl = len;
    }
    pos += 8 + len + (len & 1);
  }
  const bool isF = (fmt == 3);
  if (!data || !w.channels || (fmt != 1 && fmt != 0xFFFE && fmt != 3))
    return false;
  if (isF ? w.bits != 32 : (w.bits != 16 && w.bits != 24))
    return false;
  const unsigned bp = w.bits / 8, fb = bp * w.channels;
  if (!fb)
    return false;
  const uint32_t fr = dl / fb;
  if (fr < 32)
    return false;
  w.ch.assign(w.channels, std::vector<int32_t>(fr));
  for (uint32_t i = 0; i < fr; i++)
    for (unsigned c = 0; c < w.channels; c++) {
      const unsigned char* p = data + (size_t)i * fb + c * bp;
      if (isF) {
        float fv;
        memcpy(&fv, p, 4);
        double v = (double)fv * 8388608.0;
        v = v > 8388607 ? 8388607 : (v < -8388608 ? -8388608 : v);
        w.ch[c][i] = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
      } else
        w.ch[c][i] = (w.bits == 24)
          ? ((int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8)
          : (int16_t)(p[0] | (p[1] << 8));
    }
  if (isF)
    w.bits = 24;
  return true;
}

static bool loadWavPack(const char* path, Wav& w) {
  char err[128] = {0};
  WavpackContext* c = WavpackOpenFileInput(path, err, OPEN_NORMALIZE, 0);
  if (!c)
    return false;
  w.channels = WavpackGetNumChannels(c);
  w.bits = WavpackGetBitsPerSample(c);
  const uint32_t fr = WavpackGetNumSamples(c);
  if (!w.channels || fr < 32 || (w.bits != 16 && w.bits != 24)) {
    WavpackCloseFile(c);
    return false;
  }
  w.ch.assign(w.channels, std::vector<int32_t>(fr));
  std::vector<int32_t> buf((size_t)4096 * w.channels);
  uint32_t got = 0;
  while (got < fr) {
    uint32_t n = WavpackUnpackSamples(c, buf.data(), 4096);
    if (!n)
      break;
    for (uint32_t i = 0; i < n && got + i < fr; i++)
      for (unsigned ch = 0; ch < w.channels; ch++)
        w.ch[ch][got + i] = buf[(size_t)i * w.channels + ch];
    got += n;
  }
  WavpackCloseFile(c);
  return got >= 32;
}

static bool loadAny(const char* path, Wav& w) {
  if (loadPcmWav(path, w))
    return true;
  w = Wav();
  return loadWavPack(path, w);
}

// ---------- small linear algebra ----------
static void solve3(double R[3][3], double r[3], double a[3], int n) {
  double M[3][4];
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++)
      M[i][j] = R[i][j];
    M[i][n] = r[i];
  }
  for (int col = 0; col < n; col++) {
    int piv = col;
    for (int row = col + 1; row < n; row++)
      if (fabs(M[row][col]) > fabs(M[piv][col]))
        piv = row;
    for (int j = 0; j <= n; j++)
      std::swap(M[col][j], M[piv][j]);
    if (fabs(M[col][col]) < 1e-12) {
      a[col] = 0;
      continue;
    }
    for (int row = 0; row < n; row++)
      if (row != col) {
        double f = M[row][col] / M[col][col];
        for (int j = 0; j <= n; j++)
          M[row][j] -= f * M[col][j];
      }
  }
  for (int i = 0; i < n; i++)
    a[i] = fabs(M[i][i]) < 1e-12 ? 0 : M[i][n] / M[i][i];
}

// FLAC-style quantization: q[j] = round(a[j] * 2^shift), 15-bit magnitude cap.
struct QCoef {
  long long q[3] = {0, 0, 0};
  int shift = 14;
};
static QCoef quantize(const double a[3], int n) {
  QCoef qc;
  double maxc = 0;
  for (int j = 0; j < n; j++)
    maxc = std::max(maxc, fabs(a[j]));
  int shift = 14;
  if (maxc > 0)
    while ((maxc * (double)(1 << shift)) >= 32768.0 && shift > 0)
      shift--;
  qc.shift = shift;
  for (int j = 0; j < n; j++)
    qc.q[j] = llround(a[j] * (double)(1 << shift));
  return qc;
}

// ---------- per-block machinery ----------
// Residual of quantized predictor over [base, base+m) with continuous history.
static void residQ(
  const std::vector<int32_t>& v,
  size_t base,
  unsigned m,
  const QCoef& qc,
  int n,
  int32_t* res) {
  for (unsigned i = 0; i < m; i++) {
    size_t g = base + i;
    long long p = 0;
    for (int j = 0; j < n; j++)
      p += qc.q[j] * (long long)(g >= (size_t)(j + 1) ? v[g - 1 - j] : 0);
    p >>= qc.shift;
    res[i] = (int32_t)((long long)v[g] - p);
  }
}
static int widthOf(const int32_t* res, unsigned m) {
  int32_t mn = res[0], mx = res[0];
  for (unsigned i = 0; i < m; i++) {
    if (res[i] < mn)
      mn = res[i];
    if (res[i] > mx)
      mx = res[i];
  }
  return GOSoundCompressionCacheV3::bytesNeededSigned(mn, mx);
}
static int peakOf(const int32_t* res, unsigned m) {
  int32_t pk = 0;
  for (unsigned i = 0; i < m; i++)
    pk = std::max(pk, res[i] < 0 ? -res[i] : res[i]);
  return pk;
}

// Weighted LS over a block (weights w, taps continuous), coeffs out in a[].
static void weightedLS(
  const std::vector<int32_t>& v,
  size_t base,
  unsigned m,
  const double* wgt,
  int n,
  double a[3]) {
  double R[3][3] = {{0}}, r[3] = {0};
  for (unsigned i = 0; i < m; i++) {
    size_t g = base + i;
    double x[3];
    for (int j = 0; j < n; j++)
      x[j] = (g >= (size_t)(j + 1)) ? (double)v[g - 1 - j] : 0.0;
    double y = v[g], ww = wgt ? wgt[i] : 1.0;
    for (int p = 0; p < n; p++) {
      for (int q = 0; q < n; q++)
        R[p][q] += ww * x[p] * x[q];
      r[p] += ww * y * x[p];
    }
  }
  solve3(R, r, a, n);
}

// Peak-minimizing per-block fit: LS init, IRLS ramp p=4,8,16 (three passes
// each), evaluating the TRUE quantized block width after every iteration
// and keeping the best (width, peak) seen.
static void fitPeakBlock(
  const std::vector<int32_t>& v,
  size_t base,
  unsigned m,
  int n,
  QCoef& bestQ,
  int& bestW,
  int& bestPk,
  double aBest[3]) {
  std::vector<int32_t> res(m);
  std::vector<double> wgt(m, 1.0);
  double a[3];
  weightedLS(v, base, m, nullptr, n, a);
  QCoef qc = quantize(a, n);
  residQ(v, base, m, qc, n, res.data());
  bestQ = qc;
  bestW = widthOf(res.data(), m);
  bestPk = peakOf(res.data(), m);
  for (int j = 0; j < 3; j++)
    aBest[j] = a[j];
  static const double PS[] = {4, 4, 4, 8, 8, 8, 16, 16, 16};
  for (double p : PS) {
    double mx = 1.0;
    for (unsigned i = 0; i < m; i++)
      mx = std::max(mx, fabs((double)res[i]));
    for (unsigned i = 0; i < m; i++) {
      double r = fabs((double)res[i]) / mx; // normalized, avoids overflow
      wgt[i] = pow(std::max(r, 1e-6), p - 2.0);
    }
    weightedLS(v, base, m, wgt.data(), n, a);
    qc = quantize(a, n);
    residQ(v, base, m, qc, n, res.data());
    int wd = widthOf(res.data(), m), pk = peakOf(res.data(), m);
    if (wd < bestW || (wd == bestW && pk < bestPk)) {
      bestQ = qc;
      bestW = wd;
      bestPk = pk;
      for (int j = 0; j < 3; j++)
        aBest[j] = a[j];
    }
  }
}

// Hooke-Jeeves pattern search per block, directly on the lexicographic
// (quantized width, peak) objective - the exact per-block analogue of the
// global search that proved the integers optimal corpus-wide. Answers
// whether IRLS (an L-inf proxy) leaves anything on the table.
static void hjRefineBlock(
  const std::vector<int32_t>& v,
  size_t base,
  unsigned m,
  int n,
  double a[3],
  QCoef& bestQ,
  int& bestW,
  int& bestPk) {
  std::vector<int32_t> res(m);
  auto eval = [&](const double* c, QCoef& qOut, int& wOut, int& pkOut) {
    qOut = quantize(c, n);
    residQ(v, base, m, qOut, n, res.data());
    wOut = widthOf(res.data(), m);
    pkOut = peakOf(res.data(), m);
  };
  QCoef q;
  int wd, pk;
  eval(a, q, wd, pk);
  if (wd < bestW || (wd == bestW && pk < bestPk)) {
    bestQ = q;
    bestW = wd;
    bestPk = pk;
  }
  double cur[3] = {a[0], a[1], a[2]};
  int curW = wd, curPk = pk;
  for (double step = 0.5; step > 5e-4; step *= 0.5) {
    bool moved = true;
    int guard = 0;
    while (moved && guard++ < 40) {
      moved = false;
      for (int j = 0; j < n; j++)
        for (double s : {step, -step}) {
          double trial[3] = {cur[0], cur[1], cur[2]};
          trial[j] += s;
          eval(trial, q, wd, pk);
          if (wd < curW || (wd == curW && pk < curPk)) {
            cur[j] = trial[j];
            curW = wd;
            curPk = pk;
            moved = true;
          }
        }
    }
  }
  eval(cur, q, curW, curPk);
  if (curW < bestW || (curW == bestW && curPk < bestPk)) {
    bestQ = q;
    bestW = curW;
    bestPk = curPk;
  }
}

// ---------- corpus accumulators ----------
struct Totals {
  double raw = 0, fixed2 = 0, fixed3 = 0, fileLS3 = 0;
  double blkOrder = 0, blkLS3 = 0, blkLS3o = 0, blkPeak3 = 0, blkPeak3o = 0;
  double blkHJ3 = 0, blkHJ3o = 0;
  double blkBest = 0, blkBesto = 0;
  long long blocks = 0, bestFittedWins = 0;
  long long orderPick[5] = {0, 0, 0, 0, 0}; // blk-order histogram
};

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: %s <filelist>\n", argv[0]);
    return 1;
  }
  std::vector<std::string> files;
  {
    FILE* f = fopen(argv[1], "r");
    if (!f) {
      printf("cannot open %s\n", argv[1]);
      return 1;
    }
    char ln[8192];
    while (fgets(ln, sizeof(ln), f)) {
      char* nl = strchr(ln, '\n');
      if (nl)
        *nl = 0;
      if (ln[0])
        files.emplace_back(ln);
    }
    fclose(f);
  }

  Totals T;
  unsigned loaded = 0;
  std::vector<int32_t> res(BLK), tmp(BLK);

  for (auto& path : files) {
    Wav w;
    if (!loadAny(path.c_str(), w)) {
      printf("SKIP (load failed): %s\n", path.c_str());
      continue;
    }
    loaded++;
    const unsigned bytesPer = w.bits / 8;

    // per-file LS3 (quantized), from whole-file normal equations
    double Rf[3][3] = {{0}}, rf[3] = {0}, af[3];
    for (unsigned c = 0; c < w.channels; c++) {
      const auto& v = w.ch[c];
      for (size_t i = 3; i < v.size(); i++) {
        double x[3] = {(double)v[i - 1], (double)v[i - 2], (double)v[i - 3]};
        double y = v[i];
        for (int p = 0; p < 3; p++) {
          for (int q = 0; q < 3; q++)
            Rf[p][q] += x[p] * x[q];
          rf[p] += y * x[p];
        }
      }
    }
    solve3(Rf, rf, af, 3);
    QCoef fileQ = quantize(af, 3);

    static const double FX2[3] = {2, -1, 0}, FX3[3] = {3, -3, 1};
    const QCoef fx2 = quantize(FX2, 2), fx3 = quantize(FX3, 3);
    QCoef fxOrd[5];
    for (int o = 1; o <= 4; o++) {
      double a[3];
      // PRED_COEF rows are exact small integers; quantize() keeps them exact.
      double tmpc[3] = {
        (double)GOSoundCompressionCacheV3::PRED_COEF[o][0],
        (double)GOSoundCompressionCacheV3::PRED_COEF[o][1],
        (double)GOSoundCompressionCacheV3::PRED_COEF[o][2]};
      // order 4 needs a 4th tap; restrict this study to taps 1..3 for orders
      // 1..3 and emulate order 4 with its first three taps? NO - drop the
      // 4-tap order from blk-order here and use orders 1..3 only, so every
      // mode sees identical history requirements.
      (void)a;
      fxOrd[o] = quantize(tmpc, 3);
    }

    for (unsigned c = 0; c < w.channels; c++) {
      const auto& v = w.ch[c];
      const size_t N = v.size();
      for (size_t base = 0; base < N; base += BLK) {
        const unsigned m = (unsigned)std::min((size_t)BLK, N - base);
        T.raw += (double)m * bytesPer;
        T.blocks++;

        residQ(v, base, m, fx2, 2, res.data());
        int w2 = widthOf(res.data(), m);
        T.fixed2 += 1 + (double)w2 * m;

        residQ(v, base, m, fx3, 3, res.data());
        int w3 = widthOf(res.data(), m);
        T.fixed3 += 1 + (double)w3 * m;

        residQ(v, base, m, fileQ, 3, res.data());
        T.fileLS3 += 1 + (double)widthOf(res.data(), m) * m;

        // per-block best fixed order 1..3 (order-4 excluded: extra tap)
        int bestFixedW = w3, bestFixedO = 3;
        for (int o = 1; o <= 2; o++) {
          residQ(v, base, m, fxOrd[o], 3, res.data());
          int wd = widthOf(res.data(), m);
          if (wd < bestFixedW) {
            bestFixedW = wd;
            bestFixedO = o;
          }
        }
        T.orderPick[bestFixedO]++;
        T.blkOrder += 1 + (double)bestFixedW * m;

        // per-block LS3
        double a[3];
        weightedLS(v, base, m, nullptr, 3, a);
        QCoef qLS = quantize(a, 3);
        residQ(v, base, m, qLS, 3, res.data());
        int wLS = widthOf(res.data(), m);
        T.blkLS3 += 1 + (double)wLS * m;
        T.blkLS3o += 1 + COEF_OVERHEAD + (double)wLS * m;

        // per-block peak fit
        QCoef qPk;
        int wPk, pkPk;
        double aPk[3];
        fitPeakBlock(v, base, m, 3, qPk, wPk, pkPk, aPk);
        T.blkPeak3 += 1 + (double)wPk * m;
        T.blkPeak3o += 1 + COEF_OVERHEAD + (double)wPk * m;

        // Hooke-Jeeves refinement on the true (width, peak) objective,
        // seeded with the IRLS winner - measures what IRLS leaves behind
        int wHJ = wPk, pkHJ = pkPk;
        hjRefineBlock(v, base, m, 3, aPk, qPk, wHJ, pkHJ);
        T.blkHJ3 += 1 + (double)wHJ * m;
        T.blkHJ3o += 1 + COEF_OVERHEAD + (double)wHJ * m;

        // best-of encoder
        int wBest = bestFixedW;
        bool fittedWins = false;
        if (wLS < wBest) {
          wBest = wLS;
          fittedWins = true;
        }
        if (wHJ < wBest) {
          wBest = wHJ;
          fittedWins = true;
        }
        T.blkBest += 1 + (double)wBest * m;
        T.blkBesto += 1 + (fittedWins ? COEF_OVERHEAD : 0) + (double)wBest * m;
        if (fittedWins)
          T.bestFittedWins++;
      }
    }
  }

  printf("loaded %u/%zu files, %lld blocks\n", loaded, files.size(), T.blocks);
  auto row = [&](const char* name, double bytes) {
    printf(
      "  %-28s %10.0f B  %6.2f%% raw  %+7.2f%% vs fixed3\n",
      name,
      bytes,
      100.0 * bytes / T.raw,
      100.0 * (bytes - T.fixed3) / T.fixed3);
  };
  printf("\nV3 block-width sizes (native raw = %.0f B):\n", T.raw);
  row("fixed2 (2,-1)", T.fixed2);
  row("fixed3 (3,-3,1) BASELINE", T.fixed3);
  row("per-file LS3 (quantized)", T.fileLS3);
  row("blk best fixed order 1..3", T.blkOrder);
  row("blk-LS3 (no overhead)", T.blkLS3);
  row("blk-LS3 (+7B/blk)", T.blkLS3o);
  row("blk-PEAK3 (no overhead)", T.blkPeak3);
  row("blk-PEAK3 (+7B/blk)", T.blkPeak3o);
  row("blk-HJ3 (no overhead)", T.blkHJ3);
  row("blk-HJ3 (+7B/blk)", T.blkHJ3o);
  row("blk-BEST (no overhead)", T.blkBest);
  row("blk-BEST (+7B where fitted)", T.blkBesto);
  printf(
    "\nblk-order picks: o1=%lld o2=%lld o3=%lld   fitted wins in blk-BEST: "
    "%lld/%lld blocks (%.1f%%)\n",
    T.orderPick[1],
    T.orderPick[2],
    T.orderPick[3],
    T.bestFittedWins,
    T.blocks,
    100.0 * T.bestFittedWins / (double)T.blocks);
  return 0;
}
