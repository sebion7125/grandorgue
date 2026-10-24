/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOTESTSOUNDSTREAM_H
#define GOTESTSOUNDSTREAM_H

#include <memory>
#include <string>

#include "sound/playing/GOSoundResample.h"

#include "GOMemoryPool.h"
#include "GOTest.h"

class GOSoundAudioSection;
struct GOWaveLoop;

class GOTestSoundStream : public GOTest {
private:
  static const std::string TEST_NAME;

  // Which storage format CreateAudioSection() should produce. Kept as a
  // test-local enum (not GOSoundAudioSectionCompressionCodec) because the
  // section decides its actual codec itself (e.g. compression may fall back
  // to None if it doesn't pay off) - this only expresses the request.
  enum class SectionFormat { Uncompressed, Legacy, V3 };

  GOMemoryPool m_pool;

  /**
   * Creates an audio section with nChannels channels and nFrames frames
   * of synthetic GOInt24 PCM data at 48000 Hz sample rate.
   * The requested storage format is given by `format` (see SectionFormat).
   * If pLoop is not null, the section is created with the given loop points.
   */
  std::unique_ptr<GOSoundAudioSection> CreateAudioSection(
    unsigned nChannels,
    unsigned nFrames,
    SectionFormat format,
    const GOWaveLoop *pLoop = nullptr,
    unsigned uncompressedPrefixSamples = 0);

  /**
   * Tests ReadBlock for the given interpolation, storage format and channel
   * combination. Verifies true while frames available, false on exhaustion,
   * and zeros after end.
   */
  void TestReadBlock(
    GOSoundResample::InterpolationType interpolationType,
    SectionFormat format,
    unsigned nChannels);

  /**
   * Tests that ReadBlock always returns true for a looped stream,
   * running 100 iterations of 50 frames each.
   */
  void TestLoopedStreamAlwaysReturnsTrue();

  /**
   * Tests that wrap-around between loop segments with different end_pos
   * keeps the source position consistent. Catches a regression where
   * ReadBlock used the new segment's end_pos instead of the previous one
   * when computing the post-wrap position.
   */
  void TestLoopTransitionAcrossDifferentEndPos();

  /**
   * Tests InitAlignedStream with real release alignment, for both an
   * uncompressed and a V3 release section - the latter covers the
   * load-time ComputeTable scan that froze (O(n^2)) when GetSample()'s
   * cross-codec bridge reset its decode state on every call.
   * Verifies that ReadBlock returns true after alignment.
   */
  void TestInitAlignedStream();

  /**
   * Verifies GetSample()'s cross-codec bridge (legacy cache passed to a V3
   * section) returns correct values when alternating between two different
   * V3 sections - the risky case for the bridge's persistent thread_local
   * decode state, which must reset exactly when the underlying section
   * data changes and never carry stale state across sections.
   */
  void TestCrossCodecBridgeAlternatingSections();

  /**
   * Full SaveCache()/LoadCache() roundtrip of a V3 section through a real
   * cache file (GOCacheWriter/GOCache incl. the mmap path); the loaded
   * section must play back bit-identically to the original.
   */
  void TestV3CacheSaveLoadRoundtrip();

  /**
   * Per-voice decoded render blocks (2026-07-08 redesign): a batch call
   * to GOSoundStream::BuildBlocksBatch (the group task's decode pass,
   * grouping voices needing a rebuild into up to
   * GOSoundCompressionCacheV3Batch::LANES lanes for the AVX2 kernel) must
   * produce output bit-identical to a voice that never gets explicitly
   * pre-built and relies solely on the lazy, local, synchronous rebuild
   * inside the render window. Includes a short-final-subblock voice mixed
   * into the same batch call as normal-length ones (the scalar-fallback
   * branch within BuildBlocksBatch).
   */
  void TestVoiceBlockBatchMatchesLazy();

  /**
   * NeedsBlockRebuild-driven pre-build (what GOSoundGroupTask's decode
   * pass does every period) compared bit-exactly against an uncompressed
   * reference, at several pitches chosen against the bin ladder (first
   * rung, higher rungs, near the top rung, and beyond it - the latter
   * must opt out of the subblock path at note start and still play
   * bit-exactly via the direct V3 path). Includes loop wraps and asserts
   * the segment-anchored grid rebuilds a block starting exactly at the
   * loop start after a wrap.
   */
  void TestVoiceBlockPredictedRebuildMatchesUncompressed();

  /**
   * Phase 2 fused batch render (decode -> resample -> fade -> accumulate
   * on one warm scratch): DecodeGroupToScratch + GOSoundResampleBatch::
   * RenderAccumulate, driven period by period exactly like
   * GOSoundGroupTask::ProcessVoices, must be BIT-EXACT against a
   * self-computed scalar reference (sequential mul-then-add FIR per tap,
   * additive gain advance per frame, and the kernel's documented fixed
   * hsum tree) evaluated on an uncompressed twin section. Covers: full
   * stereo groups, a partial (3-lane) group, a mono group, constant and
   * ramp gains, a zero-gain lane with laneOut extraction (the classic-
   * fader detour), and the seamless start/valid chain across periods.
   * Skipped (with a log line) on machines without AVX2.
   */
  void TestBatchRenderBitExact();

  /**
   * Path-switching robustness: a looped V3 voice rendered period by
   * period the way ProcessVoices does (fused when ClassifyForRender says
   * BatchRender, classic ReadBlock otherwise - i.e. around loop wraps)
   * must stay position-consistent and value-equivalent (small epsilon;
   * the fused and classic FIR paths use different but equally valid
   * float summation orders) with a twin voice rendered classically
   * throughout.
   */
  void TestBatchRenderPathSwitchConsistency();

  /**
   * Regression test for an off-by-one in
   * StreamCacheReadAheadWindow::Seek() (GOSoundStream.cpp): after a loop
   * wraps to a new start segment, the compressed decoder resumes from a
   * precomputed cache snapshot pinned at that segment's start_offset, while
   * the resampling position typically requests a few samples further in
   * (the wrap remainder) - a genuine skip-ahead Seek() on an otherwise-fresh
   * cache. Using ">= index" instead of "> index" to decide whether a freshly
   * decoded sample should be stored shifted every FIR tap by one sample
   * until it self-healed after windowLen samples.
   *
   * Verified differentially: with LINEAR interpolation and factor 1.0
   * (identity), decoding is lossless, so a compressed and an uncompressed
   * section with identical PCM data and identical loop layout must produce
   * bit-identical ReadBlock() output for every frame, including immediately
   * after a wrap - no assumption about "realistic" sample values is needed.
   */
  void TestCompressedLoopWrapMatchesUncompressed();

public:
  std::string GetName() override { return TEST_NAME; }
  void run() override;
};

#endif /* GOTESTSOUNDSTREAM_H */
