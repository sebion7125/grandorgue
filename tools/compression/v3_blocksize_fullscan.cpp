// v3_blocksize_fullscan.cpp
//
// Whole-Rotterdam study (user, 2026-07-13): does V3 with a smaller block
// + run-length width header beat LEGACY in size, and is the win
// concentrated on badly-compressing pipes? Scans every WAV in a file
// list, multithreaded.
//
// Per file it computes, from ONE residual pass (V3 second-order
// predictor):
//   - raw    : source bytes (bits/8 * channels * frames)
//   - legacy : GOSoundCompressionCache variable-length per-sample bytes
//   - V3[B]  : per block size B, FIXED (1 width byte/block) and RLE
//              (width byte on change + varint run) totals; width is
//              bytesNeededSigned over both channels, exactly as V3 encodes.
//
// Aggregates: totals per scheme, per-pipe win rate vs legacy, and a
// breakdown BY COMPRESSIBILITY BUCKET (legacy ratio) to see where any
// win lives. Block sizes go down to 1 to find the overhead crossover.
//
// Build:
//   g++ -O2 -std=gnu++17 -pthread -I ../../src/grandorgue  \
//     -I ../../src/v3codec \
//       v3_blocksize_fullscan.cpp -o /tmp/claude-1000/fullscan

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sound/playing/GOSoundCompressionCache.h"
#include "GOSoundCompressionCacheV3.h"

static const unsigned kSizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048};
static const unsigned NS = sizeof(kSizes) / sizeof(kSizes[0]);
static const unsigned CUR_IDX = 10; // 1024
static const unsigned NBUCK = 6;    // legacy ratio buckets: <50,50-60,...,>=90

static uint32_t rd32(const unsigned char *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }

struct Wav {
  unsigned channels = 0, bits = 0;
  std::vector<std::vector<int32_t>> ch;
};

static bool loadWav(const char *path, Wav &w) {
  FILE *f = fopen(path, "rb");
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
  const unsigned char *data = nullptr;
  uint32_t dataLen = 0;
  unsigned fmt = 0;
  while (pos + 8 <= (size_t)sz) {
    const unsigned char *id = b.data() + pos;
    uint32_t len = rd32(b.data() + pos + 4);
    const unsigned char *body = b.data() + pos + 8;
    if (pos + 8 + len > (size_t)sz)
      len = (uint32_t)sz - (pos + 8); // truncated final chunk
    if (!memcmp(id, "fmt ", 4) && len >= 16) {
      fmt = rd16(body);
      w.channels = rd16(body + 2);
      w.bits = rd16(body + 14);
    } else if (!memcmp(id, "data", 4)) {
      data = body;
      dataLen = len;
    }
    pos += 8 + len + (len & 1);
  }
  // Accept PCM (1), EXTENSIBLE (0xFFFE) integer 16/24-bit, and IEEE
  // float (3) 32-bit. GrandOrgue converts float WAVs to its internal
  // fixed-point on load, so we scale float [-1,1] to 24-bit int and
  // treat it as a 24-bit source (format16 legacy path, 3 bytes/sample
  // raw) - the representation that actually gets compressed.
  const bool isFloat = (fmt == 3);
  if (!data || !w.channels || (fmt != 1 && fmt != 0xFFFE && fmt != 3))
    return false;
  if (isFloat) {
    if (w.bits != 32)
      return false;
  } else if (w.bits != 16 && w.bits != 24)
    return false;
  const unsigned bp = w.bits / 8, fb = bp * w.channels;
  if (fb == 0)
    return false;
  const uint32_t frames = dataLen / fb;
  if (frames < 2)
    return false;
  w.ch.assign(w.channels, std::vector<int32_t>(frames));
  for (uint32_t i = 0; i < frames; i++)
    for (unsigned c = 0; c < w.channels; c++) {
      const unsigned char *p = data + (size_t)i * fb + c * bp;
      if (isFloat) {
        float f;
        memcpy(&f, p, 4);
        double v = (double)f * 8388608.0; // [-1,1] -> 24-bit
        if (v > 8388607.0)
          v = 8388607.0;
        if (v < -8388608.0)
          v = -8388608.0;
        w.ch[c][i] = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
      } else
        w.ch[c][i] = (w.bits == 24)
          ? ((int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8)
          : (int16_t)(p[0] | (p[1] << 8));
    }
  if (isFloat)
    w.bits = 24; // downstream: 24-bit int representation
  return true;
}

static unsigned varintLen(uint32_t v) {
  unsigned n = 1;
  while (v >= 0x80) {
    v >>= 7;
    n++;
  }
  return n;
}

// Legacy per-sample byte cost (mirrors writeCompressed16 / writeCompressed8).
static inline unsigned legacyBytes(int enc, bool format16) {
  if (format16) {
    if (-16384 <= enc && enc <= 16383)
      return 2;
    if (-2097152 <= enc && enc <= 2097151)
      return 3;
    return 4;
  }
  if (-64 <= enc && enc <= 63)
    return 1;
  if (-8192 <= enc && enc <= 8191)
    return 2;
  if (-1048576 <= enc && enc <= 1048575)
    return 3;
  return 4;
}

struct Acc {
  double raw = 0, legacy = 0;
  double fixed[NS] = {0};
  double rle[NS] = {0};
  // per compressibility bucket (by legacy ratio): raw, legacy, v3@1024, v3best-rle
  double bRaw[NBUCK] = {0}, bLeg[NBUCK] = {0}, bCur[NBUCK] = {0}, bBest[NBUCK] = {0};
  uint64_t bFiles[NBUCK] = {0};
  // per-pipe V3(best-rle) vs legacy
  uint64_t files = 0, loaded = 0, skipped = 0;
  uint64_t v3beatsLegacy = 0;      // best RLE over all B beats legacy
  uint64_t v3_64_beatsLegacy = 0;  // B=64 RLE beats legacy (pragmatic)
  // width-change stats at 1024
  double runSum1024 = 0, boundaries1024 = 0, changes1024 = 0;

  void merge(const Acc &o) {
    raw += o.raw;
    legacy += o.legacy;
    for (unsigned s = 0; s < NS; s++) {
      fixed[s] += o.fixed[s];
      rle[s] += o.rle[s];
    }
    for (unsigned k = 0; k < NBUCK; k++) {
      bRaw[k] += o.bRaw[k];
      bLeg[k] += o.bLeg[k];
      bCur[k] += o.bCur[k];
      bBest[k] += o.bBest[k];
      bFiles[k] += o.bFiles[k];
    }
    files += o.files;
    loaded += o.loaded;
    skipped += o.skipped;
    v3beatsLegacy += o.v3beatsLegacy;
    v3_64_beatsLegacy += o.v3_64_beatsLegacy;
    runSum1024 += o.runSum1024;
    boundaries1024 += o.boundaries1024;
    changes1024 += o.changes1024;
  }
};

static void processFile(const Wav &w, Acc &a) {
  const unsigned C = w.channels;
  const size_t frames = w.ch[0].size();
  const bool format16 = (w.bits > 16);

  // residuals + legacy cost in one pass.
  std::vector<std::vector<int32_t>> res(C, std::vector<int32_t>(frames));
  double legacy = 0;
  for (unsigned c = 0; c < C; c++) {
    int32_t last = 0, prev = 0, value = 0;
    const auto &s = w.ch[c];
    auto &r = res[c];
    for (size_t i = 0; i < frames; i++) {
      last = prev;
      prev = value;
      const int32_t val = s[i];
      const int32_t enc = val - (prev + ((prev - last) >> 1));
      r[i] = enc;
      legacy += legacyBytes(enc, format16);
      value = val;
    }
  }
  const double raw = (double)frames * C * (w.bits / 8);

  // V3 at every block size: FIXED + RLE, plus width-run stats at 1024.
  double fixedB[NS], rleB[NS];
  for (unsigned s = 0; s < NS; s++) {
    const unsigned B = kSizes[s];
    double data = 0, hdr = 0;
    int prevW = -1;
    uint32_t run = 0;
    double changes = 0, blocks = 0;
    for (size_t base = 0; base < frames; base += B) {
      const size_t n = std::min((size_t)B, frames - base);
      int32_t mn = res[0][base], mx = res[0][base];
      for (unsigned c = 0; c < C; c++) {
        const int32_t *rc = res[c].data();
        for (size_t j = 0; j < n; j++) {
          const int32_t v = rc[base + j];
          if (v < mn)
            mn = v;
          if (v > mx)
            mx = v;
        }
      }
      const unsigned width
        = (unsigned)GOSoundCompressionCacheV3::bytesNeededSigned(mn, mx);
      data += (double)n * C * width;
      blocks++;
      if ((int)width != prevW) {
        if (prevW >= 0) {
          hdr += 1 + varintLen(run);
          changes++;
        }
        prevW = (int)width;
        run = 1;
      } else
        run++;
    }
    if (prevW >= 0)
      hdr += 1 + varintLen(run);
    fixedB[s] = data + blocks; // 1 width byte per block
    rleB[s] = data + hdr;
    if (s == CUR_IDX) {
      a.runSum1024 += blocks;
      a.boundaries1024 += (blocks > 0 ? blocks - 1 : 0);
      a.changes1024 += changes;
    }
  }

  // best RLE over all block sizes for this pipe.
  double bestRle = 1e300;
  for (unsigned s = 0; s < NS; s++)
    if (rleB[s] < bestRle)
      bestRle = rleB[s];

  // compressibility bucket by legacy ratio.
  const double lr = legacy / raw; // <1 = compresses
  unsigned k;
  if (lr < 0.50)
    k = 0;
  else if (lr < 0.60)
    k = 1;
  else if (lr < 0.70)
    k = 2;
  else if (lr < 0.80)
    k = 3;
  else if (lr < 0.90)
    k = 4;
  else
    k = 5;

  a.raw += raw;
  a.legacy += legacy;
  for (unsigned s = 0; s < NS; s++) {
    a.fixed[s] += fixedB[s];
    a.rle[s] += rleB[s];
  }
  a.bRaw[k] += raw;
  a.bLeg[k] += legacy;
  a.bCur[k] += fixedB[CUR_IDX];
  a.bBest[k] += bestRle;
  a.bFiles[k]++;
  a.loaded++;
  if (bestRle < legacy)
    a.v3beatsLegacy++;
  // B=64 is index 6
  if (rleB[6] < legacy)
    a.v3_64_beatsLegacy++;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: %s <filelist.txt> [nthreads]\n", argv[0]);
    return 1;
  }
  std::vector<std::string> files;
  {
    FILE *f = fopen(argv[1], "r");
    if (!f) {
      printf("cannot open %s\n", argv[1]);
      return 1;
    }
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
      char *nl = strchr(line, '\n');
      if (nl)
        *nl = 0;
      if (line[0])
        files.emplace_back(line);
    }
    fclose(f);
  }
  const unsigned nthreads
    = argc >= 3 ? (unsigned)atoi(argv[2]) : std::thread::hardware_concurrency();
  printf(
    "scanning %zu files with %u threads...\n", files.size(), nthreads);
  fflush(stdout);

  std::atomic<size_t> next{0};
  std::atomic<size_t> doneCount{0};
  std::vector<Acc> perThread(nthreads);
  std::mutex logMx;

  auto worker = [&](unsigned tid) {
    Acc &a = perThread[tid];
    for (;;) {
      size_t i = next.fetch_add(1);
      if (i >= files.size())
        break;
      a.files++;
      Wav w;
      if (loadWav(files[i].c_str(), w))
        processFile(w, a);
      else
        a.skipped++;
      size_t d = ++doneCount;
      if ((d & 0x3FF) == 0) {
        std::lock_guard<std::mutex> lk(logMx);
        printf("\r  %zu / %zu", d, files.size());
        fflush(stdout);
      }
    }
  };

  std::vector<std::thread> pool;
  for (unsigned t = 0; t < nthreads; t++)
    pool.emplace_back(worker, t);
  for (auto &th : pool)
    th.join();

  Acc a;
  for (auto &p : perThread)
    a.merge(p);

  printf("\rdone. loaded %llu, skipped %llu of %llu\n\n",
    (unsigned long long)a.loaded,
    (unsigned long long)a.skipped,
    (unsigned long long)a.files);

  printf("=== AGGREGATE (whole set) ===\n");
  printf("raw 24-bit bytes : %.0f (%.2f GB)\n", a.raw, a.raw / 1e9);
  printf("legacy           : %.0f  (%.2f%% of raw)\n",
    a.legacy, 100.0 * a.legacy / a.raw);
  printf("V3 @1024 fixed   : %.0f  (%.2f%% of raw)  [current production]\n\n",
    a.fixed[CUR_IDX], 100.0 * a.fixed[CUR_IDX] / a.raw);

  printf("%-6s | %14s %7s | %14s %7s | %8s\n",
    "B", "FIXED", "%raw", "RLE", "%raw", "vs.leg");
  for (unsigned s = 0; s < NS; s++) {
    printf("%-6u | %14.0f %6.2f%% | %14.0f %6.2f%% | %+7.2f%%%s\n",
      kSizes[s],
      a.fixed[s], 100.0 * a.fixed[s] / a.raw,
      a.rle[s], 100.0 * a.rle[s] / a.raw,
      100.0 * (a.rle[s] - a.legacy) / a.legacy,
      s == CUR_IDX ? "  <-cur" : "");
  }

  // best aggregate scheme
  double best = 1e300;
  unsigned bestB = 0;
  const char *bestScheme = "";
  for (unsigned s = 0; s < NS; s++) {
    if (a.fixed[s] < best) { best = a.fixed[s]; bestB = kSizes[s]; bestScheme = "fixed"; }
    if (a.rle[s] < best) { best = a.rle[s]; bestB = kSizes[s]; bestScheme = "rle"; }
  }
  printf("\nbest aggregate : B=%u %s = %.2f%% of raw, %+.2f%% vs legacy, "
    "%+.2f%% vs current\n",
    bestB, bestScheme, 100.0 * best / a.raw,
    100.0 * (best - a.legacy) / a.legacy,
    100.0 * (best - a.fixed[CUR_IDX]) / a.fixed[CUR_IDX]);

  printf("\nper-pipe: V3(best-RLE) beats legacy on %llu / %llu (%.1f%%);  "
    "B=64-RLE beats legacy on %.1f%%\n",
    (unsigned long long)a.v3beatsLegacy, (unsigned long long)a.loaded,
    100.0 * a.v3beatsLegacy / a.loaded,
    100.0 * a.v3_64_beatsLegacy / a.loaded);

  printf("\n=== BY COMPRESSIBILITY (legacy ratio bucket) ===\n");
  printf("%-10s %8s | %6s %6s %6s %7s\n",
    "leg-ratio", "pipes", "leg%", "cur%", "best%", "best-leg");
  const char *blab[NBUCK]
    = {"<50%", "50-60%", "60-70%", "70-80%", "80-90%", ">=90%"};
  for (unsigned k = 0; k < NBUCK; k++) {
    if (a.bFiles[k] == 0) {
      printf("%-10s %8llu |   (none)\n", blab[k], 0ULL);
      continue;
    }
    printf("%-10s %8llu | %5.1f%% %5.1f%% %5.1f%% %+6.2f%%\n",
      blab[k], (unsigned long long)a.bFiles[k],
      100.0 * a.bLeg[k] / a.bRaw[k],
      100.0 * a.bCur[k] / a.bRaw[k],
      100.0 * a.bBest[k] / a.bRaw[k],
      100.0 * (a.bBest[k] - a.bLeg[k]) / a.bLeg[k]);
  }

  printf("\n=== width behaviour at B=1024 ===\n");
  printf("blocks %.0f, width changes %.0f -> unchanged across %.2f%% of "
    "boundaries; avg run %.1f blocks\n",
    a.runSum1024, a.changes1024,
    100.0 * (1.0 - a.changes1024 / a.boundaries1024),
    a.runSum1024 / (a.changes1024 + 1));
  return 0;
}
