// scalar_codec_duel.cpp
//
// User question (2026-07-12): shouldn't V3's SCALAR decode always beat the
// legacy decoder, because it is not branchy? Legacy decodes per-sample
// variable-length codes (data-dependent flag-bit branches, prediction
// quality depends on the material); V3 decides the byte width ONCE per
// 1024-sample block and then runs a branchless fixed-width loop.
//
// Duel: production GOSoundCompressionCache (24-bit fmt16 codes) vs
// production GOSoundCompressionCacheV3, both pure scalar DecompressionStep
// chains, on three stereo materials:
//   smooth   - quiet flute-like signal, tiny residuals (branch-friendly)
//   loud     - bright loud mixture (the Rotterdam case, mixed residuals)
//   hostile  - noise-heavy, residuals straddling the code-length
//              boundaries (branch-predictor poison for legacy)
//
// Build:
//   g++ -O3 -std=gnu++17 -I ../../src/grandorgue  \
//     -I ../../src/v3codec scalar_codec_duel.cpp \
//       -o /tmp/claude-1000/duel

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "sound/playing/GOSoundCompressionCache.h"
#include "GOSoundCompressionCacheV3.h"

using Clock = std::chrono::steady_clock;
constexpr unsigned FRAMES = 400000;
constexpr unsigned REPS = 9;
volatile int g_sink;

static double bestSeconds(unsigned reps, auto &&fn) {
  double best = 1e30;
  for (unsigned r = 0; r < reps; r++) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
  }
  return best;
}

// Legacy encode (mirrors GOSoundAudioSection::Compress, fmt16 = 24 bit).
static std::vector<unsigned char> encodeLegacy(
  const std::vector<int32_t> &src) {
  const unsigned nFrames = (unsigned)src.size() / 2;
  std::vector<unsigned char> out(src.size() * 4 + 16);
  unsigned len = 0;
  GOSoundCompressionCache st;

  st.Init();
  for (unsigned i = 0; i < nFrames; i++) {
    st.m_last[0] = st.m_prev[0];
    st.m_last[1] = st.m_prev[1];
    st.m_prev[0] = st.m_value[0];
    st.m_prev[1] = st.m_value[1];
    st.m_value[0] = src[i * 2];
    st.m_value[1] = src[i * 2 + 1];
    for (unsigned j = 0; j < 2; j++) {
      const int ev = st.m_value[j]
        - (st.m_prev[j] + (st.m_prev[j] - st.m_last[j]) / 2);
      GOSoundCompressionCache::writeCompressed16(out.data(), len, ev);
    }
  }
  out.resize(len);
  return out;
}

int main() {
  std::mt19937 rng(4711);
  std::normal_distribution<float> gauss(0.f, 1.f);

  struct Mat {
    const char *name;
    std::vector<int32_t> pcm;
  } mats[3];

  for (int m = 0; m < 3; m++) {
    mats[m].pcm.resize((size_t)FRAMES * 2);
    double lp = 0;
    for (unsigned i = 0; i < FRAMES; i++)
      for (unsigned c = 0; c < 2; c++) {
        const double ph = i * 0.0184 + c * 0.7;
        double v;
        if (m == 0) { // smooth
          v = 120000.0 * (std::sin(ph) + 0.2 * std::sin(2.0 * ph));
        } else if (m == 1) { // loud/bright
          v = 2500000.0
            * (std::sin(ph) + 0.7 * std::sin(5.02 * ph)
               + 0.5 * std::sin(11.05 * ph) + 0.3 * std::sin(17.1 * ph));
        } else { // hostile: filtered noise, residuals straddle boundaries
          lp = 0.86 * lp + 90000.0 * gauss(rng);
          v = lp + 300000.0 * std::sin(ph);
        }
        mats[m].pcm[(size_t)i * 2 + c] = (int32_t)v;
      }
    mats[m].name = m == 0 ? "smooth " : (m == 1 ? "loud   " : "hostile");
  }

  printf("scalar decode duel, %u stereo frames, ns/frame (beide Kanaele)\n",
    FRAMES);
  printf("material | legacy 24-bit | V3 scalar | V3/legacy | Bytes L/V3\n");
  for (auto &mt : mats) {
    const auto encL = encodeLegacy(mt.pcm);

    std::vector<unsigned char> encV(
      GOSoundCompressionCacheV3::WorstCaseSize(FRAMES, 2));
    GOSoundCompressionCacheV3 enc;
    const unsigned wr = enc.Encode(
      encV.data(), FRAMES, 2, nullptr, 0,
      [&](unsigned idx, unsigned ch) {
        return (int)mt.pcm[(size_t)idx * 2 + ch];
      },
      nullptr);
    encV.resize(wr);

    const double tL = bestSeconds(REPS, [&] {
      GOSoundCompressionCache c;
      c.Init();
      c.m_ptr = encL.data();
      for (unsigned i = 0; i < FRAMES; i++)
        c.DecompressionStep(2, true);
      g_sink = c.m_value[0];
    });
    const double tV = bestSeconds(REPS, [&] {
      GOSoundCompressionCacheV3 c;
      c.Init();
      c.DecompressTo(0, encV.data(), 2);
      for (unsigned i = 1; i < FRAMES; i++)
        c.DecompressionStep(2);
      g_sink = c.m_value[0];
    });
    printf(
      " %s | %10.2f    | %8.2f  |   %.2fx   | %zu / %u\n",
      mt.name,
      tL / FRAMES * 1e9,
      tV / FRAMES * 1e9,
      tV / tL,
      encL.size(),
      wr);
  }
  return 0;
}
