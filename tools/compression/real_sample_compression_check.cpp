// real_sample_compression_check.cpp
//
// Measures GO's actual compression ratio (GOSoundCompressionCache, the real
// production encoder/decoder) against real recorded pipe organ WAV files,
// to check a suspicion that the earlier stage-timing harness's synthetic
// signal (decaying harmonics + noise) understated the real ratio because
// its injected noise term defeats the delta predictor on every sample -
// real sustained pipe tones should be far more predictable.
//
// Reads plain PCM WAV (24-bit or 16-bit, mono or stereo), feeds every frame
// through the exact same encode loop as GOSoundAudioSection::Compress() and
// the exact same GOSoundCompressionCache::DecompressionStep() decode used in
// production, and reports the real compressed/uncompressed ratio.
//
// Result: real 24-bit organ material (clean pipe tones AND motor/room
// noise alike) compresses at EXACTLY 1.500x, always - not content
// dependent. Reason: 24-bit is packed at exactly 3 bytes/sample
// (wave_bytes_per_sample), while writeCompressed16's cheapest code is
// 2 bytes - a hard, format-imposed ceiling. Resolved the "3-4x vs 1.5x"
// dispute: 3-4x needs real entropy coding (see entropy_ratio_check.py),
// not GO's byte-granularity scheme.
//
// Build:
//   g++ -O2 -std=gnu++17 -I <repo>/src/grandorgue \
//     real_sample_compression_check.cpp -o real_sample_compression_check

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "sound/playing/GOSoundCompressionCache.h"

namespace {

struct WavData {
  unsigned channels;
  unsigned sampleRate;
  unsigned bitsPerSample;
  std::vector<int32_t> samples; // interleaved, sign-extended to int32
};

bool loadWav(const std::string &path, WavData &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;

  char riff[4];
  f.read(riff, 4);
  if (memcmp(riff, "RIFF", 4) != 0)
    return false;
  f.seekg(4, std::ios::cur); // chunk size
  char wave[4];
  f.read(wave, 4);
  if (memcmp(wave, "WAVE", 4) != 0)
    return false;

  unsigned channels = 0, sampleRate = 0, bitsPerSample = 0;
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
      sampleRate = sr;
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
  out.sampleRate = sampleRate;
  out.bitsPerSample = bitsPerSample;

  const unsigned bytesPerSample = bitsPerSample / 8;
  const unsigned nFrames = (unsigned)data.size() / (bytesPerSample * channels);
  out.samples.resize((size_t)nFrames * channels);

  const unsigned char *p = data.data();
  for (unsigned i = 0; i < nFrames * channels; i++) {
    int32_t v = 0;
    if (bytesPerSample == 3) {
      v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
      if (v & 0x800000)
        v |= 0xFF000000;
    } else if (bytesPerSample == 2) {
      v = (int16_t)(p[0] | (p[1] << 8));
    }
    out.samples[i] = v;
    p += bytesPerSample;
  }
  return true;
}

// Byte-exact replica of GOSoundAudioSection::Compress()'s predictor+encode
// loop, using the real production writeCompressed8/16.
size_t encodedSize(const WavData &w) {
  const bool format16 = w.bitsPerSample > 16;
  const unsigned nFrames = (unsigned)w.samples.size() / w.channels;
  std::vector<unsigned char> out(w.samples.size() * 4 + 16);
  unsigned len = 0;
  GOSoundCompressionCache state;

  state.Init();
  for (unsigned i = 0; i < nFrames; i++) {
    state.m_last[0] = state.m_prev[0];
    state.m_prev[0] = state.m_value[0];
    state.m_value[0] = w.samples[i * w.channels];
    if (w.channels > 1) {
      state.m_last[1] = state.m_prev[1];
      state.m_prev[1] = state.m_value[1];
      state.m_value[1] = w.samples[i * w.channels + 1];
    }
    for (unsigned ch = 0; ch < w.channels; ch++) {
      const int encode
        = state.m_value[ch] - (state.m_prev[ch] + (state.m_prev[ch] - state.m_last[ch]) / 2);
      if (format16)
        GOSoundCompressionCache::writeCompressed16(out.data(), len, encode);
      else
        GOSoundCompressionCache::writeCompressed8(out.data(), len, encode);
    }
  }
  return len;
}

// Parity self-check: decode back and compare against the source samples,
// using the real production DecompressionStep().
bool verifyRoundTrip(
  const WavData &w, const std::vector<unsigned char> &compressed) {
  const bool format16 = w.bitsPerSample > 16;
  const unsigned nFrames = (unsigned)w.samples.size() / w.channels;
  GOSoundCompressionCache c;
  c.Init();
  c.m_ptr = compressed.data();
  for (unsigned i = 0; i < nFrames; i++) {
    c.DecompressionStep(w.channels, format16);
    if (c.m_value[0] != w.samples[i * w.channels])
      return false;
    if (w.channels > 1 && c.m_value[1] != w.samples[i * w.channels + 1])
      return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  double totalRaw = 0, totalCompressed = 0;

  for (int i = 1; i < argc; i++) {
    WavData w;
    if (!loadWav(argv[i], w)) {
      printf("SKIP (unreadable): %s\n", argv[i]);
      continue;
    }

    const size_t rawBytes = w.samples.size() * (w.bitsPerSample > 16 ? 3 : 2);
    // Re-encode via the *actual* writeCompressed call, capturing bytes into
    // a real buffer this time (encodedSize() above only measures length;
    // redo with storage for the round-trip check).
    const bool format16 = w.bitsPerSample > 16;
    const unsigned nFrames = (unsigned)w.samples.size() / w.channels;
    std::vector<unsigned char> out(w.samples.size() * 4 + 16);
    unsigned len = 0;
    GOSoundCompressionCache state;
    state.Init();
    for (unsigned f = 0; f < nFrames; f++) {
      state.m_last[0] = state.m_prev[0];
      state.m_prev[0] = state.m_value[0];
      state.m_value[0] = w.samples[f * w.channels];
      if (w.channels > 1) {
        state.m_last[1] = state.m_prev[1];
        state.m_prev[1] = state.m_value[1];
        state.m_value[1] = w.samples[f * w.channels + 1];
      }
      for (unsigned ch = 0; ch < w.channels; ch++) {
        const int encode = state.m_value[ch]
          - (state.m_prev[ch] + (state.m_prev[ch] - state.m_last[ch]) / 2);
        if (format16)
          GOSoundCompressionCache::writeCompressed16(out.data(), len, encode);
        else
          GOSoundCompressionCache::writeCompressed8(out.data(), len, encode);
      }
    }
    out.resize(len);

    const bool ok = verifyRoundTrip(w, out);
    const double ratio = (double)rawBytes / (double)len;

    printf(
      "%-70s %2u ch %2u bit  raw %8zu B  compressed %8u B  ratio %.3fx  "
      "%s\n",
      argv[i],
      w.channels,
      w.bitsPerSample,
      rawBytes,
      len,
      ratio,
      ok ? "roundtrip OK" : "ROUNDTRIP MISMATCH!");

    totalRaw += rawBytes;
    totalCompressed += len;
  }

  if (totalCompressed > 0)
    printf(
      "\nTOTAL: raw %.0f B  compressed %.0f B  ratio %.3fx\n",
      totalRaw,
      totalCompressed,
      totalRaw / totalCompressed);

  return 0;
}
