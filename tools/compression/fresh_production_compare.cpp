// fresh_production_compare.cpp
//
// Direct, same-run, same-files comparison against GO's REAL, UNMODIFIED
// production decoder (GOSoundCompressionCache.h, copied verbatim below) -
// to settle a legitimate worry: earlier numbers for "production" and for
// our new codec came from different test runs at different points in the
// session, which is exactly how the earlier stereo-frame-vs-mono-channel
// mixup happened. This harness measures all three paths in ONE binary, on
// the SAME 8 files, all normalized to ns/channel-sample, so there is no
// room for a unit mismatch this time:
//
//   production   - GO's actual DecompressionStep(), one instance per lane
//                  (channels=1), exactly how 8 simultaneously playing mono
//                  voices are decoded today, one sample at a time.
//   independent scalar - our new codec, honestly independent per-lane
//                  widths, no batching trick (fair baseline for our codec).
//   planar+transpose (v3) - our SIMD decode from the last measurement.
//
// Build:
//   g++ -O3 -mavx2 -ffast-math -funroll-loops -std=gnu++17 \
//     -I /home/vboxuser/grandorgue/src/grandorgue \
//     fresh_production_compare.cpp -o fresh_production_compare

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
constexpr unsigned REPS = 9;
constexpr unsigned BLOCK_SAMPLES = 1024;
constexpr int LANES = 8;

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

std::vector<int32_t> loadWavMono(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  char riff[4];
  f.read(riff, 4);
  f.seekg(4, std::ios::cur);
  char wave[4];
  f.read(wave, 4);
  unsigned channels = 0, bitsPerSample = 0;
  std::vector<unsigned char> data;
  while (f) {
    char id[4];
    f.read(id, 4);
    if (!f)
      break;
    uint32_t chunkSize = 0;
    f.read((char *)&chunkSize, 4);
    if (memcmp(id, "fmt ", 4) == 0) {
      uint16_t audioFormat, nChannels, blockAlign, bps;
      uint32_t sr, byteRate;
      f.read((char *)&audioFormat, 2);
      f.read((char *)&nChannels, 2);
      f.read((char *)&sr, 4);
      f.read((char *)&byteRate, 4);
      f.read((char *)&blockAlign, 2);
      f.read((char *)&bps, 2);
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
  const unsigned bytesPerSample = bitsPerSample / 8;
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
  for (unsigned j = 0; j < m; j++) {
    const uint8_t *__restrict b = ib + (size_t)j * WIDTH;
    int32_t val;
    if constexpr (WIDTH == 1) {
      val = (int32_t)(int8_t)b[0];
    } else if constexpr (WIDTH == 2) {
      int16_t v;
      memcpy(&v, b, 2);
      val = v;
    } else if constexpr (WIDTH == 3) {
      const uint32_t raw
        = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
      val = (int32_t)(raw << 8) >> 8;
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

} // namespace

int main(int argc, char **argv) {
  if (argc < 1 + LANES) {
    printf("usage: %s <8 wav files>\n", argv[0]);
    return 1;
  }

  std::vector<std::vector<int32_t>> voices(LANES);
  unsigned nSamples = UINT32_MAX;
  for (int v = 0; v < LANES; v++) {
    voices[v] = loadWavMono(argv[v + 1]);
    nSamples = std::min(nSamples, (unsigned)voices[v].size());
  }
  printf("%u samples/lane, %d lanes\n\n", nSamples, LANES);

  // ================= PRODUCTION (real GO codec, unmodified) =================
  // GO's own predictor: pred = prev + (prev-last)/2 (truncating /2, not >>1).
  // writeCompressed16, one independent GOSoundCompressionCache per lane -
  // exactly how 8 simultaneous mono voices are decoded today (channels=1
  // each, called once per sample in the real hot loop).
  std::vector<std::vector<uint8_t>> prodPacked(LANES);
  for (int v = 0; v < LANES; v++) {
    std::vector<uint8_t> &buf = prodPacked[v];
    buf.resize((size_t)nSamples * 4 + 16); // worst case
    unsigned outLen = 0;
    int value = 0, prev = 0, last = 0;
    for (unsigned i = 0; i < nSamples; i++) {
      last = prev;
      prev = value;
      value = voices[v][i];
      const int pred = prev + (prev - last) / 2;
      const int encode = value - pred;
      GOSoundCompressionCache::writeCompressed16(
        buf.data(), outLen, encode);
    }
    buf.resize(outLen);
  }
  size_t prodBytes = 0;
  for (auto &b : prodPacked)
    prodBytes += b.size();
  printf(
    "production packed: %zu B total (%.3fx vs raw 24-bit)\n",
    prodBytes,
    (double)nSamples * LANES * 3 / prodBytes);

  std::vector<std::vector<int32_t>> prodOut(
    LANES, std::vector<int32_t>(nSamples));
  auto productionDecode = [&] {
    for (int v = 0; v < LANES; v++) {
      GOSoundCompressionCache cache;
      cache.Init();
      cache.m_ptr = prodPacked[v].data();
      for (unsigned i = 0; i < nSamples; i++) {
        cache.DecompressionStep(1, true); // channels=1, format16
        prodOut[v][i] = cache.m_value[0];
      }
    }
  };
  productionDecode();
  bool prodOk = true;
  for (int v = 0; v < LANES && prodOk; v++)
    for (unsigned i = 0; i < nSamples && prodOk; i++)
      if (prodOut[v][i] != voices[v][i])
        prodOk = false;
  const double prodSec = bestSeconds(REPS, productionDecode);
  const double prodNsPerChannel = prodSec / (nSamples * (double)LANES) * 1e9;
  printf(
    "production (real GOSoundCompressionCache, %d independent mono "
    "instances): %.3f ns/channel-sample (roundtrip %s)\n\n",
    LANES,
    prodNsPerChannel,
    prodOk ? "OK" : "MISMATCH!");

  // ================= OUR CODEC: independent scalar baseline =================
  std::vector<std::vector<int32_t>> residuals(
    LANES, std::vector<int32_t>(nSamples));
  for (int v = 0; v < LANES; v++) {
    int32_t value = 0, prev = 0, last = 0;
    for (unsigned i = 0; i < nSamples; i++) {
      last = prev;
      prev = value;
      value = voices[v][i];
      const int32_t pred = prev + ((prev - last) >> 1);
      residuals[v][i] = value - pred;
    }
  }
  std::vector<EncodedLane> lanes(LANES);
  for (int v = 0; v < LANES; v++)
    lanes[v] = encodeLaneIndependent(residuals[v], nSamples);
  size_t ourBytes = 0;
  for (auto &l : lanes)
    ourBytes += l.bytes.size();
  printf(
    "our codec packed: %zu B total (%.3fx vs raw 24-bit)  [production was "
    "%.3fx on the SAME files]\n",
    ourBytes,
    (double)nSamples * LANES * 3 / ourBytes,
    (double)nSamples * LANES * 3 / prodBytes);

  std::vector<std::vector<int32_t>> scalarOut(
    LANES, std::vector<int32_t>(nSamples));
  auto independentScalarDecode = [&] {
    std::vector<const uint8_t *> p(LANES);
    for (int v = 0; v < LANES; v++)
      p[v] = lanes[v].bytes.data();
    int32_t value[LANES] = {}, prev[LANES] = {}, last[LANES] = {};
    unsigned i = 0;
    while (i < nSamples) {
      const unsigned n = std::min((unsigned)BLOCK_SAMPLES, nSamples - i);
      int width[LANES];
      for (int v = 0; v < LANES; v++)
        width[v] = *p[v]++;
      for (unsigned j = 0; j < n; j++) {
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
        p[v] += (size_t)n * width[v];
      i += n;
    }
  };
  independentScalarDecode();
  bool scalarOk = true;
  for (int v = 0; v < LANES && scalarOk; v++)
    for (unsigned i = 0; i < nSamples && scalarOk; i++)
      if (scalarOut[v][i] != voices[v][i])
        scalarOk = false;
  const double scalarSec = bestSeconds(REPS, independentScalarDecode);
  const double scalarNsPerChannel
    = scalarSec / (nSamples * (double)LANES) * 1e9;
  printf(
    "our codec, independent scalar (fair, no batching trick): %.3f "
    "ns/channel-sample (roundtrip %s)\n",
    scalarNsPerChannel,
    scalarOk ? "OK" : "MISMATCH!");

  // ================= OUR CODEC: planar + AVX2 8x8 transpose (v3) =================
  std::vector<int32_t> flat(nSamples * LANES);
  std::vector<int32_t> planar((size_t)LANES * BLOCK_SAMPLES);
  constexpr unsigned CHUNK = 256; // flat-plateau chunk size found earlier
  auto v3Decode = [&] {
    std::vector<const uint8_t *> p(LANES);
    for (int v = 0; v < LANES; v++)
      p[v] = lanes[v].bytes.data();
    __m256i value = _mm256_setzero_si256();
    __m256i prev = _mm256_setzero_si256();
    __m256i last = _mm256_setzero_si256();
    unsigned i = 0;
    while (i < nSamples) {
      const unsigned n = std::min((unsigned)BLOCK_SAMPLES, nSamples - i);
      int width[LANES];
      for (int v = 0; v < LANES; v++)
        width[v] = *p[v]++;
      for (unsigned c = 0; c < n; c += CHUNK) {
        const unsigned m = std::min(CHUNK, n - c);
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
        p[v] += (size_t)n * width[v];
      i += n;
    }
  };
  v3Decode();
  bool v3Ok = true;
  for (unsigned i = 0; i < nSamples && v3Ok; i++)
    for (int v = 0; v < LANES && v3Ok; v++)
      if (flat[i * LANES + v] != voices[v][i])
        v3Ok = false;
  const double v3Sec = bestSeconds(REPS, v3Decode);
  const double v3NsPerChannel = v3Sec / (nSamples * (double)LANES) * 1e9;
  printf(
    "our codec, planar+AVX2-transpose (SIMD): %.3f ns/channel-sample "
    "(roundtrip %s)\n\n",
    v3NsPerChannel,
    v3Ok ? "OK" : "MISMATCH!");

  printf(
    "SUMMARY (all ns/channel-sample, same 8 files, same run):\n"
    "  production (today's real GO codec):     %.3f ns\n"
    "  our codec, independent scalar:          %.3f ns  (%.2fx vs "
    "production)\n"
    "  our codec, planar+AVX2-transpose:       %.3f ns  (%.2fx vs "
    "production, %.2fx vs our own scalar)\n",
    prodNsPerChannel,
    scalarNsPerChannel,
    prodNsPerChannel / scalarNsPerChannel,
    v3NsPerChannel,
    prodNsPerChannel / v3NsPerChannel,
    scalarNsPerChannel / v3NsPerChannel);

  return 0;
}
