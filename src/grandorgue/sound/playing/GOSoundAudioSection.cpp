/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundAudioSection.h"

#include <wx/filename.h>
#include <wx/intl.h>
#include <wx/log.h>

#include "loader/cache/GOCache.h"
#include "loader/cache/GOCacheWriter.h"

#include "loader/GOLoaderFilename.h"
#include "model/GOCacheObject.h"

#include "../GOCrossfadeParam.h"
#include "GOAlloc.h"
#include "GOMemoryPool.h"
#include "GOSampleStatistic.h"
#include "GOSoundAudioSectionCacheFormat.h"
#include "GOSoundCompressionCache.h"
#include "GOSoundCompressionCacheV3.h"
#include "GOSoundReleaseAlignTable.h"
#include "GOSoundResample.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// maximal readahead is necessary for polyphase resampling
static constexpr unsigned MAX_READAHEAD = GOSoundResample::POLYPHASE_POINTS;
static constexpr unsigned DEFAULT_END_SEG_LENGTH = MAX_READAHEAD * 2;
static constexpr bool kForceLegacyLoopCrossfade
  = true; // when true, cached loop crossfades keep legacy cosine method

const unsigned GOSoundAudioSection::getMaxReadAhead() { return MAX_READAHEAD; }

GOSoundAudioSection::GOSoundAudioSection(GOMemoryPool &pool)
  : m_UncompressedPrefixData(NULL),
    m_UncompressedPrefixSamples(0),
    m_UncompressedPrefixStoredSamples(0),
    m_data(NULL),
    m_ReleaseAligner(NULL),
    m_ReleaseStartSegment(0),
    m_releaseParseIndex((unsigned)-1),
    m_Pool(pool) {
  ClearData();
}

void GOSoundAudioSection::ClearData() {
  // Withdraw this section's report-statistics contribution (see
  // RegisterStorageStats) - runs on destruction AND on reload, BEFORE the
  // fields below are cleared (the amounts are recomputed from them).
  if (m_StatsRegistered) {
    uint64_t raw, stored;

    ComputeStorageBytes(raw, stored);
    s_TotalRawBytes.fetch_sub(raw, std::memory_order_relaxed);
    s_TotalStoredBytes.fetch_sub(stored, std::memory_order_relaxed);
    m_StatsRegistered = false;
  }
  m_loaderBasename[0] = '\0';
  m_AllocSize = 0;
  m_SampleCount = 0;
  m_SampleRate = 0;
  m_BitsPerSample = 0;
  m_BytesPerSample = 0;
  m_WaveTremulantStateFor = BOOL3_DEFAULT;
  m_IsCompressed = false;
  m_CompressionCodec = GOSoundAudioSectionCompressionCodec::None;
  m_channels = 0;
  if (m_data) {
    m_Pool.Free(m_data);
    m_data = NULL;
  }
  if (m_ReleaseAligner) {
    delete m_ReleaseAligner;
    m_ReleaseAligner = NULL;
  }
  m_SampleFracBits = 0;
  while (m_EndSegments.size()) {
    if (m_EndSegments.back().end_data)
      m_Pool.Free(m_EndSegments.back().end_data);
    m_EndSegments.pop_back();
  }
  m_StartSegments.clear();
  m_StartSegmentsV3.clear();
  m_SubblockSeedsV3.clear();
  if (m_UncompressedPrefixData) {
    m_Pool.Free(m_UncompressedPrefixData);
    m_UncompressedPrefixData = NULL;
  }
  m_UncompressedPrefixSamples = 0;
  m_UncompressedPrefixStoredSamples = 0;
  m_ReleaseCrossfadeLength = 0;
}

bool GOSoundAudioSection::LoadCache(GOCache &cache) {
  uint32_t firstWord = 0;
  bool hasNewHeader = false;
  GOSoundAudioSectionCompressionCodec compressionCodec
    = GOSoundAudioSectionCompressionCodec::None;

  if (!cache.Read(&firstWord, sizeof(firstWord)))
    return false;
  if (firstWord == GO_SOUND_AUDIO_SECTION_CACHE_MAGIC) {
    hasNewHeader = true;

    uint16_t version = 0;
    uint8_t codec = 0;
    uint8_t flags = 0;
    if (!cache.Read(&version, sizeof(version)))
      return false;
    if (version != GO_SOUND_AUDIO_SECTION_CACHE_VERSION)
      return false;
    if (!cache.Read(&codec, sizeof(codec)))
      return false;
    if (!IsValidGOSoundAudioSectionCompressionCodec(codec))
      return false;
    compressionCodec = (GOSoundAudioSectionCompressionCodec)codec;
    if (!cache.Read(&flags, sizeof(flags)))
      return false;
    if (flags != 0)
      return false;
    // Cache version 11: this section's predictor order (1..4). Order-4
    // sections additionally carry per-snapshot hist3 taps (read below).
    if (!cache.Read(&m_PredictorOrder, sizeof(m_PredictorOrder)))
      return false;
    if (m_PredictorOrder < 1 || m_PredictorOrder > 4)
      return false;
    if (!cache.Read(&m_AllocSize, sizeof(m_AllocSize)))
      return false;
  } else {
    m_AllocSize = firstWord;
  }

  if (!cache.Read(&m_SampleCount, sizeof(m_SampleCount)))
    return false;
  if (!cache.Read(&m_SampleRate, sizeof(m_SampleRate)))
    return false;
  if (!cache.Read(&m_BitsPerSample, sizeof(m_BitsPerSample)))
    return false;
  if (!cache.Read(&m_BytesPerSample, sizeof(m_BytesPerSample)))
    return false;
  if (!cache.Read(&m_WaveTremulantStateFor, sizeof(m_WaveTremulantStateFor)))
    return false;
  if (hasNewHeader) {
    m_IsCompressed
      = compressionCodec == GOSoundAudioSectionCompressionCodec::Legacy;
  } else {
    if (!cache.Read(&m_IsCompressed, sizeof(m_IsCompressed)))
      return false;
  }
  if (!cache.Read(&m_channels, sizeof(m_channels)))
    return false;
  if (!cache.Read(&m_SampleFracBits, sizeof(m_SampleFracBits)))
    return false;
  if (!cache.Read(&m_MaxAmplitude, sizeof(m_MaxAmplitude)))
    return false;
  if (!cache.Read(&m_ReleaseStartSegment, sizeof(m_ReleaseStartSegment)))
    return false;
  if (!cache.Read(&m_ReleaseCrossfadeLength, sizeof(m_ReleaseCrossfadeLength)))
    return false;
  m_data = (unsigned char *)cache.ReadBlock(m_AllocSize);
  if (!m_data)
    return false;

  m_StartSegments.clear();
  m_StartSegmentsV3.clear();
  m_SubblockSeedsV3.clear();
  m_SegHist3V3.clear();
  m_SeedHist3V3.clear();
  if (hasNewHeader) {
    m_IsCompressed
      = compressionCodec != GOSoundAudioSectionCompressionCodec::None;
    m_CompressionCodec = compressionCodec;
  } else {
    /* m_IsCompressed was already read above, in its legacy stream position
     * (before m_channels) - the legacy format stores it exactly once.
     * Reading it a second time here shifted the stream cursor by one byte,
     * so every following length field (start/end segment counts, EndSegment
     * end_size) was garbage - ReadBlock() then faulted on a bogus
     * multi-gigabyte length. This crashed every organ load from a cache
     * written WITHOUT V3 (the V3 setting makes SaveCache use the new
     * explicit header, whose read path doesn't come through here - which is
     * why enabling V3 appeared to "fix" it). */
    m_CompressionCodec
      = m_IsCompressed ? GOSoundAudioSectionCompressionCodec::Legacy
                       : GOSoundAudioSectionCompressionCodec::None;
  }

  unsigned temp;
  if (m_CompressionCodec == GOSoundAudioSectionCompressionCodec::V3) {
    /* Cache version 11: order-4 sections store each snapshot's 4th tap
     * right after its 40-byte Snapshot; fixed 1-3 sections' snapshot
     * stream is bit-identical to v10 (cost isolation - see the format
     * header). The taps live in index-parallel arrays, never in the
     * Snapshot struct. */
    const bool hasHist3 = m_PredictorOrder >= 4;

    if (!cache.Read(&temp, sizeof(temp)))
      return false;
    m_StartSegments.reserve(temp);
    m_StartSegmentsV3.reserve(temp);
    if (hasHist3)
      m_SegHist3V3.reserve(temp);
    for (unsigned i = 0; i < temp; i++) {
      StartSegment s;
      StartSegmentV3 sV3;
      s.cache.Init();
      if (!cache.Read(&s.start_offset, sizeof(s.start_offset)))
        return false;
      sV3.start_offset = s.start_offset;
      if (!cache.Read(&sV3.cache, sizeof(sV3.cache)))
        return false;
      if (hasHist3) {
        std::array<int, MAX_OUTPUT_CHANNELS> h3;
        if (!cache.Read(h3.data(), sizeof(h3)))
          return false;
        m_SegHist3V3.push_back(h3);
      }
      m_StartSegments.push_back(s);
      m_StartSegmentsV3.push_back(sV3);
    }

    // Cache version 2: subblock seed snapshots (see m_SubblockSeedsV3).
    unsigned seedCount;

    if (!cache.Read(&seedCount, sizeof(seedCount)))
      return false;
    m_SubblockSeedsV3.reserve(seedCount);
    if (hasHist3)
      m_SeedHist3V3.reserve(seedCount);
    for (unsigned i = 0; i < seedCount; i++) {
      StartSegmentV3 seed;

      if (!cache.Read(&seed.start_offset, sizeof(seed.start_offset)))
        return false;
      if (!cache.Read(&seed.cache, sizeof(seed.cache)))
        return false;
      if (hasHist3) {
        std::array<int, MAX_OUTPUT_CHANNELS> h3;
        if (!cache.Read(h3.data(), sizeof(h3)))
          return false;
        m_SeedHist3V3.push_back(h3);
      }
      m_SubblockSeedsV3.push_back(seed);
    }

    // Cache version 3: uncompressed prefix (see m_UncompressedPrefixData).
    if (!cache.Read(
          &m_UncompressedPrefixSamples, sizeof(m_UncompressedPrefixSamples)))
      return false;
    if (!cache.Read(
          &m_UncompressedPrefixStoredSamples,
          sizeof(m_UncompressedPrefixStoredSamples)))
      return false;
    if (
      m_UncompressedPrefixSamples > m_UncompressedPrefixStoredSamples
      || m_UncompressedPrefixStoredSamples > m_SampleCount)
      return false;
    if (m_UncompressedPrefixStoredSamples) {
      m_UncompressedPrefixData = (unsigned char *)cache.ReadBlock(
        (size_t)m_UncompressedPrefixStoredSamples * m_BytesPerSample);
      if (!m_UncompressedPrefixData)
        return false;
    }
  } else {
    if (!cache.Read(&temp, sizeof(temp)))
      return false;
    for (unsigned i = 0; i < temp; i++) {
      StartSegment s;
      if (!cache.Read(&s, sizeof(s)))
        return false;
      m_StartSegments.push_back(s);
    }
  }

  if (!cache.Read(&temp, sizeof(temp)))
    return false;
  for (unsigned i = 0; i < temp; i++) {
    EndSegment s;

    if (!cache.Read(
          static_cast<EndSegmentDescription *>(&s),
          sizeof(EndSegmentDescription)))
      return false;
    s.end_data = (unsigned char *)cache.ReadBlock(s.end_size);
    if (!s.end_data)
      return false;
    s.end_ptr = s.end_data - m_BytesPerSample * s.transition_offset;

    m_EndSegments.push_back(s);
  }

  bool load_align_tracker;
  if (!cache.Read(&load_align_tracker, sizeof(load_align_tracker)))
    return false;
  m_ReleaseAligner = NULL;
  if (load_align_tracker) {
    m_ReleaseAligner = new GOSoundReleaseAlignTable();
    if (!m_ReleaseAligner->Load(cache))
      return false;
  }

  RegisterStorageStats();
  return true;
}

bool GOSoundAudioSection::SaveCache(GOCacheWriter &cache) const {
  const bool save_v3
    = (m_CompressionCodec == GOSoundAudioSectionCompressionCodec::V3);
  if (save_v3) {
    const uint32_t magic = GO_SOUND_AUDIO_SECTION_CACHE_MAGIC;
    const uint16_t version = GO_SOUND_AUDIO_SECTION_CACHE_VERSION;
    const uint8_t codec = (uint8_t)m_CompressionCodec;
    const uint8_t flags = 0;
    if (!cache.Write(&magic, sizeof(magic)))
      return false;
    if (!cache.Write(&version, sizeof(version)))
      return false;
    if (!cache.Write(&codec, sizeof(codec)))
      return false;
    if (!cache.Write(&flags, sizeof(flags)))
      return false;
    // Cache version 11: this section's predictor order (see LoadCache).
    if (!cache.Write(&m_PredictorOrder, sizeof(m_PredictorOrder)))
      return false;
    if (!cache.Write(&m_AllocSize, sizeof(m_AllocSize)))
      return false;
  } else {
    if (!cache.Write(&m_AllocSize, sizeof(m_AllocSize)))
      return false;
  }
  if (!cache.Write(&m_SampleCount, sizeof(m_SampleCount)))
    return false;
  if (!cache.Write(&m_SampleRate, sizeof(m_SampleRate)))
    return false;
  if (!cache.Write(&m_BitsPerSample, sizeof(m_BitsPerSample)))
    return false;
  if (!cache.Write(&m_BytesPerSample, sizeof(m_BytesPerSample)))
    return false;
  if (!cache.Write(&m_WaveTremulantStateFor, sizeof(m_WaveTremulantStateFor)))
    return false;
  if (!save_v3) {
    if (!cache.Write(&m_IsCompressed, sizeof(m_IsCompressed)))
      return false;
  }
  if (!cache.Write(&m_channels, sizeof(m_channels)))
    return false;
  if (!cache.Write(&m_SampleFracBits, sizeof(m_SampleFracBits)))
    return false;
  if (!cache.Write(&m_MaxAmplitude, sizeof(m_MaxAmplitude)))
    return false;
  if (!cache.Write(&m_ReleaseStartSegment, sizeof(m_ReleaseStartSegment)))
    return false;
  if (!cache.Write(&m_ReleaseCrossfadeLength, sizeof(m_ReleaseCrossfadeLength)))
    return false;
  if (!cache.WriteBlock(m_data, m_AllocSize))
    return false;

  unsigned temp;

  temp = m_StartSegments.size();
  if (!cache.Write(&temp, sizeof(unsigned)))
    return false;
  if (save_v3) {
    /* Cache version 11: order-4 sections append each snapshot's 4th tap
     * (see LoadCache); the arrays must then be index-parallel. */
    const bool hasHist3 = m_PredictorOrder >= 4;

    if (m_StartSegmentsV3.size() != m_StartSegments.size())
      return false;
    if (hasHist3
        && (m_SegHist3V3.size() != m_StartSegmentsV3.size()
            || m_SeedHist3V3.size() != m_SubblockSeedsV3.size()))
      return false;
    for (unsigned i = 0; i < temp; i++) {
      const StartSegmentV3 *s = &m_StartSegmentsV3[i];
      if (!cache.Write(&s->start_offset, sizeof(s->start_offset)))
        return false;
      if (!cache.Write(&s->cache, sizeof(s->cache)))
        return false;
      if (hasHist3
          && !cache.Write(m_SegHist3V3[i].data(), sizeof(m_SegHist3V3[i])))
        return false;
    }

    // Cache version 2: subblock seed snapshots (see m_SubblockSeedsV3).
    const unsigned seedCount = (unsigned)m_SubblockSeedsV3.size();

    if (!cache.Write(&seedCount, sizeof(seedCount)))
      return false;
    for (unsigned i = 0; i < seedCount; i++) {
      const StartSegmentV3 &seed = m_SubblockSeedsV3[i];
      if (!cache.Write(&seed.start_offset, sizeof(seed.start_offset)))
        return false;
      if (!cache.Write(&seed.cache, sizeof(seed.cache)))
        return false;
      if (hasHist3
          && !cache.Write(m_SeedHist3V3[i].data(), sizeof(m_SeedHist3V3[i])))
        return false;
    }

    // Cache version 3: uncompressed prefix (see m_UncompressedPrefixData).
    if (!cache.Write(
          &m_UncompressedPrefixSamples, sizeof(m_UncompressedPrefixSamples)))
      return false;
    if (!cache.Write(
          &m_UncompressedPrefixStoredSamples,
          sizeof(m_UncompressedPrefixStoredSamples)))
      return false;
    if (m_UncompressedPrefixStoredSamples)
      if (!cache.WriteBlock(
            m_UncompressedPrefixData,
            (size_t)m_UncompressedPrefixStoredSamples * m_BytesPerSample))
        return false;
  } else {
    for (unsigned i = 0; i < temp; i++) {
      const StartSegment *s = &m_StartSegments[i];
      if (!cache.Write(s, sizeof(*s)))
        return false;
    }
  }

  temp = m_EndSegments.size();
  if (!cache.Write(&temp, sizeof(temp)))
    return false;
  for (unsigned i = 0; i < temp; i++) {
    const EndSegment &s = m_EndSegments[i];

    if (!cache.Write(
          static_cast<const EndSegmentDescription *>(&s),
          sizeof(EndSegmentDescription)))
      return false;
    if (!cache.WriteBlock(s.end_data, s.end_size))
      return false;
  }

  const bool save_align_tracker = (m_ReleaseAligner != NULL);
  if (!cache.Write(&save_align_tracker, sizeof(save_align_tracker)))
    return false;

  if (save_align_tracker) {
    if (!m_ReleaseAligner->Save(cache))
      return false;
  }

  return true;
}

unsigned GOSoundAudioSection::PickEndSegment(
  unsigned start_segment_index) const {
  const unsigned x = abs(rand());
  for (unsigned i = 0; i < m_EndSegments.size(); i++) {
    const unsigned idx = (i + x) % m_EndSegments.size();
    const EndSegment *end = &m_EndSegments[idx];
    if (
      end->transition_offset
      >= m_StartSegments[start_segment_index].start_offset)
      return idx;
  }
  assert(0 && "should always find suitable end segment");
  return 0;
}

static inline unsigned wave_bits_per_sample(GOWave::SAMPLE_FORMAT format) {
  switch (format) {
  case GOWave::SF_SIGNEDBYTE_8:
    return 8;
  case GOWave::SF_SIGNEDSHORT_12:
    return 12;
  case GOWave::SF_SIGNEDSHORT_16:
    return 16;
  case GOWave::SF_SIGNEDINT24_20:
    return 20;
  case GOWave::SF_SIGNEDINT24_24:
    return 24;
  case GOWave::SF_IEEE_FLOAT:
    return 32;
  default:
    assert(0 && "bad sample format enumeration");
    return 0;
  }
}

static inline unsigned wave_bytes_per_sample(GOWave::SAMPLE_FORMAT format) {
  return (wave_bits_per_sample(format) + 7) / 8;
}

static inline void loop_memcpy(
  unsigned char *dest,
  const unsigned char *source,
  unsigned source_len,
  unsigned count) {
  while (count > source_len) {
    memcpy(dest, source, source_len);
    dest += source_len;
    count -= source_len;
  }
  memcpy(dest, source, count);
}

void GOSoundAudioSection::GetMaxAmplitudeAndDerivative() {
  SampleReader cache;

  m_MaxAmplitude = 0;
  m_MaxAbsAmplitude = 0;
  m_MaxAbsDerivative = 0;

  int f_p = 0; /* to avoid compiler warning */
  for (unsigned int i = 0; i < m_SampleCount; i++) {
    /* Get sum of amplitudes in channels */
    int f = 0;
    for (unsigned int j = 0; j < m_channels; j++) {
      int val = GetSample(i, j, cache);
      f += val;
      if (abs(val) > m_MaxAmplitude)
        m_MaxAmplitude = abs(val);
    }

    if (abs(f) > m_MaxAbsAmplitude)
      m_MaxAbsAmplitude = abs(f);

    if (i != 0) {
      /* Get v */
      int v = f - f_p;
      if (abs(v) > m_MaxAbsDerivative)
        m_MaxAbsDerivative = abs(v);
    }
    f_p = f;
  }
}

void GOSoundAudioSection::DoCrossfade(
  unsigned char *dest,
  unsigned dest_offset,
  const unsigned char *src,
  unsigned src_offset,
  unsigned fade_length,
  unsigned loop_length,
  unsigned length) {
  for (; dest_offset < length; dest_offset += loop_length)
    for (unsigned pos = 0; pos < fade_length; pos++) {
      // t in [0,1] for the crossfade position
      float t = float(pos + 0.5f) / float(fade_length);
      if (t < 0.0f)
        t = 0.0f;
      else if (t > 1.0f)
        t = 1.0f;

      if (kForceLegacyLoopCrossfade) {
        // legacy behaviour used for cached loop crossfades: cosine-based factor
        float factor = (cos(M_PI * (pos + 0.5) / fade_length) + 1.0) * 0.5;

        for (uint8_t j = 0; j < m_channels; j++) {
          float val1 = GetSampleData(dest, pos + dest_offset, j);
          float val2 = GetSampleData(src, pos + src_offset, j);
          float result = val1 * factor + val2 * (1.0f - factor);

          SetSampleData(dest, pos + dest_offset, j, (int)result);
        }
      } else {
        // runtime-configurable crossfade (uses current mode). kept ready for
        // future switching.
        using namespace GOAudioParams;
        const auto mode = GetCrossfadeMode();
        const auto g = go_crossfade_eval(mode, t);

        for (uint8_t j = 0; j < m_channels; j++) {
          float val1 = GetSampleData(dest, pos + dest_offset, j);
          float val2 = GetSampleData(src, pos + src_offset, j);
          float result = g.a * val1 + g.b * val2;

          SetSampleData(dest, pos + dest_offset, j, (int)result);
        }
      }
    }
}

void GOSoundAudioSection::Setup(
  const GOCacheObject *pObjectFor,
  const GOLoaderFilename *pLoaderFilename,
  const void *pcm_data,
  const GOWave::SAMPLE_FORMAT pcm_data_format,
  const unsigned pcm_data_channels,
  const unsigned pcm_data_sample_rate,
  const unsigned pcm_data_nb_samples,
  const std::vector<GOWaveLoop> *loop_points,
  GOBool3 waveTremulantStateFor,
  bool compress,
  bool useV3Compression,
  unsigned loopCrossfadeLength,
  unsigned releaseCrossfadeLength,
  unsigned uncompressedPrefixSamples) {
  if (pcm_data_channels < 1 || pcm_data_channels > 2)
    throw(wxString) _("< More than 2 channels in");

  m_channels = pcm_data_channels;
  m_BitsPerSample = wave_bits_per_sample(pcm_data_format);
  compress = (compress) && (m_BitsPerSample > 8);

  // Store the source basename for attack-variant identification in CSV logging.
  m_loaderBasename[0] = '\0';
  if (pLoaderFilename) {
    wxFileName fn(pLoaderFilename->GetPath());
    fn.SetPath(wxEmptyString); // keep only name+ext
    strncpy(
      m_loaderBasename,
      fn.GetFullName().utf8_str(),
      sizeof(m_loaderBasename) - 1);
    m_loaderBasename[sizeof(m_loaderBasename) - 1] = '\0';
  }

  unsigned fade_len = loopCrossfadeLength * pcm_data_sample_rate / 1000;

  m_ReleaseCrossfadeLength = releaseCrossfadeLength;
  assert(pcm_data_nb_samples > 0);

  const unsigned bytes_per_sample = wave_bytes_per_sample(pcm_data_format);

  m_BytesPerSample = bytes_per_sample * pcm_data_channels;

  unsigned total_alloc_samples = pcm_data_nb_samples;
  /* Create a start segment */
  {
    StartSegment start_seg;
    start_seg.start_offset = 0;
    m_StartSegments.push_back(start_seg);
  }

  if ((loop_points) && (loop_points->size() >= 1)) {
    /* Setup the loops and find the amount of data we need to store in the
     * main block. */
    unsigned min_reqd_samples = 0;

    for (unsigned i = 0; i < loop_points->size(); i++) {
      StartSegment start_seg;
      EndSegment end_seg;
      const GOWaveLoop &loop = (*loop_points)[i];

      start_seg.start_offset = loop.m_StartPosition;
      /* According to
       * https://www.mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/Docs/RIFFNEW.pdf
       * dwEnd is the position of the last sample of the loop, but we need the
       * first position after this sample
       */
      end_seg.end_pos = loop.m_EndPosition + 1;
      if (end_seg.end_pos > min_reqd_samples)
        min_reqd_samples = end_seg.end_pos;
      end_seg.next_start_segment_index = i + 1;

      const unsigned loop_length = end_seg.end_pos - start_seg.start_offset;
      wxString loopError;

      if (fade_len > loop_length - 1)
        loopError = wxString::Format(
          _("The loop %u is ignored: it is too short for crossfade"), i + 1);
      else if (start_seg.start_offset < fade_len)
        loopError = wxString::Format(
          _("The loop %u is ignored: not enough samples for crossfade before "
            "it's start"),
          i + 1);

      if (loopError.IsEmpty()) {
        unsigned end_length;

        // calculate the fade segment size and offsets
        if (loop_length >= SHORT_LOOP_LENGTH) {
          end_seg.transition_offset
            = end_seg.end_pos - MAX_READAHEAD - fade_len;
          end_length = DEFAULT_END_SEG_LENGTH + fade_len;
        } else {
          end_seg.transition_offset = start_seg.start_offset;
          end_length = SHORT_LOOP_LENGTH + MAX_READAHEAD;
          if (
            end_length < MAX_READAHEAD
              + (SHORT_LOOP_LENGTH / loop_length) * SHORT_LOOP_LENGTH
              + fade_len)
            end_length = MAX_READAHEAD
              + (SHORT_LOOP_LENGTH / loop_length) * SHORT_LOOP_LENGTH
              + fade_len;
        }
        end_seg.end_size = end_length * m_BytesPerSample;

        // Allocate the fade segment
        end_seg.end_data
          = (unsigned char *)m_Pool.Alloc(end_seg.end_size, true);
        if (!end_seg.end_data)
          throw GOOutOfMemory();

        // make a virtual pointer for reading the end segment with the same
        // offset as the main data
        end_seg.end_ptr
          = end_seg.end_data - m_BytesPerSample * end_seg.transition_offset;

        const unsigned nBytesToCopy
          = (end_seg.end_pos - end_seg.transition_offset) * m_BytesPerSample;

        // Fill the fade seg with transition data, then with the loop start data
        memcpy(
          end_seg.end_data,
          ((const unsigned char *)pcm_data)
            + end_seg.transition_offset * m_BytesPerSample,
          nBytesToCopy);
        loop_memcpy(
          ((unsigned char *)end_seg.end_data) + nBytesToCopy,
          ((const unsigned char *)pcm_data)
            + loop.m_StartPosition * m_BytesPerSample,
          loop_length * m_BytesPerSample,
          end_seg.end_size - nBytesToCopy);
        if (fade_len > 0)
          // TODO: reduce the number of parameters of DoCrossfade that the call
          // would be easy readable without additional comments
          DoCrossfade(
            end_seg.end_data,
            MAX_READAHEAD,
            (const unsigned char *)pcm_data,
            start_seg.start_offset - fade_len,
            fade_len,
            loop_length,
            end_length);

        assert(end_length >= MAX_READAHEAD);

        m_StartSegments.push_back(start_seg);
        m_EndSegments.push_back(end_seg);
      } else
        wxLogWarning(GOCacheObject::generateMessage(
          pObjectFor, pLoaderFilename, loopError));
      if (!m_EndSegments.size())
        throw(wxString) _("No valid loops exist in the file");
    }

    /* There is no need to store any samples after the end of the last loop. */
    if (total_alloc_samples > min_reqd_samples)
      total_alloc_samples = min_reqd_samples;
  } else {
    /* Create a default end segment */
    EndSegment end_seg;

    end_seg.end_pos = pcm_data_nb_samples;
    end_seg.next_start_segment_index = -1;
    end_seg.end_size = m_BytesPerSample * DEFAULT_END_SEG_LENGTH;
    end_seg.end_data = (unsigned char *)m_Pool.Alloc(end_seg.end_size, true);

    if (!end_seg.end_data)
      throw GOOutOfMemory();

    end_seg.transition_offset = limitedDiff(end_seg.end_pos, MAX_READAHEAD);
    end_seg.end_ptr
      = end_seg.end_data - m_BytesPerSample * end_seg.transition_offset;

    const unsigned nBytesToCopy
      = (end_seg.end_pos - end_seg.transition_offset) * m_BytesPerSample;

    memcpy(
      end_seg.end_data,
      ((const unsigned char *)pcm_data)
        + m_BytesPerSample * end_seg.transition_offset,
      nBytesToCopy);
    memset(
      ((unsigned char *)end_seg.end_data) + nBytesToCopy,
      0,
      end_seg.end_size - nBytesToCopy);

    m_EndSegments.push_back(end_seg);
  }

  m_AllocSize = total_alloc_samples * m_BytesPerSample;
  m_data = (unsigned char *)m_Pool.Alloc(m_AllocSize, !compress);
  if (m_data == NULL)
    throw GOOutOfMemory();
  m_SampleRate = pcm_data_sample_rate;
  m_SampleCount = total_alloc_samples;
  m_SampleFracBits = m_BitsPerSample - 1;
  m_IsCompressed = false;
  m_CompressionCodec = GOSoundAudioSectionCompressionCodec::None;
  m_WaveTremulantStateFor = waveTremulantStateFor;

  /* Store the main data blob. */
  memcpy(m_data, pcm_data, m_AllocSize);

  GetMaxAmplitudeAndDerivative();

  if (compress) {
    if (useV3Compression)
      CompressV3(m_BitsPerSample > 16, uncompressedPrefixSamples);
    else
      Compress(m_BitsPerSample > 16);
  }
  RegisterStorageStats();
}

void GOSoundAudioSection::Compress(bool format16) {
  unsigned char *data = (unsigned char *)m_Pool.Alloc(m_AllocSize, false);
  if (data == NULL)
    throw GOOutOfMemory();

  unsigned output_len = 0;
  GOSoundCompressionCache state;

  state.Init();
  for (unsigned i = 0; i < m_SampleCount; i++) {
    state.m_position = i;
    state.m_ptr = (const unsigned char *)(intptr_t)output_len;
    for (unsigned j = 0; j < m_StartSegments.size(); j++) {
      if (m_StartSegments[j].start_offset == i) {
        m_StartSegments[j].cache = state;
      }
    }

    state.m_last[0] = state.m_prev[0];
    state.m_last[1] = state.m_prev[1];
    state.m_prev[0] = state.m_value[0];
    state.m_prev[1] = state.m_value[1];

    state.m_value[0] = GetSample(i, 0);
    if (m_channels > 1)
      state.m_value[1] = GetSample(i, 1);

    for (unsigned j = 0; j < m_channels; j++) {
      int val = state.m_value[j];
      int encode
        = val - (state.m_prev[j] + (state.m_prev[j] - state.m_last[j]) / 2);

      if (format16)
        GOSoundCompressionCache::writeCompressed16(data, output_len, encode);
      else
        GOSoundCompressionCache::writeCompressed8(data, output_len, encode);

      /* Early abort if the compressed data will be larger than the
       * uncompressed data. */
      if (output_len + 10 >= m_AllocSize) {
        m_Pool.Free(data);
        m_data = (unsigned char *)m_Pool.MoveToPool(m_data, m_AllocSize);
        if (m_data == NULL)
          throw GOOutOfMemory();
        return;
      }
    }
  }

  m_Pool.Free(m_data);
  m_data = data;
  m_AllocSize = output_len;
  m_IsCompressed = true;
  m_CompressionCodec = GOSoundAudioSectionCompressionCodec::Legacy;
  m_StartSegmentsV3.clear();
  m_SubblockSeedsV3.clear();

  m_data = (unsigned char *)m_Pool.MoveToPool(m_data, m_AllocSize);
  if (m_data == NULL)
    throw GOOutOfMemory();
}

void GOSoundAudioSection::CompressV3(
  bool format16, unsigned uncompressedPrefixSamples) {
  (void)format16;
  GOSoundCompressionCacheV3 codec;
  const unsigned worstCase = GOSoundCompressionCacheV3::WorstCaseSize(
    m_SampleCount, m_channels);
  unsigned char *data = (unsigned char *)m_Pool.Alloc(worstCase, false);
  if (data == NULL)
    throw GOOutOfMemory();

  /* Snapshot positions: the start segment offsets (unique, sorted -
   * Encode() requires strictly ascending positions and captures exactly
   * one snapshot per entry). Grid-floor positions are NOT seeded anymore:
   * since the per-voice render-block grid is anchored at the segment
   * start (GOSoundStream::m_BlockAnchor, 2026-07-09 bin redesign), a
   * block begins exactly at the loop start after every wrap - the
   * segment's own snapshot IS the block's start state, for any bin
   * length. (A global position-0 modulo grid needed floor seeds because
   * a loop start rarely sat on it.) */
  std::vector<unsigned> snapshotPositions;

  /* Uncompressed prefix request (see m_UncompressedPrefixData): clip to
   * the section; a prefix covering the whole (short) section is pointless
   * - such starts are cheap anyway - so disable it there. The prefix-end
   * position becomes one more seed snapshot: the exact state where a
   * voice's block chain leaves the raw prefix and enters the compressed
   * stream. */
  const unsigned prefixSamples
    = (uncompressedPrefixSamples > 0
       && uncompressedPrefixSamples < m_SampleCount)
    ? uncompressedPrefixSamples
    : 0;

  snapshotPositions.reserve(m_StartSegments.size() + 1);
  for (const StartSegment &seg : m_StartSegments)
    snapshotPositions.push_back(seg.start_offset);
  if (prefixSamples)
    snapshotPositions.push_back(prefixSamples);
  std::sort(snapshotPositions.begin(), snapshotPositions.end());
  snapshotPositions.erase(
    std::unique(snapshotPositions.begin(), snapshotPositions.end()),
    snapshotPositions.end());

  std::vector<GOSoundCompressionCacheV3::Snapshot> snapshots(
    snapshotPositions.size());

  const auto sampleFn
    = [&](unsigned absIdx, unsigned ch) { return GetSampleData(m_data, absIdx, ch); };
  /* Per-section predictor order selection (cache version 5): the fixed
   * integer predictors (1..4) are peak/width-optimal for this format, so
   * pure SELECTION of the smallest-encoding order wins (per-file study).
   * The order is stored self-describingly in each block header, so
   * nothing else needs to carry it. Sizing passes need no snapshots
   * (snapshots do not affect stream length); the winner is re-encoded
   * with the real snapshot set. Encoding is offline (cache generation),
   * so the 4 extra sizing passes are free. */
  /* Fixed modes: the mode IS the order (enum value 1..4). Variable (0):
   * per-section search - encode each order once WITHOUT snapshots (sizing
   * only; snapshots do not change the stream length) and keep the
   * smallest, ties to the lower order (cheaper decode). Encoding is
   * offline (cache generation), so the extra sizing passes are free. No
   * order byte in the block stream - the order is section metadata
   * (m_PredictorOrder, v11 envelope). */
  const GOV3PredictorMode mode = GetPredictorMode();
  unsigned bestOrder = (unsigned)mode;
  if (mode == GOV3PredictorMode::Variable) {
    unsigned bestLen = ~0u;

    bestOrder = 3;
    for (unsigned order = 1; order <= 4; order++) {
      const unsigned len = codec.Encode(
        data, m_SampleCount, m_channels, order, nullptr, 0, sampleFn, nullptr);
      if (len < bestLen) {
        bestLen = len;
        bestOrder = order;
      }
    }
  }
  /* 4th-tap capture buffer, parallel to `snapshots` - only order 4 needs
   * it (cost isolation: it never enters the 40-byte Snapshot struct).
   * std::array<int, N> is layout-compatible with int[N] (standard layout,
   * data() == the array), so the cast below is safe. */
  std::vector<std::array<int, MAX_OUTPUT_CHANNELS>> snapHist3(
    bestOrder >= 4 ? snapshotPositions.size() : 0);
  unsigned output_len = codec.Encode(
    data,
    m_SampleCount,
    m_channels,
    bestOrder,
    snapshotPositions.data(),
    (unsigned)snapshotPositions.size(),
    sampleFn,
    snapshots.data(),
    bestOrder >= 4
      ? reinterpret_cast<int(*)[MAX_OUTPUT_CHANNELS]>(snapHist3.data())
      : nullptr);

  if (output_len >= m_AllocSize) {
    m_Pool.Free(data);
    m_StartSegmentsV3.clear();
    m_SubblockSeedsV3.clear();
    m_SegHist3V3.clear();
    m_SeedHist3V3.clear();
    m_IsCompressed = false;
    m_CompressionCodec = GOSoundAudioSectionCompressionCodec::None;
    return;
  }
  m_PredictorOrder = (uint8_t)bestOrder;

  // All captured positions become seeds (sorted, unique)...
  m_SubblockSeedsV3.resize(snapshotPositions.size());
  m_SeedHist3V3.assign(snapHist3.begin(), snapHist3.end());
  for (unsigned i = 0; i < snapshotPositions.size(); i++) {
    m_SubblockSeedsV3[i].start_offset = snapshotPositions[i];
    m_SubblockSeedsV3[i].cache = snapshots[i];
  }
  // ...and each start segment picks its own snapshot out of them (index
  // order must match m_StartSegments exactly - GoToStartSegment addresses
  // by segment index). The hist3 arrays stay index-parallel (empty unless
  // order 4 - snapHist3 was sized 0 then).
  m_StartSegmentsV3.resize(m_StartSegments.size());
  m_SegHist3V3.resize(bestOrder >= 4 ? m_StartSegments.size() : 0);
  for (unsigned i = 0; i < m_StartSegments.size(); i++) {
    const unsigned off = m_StartSegments[i].start_offset;
    const auto it = std::lower_bound(
      snapshotPositions.begin(), snapshotPositions.end(), off);

    assert(it != snapshotPositions.end() && *it == off);
    m_StartSegmentsV3[i].start_offset = off;
    const size_t snapIdx = (size_t)(it - snapshotPositions.begin());
    m_StartSegmentsV3[i].cache = snapshots[snapIdx];
    if (bestOrder >= 4)
      m_SegHist3V3[i] = snapHist3[snapIdx];
  }

  /* Copy the raw prefix (+ read-ahead guard, clipped to the section) out
   * of the still-uncompressed m_data BEFORE it is replaced. */
  if (prefixSamples) {
    const unsigned storedSamples
      = std::min(prefixSamples + getMaxReadAhead(), m_SampleCount);
    const size_t prefixBytes = (size_t)storedSamples * m_BytesPerSample;

    m_UncompressedPrefixData = (unsigned char *)m_Pool.Alloc(prefixBytes, true);
    if (!m_UncompressedPrefixData) {
      m_Pool.Free(data);
      throw GOOutOfMemory();
    }
    memcpy(m_UncompressedPrefixData, m_data, prefixBytes);
    m_UncompressedPrefixSamples = prefixSamples;
    m_UncompressedPrefixStoredSamples = storedSamples;
  }

  m_Pool.Free(m_data);
  m_data = data;
  m_AllocSize = output_len;
  m_IsCompressed = true;
  m_CompressionCodec = GOSoundAudioSectionCompressionCodec::V3;

  m_data = (unsigned char *)m_Pool.MoveToPool(m_data, m_AllocSize);
  if (m_data == NULL)
    throw GOOutOfMemory();
}

const GOSoundAudioSection::StartSegmentV3 *
GOSoundAudioSection::FindV3SnapshotAtOrBefore(unsigned position) const {
  // m_SubblockSeedsV3 is sorted by start_offset and is a superset of the
  // start segments' snapshot positions (see CompressV3).
  const auto it = std::upper_bound(
    m_SubblockSeedsV3.begin(),
    m_SubblockSeedsV3.end(),
    position,
    [](unsigned pos, const StartSegmentV3 &seed) {
      return pos < seed.start_offset;
    });

  return it == m_SubblockSeedsV3.begin() ? nullptr : &*(it - 1);
}

void GOSoundAudioSection::AssignAttackLutPointers(
  const std::vector<const GOSoundAudioSection *> &attacks) {
  if (m_ReleaseAligner)
    m_ReleaseAligner->AssignAttackPointers(attacks);
}

void GOSoundAudioSection::SetupStreamAlignment(
  const std::vector<const GOSoundAudioSection *> &joinables,
  unsigned start_index,
  float sample_freq_hz,
  unsigned harmonic_number,
  unsigned min_key_press_ms,
  unsigned max_key_press_ms,
  bool skipCorrLut
#if __has_include("GOLogReleaseAlignEnable.h")
  ,
  const char *label
#endif
) {
  if (m_ReleaseAligner) {
    delete m_ReleaseAligner;
    m_ReleaseAligner = NULL;
  }
  if (!joinables.size())
    return;

  int max_amplitude = m_MaxAbsAmplitude;
  int max_derivative = m_MaxAbsDerivative;
  for (unsigned i = 0; i < joinables.size(); i++) {
    if (joinables[i]->m_MaxAbsAmplitude > max_amplitude)
      max_amplitude = joinables[i]->m_MaxAbsAmplitude;
    if (joinables[i]->m_MaxAbsDerivative > max_derivative)
      max_derivative = joinables[i]->m_MaxAbsDerivative;
  }
  m_ReleaseStartSegment = start_index;
  if (m_ReleaseStartSegment >= m_StartSegments.size())
    m_ReleaseStartSegment = 0;

  if ((max_derivative != 0) && (max_amplitude != 0)) {
    m_ReleaseAligner = new GOSoundReleaseAlignTable();
    m_ReleaseAligner->ComputeTable(
      *this,
      max_amplitude,
      max_derivative,
      m_SampleRate,
      m_StartSegments[m_ReleaseStartSegment].start_offset);

    if (
      start_index == 0 && !joinables.empty() && m_ReleaseCrossfadeLength > 0) {
      if (!skipCorrLut) {
        unsigned crossfade_samples
          = m_ReleaseCrossfadeLength * m_SampleRate / 1000;
        for (const GOSoundAudioSection *pAttack : joinables)
          m_ReleaseAligner->ComputeCorrelationLut(
            *pAttack,
            *this,
            crossfade_samples,
            m_SampleRate,
            sample_freq_hz,
            harmonic_number,
            min_key_press_ms,
            max_key_press_ms
#if __has_include("GOLogReleaseAlignEnable.h")
            ,
            false,
            false,
            label
#endif
          );
      } else {
        // LUT cache will be applied after loading — just set the period so
        // that OverrideCorrLutsFromCache (which guards on period != 0) works.
        m_ReleaseAligner->InitPeriodFromFormula(m_SampleRate, sample_freq_hz);
      }
    }
  }
}

GOSampleStatistic GOSoundAudioSection::GetStatistic() {
  GOSampleStatistic stat;
  size_t size = 0;

  for (unsigned i = 0; i < m_EndSegments.size(); i++)
    size += m_EndSegments[i].end_size;
  const size_t prefixBytes
    = (size_t)m_UncompressedPrefixStoredSamples * m_BytesPerSample;

  stat.SetEndSegmentSize(size);
  stat.SetMemorySize(size + m_AllocSize + prefixBytes);
  // What the same data would occupy uncompressed: SampleCount frames at
  // m_BytesPerSample (all channels) plus the end segments, which are always
  // stored uncompressed anyway - so ratio == 1.0 for uncompressed sections.
  stat.SetUncompressedSize(
    size + (size_t)m_SampleCount * m_BytesPerSample);
  stat.SetBitsPerSample(m_BitsPerSample, m_SampleCount, m_MaxAmplitude);

  return stat;
}
