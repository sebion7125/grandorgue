// Diagnoses the "many pipes at compression factor 1" report for the
// Rotterdam Laurenskerk Hoofdorgel set: per file, computes the V3 residual
// stream's encoded size under the CURRENT per-1024-block width scheme and
// under hypothetical finer granularities (256/64-sample sub-widths), plus
// the theoretical lower bound (every sample pays only its own minimal
// width - what a perfect escape coding could approach). Distinguishes
// "the material is just noisy" (even the bound is ~1.0) from "the
// block-wide width escalation wastes real bytes" (bound clearly better
// than the 1024 scheme).
//
// Reads WAV paths from stdin (24-bit PCM; channel 0), prints aggregate
// ratios and the worst files.
//
// Build:
//   g++ -O3 -std=gnu++17 rotterdam_width_analysis.cpp -o /tmp/claude-1000/rwa
//
// Usage:
//   find .../Rotterdam -iname '*.wav' | shuf -n 800 | /tmp/claude-1000/rwa

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<int32_t> loadWav24Mono(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return {};
  char riff[4];
  f.read(riff, 4);
  f.seekg(4, std::ios::cur);
  char wave[4];
  f.read(wave, 4);
  if (!f || memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0)
    return {};
  unsigned channels = 0, bitsPerSample = 0, audioFormat = 0;
  std::vector<unsigned char> data;
  while (f) {
    char id[4];
    f.read(id, 4);
    if (!f)
      break;
    uint32_t chunkSize = 0;
    f.read((char *)&chunkSize, 4);
    if (!f)
      break;
    if (memcmp(id, "fmt ", 4) == 0) {
      uint16_t fmt, nChannels, blockAlign, bps;
      uint32_t sr, byteRate;
      f.read((char *)&fmt, 2);
      f.read((char *)&nChannels, 2);
      f.read((char *)&sr, 4);
      f.read((char *)&byteRate, 4);
      f.read((char *)&blockAlign, 2);
      f.read((char *)&bps, 2);
      audioFormat = fmt;
      channels = nChannels;
      bitsPerSample = bps;
      if (chunkSize > 16)
        f.seekg(chunkSize - 16, std::ios::cur);
    } else if (memcmp(id, "data", 4) == 0) {
      data.resize(chunkSize);
      f.read((char *)data.data(), chunkSize);
    } else {
      f.seekg(chunkSize + (chunkSize & 1), std::ios::cur);
    }
  }
  if (audioFormat != 1 || bitsPerSample != 24 || channels == 0 || data.empty())
    return {};
  const unsigned nFrames = (unsigned)data.size() / (3u * channels);
  std::vector<int32_t> L(nFrames);
  const unsigned char *p = data.data();
  for (unsigned i = 0; i < nFrames; i++) {
    int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
    if (v & 0x800000)
      v |= 0xFF000000;
    L[i] = v;
    p += 3 * channels;
  }
  return L;
}

inline int widthOf(int32_t v) {
  if (v >= -128 && v <= 127)
    return 1;
  if (v >= -32768 && v <= 32767)
    return 2;
  if (v >= -8388608 && v <= 8388607)
    return 3;
  return 4;
}

// Encoded bytes with one width per `block` samples (1 header byte each).
size_t encodedBytes(const std::vector<int32_t> &res, unsigned block) {
  size_t total = 0;
  const unsigned n = (unsigned)res.size();
  for (unsigned i = 0; i < n; i += block) {
    const unsigned m = std::min(block, n - i);
    int w = 1;
    for (unsigned j = 0; j < m; j++)
      w = std::max(w, widthOf(res[i + j]));
    total += 1 + (size_t)m * w;
  }
  return total;
}

struct FileStat {
  std::string path;
  double r1024, r256, r64, rBound;
  unsigned nSamples;
};

} // namespace

// --detail <files...>: per-file inspection - a width timeline (one char
// per 1024-block: '.'=w1 ':'=w2 '#'=w3 '!'=w4, 64 blocks per line ~= 1.4s
// at 48k) plus residual statistics, to see WHERE the wide blocks sit
// (attack, sustain, throughout).
static int detailMode(int argc, char **argv) {
  for (int a = 2; a < argc; a++) {
    const auto pcm = loadWav24Mono(argv[a]);

    if (pcm.size() < 4096) {
      printf("%s: unusable (not 24-bit PCM or too short)\n", argv[a]);
      continue;
    }
    std::vector<int32_t> res(pcm.size());
    {
      int32_t value = 0, prev = 0, last = 0;
      for (size_t i = 0; i < pcm.size(); i++) {
        last = prev;
        prev = value;
        value = pcm[i];
        res[i] = value - (prev + ((prev - last) >> 1));
      }
    }
    double rms = 0, peak = 0;
    for (const int32_t r : res) {
      rms += (double)r * r;
      peak = std::max(peak, (double)std::abs(r));
    }
    rms = std::sqrt(rms / res.size());
    printf(
      "\n%s\n  %u samples (%.1fs @48k), residual RMS %.0f, peak %.0f, "
      "ratio %.3f (bound %.3f)\n  width timeline:\n",
      argv[a],
      (unsigned)pcm.size(),
      pcm.size() / 48000.0,
      rms,
      peak,
      (double)(pcm.size() * 3) / encodedBytes(res, 1024),
      [&] {
        size_t b = 0;
        for (const int32_t r : res)
          b += widthOf(r);
        return (double)(pcm.size() * 3) / b;
      }());
    const char sym[5] = {'?', '.', ':', '#', '!'};
    std::string tl;
    for (size_t i = 0; i < res.size(); i += 1024) {
      const size_t m = std::min<size_t>(1024, res.size() - i);
      int w = 1;
      for (size_t j = 0; j < m; j++)
        w = std::max(w, widthOf(res[i + j]));
      tl += sym[w];
    }
    for (size_t i = 0; i < tl.size(); i += 64)
      printf("  %6.1fs %s\n", i * 1024 / 48000.0, tl.substr(i, 64).c_str());
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc >= 3 && strcmp(argv[1], "--detail") == 0)
    return detailMode(argc, argv);

  std::string line;
  std::vector<FileStat> stats;
  size_t rawTotal = 0, b1024 = 0, b256 = 0, b64 = 0, bBound = 0;
  uint64_t widthBlocks[5] = {0}; // per-1024-block width histogram
  uint64_t outlierIn3Blocks = 0, samplesIn3Blocks = 0;

  while (std::getline(std::cin, line)) {
    if (line.empty())
      continue;
    const auto pcm = loadWav24Mono(line);
    if (pcm.size() < 4096)
      continue;

    // The production predictor: pred = prev + ((prev - last) >> 1).
    std::vector<int32_t> res(pcm.size());
    {
      int32_t value = 0, prev = 0, last = 0;
      for (size_t i = 0; i < pcm.size(); i++) {
        last = prev;
        prev = value;
        value = pcm[i];
        res[i] = value - (prev + ((prev - last) >> 1));
      }
    }

    const size_t raw = pcm.size() * 3;
    const size_t e1024 = encodedBytes(res, 1024);
    const size_t e256 = encodedBytes(res, 256);
    const size_t e64 = encodedBytes(res, 64);
    size_t bound = 0;
    for (size_t i = 0; i < res.size(); i++)
      bound += widthOf(res[i]);

    // Width histogram + outlier count inside width-3 1024-blocks: how many
    // of their samples individually needed 3 bytes? Few outliers = a finer
    // scheme wins big; most samples at 3 = the material is genuinely wide.
    for (size_t i = 0; i < res.size(); i += 1024) {
      const size_t m = std::min<size_t>(1024, res.size() - i);
      int w = 1;
      for (size_t j = 0; j < m; j++)
        w = std::max(w, widthOf(res[i + j]));
      widthBlocks[w]++;
      if (w >= 3) {
        samplesIn3Blocks += m;
        for (size_t j = 0; j < m; j++)
          if (widthOf(res[i + j]) >= 3)
            outlierIn3Blocks++;
      }
    }

    rawTotal += raw;
    b1024 += e1024;
    b256 += e256;
    b64 += e64;
    bBound += bound;
    stats.push_back(
      {line,
       (double)raw / e1024,
       (double)raw / e256,
       (double)raw / e64,
       (double)raw / bound,
       (unsigned)pcm.size()});
  }

  if (stats.empty()) {
    fprintf(stderr, "no usable files\n");
    return 1;
  }

  printf("files analysed: %zu\n", stats.size());
  printf(
    "aggregate ratio: 1024-block %.3fx | 256 %.3fx | 64 %.3fx | "
    "per-sample bound %.3fx\n",
    (double)rawTotal / b1024,
    (double)rawTotal / b256,
    (double)rawTotal / b64,
    (double)rawTotal / bBound);

  size_t fallback = 0;
  for (const auto &s : stats)
    if (s.r1024 <= 1.0)
      fallback++;
  printf(
    "files at ratio <= 1.0 under the current scheme (=> factor-1 fallback): "
    "%zu of %zu (%.1f%%)\n",
    fallback,
    stats.size(),
    100.0 * fallback / stats.size());

  const uint64_t totalBlocks
    = widthBlocks[1] + widthBlocks[2] + widthBlocks[3] + widthBlocks[4];
  printf(
    "1024-block widths: w1 %.1f%%  w2 %.1f%%  w3 %.1f%%  w4 %.1f%%\n",
    100.0 * widthBlocks[1] / totalBlocks,
    100.0 * widthBlocks[2] / totalBlocks,
    100.0 * widthBlocks[3] / totalBlocks,
    100.0 * widthBlocks[4] / totalBlocks);
  if (samplesIn3Blocks)
    printf(
      "inside w>=3 blocks: %.2f%% of samples individually need >=3 bytes\n",
      100.0 * outlierIn3Blocks / samplesIn3Blocks);

  std::sort(stats.begin(), stats.end(), [](const auto &a, const auto &b) {
    return a.r1024 < b.r1024;
  });
  printf("\nworst 12 files (current | 256 | 64 | bound):\n");
  for (size_t i = 0; i < std::min<size_t>(12, stats.size()); i++)
    printf(
      "  %.3f | %.3f | %.3f | %.3f  %s\n",
      stats[i].r1024,
      stats[i].r256,
      stats[i].r64,
      stats[i].rBound,
      stats[i].path.c_str());
  return 0;
}
