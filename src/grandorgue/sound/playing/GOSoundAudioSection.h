/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDAUDIOSECTION_H_
#define GOSOUNDAUDIOSECTION_H_

#include <assert.h>
#include <math.h>

#include <array>
#include <atomic>

#include "GOBool3.h"
#include "GOInt.h"
#include "GOSoundAudioSectionCacheFormat.h"
#include "GOSoundCompressionCache.h"
#include "GOSoundCompressionCacheV3.h"
#include "GOSoundResample.h"
#include "GOWave.h"

// The codec library's own channel-count knob (V3CODEC_MAX_CHANNELS) must
// track GO's MAX_OUTPUT_CHANNELS (GOSoundDefs.h, pulled in transitively via
// GOSoundCompressionCache.h above) - this is the one place both are visible
// together, so a silent drift fails here, at compile time, not at runtime.
static_assert(
  MAX_OUTPUT_CHANNELS == V3CODEC_MAX_CHANNELS,
  "GO's MAX_OUTPUT_CHANNELS and the V3 codec library's V3CODEC_MAX_CHANNELS "
  "must match - see GOSoundCompressionCacheV3.h's config block");

class GOSoundAudioSection;
class GOCache;
class GOCacheObject;
class GOCacheWriter;
class GOLoaderFilename;
class GOMemoryPool;
class GOSoundReleaseAlignTable;
class GOSampleStatistic;

class GOSoundAudioSection {
public:
  static const unsigned getMaxReadAhead();

  /* Global V3 predictor mode, set from config before organ load (so the
   * encoder honors it) and stable for the session. The codec owns the flag
   * (it decides the block-header layout too, see GOSoundCompressionCacheV3);
   * these just forward so callers need not know where it lives. The encoder
   * reads it to force a fixed order or search 1..4; the render scheduler
   * reads IsVariablePredictorOrder() to decide whether to group by order.
   *
   * GOV3PredictorMode (GO's own, used throughout config/GUI) and the
   * library's V3PredictorMode are separate, identically-valued enums - this
   * is the one seam where GO crosses into the self-contained codec header,
   * so the cast lives here rather than coupling the library to GO's type. */
  static void SetPredictorMode(GOV3PredictorMode mode) {
    GOSoundCompressionCacheV3::SetPredictorMode((V3PredictorMode)(uint8_t)mode);
  }
  static GOV3PredictorMode GetPredictorMode() {
    return (GOV3PredictorMode)(
      uint8_t)GOSoundCompressionCacheV3::GetPredictorMode();
  }
  /* True in the Variable mode: the render scheduler then groups voices by
   * their section's order (bucket-key fold + batch flush) so every 8-lane
   * batch stays uniform-order. Gated off in the fixed modes - they keep
   * the single-order path untouched. */
  static bool IsVariablePredictorOrder() {
    return GOSoundCompressionCacheV3::IsVariableOrder();
  }

  /* Loaded-organ compression totals for the audio report (see
   * RegisterStorageStats): raw PCM bytes vs. bytes actually stored,
   * aggregated over all currently live sections. */
  static void GetCompressionTotals(uint64_t &outRaw, uint64_t &outStored) {
    outRaw = s_TotalRawBytes.load(std::memory_order_relaxed);
    outStored = s_TotalStoredBytes.load(std::memory_order_relaxed);
  }

  struct StartSegment {
    /* Sample offset into entire audio section where data begins. */
    unsigned start_offset;

    GOSoundCompressionCache cache;
  };

  struct StartSegmentV3 {
    /* Sample offset into entire audio section where data begins. */
    unsigned start_offset;

    GOSoundCompressionCacheV3::Snapshot cache;
  };

  /**
   * This segment contains copy of samples from a loop end and then from the
   * loop start. It is necessary for read-ahead when resampling
   */
  struct EndSegmentDescription {
    /* A position after the last copied sample in the end segment.
     * An end segment has more MAX_READAHEAD samples after end_pos:
     *     - A loop end segment contains a copy of samples from the loop start
     *     - A release end segment contains zero samples
     */
    unsigned end_pos;

    /* Sample offset where the uncompressed end data blob begins (must be less
     * than end_pos). */
    unsigned transition_offset;

    // The size of the end data in bytes
    unsigned end_size;

    /* Index of the next section segment to be played (-1 indicates the
     * end of the blob. */
    int next_start_segment_index;
  };

  struct EndSegment : public EndSegmentDescription {
    /* Uncompressed ending data blob. This data must start before
     * sample_offset*/
    unsigned char *end_data;

    // A virtual pointer to end_data. end_data has transition_offset to end_ptr
    unsigned char *end_ptr;
  };

private:
  void Compress(bool format16);
  void CompressV3(bool format16, unsigned uncompressedPrefixSamples);

  void GetMaxAmplitudeAndDerivative();

  void DoCrossfade(
    unsigned char *dest,
    unsigned dest_offset,
    const unsigned char *src,
    unsigned src_offset,
    unsigned fade_length,
    unsigned loop_length,
    unsigned length);

  std::vector<StartSegment> m_StartSegments;
  std::vector<StartSegmentV3> m_StartSegmentsV3;
  /* V3 seed snapshots at every distinct snapshot position CompressV3
   * captured: the start segment offsets (sorted by start_offset, unique).
   * Every voice's private decoded render block (GOSoundStream) needs a
   * resume state at or before a block's START; since the block grid is
   * anchored per voice at its current segment start (2026-07-09 bin
   * redesign), a post-wrap block begins exactly at a segment offset -
   * these seeds hit it with zero forward decode, for any bin length.
   * (Caches written by the older grid-floor design contain additional
   * floor-position seeds; they load fine and are simply redundant now.) */
  std::vector<StartSegmentV3> m_SubblockSeedsV3;
  /* 4th predictor tap (v[n-4]) for each start-segment / seed snapshot,
   * PARALLEL to m_StartSegmentsV3 / m_SubblockSeedsV3 by index - populated
   * only when m_PredictorOrder >= 4 (empty otherwise). Deliberately outside
   * the 40-byte Snapshot struct so the fixed modes' per-voice-start seed
   * traffic stays exactly v10-sized (cache v11 cost isolation). */
  std::vector<std::array<int, MAX_OUTPUT_CHANNELS>> m_SegHist3V3;
  std::vector<std::array<int, MAX_OUTPUT_CHANNELS>> m_SeedHist3V3;
  /* This section's V3 predictor order (1..4). Stage 1: always == the global
   * mode; stage 2 (per-section Variable) sets it from the encoder's search.
   * Serialized in the v11 envelope. */
  uint8_t m_PredictorOrder = 3;
  /* True while this section's storage bytes are counted in the report
   * statistics (see RegisterStorageStats). Shares the padding hole after
   * m_PredictorOrder - sizeof is part of the cache hash, see above. */
  bool m_StatsRegistered = false;

  /* Loaded-organ compression statistics for the audio report (raw PCM
   * bytes vs. bytes actually stored: main blob + uncompressed prefix +
   * end segments). Each section registers its contribution once its
   * storage is final (end of Setup / successful LoadCache) and withdraws
   * it in ClearData - the statics therefore always describe exactly the
   * currently loaded sections, with no reset choreography on reload.
   *
   * DELIBERATELY NO per-section byte members: sizeof(GOSoundAudioSection)
   * is part of the organ cache hash (GOOrganController::UpdateCacheHash's
   * ABI guard), so growing this class invalidates every user's cache -
   * caught 2026-07-18 when the first stats version added two uint64s and
   * forced a silent full rebuild. The withdraw amounts are recomputed
   * from the live fields instead (still valid at the top of ClearData),
   * and the only state is a bool in the padding hole after
   * m_PredictorOrder. Diagnostics must be layout-neutral. */
  static inline std::atomic<uint64_t> s_TotalRawBytes{0};
  static inline std::atomic<uint64_t> s_TotalStoredBytes{0};

  void ComputeStorageBytes(uint64_t &outRaw, uint64_t &outStored) const {
    uint64_t stored = (uint64_t)m_AllocSize
      + (uint64_t)m_UncompressedPrefixStoredSamples * m_BytesPerSample;
    for (const EndSegment &s : m_EndSegments)
      stored += s.end_size;
    outRaw = (uint64_t)m_SampleCount * m_BytesPerSample;
    outStored = stored;
  }

  void RegisterStorageStats() {
    uint64_t raw, stored;

    ComputeStorageBytes(raw, stored);
    s_TotalRawBytes.fetch_add(raw, std::memory_order_relaxed);
    s_TotalStoredBytes.fetch_add(stored, std::memory_order_relaxed);
    m_StatsRegistered = true;
  }
  std::vector<EndSegment> m_EndSegments;

  /* Uncompressed prefix (V3 sections only, opt-in per section via Setup):
   * the first m_UncompressedPrefixSamples of the section kept as raw PCM
   * (native sample format, interleaved - same layout as uncompressed
   * m_data) IN ADDITION to the compressed stream, so a voice START never
   * has to decode: attack starts read block 1 straight from here; release
   * starts (scattered over [0, pipe period) by release alignment) skip
   * both the state-building forward decode AND their first block's
   * decode. m_UncompressedPrefixStoredSamples = prefix + read-ahead guard
   * (clipped to the section length), so a block clipped at the prefix end
   * still finds its polyphase guard samples in raw form. The compressed
   * stream still covers the whole section (the prefix bytes exist twice;
   * ~0.1% of a section) and CompressV3 seeds a snapshot exactly at
   * m_UncompressedPrefixSamples, where the voice's block chain enters the
   * compressed stream. */
  unsigned char *m_UncompressedPrefixData;
  unsigned m_UncompressedPrefixSamples;
  unsigned m_UncompressedPrefixStoredSamples;

  /* Pointer to (size) bytes of data encoded in the format (type) */
  unsigned char *m_data;

  /* If this is a release section, it may contain an alignment table */
  GOSoundReleaseAlignTable *m_ReleaseAligner;
  unsigned m_ReleaseStartSegment;

  // Sequential index assigned across all releases of all pipes after loading.
  // Used to map GOLutCacheFile releaseMap entries to this section.
  // UINT_MAX = attack section or not yet indexed.
  unsigned m_releaseParseIndex;

  /* Number of significant bits in the decoded sample data */
  unsigned m_SampleFracBits;

  unsigned m_SampleCount;
  unsigned m_SampleRate;

  /* Type of the data which is stored in the data pointer */
  uint8_t m_BitsPerSample;
  uint8_t m_BytesPerSample;
  uint8_t m_channels;

  GOBool3 m_WaveTremulantStateFor;
  bool m_IsCompressed;
  GOSoundAudioSectionCompressionCodec m_CompressionCodec;

  /* Size of the section in BYTES */
  GOMemoryPool &m_Pool;
  unsigned m_AllocSize;

  unsigned m_MaxAmplitude;
  int m_MaxAbsAmplitude;
  int m_MaxAbsDerivative;
  unsigned m_ReleaseCrossfadeLength; // in ms

  // Basename of the source file (set in Setup, used for CSV logging).
  // Fixed-size to avoid pulling <string> into this header.
  char m_loaderBasename[256];

  void ClearData();

  template <typename T>
  inline static int getSampleData(
    const T *data, unsigned position, uint8_t channels, uint8_t channel) {
    return data[position * channels + channel];
  }

  inline static int getSampleData(
    const unsigned char *sampleData,
    unsigned position,
    uint8_t channels,
    uint8_t channel,
    uint8_t bitsPerSample) {
    int res;

    if (bitsPerSample <= 8)
      res = getSampleData(
        (const GOInt8 *)sampleData, position, channels, channel);
    else if (bitsPerSample <= 16)
      res = getSampleData(
        (const GOInt16 *)sampleData, position, channels, channel);
    else if (bitsPerSample <= 24)
      res = getSampleData(
        (const GOInt24 *)sampleData, position, channels, channel);
    else {
      assert(0 && "broken sampler type");
      res = 0;
    }
    return res;
  }

  template <typename T>
  inline static void setSampleData(
    T *data, unsigned position, uint8_t channels, uint8_t channel, int value) {
    data[position * channels + channel] = value;
  }

  inline static void setSampleData(
    unsigned char *sampleData,
    unsigned position,
    uint8_t channels,
    uint8_t channel,
    uint8_t bitsPerSample,
    int value) {
    if (bitsPerSample <= 8)
      setSampleData((GOInt8 *)sampleData, position, channels, channel, value);
    else if (bitsPerSample <= 16)
      setSampleData((GOInt16 *)sampleData, position, channels, channel, value);
    else if (bitsPerSample <= 24)
      setSampleData((GOInt24 *)sampleData, position, channels, channel, value);
    else
      assert(0 && "broken sampler type");
  }

  inline void SetSampleData(
    unsigned char *sampleData, unsigned position, uint8_t channel, int value) {
    setSampleData(
      sampleData, position, m_channels, channel, m_BitsPerSample, value);
  }

public:
  inline static unsigned limitedDiff(unsigned a, unsigned b) {
    return (a > b) ? a - b : 0;
  }

  GOSoundAudioSection(GOMemoryPool &pool);
  ~GOSoundAudioSection() { ClearData(); }

  GOSoundReleaseAlignTable *GetReleaseAligner() const {
    return m_ReleaseAligner;
  }
  unsigned GetReleaseStartSegment() const { return m_ReleaseStartSegment; }
  unsigned GetSampleRate() const { return m_SampleRate; }
  uint8_t GetBitsPerSample() const { return m_BitsPerSample; }
  uint8_t GetBytesPerSample() const { return m_BytesPerSample; }
  inline uint8_t GetChannels() const { return m_channels; }
  bool IsCompressed() const { return m_IsCompressed; }
  GOSoundAudioSectionCompressionCodec GetCompressionCodec() const {
    return m_CompressionCodec;
  }

  inline GOBool3 GetWaveTremulantStateFor() const {
    return m_WaveTremulantStateFor;
  }

  inline unsigned GetLength() const { return m_SampleCount; }

  // Returns the basename of the source WAV file (empty string if not set).
  const char *GetLoaderBasename() const { return m_loaderBasename; }

  // Returns the sample offset of the first sustain loop, or 0 if no loop.
  inline unsigned GetLoopStart() const {
    return (m_StartSegments.size() > 1) ? m_StartSegments[1].start_offset : 0;
  }

  // Returns the latest loop-end sample position across all sustain loops,
  // or 0 if no sustain loops exist.
  // end_seg.end_pos = loop.m_EndPosition + 1 (exclusive), so end = end_pos - 1.
  // next_start_segment_index >= 0 identifies loop ends (vs. release end = -1).
  inline unsigned GetLatestLoopEnd() const {
    unsigned latest = 0;
    for (const auto &seg : m_EndSegments)
      if (seg.next_start_segment_index >= 0 && seg.end_pos > 0)
        latest = std::max(latest, seg.end_pos - 1);
    return latest;
  }

  // Returns the sample position of the first sustain loop end (inclusive,
  // 0-based), matching Python's loops[0][1] from the SMPL chunk. Finds the
  // EndSegment whose next_start_segment_index == 1
  // (= m_StartSegments[1], the first loop start).
  // Falls back to GetLatestLoopEnd() for single-loop pipes or unusual
  // orderings.
  inline unsigned GetFirstLoopEnd() const {
    for (const auto &seg : m_EndSegments)
      if (seg.next_start_segment_index == 1 && seg.end_pos > 0)
        return seg.end_pos - 1;
    return GetLatestLoopEnd();
  }

  inline unsigned GetStartSegmentCount() const {
    return (unsigned)m_StartSegments.size();
  }

  /* Uncompressed prefix accessors (see m_UncompressedPrefixData). A voice
   * block starting below GetUncompressedPrefixSamples() can be filled via
   * ReadPrefixSample() without touching the decoder at all. */
  inline unsigned GetUncompressedPrefixSamples() const {
    return m_UncompressedPrefixSamples;
  }
  inline unsigned GetUncompressedPrefixStoredSamples() const {
    return m_UncompressedPrefixStoredSamples;
  }
  inline int ReadPrefixSample(unsigned position, uint8_t channel) const {
    return getSampleData(
      m_UncompressedPrefixData, position, m_channels, channel, m_BitsPerSample);
  }

  inline unsigned GetEndSegmentCount() const {
    return (unsigned)m_EndSegments.size();
  }

  unsigned GetReleaseCrossfadeLength() const {
    return m_ReleaseCrossfadeLength;
  }

  const StartSegment &GetStartSegment(unsigned index) const {
    return m_StartSegments[index];
  }

  const StartSegmentV3 &GetStartSegmentV3(unsigned index) const {
    return m_StartSegmentsV3[index];
  }

  unsigned GetSubblockSeedCountV3() const {
    return (unsigned)m_SubblockSeedsV3.size();
  }

  // This section's V3 predictor order (stage 1: == the mode; see
  // m_PredictorOrder). Streams take their per-voice m_order from here.
  uint8_t GetPredictorOrder() const { return m_PredictorOrder; }

  /* The 4th-tap seed for a start segment / a seed snapshot returned by
   * FindV3SnapshotAtOrBefore - null unless this section is order 4 (the
   * parallel arrays are empty then; LoadSnapshot zeroes the taps). */
  const int *GetSegHist3V3(unsigned index) const {
    return index < m_SegHist3V3.size() ? m_SegHist3V3[index].data() : nullptr;
  }
  const int *GetSeedHist3V3(const StartSegmentV3 *seed) const {
    if (m_SeedHist3V3.empty())
      return nullptr;
    return m_SeedHist3V3[(size_t)(seed - m_SubblockSeedsV3.data())].data();
  }

  /**
   * The nearest V3 resume snapshot at or before `position`, searched over
   * the subblock seed snapshots (superset of the start segments). Never
   * null for a V3 section: position 0 always has a snapshot. Used by
   * GOSoundStream to prepare each voice's private decoded render block.
   */
  const StartSegmentV3 *FindV3SnapshotAtOrBefore(unsigned position) const;

  const EndSegment &GetEndSegment(unsigned index) const {
    return m_EndSegments[index];
  }

  const unsigned char *GetData() const { return m_data; }

  inline int GetSampleData(
    const unsigned char *sampleData, unsigned position, uint8_t channel) const {
    return getSampleData(
      sampleData, position, m_channels, channel, m_BitsPerSample);
  }

  bool LoadCache(GOCache &cache);
  bool SaveCache(GOCacheWriter &cache) const;

  void Setup(
    const GOCacheObject *pObjectFor,
    const GOLoaderFilename *pLoaderFilename,
    const void *pcm_data,
    GOWave::SAMPLE_FORMAT pcm_data_format,
    unsigned pcm_data_channels,
    unsigned pcm_data_sample_rate,
    unsigned pcm_data_nb_samples,
    const std::vector<GOWaveLoop> *loop_points,
    GOBool3 waveTremulantStateFor,
    bool compress,
    bool useV3Compression,
    unsigned loopCrossfadeLength,
    unsigned releaseCrossfadeLength,
    unsigned uncompressedPrefixSamples = 0);

  inline bool IsOneshot() const {
    return (m_EndSegments.size() == 1)
      && (m_EndSegments[0].next_start_segment_index < 0);
  }

  unsigned PickEndSegment(unsigned start_segment_index) const;

  /**
   * Codec-aware sequential sample reader. Callers that walk a section
   * (forward, possibly with gaps) MUST use this instead of bringing their
   * own GOSoundCompressionCache: a caller-owned cache of the WRONG codec
   * type forces GetSample() onto its cross-codec bridge, which cannot
   * carry decode progress between calls - every call then re-decodes from
   * position 0, turning an O(n) scan into O(n^2). On a real organ this
   * showed up as a load-time freeze at 100% CPU (release-alignment scans
   * over a several-hundred-thousand-sample V3 section ~ 1e11 decode
   * steps). This reader holds BOTH codec states and lets GetSample() pick
   * the matching one, so sequential access stays O(n) for every codec.
   */
  class SampleReader {
    friend class GOSoundAudioSection;
    GOSoundCompressionCache m_legacy;
    GOSoundCompressionCacheV3 m_v3;

  public:
    SampleReader() { Reset(); }
    void Reset() {
      m_legacy.Init();
      m_v3.Init();
    }
  };

  inline int GetSample(
    unsigned position, unsigned channel, SampleReader &reader) const {
    if (!m_IsCompressed
        || m_CompressionCodec == GOSoundAudioSectionCompressionCodec::None)
      return GetSampleData(m_data, position, channel);
    if (m_CompressionCodec == GOSoundAudioSectionCompressionCodec::V3)
      return GetSample(position, channel, &reader.m_v3);
    return GetSample(position, channel, &reader.m_legacy);
  }

  inline int GetSample(
    unsigned position,
    unsigned channel,
    GOSoundCompressionCache *cache = nullptr) const {
    if (!m_IsCompressed
        || m_CompressionCodec == GOSoundAudioSectionCompressionCodec::None) {
      return GetSampleData(m_data, position, channel);
    }
    if (m_CompressionCodec == GOSoundAudioSectionCompressionCodec::V3) {
      /* Cross-codec bridge (caller brought a legacy cache, or none, to a
       * V3 section). The thread_local state must survive across calls -
       * resetting it per call re-decodes from position 0 every time and
       * makes sequential scans O(n^2) (the "frozen at 100% CPU on organ
       * load" bug). Only reset when this thread's bridge state belongs to
       * a different section's data; backward seeks within the same data
       * are handled by DecompressTo() itself. */
      thread_local GOSoundCompressionCacheV3 tmpV3;
      thread_local const unsigned char *tmpV3Data = nullptr;

      if (tmpV3Data != m_data) {
        tmpV3.Init();
        tmpV3Data = m_data;
      }
      return GetSample(position, channel, &tmpV3);
    }
    GOSoundCompressionCache tmp;
    if (!cache) {
      cache = &tmp;
      cache->Init();
    }

    assert(m_BitsPerSample >= 12);
    cache->DecompressTo(position, m_data, m_channels, (m_BitsPerSample >= 20));
    return cache->m_value[channel];
  }

  inline int GetSample(
    unsigned position,
    unsigned channel,
    GOSoundCompressionCacheV3 *cache) const {
    if (!m_IsCompressed
        || m_CompressionCodec == GOSoundAudioSectionCompressionCodec::None) {
      return GetSampleData(m_data, position, channel);
    }
    if (m_CompressionCodec == GOSoundAudioSectionCompressionCodec::Legacy) {
      /* Cross-codec bridge, mirrored - see the V3 bridge above for why the
       * state must persist across calls. */
      thread_local GOSoundCompressionCache tmpLegacy;
      thread_local const unsigned char *tmpLegacyData = nullptr;

      if (tmpLegacyData != m_data) {
        tmpLegacy.Init();
        tmpLegacyData = m_data;
      }
      return GetSample(position, channel, &tmpLegacy);
    }
    GOSoundCompressionCacheV3 tmp;
    if (!cache) {
      cache = &tmp;
      cache->Init();
    }

    // This section owns the order (Variable: per section, v11) - hand it
    // to the decoder for its internal reset path.
    cache->DecompressTo(position, m_data, m_channels, m_PredictorOrder);
    return cache->m_value[channel];
  }

  inline float GetNormGain() const {
    return scalbnf(1.0f, -((int)m_SampleFracBits));
  }

  inline bool SupportsStreamAlignment() const { return (m_ReleaseAligner); }

  void SetReleaseParseIndex(unsigned idx) { m_releaseParseIndex = idx; }
  unsigned GetReleaseParseIndex() const { return m_releaseParseIndex; }

  void SetupStreamAlignment(
    const std::vector<const GOSoundAudioSection *> &joinables,
    unsigned start_index,
    float sample_freq_hz = 440.f,
    unsigned harmonic_number = 8,
    unsigned min_key_press_ms = 0,
    unsigned max_key_press_ms = 0,
    bool skipCorrLut = false
#if __has_include("GOLogReleaseAlignEnable.h")
    ,
    const char *label = nullptr
#endif
  );

  void AssignAttackLutPointers(
    const std::vector<const GOSoundAudioSection *> &attacks);

  GOSampleStatistic GetStatistic();
};

#endif /* GOSOUNDAUDIOSECTION_H_ */
