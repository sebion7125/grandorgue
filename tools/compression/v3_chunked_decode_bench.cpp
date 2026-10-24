// v3_chunked_decode_bench.cpp
//
// Isolates where the fleet benchmark's decode speed goes once the SAME
// kernel runs under the production render pipeline's conditions. The
// original codec_fleet_benchmark measured ~4.75-4.89x vs legacy for one
// CONTINUOUS decode run per file (state lives in registers across the
// whole file, no block management, best-of-N warm-cache timing). The
// production path (GOSoundStream::DecodeGroupToScratch) instead decodes
// per audio callback: 128-sample spans, each with a dependent-forward
// state copy in, an end-state snapshot out, a scalar per-lane guard tail
// (GUARD samples via single-voice DecompressionStep), and an implicit
// re-decode of those guard samples by the next span's SIMD run.
//
// Scenario ladder (all on the REAL production headers, 8 mono lanes,
// phase-misaligned like real claimed voices; ns per channel-sample,
// best of REPS like the fleet tool):
//   LEGACY  - GOSoundCompressionCache::DecompressionStep, continuous
//             (the fleet benchmark's 'production' reference).
//   A       - DecodeBatchInterleaved, ONE continuous call per group
//             (fleet conditions, but the real production kernel with its
//             target("avx2") codegen instead of the prototype's global
//             -mavx2 -ffast-math build).
//   B       - DecodeBatchInterleaved in SPAN-sample calls, lane state
//             persisting across calls (adds ONLY the per-call kernel
//             entry/exit cost: padding setup, state register load/store,
//             chunk bookkeeping).
//   C       - full production pattern per span: state copy in (the
//             dependent-forward PrepareBlockState hit), SIMD span, state
//             snapshot out, scalar guard tail of GUARD frames per lane -
//             and because the next span resumes from the snapshot, the
//             SIMD re-decodes those GUARD frames (the production
//             double-decode).
//   C-NOG   - C without the scalar guard tail (isolates the guard cost;
//             the snapshot copies stay).
//
// Usage: v3_chunked_decode_bench <8 wav files>  [--span N] [--frames N]
//   Channel 0 of each file is used (mono lanes - comparable to the fleet
//   benchmark's numbers).
//
// Build (production-equivalent codegen - NO global -mavx2/-ffast-math;
// the kernels carry their own target attributes):
//   g++ -O3 -std=gnu++20 -I /home/vboxuser/grandorgue/src/grandorgue  \
//     -I /home/vboxuser/grandorgue/src/v3codec \
//     v3_chunked_decode_bench.cpp -o v3_chunked_decode_bench

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "sound/playing/GOSoundCompressionCache.h"
#include "GOSoundCompressionCacheV3.h"
#include "GOSoundCompressionCacheV3Batch.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr unsigned REPS = 7;
constexpr unsigned LANES = GOSoundCompressionCacheV3Batch::LANES;
constexpr unsigned GUARD = 8; // GOSoundResample::POLYPHASE_POINTS

struct WavData {
  unsigned channels = 0;
  unsigned bitsPerSample = 0;
  std::vector<int32_t> samples; // interleaved
};

bool loadWav(const std::string &path, WavData &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;
  char riff[4];
  f.read(riff, 4);
  if (memcmp(riff, "RIFF", 4) != 0)
    return false;
  f.seekg(4, std::ios::cur);
  char wave[4];
  f.read(wave, 4);
  if (memcmp(wave, "WAVE", 4) != 0)
    return false;

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
      uint32_t sampleRate, byteRate;
      f.read((char *)&audioFormat, 2);
      f.read((char *)&nChannels, 2);
      f.read((char *)&sampleRate, 4);
      f.read((char *)&byteRate, 4);
      f.read((char *)&blockAlign, 2);
      f.read((char *)&bps, 2);
      channels = nChannels;
      bitsPerSample = bps;
      f.seekg((std::streamoff)chunkSize - 16, std::ios::cur);
    } else if (memcmp(id, "data", 4) == 0) {
      data.resize(chunkSize);
      f.read((char *)data.data(), chunkSize);
    } else {
      f.seekg((std::streamoff)chunkSize + (chunkSize & 1), std::ios::cur);
    }
  }
  if (channels == 0 || bitsPerSample != 24 || data.empty())
    return false;

  const size_t n = data.size() / 3;
  out.channels = channels;
  out.bitsPerSample = bitsPerSample;
  out.samples.resize(n);
  for (size_t i = 0; i < n; i++) {
    const unsigned char *b = data.data() + i * 3;
    const uint32_t raw
      = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
    out.samples[i] = (int32_t)(raw << 8) >> 8;
  }
  return true;
}

double bestSeconds(unsigned reps, auto &&fn) {
  double best = 1e100;
  for (unsigned r = 0; r < reps; r++) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
  }
  return best;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 9) {
    printf("usage: %s <8 wav files> [--span N] [--frames N]\n", argv[0]);
    return 1;
  }
  unsigned span = 128;
  unsigned framesArg = 0;
  for (int i = 9; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--span"))
      span = (unsigned)atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--frames"))
      framesArg = (unsigned)atoi(argv[i + 1]);
  }
  if (!GOSoundCompressionCacheV3Batch::CpuSupported()) {
    printf("no AVX2 - cannot benchmark the batch kernel\n");
    return 1;
  }

  // Load 8 files, mono (channel 0), encode each as one continuous V3
  // stream with a position-0 snapshot - like a real MainSegment.
  WavData waves[LANES];
  std::vector<unsigned char> encoded[LANES];
  GOSoundCompressionCacheV3::Snapshot startSnap[LANES];
  unsigned nFrames = ~0u;

  for (unsigned l = 0; l < LANES; l++) {
    if (!loadWav(argv[1 + l], waves[l])) {
      printf("%s: failed to load (need 24-bit wav)\n", argv[1 + l]);
      return 1;
    }
    const unsigned frames
      = (unsigned)(waves[l].samples.size() / waves[l].channels);
    nFrames = std::min(nFrames, frames);
  }
  if (framesArg)
    nFrames = std::min(nFrames, framesArg);
  nFrames -= nFrames % span; // whole spans only, keeps scenarios comparable

  for (unsigned l = 0; l < LANES; l++) {
    const unsigned ch = waves[l].channels;
    const unsigned frames
      = (unsigned)(waves[l].samples.size() / waves[l].channels);
    encoded[l].resize(GOSoundCompressionCacheV3::WorstCaseSize(frames, 1));
    GOSoundCompressionCacheV3 enc;
    const unsigned snapPos = 0;
    const unsigned written = enc.Encode(
      encoded[l].data(),
      frames,
      1,
      /*order=*/3,
      &snapPos,
      1,
      [&](unsigned idx, unsigned c) {
        (void)c;
        return waves[l].samples[(size_t)idx * ch]; // channel 0 = mono lane
      },
      &startSnap[l]);
    encoded[l].resize(written);
  }

  printf(
    "8 mono lanes, %u frames each, span %u (+%u guard)\n\n",
    nFrames,
    span,
    GUARD);

  std::vector<int32_t> outStore((size_t)nFrames * LANES + GUARD * LANES);
  int32_t *outPtrs[MAX_OUTPUT_CHANNELS] = {outStore.data(), nullptr};
  const double denom = (double)nFrames * LANES;

  auto freshStates
    = [&](GOSoundCompressionCacheV3 states[LANES]) {
        for (unsigned l = 0; l < LANES; l++)
          states[l].LoadSnapshot(startSnap[l], encoded[l].data(), 1);
      };
  auto makeLanes = [&](
                     GOSoundCompressionCacheV3 states[LANES],
                     GOSoundCompressionCacheV3Batch::Lane lanes[LANES]) {
    for (unsigned l = 0; l < LANES; l++) {
      lanes[l].cache = &states[l];
      lanes[l].channels = 1;
    }
  };

  // ---- LEGACY reference: continuous scalar DecompressionStep on the
  // real legacy codec - encode/decode pattern taken verbatim from
  // codec_fleet_benchmark.cpp's 'production' leg ----
  std::vector<std::vector<uint8_t>> legacyEnc(LANES);
  for (unsigned l = 0; l < LANES; l++) {
    std::vector<uint8_t> &buf = legacyEnc[l];

    buf.resize((size_t)nFrames * 4 + 16);
    unsigned outLen = 0;
    int value = 0, prev = 0, last = 0;
    const unsigned chIn = waves[l].channels;
    for (unsigned i = 0; i < nFrames; i++) {
      last = prev;
      prev = value;
      value = waves[l].samples[(size_t)i * chIn];
      const int pred = prev + (prev - last) / 2;
      GOSoundCompressionCache::writeCompressed16(
        buf.data(), outLen, value - pred);
    }
    buf.resize(outLen);
  }
  const double legacySec = bestSeconds(REPS, [&] {
    for (unsigned l = 0; l < LANES; l++) {
      GOSoundCompressionCache dec;
      dec.Init();
      dec.m_ptr = legacyEnc[l].data();
      for (unsigned i = 0; i < nFrames; i++) {
        dec.DecompressionStep(1, true);
        outStore[(size_t)i * LANES + l] = dec.m_value[0];
      }
    }
  });

  // ---- A: continuous, one kernel call (fleet conditions, real kernel) --
  const double aSec = bestSeconds(REPS, [&] {
    GOSoundCompressionCacheV3 states[LANES];
    GOSoundCompressionCacheV3Batch::Lane lanes[LANES];
    freshStates(states);
    makeLanes(states, lanes);
    GOSoundCompressionCacheV3Batch::DecodeBatchInterleaved(
      lanes, LANES, nFrames, outPtrs);
  });

  // ---- B: span-sized kernel calls, state persists (per-call cost) ----
  const double bSec = bestSeconds(REPS, [&] {
    GOSoundCompressionCacheV3 states[LANES];
    GOSoundCompressionCacheV3Batch::Lane lanes[LANES];
    freshStates(states);
    makeLanes(states, lanes);
    for (unsigned done = 0; done < nFrames; done += span) {
      int32_t *out[MAX_OUTPUT_CHANNELS]
        = {outStore.data() + (size_t)done * LANES, nullptr};
      GOSoundCompressionCacheV3Batch::DecodeBatchInterleaved(
        lanes, LANES, span, out);
    }
  });

  // ---- C / C-NOG: full production pattern per span ----
  auto runProductionPattern = [&](bool withGuard) {
    // savedState[i] = the dependent-forward chain point (m_BlockEndState)
    GOSoundCompressionCacheV3 saved[LANES];
    GOSoundCompressionCacheV3 work[LANES];
    GOSoundCompressionCacheV3Batch::Lane lanes[LANES];
    freshStates(saved);
    for (unsigned done = 0; done < nFrames; done += span) {
      // PrepareBlockState: dependent-forward copy in
      for (unsigned l = 0; l < LANES; l++)
        work[l] = saved[l];
      makeLanes(work, lanes);
      int32_t *out[MAX_OUTPUT_CHANNELS]
        = {outStore.data() + (size_t)done * LANES, nullptr};
      GOSoundCompressionCacheV3Batch::DecodeBatchInterleaved(
        lanes, LANES, span, out);
      // end-state snapshot out (the next span resumes here - so the
      // guard frames below get re-decoded by the next SIMD span, exactly
      // like production)
      for (unsigned l = 0; l < LANES; l++)
        saved[l] = work[l];
      if (withGuard && done + span < nFrames) {
        // scalar guard tail per lane (DecodeGroupToScratch's per-lane
        // finish; writes to the guard region after the span)
        for (unsigned l = 0; l < LANES; l++)
          for (unsigned g = 0; g < GUARD; g++) {
            work[l].DecompressionStep(1);
            outStore[(size_t)(done + span + g) * LANES + l]
              = work[l].m_value[0];
          }
      }
    }
  };
  const double cSec = bestSeconds(REPS, [&] { runProductionPattern(true); });
  const double cNogSec
    = bestSeconds(REPS, [&] { runProductionPattern(false); });

  // ---- D: fused guard via mid-state capture - ONE SIMD call per span
  // covering span+GUARD, chain point captured at span (proposed
  // production pattern; the 8-sample overlap between consecutive spans
  // stays, but runs in SIMD instead of the scalar tail) ----
  auto runFusedPattern = [&] {
    GOSoundCompressionCacheV3 saved[LANES];
    GOSoundCompressionCacheV3 mid[LANES];
    GOSoundCompressionCacheV3Batch::Lane lanes[LANES];

    freshStates(saved);
    for (unsigned done = 0; done < nFrames; done += span) {
      const bool guard = done + span + GUARD <= nFrames;
      const unsigned n = span + (guard ? GUARD : 0);

      makeLanes(saved, lanes); // kernel runs IN PLACE on the chain states
      int32_t *out[MAX_OUTPUT_CHANNELS]
        = {outStore.data() + (size_t)done * LANES, nullptr};
      if (guard) {
        GOSoundCompressionCacheV3Batch::DecodeBatchInterleavedMidCapture(
          lanes, LANES, n, span, mid, out);
        for (unsigned l = 0; l < LANES; l++)
          saved[l] = mid[l]; // one small copy: the chain point
      } else {
        GOSoundCompressionCacheV3Batch::DecodeBatchInterleaved(
          lanes, LANES, n, out);
      }
    }
  };
  const double dSec = bestSeconds(REPS, runFusedPattern);

  // Bit-exactness: D's decoded stream must equal a continuous reference
  // (the mid-capture must not disturb the chain).
  {
    std::vector<int32_t> ref((size_t)nFrames * LANES);
    GOSoundCompressionCacheV3 states[LANES];
    GOSoundCompressionCacheV3Batch::Lane lanes[LANES];

    freshStates(states);
    makeLanes(states, lanes);
    int32_t *refOut[MAX_OUTPUT_CHANNELS] = {ref.data(), nullptr};

    GOSoundCompressionCacheV3Batch::DecodeBatchInterleaved(
      lanes, LANES, nFrames, refOut);
    runFusedPattern(); // leaves its result in outStore
    for (size_t i = 0; i < (size_t)nFrames * LANES; i++)
      if (outStore[i] != ref[i]) {
        printf(
          "MID-CAPTURE MISMATCH at flat index %zu (frame %zu lane %zu)\n",
          i,
          i / LANES,
          i % LANES);
        return 1;
      }
  }

  printf("ns per channel-sample (best of %u, warm):\n", REPS);
  printf("  LEGACY continuous scalar:            %.3f\n", legacySec / denom * 1e9);
  printf(
    "  A  kernel, continuous:               %.3f  (%.2fx vs legacy)\n",
    aSec / denom * 1e9,
    legacySec / aSec);
  printf(
    "  B  kernel, %4u-spans:               %.3f  (%.2fx vs legacy, +%.0f%% vs A)\n",
    span,
    bSec / denom * 1e9,
    legacySec / bSec,
    (bSec / aSec - 1.0) * 100.0);
  printf(
    "  C-NOG spans + state copies:          %.3f  (%.2fx vs legacy, +%.0f%% vs A)\n",
    cNogSec / denom * 1e9,
    legacySec / cNogSec,
    (cNogSec / aSec - 1.0) * 100.0);
  printf(
    "  C  full production pattern (+guard): %.3f  (%.2fx vs legacy, +%.0f%% vs A)\n",
    cSec / denom * 1e9,
    legacySec / cSec,
    (cSec / aSec - 1.0) * 100.0);
  printf(
    "  D  fused guard, mid-state capture:   %.3f  (%.2fx vs legacy, +%.0f%% vs A"
    ", %.0f%% faster than C)\n",
    dSec / denom * 1e9,
    legacySec / dSec,
    (dSec / aSec - 1.0) * 100.0,
    (cSec / dSec - 1.0) * 100.0);
  printf("  mid-capture chain: BIT-EXACT vs continuous decode\n");
  return 0;
}
