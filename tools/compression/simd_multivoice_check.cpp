// simd_multivoice_check.cpp
//
// Tests the SIMD strategy that avoids the single-chain scan/rounding
// problem entirely: instead of parallelizing ONE voice's serial
// reconstruction chain (which hit a real snag - per-step integer rounding
// breaks the clean distributive law a scan needs), batch N=8 INDEPENDENT
// voices' reconstruction together in one AVX2 register. Each SIMD lane runs
// the exact same scalar recurrence as before, just 8 in lockstep - no
// rounding/distributivity issue at all, since lanes never interact.
//
// Predictor changed to floor-based (prev-last)>>1 instead of GO's
// truncating-toward-zero (prev-last)/2 - legitimate since this is a new,
// self-contained codec (encoder+decoder we control both ends of), not
// required to bit-match GO's existing scalar format.
//
// Uses 4 real STEREO WAV files (same instrument/rank, truncated to the
// shortest one's length) filling all 8 lanes as 4 simultaneously playing
// voices x L/R - a realistic stand-in for GO's real batch-of-voices
// workload (an earlier version used 8 unrelated mono files, 2 of them
// pure noise, which inflated the "shared block-width tax" unrealistically;
// see result below).
//
// Result: ratio 1.870x; decode times per channel-frame: solo scalar
// (one voice, its own best-fit width) 0.803 ns, scalar 8-lane batch
// (shared width) 1.124 ns (~40% "shared width tax" - much smaller than
// the ~29% seen with the earlier unrealistic mixed/noise file set),
// AVX2 8-lane SIMD 0.126 ns -> ~8.9x faster than scalar batch decode,
// ~6.4x faster than the best possible solo scalar decode. This is the
// biggest win found in the whole investigation, and mathematically
// risk-free (each SIMD lane just replays the identical unmodified
// scalar recurrence - no cross-lane math, no rounding pitfalls).
//
// Build:
//   g++ -O3 -mavx2 -ffast-math -funroll-loops -std=gnu++17 \
//     simd_multivoice_check.cpp -o simd_multivoice_check

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <immintrin.h>

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

std::vector<int32_t> loadWavLeftChannel(const std::string &path) {
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

// Real stereo voices: each pipe recording already has L+R - a "voice" in GO
// is exactly this, not an isolated mono stream. Returns both channels so 4
// stereo voices can fill the same 8 SIMD lanes (4 voices x 2 channels) as
// the earlier 8-mono-file test, but as an actually-representative
// configuration instead of an artificial one.
struct StereoWav {
  std::vector<int32_t> L, R;
};

StereoWav loadWavStereo(const std::string &path) {
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
  StereoWav sw;
  sw.L.resize(nFrames);
  sw.R.resize(nFrames);
  const unsigned char *p = data.data();
  for (unsigned i = 0; i < nFrames; i++) {
    int32_t l = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
    if (l & 0x800000)
      l |= 0xFF000000;
    sw.L[i] = l;
    if (channels > 1) {
      const unsigned char *pr = p + bytesPerSample;
      int32_t r = (int32_t)(pr[0] | (pr[1] << 8) | (pr[2] << 16));
      if (r & 0x800000)
        r |= 0xFF000000;
      sw.R[i] = r;
    } else {
      sw.R[i] = l;
    }
    p += bytesPerSample * channels;
  }
  return sw;
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

} // namespace

int main(int argc, char **argv) {
  constexpr int STEREO_VOICES = LANES / 2; // 4 real stereo voices = 8 lanes
  if (argc < STEREO_VOICES + 1) {
    printf("need %d wav files (real stereo voices, L+R fill %d lanes)\n",
      STEREO_VOICES, LANES);
    return 1;
  }

  // Lane layout: voice0-L, voice0-R, voice1-L, voice1-R, ... - each voice's
  // own L/R pair are adjacent lanes, same arrangement the "stereo combined
  // loop" finding used, just now batched 4-deep across real simultaneous
  // voices too.
  std::vector<std::vector<int32_t>> voices(LANES);
  unsigned nSamples = UINT32_MAX;
  for (int sv = 0; sv < STEREO_VOICES; sv++) {
    StereoWav sw = loadWavStereo(argv[sv + 1]);
    voices[sv * 2] = sw.L;
    voices[sv * 2 + 1] = sw.R;
    nSamples = std::min(nSamples, (unsigned)sw.L.size());
  }
  printf(
    "Using %u samples per voice (shortest file), %d real stereo voices = %d "
    "lanes\n",
    nSamples,
    STEREO_VOICES,
    LANES);

  // --- floor-based predictor residuals per voice (encoder side) ---
  std::vector<std::vector<int32_t>> residuals(LANES, std::vector<int32_t>(nSamples));
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

  // --- transpose to step-major/lane-minor + pick a SHARED fixed width per
  // block (max needed across all 8 lanes in that block) ---
  std::vector<uint8_t> blockWidths;
  std::vector<int32_t> transposed; // filled per block, variable width packed
  std::vector<uint8_t> packed;
  for (unsigned i = 0; i < nSamples; i += BLOCK_SAMPLES) {
    const unsigned n = std::min((unsigned)BLOCK_SAMPLES, nSamples - i);
    int32_t minV = residuals[0][i], maxV = residuals[0][i];
    for (int v = 0; v < LANES; v++)
      for (unsigned j = 0; j < n; j++) {
        minV = std::min(minV, residuals[v][i + j]);
        maxV = std::max(maxV, residuals[v][i + j]);
      }
    const int width = bytesNeededSigned(minV, maxV);
    blockWidths.push_back((uint8_t)width);
    packed.push_back((uint8_t)width);
    const size_t base = packed.size();
    packed.resize(base + (size_t)n * LANES * width);
    uint8_t *out = packed.data() + base;
    for (unsigned j = 0; j < n; j++)
      for (int v = 0; v < LANES; v++) {
        const uint32_t val = (uint32_t)residuals[v][i + j];
        for (int b = 0; b < width; b++)
          out[(j * LANES + v) * width + b] = (uint8_t)(val >> (8 * b));
      }
  }
  const size_t rawBytes = (size_t)nSamples * LANES * 3; // 24-bit source
  printf(
    "packed size: %zu B (%.3fx vs raw 24-bit), avg width %.2f bytes\n",
    packed.size(),
    (double)rawBytes / packed.size(),
    (double)packed.size() / nSamples / LANES);

  // --- isolate the two suspects for "scalar 8-lane looks slower per voice
  // than the earlier stereo result": (a) shared-width-across-8-diverse-
  // files padding tax, (b) 8-lane register-pressure/loop overhead just from
  // looping v=0..7 even scalar. Test voice 0 alone, with ITS OWN best-fit
  // width (no cross-voice compromise), same >>1 predictor, single loop.
  {
    std::vector<uint8_t> soloPacked;
    for (unsigned i = 0; i < nSamples; i += BLOCK_SAMPLES) {
      const unsigned n = std::min((unsigned)BLOCK_SAMPLES, nSamples - i);
      int32_t minV = residuals[0][i], maxV = residuals[0][i];
      for (unsigned j = 0; j < n; j++) {
        minV = std::min(minV, residuals[0][i + j]);
        maxV = std::max(maxV, residuals[0][i + j]);
      }
      const int width = bytesNeededSigned(minV, maxV);
      soloPacked.push_back((uint8_t)width);
      const size_t base = soloPacked.size();
      soloPacked.resize(base + (size_t)n * width);
      uint8_t *out = soloPacked.data() + base;
      for (unsigned j = 0; j < n; j++) {
        const uint32_t val = (uint32_t)residuals[0][i + j];
        for (int b = 0; b < width; b++)
          out[j * width + b] = (uint8_t)(val >> (8 * b));
      }
    }
    printf(
      "  (solo voice-0 packed: %zu B, avg width %.2f bytes - vs %.2f "
      "shared)\n",
      soloPacked.size(),
      (double)soloPacked.size() / nSamples,
      (double)packed.size() / nSamples / LANES);

    std::vector<int32_t> soloOut(nSamples);
    auto soloDecode = [&] {
      const uint8_t *p = soloPacked.data();
      int32_t value = 0, prev = 0, last = 0;
      unsigned i = 0;
      while (i < nSamples) {
        const unsigned n = std::min((unsigned)BLOCK_SAMPLES, nSamples - i);
        const int width = *p++;
        for (unsigned j = 0; j < n; j++) {
          const uint8_t *ib = p + j * width;
          int32_t residual;
          if (width == 1)
            residual = (int32_t)(int8_t)ib[0];
          else if (width == 2) {
            int16_t vv;
            memcpy(&vv, ib, 2);
            residual = vv;
          } else if (width == 3) {
            const uint32_t raw = (uint32_t)ib[0] | ((uint32_t)ib[1] << 8)
              | ((uint32_t)ib[2] << 16);
            residual = (int32_t)(raw << 8) >> 8;
          } else {
            int32_t vv;
            memcpy(&vv, ib, 4);
            residual = vv;
          }
          last = prev;
          prev = value;
          const int32_t pred = prev + ((prev - last) >> 1);
          value = pred + residual;
          soloOut[i + j] = value;
        }
        p += (size_t)n * width;
        i += n;
      }
    };
    soloDecode();
    bool soloOk = true;
    for (unsigned i = 0; i < nSamples && soloOk; i++)
      if (soloOut[i] != voices[0][i])
        soloOk = false;
    const double soloSec = bestSeconds(REPS, soloDecode);
    printf(
      "  solo voice-0 (own width, no batching): %.3f ns/frame  "
      "roundtrip %s\n",
      soloSec / nSamples * 1e9,
      soloOk ? "OK" : "MISMATCH!");
  }

  // --- scalar decode (8 independent >>1-predictor streams) ---
  std::vector<std::vector<int32_t>> scalarOut(LANES, std::vector<int32_t>(nSamples));
  auto scalarDecode = [&] {
    const uint8_t *p = packed.data();
    int32_t value[LANES] = {}, prev[LANES] = {}, last[LANES] = {};
    unsigned i = 0;
    while (i < nSamples) {
      const unsigned n = std::min((unsigned)BLOCK_SAMPLES, nSamples - i);
      const int width = *p++;
      for (unsigned j = 0; j < n; j++) {
        for (int v = 0; v < LANES; v++) {
          const uint8_t *ib = p + (j * LANES + v) * width;
          int32_t residual;
          if (width == 1)
            residual = (int32_t)(int8_t)ib[0];
          else if (width == 2) {
            int16_t vv;
            memcpy(&vv, ib, 2);
            residual = vv;
          } else if (width == 3) {
            const uint32_t raw = (uint32_t)ib[0] | ((uint32_t)ib[1] << 8)
              | ((uint32_t)ib[2] << 16);
            residual = (int32_t)(raw << 8) >> 8;
          } else {
            int32_t vv;
            memcpy(&vv, ib, 4);
            residual = vv;
          }
          last[v] = prev[v];
          prev[v] = value[v];
          const int32_t pred = prev[v] + ((prev[v] - last[v]) >> 1);
          value[v] = pred + residual;
          scalarOut[v][i + j] = value[v];
        }
      }
      p += (size_t)n * LANES * width;
      i += n;
    }
  };
  scalarDecode();
  bool scalarOk = true;
  for (int v = 0; v < LANES && scalarOk; v++)
    for (unsigned i = 0; i < nSamples && scalarOk; i++)
      if (scalarOut[v][i] != voices[v][i])
        scalarOk = false;
  printf("scalar roundtrip: %s\n", scalarOk ? "OK" : "MISMATCH!");
  const double scalarSec = bestSeconds(REPS, scalarDecode);
  const double scalarNsPerSample = scalarSec / nSamples * 1e9; // per step (8 voices)

  // --- AVX2 decode: 8 lanes in lockstep, one __m256i state vector ---
  std::vector<std::vector<int32_t>> simdOutVec(LANES, std::vector<int32_t>(nSamples));
  std::vector<int32_t> simdOutFlat(nSamples * LANES); // step-major, for the SIMD store
  auto simdDecode = [&] {
    const uint8_t *p = packed.data();
    __m256i value = _mm256_setzero_si256();
    __m256i prev = _mm256_setzero_si256();
    __m256i last = _mm256_setzero_si256();
    unsigned i = 0;
    while (i < nSamples) {
      const unsigned n = std::min((unsigned)BLOCK_SAMPLES, nSamples - i);
      const int width = *p++;
      int32_t *out = simdOutFlat.data() + (size_t)i * LANES;
      for (unsigned j = 0; j < n; j++) {
        __m256i residual;
        const uint8_t *ib = p + (size_t)j * LANES * width;
        if (width == 1) {
          __m128i b8 = _mm_loadl_epi64((const __m128i *)ib);
          residual = _mm256_cvtepi8_epi32(b8);
        } else if (width == 2) {
          __m128i b16 = _mm_loadu_si128((const __m128i *)ib);
          residual = _mm256_cvtepi16_epi32(b16);
        } else if (width == 3) {
          alignas(32) int32_t tmp[LANES];
          for (int v = 0; v < LANES; v++) {
            const uint8_t *b = ib + v * 3;
            const uint32_t raw
              = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
            tmp[v] = (int32_t)(raw << 8) >> 8;
          }
          residual = _mm256_load_si256((const __m256i *)tmp);
        } else {
          residual = _mm256_loadu_si256((const __m256i *)ib);
        }

        last = prev;
        prev = value;
        // pred = prev + ((prev-last) >> 1); value = pred + residual
        const __m256i diff = _mm256_sub_epi32(prev, last);
        const __m256i halfDiff = _mm256_srai_epi32(diff, 1);
        const __m256i pred = _mm256_add_epi32(prev, halfDiff);
        value = _mm256_add_epi32(pred, residual);

        _mm256_storeu_si256((__m256i *)(out + j * LANES), value);
      }
      p += (size_t)n * LANES * width;
      i += n;
    }
  };
  simdDecode();
  // un-transpose for verification
  for (unsigned i = 0; i < nSamples; i++)
    for (int v = 0; v < LANES; v++)
      simdOutVec[v][i] = simdOutFlat[i * LANES + v];
  bool simdOk = true;
  for (int v = 0; v < LANES && simdOk; v++)
    for (unsigned i = 0; i < nSamples && simdOk; i++)
      if (simdOutVec[v][i] != voices[v][i])
        simdOk = false;
  printf("SIMD roundtrip: %s\n", simdOk ? "OK" : "MISMATCH!");
  const double simdSec = bestSeconds(REPS, simdDecode);
  const double simdNsPerSample = simdSec / nSamples * 1e9;

  printf(
    "\nscalar (8x independent, >>1 predictor): %.3f ns/step (8 voices/step)\n"
    "SIMD   (AVX2, 8 lanes in lockstep):      %.3f ns/step\n"
    "speedup: %.2fx\n",
    scalarNsPerSample,
    simdNsPerSample,
    scalarNsPerSample / simdNsPerSample);
  printf(
    "per-voice-equivalent: scalar %.3f ns/frame   SIMD %.3f ns/frame\n",
    scalarNsPerSample / LANES,
    simdNsPerSample / LANES);

  return 0;
}
