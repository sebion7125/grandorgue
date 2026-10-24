// v3_subblock_builder_api_check.cpp
//
// Prototype-level check for the planned decoded render-subblock builder APIs:
//   - scalar MainSegment V3 builder;
//   - batch MainSegment V3 lane-interleaved builder;
//   - uncompressed EndSegment copy/convert builder.
//
// This is still a tool, not production wiring. It intentionally avoids cache
// serialization and GOSoundStream integration.
//
// Build:
//   g++ -O3 -mavx2 -std=gnu++17 -I <repo>/src/grandorgue  \
//     -I <repo>/src/v3codec \
//     v3_subblock_builder_api_check.cpp -o v3_subblock_builder_api_check
//
// Usage:
//   v3_subblock_builder_api_check file1.wav ... file8.wav

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "GOSoundCompressionCacheV3.h"
#include "GOSoundCompressionCacheV3Batch.h"

namespace {

constexpr unsigned LANES = GOSoundCompressionCacheV3Batch::LANES;
constexpr unsigned SUBBLOCK_SAMPLES = 128;
constexpr unsigned GUARD_SAMPLES = 8;
constexpr unsigned BUILD_SAMPLES = SUBBLOCK_SAMPLES + GUARD_SAMPLES;

struct WavData {
  unsigned channels = 0;
  unsigned bitsPerSample = 0;
  std::vector<int32_t> samples;
};

struct MainBuildRequest {
  const unsigned char *encoded = nullptr;
  GOSoundCompressionCacheV3::Snapshot snapshot;
  unsigned channels = 0;
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
  if (channels == 0 || data.empty() || channels > MAX_OUTPUT_CHANNELS)
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

void buildMainV3Scalar(
  const MainBuildRequest &req,
  unsigned samples,
  std::vector<int32_t> &out) {
  out.assign((size_t)req.channels * samples, 0);
  GOSoundCompressionCacheV3 dec;
  dec.LoadSnapshot(req.snapshot, req.encoded, req.channels);
  for (unsigned i = 0; i < samples; i++) {
    dec.DecompressionStep(req.channels);
    for (unsigned c = 0; c < req.channels; c++)
      out[(size_t)c * samples + i] = dec.m_value[c];
  }
}

void buildMainV3BatchInterleaved(
  const MainBuildRequest reqs[LANES],
  unsigned numLanes,
  unsigned samples,
  std::vector<std::vector<int32_t>> &out) {
  const unsigned channels = reqs[0].channels;
  out.assign(channels, std::vector<int32_t>((size_t)samples * LANES));

  std::vector<GOSoundCompressionCacheV3> caches(LANES);
  GOSoundCompressionCacheV3Batch::Lane lanes[LANES];
  for (unsigned l = 0; l < LANES; l++) {
    const unsigned srcLane = l < numLanes ? l : 0;
    caches[l].LoadSnapshot(reqs[srcLane].snapshot, reqs[srcLane].encoded, channels);
    lanes[l].cache = &caches[l];
    lanes[l].channels = channels;
  }

  int32_t *outPtrs[MAX_OUTPUT_CHANNELS] = {};
  for (unsigned c = 0; c < channels; c++)
    outPtrs[c] = out[c].data();

  GOSoundCompressionCacheV3Batch::DecodeBatchInterleaved(
    lanes, numLanes, samples, outPtrs);
}

void buildEndSegmentScalar(
  const std::vector<std::vector<int32_t>> &endData,
  unsigned start,
  unsigned samples,
  std::vector<int32_t> &out) {
  const unsigned channels = (unsigned)endData.size();
  out.assign((size_t)channels * samples, 0);
  for (unsigned c = 0; c < channels; c++)
    for (unsigned i = 0; i < samples; i++)
      if (start + i < endData[c].size())
        out[(size_t)c * samples + i] = endData[c][start + i];
}

void buildEndSegmentInterleaved(
  const std::vector<std::vector<int32_t>> endData[LANES],
  unsigned start,
  unsigned samples,
  unsigned channels,
  std::vector<std::vector<int32_t>> &out) {
  out.assign(channels, std::vector<int32_t>((size_t)samples * LANES));
  for (unsigned c = 0; c < channels; c++)
    for (unsigned i = 0; i < samples; i++)
      for (unsigned l = 0; l < LANES; l++)
        if (start + i < endData[l][c].size())
          out[c][(size_t)i * LANES + l] = endData[l][c][start + i];
}

bool checkMainBuilders(int argc, char **argv) {
  std::vector<WavData> waves(LANES);
  for (unsigned l = 0; l < LANES; l++) {
    if (!loadWav(argv[1 + l], waves[l])) {
      printf("%s: failed to load\n", argv[1 + l]);
      return false;
    }
  }

  unsigned channels = waves[0].channels;
  unsigned minFrames = nFrames(waves[0]);
  for (unsigned l = 0; l < LANES; l++) {
    channels = std::min(channels, waves[l].channels);
    minFrames = std::min(minFrames, nFrames(waves[l]));
  }
  if (channels == 0 || minFrames <= BUILD_SAMPLES * 4) {
    printf("input files too short or channel-less\n");
    return false;
  }

  std::vector<std::vector<unsigned char>> encoded(LANES);
  MainBuildRequest reqs[LANES];
  unsigned starts[LANES];
  for (unsigned l = 0; l < LANES; l++) {
    starts[l] = ((l * 97u + 31u) % (minFrames / 2));
    starts[l] = (starts[l] / SUBBLOCK_SAMPLES) * SUBBLOCK_SAMPLES;
    encoded[l].resize(
      GOSoundCompressionCacheV3::WorstCaseSize(nFrames(waves[l]), channels));
    GOSoundCompressionCacheV3::Snapshot snap;
    GOSoundCompressionCacheV3 enc;
    const unsigned written = enc.Encode(
      encoded[l].data(),
      nFrames(waves[l]),
      channels,
      &starts[l],
      1,
      [&](unsigned idx, unsigned ch) { return getSample(waves[l], idx, ch); },
      &snap);
    encoded[l].resize(written);
    reqs[l].encoded = encoded[l].data();
    reqs[l].snapshot = snap;
    reqs[l].channels = channels;
  }

  std::vector<std::vector<int32_t>> scalar(LANES);
  for (unsigned l = 0; l < LANES; l++)
    buildMainV3Scalar(reqs[l], BUILD_SAMPLES, scalar[l]);

  std::vector<std::vector<int32_t>> interleaved;
  buildMainV3BatchInterleaved(reqs, LANES, BUILD_SAMPLES, interleaved);

  for (unsigned l = 0; l < LANES; l++) {
    for (unsigned c = 0; c < channels; c++) {
      for (unsigned i = 0; i < BUILD_SAMPLES; i++) {
        const int32_t want = getSample(waves[l], starts[l] + i, c);
        const int32_t s = scalar[l][(size_t)c * BUILD_SAMPLES + i];
        const int32_t b = interleaved[c][(size_t)i * LANES + l];
        if (s != want || b != want) {
          printf(
            "main builder mismatch lane=%u start=%u sample=%u ch=%u "
            "scalar=%d batch=%d want=%d\n",
            l, starts[l], starts[l] + i, c, s, b, want);
          return false;
        }
      }
    }
  }

  printf(
    "main builders: OK lanes=%u channels=%u samples=%u starts:",
    LANES, channels, BUILD_SAMPLES);
  for (unsigned l = 0; l < LANES; l++)
    printf(" %u", starts[l]);
  printf("\n");

  constexpr unsigned PARTIAL_LANES = 5;
  std::vector<std::vector<int32_t>> partialInterleaved;
  buildMainV3BatchInterleaved(
    reqs, PARTIAL_LANES, BUILD_SAMPLES, partialInterleaved);
  for (unsigned l = 0; l < PARTIAL_LANES; l++) {
    for (unsigned c = 0; c < channels; c++) {
      for (unsigned i = 0; i < BUILD_SAMPLES; i++) {
        const int32_t want = scalar[l][(size_t)c * BUILD_SAMPLES + i];
        const int32_t got = partialInterleaved[c][(size_t)i * LANES + l];
        if (got != want) {
          printf(
            "partial main builder mismatch lane=%u start=%u sample=%u ch=%u "
            "got=%d want=%d\n",
            l, starts[l], starts[l] + i, c, got, want);
          return false;
        }
      }
    }
  }
  printf(
    "partial main builders: OK lanes=%u/%u channels=%u samples=%u\n",
    PARTIAL_LANES, LANES, channels, BUILD_SAMPLES);
  return true;
}

int32_t endSample(unsigned lane, unsigned pos, unsigned ch) {
  return (int32_t)(100000 + lane * 1000 + ch * 100 + pos * 3);
}

bool checkEndSegmentBuilders() {
  constexpr unsigned END_SAMPLES = 256;
  constexpr unsigned START = 37;
  std::vector<std::vector<int32_t>> endData[LANES];
  for (unsigned l = 0; l < LANES; l++) {
    endData[l].assign(2, std::vector<int32_t>(END_SAMPLES));
    for (unsigned c = 0; c < 2; c++)
      for (unsigned i = 0; i < END_SAMPLES; i++)
        endData[l][c][i] = endSample(l, i, c);
  }

  std::vector<std::vector<int32_t>> scalar(LANES);
  for (unsigned l = 0; l < LANES; l++)
    buildEndSegmentScalar(endData[l], START, BUILD_SAMPLES, scalar[l]);

  std::vector<std::vector<int32_t>> interleaved;
  buildEndSegmentInterleaved(endData, START, BUILD_SAMPLES, 2, interleaved);

  for (unsigned l = 0; l < LANES; l++) {
    for (unsigned c = 0; c < 2; c++) {
      for (unsigned i = 0; i < BUILD_SAMPLES; i++) {
        const int32_t want = endSample(l, START + i, c);
        const int32_t s = scalar[l][(size_t)c * BUILD_SAMPLES + i];
        const int32_t b = interleaved[c][(size_t)i * LANES + l];
        if (s != want || b != want) {
          printf(
            "end builder mismatch lane=%u pos=%u ch=%u scalar=%d batch=%d "
            "want=%d\n",
            l, START + i, c, s, b, want);
          return false;
        }
      }
    }
  }

  printf(
    "end builders: OK lanes=%u channels=2 samples=%u start=%u\n",
    LANES, BUILD_SAMPLES, START);
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc <= (int)LANES) {
    printf("usage: %s file1.wav ... file%u.wav\n", argv[0], LANES);
    return 1;
  }

  bool ok = true;
  ok &= checkMainBuilders(argc, argv);
  ok &= checkEndSegmentBuilders();

  printf("\n%s\n", ok ? "builder API checks passed" : "builder API checks failed");
  return ok ? 0 : 1;
}
