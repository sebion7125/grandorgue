/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOTestSoundStream.h"

#include <cmath>
#include <cstdlib>
#include <vector>

#include <wx/file.h>
#include <wx/filename.h>
#include <wx/wfstream.h>

#include "loader/cache/GOCache.h"
#include "loader/cache/GOCacheWriter.h"
#include "sound/playing/GOSoundAudioSection.h"
#include "sound/playing/GOSoundResampleBatch.h"
#include "sound/playing/GOSoundStream.h"
#include "sound/playing/GOSoundSubblockGrid.h"

#include "GOInt.h"
#include "GOWave.h"
#include "GOWaveLoop.h"

const std::string GOTestSoundStream::TEST_NAME = "GOTestSoundStream";

static constexpr unsigned SECTION_RATE = 48000;
static constexpr float SAMPLE_RATE_ADJUSTMENT = 1.0f / SECTION_RATE;
static constexpr unsigned N_FRAMES = 500;
static constexpr unsigned N_ATTACK_FRAMES = 100;
static constexpr unsigned N_FRAMES_PER_BLOCK = 50;
static constexpr unsigned N_BUFFER_ITEMS = N_FRAMES_PER_BLOCK * 2;

/* In production GOSoundSamplerPool owns each voice's render-block sample
 * storage and assigns it to the pooled stream (see
 * GOSoundStream::SetBlockData); tests construct bare GOSoundStream objects,
 * so each subblock-path stream gets its storage attached from here. Must be
 * attached BEFORE InitStream - ApplySubblockBin() opts the voice out of the
 * subblock path when no storage is assigned. Requires
 * GOSoundSubblockBins::SetBase() to have been called (see run()). */
struct TestVoiceBlockStorage {
  std::vector<int> data;

  void Attach(GOSoundStream &stream) {
    const unsigned stride
      = GOSoundSubblockBins::GetMaxBin() + GOSoundStream::GUARD_SAMPLES;

    data.assign((size_t)MAX_OUTPUT_CHANNELS * stride, 0);
    stream.SetBlockData(data.data(), stride);
  }
};

std::unique_ptr<GOSoundAudioSection> GOTestSoundStream::CreateAudioSection(
  unsigned nChannels,
  unsigned nFrames,
  SectionFormat format,
  const GOWaveLoop *pLoop,
  unsigned uncompressedPrefixSamples) {
  const unsigned nSamples = nChannels * nFrames;
  std::vector<GOInt24> pcmData(nSamples);
  std::vector<GOWaveLoop> loops;

  /* Sawtooth with period 40 and step 1. The small amplitude is deliberate:
   * these tests need per-position discriminable values (any stream position
   * error shows up in the bit-exact comparisons), not realistic amplitudes.
   * V3's block format always reserves full BLOCK_SAMPLES slots per channel,
   * so on sections this short (a few hundred samples = one partial block)
   * any residual needing >1 byte makes the compressed size exceed the raw
   * PCM and V3 falls back to uncompressed - which would silently turn the
   * V3 test cases into no-ops. The largest residual is the post-wrap
   * transient, which grows with the predictor order (~period for order 2,
   * ~2x for order 3, ~3x for order 4 - binomial tap weights). The suite
   * runs all orders 1-4 (cache v11), so the period must keep ORDER 4's
   * transient within 1 signed byte: 3 * 40 = 120 <= 127. (History: 80 for
   * the order-2 era, 60 for the fixed-order-3 era.) */
  for (unsigned i = 0; i < nSamples; i++)
    pcmData[i] = i % 40;

  if (pLoop)
    loops.push_back(*pLoop);

  auto pSection = std::make_unique<GOSoundAudioSection>(m_pool);

  pSection->Setup(
    nullptr,
    nullptr,
    pcmData.data(),
    GOWave::SF_SIGNEDINT24_24,
    nChannels,
    SECTION_RATE,
    nFrames,
    &loops,
    BOOL3_DEFAULT,
    format != SectionFormat::Uncompressed,
    format == SectionFormat::V3,
    0,
    0,
    uncompressedPrefixSamples);
  if (format == SectionFormat::V3)
    // Compression may silently fall back (early abort) if it doesn't pay
    // off; for V3 test coverage that would mean testing nothing, so fail
    // loudly instead.
    GOAssert(
      pSection->GetCompressionCodec()
        == GOSoundAudioSectionCompressionCodec::V3,
      "test section unexpectedly fell back from V3 compression");
  return pSection;
}

void GOTestSoundStream::TestReadBlock(
  GOSoundResample::InterpolationType interpolationType,
  SectionFormat format,
  unsigned nChannels) {
  const std::string label = GOFormat(
    "interpolation={} format={} channels={}",
    interpolationType == GOSoundResample::GO_LINEAR_INTERPOLATION ? "linear"
                                                                  : "polyphase",
    format == SectionFormat::Uncompressed ? "uncompressed"
      : format == SectionFormat::Legacy   ? "legacy"
                                          : "v3",
    nChannels);
  const auto pSection = CreateAudioSection(nChannels, N_FRAMES, format);
  GOSoundResample resample;
  GOSoundStream stream;

  stream.InitStream(
    &resample, pSection.get(), interpolationType, SAMPLE_RATE_ADJUSTMENT);

  float buffer[N_BUFFER_ITEMS];

  for (unsigned frameI = 0; frameI < N_FRAMES; frameI += N_FRAMES_PER_BLOCK) {
    const bool result = stream.ReadBlock(buffer, N_FRAMES_PER_BLOCK);

    GOAssert(
      result,
      GOFormat(
        "ReadBlock should return true while frames available ({})", label));
  }

  // ReadBlock should return false after exhaustion
  const bool resultAfterEnd = stream.ReadBlock(buffer, N_FRAMES_PER_BLOCK);

  GOAssert(
    !resultAfterEnd,
    GOFormat("ReadBlock should return false after exhaustion ({})", label));

  // Buffer should be filled with zeros after end
  for (unsigned i = 0; i < N_BUFFER_ITEMS; i++)
    GOAssert(
      buffer[i] == 0.0f,
      GOFormat(
        "Buffer[{}] should be 0.0f after end ({}, got: {})",
        i,
        label,
        buffer[i]));

  // TODO: verify decoded frame values
}

void GOTestSoundStream::TestLoopedStreamAlwaysReturnsTrue() {
  constexpr unsigned N_ITERATIONS = 100;
  const GOWaveLoop loop = {100, 499};
  const auto pSection
    = CreateAudioSection(1, N_FRAMES, SectionFormat::Uncompressed, &loop);
  GOSoundResample resample;
  GOSoundStream stream;

  stream.InitStream(
    &resample,
    pSection.get(),
    GOSoundResample::GO_LINEAR_INTERPOLATION,
    SAMPLE_RATE_ADJUSTMENT);

  float buffer[N_BUFFER_ITEMS];

  for (unsigned iterI = 0; iterI < N_ITERATIONS; iterI++) {
    const bool result = stream.ReadBlock(buffer, N_FRAMES_PER_BLOCK);

    GOAssert(
      result,
      GOFormat(
        "ReadBlock should always return true for looped stream (iteration {})",
        iterI));
  }
}

void GOTestSoundStream::TestInitAlignedStream() {
  /* Also runs with a V3 release: SetupStreamAlignment's ComputeTable scans
   * the release via GetSample() with a caller-owned reader - the exact
   * load-time path that froze at 100% CPU on real organs when V3 sections
   * forced GetSample() onto its per-call cross-codec bridge (O(n^2)). */
  for (SectionFormat format :
       {SectionFormat::Uncompressed, SectionFormat::V3}) {
    const auto pAttack = CreateAudioSection(1, N_FRAMES, format);
    const auto pRelease = CreateAudioSection(1, N_FRAMES, format);

    pRelease->SetupStreamAlignment({pAttack.get()}, 0);

    GOSoundResample resample;
    GOSoundStream attackStream;

    attackStream.InitStream(
      &resample,
      pAttack.get(),
      GOSoundResample::GO_LINEAR_INTERPOLATION,
      SAMPLE_RATE_ADJUSTMENT);

    float buffer[N_BUFFER_ITEMS];

    for (unsigned frameI = 0; frameI < N_ATTACK_FRAMES;
         frameI += N_FRAMES_PER_BLOCK)
      attackStream.ReadBlock(buffer, N_FRAMES_PER_BLOCK);

    GOSoundStream releaseStream;

    releaseStream.InitAlignedStream(
      pRelease.get(), GOSoundResample::GO_LINEAR_INTERPOLATION, &attackStream);

    const bool result = releaseStream.ReadBlock(buffer, N_FRAMES_PER_BLOCK);

    GOAssert(result, "ReadBlock should return true after InitAlignedStream");
  }
}

void GOTestSoundStream::TestV3CacheSaveLoadRoundtrip() {
  /* Full SaveCache()/LoadCache() roundtrip of a V3 section through a real
   * cache file (the same GOCacheWriter/GOCache machinery real organs use),
   * covering the explicit-header V3 payload incl. StartSegmentV3
   * snapshots. The loaded section must play back bit-identically to the
   * original - this is exactly the path that silently broke once before
   * (double m_IsCompressed read), so it gets its own regression coverage. */
  constexpr unsigned N_TOTAL_FRAMES = 1400;
  constexpr unsigned N_ITERATIONS = 100;
  const GOWaveLoop loop = {600, 1299};
  const unsigned nChannels = 2;
  constexpr unsigned ROUNDTRIP_PREFIX = 200;
  const auto pOriginal = CreateAudioSection(
    nChannels, N_TOTAL_FRAMES, SectionFormat::V3, &loop, ROUNDTRIP_PREFIX);

  GOAssert(
    pOriginal->GetUncompressedPrefixSamples() == ROUNDTRIP_PREFIX,
    "roundtrip section should carry an uncompressed prefix");

  const wxString tmpPath = wxFileName::CreateTempFileName(wxT("gotest_cache"));

  {
    wxFileOutputStream outStream(tmpPath);

    GOAssert(outStream.IsOk(), "temp cache file could not be created");

    GOCacheWriter writer(outStream, false);

    GOAssert(writer.WriteHeader(), "cache header write failed");
    GOAssert(pOriginal->SaveCache(writer), "V3 SaveCache failed");
    writer.Close();
  }

  /* The loaded section needs its OWN pool: GOCache's mapable path calls
   * GOMemoryPool::SetCacheFile(), which frees/unmaps the pool's existing
   * content first (fine in real GO, where the cache is attached before
   * anything is loaded into the pool) - reusing m_pool here would unmap
   * pOriginal's data mid-test. */
  GOMemoryPool loadPool;
  auto pLoaded = std::make_unique<GOSoundAudioSection>(loadPool);

  {
    wxFile inFile(tmpPath);

    GOAssert(inFile.IsOpened(), "temp cache file could not be reopened");

    GOCache reader(inFile, loadPool);

    GOAssert(reader.ReadHeader(), "cache header read failed");
    GOAssert(pLoaded->LoadCache(reader), "V3 LoadCache failed");
    reader.Close();
  }
  /* Deliberately NOT removed before the reads below: the loaded section's
   * data may live in the pool's read-only mapping of this very file.
   * Removal happens at the end of the test. */

  GOAssert(
    pLoaded->GetCompressionCodec() == GOSoundAudioSectionCompressionCodec::V3,
    "loaded section lost its V3 codec");
  GOAssert(
    pLoaded->GetUncompressedPrefixSamples()
        == pOriginal->GetUncompressedPrefixSamples()
      && pLoaded->GetUncompressedPrefixStoredSamples()
        == pOriginal->GetUncompressedPrefixStoredSamples(),
    "loaded section lost its uncompressed prefix metadata");
  GOAssert(
    pLoaded->GetSubblockSeedCountV3() == pOriginal->GetSubblockSeedCountV3()
      && pLoaded->GetSubblockSeedCountV3() > 0,
    "subblock seed snapshots should roundtrip through the cache file");

  // Looped playback through the full stream path must match bit-exactly.
  auto capture = [&](const GOSoundAudioSection *pSection) {
    GOSoundResample resample;
    GOSoundStream stream;

    std::srand(0);
    stream.InitStream(
      &resample,
      pSection,
      GOSoundResample::GO_LINEAR_INTERPOLATION,
      SAMPLE_RATE_ADJUSTMENT);

    std::vector<float> captured(N_ITERATIONS * N_BUFFER_ITEMS);
    float buffer[N_BUFFER_ITEMS];

    for (unsigned iterI = 0; iterI < N_ITERATIONS; iterI++) {
      stream.ReadBlock(buffer, N_FRAMES_PER_BLOCK);
      for (unsigned i = 0; i < N_BUFFER_ITEMS; i++)
        captured[iterI * N_BUFFER_ITEMS + i] = buffer[i];
    }
    return captured;
  };
  const std::vector<float> original = capture(pOriginal.get());
  const std::vector<float> loaded = capture(pLoaded.get());

  for (unsigned i = 0; i < original.size(); i++)
    GOAssert(
      loaded[i] == original[i],
      GOFormat(
        "loaded/original playback mismatch at output sample {}: {} != {}",
        i,
        loaded[i],
        original[i]));

  /* All reads done; drop the loaded section before removing the file its
   * pool mapping is backed by. */
  pLoaded.reset();
  wxRemoveFile(tmpPath);
}

void GOTestSoundStream::TestVoiceBlockBatchMatchesLazy() {
  /* Per-voice decoded render blocks (2026-07-08 redesign): each
   * GOSoundStream owns its block privately - GOSoundGroupTask's decode
   * pass groups voices needing a rebuild into batches of up to
   * GOSoundCompressionCacheV3Batch::LANES and builds them via
   * GOSoundStream::BuildBlocksBatch (AVX2 kernel where available); a
   * voice that never gets pre-built still produces identical output via
   * the lazy, local, synchronous rebuild inside the render window
   * (BuildBlockScalar). This is the one behavior not already covered by
   * TestCompressedLoopWrapMatchesUncompressed (which only ever drives a
   * single stream, so it never exercises the batch call at all): prove
   * BuildBlocksBatch and lazy on-demand building are bit-exact
   * equivalent, including the short-final-subblock-forces-scalar case
   * mixed into the same batch call as normal-length ones. */
  constexpr unsigned N_ITERATIONS = 400;
  const unsigned nChannels = 2;
  const GOWaveLoop loopA = {700, 2399}; // loop start off any block boundary
  // pB has no loop: its MainSegment's last subblock before transition_position
  // is a short final one (validSamples < bin) - the batch-call fallback path.

  GOSoundStream::SetUseSubblockCache(true);

  const auto pA = CreateAudioSection(nChannels, 2500, SectionFormat::V3, &loopA);
  const auto pB = CreateAudioSection(nChannels, 1500, SectionFormat::V3);
  const GOSoundAudioSection *sections[] = {pA.get(), pA.get(), pB.get(), pA.get()};
  constexpr unsigned N_VOICES = sizeof(sections) / sizeof(sections[0]);

  GOSoundResample resample;
  GOSoundStream prebuilt[N_VOICES];
  GOSoundStream lazy[N_VOICES];
  TestVoiceBlockStorage storage[2][N_VOICES];

  for (unsigned v = 0; v < N_VOICES; v++) {
    // Deliberately phase-misaligned start positions (like real voices
    // claimed into a batch mid-flight, not all starting together).
    storage[0][v].Attach(prebuilt[v]);
    prebuilt[v].InitStream(
      &resample, sections[v], GOSoundResample::GO_POLYPHASE_INTERPOLATION,
      SAMPLE_RATE_ADJUSTMENT);
    storage[1][v].Attach(lazy[v]);
    lazy[v].InitStream(
      &resample, sections[v], GOSoundResample::GO_POLYPHASE_INTERPOLATION,
      SAMPLE_RATE_ADJUSTMENT);
    GOAssert(
      prebuilt[v].UsesSubblockCache() && lazy[v].UsesSubblockCache(),
      "test streams should be on the per-voice subblock path");
  }

  float prebuiltBuf[N_BUFFER_ITEMS];
  float lazyBuf[N_BUFFER_ITEMS];

  for (unsigned iterI = 0; iterI < N_ITERATIONS; iterI++) {
    // Mimic GOSoundGroupTask::RunSubblockDecodePass: collect voices
    // needing a rebuild this period and force them through ONE batch call
    // (count >= 2), engaging the AVX2 kernel where available - the point
    // of this test is exercising that call, not lazily falling back to
    // one-at-a-time scalar builds.
    GOSoundStream *pending[N_VOICES];
    unsigned starts[N_VOICES];
    unsigned valids[N_VOICES];
    unsigned nPending = 0;

    for (unsigned v = 0; v < N_VOICES; v++) {
      unsigned start;
      unsigned valid;

      if (prebuilt[v].NeedsBlockRebuild(start, valid)) {
        pending[nPending] = &prebuilt[v];
        starts[nPending] = start;
        valids[nPending] = valid;
        nPending++;
      }
    }
    if (nPending)
      // All voices here share one pitch factor, hence one ladder rung -
      // the same-rung precondition BuildBlocksBatch requires of a group.
      GOSoundStream::BuildBlocksBatch(
        pending, starts, valids, nPending, nChannels);

    for (unsigned v = 0; v < N_VOICES; v++) {
      prebuilt[v].ReadBlock(prebuiltBuf, N_FRAMES_PER_BLOCK);
      lazy[v].ReadBlock(lazyBuf, N_FRAMES_PER_BLOCK);
      for (unsigned i = 0; i < N_BUFFER_ITEMS; i++)
        GOAssert(
          prebuiltBuf[i] == lazyBuf[i],
          GOFormat(
            "batch-prebuilt/lazy mismatch: voice {} iteration {} sample {}: "
            "prebuilt={} lazy={}",
            v,
            iterI,
            i,
            prebuiltBuf[i],
            lazyBuf[i]));
    }
  }
  GOSoundStream::SetUseSubblockCache(false);
}

void GOTestSoundStream::TestVoiceBlockPredictedRebuildMatchesUncompressed() {
  /* NeedsBlockRebuild-driven pre-build (what GOSoundGroupTask's decode
   * pass does every period) must never change what gets played, at
   * several pitches - compared bit-exactly against an uncompressed
   * reference, the same differential technique used throughout this
   * file. The pitches are chosen against the bin ladder (base =
   * N_FRAMES_PER_BLOCK, top rung = 4x base): 1.0/0.8 land on the first
   * rung, 1.25 on a higher one, 3.5 near the top, and 4.5 exceeds it -
   * the voice must then OPT OUT of the subblock path at note start and
   * still play bit-exactly through the direct V3 path. Loop wraps are
   * included; with the segment-anchored grid, the first rebuild after a
   * wrap must start exactly at the loop start (700). */
  constexpr unsigned N_ITERATIONS = 300;
  constexpr unsigned LOOP_START = 700;
  const unsigned nChannels = 2;
  const GOWaveLoop loop = {LOOP_START, 2399};
  const float pitches[] = {1.0f, 1.25f, 0.8f, 3.5f, 4.5f};

  for (const float pitch : pitches) {
    const bool expectSubblockPath
      = pitch <= (float)GOSoundSubblockBins::MAX_FACTOR;
    const auto pUncompressed
      = CreateAudioSection(nChannels, 2500, SectionFormat::Uncompressed, &loop);
    const auto pV3
      = CreateAudioSection(nChannels, 2500, SectionFormat::V3, &loop);

    GOSoundResample resample;
    GOSoundStream uncompressedStream;
    GOSoundStream v3Stream;
    TestVoiceBlockStorage storage;

    std::srand(0);
    uncompressedStream.InitStream(
      &resample, pUncompressed.get(), GOSoundResample::GO_LINEAR_INTERPOLATION,
      pitch * SAMPLE_RATE_ADJUSTMENT);
    std::srand(0);
    GOSoundStream::SetUseSubblockCache(true);
    storage.Attach(v3Stream);
    v3Stream.InitStream(
      &resample, pV3.get(), GOSoundResample::GO_LINEAR_INTERPOLATION,
      pitch * SAMPLE_RATE_ADJUSTMENT);
    GOAssert(
      v3Stream.UsesSubblockCache() == expectSubblockPath,
      GOFormat(
        "pitch {}: stream should be {} the per-voice subblock path",
        pitch,
        expectSubblockPath ? "on" : "opted out of"));

    float uncompressedBuf[N_BUFFER_ITEMS];
    float v3Buf[N_BUFFER_ITEMS];
    const uint64_t fallbacksBefore
      = GOSoundStream::s_SyncFallbacks.load(std::memory_order_relaxed);

    for (unsigned iterI = 0; iterI < N_ITERATIONS; iterI++) {
      unsigned start;
      unsigned valid;

      if (v3Stream.NeedsBlockRebuild(start, valid)) {
        GOSoundStream *single[1] = {&v3Stream};
        unsigned starts[1] = {start};
        unsigned valids[1] = {valid};

        // count==1 deliberately: exercises the solo/scalar path of
        // BuildBlocksBatch (a real period can have exactly one voice
        // needing a rebuild), not just the multi-lane one.
        GOSoundStream::BuildBlocksBatch(single, starts, valids, 1, nChannels);
      }
      uncompressedStream.ReadBlock(uncompressedBuf, N_FRAMES_PER_BLOCK);
      v3Stream.ReadBlock(v3Buf, N_FRAMES_PER_BLOCK);
      for (unsigned i = 0; i < N_BUFFER_ITEMS; i++)
        GOAssert(
          v3Buf[i] == uncompressedBuf[i],
          GOFormat(
            "predicted-rebuild/uncompressed mismatch (pitch={}) at output "
            "sample {} (iteration {}, offset {}): v3={} uncompressed={}",
            pitch,
            i,
            i / N_BUFFER_ITEMS,
            i % N_BUFFER_ITEMS,
            v3Buf[i],
            uncompressedBuf[i]));
    }

    /* Callback-aligned blocks: the decode pass covers every plain-forward
     * callback completely, so synchronous mid-render fallbacks may occur
     * for loop wraps landing mid-callback ONLY - bounded by the number of
     * loop passes, far below one per callback (the failure mode of the
     * former fixed-grid designs, where drifting block boundaries made
     * nearly EVERY callback fall back; that regression is what this
     * bound catches). Wraps per iteration ~= consumption / loop length. */
    if (expectSubblockPath) {
      const uint64_t fallbacks
        = GOSoundStream::s_SyncFallbacks.load(std::memory_order_relaxed)
        - fallbacksBefore;
      const uint64_t maxWraps = (uint64_t)(
        (double)N_ITERATIONS * N_FRAMES_PER_BLOCK * pitch / (2400 - LOOP_START)
        + 2);

      GOAssert(
        fallbacks <= maxWraps,
        GOFormat(
          "pitch {}: {} mid-render fallbacks for at most {} loop wraps - "
          "the decode pass is missing plain-forward callbacks",
          pitch,
          (unsigned)fallbacks,
          (unsigned)maxWraps));
    }
    GOSoundStream::SetUseSubblockCache(false);
  }
}

#ifdef GO_RESAMPLE_BATCH_AVX2
/* One frame of the scalar reference for TestBatchRenderBitExact, isolated
 * in its own non-inlinable function with pinned FP semantics: fmaf per
 * tap (the kernel's explicit FMA), plain mul for the gain, the kernel's
 * fixed hsum tree - and NO fusing/vectorizing transformations by the
 * surrounding optimizer (inlined into the test loop, gcc's
 * -ffp-contract=fast re-fused the gain multiply into the tree adds
 * whenever the intermediate values weren't observed - a genuine
 * heisenbug: adding a debug printf made the test pass). */
__attribute__((noinline
#ifndef __clang__
               ,
               optimize("fp-contract=off", "no-tree-vectorize")
#endif
               )) static void
computeReferenceFrame(
  const GOSoundAudioSection *const refSections[],
  const unsigned refIdx[],
  const unsigned refFrac[],
  const float refGain[],
  unsigned nVoices,
  unsigned nChannels,
  const GOSoundResample &resample,
  float laneAccOut[][MAX_OUTPUT_CHANNELS],
  float frameSumOut[]) {
  constexpr unsigned LANES = GOSoundResampleBatch::LANES;
  constexpr unsigned TAPS = GOSoundResampleBatch::TAPS;

  for (unsigned v = 0; v < LANES; v++) {
    // Padded lanes mirror lane 0's position with zero gain.
    const unsigned src = v < nVoices ? v : 0;
    const float(&coefRow)[TAPS] = resample.m_PolyphaseCoefs[refFrac[src]];

    for (unsigned c = 0; c < nChannels; c++) {
      float acc = 0.0f;

      for (unsigned t = 0; t < TAPS; t++)
        acc = std::fmaf(
          (float)refSections[src]->GetSample(refIdx[src] + t, c),
          coefRow[t],
          acc);
      laneAccOut[v][c] = acc;
    }
  }
  for (unsigned c = 0; c < nChannels; c++) {
    // The kernel's hsum tree over lane contributions a_v = acc * gain
    float a[LANES];

    for (unsigned v = 0; v < LANES; v++)
      a[v] = laneAccOut[v][c] * (v < nVoices ? refGain[v] : 0.0f);

    const float s0 = a[0] + a[4];
    const float s1 = a[1] + a[5];
    const float s2 = a[2] + a[6];
    const float s3 = a[3] + a[7];

    frameSumOut[c] = (s0 + s1) + (s2 + s3);
  }
}
#endif // GO_RESAMPLE_BATCH_AVX2

void GOTestSoundStream::TestBatchRenderBitExact() {
#ifndef GO_RESAMPLE_BATCH_AVX2
  wxLogWarning("TestBatchRenderBitExact: no AVX2 kernel on this platform");
  return;
#else
  if (!GOSoundResampleBatch::CpuSupported()) {
    wxLogWarning("TestBatchRenderBitExact: CPU has no AVX2 - skipped");
    return;
  }

  constexpr unsigned LANES = GOSoundResampleBatch::LANES;
  constexpr unsigned TAPS = GOSoundResampleBatch::TAPS;
  constexpr unsigned N_ITERATIONS = 40;

  /* Three group shapes: full-ish stereo (4 lanes), partial stereo
   * (3 lanes), mono (4 lanes). Pitches 1.0 and 0.99 share the base
   * ladder rung (needed stays within it) but give the lanes genuinely
   * different fraction phases. Voice 1 runs a linear fade-out (Ramp
   * gains), voice 2 (where present) is a zero-gain lane whose raw
   * samples are picked from laneOut - the classic-fader detour. */
  struct GroupShape {
    unsigned nChannels;
    unsigned nVoices;
  };
  const GroupShape shapes[] = {{2, 4}, {2, 3}, {1, 4}};

  for (const GroupShape &shape : shapes) {
    const float pitches[] = {1.0f, 0.99f, 1.0f, 0.995f};
    const float volumes[] = {0.8f, 1.0f, 0.5f, 0.9f};
    const unsigned nFrames = N_FRAMES_PER_BLOCK;

    GOSoundStream::SetUseSubblockCache(true);

    std::vector<std::unique_ptr<GOSoundAudioSection>> v3Sections;
    std::vector<std::unique_ptr<GOSoundAudioSection>> refSections;
    GOSoundResample resample;
    GOSoundStream streams[LANES];
    TestVoiceBlockStorage storage[LANES];
    GOSoundFader faders[LANES];
    // Reference gain state, advanced additively exactly like the kernel.
    float refGain[LANES];
    float refDg[LANES];

    for (unsigned v = 0; v < shape.nVoices; v++) {
      v3Sections.push_back(
        CreateAudioSection(shape.nChannels, 6000, SectionFormat::V3));
      refSections.push_back(
        CreateAudioSection(shape.nChannels, 6000, SectionFormat::Uncompressed));
      storage[v].Attach(streams[v]);
      streams[v].InitStream(
        &resample,
        v3Sections[v].get(),
        GOSoundResample::GO_POLYPHASE_INTERPOLATION,
        pitches[v] * SAMPLE_RATE_ADJUSTMENT);
      GOAssert(
        streams[v].UsesSubblockCache(),
        "batch render test stream should be on the subblock path");
      faders[v].Setup(0.001f, 1.0f, 0);
      if (v == 1)
        faders[v].StartDecreasingVolume(nFrames * (N_ITERATIONS / 2), true);
    }

    // Reference resampling positions, advanced with the same integer
    // arithmetic as the kernel.
    unsigned refIdx[LANES] = {0};
    unsigned refFrac[LANES] = {0};

    for (unsigned iterI = 0; iterI < N_ITERATIONS; iterI++) {
      GOSoundStream *groupStreams[LANES];
      unsigned starts[LANES];
      unsigned valids[LANES];
      GOSoundResampleBatch::Lane lanes[LANES];

      for (unsigned v = 0; v < shape.nVoices; v++) {
        unsigned start;
        unsigned valid;
        const GOSoundStream::RenderClass cls
          = streams[v].ClassifyForRender(start, valid);

        GOAssert(
          cls == GOSoundStream::RenderClass::BatchRender,
          GOFormat(
            "voice {} iteration {}: expected BatchRender classification",
            v,
            iterI));
        // Seamless chain: this callback's block starts exactly at the
        // reference position (which trails the same arithmetic).
        GOAssert(
          start == refIdx[v],
          GOFormat(
            "voice {} iteration {}: start {} != reference position {}",
            v,
            iterI,
            start,
            refIdx[v]));
        groupStreams[v] = &streams[v];
        starts[v] = start;
        valids[v] = valid;
        lanes[v].rel = 0;
        lanes[v].fraction = streams[v].GetResamplingFraction();
        lanes[v].increment = streams[v].GetResamplingIncrement();
        if (v == 2) {
          // Zero-gain lane: contributes nothing to the mix; its raw
          // samples come from laneOut below.
          lanes[v].g0 = 0.0f;
          lanes[v].dg = 0.0f;
          refGain[v] = 0.0f;
          refDg[v] = 0.0f;
        } else {
          const GOFadeKernelKind kind = faders[v].ComputeFrameGains(
            nFrames, volumes[v], lanes[v].g0, lanes[v].dg);

          (void)kind;
          refGain[v] = lanes[v].g0;
          refDg[v] = lanes[v].dg;
        }
      }

      const unsigned maxTotal
        = *std::max_element(valids, valids + shape.nVoices) + TAPS;
      std::vector<int32_t> scratchStore(
        (size_t)MAX_OUTPUT_CHANNELS * maxTotal * LANES);
      std::vector<float> laneOutStore(
        (size_t)MAX_OUTPUT_CHANNELS * nFrames * LANES);
      int32_t *const scratch[MAX_OUTPUT_CHANNELS]
        = {scratchStore.data(), scratchStore.data() + (size_t)maxTotal * LANES};
      float *const laneOut[MAX_OUTPUT_CHANNELS]
        = {laneOutStore.data(),
           laneOutStore.data() + (size_t)nFrames * LANES};
      std::vector<float> output(nFrames * 2, 0.0f);
      std::vector<float> expected(nFrames * 2, 0.0f);

      GOSoundStream::DecodeGroupToScratch(
        groupStreams, starts, valids, shape.nVoices, shape.nChannels, scratch);
      GOSoundResampleBatch::RenderAccumulate(
        lanes,
        shape.nVoices,
        scratch,
        shape.nChannels,
        nFrames,
        resample.m_PolyphaseCoefs,
        output.data(),
        laneOut);

      /* Self-computed reference on the uncompressed twins: see
       * computeReferenceFrame above (fmaf per tap, plain gain mul, the
       * kernel's fixed hsum tree - isolated against optimizer
       * re-fusing). Bit-exact - no epsilon anywhere. */
      const GOSoundAudioSection *refSectionPtrs[LANES];
      float laneAcc[LANES][MAX_OUTPUT_CHANNELS];
      float frameSum[MAX_OUTPUT_CHANNELS];

      for (unsigned v = 0; v < LANES; v++)
        refSectionPtrs[v]
          = refSections[v < shape.nVoices ? v : 0].get();

      for (unsigned j = 0; j < nFrames; j++) {
        // refIdx/refFrac hold each lane's CURRENT absolute source
        // position for this frame; both advance after the frame, with
        // the same integer arithmetic as the kernel.
        computeReferenceFrame(
          refSectionPtrs,
          refIdx,
          refFrac,
          refGain,
          shape.nVoices,
          shape.nChannels,
          resample,
          laneAcc,
          frameSum);
        for (unsigned c = 0; c < shape.nChannels; c++) {
          // laneOut check (raw, pre-gain) for every real lane
          for (unsigned v = 0; v < shape.nVoices; v++)
            GOAssert(
              laneOut[c][(size_t)j * LANES + v] == laneAcc[v][c],
              GOFormat(
                "laneOut mismatch voice {} frame {} ch {} (iter {}): {} != {}",
                v,
                j,
                c,
                iterI,
                laneOut[c][(size_t)j * LANES + v],
                laneAcc[v][c]));
          expected[j * 2 + c] += frameSum[c];
        }
        if (shape.nChannels == 1)
          expected[j * 2 + 1] = expected[j * 2];
        // Advance reference positions and gains exactly like the kernel.
        for (unsigned v = 0; v < shape.nVoices; v++) {
          refFrac[v] += streams[v].GetResamplingIncrement();
          refIdx[v] += refFrac[v] >> GOSoundResample::UPSAMPLE_BITS;
          refFrac[v] &= GOSoundResample::UPSAMPLE_MASK;
          refGain[v] += refDg[v];
        }
      }
      for (unsigned i = 0; i < nFrames * 2; i++)
        GOAssert(
          output[i] == expected[i],
          GOFormat(
            "fused output mismatch at sample {} (iteration {}, {}ch "
            "{} voices): {} != {}",
            i,
            iterI,
            shape.nChannels,
            shape.nVoices,
            output[i],
            expected[i]));

      for (unsigned v = 0; v < shape.nVoices; v++)
        streams[v].AdvanceAfterBatchRender(nFrames);
    }
    GOSoundStream::SetUseSubblockCache(false);
  }
#endif
}

void GOTestSoundStream::TestBatchRenderPathSwitchConsistency() {
#ifdef GO_RESAMPLE_BATCH_AVX2
  if (!GOSoundResampleBatch::CpuSupported())
    return;

  constexpr unsigned LANES = GOSoundResampleBatch::LANES;
  constexpr unsigned N_ITERATIONS = 300;
  const unsigned nChannels = 2;
  const unsigned nFrames = N_FRAMES_PER_BLOCK;
  const GOWaveLoop loop = {700, 2399};

  GOSoundStream::SetUseSubblockCache(true);

  /* Give the section an uncompressed prefix so this test also covers the
   * prefix-driven classification: while a block starts inside the prefix,
   * ClassifyForRender must say BuildOnly (raw-copy build + classic render,
   * the decode-free voice start), and the voice must join the fused path
   * seamlessly at the prefix-end seed. */
  constexpr unsigned PATH_SWITCH_PREFIX = 300;
  const auto pV3 = CreateAudioSection(
    nChannels, 2500, SectionFormat::V3, &loop, PATH_SWITCH_PREFIX);
  GOSoundResample resample;
  GOSoundStream fusedStream;
  GOSoundStream classicStream;
  TestVoiceBlockStorage storageA, storageB;

  std::srand(0);
  storageA.Attach(fusedStream);
  fusedStream.InitStream(
    &resample,
    pV3.get(),
    GOSoundResample::GO_POLYPHASE_INTERPOLATION,
    SAMPLE_RATE_ADJUSTMENT);
  std::srand(0);
  storageB.Attach(classicStream);
  classicStream.InitStream(
    &resample,
    pV3.get(),
    GOSoundResample::GO_POLYPHASE_INTERPOLATION,
    SAMPLE_RATE_ADJUSTMENT);

  float classicBuf[N_BUFFER_ITEMS];
  unsigned fusedPeriods = 0;
  unsigned prefixBuildOnlyPeriods = 0;

  for (unsigned iterI = 0; iterI < N_ITERATIONS; iterI++) {
    unsigned start;
    unsigned valid;
    const GOSoundStream::RenderClass cls
      = fusedStream.ClassifyForRender(start, valid);

    if (cls != GOSoundStream::RenderClass::None && start < PATH_SWITCH_PREFIX) {
      GOAssert(
        cls == GOSoundStream::RenderClass::BuildOnly,
        GOFormat(
          "a block starting inside the uncompressed prefix (start={}) must "
          "classify BuildOnly, never BatchRender (iteration {})",
          start,
          iterI));
      prefixBuildOnlyPeriods++;
    }
    if (cls == GOSoundStream::RenderClass::BatchRender) {
      // Fused period: render through the kernel (2 lanes - the real
      // group task never runs the kernel solo; lane 1 is a dummy copy
      // with zero gain, exercising the padding conventions).
      GOSoundStream *groupStreams[2] = {&fusedStream, &fusedStream};
      unsigned starts[2] = {start, start};
      unsigned valids[2] = {valid, valid};
      GOSoundResampleBatch::Lane lanes[2];
      const unsigned maxTotal = valid + GOSoundResampleBatch::TAPS;
      std::vector<int32_t> scratchStore(
        (size_t)MAX_OUTPUT_CHANNELS * maxTotal * LANES);
      int32_t *const scratch[MAX_OUTPUT_CHANNELS]
        = {scratchStore.data(),
           scratchStore.data() + (size_t)maxTotal * LANES};
      std::vector<float> output(nFrames * 2, 0.0f);

      // NOTE deliberately only ONE stream decoded (both lanes share it):
      // DecodeGroupToScratch must not be handed the same stream twice
      // (its end-state snapshot would run twice), so decode solo.
      GOSoundStream *soloStream[1] = {&fusedStream};
      unsigned soloStart[1] = {start};
      unsigned soloValid[1] = {valid};

      GOSoundStream::DecodeGroupToScratch(
        soloStream, soloStart, soloValid, 1, nChannels, scratch);
      lanes[0].rel = 0;
      lanes[0].fraction = fusedStream.GetResamplingFraction();
      lanes[0].increment = fusedStream.GetResamplingIncrement();
      lanes[0].g0 = 1.0f;
      lanes[0].dg = 0.0f;
      lanes[1] = lanes[0];
      lanes[1].g0 = 0.0f;
      GOSoundResampleBatch::RenderAccumulate(
        lanes,
        2,
        scratch,
        nChannels,
        nFrames,
        resample.m_PolyphaseCoefs,
        output.data());
      fusedStream.AdvanceAfterBatchRender(nFrames);
      classicStream.ReadBlock(classicBuf, nFrames);
      // Values: both paths compute the same FIR on the same lossless
      // data, in different float summation orders - compare with a
      // small tolerance.
      for (unsigned i = 0; i < nFrames * 2; i++) {
        const float d = output[i] - classicBuf[i];
        const float mag = std::max(1.0f, std::abs(classicBuf[i]));

        GOAssert(
          std::abs(d) <= 1e-4f * mag,
          GOFormat(
            "fused/classic divergence at sample {} (iteration {}): "
            "{} != {}",
            i,
            iterI,
            output[i],
            classicBuf[i]));
      }
      fusedPeriods++;
    } else {
      // Classic period (loop wrap etc.): both streams take the very
      // same code path - keep them in lockstep.
      if (cls == GOSoundStream::RenderClass::BuildOnly) {
        GOSoundStream *stream = &fusedStream;

        GOSoundStream::BuildBlocksBatch(
          &stream, &start, &valid, 1, nChannels);
      }

      float fusedBuf[N_BUFFER_ITEMS];

      fusedStream.ReadBlock(fusedBuf, nFrames);
      classicStream.ReadBlock(classicBuf, nFrames);
      for (unsigned i = 0; i < nFrames * 2; i++)
        GOAssert(
          fusedBuf[i] == classicBuf[i],
          GOFormat(
            "classic-period mismatch at sample {} (iteration {})",
            i,
            iterI));
    }
  }
  GOAssert(
    fusedPeriods > N_ITERATIONS / 2,
    "the fused path should carry most periods of a looped voice");
  GOAssert(
    prefixBuildOnlyPeriods > 0,
    "the voice start should have spent at least one period on the "
    "prefix-driven BuildOnly path");
  GOSoundStream::SetUseSubblockCache(false);
#endif
}

void GOTestSoundStream::TestCrossCodecBridgeAlternatingSections() {
  /* The cross-codec GetSample() bridges keep a thread_local decode state
   * that persists ACROSS CALLS (per-call reset was the O(n^2) load-freeze
   * bug) and reset only when the section's data pointer changes. The risky
   * case of that design is alternating between two different V3 sections:
   * stale state from section A must never be applied to section B. Verify
   * value correctness under exactly that pattern, forcing the bridge by
   * passing a LEGACY cache pointer to V3 sections. */
  const auto pSectionA = CreateAudioSection(1, N_FRAMES, SectionFormat::V3);
  const auto pSectionB = CreateAudioSection(2, N_FRAMES, SectionFormat::V3);
  const auto pReferenceA
    = CreateAudioSection(1, N_FRAMES, SectionFormat::Uncompressed);
  const auto pReferenceB
    = CreateAudioSection(2, N_FRAMES, SectionFormat::Uncompressed);
  GOSoundCompressionCache legacyCache; // wrong codec on purpose

  legacyCache.Init();
  for (unsigned i = 0; i < N_FRAMES; i += 7) {
    const int a = pSectionA->GetSample(i, 0, &legacyCache);
    const int aRef = pReferenceA->GetSample(i, 0);

    GOAssert(
      a == aRef,
      GOFormat("bridge mismatch on section A at {}: {} != {}", i, a, aRef));

    const int b = pSectionB->GetSample(i, 1, &legacyCache);
    const int bRef = pReferenceB->GetSample(i, 1);

    GOAssert(
      b == bRef,
      GOFormat("bridge mismatch on section B at {}: {} != {}", i, b, bRef));
  }
}

void GOTestSoundStream::TestLoopTransitionAcrossDifferentEndPos() {
  // Two loops with different end_pos: a long loop and a short loop placed
  // so PickEndSegment can pick either for some start segments. With the
  // regression in ReadBlock (using the new segment's end_pos when computing
  // the post-wrap position), wrap-around between segments with different
  // end_pos breaks the source position and eventually triggers the
  // "invalid loop" path, after which ReadBlock returns false.
  constexpr unsigned N_TOTAL_FRAMES = 1100;
  constexpr unsigned N_ITERATIONS = 500;
  // Two long loops with very different end_pos. start_offsets are chosen
  // such that an unsigned underflow in the buggy expression
  //   startOffset + (currSrcOffset - end_pos_new)
  // does NOT happen to wrap back into a valid (< end_pos_new) range — it
  // stays huge, so the broken code reliably enters the "Breaking invalid
  // loop" branch and ReadBlock subsequently returns false.
  const std::vector<GOWaveLoop> loops = {
    {500, 999}, // long loop, start_offset=500, end_pos=1000
    {200, 599}, // long loop, start_offset=200, end_pos=600
  };
  const unsigned nChannels = 1;
  const unsigned nSamples = nChannels * N_TOTAL_FRAMES;
  std::vector<GOInt24> pcmData(nSamples);

  for (unsigned i = 0; i < nSamples; i++)
    pcmData[i] = (i % 100) * 30000 - 1500000;

  auto pSection = std::make_unique<GOSoundAudioSection>(m_pool);

  pSection->Setup(
    nullptr,
    nullptr,
    pcmData.data(),
    GOWave::SF_SIGNEDINT24_24,
    nChannels,
    SECTION_RATE,
    N_TOTAL_FRAMES,
    &loops,
    BOOL3_DEFAULT,
    false,
    false,
    0,
    0);

  GOSoundResample resample;
  GOSoundStream stream;

  // PickEndSegment uses rand(); seed for reproducibility of the test path.
  std::srand(0);
  stream.InitStream(
    &resample,
    pSection.get(),
    GOSoundResample::GO_LINEAR_INTERPOLATION,
    SAMPLE_RATE_ADJUSTMENT);

  float buffer[N_BUFFER_ITEMS];

  for (unsigned iterI = 0; iterI < N_ITERATIONS; iterI++) {
    const bool result = stream.ReadBlock(buffer, N_FRAMES_PER_BLOCK);

    GOAssert(
      result,
      GOFormat(
        "ReadBlock should always return true across loop-segment "
        "transitions with different end_pos (iteration {})",
        iterI));
  }
}

void GOTestSoundStream::TestCompressedLoopWrapMatchesUncompressed() {
  constexpr unsigned N_TOTAL_FRAMES = 1100;
  constexpr unsigned N_ITERATIONS = 500;
  const std::vector<GOWaveLoop> loops = {
    {500, 999}, // long loop, start_offset=500, end_pos=1000
    {200, 599}, // long loop, start_offset=200, end_pos=600
  };
  const unsigned nChannels = 1;
  const unsigned nSamples = nChannels * N_TOTAL_FRAMES;
  std::vector<GOInt24> pcmData(nSamples);

  /* Same small-amplitude sawtooth as CreateAudioSection() and for the same
   * reason: this test compares positions bit-exactly (values only need to be
   * per-position discriminable), and V3's full-slot block format would fall
   * back to uncompressed on a section this short if any residual needed
   * more than 1 byte - silently removing the V3 leg of the comparison. */
  for (unsigned i = 0; i < nSamples; i++)
    pcmData[i] = i % 80;

  auto runCapture = [&](
                      SectionFormat format,
                      GOSoundResample::InterpolationType interpolation,
                      float pitchFactor,
                      unsigned uncompressedPrefixSamples = 0) {
    auto pSection = std::make_unique<GOSoundAudioSection>(m_pool);

    pSection->Setup(
      nullptr,
      nullptr,
      pcmData.data(),
      GOWave::SF_SIGNEDINT24_24,
      nChannels,
      SECTION_RATE,
      N_TOTAL_FRAMES,
      &loops,
      BOOL3_DEFAULT,
      format != SectionFormat::Uncompressed,
      format == SectionFormat::V3,
      0,
      0,
      uncompressedPrefixSamples);
    if (format == SectionFormat::V3)
      GOAssert(
        pSection->GetCompressionCodec()
          == GOSoundAudioSectionCompressionCodec::V3,
        "test section unexpectedly fell back from V3 compression");
    if (uncompressedPrefixSamples)
      GOAssert(
        pSection->GetUncompressedPrefixSamples() == uncompressedPrefixSamples,
        "uncompressed prefix was not applied");

    GOSoundResample resample;
    GOSoundStream stream;
    TestVoiceBlockStorage storage;

    // Same seed for all runs: PickEndSegment() uses rand() to choose among
    // multiple valid end segments, and all runs must pick identically so
    // the wrap timing (and hence the skip-ahead pattern) matches.
    std::srand(0);
    storage.Attach(stream); // no-op unless the subblock path is enabled
    stream.InitStream(
      &resample,
      pSection.get(),
      interpolation,
      pitchFactor * SAMPLE_RATE_ADJUSTMENT);

    std::vector<float> captured(N_ITERATIONS * N_BUFFER_ITEMS);
    float buffer[N_BUFFER_ITEMS];

    for (unsigned iterI = 0; iterI < N_ITERATIONS; iterI++) {
      stream.ReadBlock(buffer, N_FRAMES_PER_BLOCK);
      for (unsigned i = 0; i < N_BUFFER_ITEMS; i++)
        captured[iterI * N_BUFFER_ITEMS + i] = buffer[i];
    }
    return captured;
  };

  const struct {
    SectionFormat format;
    const char *name;
  } codecs[] = {
    {SectionFormat::Legacy, "legacy"},
    {SectionFormat::V3, "v3"},
  };
  /* pitch != 1 makes the source position advance in genuine fractional
   * steps, exercising the compressed windows' skip-ahead Seek() logic and
   * larger loop-wrap overflows - a path factor 1.0 (identity) never hits. */
  const float pitches[] = {1.0f, 1.25f, 0.8f};

  for (const float pitch : pitches) {
    /* LINEAR: compressed vs uncompressed use the same resampler arithmetic
     * on the same (losslessly decoded) sample values, so the comparison is
     * bit-exact even at pitch != 1. */
    const std::vector<float> uncompressed = runCapture(
      SectionFormat::Uncompressed,
      GOSoundResample::GO_LINEAR_INTERPOLATION,
      pitch);

    for (const auto &codec : codecs) {
      const std::vector<float> compressed = runCapture(
        codec.format, GOSoundResample::GO_LINEAR_INTERPOLATION, pitch);

      for (unsigned i = 0; i < uncompressed.size(); i++)
        GOAssert(
          compressed[i] == uncompressed[i],
          GOFormat(
            "{}/uncompressed mismatch (linear, pitch={}) at output sample {} "
            "(iteration {}, offset {}): {}={} uncompressed={}",
            codec.name,
            pitch,
            i,
            i / N_BUFFER_ITEMS,
            i % N_BUFFER_ITEMS,
            codec.name,
            compressed[i],
            uncompressed[i]));
    }

    /* POLYPHASE: compressed sections use the planar read-ahead window (SIMD
     * FIR kernel) while uncompressed sections use the plain pointer window -
     * different float summation order, so comparing against uncompressed is
     * NOT guaranteed bit-exact. Compare V3 against legacy instead: both go
     * through the identical window class and kernel, only the codec behind
     * it differs, and both are lossless - so they must match bit-exactly. */
    const std::vector<float> polyLegacy = runCapture(
      SectionFormat::Legacy,
      GOSoundResample::GO_POLYPHASE_INTERPOLATION,
      pitch);
    const std::vector<float> polyV3 = runCapture(
      SectionFormat::V3, GOSoundResample::GO_POLYPHASE_INTERPOLATION, pitch);

    for (unsigned i = 0; i < polyLegacy.size(); i++)
      GOAssert(
        polyV3[i] == polyLegacy[i],
        GOFormat(
          "v3/legacy mismatch (polyphase, pitch={}) at output sample {} "
          "(iteration {}, offset {}): v3={} legacy={}",
          pitch,
          i,
          i / N_BUFFER_ITEMS,
          i % N_BUFFER_ITEMS,
          polyV3[i],
          polyLegacy[i]));

    /* SUBBLOCK CACHE path: same V3 sections read through each voice's
     * private decoded render block instead of per-voice
     * DecompressionStep. LINEAR must match uncompressed bit-exactly (same
     * lossless values, same resampler arithmetic). POLYPHASE must match
     * the direct-V3 capture bit-exactly: both feed planar stride-1 int32
     * taps into the identical SIMD FIR kernel. */
    GOSoundStream::SetUseSubblockCache(true);

    const std::vector<float> subLin = runCapture(
      SectionFormat::V3, GOSoundResample::GO_LINEAR_INTERPOLATION, pitch);
    const std::vector<float> subPoly = runCapture(
      SectionFormat::V3, GOSoundResample::GO_POLYPHASE_INTERPOLATION, pitch);

    /* Same again with an uncompressed prefix (200 samples: the voice
     * starts inside it, crosses the prefix-end seam at 200, and the loop
     * wraps land beyond it) - the prefix fast path must be bit-identical
     * to full decoding on both resamplers. */
    constexpr unsigned TEST_PREFIX = 200;
    const std::vector<float> preLin = runCapture(
      SectionFormat::V3,
      GOSoundResample::GO_LINEAR_INTERPOLATION,
      pitch,
      TEST_PREFIX);
    const std::vector<float> prePoly = runCapture(
      SectionFormat::V3,
      GOSoundResample::GO_POLYPHASE_INTERPOLATION,
      pitch,
      TEST_PREFIX);

    GOSoundStream::SetUseSubblockCache(false);

    for (unsigned i = 0; i < subLin.size(); i++)
      GOAssert(
        preLin[i] == subLin[i],
        GOFormat(
          "v3-prefix/v3 mismatch (linear, pitch={}) at output sample {}: "
          "prefix={} plain={}",
          pitch,
          i,
          preLin[i],
          subLin[i]));
    for (unsigned i = 0; i < subPoly.size(); i++)
      GOAssert(
        prePoly[i] == subPoly[i],
        GOFormat(
          "v3-prefix/v3 mismatch (polyphase, pitch={}) at output sample {}: "
          "prefix={} plain={}",
          pitch,
          i,
          prePoly[i],
          subPoly[i]));

    for (unsigned i = 0; i < uncompressed.size(); i++)
      GOAssert(
        subLin[i] == uncompressed[i],
        GOFormat(
          "v3-subblock/uncompressed mismatch (linear, pitch={}) at output "
          "sample {} (iteration {}, offset {}): subblock={} uncompressed={}",
          pitch,
          i,
          i / N_BUFFER_ITEMS,
          i % N_BUFFER_ITEMS,
          subLin[i],
          uncompressed[i]));
    for (unsigned i = 0; i < polyV3.size(); i++)
      GOAssert(
        subPoly[i] == polyV3[i],
        GOFormat(
          "v3-subblock/v3-direct mismatch (polyphase, pitch={}) at output "
          "sample {} (iteration {}, offset {}): subblock={} direct={}",
          pitch,
          i,
          i / N_BUFFER_ITEMS,
          i % N_BUFFER_ITEMS,
          subPoly[i],
          polyV3[i]));
  }
}

void GOTestSoundStream::run() {
  /* Bin ladder setup (in production: GOSoundOrganEngine setup, from the
   * device's real samples_per_buffer). Base = this file's per-ReadBlock
   * frame count, so a factor-1.0 voice needs exactly one rung-1 block per
   * "callback"; top rung = 4x base. */
  GOSoundSubblockBins::SetBase(N_FRAMES_PER_BLOCK);

  /* The whole suite runs once per predictor order (cache v11: order 4 is
   * back): sections are compressed fresh inside each test, so every order
   * exercises encode, scalar decode, the uniform-order batch kernel, the
   * loop wrap, and the SaveCache/LoadCache roundtrip (incl. the order-4
   * hist3 side arrays). Fixed3 (the default) runs first. */
  /* Variable runs last: on the uniform sawtooth its per-section search
   * resolves every section to the same (lowest-tied) order, so it mainly
   * exercises the Variable dispatch plumbing (section-order propagation,
   * bucket fold, Init/DecompressTo reset paths with mode==0). */
  for (GOV3PredictorMode mode :
       {GOV3PredictorMode::Fixed3,
        GOV3PredictorMode::Fixed1,
        GOV3PredictorMode::Fixed2,
        GOV3PredictorMode::Fixed4,
        GOV3PredictorMode::Variable}) {
    GOSoundAudioSection::SetPredictorMode(mode);

    for (unsigned nChannels : {1u, 2u}) {
      for (SectionFormat format :
           {SectionFormat::Uncompressed,
            SectionFormat::Legacy,
            SectionFormat::V3}) {
        TestReadBlock(
          GOSoundResample::GO_LINEAR_INTERPOLATION, format, nChannels);
        TestReadBlock(
          GOSoundResample::GO_POLYPHASE_INTERPOLATION, format, nChannels);
      }
    }
    TestLoopedStreamAlwaysReturnsTrue();
    TestLoopTransitionAcrossDifferentEndPos();
    TestInitAlignedStream();
    TestCrossCodecBridgeAlternatingSections();
    TestV3CacheSaveLoadRoundtrip();
    TestVoiceBlockBatchMatchesLazy();
    TestVoiceBlockPredictedRebuildMatchesUncompressed();
    TestCompressedLoopWrapMatchesUncompressed();
    TestBatchRenderBitExact();
    TestBatchRenderPathSwitchConsistency();
  }
  GOSoundAudioSection::SetPredictorMode(GOV3PredictorMode::Fixed3);
}
