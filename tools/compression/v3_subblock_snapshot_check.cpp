// v3_subblock_snapshot_check.cpp
//
// Validates the SubblockStartSnapshot idea before any production cache or
// hotpath wiring exists. The goal is precise predictor-state correctness at
// every decoded render-subblock boundary, not renderer performance.
//
// Build:
//   g++ -O3 -std=gnu++17 -I <repo>/src/grandorgue  \
//     -I <repo>/src/v3codec \
//     v3_subblock_snapshot_check.cpp -o v3_subblock_snapshot_check
//
// Usage:
//   v3_subblock_snapshot_check file1.wav [file2.wav ...]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "GOSoundCompressionCacheV3.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr unsigned GUARD_SAMPLES = 8;
constexpr unsigned REPS = 5;
constexpr unsigned SUBBLOCK_SIZES[] = {64, 128, 256, 512};

struct WavData {
  unsigned channels = 0;
  unsigned bitsPerSample = 0;
  std::vector<int32_t> samples;
};

template <class Fn> double bestSeconds(unsigned reps, Fn fn) {
  double best = 1e30;
  for (unsigned r = 0; r < reps; r++) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
  }
  return best;
}

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

  unsigned channels = 0;
  unsigned bitsPerSample = 0;
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
  if (channels > MAX_OUTPUT_CHANNELS)
    return false;

  out.channels = channels;
  out.bitsPerSample = bitsPerSample;
  const unsigned bytesPerSample = bitsPerSample / 8;
  if (bytesPerSample == 0)
    return false;

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
      v = (int8_t)p[0];
    out.samples[i] = v;
    p += bytesPerSample;
  }
  return true;
}

unsigned nFrames(const WavData &w) {
  return (unsigned)w.samples.size() / w.channels;
}

int32_t getSample(const WavData &w, unsigned frame, unsigned ch) {
  return w.samples[(size_t)frame * w.channels + ch];
}

int32_t historySample(const WavData &w, unsigned position, unsigned ch, unsigned back) {
  if (position < back)
    return 0;
  return getSample(w, position - back, ch);
}

std::vector<unsigned> makeGridPositions(unsigned total, unsigned subblockSize) {
  std::vector<unsigned> positions;
  for (unsigned p = 0; p < total; p += subblockSize)
    positions.push_back(p);
  return positions;
}

std::vector<unsigned> makeJumpTargetPositions(unsigned total, unsigned subblockSize) {
  std::vector<unsigned> targets;
  if (total == 0)
    return targets;

  const unsigned candidates[] = {
    std::min(total - 1, subblockSize / 2 + 3),
    std::min(total - 1, subblockSize > 1 ? subblockSize - 1 : 0),
    std::min(total - 1, subblockSize + 17),
    std::min(total - 1, total / 3 + 5),
    std::min(total - 1, total / 2 + subblockSize / 3),
    total - 1,
  };
  for (unsigned p : candidates)
    targets.push_back(p);
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  return targets;
}

bool failState(
  const std::string &path,
  unsigned subblockSize,
  unsigned subblockStart,
  const char *field,
  unsigned ch,
  int32_t got,
  int32_t want) {
  printf(
    "%s: STATE MISMATCH subblock=%u start=%u field=%s ch=%u got=%d want=%d\n",
    path.c_str(), subblockSize, subblockStart, field, ch, got, want);
  return false;
}

bool failSample(
  const std::string &path,
  unsigned subblockSize,
  unsigned subblockStart,
  unsigned position,
  unsigned ch,
  int32_t got,
  int32_t want,
  const char *what) {
  printf(
    "%s: %s MISMATCH subblock=%u start=%u pos=%u ch=%u got=%d want=%d\n",
    path.c_str(), what, subblockSize, subblockStart, position, ch, got, want);
  return false;
}

bool checkSnapshotState(
  const std::string &path,
  const WavData &w,
  unsigned subblockSize,
  unsigned subblockStart,
  const GOSoundCompressionCacheV3::Snapshot &snap,
  const unsigned char *encoded) {
  const unsigned channels = w.channels;

  if (snap.position != subblockStart) {
    printf(
      "%s: SNAPSHOT POSITION MISMATCH subblock=%u start=%u got=%u\n",
      path.c_str(), subblockSize, subblockStart, snap.position);
    return false;
  }

  GOSoundCompressionCacheV3 dec;
  dec.LoadSnapshot(snap, encoded, channels);
  if (dec.m_position != subblockStart) {
    printf(
      "%s: LOADED POSITION MISMATCH subblock=%u start=%u got=%u\n",
      path.c_str(), subblockSize, subblockStart, dec.m_position);
    return false;
  }

  for (unsigned c = 0; c < channels; c++) {
    if (dec.m_value[c] != historySample(w, subblockStart, c, 1))
      return failState(
        path, subblockSize, subblockStart, "m_value(P-1)", c,
        dec.m_value[c], historySample(w, subblockStart, c, 1));
    if (dec.m_prev[c] != historySample(w, subblockStart, c, 2))
      return failState(
        path, subblockSize, subblockStart, "m_prev(P-2)", c,
        dec.m_prev[c], historySample(w, subblockStart, c, 2));
    if (dec.m_last[c] != historySample(w, subblockStart, c, 3))
      return failState(
        path, subblockSize, subblockStart, "m_last(P-3)", c,
        dec.m_last[c], historySample(w, subblockStart, c, 3));
  }

  if (subblockStart < nFrames(w)) {
    dec.DecompressionStep(channels);
    if (dec.m_position != subblockStart + 1) {
      printf(
        "%s: FIRST STEP POSITION MISMATCH subblock=%u start=%u got=%u\n",
        path.c_str(), subblockSize, subblockStart, dec.m_position);
      return false;
    }
    for (unsigned c = 0; c < channels; c++) {
      const int32_t want = getSample(w, subblockStart, c);
      if (dec.m_value[c] != want)
        return failSample(
          path, subblockSize, subblockStart, subblockStart, c,
          dec.m_value[c], want, "FIRST-STEP");
    }
  }

  return true;
}

bool checkSubblockDecode(
  const std::string &path,
  const WavData &w,
  unsigned subblockSize,
  unsigned subblockStart,
  const GOSoundCompressionCacheV3::Snapshot &snap,
  const unsigned char *encoded) {
  const unsigned channels = w.channels;
  const unsigned total = nFrames(w);
  const unsigned decodeLen
    = std::min(subblockSize + GUARD_SAMPLES, total - subblockStart);

  GOSoundCompressionCacheV3 dec;
  dec.LoadSnapshot(snap, encoded, channels);
  for (unsigned i = 0; i < decodeLen; i++) {
    const unsigned pos = subblockStart + i;
    dec.DecompressionStep(channels);
    for (unsigned c = 0; c < channels; c++) {
      const int32_t want = getSample(w, pos, c);
      if (dec.m_value[c] != want)
        return failSample(
          path, subblockSize, subblockStart, pos, c, dec.m_value[c], want,
          "SUBBLOCK");
    }
  }

  std::vector<unsigned> offsets;
  offsets.push_back(0);
  if (decodeLen > 2)
    offsets.push_back(decodeLen / 2);
  if (decodeLen > 1)
    offsets.push_back(decodeLen - 1);

  for (unsigned offset : offsets) {
    const unsigned pos = subblockStart + offset;
    GOSoundCompressionCacheV3 fromSnapshot;
    fromSnapshot.LoadSnapshot(snap, encoded, channels);
    fromSnapshot.DecompressTo(pos, encoded, channels);

    GOSoundCompressionCacheV3 direct;
    direct.Init();
    direct.m_ptr = encoded;
    direct.DecompressTo(pos, encoded, channels);

    for (unsigned c = 0; c < channels; c++) {
      const int32_t want = getSample(w, pos, c);
      if (fromSnapshot.m_value[c] != want)
        return failSample(
          path, subblockSize, subblockStart, pos, c, fromSnapshot.m_value[c],
          want, "SNAPSHOT-SEEK");
      if (direct.m_value[c] != fromSnapshot.m_value[c])
        return failSample(
          path, subblockSize, subblockStart, pos, c, fromSnapshot.m_value[c],
          direct.m_value[c], "DIRECT-VS-SNAPSHOT");
    }
  }

  return true;
}

bool checkJumpTargets(
  const std::string &path,
  const WavData &w,
  unsigned subblockSize,
  const std::vector<unsigned> &positions,
  const std::vector<GOSoundCompressionCacheV3::Snapshot> &snapshots,
  const unsigned char *encoded,
  unsigned &exactJumpTargets,
  unsigned &fallbackJumpTargets) {
  const unsigned channels = w.channels;
  const std::vector<unsigned> jumpTargets
    = makeJumpTargetPositions(nFrames(w), subblockSize);

  for (unsigned target : jumpTargets) {
    const unsigned subblockStart = (target / subblockSize) * subblockSize;
    const size_t snapshotIndex = subblockStart / subblockSize;
    if (
      snapshotIndex >= positions.size()
      || positions[snapshotIndex] != subblockStart) {
      printf(
        "%s: JUMP SNAPSHOT MISSING subblock=%u target=%u start=%u\n",
        path.c_str(), subblockSize, target, subblockStart);
      return false;
    }

    GOSoundCompressionCacheV3 exact;
    exact.LoadSnapshot(snapshots[snapshotIndex], encoded, channels);
    if (exact.m_position != subblockStart) {
      printf(
        "%s: JUMP START POSITION MISMATCH subblock=%u target=%u start=%u "
        "got=%u\n",
        path.c_str(), subblockSize, target, subblockStart, exact.m_position);
      return false;
    }
    exact.DecompressTo(target, encoded, channels);
    for (unsigned c = 0; c < channels; c++) {
      const int32_t want = getSample(w, target, c);
      if (exact.m_value[c] != want)
        return failSample(
          path, subblockSize, subblockStart, target, c, exact.m_value[c],
          want, "JUMP-TARGET-EXACT");
    }
    exactJumpTargets++;

    // Simulate the allowed fallback when an exact subblock-start snapshot is
    // absent: build deterministically from the previous dependent snapshot,
    // decode forward to the subblock start, then continue to the target. This
    // must produce the same state; it is never a zero-state reset except at
    // true section start.
    const unsigned prevStart
      = (subblockStart >= subblockSize) ? subblockStart - subblockSize : 0;
    const size_t prevIndex = prevStart / subblockSize;
    GOSoundCompressionCacheV3 fallback;
    fallback.LoadSnapshot(snapshots[prevIndex], encoded, channels);
    if (prevStart < subblockStart)
      fallback.DecompressTo(subblockStart - 1, encoded, channels);
    if (fallback.m_position != subblockStart) {
      printf(
        "%s: FALLBACK START POSITION MISMATCH subblock=%u target=%u start=%u "
        "prev=%u got=%u\n",
        path.c_str(),
        subblockSize,
        target,
        subblockStart,
        prevStart,
        fallback.m_position);
      return false;
    }
    for (unsigned c = 0; c < channels; c++) {
      if (fallback.m_value[c] != historySample(w, subblockStart, c, 1))
        return failState(
          path, subblockSize, subblockStart, "fallback m_value(P-1)", c,
          fallback.m_value[c], historySample(w, subblockStart, c, 1));
      if (fallback.m_prev[c] != historySample(w, subblockStart, c, 2))
        return failState(
          path, subblockSize, subblockStart, "fallback m_prev(P-2)", c,
          fallback.m_prev[c], historySample(w, subblockStart, c, 2));
      if (fallback.m_last[c] != historySample(w, subblockStart, c, 3))
        return failState(
          path, subblockSize, subblockStart, "fallback m_last(P-3)", c,
          fallback.m_last[c], historySample(w, subblockStart, c, 3));
    }
    fallback.DecompressTo(target, encoded, channels);
    for (unsigned c = 0; c < channels; c++) {
      const int32_t want = getSample(w, target, c);
      if (fallback.m_value[c] != want)
        return failSample(
          path, subblockSize, subblockStart, target, c, fallback.m_value[c],
          want, "JUMP-TARGET-FALLBACK");
    }
    fallbackJumpTargets++;
  }

  return true;
}

bool checkFileSubblockSize(
  const std::string &path, const WavData &w, unsigned subblockSize) {
  const unsigned channels = w.channels;
  const unsigned total = nFrames(w);
  if (total == 0)
    return false;

  const std::vector<unsigned> positions = makeGridPositions(total, subblockSize);
  std::vector<unsigned char> encoded(
    GOSoundCompressionCacheV3::WorstCaseSize(total, channels));
  std::vector<GOSoundCompressionCacheV3::Snapshot> snapshots(positions.size());

  GOSoundCompressionCacheV3 enc;
  const unsigned written = enc.Encode(
    encoded.data(),
    total,
    channels,
    positions.data(),
    (unsigned)positions.size(),
    [&](unsigned idx, unsigned ch) { return getSample(w, idx, ch); },
    snapshots.data());
  encoded.resize(written);

  for (size_t s = 0; s < positions.size(); s++) {
    if (!checkSnapshotState(
          path, w, subblockSize, positions[s], snapshots[s], encoded.data()))
      return false;
    if (!checkSubblockDecode(
          path, w, subblockSize, positions[s], snapshots[s], encoded.data()))
      return false;
  }

  unsigned exactJumpTargets = 0;
  unsigned fallbackJumpTargets = 0;
  if (!checkJumpTargets(
        path,
        w,
        subblockSize,
        positions,
        snapshots,
        encoded.data(),
        exactJumpTargets,
        fallbackJumpTargets))
    return false;

  uint64_t decodedChannelSamples = 0;
  for (unsigned p : positions)
    decodedChannelSamples += (uint64_t)std::min(subblockSize + GUARD_SAMPLES, total - p)
      * channels;

  volatile int32_t sink = 0;
  const double sec = bestSeconds(REPS, [&] {
    for (size_t s = 0; s < positions.size(); s++) {
      const unsigned len
        = std::min(subblockSize + GUARD_SAMPLES, total - positions[s]);
      GOSoundCompressionCacheV3 dec;
      dec.LoadSnapshot(snapshots[s], encoded.data(), channels);
      for (unsigned i = 0; i < len; i++) {
        dec.DecompressionStep(channels);
        sink += dec.m_value[0];
      }
    }
  });

  const size_t metadataBytes
    = positions.size()
    * (sizeof(unsigned) + sizeof(GOSoundCompressionCacheV3::Snapshot));
  const double metadataPerFrame = (double)metadataBytes / total;
  const double decodeNs = sec * 1e9 / (double)decodedChannelSamples;
  const double ratio
    = (double)(total * channels * (w.bitsPerSample / 8)) / (double)written;

  printf(
    "%s: OK frames=%u channels=%u subblock=%u snapshots=%zu "
    "jumpTargets=%u fallbackTargets=%u metadata=%zu "
    "metadata/frame=%.3fB decode=%.3f ns/channel-sample encoded=%u "
    "ratio=%.4fx\n",
    path.c_str(),
    total,
    channels,
    subblockSize,
    positions.size(),
    exactJumpTargets,
    fallbackJumpTargets,
    metadataBytes,
    metadataPerFrame,
    decodeNs,
    written,
    ratio);
  (void)sink;
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("usage: %s file1.wav [file2.wav ...]\n", argv[0]);
    return 1;
  }

  int checked = 0;
  int failures = 0;
  for (int i = 1; i < argc; i++) {
    WavData w;
    if (!loadWav(argv[i], w)) {
      printf("%s: failed to load\n", argv[i]);
      failures++;
      continue;
    }

    for (unsigned subblockSize : SUBBLOCK_SIZES) {
      checked++;
      if (!checkFileSubblockSize(argv[i], w, subblockSize))
        failures++;
    }
  }

  printf(
    "\n%d/%d file/subblock-size checks passed\n", checked - failures, checked);
  return failures ? 1 : 0;
}
