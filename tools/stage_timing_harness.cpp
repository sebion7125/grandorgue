// stage_timing_harness.cpp
//
// Measures the per-stage compute cost of GrandOrgue's voice hotpath using the
// real production code where it is header-reachable without wx:
//
//   D  compressed delta-stream decode      GOSoundCompressionCache (production)
//   D+ decode + planar window store        (what the ReadAhead refill does)
//   R  polyphase 8-tap FIR resample        GOSoundResample kernels (production),
//                                          AVX2-planar vs scalar fallback
//   F  fader gain loops                    structural replicas of
//                                          GOSoundFader::ProcessAndAccumulate
//                                          (constant / ramp / nonlinear-LUT)
//
// Build (matches the production win64 optimization level, native x86-64):
//   g++ -O3 -ffast-math -funroll-loops -msse3 -std=gnu++20 \
//       -I ../../src/grandorgue -I ../../src/grandorgue/sound/playing \
//       stage_timing_harness.cpp GOSoundResample.cpp -o stage_timing_harness
//
// Methodology: per stage, run the work in 64-frame blocks over the whole
// stream, repeat REPS times, report the fastest run (min = compute floor,
// least scheduler noise). Two working-set sizes: HOT fits in L2 (compute
// ceiling), STREAM is several MB (LLC/DRAM streaming, closer to a real organ
// where thousands of voices interleave).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "sound/playing/GOSoundCompressionCache.h"
#include "sound/playing/GOSoundResample.h"

namespace {

constexpr unsigned BLOCK_FRAMES = 64; // typical samples-per-buffer
constexpr unsigned REPS = 9;

using Clock = std::chrono::steady_clock;

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

// Keep a result alive so the optimizer cannot delete the measured work.
volatile float g_sinkF;
volatile int g_sinkI;

// ---------------------------------------------------------------------------
// Synthetic organ-like stereo source: decaying harmonic stack + noise floor.
// Amplitude scaled for the requested bit depth so the encoder produces
// realistic 1..4-byte code lengths.
std::vector<int32_t> makeSource(unsigned nFrames, unsigned bits) {
  std::vector<int32_t> s(nFrames * 2);
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> noise(-1.f, 1.f);
  const double amp = double((1 << (bits - 1)) - 1);

  for (unsigned i = 0; i < nFrames; i++) {
    const double t = (double)i / 48000.0;
    double v = 0.0;
    for (unsigned h = 1; h <= 8; h++)
      v += std::sin(2.0 * M_PI * 220.0 * h * t + 0.7 * h) / (h * 1.3);
    v *= 0.35 * std::exp(-t * 0.15);
    v += 0.002 * noise(rng);
    const double r = 0.995 * v; // slight channel decorrelation
    s[i * 2] = (int32_t)std::lround(std::clamp(v, -1.0, 1.0) * amp);
    s[i * 2 + 1] = (int32_t)std::lround(std::clamp(r, -1.0, 1.0) * amp);
  }
  return s;
}

// Encoder: byte-exact replica of the predictor loop in
// GOSoundAudioSection::Compress(), stereo, using the production
// writeCompressed8/16.
std::vector<unsigned char> encode(
  const std::vector<int32_t> &src, bool format16) {
  const unsigned nFrames = (unsigned)src.size() / 2;
  std::vector<unsigned char> out(src.size() * 4 + 16);
  unsigned len = 0;
  GOSoundCompressionCache state;

  state.Init();
  for (unsigned i = 0; i < nFrames; i++) {
    state.m_last[0] = state.m_prev[0];
    state.m_last[1] = state.m_prev[1];
    state.m_prev[0] = state.m_value[0];
    state.m_prev[1] = state.m_value[1];
    state.m_value[0] = src[i * 2];
    state.m_value[1] = src[i * 2 + 1];
    for (unsigned j = 0; j < 2; j++) {
      const int encodeVal = state.m_value[j]
        - (state.m_prev[j] + (state.m_prev[j] - state.m_last[j]) / 2);
      if (format16)
        GOSoundCompressionCache::writeCompressed16(out.data(), len, encodeVal);
      else
        GOSoundCompressionCache::writeCompressed8(out.data(), len, encodeVal);
    }
  }
  out.resize(len);
  return out;
}

// ---------------------------------------------------------------------------
// Planar sample vectors for the production resampler kernels: same access
// semantics as StreamCacheReadAheadWindow (contiguous int32 taps per channel),
// but backed by the fully decoded stream so only the FIR itself is timed.
struct PlanarVectorAvx2 : GOSoundResample::FloatingSampleVector<2> {
  static constexpr bool kPlanarInt32Taps = true;
  const int32_t *plane[2];
  const int32_t *p = nullptr;

  inline void Seek(unsigned index, uint8_t ch) { p = plane[ch] + index; }
  inline const int32_t *GetPtr() const { return p; }
  inline float NextSample() { return (float)*p++; }
};

struct PlanarVectorScalar : GOSoundResample::FloatingSampleVector<2> {
  static constexpr bool kPlanarInt32Taps = false; // force scalar dispatch
  const int32_t *plane[2];
  const int32_t *p = nullptr;

  inline void Seek(unsigned index, uint8_t ch) { p = plane[ch] + index; }
  inline float NextSample() { return (float)*p++; }
};

// ---------------------------------------------------------------------------
// Fader kernels: structural replicas of GOSoundFader::ProcessAndAccumulate.
__attribute__((noinline)) void faderConstant(
  unsigned nFrames, const float *in, float *out, float g) {
  const float *src = in;
  float *dst = out;
  for (unsigned i = 0; i < nFrames; ++i) {
    dst[0] += src[0] * g;
    dst[1] += src[1] * g;
    src += 2;
    dst += 2;
  }
}

__attribute__((noinline)) void faderRamp(
  unsigned nFrames, const float *in, float *out, float g0, float dg) {
  float g = g0;
  const float *src = in;
  float *dst = out;
  for (unsigned i = 0; i < nFrames; ++i) {
    dst[0] += src[0] * g;
    dst[1] += src[1] * g;
    src += 2;
    dst += 2;
    g += dg;
  }
}

// Replica of the non-linear template hot loop (GOSoundFader.cpp, the
// unvectorized "control flow in loop" one), in_on && out_on, useRef=false.
__attribute__((noinline)) void faderNonLinearTpl(
  unsigned nFrames,
  const float *in,
  float *out,
  float targetVolume,
  float startExt,
  float dExt,
  bool in_on,
  const float *tplB,
  unsigned inLen,
  unsigned inPos,
  bool out_on,
  const float *tplA,
  unsigned outLen,
  unsigned outPos) {
  const float *src = in;
  float *dst = out;
  for (unsigned i = 0; i < nFrames; ++i) {
    const float extNow = startExt + i * dExt;
    const float base = targetVolume * extNow;
    float bi = 0.0f, ao = 1.0f;
    if (in_on) {
      const unsigned idx = std::min(inPos + i, inLen ? inLen - 1 : 0);
      bi = tplB[idx];
    }
    if (out_on) {
      const unsigned idx = std::min(outPos + i, outLen ? outLen - 1 : 0);
      ao = tplA[idx];
    }
    const float vol = base * ((in_on ? bi : 1.0f) * (out_on ? ao : 1.0f));
    dst[0] += src[0] * vol;
    dst[1] += src[1] * vol;
    src += 2;
    dst += 2;
  }
}

// ---------------------------------------------------------------------------
struct StageResult {
  double nsPerOutFrame;
};

void runSuite(unsigned nFrames, const char *label) {
  printf("\n=== %s: %u stereo frames (planar data %.1f MB) ===\n",
    label, nFrames, nFrames * 2 * 4 / 1048576.0);

  const GOSoundResample resample;
  const double resampleFactor = 1.02; // representative random-speaking detune
  const unsigned srcPerOut = 1;       // ~1 source frame per output frame

  // --- prepare data --------------------------------------------------------
  const auto src24 = makeSource(nFrames, 24);
  const auto src16 = makeSource(nFrames, 16);
  const auto comp24 = encode(src24, true);
  const auto comp16 = encode(src16, false);
  printf("compression ratio: 24-bit %.2f (%.2f B/sample)   "
         "16-bit %.2f (%.2f B/sample)\n",
    (double)(nFrames * 2 * 3) / comp24.size(),
    (double)comp24.size() / (nFrames * 2),
    (double)(nFrames * 2 * 2) / comp16.size(),
    (double)comp16.size() / (nFrames * 2));

  // decode parity self-check (24-bit) + planar planes for the FIR stages
  std::vector<int32_t> planeL(nFrames + 16), planeR(nFrames + 16);
  {
    GOSoundCompressionCache c;
    c.Init();
    c.m_ptr = comp24.data();
    for (unsigned i = 0; i < nFrames; i++) {
      c.DecompressionStep(2, true);
      if (c.m_value[0] != src24[i * 2] || c.m_value[1] != src24[i * 2 + 1]) {
        printf("PARITY FAIL at frame %u\n", i);
        return;
      }
      planeL[i] = c.m_value[0];
      planeR[i] = c.m_value[1];
    }
  }

  const double outFrames = // output frames the decoded range corresponds to
    (double)nFrames / resampleFactor;

  // --- Stage D: pure decode chain ------------------------------------------
  auto timeDecode = [&](const std::vector<unsigned char> &comp,
                      bool format16) {
    return bestSeconds(REPS, [&] {
      GOSoundCompressionCache c;
      c.Init();
      c.m_ptr = comp.data();
      for (unsigned i = 0; i < nFrames; i++)
        c.DecompressionStep(2, format16);
      g_sinkI = c.m_value[0];
    });
  };
  const double d24 = timeDecode(comp24, true);
  const double d16 = timeDecode(comp16, false);

  // --- Stage D+: decode + planar store (the ReadAhead refill work) ---------
  std::vector<int32_t> wL(nFrames + 16), wR(nFrames + 16);
  const double dPlanar = bestSeconds(REPS, [&] {
    GOSoundCompressionCache c;
    c.Init();
    c.m_ptr = comp24.data();
    for (unsigned i = 0; i < nFrames; i++) {
      c.DecompressionStep(2, true);
      wL[i] = c.m_value[0];
      wR[i] = c.m_value[1];
    }
    g_sinkI = wL[nFrames - 1];
  });

  // --- Stage R: polyphase FIR, AVX2-planar vs scalar ------------------------
  std::vector<float> outBuf(BLOCK_FRAMES * 2);
  auto timeFir = [&](auto &vec) {
    return bestSeconds(REPS, [&] {
      GOSoundResample::PolyphaseResampler poly(resample);
      GOSoundResample::ResamplingPosition pos;
      pos.Init((float)resampleFactor, 0);
      const unsigned maxIndex
        = nFrames - GOSoundResample::POLYPHASE_POINTS - BLOCK_FRAMES;
      while (pos.GetIndex() < maxIndex) {
        poly.ResampleBlock<std::remove_reference_t<decltype(vec)>, 2>(
          pos, vec, outBuf.data(), BLOCK_FRAMES);
      }
      g_sinkF = outBuf[0];
    });
  };
  PlanarVectorAvx2 vecAvx;
  vecAvx.plane[0] = planeL.data();
  vecAvx.plane[1] = planeR.data();
  PlanarVectorScalar vecScalar;
  vecScalar.plane[0] = planeL.data();
  vecScalar.plane[1] = planeR.data();
  const double rAvx = timeFir(vecAvx);
  const double rScalar = timeFir(vecScalar);

  // --- Stage F: fader loops --------------------------------------------------
  std::vector<float> tempIn(BLOCK_FRAMES * 2, 0.25f), accOut(BLOCK_FRAMES * 2);
  const unsigned fadeLen = 8830; // ~184ms @ 48k
  std::vector<float> tplA(fadeLen), tplB(fadeLen);
  for (unsigned i = 0; i < fadeLen; i++) {
    const float t = (float)i / (fadeLen - 1);
    tplA[i] = std::cos(t * (float)M_PI_2);
    tplB[i] = std::sin(t * (float)M_PI_2);
  }
  const unsigned faderBlocks = nFrames / BLOCK_FRAMES;
  auto timeFader = [&](auto &&call) {
    return bestSeconds(REPS, [&] {
      for (unsigned b = 0; b < faderBlocks; b++)
        call(b);
      g_sinkF = accOut[0];
    });
  };
  const double fConst = timeFader([&](unsigned) {
    faderConstant(BLOCK_FRAMES, tempIn.data(), accOut.data(), 0.7f);
  });
  const double fRamp = timeFader([&](unsigned) {
    faderRamp(BLOCK_FRAMES, tempIn.data(), accOut.data(), 0.7f, 1e-5f);
  });
  const double fNl = timeFader([&](unsigned b) {
    const unsigned pos = (b * BLOCK_FRAMES) % (fadeLen - BLOCK_FRAMES);
    faderNonLinearTpl(BLOCK_FRAMES, tempIn.data(), accOut.data(), 0.7f, 1.0f,
      0.0f, true, tplB.data(), fadeLen, pos, true, tplA.data(), fadeLen, pos);
  });

  // --- report ---------------------------------------------------------------
  auto perOut = [&](double sec, double frames) { return sec / frames * 1e9; };
  const double nsD24 = perOut(d24, outFrames);
  const double nsD16 = perOut(d16, outFrames);
  const double nsDpl = perOut(dPlanar, outFrames);
  const double firFrames = outFrames; // both FIR runs produce ~outFrames
  const double nsRAvx = perOut(rAvx, firFrames);
  const double nsRSc = perOut(rScalar, firFrames);
  const double nsFC = perOut(fConst, faderBlocks * BLOCK_FRAMES);
  const double nsFR = perOut(fRamp, faderBlocks * BLOCK_FRAMES);
  const double nsFN = perOut(fNl, faderBlocks * BLOCK_FRAMES);

  printf("\n  stage                                ns/output-frame (stereo)\n");
  printf("  D   decode 24-bit (fmt16 codes)          %7.2f\n", nsD24);
  printf("  D   decode 16-bit (fmt8 codes)           %7.2f\n", nsD16);
  printf("  D+  decode 24-bit + planar store         %7.2f\n", nsDpl);
  printf("  R   polyphase FIR  AVX2 planar           %7.2f\n", nsRAvx);
  printf("  R   polyphase FIR  scalar fallback       %7.2f\n", nsRSc);
  printf("  F   fader constant                       %7.2f\n", nsFC);
  printf("  F   fader ramp                           %7.2f\n", nsFR);
  printf("  F   fader nonlinear LUT (unvectorized)   %7.2f\n", nsFN);

  const double voice = nsDpl * srcPerOut + nsRAvx + nsFC;
  printf("\n  model voice (compressed 24-bit stereo, polyphase AVX2, "
         "const fader): %.2f ns/frame\n", voice);
  printf("    decode share %.0f%%   FIR share %.0f%%   fader share %.0f%%\n",
    100.0 * nsDpl / voice, 100.0 * nsRAvx / voice, 100.0 * nsFC / voice);
}

} // namespace

int main() {
  runSuite(1u << 16, "HOT (L2-resident)");
  runSuite(1u << 21, "STREAM (LLC/DRAM)");
  return 0;
}
