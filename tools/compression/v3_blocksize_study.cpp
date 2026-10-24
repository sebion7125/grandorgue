// v3_blocksize_study.cpp
//
// Question (user, 2026-07-13): V3 spends one width byte per 1024-sample
// block. The block width must cover the LOUDEST residual in the whole
// block, so a single transient inflates 1024 samples. Would a SMALLER
// block (finer width adaptation) plus a run-length width header ("this
// width holds for N blocks") save space overall? Must be measured on
// real samples.
//
// This replicates V3's exact predictor and width choice
// (GOSoundCompressionCacheV3::Encode / bytesNeededSigned), computes the
// residual stream ONCE per file (the predictor does not reset per block,
// so residuals are block-size independent), then for each candidate
// block size totals:
//   - FIXED  : current scheme, 1 width byte per block
//   - RLE    : width byte only when it changes + a varint run length
//   - DATA   : residual payload only (no header)   [the adaptation floor]
// and, at B=1024, the width histogram and the run-length distribution of
// unchanged width (directly: how often does the width actually change).
//
// Build:
//   g++ -O2 -std=gnu++17 -I ../../src/grandorgue  \
//     -I ../../src/v3codec \
//       v3_blocksize_study.cpp -o /tmp/claude-1000/blocksize

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GOSoundCompressionCacheV3.h"

// ---- minimal WAV reader: 16/24-bit PCM, mono/stereo, chunk-walked ----
struct Wav {
  unsigned channels = 0, bits = 0, rate = 0;
  std::vector<std::vector<int32_t>> ch; // ch[c][frame]
};

static uint32_t rd32(const unsigned char *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }

static bool loadWav(const char *path, Wav &w) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<unsigned char> b(sz);
  if (fread(b.data(), 1, sz, f) != (size_t)sz) {
    fclose(f);
    return false;
  }
  fclose(f);
  if (sz < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4))
    return false;

  size_t pos = 12;
  const unsigned char *data = nullptr;
  uint32_t dataLen = 0;
  while (pos + 8 <= (size_t)sz) {
    const unsigned char *id = b.data() + pos;
    uint32_t len = rd32(b.data() + pos + 4);
    const unsigned char *body = b.data() + pos + 8;
    if (!memcmp(id, "fmt ", 4) && len >= 16) {
      w.channels = rd16(body + 2);
      w.rate = rd32(body + 4);
      w.bits = rd16(body + 14);
    } else if (!memcmp(id, "data", 4)) {
      data = body;
      dataLen = len;
    }
    pos += 8 + len + (len & 1); // chunks are word-aligned
  }
  if (!data || !w.channels || (w.bits != 16 && w.bits != 24))
    return false;

  const unsigned bytesPer = w.bits / 8;
  const unsigned frameBytes = bytesPer * w.channels;
  const uint32_t frames = dataLen / frameBytes;
  w.ch.assign(w.channels, std::vector<int32_t>(frames));
  for (uint32_t i = 0; i < frames; i++)
    for (unsigned c = 0; c < w.channels; c++) {
      const unsigned char *p = data + (size_t)i * frameBytes + c * bytesPer;
      int32_t v;
      if (w.bits == 24)
        v = (int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8;
      else
        v = (int16_t)(p[0] | (p[1] << 8));
      w.ch[c][i] = v;
    }
  return true;
}

// V3 predictor residuals, continuous over the whole stream (per channel).
static void residuals(const Wav &w, std::vector<std::vector<int32_t>> &res) {
  res.assign(w.channels, {});
  for (unsigned c = 0; c < w.channels; c++) {
    const auto &s = w.ch[c];
    res[c].resize(s.size());
    int32_t last = 0, prev = 0, value = 0;
    for (size_t i = 0; i < s.size(); i++) {
      last = prev;
      prev = value;
      const int32_t val = s[i];
      const int32_t pred = prev + ((prev - last) >> 1);
      res[c][i] = val - pred;
      value = val;
    }
  }
}

static unsigned varintLen(uint32_t v) {
  unsigned n = 1;
  while (v >= 0x80) {
    v >>= 7;
    n++;
  }
  return n;
}

struct Totals {
  double fixed = 0, rle = 0, data = 0;
};

// One file, one block size: returns byte totals for the three schemes.
static Totals scoreBlockSize(
  const std::vector<std::vector<int32_t>> &res,
  unsigned channels,
  unsigned B,
  std::vector<unsigned> *widthHist = nullptr,
  std::vector<uint32_t> *runLengths = nullptr) {
  const size_t frames = res[0].size();
  Totals t;
  int prevWidth = -1;
  uint32_t curRun = 0;

  for (size_t base = 0; base < frames; base += B) {
    const size_t n = std::min((size_t)B, frames - base);
    int32_t mn = res[0][base], mx = res[0][base];
    for (unsigned c = 0; c < channels; c++)
      for (size_t j = 0; j < n; j++) {
        const int32_t r = res[c][base + j];
        if (r < mn)
          mn = r;
        if (r > mx)
          mx = r;
      }
    const unsigned width
      = (unsigned)GOSoundCompressionCacheV3::bytesNeededSigned(mn, mx);

    t.data += (double)n * channels * width;
    t.fixed += 1 + (double)n * channels * width;
    if ((int)width != prevWidth) {
      if (prevWidth >= 0) {
        t.rle += 1 + varintLen(curRun); // close previous run
        if (runLengths)
          runLengths->push_back(curRun);
      }
      prevWidth = (int)width;
      curRun = 1;
    } else
      curRun++;

    if (widthHist) {
      if (widthHist->size() < 5)
        widthHist->assign(5, 0);
      (*widthHist)[width]++;
    }
  }
  if (prevWidth >= 0) {
    t.rle += 1 + varintLen(curRun);
    if (runLengths)
      runLengths->push_back(curRun);
  }
  t.rle += t.data; // RLE total = residual payload + run-length width header
  return t;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: %s <samplelist.txt (label|path per line)>\n", argv[0]);
    return 1;
  }
  FILE *lst = fopen(argv[1], "r");
  if (!lst) {
    printf("cannot open %s\n", argv[1]);
    return 1;
  }

  const unsigned sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048};
  const unsigned NS = sizeof(sizes) / sizeof(sizes[0]);
  const unsigned CUR = 1024;

  // Aggregate over all files (sum of bytes), plus raw 24-bit baseline.
  std::vector<Totals> agg(NS);
  double aggRaw = 0;
  std::vector<uint32_t> aggRuns1024;
  std::vector<unsigned> aggHist1024(5, 0);

  char line[4096];
  printf("per-stop RLE savings vs current (B=1024 fixed); w1@64 = %% of "
    "64-blocks at width 1\n");
  printf("%s\n", std::string(92, '-').c_str());

  while (fgets(line, sizeof(line), lst)) {
    char *nl = strchr(line, '\n');
    if (nl)
      *nl = 0;
    if (!line[0])
      continue;
    char *bar = strchr(line, '|');
    std::string label = bar ? std::string(line, bar - line) : "?";
    const char *path = bar ? bar + 1 : line;

    Wav w;
    if (!loadWav(path, w)) {
      printf("%-16s  (load failed)\n", label.c_str());
      continue;
    }
    std::vector<std::vector<int32_t>> res;
    residuals(w, res);
    const size_t frames = res[0].size();
    aggRaw += (double)frames * w.channels * 3; // 24-bit source bytes

    Totals cur;
    double bestRle = 1e18;
    unsigned bestB = CUR;
    for (unsigned s = 0; s < NS; s++) {
      std::vector<unsigned> *hp = nullptr;
      std::vector<uint32_t> *rp = nullptr;
      std::vector<unsigned> hist;
      std::vector<uint32_t> runs;
      if (sizes[s] == CUR) {
        hp = &hist;
        rp = &runs;
      }
      Totals t = scoreBlockSize(res, w.channels, sizes[s], hp, rp);
      agg[s].fixed += t.fixed;
      agg[s].rle += t.rle;
      agg[s].data += t.data;
      if (sizes[s] == CUR) {
        cur = t;
        for (unsigned k = 1; k <= 4; k++)
          aggHist1024[k] += (k < hist.size() ? hist[k] : 0);
        aggRuns1024.insert(aggRuns1024.end(), runs.begin(), runs.end());
      }
      if (t.rle < bestRle) {
        bestRle = t.rle;
        bestB = sizes[s];
      }
    }
    // Pragmatic sweet spots + the mechanism: width-1 usage at B=64.
    std::vector<unsigned> h64;
    Totals t64 = scoreBlockSize(res, w.channels, 64, &h64);
    Totals t128 = scoreBlockSize(res, w.channels, 128);
    double blk64 = 0;
    for (unsigned k = 1; k <= 4; k++)
      blk64 += (k < h64.size() ? h64[k] : 0);
    const double w1 = h64.size() > 1 ? 100.0 * h64[1] / blk64 : 0.0;
    printf(
      "%-16s %8zu | rle@64 %+6.1f%%  rle@128 %+6.1f%%  best B=%-5u %+6.1f%% "
      "| w1@64=%.0f%%\n",
      label.c_str(),
      frames,
      100.0 * (t64.rle - cur.fixed) / cur.fixed,
      100.0 * (t128.rle - cur.fixed) / cur.fixed,
      bestB,
      100.0 * (bestRle - cur.fixed) / cur.fixed,
      w1);
  }
  fclose(lst);

  printf("\n=== AGGREGATE over all files ===\n");
  printf("raw 24-bit source bytes : %.0f\n", aggRaw);
  printf(
    "%-6s | %12s %6s | %12s %6s | %12s %6s\n",
    "B",
    "FIXED",
    "ratio",
    "RLE",
    "ratio",
    "DATA-only",
    "ratio");
  double curFixed = 0;
  for (unsigned s = 0; s < NS; s++)
    if (sizes[s] == CUR)
      curFixed = agg[s].fixed;
  for (unsigned s = 0; s < NS; s++) {
    printf(
      "%-6u | %12.0f %5.1f%% | %12.0f %5.1f%% | %12.0f %5.1f%%%s\n",
      sizes[s],
      agg[s].fixed,
      100.0 * agg[s].fixed / aggRaw,
      agg[s].rle,
      100.0 * agg[s].rle / aggRaw,
      agg[s].data,
      100.0 * agg[s].data / aggRaw,
      sizes[s] == CUR ? "   <- current" : "");
  }

  // Savings of the best scheme vs current.
  double bestAgg = 1e18;
  unsigned bestAggB = CUR;
  const char *bestScheme = "fixed";
  for (unsigned s = 0; s < NS; s++) {
    if (agg[s].fixed < bestAgg) {
      bestAgg = agg[s].fixed;
      bestAggB = sizes[s];
      bestScheme = "fixed";
    }
    if (agg[s].rle < bestAgg) {
      bestAgg = agg[s].rle;
      bestAggB = sizes[s];
      bestScheme = "rle";
    }
  }
  printf(
    "\nbest: B=%u %s -> %.2f%% smaller than current (B=1024 fixed)\n",
    bestAggB,
    bestScheme,
    100.0 * (curFixed - bestAgg) / curFixed);

  // Width behaviour at the CURRENT block size.
  printf("\n=== width behaviour at B=1024 (current) ===\n");
  double totBlocks = 0;
  for (unsigned k = 1; k <= 4; k++)
    totBlocks += aggHist1024[k];
  for (unsigned k = 1; k <= 4; k++)
    printf(
      "  width %u byte : %8u blocks (%.1f%%)\n",
      k,
      aggHist1024[k],
      100.0 * aggHist1024[k] / totBlocks);

  // Run-length: how many consecutive 1024-blocks keep the same width.
  double runSum = 0, runMax = 0;
  for (uint32_t r : aggRuns1024) {
    runSum += r;
    if (r > runMax)
      runMax = r;
  }
  const double changes = aggRuns1024.size();
  printf(
    "\n  width-change runs: %.0f  (a change every %.1f blocks on average;\n"
    "  i.e. the width is UNCHANGED across %.1f%% of block boundaries)\n",
    changes,
    runSum / changes,
    100.0 * (1.0 - changes / totBlocks));
  printf("  longest unchanged run: %.0f blocks\n", runMax);
  return 0;
}
