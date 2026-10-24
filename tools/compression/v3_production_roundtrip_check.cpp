// v3_production_roundtrip_check.cpp
//
// Step 1 of the implementation plan (see tools/compression/INVESTIGATION.md)
// is "integrate as single-voice decode first... to validate loop points,
// seeking/backward jumps... none of this was covered by the standalone test
// harnesses". This tool closes exactly that gap, against the REAL production
// header (src/v3codec/GOSoundCompressionCacheV3.h), not a
// copy or a simplified stand-in:
//
//   1. Sequential round-trip: encode a whole file in one Encode() call,
//      decode every sample sequentially, compare bit-exact to source PCM.
//   2. Segment-boundary resume: simulate GOSoundAudioSection's StartSegment
//      mechanism directly - split the file at several arbitrary (NOT
//      block-aligned) offsets, call Encode() once per segment with the
//      predictor state carried across calls (exactly like
//      GOSoundAudioSection::Compress does across m_StartSegments today),
//      snapshot the cache state before each call, then verify that resuming
//      decode from each snapshot (as GOSoundStream::GoToStartSegment does by
//      rebasing m_ptr) reproduces the exact same samples as the sequential
//      decode from step 1.
//   3. Backward-seek fallback: call DecompressTo() with a position before
//      the cache's current position (as happens when GOSoundStream jumps
//      backward), confirm it resets and replays to the bit-exact correct
//      value - mirrors legacy GOSoundCompressionCache::DecompressTo's own
//      fallback semantics.
//   4. Speed: single-voice (no SIMD batching) decode speed vs the real,
//      unmodified legacy GOSoundCompressionCache on the same files - the
//      fair, apples-to-apples number for what Step 1 alone (before any
//      multi-voice batching) actually buys in production.
//
// Build:
//   g++ -O3 -ffast-math -funroll-loops -std=gnu++17 \
//     -I <repo>/src/grandorgue  \
//     -I <repo>/src/v3codec v3_production_roundtrip_check.cpp \
//     -o v3_production_roundtrip_check
//
// Usage: v3_production_roundtrip_check file1.wav file2.wav ...

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "sound/playing/GOSoundCompressionCache.h"
#include "GOSoundCompressionCacheV3.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr unsigned REPS = 5;

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

struct WavData {
  unsigned channels;
  unsigned bitsPerSample;
  std::vector<int32_t> samples; // interleaved, sign-extended
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
  if (channels == 0 || data.empty())
    return false;

  out.channels = channels;
  out.bitsPerSample = bitsPerSample;
  const unsigned bytesPerSample = bitsPerSample / 8;
  const unsigned nFrames = (unsigned)data.size() / (bytesPerSample * channels);
  out.samples.resize((size_t)nFrames * channels);
  const unsigned char *p = data.data();
  for (unsigned i = 0; i < nFrames * channels; i++) {
    int32_t v = 0;
    if (bytesPerSample == 2)
      v = (int16_t)(p[0] | (p[1] << 8));
    else if (bytesPerSample == 3) {
      v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
      if (v & 0x800000)
        v -= 0x1000000;
    } else if (bytesPerSample == 4)
      v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
    else
      v = (int8_t)p[0] - 0 /* keep 8-bit signed as-is if ever encountered */;
    out.samples[i] = v;
    p += bytesPerSample;
  }
  return true;
}

unsigned nFrames(const WavData &w) { return (unsigned)w.samples.size() / w.channels; }

int getSample(const WavData &w, unsigned frame, unsigned ch) {
  return w.samples[(size_t)frame * w.channels + ch];
}

// ---------------------------------------------------------------------------
// Test 1+2 combined: encode as N segments (simulating StartSegments at
// arbitrary, non-block-aligned offsets), verify sequential decode AND
// per-segment snapshot resume both reproduce the source bit-exact.
bool checkRoundtripAndSegments(
  const std::string &path, const WavData &w, bool verbose) {
  const unsigned channels = w.channels;
  const unsigned total = nFrames(w);

  // Arbitrary, deliberately NOT block-aligned "StartSegment" positions,
  // mirroring GOSoundAudioSection's attack/loop-start bookmarks into ONE
  // single continuous encode of the whole section (not separate re-encodes
  // per segment - that was an earlier, incorrect design: see git history).
  std::vector<unsigned> segStarts = {0};
  if (total > 5000)
    segStarts.push_back(777); // mid-first-block
  if (total > 20000)
    segStarts.push_back(15001); // spans several blocks in
  if (total > 100000)
    segStarts.push_back(100033);

  std::vector<unsigned char> data(
    GOSoundCompressionCacheV3::WorstCaseSize(total, channels));
  std::vector<GOSoundCompressionCacheV3::Snapshot> snapshots(segStarts.size());

  GOSoundCompressionCacheV3 enc;
  const unsigned written = enc.Encode(
    data.data(),
    total,
    channels,
    segStarts.data(),
    (unsigned)segStarts.size(),
    [&](unsigned idx, unsigned ch) { return getSample(w, idx, ch); },
    snapshots.data());

  // --- Test 1: sequential decode of the whole stream, must match source
  // bit-exact.
  {
    GOSoundCompressionCacheV3 dec;
    dec.Init();
    dec.m_ptr = data.data();
    for (unsigned i = 0; i < total; i++) {
      dec.DecompressTo(i, data.data(), channels);
      for (unsigned c = 0; c < channels; c++) {
        if (dec.m_value[c] != getSample(w, i, c)) {
          printf(
            "%s: SEQUENTIAL MISMATCH at frame %u ch %u: got %d want %d\n",
            path.c_str(), i, c, dec.m_value[c], getSample(w, i, c));
          return false;
        }
      }
    }
  }

  // --- Test 2: resume from each segment snapshot (mirrors
  // GOSoundStream::GoToStartSegment rebasing cache.m_ptr onto the real
  // buffer), decode forward through that segment, compare bit-exact.
  for (size_t s = 0; s < segStarts.size(); s++) {
    const unsigned segStart = segStarts[s];
    const unsigned segEnd
      = (s + 1 < segStarts.size()) ? segStarts[s + 1] : total;

    GOSoundCompressionCacheV3 dec;
    dec.LoadSnapshot(snapshots[s], data.data(), channels);

    for (unsigned i = segStart; i < segEnd; i++) {
      dec.DecompressTo(i, data.data(), channels);
      for (unsigned c = 0; c < channels; c++) {
        if (dec.m_value[c] != getSample(w, i, c)) {
          printf(
            "%s: SEGMENT %zu RESUME MISMATCH at frame %u ch %u: got %d want "
            "%d\n",
            path.c_str(), s, i, c, dec.m_value[c], getSample(w, i, c));
          return false;
        }
      }
    }
  }

  // --- Test 3: backward-seek fallback. Decode forward to some position,
  // then request an earlier position (a real backward jump, e.g. release
  // alignment correlation search), confirm the reset+replay path in
  // DecompressTo gives the bit-exact correct value.
  if (total > 50) {
    GOSoundCompressionCacheV3 dec;
    dec.Init();
    dec.m_ptr = data.data();
    const unsigned fwd = total / 2;
    dec.DecompressTo(fwd, data.data(), channels);

    const unsigned back = fwd / 3;
    dec.DecompressTo(back, data.data(), channels);
    for (unsigned c = 0; c < channels; c++) {
      if (dec.m_value[c] != getSample(w, back, c)) {
        printf(
          "%s: BACKWARD-SEEK MISMATCH at frame %u ch %u: got %d want %d\n",
          path.c_str(), back, c, dec.m_value[c], getSample(w, back, c));
        return false;
      }
    }
    // and forward again past the original position, from the reset state
    const unsigned fwd2 = total - 1;
    dec.DecompressTo(fwd2, data.data(), channels);
    for (unsigned c = 0; c < channels; c++) {
      if (dec.m_value[c] != getSample(w, fwd2, c)) {
        printf(
          "%s: POST-BACKWARD-SEEK FORWARD MISMATCH at frame %u ch %u: got %d "
          "want %d\n",
          path.c_str(), fwd2, c, dec.m_value[c], getSample(w, fwd2, c));
        return false;
      }
    }
  }

  const double ratio = (double)(total * channels * (w.bitsPerSample / 8))
    / (double)written;
  if (verbose)
    printf(
      "%s: OK  frames=%u channels=%u ratio=%.4fx segments=%zu\n",
      path.c_str(), total, channels, ratio, segStarts.size());
  return true;
}

// ---------------------------------------------------------------------------
// Test 4: single-voice decode speed, V3 vs real unmodified legacy cache.
void benchSpeed(const WavData &w) {
  const unsigned channels = w.channels;
  const unsigned total = nFrames(w);
  const bool format16 = w.bitsPerSample > 16;

  // Legacy encode (byte-for-byte what GOSoundAudioSection::Compress does).
  std::vector<unsigned char> legacyData(
    (size_t)total * channels * 4 + 16);
  {
    GOSoundCompressionCache state;
    state.Init();
    unsigned output_len = 0;
    for (unsigned i = 0; i < total; i++) {
      state.m_last[0] = state.m_prev[0];
      state.m_prev[0] = state.m_value[0];
      state.m_value[0] = getSample(w, i, 0);
      if (channels > 1) {
        state.m_last[1] = state.m_prev[1];
        state.m_prev[1] = state.m_value[1];
        state.m_value[1] = getSample(w, i, 1);
      }
      for (unsigned j = 0; j < channels; j++) {
        int val = state.m_value[j];
        int encode
          = val - (state.m_prev[j] + (state.m_prev[j] - state.m_last[j]) / 2);
        if (format16)
          GOSoundCompressionCache::writeCompressed16(
            legacyData.data(), output_len, encode);
        else
          GOSoundCompressionCache::writeCompressed8(
            legacyData.data(), output_len, encode);
      }
    }
    legacyData.resize(output_len);
  }

  std::vector<unsigned char> v3Data(
    GOSoundCompressionCacheV3::WorstCaseSize(total, channels));
  unsigned v3Written;
  {
    GOSoundCompressionCacheV3 enc;
    v3Written = enc.Encode(
      v3Data.data(), total, channels, nullptr, 0,
      [&](unsigned idx, unsigned ch) { return getSample(w, idx, ch); },
      nullptr);
  }

  volatile int sink = 0;
  const double legacySec = bestSeconds(REPS, [&] {
    GOSoundCompressionCache dec;
    dec.Init();
    dec.m_ptr = legacyData.data();
    for (unsigned i = 0; i < total; i++)
      dec.DecompressionStep(channels, format16);
    sink += dec.m_value[0];
  });
  const double v3Sec = bestSeconds(REPS, [&] {
    GOSoundCompressionCacheV3 dec;
    dec.Init();
    dec.m_ptr = v3Data.data();
    for (unsigned i = 0; i < total; i++)
      dec.DecompressionStep(channels);
    sink += dec.m_value[0];
  });

  const double legacyNs = legacySec * 1e9 / total;
  const double v3Ns = v3Sec * 1e9 / total;
  printf(
    "  speed: legacy=%.3f ns/frame  v3(scalar,single-voice)=%.3f ns/frame  "
    "speedup=%.2fx  (sizes: legacy=%u v3=%u)\n",
    legacyNs, v3Ns, legacyNs / v3Ns, (unsigned)legacyData.size(), v3Written);
  (void)sink;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: %s file1.wav [file2.wav ...]\n", argv[0]);
    return 1;
  }

  int failures = 0;
  int checked = 0;
  for (int i = 1; i < argc; i++) {
    WavData w;
    if (!loadWav(argv[i], w)) {
      printf("%s: failed to load\n", argv[i]);
      continue;
    }
    checked++;
    if (!checkRoundtripAndSegments(argv[i], w, true)) {
      failures++;
      continue;
    }
    benchSpeed(w);
  }

  printf(
    "\n%d/%d files passed all correctness checks (roundtrip, segment-resume, "
    "backward-seek)\n",
    checked - failures, checked);
  return failures ? 1 : 0;
}
