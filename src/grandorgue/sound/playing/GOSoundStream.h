/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDSTREAM_H
#define GOSOUNDSTREAM_H

#include <atomic>

#include "GOHotPath.h"
#include "GOSoundAudioSectionCacheFormat.h"
#include "GOSoundCompressionCache.h"
#include "GOSoundCompressionCacheV3.h"
#include "GOSoundResample.h"

class GOSoundAudioSection;

class GOSoundStream {
private:
  // Shared tail of DecodeGroupToScratch/DecodeGroupToPlanes (chain-point
  // advance, block-end-state publish, block invalidation).
  GO_HOT static void FinishGroupDecode(
    GOSoundStream *const streams[],
    const unsigned validSamples[],
    unsigned count,
    unsigned channels,
    GOSoundCompressionCacheV3 mid[]);

  /* Sample vectors over a contiguous per-lane plane (variant-D render,
   * see RenderBlockFromPlanes below): same access contract as the
   * classic planar ReadAhead window - Seek() gets the ABSOLUTE source
   * index and maps it into the plane via the captured base. */
  struct PlaneVecStereo : GOSoundResample::FloatingSampleVector<2> {
    static constexpr bool kPlanarInt32Taps = true;
    const int32_t *plane[2];
    unsigned base;
    const int32_t *p = nullptr;

    GO_HOT GO_ALWAYS_INLINE void Seek(unsigned index, uint8_t ch) {
      p = plane[ch] + (index - base);
    }
    GO_HOT GO_ALWAYS_INLINE const int32_t *GetPtr() const { return p; }
    inline float NextSample() { return (float)*p++; }
  };
  struct PlaneVecMono : GOSoundResample::FloatingSampleVector<1> {
    static constexpr bool kPlanarInt32Taps = true;
    const int32_t *plane[1];
    unsigned base;
    const int32_t *p = nullptr;

    GO_HOT GO_ALWAYS_INLINE void Seek(unsigned index, uint8_t ch) {
      p = plane[0] + (index - base);
    }
    GO_HOT GO_ALWAYS_INLINE const int32_t *GetPtr() const { return p; }
    inline float NextSample() { return (float)*p++; }
  };

private:
  /* Maximum number of continous samples required for resampling of one target
   * sample. The polyphase algorithm requires the longest sample vector */
  static constexpr unsigned MAX_WINDOW_LEN = GOSoundResample::POLYPHASE_POINTS;
  /* Maximum number of audio chanels in the source samples */
  static constexpr unsigned MAX_INPUT_CHANNELS = 2;

  const GOSoundAudioSection *audio_section;
  const GOSoundResample *resample;

  template <class SampleT, uint8_t nChannels> class StreamPtrWindow;

  template <bool format16, uint8_t nChannels, class CacheT = GOSoundCompressionCache>
  class StreamCacheWindow;

  template <
    bool format16,
    unsigned windowLen,
    uint8_t nChannels,
    class CacheT = GOSoundCompressionCache>
  class StreamCacheReadAheadWindow;

  template <uint8_t nChannels> class StreamSubblockWindow;

  typedef void (GOSoundStream::*DecodeBlockFunction)(
    float *pOut, unsigned nOutSamples);

  template <class CacheT> CacheT &GetCache();
  template <class CacheT> const CacheT &GetCache() const;
  template <class CacheT>
  static DecodeBlockFunction getCompressedDecodeBlockFunction(
    uint8_t nChannels,
    uint8_t nBitsPerSoundItem,
    GOSoundResample::InterpolationType interpolationType);

  /* Opt-in (default off): read V3 MainSegments through this voice's own
   * private decoded render block (batched 8-wide across a worker's claimed
   * voices at build time) instead of per-voice DecompressionStep decoding.
   * Read once per stream at InitFromSection time, so a running voice never
   * switches paths mid-note; set from the engine before any stream is
   * initialized. */
  static std::atomic<bool> s_UseSubblockCache;

  /* Runtime toggle (default OFF since 2026-07-26) for the next-period
   * decode-input read-ahead in FinishGroupDecode: a prefetcht2 of ~one
   * callback's worth of the compressed residual stream (channel 0) into L3,
   * a full period ahead. Warms the one genuinely DRAM-cold stream, but the
   * speculative traffic competes with demand misses for the line-fill
   * buffers at the latency-bound ceiling (24k voices) - same finding as the
   * prefetch-distance knob, hence the same default. The Audio Monitor
   * checkbox re-enables it per session for A/B runs. */
  static std::atomic<bool> s_DecodeReadAhead;

  /* Method used to decode stream */
  DecodeBlockFunction decode_call;

  /* Cached method used to decode the data in end_ptr. This is just the
   * uncompressed version of decode_call */
  DecodeBlockFunction end_decode_call;

  /* Current pointer. Used in the DecodeBlock functions. May be a real pointer
   * to the audio section data or an end segment vir tual pointer */
  const unsigned char *ptr;

  // A virtual pointer to the end segment. An real pointer has the
  // transition_position offset from this
  const unsigned char *end_ptr;

  // when to switch to the end segment
  unsigned transition_position;
  // loop end pos, when to switch to the next start segment
  unsigned end_pos;
  // -1 means it is not looped sample, otherwise the index of the start segment
  int m_NextStartSegmentIndex;

  /* True when decode_call reads MainSegment data through this voice's own
   * private decoded render block (V3 + subblock mode enabled at voice
   * start). Only such streams participate in the group task's per-batch
   * decode pass (see NeedsBlockRebuild). */
  bool m_UsesSubblockCache = false;

  GOSoundResample::ResamplingPosition m_ResamplingPos;

public:
  /* GOSoundResample::POLYPHASE_POINTS: the resampler reads up to 8 source
   * samples ahead of its integer position. */
  static constexpr unsigned GUARD_SAMPLES = GOSoundResample::POLYPHASE_POINTS;

private:
  /* Render subblock metadata: decoded source window kept per voice (NOT
   * shared - see the 2026-07-08 architecture decision recorded in
   * tools/compression/INVESTIGATION.md: a real pipe never sounds twice at
   * once, so there is no legitimate case where two simultaneously active
   * voices need to coordinate over the same decoded window; every voice
   * owns exactly the state it itself needs).
   *
   * CALLBACK-ALIGNED (2026-07-09, replacing the short-lived fixed-grid
   * designs - both the global modulo grid and the segment-anchored bin
   * raster drifted against the real callback starts, so nearly every
   * callback crossed a block boundary mid-render and fell back to an
   * unbatched scalar rebuild): each block begins exactly at a callback's
   * first source position and its validSamples is that callback's EXACT
   * source consumption (computable in advance - ResamplingPosition::
   * IndexAfter reproduces the resampler's integer arithmetic precisely).
   * Consequently a block always covers its whole callback, the next
   * block starts exactly where this one ends (m_BlockEndState is
   * captured precisely there - a seamless dependent-forward chain), and
   * the ONLY remaining mid-callback rebuild is a loop wrap landing
   * mid-callback. m_BinSamples (the ladder rung >= the worst-case
   * consumption, see GOSoundSubblockBins) is the storage CAPACITY and
   * the batch-grouping key, not the block length.
   *
   * The sample storage itself (m_BlockData) is owned by
   * GOSoundSamplerPool (one buffer per pooled sampler, restored around
   * the pool's memset - see GOSoundSamplerPool::GetSampler), planar:
   * channel c occupies [m_BlockData + c * m_BlockChannelStride, ... +
   * validSamples + GUARD_SAMPLES). Guard samples past the section end
   * are zero. subblockStart identifies which source window this
   * currently holds; UINT_MAX means "nothing built yet". */
  struct VoiceBlock {
    unsigned subblockStart = ~0u;
    unsigned validSamples = 0;
    unsigned guardSamples = 0;
  };
  VoiceBlock m_Block;
  /* Pool-owned planar sample storage for m_Block (see above); null until
   * the pool assigns it. NOT owned by this stream - the pool restores it
   * after its per-note memset. */
  int *m_BlockData = nullptr;
  /* Ints per channel within m_BlockData (= max bin + GUARD_SAMPLES,
   * fixed at engine setup). */
  unsigned m_BlockChannelStride = 0;
  /* This voice's storage capacity per rebuild, in source samples - the
   * ladder rung picked at note start (see GOSoundSubblockBins), always
   * >= the worst-case per-callback consumption. 0 while the voice is
   * not on the subblock path. */
  unsigned m_BinSamples = 0;
  /* Bucket-sort key for GOSoundGroupTask::DrainIntoEpoch, precomputed by
   * ApplySubblockBin (0 = not on the subblock path, else
   * 1 + binIndex*2 + (channels-1)). Cached here because the drain is the
   * period's single-threaded phase: computing the key there live meant
   * dereferencing stream -> audio_section for every sampler every period
   * (one cache miss each), which dominated the drain's runtime. */
  unsigned m_SecLength = 0;
  unsigned m_SecPrefixSamples = 0;
  unsigned m_SecChannels = 1;
  unsigned m_BucketKey = 0;
  /* The rung's ladder index - the group task's batch-grouping key. */
  unsigned m_BinIndex = 0;
  /* True when this voice renders through the polyphase resampler -
   * required for the fused batch render path (the batch kernel only
   * implements the polyphase FIR). Set alongside the bin at note start. */
  bool m_IsPolyphase = false;
  /* The section's V3 predictor order (cached like the other m_Sec*
   * constants; lives in the padding hole after m_IsPolyphase - sizeof
   * stays 448). In Variable mode the scheduler groups voices by it
   * (bucket-key fold + batch flush) so every 8-lane batch decodes with
   * ONE hardcoded predictor. */
  uint8_t m_SecPredictorOrder = 3;

  inline int *BlockChannel(unsigned channelN) const {
    return m_BlockData + channelN * m_BlockChannelStride;
  }

  /* Picks this voice's bin from the ladder using the real resampling
   * factor (call AFTER m_ResamplingPos is initialized). If the need
   * exceeds the top rung, or no block storage is assigned, the voice
   * opts OUT of the subblock path: m_UsesSubblockCache is cleared and
   * decode_call is re-pointed at the direct V3 path (which stays
   * correct for any factor). */
  GO_HOT void ApplySubblockBin(
    GOSoundResample::InterpolationType interpolationType);
  /* Decoder state right after m_Block's last VALID sample (i.e. before its
   * guard was decoded) - the exact start state of the next subblock
   * (dependent-forward rule), captured every rebuild so the overwhelmingly
   * common case (sequential playback, no jump) never needs to search for
   * or replay from a seed snapshot. */
  GOSoundCompressionCacheV3 m_BlockEndState;
  bool m_HasBlockEndState = false;

  /* Positions `state` to build the subblock starting at `subblockStart`,
   * following the same no-grid state-source rule used throughout this
   * design: dependent-forward from m_BlockEndState when it matches
   * exactly (the seamless chain case: the previous block's validSamples
   * ended exactly here), otherwise the nearest StartSegmentV3 seed
   * snapshot (GOSoundAudioSection::FindV3SnapshotAtOrBefore) with
   * forward decode for the remainder - taken once per voice start,
   * release alignment, and (bounded to under one callback's overflow)
   * after a loop wrap. */
  GO_HOT void PrepareBlockState(
    unsigned subblockStart, GOSoundCompressionCacheV3 &state) const;

  /* Full single-voice scalar build of [subblockStart, subblockStart +
   * validSamples) + guard: prepares state (see above) and decodes with
   * the per-sample section-end bounds check (zero-fill-past-length rule),
   * then updates m_Block and m_BlockEndState (captured after exactly
   * validSamples). Used for the cold/first-use case and the mid-callback
   * loop-wrap crossing (rare - not worth batching); the render window's
   * fallback passes the full capacity as validSamples since it cannot
   * know the callback-exact span. */
public:
  GO_HOT void BuildBlockScalar(unsigned subblockStart, unsigned validSamples);

  /* Read access to the voice's decoded planar block; the raw-fused
   * render path (uncompressed sections, see RenderClass::RawFused)
   * feeds these planes straight into RenderBlockFromPlanes*. Valid for
   * source indices [m_Block.subblockStart, + validSamples + guard). */
  const int *GetBlockChannelData(uint8_t channelN) const {
    return m_BlockData + channelN * m_BlockChannelStride;
  }

  /* Diagnostics: synchronous mid-render rebuilds (the render window's
   * fallback path in StreamSubblockWindow::Seek) - the decode pass's
   * blind spot until 2026-07-09: these never showed in the rebuild
   * counters while doing full scalar block builds on the render path.
   * After the callback-aligned redesign this should stay near zero
   * except for mid-callback loop wraps. Read by the group task's
   * once-per-period log line and by the regression test that pins the
   * fallback rate to the loop-wrap rate. */
  static inline std::atomic<uint64_t> s_SyncFallbacks{0};

private:

public:

  /* for decoding compressed format */
  GOSoundCompressionCache cache;
  GOSoundCompressionCacheV3 cacheV3;

  /* A ring buffer for resampling of compressed samples, in planar layout:
   * each channel owns a contiguous region of 2 * MAX_WINDOW_LEN samples.
   * The double window length guarantees a continous memory region of
   * MAX_WINDOW_LEN samples per channel for any read position (see
   * StreamCacheReadAheadWindow) */
  int m_ReadAheadBuffer[MAX_INPUT_CHANNELS * MAX_WINDOW_LEN * 2];

  /* The block decode functions should provide whatever the normal resolution of
   * the audio is. The fade engine should ensure that this data is always
   * brought into the correct range. */
  template <class ResamplerT, class WindowT>
  GO_HOT void DecodeBlock(float *pOut, unsigned nOutSamples);

  /* These functions must be static class members rather than file-scope
   * functions because they reference the private typedef DecodeBlockFunction
   * and the private nested window classes (StreamPtrWindow, StreamCacheWindow,
   * StreamCacheReadAheadWindow). */
  /* allowSubblock: the render-subblock window is only valid for the
   * MainSegment decode of a stream that owns block storage - the end
   * segments (raw end_data blob via ptr) and the ApplySubblockBin
   * opt-out repoint must keep the direct windows. */
  static DecodeBlockFunction getDecodeBlockFunction(
    uint8_t nChannels,
    uint8_t nBitsPerSoundItem,
    bool isCompressed,
    GOSoundAudioSectionCompressionCodec compressionCodec,
    GOSoundResample::InterpolationType interpolationType,
    bool allowSubblock);

  /**
   * Returns a decode block function for the given audio section,
   * compression flag and interpolation type.
   */
  static DecodeBlockFunction getDecodeBlockFunctionFor(
    const GOSoundAudioSection *pSection,
    bool isCompressed,
    GOSoundAudioSectionCompressionCodec compressionCodec,
    GOSoundResample::InterpolationType interpolationType,
    bool allowSubblock);

  void GetHistory(int history[BLOCK_HISTORY][MAX_OUTPUT_CHANNELS]) const;

  /**
   * Switches the stream to the start segment with the given index.
   * Sets up ptr, cache, end segment fields and next segment index.
   * Returns start_offset of the start segment.
   */
  unsigned GoToStartSegment(unsigned startSegmentIndex);

  /** Switches the stream to the end segment. */
  void GoToEndSegment() { ptr = end_ptr; }

  /**
   * Initializes audio_section, sets up decode functions and switches
   * to the start segment with the given index.
   * Returns start_offset of the start segment.
   */
  unsigned InitFromSection(
    const GOSoundAudioSection *pSection,
    unsigned startSegmentIndex,
    GOSoundResample::InterpolationType interpolationType);

public:
  // Current read position in the audio data - used only for prefetch hints
  // (GOSoundGroupTask hints this address for a *different* voice a few
  // iterations before its processing turn, to warm the cache ahead of time).
  //
  // ptr itself is *not* this position: it's only the current segment's base
  // address, set once in GoToStartSegment()/GoToEndSegment() and never
  // moved afterwards - for a long-held note whose actual read position (a
  // separate, continuously advancing counter, m_ResamplingPos) has drifted
  // far from that base, prefetching ptr would keep re-warming bytes that
  // were relevant when the segment started and aren't being read anymore,
  // instead of the bytes about to be consumed. So:
  //  - compressed main-segment data: cache.m_ptr already tracks the live
  //    position in the compressed byte stream (DecompressionStep() advances
  //    it every step, and even prefetches 128 bytes ahead of it itself) -
  //    far more accurate than ptr.
  //  - anything else (uncompressed, or the end segment - always uncompressed
  //    regardless of the section's own compression, see end_decode_call):
  //    reconstruct the real address as ptr + the resampling index's byte
  //    offset.
  //
  // Defined in the .cpp: needs the complete GOSoundAudioSection type, which
  // this header only forward-declares.
  const unsigned char *GetCurrentPtr() const;

  // Whether the section behind this stream stores compressed data (the end
  // segment is always uncompressed regardless). Diagnostics for the Audio
  // Monitor's voice-mix histogram. Defined in the .cpp for the same reason
  // as GetCurrentPtr().
  bool IsCompressedSection() const;

  // True when the resampling ratio is exactly 1:1 (device SR == sample SR and
  // no pitch shift), meaning ResampleBlock() takes the fast identity path and
  // skips the FIR computation entirely.
  bool IsIdentityResample() const {
    return m_ResamplingPos.GetFractionIncrement()
      == GOSoundResample::UPSAMPLE_FACTOR;
  }

  static void SetUseSubblockCache(bool use) {
    s_UseSubblockCache.store(use, std::memory_order_relaxed);
  }
  static bool IsSubblockCacheEnabled() {
    return s_UseSubblockCache.load(std::memory_order_relaxed);
  }

  static void SetDecodeReadAhead(bool enable) {
    s_DecodeReadAhead.store(enable, std::memory_order_relaxed);
  }
  static bool IsDecodeReadAheadEnabled() {
    return s_DecodeReadAhead.load(std::memory_order_relaxed);
  }

  bool UsesSubblockCache() const { return m_UsesSubblockCache; }

  /* Section-constant values cached at InitFromSection (2026-07-12, same
   * pattern as m_BucketKey): NeedsBlockRebuild/ClassifyForRender and the
   * group task's per-voice grouping used to pointer-chase audio_section
   * for every voice every period - three dependent loads on the claim/
   * classify latency chain that never change while the section plays. */
  unsigned GetCachedSectionLength() const { return m_SecLength; }
  unsigned GetCachedPrefixSamples() const { return m_SecPrefixSamples; }
  uint8_t GetPredictorOrder() const { return m_SecPredictorOrder; }

  // Channel count of the underlying section - used by the group task to
  // group same-channel-count voices into batches for BuildBlocksBatch.
  // Defined in the .cpp (needs the complete GOSoundAudioSection type).
  unsigned GetSectionChannels() const { return m_SecChannels; }

  // This voice's private block length (ladder rung) and its ladder index -
  // the group task's batch-grouping key alongside the channel count.
  // 0 / meaningless while the voice is not on the subblock path.
  unsigned GetBinSamples() const { return m_BinSamples; }
  unsigned GetBinIndex() const { return m_BinIndex; }
  unsigned GetBucketKey() const { return m_BucketKey; }

  // Pool-side accessors for the block sample storage: the pool owns the
  // buffer, assigns it at sampler construction, and must restore it (the
  // pointer lives inside the pool's per-note memset range) - see
  // GOSoundSamplerPool::GetSampler.
  int *GetBlockData() const { return m_BlockData; }
  void SetBlockData(int *pData, unsigned channelStride) {
    m_BlockData = pData;
    m_BlockChannelStride = channelStride;
  }

  /**
   * Decode-pass check (per-voice, called by the worker that currently
   * claimed this voice, once per period BEFORE that worker renders its
   * claimed batch - see GOSoundGroupTask::ProcessList): does m_Block
   * cover this callback's ENTIRE MainSegment read span [first position,
   * first position + exact consumption)? If not, returns true with the
   * callback-aligned block to build: outSubblockStart = the callback's
   * first source position, outValidSamples = its exact source
   * consumption (ResamplingPosition::IndexAfter over the engine's
   * samples_per_buffer, clamped to the section length). In the seamless
   * steady state the new block starts exactly at the previous block's
   * valid end, so the dependent-forward chain never breaks and a voice
   * rebuilds EXACTLY once per period - always through this (batchable)
   * path, never mid-render. No-op (returns false) for streams not on
   * the subblock path and for callbacks starting inside the
   * (uncompressed) EndSegment.
   *
   * Reproduces ReadBlock()'s segment-walk arithmetic for the FIRST
   * position of this callback (including a loop wrap landing right at
   * the callback boundary); a wrap landing MID-callback is the one
   * remaining case handled by the render window's local synchronous
   * fallback (BuildBlockScalar; counted in s_SyncFallbacks) - safe and
   * cheap because the block is private to this voice.
   */
  GO_HOT bool NeedsBlockRebuild(
    unsigned &outSubblockStart, unsigned &outValidSamples) const;

  /* Per-period classification for the fused batch render path (Phase 2,
   * see tools/compression/V3_SUBBLOCK_IMPLEMENTATION_PLAN.md Step 12):
   *
   * - BatchRender: the whole callback is a plain forward MainSegment
   *   read (no wrap, no EndSegment transition, guard inside the
   *   section, polyphase resampler, span within the batch scratch
   *   bound) - decode, resample, fade and mix can all run fused on the
   *   group's warm scratch. outStart/outValid as NeedsBlockRebuild.
   * - BuildOnly: a block build is needed but the callback is not plain
   *   forward (loop wrap at the callback start, section end inside the
   *   span, linear interpolation, oversized bin) - build the persistent
   *   VoiceBlock (BuildBlocksBatch) and render classically.
   * - None: nothing to prebuild (EndSegment, exhausted, covered, not on
   *   the subblock path) - render classically.
   */
  /* RawFused: like BatchRender, but the section is UNCOMPRESSED - there
   * is no decoder state and nothing for a group decode to share, so the
   * group task builds the voice's block inline (BuildBlockScalar's raw
   * widen) and renders it through the same fused per-lane kernels as
   * the V3 lanes (2026-07-13: previously these 40+% of Rotterdam's
   * voice calls ran a scalar FIR over PtrSampleVector plus the two-pass
   * fader). */
  enum class RenderClass : uint8_t { None, BuildOnly, BatchRender, RawFused };

  /* Largest valid + GUARD_SAMPLES span the fused path handles: bounds
   * the group task's stack scratch (8 lanes x 2 channels x 4 bytes x
   * this = ~66 KB). Covers the whole ladder for base <= 258; larger
   * bins fall back to BuildOnly. */
  static constexpr unsigned BATCH_RENDER_MAX_TOTAL = 1032;

  GO_HOT RenderClass ClassifyForRender(
    unsigned &outSubblockStart, unsigned &outValidSamples) const;

  /**
   * Batched decode-pass build for up to
   * GOSoundCompressionCacheV3Batch::LANES voices that all
   * NeedsBlockRebuild() this period and share the same channel count AND
   * the same bin rung (grouping by both is the caller's responsibility).
   * The batch kernel steps all lanes in lockstep, so the lanes decode
   * min(validSamples[i]) together; each lane's few remaining samples
   * (within one rung the exact consumptions differ only by the
   * per-voice fraction phase), its end-state snapshot at exactly
   * validSamples[i], and its guard run scalar per lane afterwards.
   * A voice whose span would cross the section end is finished via
   * BuildBlockScalar instead (the kernel has no per-sample bounds
   * check).
   */
  GO_HOT static void BuildBlocksBatch(
    GOSoundStream *const streams[],
    const unsigned subblockStarts[],
    const unsigned validSamples[],
    unsigned count,
    unsigned channels);

  /**
   * Fused-path decode: decodes the group's callback-aligned spans into
   * the caller's interleaved scratch (scratch[c][srcSample*8+lane], the
   * DecodeBatchInterleaved layout - exactly what the batch render
   * kernel consumes), including each lane's fraction-phase tail and
   * guard, and captures each voice's m_BlockEndState at exactly
   * validSamples[i] (the dependent-forward chain point). Does NOT
   * persist anything into the voices' VoiceBlocks; each voice's block
   * is invalidated (validSamples = 0) so a later fallback rebuild never
   * sees stale data. Caller guarantees (via ClassifyForRender):
   * AVX2 present, count >= 1, all lanes same channel count, every span
   * incl. guard inside the section, spans within the scratch bound.
   */
  // Variant D with the transpose fused into the decoder's store (bench
  // F2): same protocol as DecodeGroupToScratch, output PLANAR per lane
  // (planes[c] + lane*planeStride). See GOSoundCompressionCacheV3Batch::
  // DecodeBatchPlanarMidCapture.
  GO_HOT static void DecodeGroupToPlanes(
    GOSoundStream *const streams[],
    const unsigned subblockStarts[],
    const unsigned validSamples[],
    unsigned count,
    unsigned channels,
    int32_t *const planes[MAX_OUTPUT_CHANNELS],
    unsigned planeStride);

  GO_HOT static void DecodeGroupToScratch(
    GOSoundStream *const streams[],
    const unsigned subblockStarts[],
    const unsigned validSamples[],
    unsigned count,
    unsigned channels,
    int32_t *const scratch[MAX_OUTPUT_CHANNELS]);

  /* Batch-render plumbing for the group task: the current resampling
   * fraction/increment for the kernel lane (the block-relative position
   * starts at 0 by construction - the block begins at this callback's
   * first source position), the coefficient table, and the exact
   * position advance after a batch-rendered callback (bit-identical to
   * nFrames sequential Inc() calls - see ResamplingPosition::AdvanceBy). */
  unsigned GetResamplingFraction() const {
    return m_ResamplingPos.GetFraction();
  }
  unsigned GetResamplingIncrement() const {
    return m_ResamplingPos.GetFractionIncrement();
  }
  const GOSoundResample *GetResample() const { return resample; }
  void AdvanceAfterBatchRender(unsigned nFrames) {
    m_ResamplingPos.AdvanceBy(nFrames);
  }

  /* Fused-path per-lane render, transpose variant D (2026-07-12):
   * resamples nFrames through the production polyphase from a contiguous
   * per-lane PLANE covering source indices [planeBase, planeBase+span) -
   * the group's decode scratch flipped planar by
   * GOSoundResampleBatch::TransposeToPlanes. Advances m_ResamplingPos in
   * place (per-frame Inc(), classic semantics), so the caller must NOT
   * also call AdvanceAfterBatchRender. Mono sections pass the same plane
   * twice; the vector's m_NChannels==1 makes ResampleBlock render once
   * and duplicate, exactly like the classic mono path. Segment logic is
   * absent by design: ClassifyForRender only routes a voice here when
   * the whole span is one plain forward MainSegment read. */
  /* Fused steady-linear variant: resamples and accumulates
   * out += fir * (gain + i*dGain) directly into the interleaved-stereo
   * output buffer - no 1 KB temp round-trip, no separate fader pass.
   * dGain == 0 keeps the former constant kernel bit-identically. Only
   * for voices whose fader confirmed TrySteadyLinearGain(). */
#ifdef GO_POLYPHASE_AVX2
  void RenderBlockFromPlanesAccum(
    const int32_t *plane0,
    const int32_t *plane1,
    unsigned planeBase,
    unsigned nChannels,
    float *out,
    unsigned nFrames,
    float gain,
    float dGain) {
    const GOSoundResample::PolyphaseResampler poly(*resample);

    if (nChannels > 1) {
      PlaneVecStereo vec;

      vec.plane[0] = plane0;
      vec.plane[1] = plane1;
      vec.base = planeBase;
      poly.ResampleBlockAccum<PlaneVecStereo, 2>(
        m_ResamplingPos, vec, out, nFrames, gain, dGain);
    } else {
      PlaneVecMono vec;

      vec.plane[0] = plane0;
      vec.base = planeBase;
      poly.ResampleBlockAccum<PlaneVecMono, 2>(
        m_ResamplingPos, vec, out, nFrames, gain, dGain);
    }
  }
#endif // GO_POLYPHASE_AVX2

  void RenderBlockFromPlanes(
    const int32_t *plane0,
    const int32_t *plane1,
    unsigned planeBase,
    unsigned nChannels,
    float *temp,
    unsigned nFrames) {
    const GOSoundResample::PolyphaseResampler poly(*resample);

    if (nChannels > 1) {
      PlaneVecStereo vec;

      vec.plane[0] = plane0;
      vec.plane[1] = plane1;
      vec.base = planeBase;
      poly.ResampleBlock<PlaneVecStereo, 2>(
        m_ResamplingPos, vec, temp, nFrames);
    } else {
      PlaneVecMono vec;

      vec.plane[0] = plane0;
      vec.base = planeBase;
      poly.ResampleBlock<PlaneVecMono, 2>(m_ResamplingPos, vec, temp, nFrames);
    }
  }

  /* Initialize a stream to play this audio section */
  void InitStream(
    const GOSoundResample *pResample,
    const GOSoundAudioSection *pSection,
    GOSoundResample::InterpolationType interpolationType,
    float sampleRateAdjustment);

  /**
   * Initializes a release stream aligned to the existing attack stream.
   * Uses release alignment table if available.
   */
  void InitAlignedStream(
    const GOSoundAudioSection *pSection,
    GOSoundResample::InterpolationType interpolation,
    const GOSoundStream *existing_stream,
    const char *debugLabel = nullptr);

  /* Read an audio buffer from an audio section stream */
  GO_HOT bool ReadBlock(float *buffer, unsigned int n_blocks);
};

/* SIZE GATE - see the extended rationale at the size gates in
 * GOSoundCompressionCacheV3.h.
 *
 * A stream is embedded in every sampler, and the sampler in turn is placed
 * in a cache-line-padded arena slot (SAMPLER_SLOT_SIZE, GOSoundSamplerPool
 * .cpp). Growing the stream therefore costs DRAM traffic per active voice
 * at the polyphony ceiling, and can additionally push the sampler over a
 * slot boundary - which changes the arena stride for every voice at once.
 *
 * If this fires, prefer moving the new member out of the stream (section
 * metadata, a side array indexed like the streams) over updating the
 * number. */
static_assert(
  MAX_OUTPUT_CHANNELS != 2 || sizeof(GOSoundStream) == 448,
  "GOSoundStream must stay 448 bytes - it is embedded per sampler and "
  "feeds the sampler slot stride; see the size-gate comment above");

#endif /* GOSOUNDSTREAM_H */
