/*
 * Prototype-only cache-format contract check for the V3 compressed sample
 * work. This does not read or write real GrandOrgue cache files; it models
 * the GOSoundAudioSection cache prefix closely enough to lock down the
 * migration rules before production serialization is touched.
 *
 * The important rule is deliberately boring:
 *   - legacy-format output must stay byte-for-byte identical when V3 is not
 *     selected;
 *   - a new reader must recognize old bytes as the legacy format;
 *   - V3 bytes must carry an explicit section-local magic/version/codec field
 *     instead of relying only on struct-size hash invalidation.
 *
 * The current production loader policy is stricter than the prefix parser:
 * it recognizes the V3 codec value but rejects it until the V3 payload
 * metadata and stream path are wired.
 */

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "sound/playing/GOSoundAudioSectionCacheFormat.h"

namespace {

using CompressionCodec = GOSoundAudioSectionCompressionCodec;

struct LegacyPrefix {
  uint32_t allocSize = 0;
  uint32_t sampleCount = 0;
  uint32_t sampleRate = 0;
  uint8_t bitsPerSample = 0;
  uint8_t bytesPerSample = 0;
  uint8_t waveTremulantStateFor = 0;
  uint8_t isCompressed = 0;
  uint8_t channels = 0;
  uint32_t sampleFracBits = 0;
  uint32_t maxAmplitude = 0;
  uint32_t releaseStartSegment = 0;
  uint32_t releaseCrossfadeLength = 0;
};

struct NewPrefix {
  uint32_t magic = GO_SOUND_AUDIO_SECTION_CACHE_MAGIC;
  uint16_t version = GO_SOUND_AUDIO_SECTION_CACHE_VERSION;
  uint8_t compressionFormat = (uint8_t)CompressionCodec::V3;
  uint8_t flags = 0;
  uint32_t allocSize = 0;
  uint32_t sampleCount = 0;
  uint32_t sampleRate = 0;
  uint8_t bitsPerSample = 0;
  uint8_t bytesPerSample = 0;
  uint8_t waveTremulantStateFor = 0;
  uint8_t channels = 0;
  uint32_t sampleFracBits = 0;
  uint32_t maxAmplitude = 0;
  uint32_t releaseStartSegment = 0;
  uint32_t releaseCrossfadeLength = 0;
};

struct ParsedPrefix {
  bool hasNewHeader = false;
  CompressionCodec compressionFormat = CompressionCodec::None;
  uint32_t allocSize = 0;
  uint32_t sampleCount = 0;
  uint32_t sampleRate = 0;
  uint8_t bitsPerSample = 0;
  uint8_t bytesPerSample = 0;
  uint8_t waveTremulantStateFor = 0;
  uint8_t channels = 0;
  uint32_t sampleFracBits = 0;
  uint32_t maxAmplitude = 0;
  uint32_t releaseStartSegment = 0;
  uint32_t releaseCrossfadeLength = 0;
};

enum class ReaderMode {
  PrefixOnly,
  CurrentProductionLoader,
};

template <typename T> void appendPod(std::vector<uint8_t> &out, const T &v) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}

class Reader {
private:
  const std::vector<uint8_t> &m_data;
  size_t m_pos = 0;

public:
  explicit Reader(const std::vector<uint8_t> &data) : m_data(data) {}

  template <typename T> T read() {
    if (m_pos + sizeof(T) > m_data.size())
      throw std::runtime_error("short read");
    T v;
    std::memcpy(&v, m_data.data() + m_pos, sizeof(T));
    m_pos += sizeof(T);
    return v;
  }

  size_t pos() const { return m_pos; }
};

std::vector<uint8_t> writeLegacyPrefix(const LegacyPrefix &p) {
  std::vector<uint8_t> out;
  appendPod(out, p.allocSize);
  appendPod(out, p.sampleCount);
  appendPod(out, p.sampleRate);
  appendPod(out, p.bitsPerSample);
  appendPod(out, p.bytesPerSample);
  appendPod(out, p.waveTremulantStateFor);
  appendPod(out, p.isCompressed);
  appendPod(out, p.channels);
  appendPod(out, p.sampleFracBits);
  appendPod(out, p.maxAmplitude);
  appendPod(out, p.releaseStartSegment);
  appendPod(out, p.releaseCrossfadeLength);
  return out;
}

std::vector<uint8_t> writeNewPrefix(const NewPrefix &p) {
  std::vector<uint8_t> out;
  appendPod(out, p.magic);
  appendPod(out, p.version);
  appendPod(out, p.compressionFormat);
  appendPod(out, p.flags);
  appendPod(out, p.allocSize);
  appendPod(out, p.sampleCount);
  appendPod(out, p.sampleRate);
  appendPod(out, p.bitsPerSample);
  appendPod(out, p.bytesPerSample);
  appendPod(out, p.waveTremulantStateFor);
  appendPod(out, p.channels);
  appendPod(out, p.sampleFracBits);
  appendPod(out, p.maxAmplitude);
  appendPod(out, p.releaseStartSegment);
  appendPod(out, p.releaseCrossfadeLength);
  return out;
}

CompressionCodec parseCompressionFormat(uint8_t v) {
  if (!IsValidGOSoundAudioSectionCompressionCodec(v))
    throw std::runtime_error("invalid compression format");
  return (CompressionCodec)v;
}

ParsedPrefix readPrefixNewCompatible(
  const std::vector<uint8_t> &bytes,
  ReaderMode mode = ReaderMode::PrefixOnly) {
  Reader r(bytes);
  ParsedPrefix out;
  const uint32_t first = r.read<uint32_t>();

  if (first == GO_SOUND_AUDIO_SECTION_CACHE_MAGIC) {
    out.hasNewHeader = true;
    const uint16_t version = r.read<uint16_t>();
    if (version != GO_SOUND_AUDIO_SECTION_CACHE_VERSION)
      throw std::runtime_error("unsupported audio-section cache version");
    out.compressionFormat = parseCompressionFormat(r.read<uint8_t>());
    if (
      mode == ReaderMode::CurrentProductionLoader
      && out.compressionFormat == CompressionCodec::V3)
      throw std::runtime_error("V3 payload not supported by production loader");
    const uint8_t flags = r.read<uint8_t>();
    if (flags != 0)
      throw std::runtime_error("unsupported audio-section cache flags");
    out.allocSize = r.read<uint32_t>();
  } else {
    out.hasNewHeader = false;
    out.allocSize = first;
    out.sampleCount = r.read<uint32_t>();
    out.sampleRate = r.read<uint32_t>();
    out.bitsPerSample = r.read<uint8_t>();
    out.bytesPerSample = r.read<uint8_t>();
    out.waveTremulantStateFor = r.read<uint8_t>();
    const uint8_t isCompressed = r.read<uint8_t>();
    out.compressionFormat = isCompressed ? CompressionCodec::Legacy
                                         : CompressionCodec::None;
    out.channels = r.read<uint8_t>();
    out.sampleFracBits = r.read<uint32_t>();
    out.maxAmplitude = r.read<uint32_t>();
    out.releaseStartSegment = r.read<uint32_t>();
    out.releaseCrossfadeLength = r.read<uint32_t>();
    return out;
  }

  out.sampleCount = r.read<uint32_t>();
  out.sampleRate = r.read<uint32_t>();
  out.bitsPerSample = r.read<uint8_t>();
  out.bytesPerSample = r.read<uint8_t>();
  out.waveTremulantStateFor = r.read<uint8_t>();
  out.channels = r.read<uint8_t>();
  out.sampleFracBits = r.read<uint32_t>();
  out.maxAmplitude = r.read<uint32_t>();
  out.releaseStartSegment = r.read<uint32_t>();
  out.releaseCrossfadeLength = r.read<uint32_t>();
  return out;
}

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void requireSameCommonFields(const ParsedPrefix &parsed, const LegacyPrefix &src) {
  require(parsed.allocSize == src.allocSize, "allocSize mismatch");
  require(parsed.sampleCount == src.sampleCount, "sampleCount mismatch");
  require(parsed.sampleRate == src.sampleRate, "sampleRate mismatch");
  require(parsed.bitsPerSample == src.bitsPerSample, "bitsPerSample mismatch");
  require(parsed.bytesPerSample == src.bytesPerSample, "bytesPerSample mismatch");
  require(
    parsed.waveTremulantStateFor == src.waveTremulantStateFor,
    "waveTremulantStateFor mismatch");
  require(parsed.channels == src.channels, "channels mismatch");
  require(parsed.sampleFracBits == src.sampleFracBits, "sampleFracBits mismatch");
  require(parsed.maxAmplitude == src.maxAmplitude, "maxAmplitude mismatch");
  require(
    parsed.releaseStartSegment == src.releaseStartSegment,
    "releaseStartSegment mismatch");
  require(
    parsed.releaseCrossfadeLength == src.releaseCrossfadeLength,
    "releaseCrossfadeLength mismatch");
}

void requireSameCommonFields(const ParsedPrefix &parsed, const NewPrefix &src) {
  require(parsed.allocSize == src.allocSize, "allocSize mismatch");
  require(parsed.sampleCount == src.sampleCount, "sampleCount mismatch");
  require(parsed.sampleRate == src.sampleRate, "sampleRate mismatch");
  require(parsed.bitsPerSample == src.bitsPerSample, "bitsPerSample mismatch");
  require(parsed.bytesPerSample == src.bytesPerSample, "bytesPerSample mismatch");
  require(
    parsed.waveTremulantStateFor == src.waveTremulantStateFor,
    "waveTremulantStateFor mismatch");
  require(parsed.channels == src.channels, "channels mismatch");
  require(parsed.sampleFracBits == src.sampleFracBits, "sampleFracBits mismatch");
  require(parsed.maxAmplitude == src.maxAmplitude, "maxAmplitude mismatch");
  require(
    parsed.releaseStartSegment == src.releaseStartSegment,
    "releaseStartSegment mismatch");
  require(
    parsed.releaseCrossfadeLength == src.releaseCrossfadeLength,
    "releaseCrossfadeLength mismatch");
}

bool throwsOnRead(
  const std::vector<uint8_t> &bytes,
  std::string *what,
  ReaderMode mode = ReaderMode::PrefixOnly) {
  try {
    (void)readPrefixNewCompatible(bytes, mode);
  } catch (const std::exception &e) {
    if (what)
      *what = e.what();
    return true;
  }
  return false;
}

} // namespace

int main() {
  try {
    LegacyPrefix legacyCompressed;
    legacyCompressed.allocSize = 123456;
    legacyCompressed.sampleCount = 44100;
    legacyCompressed.sampleRate = 48000;
    legacyCompressed.bitsPerSample = 24;
    legacyCompressed.bytesPerSample = 3;
    legacyCompressed.waveTremulantStateFor = 2;
    legacyCompressed.isCompressed = 1;
    legacyCompressed.channels = 2;
    legacyCompressed.sampleFracBits = 23;
    legacyCompressed.maxAmplitude = 0x5fffff;
    legacyCompressed.releaseStartSegment = 7;
    legacyCompressed.releaseCrossfadeLength = 42;

    LegacyPrefix legacyUncompressed = legacyCompressed;
    legacyUncompressed.isCompressed = 0;

    const std::vector<uint8_t> legacyCompressedBytes
      = writeLegacyPrefix(legacyCompressed);
    const std::vector<uint8_t> legacyCompressedBytesAgain
      = writeLegacyPrefix(legacyCompressed);
    require(
      legacyCompressedBytes == legacyCompressedBytesAgain,
      "legacy writer is not byte-stable");

    ParsedPrefix parsedLegacyCompressed
      = readPrefixNewCompatible(legacyCompressedBytes);
    require(!parsedLegacyCompressed.hasNewHeader, "legacy header misdetected");
    require(
      parsedLegacyCompressed.compressionFormat == CompressionCodec::Legacy,
      "legacy compressed format misclassified");
    requireSameCommonFields(parsedLegacyCompressed, legacyCompressed);

    ParsedPrefix parsedLegacyUncompressed
      = readPrefixNewCompatible(writeLegacyPrefix(legacyUncompressed));
    require(!parsedLegacyUncompressed.hasNewHeader, "legacy header misdetected");
    require(
      parsedLegacyUncompressed.compressionFormat
        == CompressionCodec::None,
      "legacy uncompressed format misclassified");
    requireSameCommonFields(parsedLegacyUncompressed, legacyUncompressed);

    NewPrefix v3;
    v3.allocSize = 654321;
    v3.sampleCount = 88200;
    v3.sampleRate = 44100;
    v3.bitsPerSample = 24;
    v3.bytesPerSample = 3;
    v3.waveTremulantStateFor = 1;
    v3.channels = 2;
    v3.sampleFracBits = 23;
    v3.maxAmplitude = 0x6fffff;
    v3.releaseStartSegment = 3;
    v3.releaseCrossfadeLength = 99;
    ParsedPrefix parsedV3 = readPrefixNewCompatible(writeNewPrefix(v3));
    require(parsedV3.hasNewHeader, "V3 header not detected");
    require(
      parsedV3.compressionFormat == CompressionCodec::V3,
      "V3 format misclassified");
    requireSameCommonFields(parsedV3, v3);

    std::string error;

    NewPrefix newLegacy = v3;
    newLegacy.compressionFormat = (uint8_t)CompressionCodec::Legacy;
    ParsedPrefix parsedNewLegacy = readPrefixNewCompatible(
      writeNewPrefix(newLegacy), ReaderMode::CurrentProductionLoader);
    require(
      parsedNewLegacy.compressionFormat == CompressionCodec::Legacy,
      "new-header legacy format rejected");
    requireSameCommonFields(parsedNewLegacy, newLegacy);

    NewPrefix newUncompressed = v3;
    newUncompressed.compressionFormat = (uint8_t)CompressionCodec::None;
    ParsedPrefix parsedNewUncompressed = readPrefixNewCompatible(
      writeNewPrefix(newUncompressed), ReaderMode::CurrentProductionLoader);
    require(
      parsedNewUncompressed.compressionFormat == CompressionCodec::None,
      "new-header uncompressed format rejected");
    requireSameCommonFields(parsedNewUncompressed, newUncompressed);

    require(
      throwsOnRead(
        writeNewPrefix(v3), &error, ReaderMode::CurrentProductionLoader),
      "current production loader accepted V3 payload");

    NewPrefix futureVersion = v3;
    futureVersion.version = GO_SOUND_AUDIO_SECTION_CACHE_VERSION + 1;
    require(
      throwsOnRead(writeNewPrefix(futureVersion), &error),
      "future version was accepted");

    NewPrefix invalidFormat = v3;
    invalidFormat.compressionFormat = 99;
    require(
      throwsOnRead(writeNewPrefix(invalidFormat), &error),
      "invalid compression format was accepted");

    std::vector<uint8_t> truncated = writeNewPrefix(v3);
    truncated.resize(truncated.size() - 3);
    require(throwsOnRead(truncated, &error), "truncated prefix was accepted");

    std::cout << "legacy compressed: OK bytes=" << legacyCompressedBytes.size()
              << "\n";
    std::cout << "legacy uncompressed: OK bytes="
              << writeLegacyPrefix(legacyUncompressed).size() << "\n";
    std::cout << "V3 explicit header: OK bytes=" << writeNewPrefix(v3).size()
              << " magic=0x" << std::hex << GO_SOUND_AUDIO_SECTION_CACHE_MAGIC
              << std::dec
              << " version=" << GO_SOUND_AUDIO_SECTION_CACHE_VERSION << "\n";
    std::cout << "production loader policy: OK accepts new None/Legacy, "
                 "rejects V3 until payload support exists\n";
    std::cout << "negative cases: OK future-version invalid-format truncated\n";
    std::cout << "cache format contract checks passed\n";
  } catch (const std::exception &e) {
    std::cerr << "cache format contract check failed: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
