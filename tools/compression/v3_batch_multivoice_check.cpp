// v3_batch_multivoice_check.cpp
//
// Validates the multi-voice AVX2 batched decode kernel
// (src/v3codec/GOSoundCompressionCacheV3Batch.h) against
// the REAL single-voice decoder
// (src/v3codec/GOSoundCompressionCacheV3.h), closing the
// gap flagged in INVESTIGATION.md's "Known unknowns": the original
// codec_fleet_benchmark.cpp only ever batched 8 lanes that all started
// decoding together at position 0, so they happened to cross block
// boundaries in lockstep (phase-aligned by construction, never tested
// otherwise). Real GOSoundGroupTask batches are 8 arbitrary voices claimed
// together by whatever worker thread got there first - they are normally
// NOT phase-aligned: different start times, different loop/release
// positions, different pitches. This tool builds lanes from REAL files,
// deliberately phase-offset (each lane starts decoding from a different,
// arbitrary sample position within its own file, like a real mid-flight
// voice would), and verifies the batched kernel still produces bit-exact
// output per lane, plus measures the resulting real speedup. It also
// exercises a PARTIAL batch (5 of 8 lanes) to validate the lane-0
// padding/backup-restore path in GOSoundCompressionCacheV3Batch, which the
// original full-8-lanes-only prototype never touched.
//
// Build:
//   g++ -O3 -mavx2 -ffast-math -funroll-loops -std=gnu++17 \
//     -I <repo>/src/grandorgue  \
//     -I <repo>/src/v3codec v3_batch_multivoice_check.cpp \
//     -o v3_batch_multivoice_check
//
// Usage: v3_batch_multivoice_check file1.wav file2.wav ... (needs >= 8)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "GOSoundCompressionCacheV3.h"
#include "GOSoundCompressionCacheV3Batch.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr unsigned REPS = 7;
constexpr unsigned CONSUMER_REPS = 31;
constexpr unsigned FIR_POINTS = 8;
constexpr unsigned LANES = GOSoundCompressionCacheV3Batch::LANES;

struct WavData {
  unsigned channels;
  unsigned bitsPerSample;
  std::vector<int32_t> samples;
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
    out.samples[i] = v;
    p += bytesPerSample;
  }
  return true;
}

unsigned nFrames(const WavData &w) { return (unsigned)w.samples.size() / w.channels; }
int getSample(const WavData &w, unsigned frame, unsigned ch) {
  return w.samples[(size_t)frame * w.channels + ch];
}

struct TestData {
  std::vector<WavData> waves;
  std::vector<std::vector<unsigned char>> encoded;
  std::vector<unsigned> totalFrames, channels, startFrame;
  unsigned commonChannels;
  unsigned nSamples;
};

bool prepare(int argc, char **argv, TestData &t) {
  t.waves.resize(LANES);
  for (unsigned l = 0; l < LANES; l++) {
    if (!loadWav(argv[1 + l], t.waves[l])) {
      printf("%s: failed to load\n", argv[1 + l]);
      return false;
    }
  }

  // Encode each lane fully (whole file, one continuous stream, as real
  // GOSoundAudioSection::Compress does).
  t.encoded.resize(LANES);
  t.totalFrames.resize(LANES);
  t.channels.resize(LANES);
  for (unsigned l = 0; l < LANES; l++) {
    t.channels[l] = t.waves[l].channels;
    t.totalFrames[l] = nFrames(t.waves[l]);
    t.encoded[l].resize(
      GOSoundCompressionCacheV3::WorstCaseSize(t.totalFrames[l], t.channels[l]));
    GOSoundCompressionCacheV3 enc;
    const unsigned written = enc.Encode(
      t.encoded[l].data(), t.totalFrames[l], t.channels[l], nullptr, 0,
      [&](unsigned idx, unsigned ch) { return getSample(t.waves[l], idx, ch); },
      nullptr);
    t.encoded[l].resize(written);
  }

  // Deliberately NOT phase-aligned: each lane starts decoding from a
  // different arbitrary offset into its own file (simulating real voices
  // claimed together mid-flight, each already at a different position).
  t.commonChannels = *std::min_element(t.channels.begin(), t.channels.end());
  t.startFrame.resize(LANES);
  for (unsigned l = 0; l < LANES; l++) {
    const unsigned span = std::max(1u, t.totalFrames[l] / 2);
    t.startFrame[l] = (unsigned)((l * 2654435761u) % span);
  }
  t.nSamples = *std::min_element(t.totalFrames.begin(), t.totalFrames.end()) / 2 - 1;
  if (t.nSamples <= FIR_POINTS) {
    printf("test files are too short for the %u-tap consumer benchmark\n", FIR_POINTS);
    return false;
  }
  return true;
}

// Runs a bit-exact check of DecodeBatch(numLanes lanes) against sequential
// single-voice DecompressionStep, using lanes [0, numLanes) of `t`.
bool checkBatch(const TestData &t, unsigned numLanes) {
  std::vector<GOSoundCompressionCacheV3> caches(LANES);
  GOSoundCompressionCacheV3Batch::Lane lanes[GOSoundCompressionCacheV3Batch::LANES];
  for (unsigned l = 0; l < numLanes; l++) {
    caches[l].Init();
    caches[l].DecompressTo(t.startFrame[l], t.encoded[l].data(), t.commonChannels);
    lanes[l].cache = &caches[l];
    lanes[l].channels = t.commonChannels;
  }

  std::vector<std::vector<int32_t>> expected(numLanes);
  for (unsigned l = 0; l < numLanes; l++) {
    expected[l].resize((size_t)t.commonChannels * t.nSamples);
    GOSoundCompressionCacheV3 ref;
    ref.Init();
    ref.DecompressTo(t.startFrame[l], t.encoded[l].data(), t.commonChannels);
    for (unsigned i = 0; i < t.nSamples; i++) {
      ref.DecompressionStep(t.commonChannels);
      for (unsigned c = 0; c < t.commonChannels; c++)
        expected[l][(size_t)c * t.nSamples + i] = ref.m_value[c];
    }
  }

  std::vector<std::vector<int32_t>> got(numLanes);
  int32_t *outPtrs[GOSoundCompressionCacheV3Batch::LANES][MAX_OUTPUT_CHANNELS];
  for (unsigned l = 0; l < numLanes; l++) {
    got[l].resize((size_t)t.commonChannels * t.nSamples);
    for (unsigned c = 0; c < t.commonChannels; c++)
      outPtrs[l][c] = got[l].data() + (size_t)c * t.nSamples;
  }

  // lane 0's real state before the call, to verify the partial-batch
  // padding/backup-restore path doesn't corrupt it when numLanes < LANES.
  const GOSoundCompressionCacheV3 lane0Before = caches[0];

  GOSoundCompressionCacheV3Batch::DecodeBatch(lanes, numLanes, t.nSamples, outPtrs);

  bool ok = true;
  for (unsigned l = 0; l < numLanes; l++) {
    for (unsigned c = 0; c < t.commonChannels; c++) {
      for (unsigned i = 0; i < t.nSamples; i++) {
        const int32_t g = got[l][(size_t)c * t.nSamples + i];
        const int32_t e = expected[l][(size_t)c * t.nSamples + i];
        if (g != e) {
          printf(
            "numLanes=%u LANE %u MISMATCH ch %u sample %u: got %d want %d "
            "(startFrame=%u)\n",
            numLanes, l, c, i, g, e, t.startFrame[l]);
          ok = false;
        }
      }
    }
  }

  if (numLanes < LANES) {
    // lane 0 got reprocessed LANES-numLanes extra times as padding; its
    // cache must come back exactly as it would from a single, real,
    // numLanes-times advance (i.e. identical to a full-LANES call would
    // have left it, NOT double-advanced by the padding repeats).
    GOSoundCompressionCacheV3 refLane0 = lane0Before;
    refLane0.DecompressTo(
      t.startFrame[0] + t.nSamples, t.encoded[0].data(), t.commonChannels);
    if (memcmp(&refLane0, &caches[0], sizeof(GOSoundCompressionCacheV3)) != 0) {
      printf(
        "numLanes=%u: lane 0's cache was corrupted by padding "
        "(backup/restore bug)\n",
        numLanes);
      ok = false;
    }
  }

  std::vector<GOSoundCompressionCacheV3> interleavedCaches(LANES);
  GOSoundCompressionCacheV3Batch::Lane interleavedLanes[LANES];
  for (unsigned l = 0; l < numLanes; l++) {
    interleavedCaches[l].Init();
    interleavedCaches[l].DecompressTo(
      t.startFrame[l], t.encoded[l].data(), t.commonChannels);
    interleavedLanes[l].cache = &interleavedCaches[l];
    interleavedLanes[l].channels = t.commonChannels;
  }
  std::vector<std::vector<int32_t>> interleaved(t.commonChannels);
  int32_t *interleavedPtrs[MAX_OUTPUT_CHANNELS] = {};
  for (unsigned c = 0; c < t.commonChannels; c++) {
    interleaved[c].resize((size_t)t.nSamples * LANES);
    interleavedPtrs[c] = interleaved[c].data();
  }
  GOSoundCompressionCacheV3Batch::DecodeBatchInterleaved(
    interleavedLanes, numLanes, t.nSamples, interleavedPtrs);
  for (unsigned l = 0; l < numLanes; l++) {
    for (unsigned c = 0; c < t.commonChannels; c++) {
      for (unsigned i = 0; i < t.nSamples; i++) {
        const int32_t g = interleaved[c][(size_t)i * LANES + l];
        const int32_t e = expected[l][(size_t)c * t.nSamples + i];
        if (g != e) {
          printf(
            "numLanes=%u INTERLEAVED lane %u MISMATCH ch %u sample %u: "
            "got %d want %d (startFrame=%u)\n",
            numLanes, l, c, i, g, e, t.startFrame[l]);
          ok = false;
        }
      }
    }
  }

  printf(
    ok ? "OK: numLanes=%u phase-misaligned, bit-exact over %u samples, "
         "channels=%u\n"
       : "FAILED (numLanes=%u)\n",
    numLanes, t.nSamples, t.commonChannels);
  return ok;
}

void printBatchShape(const TestData &t) {
  unsigned widthCounts[5] = {};
  unsigned chunks = 0;
  unsigned minChunk = t.nSamples;
  unsigned maxChunk = 0;
  unsigned done = 0;
  std::vector<GOSoundCompressionCacheV3> caches(LANES);

  for (unsigned l = 0; l < LANES; l++) {
    caches[l].Init();
    caches[l].DecompressTo(t.startFrame[l], t.encoded[l].data(), t.commonChannels);
  }

  while (done < t.nSamples) {
    for (unsigned l = 0; l < LANES; l++) {
      GOSoundCompressionCacheV3 &c = caches[l];
      if (c.m_blockRemaining == 0) {
        c.m_curWidth = *c.m_ptr;
        c.m_blockData = c.m_ptr + 1;
        c.m_ptr
          = c.m_blockData
          + (size_t)t.commonChannels * GOSoundCompressionCacheV3::BLOCK_SAMPLES
            * c.m_curWidth;
        c.m_blockRemaining = GOSoundCompressionCacheV3::BLOCK_SAMPLES;
      }
    }

    unsigned chunk = t.nSamples - done;
    for (unsigned l = 0; l < LANES; l++)
      chunk = std::min(chunk, caches[l].m_blockRemaining);
    minChunk = std::min(minChunk, chunk);
    maxChunk = std::max(maxChunk, chunk);
    chunks++;

    for (unsigned l = 0; l < LANES; l++) {
      const unsigned w = caches[l].m_curWidth <= 4 ? caches[l].m_curWidth : 4;
      widthCounts[w] += chunk * t.commonChannels;
      caches[l].m_blockRemaining -= chunk;
      caches[l].m_position += chunk;
    }
    done += chunk;
  }

  const unsigned widthTotal
    = widthCounts[1] + widthCounts[2] + widthCounts[3] + widthCounts[4];
  printf(
    "shape: samples=%u channels=%u chunks=%u minChunk=%u maxChunk=%u "
    "widths: 1=%u%% 2=%u%% 3=%u%% 4=%u%%\n",
    t.nSamples,
    t.commonChannels,
    chunks,
    minChunk,
    maxChunk,
    widthTotal ? (100 * widthCounts[1] / widthTotal) : 0,
    widthTotal ? (100 * widthCounts[2] / widthTotal) : 0,
    widthTotal ? (100 * widthCounts[3] / widthTotal) : 0,
    widthTotal ? (100 * widthCounts[4] / widthTotal) : 0);
}

template <class Fn> double bestOf(unsigned reps, Fn fn) {
  double best = 1e30;
  for (unsigned r = 0; r < reps; r++) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
  }
  return best;
}

float consumeSplitScalar(
  const std::vector<std::vector<int32_t>> &split,
  unsigned channels,
  unsigned nSamples) {
  static constexpr float coefs[FIR_POINTS]
    = {0.03125f, -0.0625f, 0.125f, 0.375f, 0.375f, 0.125f, -0.0625f, 0.03125f};
  const unsigned nOut = nSamples - FIR_POINTS;
  float sum = 0.0f;

  // Current scalar shape: one voice at a time; each channel's FIR taps are
  // contiguous, matching StreamCacheReadAheadWindow's planar read-ahead cache.
  for (unsigned l = 0; l < LANES; l++) {
    for (unsigned i = 0; i < nOut; i++) {
      for (unsigned c = 0; c < channels; c++) {
        const int32_t *p = split[l].data() + (size_t)c * nSamples + i;
        float out = 0.0f;
        for (unsigned j = 0; j < FIR_POINTS; j++)
          out += p[j] * coefs[j];
        sum += out;
      }
    }
  }
  return sum;
}

float consumeInterleavedScalar(
  const std::vector<std::vector<int32_t>> &interleaved,
  unsigned channels,
  unsigned nSamples) {
  static constexpr float coefs[FIR_POINTS]
    = {0.03125f, -0.0625f, 0.125f, 0.375f, 0.375f, 0.125f, -0.0625f, 0.03125f};
  const unsigned nOut = nSamples - FIR_POINTS;
  float sum = 0.0f;

  // Compatibility path: one scalar voice reads one lane from the batch cache.
  // Taps are still predictable, but each lane has fixed stride LANES.
  for (unsigned l = 0; l < LANES; l++) {
    for (unsigned i = 0; i < nOut; i++) {
      for (unsigned c = 0; c < channels; c++) {
        const int32_t *p = interleaved[c].data() + (size_t)i * LANES + l;
        float out = 0.0f;
        for (unsigned j = 0; j < FIR_POINTS; j++)
          out += p[(size_t)j * LANES] * coefs[j];
        sum += out;
      }
    }
  }
  return sum;
}

float consumeInterleavedBatch(
  const std::vector<std::vector<int32_t>> &interleaved,
  unsigned channels,
  unsigned nSamples) {
  static constexpr float coefs[FIR_POINTS]
    = {0.03125f, -0.0625f, 0.125f, 0.375f, 0.375f, 0.125f, -0.0625f, 0.03125f};
  const unsigned nOut = nSamples - FIR_POINTS;
  alignas(32) float lanes[LANES];
  float sum = 0.0f;

  // Batch renderer shape: all lanes for a source sample are consumed together.
  // This keeps the same sample-major layout advantage the decoder writes.
  for (unsigned i = 0; i < nOut; i++) {
    for (unsigned c = 0; c < channels; c++) {
      __m256 acc = _mm256_setzero_ps();
      for (unsigned j = 0; j < FIR_POINTS; j++) {
        const __m256 taps = _mm256_cvtepi32_ps(_mm256_loadu_si256(
          (const __m256i *)(interleaved[c].data() + (size_t)(i + j) * LANES)));
        acc = _mm256_add_ps(acc, _mm256_mul_ps(taps, _mm256_set1_ps(coefs[j])));
      }
      _mm256_store_ps(lanes, acc);
      for (unsigned l = 0; l < LANES; l++)
        sum += lanes[l];
    }
  }
  return sum;
}

} // namespace

int main(int argc, char **argv) {
  if (argc <= (int)LANES) {
    printf("usage: %s file1.wav ... file%u.wav (need >= %u files)\n", argv[0], LANES, LANES);
    return 1;
  }

  TestData t;
  if (!prepare(argc, argv, t))
    return 1;

  bool ok = checkBatch(t, LANES);
  ok &= checkBatch(t, 5); // partial batch: exercises lane-0 padding/restore
  if (!ok)
    return 1;
  printBatchShape(t);

  // ---- speed: batched AVX2 vs sequential scalar, full LANES batch ----
  // NOTE: seeking to startFrame via scalar DecompressTo costs O(startFrame)
  // - up to tens of thousands of samples here - and must happen OUTSIDE
  // the timed section, or the seek cost swamps the decode being measured
  // and both numbers come out meaninglessly close (an earlier version of
  // this benchmark made exactly that mistake).
  std::vector<GOSoundCompressionCacheV3> scalarCaches(LANES);
  auto resetScalar = [&] {
    for (unsigned l = 0; l < LANES; l++) {
      scalarCaches[l].Init();
      scalarCaches[l].DecompressTo(
        t.startFrame[l], t.encoded[l].data(), t.commonChannels);
    }
  };
  volatile int32_t sink = 0;
  double scalarSec = 1e30;
  for (unsigned r = 0; r < REPS; r++) {
    resetScalar();
    const auto t0 = Clock::now();
    for (unsigned i = 0; i < t.nSamples; i++)
      for (unsigned l = 0; l < LANES; l++) {
        scalarCaches[l].DecompressionStep(t.commonChannels);
        sink += scalarCaches[l].m_value[0];
      }
    const auto t1 = Clock::now();
    scalarSec = std::min(scalarSec, std::chrono::duration<double>(t1 - t0).count());
  }

  std::vector<GOSoundCompressionCacheV3> batchCaches(LANES);
  GOSoundCompressionCacheV3Batch::Lane lanes[GOSoundCompressionCacheV3Batch::LANES];
  for (unsigned l = 0; l < LANES; l++) {
    lanes[l].cache = &batchCaches[l];
    lanes[l].channels = t.commonChannels;
  }
  std::vector<std::vector<int32_t>> got(LANES);
  int32_t *outPtrs[GOSoundCompressionCacheV3Batch::LANES][MAX_OUTPUT_CHANNELS];
  for (unsigned l = 0; l < LANES; l++) {
    got[l].resize((size_t)t.commonChannels * t.nSamples);
    for (unsigned c = 0; c < t.commonChannels; c++)
      outPtrs[l][c] = got[l].data() + (size_t)c * t.nSamples;
  }
  auto resetBatch = [&] {
    for (unsigned l = 0; l < LANES; l++) {
      batchCaches[l].Init();
      batchCaches[l].DecompressTo(
        t.startFrame[l], t.encoded[l].data(), t.commonChannels);
    }
  };
  double batchSec = 1e30;
  for (unsigned r = 0; r < REPS; r++) {
    resetBatch();
    const auto t0 = Clock::now();
    GOSoundCompressionCacheV3Batch::DecodeBatch(lanes, LANES, t.nSamples, outPtrs);
    const auto t1 = Clock::now();
    batchSec = std::min(batchSec, std::chrono::duration<double>(t1 - t0).count());
    sink += outPtrs[0][0][0];
  }

  std::vector<GOSoundCompressionCacheV3> interleavedCaches(LANES);
  GOSoundCompressionCacheV3Batch::Lane interleavedLanes[LANES];
  for (unsigned l = 0; l < LANES; l++) {
    interleavedLanes[l].cache = &interleavedCaches[l];
    interleavedLanes[l].channels = t.commonChannels;
  }
  std::vector<std::vector<int32_t>> interleaved(t.commonChannels);
  int32_t *interleavedPtrs[MAX_OUTPUT_CHANNELS] = {};
  for (unsigned c = 0; c < t.commonChannels; c++) {
    interleaved[c].resize((size_t)t.nSamples * LANES);
    interleavedPtrs[c] = interleaved[c].data();
  }
  auto resetInterleaved = [&] {
    for (unsigned l = 0; l < LANES; l++) {
      interleavedCaches[l].Init();
      interleavedCaches[l].DecompressTo(
        t.startFrame[l], t.encoded[l].data(), t.commonChannels);
    }
  };
  double interleavedSec = 1e30;
  for (unsigned r = 0; r < REPS; r++) {
    resetInterleaved();
    const auto t0 = Clock::now();
    GOSoundCompressionCacheV3Batch::DecodeBatchInterleaved(
      interleavedLanes, LANES, t.nSamples, interleavedPtrs);
    const auto t1 = Clock::now();
    interleavedSec
      = std::min(interleavedSec, std::chrono::duration<double>(t1 - t0).count());
    sink += interleavedPtrs[0][0];
  }

  volatile float consumerSink = 0.0f;
  const double splitScalarConsumerSec = bestOf(CONSUMER_REPS, [&] {
    consumerSink += consumeSplitScalar(got, t.commonChannels, t.nSamples);
  });
  const double interleavedScalarConsumerSec = bestOf(CONSUMER_REPS, [&] {
    consumerSink
      += consumeInterleavedScalar(interleaved, t.commonChannels, t.nSamples);
  });
  const double interleavedBatchConsumerSec = bestOf(CONSUMER_REPS, [&] {
    consumerSink
      += consumeInterleavedBatch(interleaved, t.commonChannels, t.nSamples);
  });

  const double scalarNs = scalarSec * 1e9 / (t.nSamples * (double)LANES * t.commonChannels);
  const double batchNs = batchSec * 1e9 / (t.nSamples * (double)LANES * t.commonChannels);
  const double interleavedNs
    = interleavedSec * 1e9 / (t.nSamples * (double)LANES * t.commonChannels);
  const double consumerSamples = (t.nSamples - FIR_POINTS) * (double)LANES
    * t.commonChannels;
  const double splitScalarConsumerNs = splitScalarConsumerSec * 1e9 / consumerSamples;
  const double interleavedScalarConsumerNs
    = interleavedScalarConsumerSec * 1e9 / consumerSamples;
  const double interleavedBatchConsumerNs
    = interleavedBatchConsumerSec * 1e9 / consumerSamples;
  printf(
    "speed: scalar(%ux sequential)=%.3f ns/channel-sample  "
    "batch-split(AVX2)=%.3f speedup=%.2fx  "
    "batch-interleaved(AVX2)=%.3f speedup=%.2fx\n",
    LANES,
    scalarNs,
    batchNs,
    scalarNs / batchNs,
    interleavedNs,
    scalarNs / interleavedNs);
  printf(
    "consumer: split-scalar=%.3f ns/channel-sample  "
    "interleaved-scalar=%.3f slowdown=%.2fx  "
    "interleaved-batch=%.3f speedup-vs-split=%.2fx\n",
    splitScalarConsumerNs,
    interleavedScalarConsumerNs,
    interleavedScalarConsumerNs / splitScalarConsumerNs,
    interleavedBatchConsumerNs,
    splitScalarConsumerNs / interleavedBatchConsumerNs);
  printf(
    "pipeline: split-decode+scalar-consume=%.3f ns/channel-sample  "
    "interleaved-decode+scalar-consume=%.3f  "
    "interleaved-decode+batch-consume=%.3f\n",
    batchNs + splitScalarConsumerNs,
    interleavedNs + interleavedScalarConsumerNs,
    interleavedNs + interleavedBatchConsumerNs);
  (void)sink;
  (void)consumerSink;
  return 0;
}
