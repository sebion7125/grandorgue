// codec_fleet_benchmark.cpp
//
// Runs the ratio/speed comparison from fresh_production_compare.cpp across
// a large, broadly-sampled set of real GrandOrgue WAV files, instead of a
// handful of hand-picked ones - to see where we actually stand in
// aggregate, and to find out how common/rare the "attack transient beats
// our block-width scheme on ratio" case from the manual investigation
// really is across real-world material.
//
// Compares, per group of 8 real files (each treated as one independent
// mono "voice"/lane):
//   production        - GO's real, unmodified GOSoundCompressionCache
//                        (writeCompressed16/DecompressionStep), one
//                        independent instance per lane (channels=1) -
//                        exactly today's per-voice decode.
//   our codec, scalar  - independent per-lane block-width encoding,
//                        honest scalar decode (no batching trick).
//   our codec, SIMD    - planar extraction + AVX2 8x8 transpose (the
//                        winning design from the manual investigation).
//
// Reports aggregate ratio and ns/channel-sample for all three, plus the
// worst-N files by (our ratio - production ratio) to characterize how bad
// the block-width penalty gets on real material, and how often our codec
// still wins on ratio despite the flaw found on attack-heavy files.
//
// Usage:
//   codec_fleet_benchmark -l <file_list.txt> [--max-groups N] [--stride S]
//   (file_list.txt: one WAV path per line; only 24-bit files matter, others
//   are skipped)
//
// Result (2026-07-06, 500 groups = 4000 real files, 1.85 GB raw, stride-20
// sampled across the whole available organ collection - the small 8-file
// hand-picked "MIXED" set used earlier in the manual investigation was NOT
// representative):
//   ratio:  production 1.4335x   our codec 1.5321x   (we win overall; our
//           codec had the better ratio in 358/500 groups, production in 142)
//   speed (ns/channel-sample): production 1.354   our scalar 1.278 (1.06x)
//           our SIMD 0.285 (4.75x vs production, 4.48x vs our own scalar)
// The block-width-vs-per-sample-escape penalty found on 3 unusually busy
// attack-transient files (Oktavbass/Floete/Mixtur, ~40-50% needing 3-byte
// codes throughout, not just at onset) is real but a minority case in
// practice, not the norm.
//
// Width=3 (24-bit) extraction uses a PSHUFB-based unpack (_mm_shuffle_epi8
// with a 0x80 zero-fill selector, 4 samples/instruction, then one
// _mm_srai_epi32 to sign-extend) instead of a per-sample scalar OR+shift -
// width=3 was previously ~2.2x slower than widths 1/2/4 (the only
// non-power-of-2 stride, not auto-vectorizable); this closes that gap
// almost entirely and is what raised the SIMD speedup from 3.82x to 4.75x
// above.
//
// Build:
//   g++ -O3 -mavx2 -ffast-math -funroll-loops -std=gnu++17 \
//     -I /home/vboxuser/grandorgue/src/grandorgue \
//     codec_fleet_benchmark.cpp -o codec_fleet_benchmark

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <immintrin.h>

#include "sound/playing/GOSoundCompressionCache.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr unsigned REPS = 3; // fewer reps than single-file tests - averaged
                             // over many groups anyway, keeps total runtime
                             // sane across hundreds of files.
constexpr unsigned BLOCK_SAMPLES = 1024;
constexpr int LANES = 8;
constexpr unsigned MIN_SAMPLES = BLOCK_SAMPLES * 4; // skip too-short files

double bestSeconds(unsigned reps, auto &&fn) {
  double best = 1e30;
  for (unsigned r = 0; r < reps; r++) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
  }
  return best;
}

// Returns empty vector on any parse failure or non-24-bit format (caller skips).
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
      f.seekg(chunkSize, std::ios::cur);
    }
  }
  if (audioFormat != 1 || bitsPerSample != 24 || channels == 0
      || data.empty())
    return {};
  const unsigned bytesPerSample = 3;
  const unsigned nFrames = (unsigned)data.size() / (bytesPerSample * channels);
  std::vector<int32_t> L(nFrames);
  const unsigned char *p = data.data();
  for (unsigned i = 0; i < nFrames; i++) {
    int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
    if (v & 0x800000)
      v |= 0xFF000000;
    L[i] = v;
    p += bytesPerSample * channels;
  }
  return L;
}

inline int bytesNeededSigned(int32_t minV, int32_t maxV) {
  if (minV >= -128 && maxV <= 127)
    return 1;
  if (minV >= -32768 && maxV <= 32767)
    return 2;
  if (minV >= -8388608 && maxV <= 8388607)
    return 3;
  return 4;
}

struct EncodedLane {
  std::vector<uint8_t> bytes;
};

EncodedLane encodeLaneIndependent(
  const std::vector<int32_t> &residuals, unsigned nSamples) {
  EncodedLane lane;
  for (unsigned i = 0; i < nSamples; i += BLOCK_SAMPLES) {
    const unsigned n = std::min((unsigned)BLOCK_SAMPLES, nSamples - i);
    int32_t minV = residuals[i], maxV = residuals[i];
    for (unsigned j = 0; j < n; j++) {
      minV = std::min(minV, residuals[i + j]);
      maxV = std::max(maxV, residuals[i + j]);
    }
    const int width = bytesNeededSigned(minV, maxV);
    lane.bytes.push_back((uint8_t)width);
    const size_t base = lane.bytes.size();
    lane.bytes.resize(base + (size_t)n * width);
    uint8_t *out = lane.bytes.data() + base;
    for (unsigned j = 0; j < n; j++) {
      const uint32_t v = (uint32_t)residuals[i + j];
      for (int b = 0; b < width; b++)
        out[j * width + b] = (uint8_t)(v >> (8 * b));
    }
  }
  return lane;
}

inline int32_t extractResidual(const uint8_t *ib, int width) {
  if (width == 1)
    return (int32_t)(int8_t)ib[0];
  if (width == 2) {
    int16_t v;
    memcpy(&v, ib, 2);
    return v;
  }
  if (width == 3) {
    const uint32_t raw
      = (uint32_t)ib[0] | ((uint32_t)ib[1] << 8) | ((uint32_t)ib[2] << 16);
    return (int32_t)(raw << 8) >> 8;
  }
  int32_t v;
  memcpy(&v, ib, 4);
  return v;
}

template <int WIDTH>
inline void extractLanePlanar(
  const uint8_t *__restrict ib, unsigned m, int32_t *__restrict scratchRow) {
  if constexpr (WIDTH == 3) {
    // PSHUFB-based: shuffle 12 packed bytes (4 samples) into the byte
    // layout [0,b0,b1,b2] per int32 lane in one instruction (0x80 selector
    // byte forces zero), then one arithmetic shift sign-extends all 4 at
    // once. Measured 2x overall speedup for width=3 content (the only
    // non-power-of-2 stride, previously the sole non-vectorized width).
    static const __m128i mask = _mm_setr_epi8(
      (char)0x80, 0, 1, 2, (char)0x80, 3, 4, 5,
      (char)0x80, 6, 7, 8, (char)0x80, 9, 10, 11);
    unsigned j = 0;
    for (; j + 4 <= m; j += 4) {
      __m128i in = _mm_loadu_si128((const __m128i *)(ib + (size_t)j * 3));
      __m128i shuffled = _mm_shuffle_epi8(in, mask);
      __m128i result = _mm_srai_epi32(shuffled, 8);
      _mm_storeu_si128((__m128i *)(scratchRow + j), result);
    }
    for (; j < m; j++) {
      const uint8_t *b = ib + (size_t)j * 3;
      const uint32_t raw
        = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
      scratchRow[j] = (int32_t)(raw << 8) >> 8;
    }
    return;
  }
  for (unsigned j = 0; j < m; j++) {
    const uint8_t *__restrict b = ib + (size_t)j * WIDTH;
    int32_t val;
    if constexpr (WIDTH == 1) {
      val = (int32_t)(int8_t)b[0];
    } else if constexpr (WIDTH == 2) {
      int16_t v;
      memcpy(&v, b, 2);
      val = v;
    } else {
      memcpy(&val, b, 4);
    }
    scratchRow[j] = val;
  }
}

inline void transpose8x8(__m256i *m) {
  __m256 r0 = _mm256_castsi256_ps(m[0]);
  __m256 r1 = _mm256_castsi256_ps(m[1]);
  __m256 r2 = _mm256_castsi256_ps(m[2]);
  __m256 r3 = _mm256_castsi256_ps(m[3]);
  __m256 r4 = _mm256_castsi256_ps(m[4]);
  __m256 r5 = _mm256_castsi256_ps(m[5]);
  __m256 r6 = _mm256_castsi256_ps(m[6]);
  __m256 r7 = _mm256_castsi256_ps(m[7]);

  __m256 t0 = _mm256_unpacklo_ps(r0, r1);
  __m256 t1 = _mm256_unpackhi_ps(r0, r1);
  __m256 t2 = _mm256_unpacklo_ps(r2, r3);
  __m256 t3 = _mm256_unpackhi_ps(r2, r3);
  __m256 t4 = _mm256_unpacklo_ps(r4, r5);
  __m256 t5 = _mm256_unpackhi_ps(r4, r5);
  __m256 t6 = _mm256_unpacklo_ps(r6, r7);
  __m256 t7 = _mm256_unpackhi_ps(r6, r7);

  __m256 tt0 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
  __m256 tt1 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
  __m256 tt2 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
  __m256 tt3 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
  __m256 tt4 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(1, 0, 1, 0));
  __m256 tt5 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(3, 2, 3, 2));
  __m256 tt6 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(1, 0, 1, 0));
  __m256 tt7 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(3, 2, 3, 2));

  m[0] = _mm256_castps_si256(_mm256_permute2f128_ps(tt0, tt4, 0x20));
  m[1] = _mm256_castps_si256(_mm256_permute2f128_ps(tt1, tt5, 0x20));
  m[2] = _mm256_castps_si256(_mm256_permute2f128_ps(tt2, tt6, 0x20));
  m[3] = _mm256_castps_si256(_mm256_permute2f128_ps(tt3, tt7, 0x20));
  m[4] = _mm256_castps_si256(_mm256_permute2f128_ps(tt0, tt4, 0x31));
  m[5] = _mm256_castps_si256(_mm256_permute2f128_ps(tt1, tt5, 0x31));
  m[6] = _mm256_castps_si256(_mm256_permute2f128_ps(tt2, tt6, 0x31));
  m[7] = _mm256_castps_si256(_mm256_permute2f128_ps(tt3, tt7, 0x31));
}

struct GroupResult {
  bool timed = false;
  unsigned nSamples = 0;
  size_t rawBytes = 0, prodBytes = 0, ourBytes = 0;
  double prodNsPerCh = 0, scalarNsPerCh = 0, simdNsPerCh = 0;
};

// Processes one group of up to 8 files as 8 independent lanes. Always
// computes ratio (cheap); only times decode if `doTiming` is set (timing
// with REPS repeats is the expensive part, skipped for most groups to keep
// total runtime bounded while still covering many files for ratio stats).
GroupResult processGroup(
  const std::vector<std::vector<int32_t>> &voices, bool doTiming) {
  GroupResult g;
  const int n = (int)voices.size();
  g.nSamples = UINT32_MAX;
  for (auto &v : voices)
    g.nSamples = std::min(g.nSamples, (unsigned)v.size());

  // ---- production (real GO codec) ----
  std::vector<std::vector<uint8_t>> prodPacked(n);
  for (int v = 0; v < n; v++) {
    std::vector<uint8_t> &buf = prodPacked[v];
    buf.resize((size_t)g.nSamples * 4 + 16);
    unsigned outLen = 0;
    int value = 0, prev = 0, last = 0;
    for (unsigned i = 0; i < g.nSamples; i++) {
      last = prev;
      prev = value;
      value = voices[v][i];
      const int pred = prev + (prev - last) / 2;
      GOSoundCompressionCache::writeCompressed16(
        buf.data(), outLen, value - pred);
    }
    buf.resize(outLen);
    g.prodBytes += outLen;
  }
  g.rawBytes = (size_t)g.nSamples * n * 3;

  // ---- our codec (independent per-lane block width) ----
  std::vector<std::vector<int32_t>> residuals(n, std::vector<int32_t>(g.nSamples));
  for (int v = 0; v < n; v++) {
    int32_t value = 0, prev = 0, last = 0;
    for (unsigned i = 0; i < g.nSamples; i++) {
      last = prev;
      prev = value;
      value = voices[v][i];
      const int32_t pred = prev + ((prev - last) >> 1);
      residuals[v][i] = value - pred;
    }
  }
  std::vector<EncodedLane> lanes(n);
  for (int v = 0; v < n; v++) {
    lanes[v] = encodeLaneIndependent(residuals[v], g.nSamples);
    g.ourBytes += lanes[v].bytes.size();
  }

  if (!doTiming || n != LANES)
    return g;
  g.timed = true;

  // ---- time production ----
  std::vector<std::vector<int32_t>> prodOut(LANES, std::vector<int32_t>(g.nSamples));
  auto productionDecode = [&] {
    for (int v = 0; v < LANES; v++) {
      GOSoundCompressionCache cache;
      cache.Init();
      cache.m_ptr = prodPacked[v].data();
      for (unsigned i = 0; i < g.nSamples; i++) {
        cache.DecompressionStep(1, true);
        prodOut[v][i] = cache.m_value[0];
      }
    }
  };
  const double prodSec = bestSeconds(REPS, productionDecode);
  g.prodNsPerCh = prodSec / (g.nSamples * (double)LANES) * 1e9;

  // ---- time our scalar ----
  std::vector<std::vector<int32_t>> scalarOut(LANES, std::vector<int32_t>(g.nSamples));
  auto independentScalarDecode = [&] {
    std::vector<const uint8_t *> p(LANES);
    for (int v = 0; v < LANES; v++)
      p[v] = lanes[v].bytes.data();
    int32_t value[LANES] = {}, prev[LANES] = {}, last[LANES] = {};
    unsigned i = 0;
    while (i < g.nSamples) {
      const unsigned n2 = std::min((unsigned)BLOCK_SAMPLES, g.nSamples - i);
      int width[LANES];
      for (int v = 0; v < LANES; v++)
        width[v] = *p[v]++;
      for (unsigned j = 0; j < n2; j++) {
        for (int v = 0; v < LANES; v++) {
          const int32_t residual
            = extractResidual(p[v] + (size_t)j * width[v], width[v]);
          last[v] = prev[v];
          prev[v] = value[v];
          const int32_t pred = prev[v] + ((prev[v] - last[v]) >> 1);
          value[v] = pred + residual;
          scalarOut[v][i + j] = value[v];
        }
      }
      for (int v = 0; v < LANES; v++)
        p[v] += (size_t)n2 * width[v];
      i += n2;
    }
  };
  const double scalarSec = bestSeconds(REPS, independentScalarDecode);
  g.scalarNsPerCh = scalarSec / (g.nSamples * (double)LANES) * 1e9;

  // ---- time our SIMD (planar + transpose) ----
  std::vector<int32_t> flat(g.nSamples * LANES);
  std::vector<int32_t> planar((size_t)LANES * BLOCK_SAMPLES);
  constexpr unsigned CHUNK = 256;
  auto v3Decode = [&] {
    std::vector<const uint8_t *> p(LANES);
    for (int v = 0; v < LANES; v++)
      p[v] = lanes[v].bytes.data();
    __m256i value = _mm256_setzero_si256();
    __m256i prev = _mm256_setzero_si256();
    __m256i last = _mm256_setzero_si256();
    unsigned i = 0;
    while (i < g.nSamples) {
      const unsigned n2 = std::min((unsigned)BLOCK_SAMPLES, g.nSamples - i);
      int width[LANES];
      for (int v = 0; v < LANES; v++)
        width[v] = *p[v]++;
      for (unsigned c = 0; c < n2; c += CHUNK) {
        const unsigned m = std::min(CHUNK, n2 - c);
        for (int v = 0; v < LANES; v++) {
          const uint8_t *ib = p[v] + (size_t)c * width[v];
          int32_t *row = planar.data() + (size_t)v * BLOCK_SAMPLES;
          switch (width[v]) {
          case 1:
            extractLanePlanar<1>(ib, m, row);
            break;
          case 2:
            extractLanePlanar<2>(ib, m, row);
            break;
          case 3:
            extractLanePlanar<3>(ib, m, row);
            break;
          default:
            extractLanePlanar<4>(ib, m, row);
            break;
          }
        }
        int32_t *out = flat.data() + (size_t)(i + c) * LANES;
        const unsigned m8 = m - (m % 8);
        for (unsigned j = 0; j < m8; j += 8) {
          __m256i rows[8];
          for (int v = 0; v < LANES; v++)
            rows[v] = _mm256_loadu_si256((const __m256i *)(
              planar.data() + (size_t)v * BLOCK_SAMPLES + j));
          transpose8x8(rows);
          for (int k = 0; k < 8; k++) {
            last = prev;
            prev = value;
            const __m256i diff = _mm256_sub_epi32(prev, last);
            const __m256i halfDiff = _mm256_srai_epi32(diff, 1);
            const __m256i pred = _mm256_add_epi32(prev, halfDiff);
            value = _mm256_add_epi32(pred, rows[k]);
            _mm256_storeu_si256((__m256i *)(out + (j + k) * LANES), value);
          }
        }
        for (unsigned j = m8; j < m; j++) {
          alignas(32) int32_t tmp[LANES];
          for (int v = 0; v < LANES; v++)
            tmp[v] = planar[(size_t)v * BLOCK_SAMPLES + j];
          const __m256i residual = _mm256_load_si256((const __m256i *)tmp);
          last = prev;
          prev = value;
          const __m256i diff = _mm256_sub_epi32(prev, last);
          const __m256i halfDiff = _mm256_srai_epi32(diff, 1);
          const __m256i pred = _mm256_add_epi32(prev, halfDiff);
          value = _mm256_add_epi32(pred, residual);
          _mm256_storeu_si256((__m256i *)(out + j * LANES), value);
        }
      }
      for (int v = 0; v < LANES; v++)
        p[v] += (size_t)n2 * width[v];
      i += n2;
    }
  };
  const double v3Sec = bestSeconds(REPS, v3Decode);
  g.simdNsPerCh = v3Sec / (g.nSamples * (double)LANES) * 1e9;

  // spot-check correctness on this group (cheap relative to timing already done)
  bool ok = true;
  for (int v = 0; v < LANES && ok; v++)
    for (unsigned i = 0; i < g.nSamples && ok; i += 997) // sparse check, fast
      if (prodOut[v][i] != voices[v][i] || scalarOut[v][i] != voices[v][i]
          || flat[(size_t)i * LANES + v] != voices[v][i])
        ok = false;
  if (!ok)
    fprintf(stderr, "WARNING: roundtrip mismatch in a group!\n");

  return g;
}

} // namespace

int main(int argc, char **argv) {
  std::string listFile;
  unsigned maxGroups = 200;   // groups actually processed for ratio
  unsigned timeEveryNth = 5;  // only time 1 in N groups (timing is the slow part)
  unsigned stride = 1;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-l" && i + 1 < argc)
      listFile = argv[++i];
    else if (a == "--max-groups" && i + 1 < argc)
      maxGroups = (unsigned)atoi(argv[++i]);
    else if (a == "--time-every" && i + 1 < argc)
      timeEveryNth = (unsigned)atoi(argv[++i]);
    else if (a == "--stride" && i + 1 < argc)
      stride = (unsigned)atoi(argv[++i]);
  }
  if (listFile.empty()) {
    printf(
      "usage: %s -l <file_list.txt> [--max-groups N] [--time-every N] "
      "[--stride S]\n",
      argv[0]);
    return 1;
  }

  std::vector<std::string> allPaths;
  {
    std::ifstream lf(listFile);
    std::string line;
    while (std::getline(lf, line))
      if (!line.empty())
        allPaths.push_back(line);
  }
  if (stride > 1) {
    std::vector<std::string> strided;
    for (size_t i = 0; i < allPaths.size(); i += stride)
      strided.push_back(allPaths[i]);
    allPaths = strided;
  }
  printf(
    "%zu candidate files after stride, targeting %u groups of %d\n",
    allPaths.size(),
    maxGroups,
    LANES);

  size_t rawTot = 0, prodTot = 0, ourTot = 0;
  unsigned nGroupsDone = 0, nTimedGroups = 0, nFilesSkipped = 0;
  double prodTimeSum = 0, scalarTimeSum = 0, simdTimeSum = 0;

  struct Worst {
    double delta; // ourRatio - prodRatio (negative = we are worse)
    std::string files;
  };
  std::vector<Worst> worstList;

  size_t pathIdx = 0;
  while (nGroupsDone < maxGroups && pathIdx < allPaths.size()) {
    std::vector<std::vector<int32_t>> voices;
    std::vector<std::string> usedPaths;
    while (voices.size() < LANES && pathIdx < allPaths.size()) {
      auto samples = loadWav24Mono(allPaths[pathIdx]);
      if (samples.size() >= MIN_SAMPLES) {
        voices.push_back(std::move(samples));
        usedPaths.push_back(allPaths[pathIdx]);
      } else {
        nFilesSkipped++;
      }
      pathIdx++;
    }
    if (voices.size() < LANES)
      break; // ran out of files for a full group

    const bool doTiming = (nGroupsDone % timeEveryNth) == 0;
    GroupResult g = processGroup(voices, doTiming);
    rawTot += g.rawBytes;
    prodTot += g.prodBytes;
    ourTot += g.ourBytes;

    const double prodRatio = (double)g.rawBytes / g.prodBytes;
    const double ourRatio = (double)g.rawBytes / g.ourBytes;
    std::string joined;
    for (auto &s : usedPaths) {
      auto pos = s.find_last_of('/');
      joined += (pos == std::string::npos ? s : s.substr(pos + 1)) + " | ";
    }
    worstList.push_back({ourRatio - prodRatio, joined});

    if (g.timed) {
      prodTimeSum += g.prodNsPerCh;
      scalarTimeSum += g.scalarNsPerCh;
      simdTimeSum += g.simdNsPerCh;
      nTimedGroups++;
    }

    nGroupsDone++;
    if (nGroupsDone % 20 == 0)
      fprintf(
        stderr, "... %u/%u groups done\n", nGroupsDone, maxGroups);
  }

  printf(
    "\n%u groups processed (%u files), %u skipped (too short/bad format)\n",
    nGroupsDone,
    nGroupsDone * LANES,
    nFilesSkipped);
  printf(
    "\nAGGREGATE RATIO (over all %u groups, %zu raw bytes):\n"
    "  production: %.4fx\n"
    "  our codec:  %.4fx\n",
    nGroupsDone,
    rawTot,
    (double)rawTot / prodTot,
    (double)rawTot / ourTot);

  if (nTimedGroups > 0)
    printf(
      "\nAGGREGATE SPEED (avg over %u timed groups, ns/channel-sample):\n"
      "  production:              %.3f ns\n"
      "  our codec, scalar:       %.3f ns  (%.2fx vs production)\n"
      "  our codec, SIMD:         %.3f ns  (%.2fx vs production, %.2fx vs "
      "our scalar)\n",
      nTimedGroups,
      prodTimeSum / nTimedGroups,
      scalarTimeSum / nTimedGroups,
      (prodTimeSum / nTimedGroups) / (scalarTimeSum / nTimedGroups),
      simdTimeSum / nTimedGroups,
      (prodTimeSum / nTimedGroups) / (simdTimeSum / nTimedGroups),
      (scalarTimeSum / nTimedGroups) / (simdTimeSum / nTimedGroups));

  std::sort(
    worstList.begin(), worstList.end(),
    [](const Worst &a, const Worst &b) { return a.delta < b.delta; });
  printf(
    "\nWORST 10 GROUPS (our ratio - production ratio, most negative = we "
    "lose most):\n");
  for (unsigned i = 0; i < std::min((size_t)10, worstList.size()); i++)
    printf("  %+.4f  %s\n", worstList[i].delta, worstList[i].files.c_str());

  unsigned weWin = 0, weLose = 0;
  for (auto &w : worstList)
    (w.delta >= 0 ? weWin : weLose)++;
  printf(
    "\nOf %zu groups: our codec had the better ratio in %u, production in "
    "%u.\n",
    worstList.size(),
    weWin,
    weLose);

  return 0;
}
