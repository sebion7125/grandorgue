// planar_blockwidth_check.cpp
//
// Builds and measures the candidate we converged on today: planar channel
// streams (L and R fully separated, no cross-channel dependency) + a fixed
// bit width chosen ONCE PER BLOCK (not globally, not per-sample) based on
// that block's actual residual range - chatty's "block predictor + fixed
// bitpacking" idea, refined from my flatter "always 2 bytes" proposal after
// the Python prototype showed it reaches ~2.2x vs the flat scheme's 1.5x.
//
// Measures, on real pipe-organ WAV files, against GO's actual production
// decoder (GOSoundCompressionCache::DecompressionStep, unmodified):
//   - compression ratio
//   - decode time (ns per stereo frame)
//   - round-trip correctness (bit-exact vs the source PCM)
//
// Result: the WINNING design turned out to be INTERLEAVED (not planar)
// + fixed per-block width: ratio 1.630x, decode 1.71-1.76 ns/frame -
// 1.5x FASTER than production's 2.37-2.66 ns/frame. Planar separation
// wasn't actually needed once the format is fixed-width: the cross-
// channel dependency it was meant to remove only exists in GO's
// variable-length scheme. Also tested a Mid/Side stereo transform on
// top (msForward/msInverse) - rejected: ratio barely changed (1.628x)
// and decode got slower (2.12-2.13 ns/frame, all in the transform
// itself), because GO's wide-mic'd organ recordings don't correlate
// enough between L/R for M/S to pay off.
//
// Build:
//   g++ -O3 -ffast-math -funroll-loops -std=gnu++17 \
//     -I <repo>/src/grandorgue planar_blockwidth_check.cpp \
//     -o planar_blockwidth_check
//
// Usage: planar_blockwidth_check file1.wav file2.wav ...

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "sound/playing/GOSoundCompressionCache.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr unsigned REPS = 7;
constexpr unsigned BLOCK_SAMPLES = 1024;

double bestSeconds(unsigned reps, auto &&fn) {
  double best = 1e30;
  for (unsigned r = 0; r < reps; r++) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
  }
  return best;
}

// ---------------------------------------------------------------------------
struct WavData {
  unsigned channels;
  unsigned bitsPerSample;
  std::vector<int32_t> samples; // interleaved, sign-extended
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

// ---------------------------------------------------------------------------
// Bit packing (LSB-first, 64-bit accumulator - the standard fast pattern).
// Byte-aligned fixed width (1/2/3/4 bytes), not arbitrary bit width: the
// bit-packed version above measured 3.4x SLOWER than production despite
// having no per-sample branch - because unpacking arbitrary bit widths
// still needs a shift+mask+refill on every single sample, which turned out
// to be MORE work than production's aligned-load fast path. A byte-aligned
// width removes that bookkeeping entirely: one dispatch per BLOCK picks a
// specialized loop (by width), then within the block it's a straight
// typed array read, no per-sample branch AND no per-sample bit-shifting.
//
// No zigzag here (unlike the Rice-coding experiment): zigzag only earns its
// keep for *variable*-length/entropy codes, where it lets small negatives
// get short codes too. For a *fixed*-width slot, plain two's complement
// already stores any in-range value with no bias - zigzag would just be
// extra shift/xor/negate work on every single sample for no benefit.
inline int bytesNeededSigned(int32_t minV, int32_t maxV) {
  if (minV >= -128 && maxV <= 127)
    return 1;
  if (minV >= -32768 && maxV <= 32767)
    return 2;
  if (minV >= -8388608 && maxV <= 8388607)
    return 3;
  return 4;
}

// One channel's planar residual stream, block-header + fixed-width packed.
struct PlanarChannelStream {
  std::vector<uint8_t> bytes;
  unsigned nSamples = 0;
};

PlanarChannelStream encodeChannel(const std::vector<int32_t> &residuals) {
  PlanarChannelStream s;
  s.nSamples = (unsigned)residuals.size();
  for (unsigned i = 0; i < residuals.size(); i += BLOCK_SAMPLES) {
    const unsigned n = std::min((unsigned)BLOCK_SAMPLES, (unsigned)residuals.size() - i);
    int32_t minV = residuals[i], maxV = residuals[i];
    for (unsigned j = 1; j < n; j++) {
      minV = std::min(minV, residuals[i + j]);
      maxV = std::max(maxV, residuals[i + j]);
    }
    const int width = bytesNeededSigned(minV, maxV);
    s.bytes.push_back((uint8_t)width);
    const size_t base = s.bytes.size();
    s.bytes.resize(base + (size_t)n * width);
    uint8_t *out = s.bytes.data() + base;
    for (unsigned j = 0; j < n; j++) {
      const uint32_t v = (uint32_t)residuals[i + j]; // two's complement bits
      for (int b = 0; b < width; b++)
        out[j * width + b] = (uint8_t)(v >> (8 * b));
    }
  }
  return s;
}

// Specialized, branch-free-within-block decode loops, one per byte width.
// Direct typed reads (matching production's own aligned-load style)
// instead of a per-byte assembly loop, plus portable sign-extension via
// shift-up/shift-down instead of a zigzag round trip.
template <int WIDTH>
inline void decodeChannelWidth(
  const uint8_t *in,
  unsigned n,
  int32_t *out,
  unsigned stride,
  unsigned offset,
  int32_t &value,
  int32_t &prev,
  int32_t &last) {
  for (unsigned j = 0; j < n; j++) {
    int32_t residual;
    if constexpr (WIDTH == 1) {
      residual = (int32_t)(int8_t)in[j];
    } else if constexpr (WIDTH == 2) {
      int16_t v;
      memcpy(&v, in + j * 2, 2);
      residual = v;
    } else if constexpr (WIDTH == 3) {
      const uint32_t raw = (uint32_t)in[j * 3] | ((uint32_t)in[j * 3 + 1] << 8)
        | ((uint32_t)in[j * 3 + 2] << 16);
      residual = (int32_t)(raw << 8) >> 8; // sign-extend from bit 23
    } else {
      int32_t v;
      memcpy(&v, in + j * 4, 4);
      residual = v;
    }
    last = prev;
    prev = value;
    const int32_t pred = prev + (prev - last) / 2;
    value = pred + residual;
    out[(j)*stride + offset] = value;
  }
}

// Decode + reconstruct one channel's absolute values via GO's own predictor,
// writing into out[] (stride 'stride', starting at 'offset' - i.e. it can
// write directly into an interleaved buffer without a separate merge pass).
void decodeChannel(
  const PlanarChannelStream &s,
  int32_t *out,
  unsigned stride,
  unsigned offset) {
  const uint8_t *p = s.bytes.data();
  int32_t value = 0, prev = 0, last = 0;
  unsigned i = 0;
  while (i < s.nSamples) {
    const unsigned n = std::min((unsigned)BLOCK_SAMPLES, s.nSamples - i);
    const int width = *p++;
    int32_t *blockOut = out + (size_t)i * stride;
    switch (width) {
    case 1:
      decodeChannelWidth<1>(p, n, blockOut, stride, offset, value, prev, last);
      break;
    case 2:
      decodeChannelWidth<2>(p, n, blockOut, stride, offset, value, prev, last);
      break;
    case 3:
      decodeChannelWidth<3>(p, n, blockOut, stride, offset, value, prev, last);
      break;
    default:
      decodeChannelWidth<4>(p, n, blockOut, stride, offset, value, prev, last);
      break;
    }
    p += (size_t)n * width;
    i += n;
  }
}

// ---------------------------------------------------------------------------
// Interleaved (NOT planar) + fixed block width, single combined loop for
// both channels per frame - like production's own DecompressionStep shape,
// just with a fixed-width read instead of a variable-length one. Fixed
// width removes the cross-channel dependency even while interleaved (R's
// position is L's position + width, no need to read L's code length), so
// planar's separation may have been buying nothing once width is fixed -
// while its two-separate-passes cost real time. This tests that directly.
struct InterleavedBlockStream {
  std::vector<uint8_t> bytes;
  unsigned nFrames = 0;
  unsigned channels = 0;
};

InterleavedBlockStream encodeInterleavedBlockWidth(
  const std::vector<std::vector<int32_t>> &channelResiduals) {
  InterleavedBlockStream s;
  s.channels = (unsigned)channelResiduals.size();
  s.nFrames = (unsigned)channelResiduals[0].size();
  for (unsigned i = 0; i < s.nFrames; i += BLOCK_SAMPLES) {
    const unsigned n = std::min((unsigned)BLOCK_SAMPLES, s.nFrames - i);
    int32_t minV = channelResiduals[0][i], maxV = channelResiduals[0][i];
    for (unsigned c = 0; c < s.channels; c++)
      for (unsigned j = 0; j < n; j++) {
        minV = std::min(minV, channelResiduals[c][i + j]);
        maxV = std::max(maxV, channelResiduals[c][i + j]);
      }
    const int width = bytesNeededSigned(minV, maxV);
    s.bytes.push_back((uint8_t)width);
    const size_t base = s.bytes.size();
    s.bytes.resize(base + (size_t)n * s.channels * width);
    uint8_t *out = s.bytes.data() + base;
    for (unsigned j = 0; j < n; j++)
      for (unsigned c = 0; c < s.channels; c++) {
        const uint32_t v = (uint32_t)channelResiduals[c][i + j];
        for (int b = 0; b < width; b++)
          out[(j * s.channels + c) * width + b] = (uint8_t)(v >> (8 * b));
      }
  }
  return s;
}

template <int WIDTH, int CHANNELS>
inline void decodeInterleavedWidth(
  const uint8_t *in,
  unsigned n,
  int32_t *out,
  int32_t *value,
  int32_t *prev,
  int32_t *last) {
  for (unsigned j = 0; j < n; j++) {
    for (int c = 0; c < CHANNELS; c++) {
      int32_t residual;
      const uint8_t *ib = in + (j * CHANNELS + c) * WIDTH;
      if constexpr (WIDTH == 1) {
        residual = (int32_t)(int8_t)ib[0];
      } else if constexpr (WIDTH == 2) {
        int16_t v;
        memcpy(&v, ib, 2);
        residual = v;
      } else if constexpr (WIDTH == 3) {
        const uint32_t raw
          = (uint32_t)ib[0] | ((uint32_t)ib[1] << 8) | ((uint32_t)ib[2] << 16);
        residual = (int32_t)(raw << 8) >> 8;
      } else {
        int32_t v;
        memcpy(&v, ib, 4);
        residual = v;
      }
      last[c] = prev[c];
      prev[c] = value[c];
      const int32_t pred = prev[c] + (prev[c] - last[c]) / 2;
      value[c] = pred + residual;
      out[j * CHANNELS + c] = value[c];
    }
  }
}

// Stereo-only dispatch (matches our test files); general N-channel would
// dispatch CHANNELS too, omitted here for clarity.
void decodeInterleavedBlockWidth(
  const InterleavedBlockStream &s, int32_t *out) {
  const uint8_t *p = s.bytes.data();
  int32_t value[2] = {0, 0}, prev[2] = {0, 0}, last[2] = {0, 0};
  unsigned i = 0;
  while (i < s.nFrames) {
    const unsigned n = std::min((unsigned)BLOCK_SAMPLES, s.nFrames - i);
    const int width = *p++;
    int32_t *blockOut = out + (size_t)i * s.channels;
    switch (width) {
    case 1:
      decodeInterleavedWidth<1, 2>(p, n, blockOut, value, prev, last);
      break;
    case 2:
      decodeInterleavedWidth<2, 2>(p, n, blockOut, value, prev, last);
      break;
    case 3:
      decodeInterleavedWidth<3, 2>(p, n, blockOut, value, prev, last);
      break;
    default:
      decodeInterleavedWidth<4, 2>(p, n, blockOut, value, prev, last);
      break;
    }
    p += (size_t)n * s.channels * width;
    i += n;
  }
}

// ---------------------------------------------------------------------------
// Mid/Side transform (FLAC-style, exactly reversible integer arithmetic -
// no precision loss despite the >>1). Real stereo recordings are usually
// highly correlated (two mics in the same room), so side = L-R tends to be
// much smaller in magnitude than L or R alone - smaller residuals, smaller
// per-block fixed width. mid = (L+R)>>1 needs the same range as L/R; the
// discarded LSB is always recoverable from side's parity, since L+R and
// L-R always share the same parity ((L+R)-(L-R) = 2R is even).
inline void msForward(int32_t L, int32_t R, int32_t &mid, int32_t &side) {
  mid = (L + R) >> 1;
  side = L - R;
}
inline void msInverse(int32_t mid, int32_t side, int32_t &L, int32_t &R) {
  const int32_t sum = (mid << 1) | (side & 1);
  L = (sum + side) >> 1;
  R = (sum - side) >> 1;
}

// Same shape as decodeInterleavedWidth, but channel 0/1 are mid/side, not
// L/R - the ms inverse transform is folded into the same per-frame step so
// there is no separate extra pass over the data.
template <int WIDTH>
inline void decodeInterleavedWidthMS(
  const uint8_t *in,
  unsigned n,
  int32_t *out,
  int32_t *value,
  int32_t *prev,
  int32_t *last) {
  for (unsigned j = 0; j < n; j++) {
    for (int c = 0; c < 2; c++) {
      int32_t residual;
      const uint8_t *ib = in + (j * 2 + c) * WIDTH;
      if constexpr (WIDTH == 1) {
        residual = (int32_t)(int8_t)ib[0];
      } else if constexpr (WIDTH == 2) {
        int16_t v;
        memcpy(&v, ib, 2);
        residual = v;
      } else if constexpr (WIDTH == 3) {
        const uint32_t raw
          = (uint32_t)ib[0] | ((uint32_t)ib[1] << 8) | ((uint32_t)ib[2] << 16);
        residual = (int32_t)(raw << 8) >> 8;
      } else {
        int32_t v;
        memcpy(&v, ib, 4);
        residual = v;
      }
      last[c] = prev[c];
      prev[c] = value[c];
      const int32_t pred = prev[c] + (prev[c] - last[c]) / 2;
      value[c] = pred + residual;
    }
    int32_t L, R;
    msInverse(value[0], value[1], L, R);
    out[j * 2] = L;
    out[j * 2 + 1] = R;
  }
}

void decodeInterleavedBlockWidthMS(
  const InterleavedBlockStream &s, int32_t *out) {
  const uint8_t *p = s.bytes.data();
  int32_t value[2] = {0, 0}, prev[2] = {0, 0}, last[2] = {0, 0};
  unsigned i = 0;
  while (i < s.nFrames) {
    const unsigned n = std::min((unsigned)BLOCK_SAMPLES, s.nFrames - i);
    const int width = *p++;
    int32_t *blockOut = out + (size_t)i * s.channels;
    switch (width) {
    case 1:
      decodeInterleavedWidthMS<1>(p, n, blockOut, value, prev, last);
      break;
    case 2:
      decodeInterleavedWidthMS<2>(p, n, blockOut, value, prev, last);
      break;
    case 3:
      decodeInterleavedWidthMS<3>(p, n, blockOut, value, prev, last);
      break;
    default:
      decodeInterleavedWidthMS<4>(p, n, blockOut, value, prev, last);
      break;
    }
    p += (size_t)n * s.channels * width;
    i += n;
  }
}

// ---------------------------------------------------------------------------
size_t productionEncode(const WavData &w, std::vector<unsigned char> &out) {
  const bool format16 = w.bitsPerSample > 16;
  const unsigned nFrames = (unsigned)w.samples.size() / w.channels;
  out.resize(w.samples.size() * 4 + 16);
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
  return len;
}

double timeProductionDecode(
  const std::vector<unsigned char> &compressed,
  unsigned nFrames,
  unsigned channels,
  bool format16,
  std::vector<int32_t> &out /* interleaved, sized nFrames*channels */) {
  return bestSeconds(REPS, [&] {
    GOSoundCompressionCache c;
    c.Init();
    c.m_ptr = compressed.data();
    int32_t *o = out.data();
    for (unsigned i = 0; i < nFrames; i++) {
      c.DecompressionStep(channels, format16);
      o[i * channels] = c.m_value[0];
      if (channels > 1)
        o[i * channels + 1] = c.m_value[1];
    }
    volatile int32_t sink = o[0];
    (void)sink;
  });
}

} // namespace

int main(int argc, char **argv) {
  double totalRawBytes = 0, totalProdBytes = 0, totalPlanarBytes = 0;
  double totalProdNsFrame = 0, totalPlanarNsFrame = 0, totalPlanarContigNsFrame = 0;
  double totalInterNsFrame = 0, totalInterBytes = 0;
  double totalMsNsFrame = 0, totalMsBytes = 0;
  double totalFrames = 0;
  int nFiles = 0;

  for (int a = 1; a < argc; a++) {
    WavData w;
    if (!loadWav(argv[a], w)) {
      printf("SKIP (unreadable): %s\n", argv[a]);
      continue;
    }
    const bool format16 = w.bitsPerSample > 16;
    const unsigned nFrames = (unsigned)w.samples.size() / w.channels;
    const size_t rawBytes = w.samples.size() * (format16 ? 3 : 2);

    // --- production format: encode + verified round trip + timed decode ---
    std::vector<unsigned char> prodCompressed;
    const size_t prodLen = productionEncode(w, prodCompressed);
    {
      GOSoundCompressionCache c;
      c.Init();
      c.m_ptr = prodCompressed.data();
      bool ok = true;
      for (unsigned i = 0; i < nFrames && ok; i++) {
        c.DecompressionStep(w.channels, format16);
        if (c.m_value[0] != w.samples[i * w.channels])
          ok = false;
        if (w.channels > 1 && c.m_value[1] != w.samples[i * w.channels + 1])
          ok = false;
      }
      if (!ok) {
        printf("%s: PRODUCTION ROUNDTRIP MISMATCH!\n", argv[a]);
        continue;
      }
    }
    std::vector<int32_t> prodOut(w.samples.size());
    const double prodSec = timeProductionDecode(
      prodCompressed, nFrames, w.channels, format16, prodOut);
    const double prodNsFrame = prodSec / nFrames * 1e9;

    // --- planar block-width format: encode ---
    std::vector<std::vector<int32_t>> channelResiduals(w.channels);
    {
      std::vector<int32_t> value(w.channels, 0), prev(w.channels, 0),
        last(w.channels, 0);
      for (unsigned c = 0; c < w.channels; c++)
        channelResiduals[c].resize(nFrames);
      for (unsigned f = 0; f < nFrames; f++) {
        for (unsigned c = 0; c < w.channels; c++) {
          last[c] = prev[c];
          prev[c] = value[c];
          value[c] = w.samples[f * w.channels + c];
          const int32_t pred = prev[c] + (prev[c] - last[c]) / 2;
          channelResiduals[c][f] = value[c] - pred;
        }
      }
    }
    std::vector<PlanarChannelStream> planarStreams(w.channels);
    size_t planarBytes = 0;
    for (unsigned c = 0; c < w.channels; c++) {
      planarStreams[c] = encodeChannel(channelResiduals[c]);
      planarBytes += planarStreams[c].bytes.size();
    }

    // --- planar: verified round trip ---
    std::vector<int32_t> reconstructed(w.samples.size());
    for (unsigned c = 0; c < w.channels; c++)
      decodeChannel(planarStreams[c], reconstructed.data(), w.channels, c);
    bool planarOk = true;
    for (size_t i = 0; i < w.samples.size() && planarOk; i++)
      if (reconstructed[i] != w.samples[i])
        planarOk = false;
    if (!planarOk) {
      printf("%s: PLANAR ROUNDTRIP MISMATCH!\n", argv[a]);
      continue;
    }

    // --- planar: timed decode, interleaved-output variant (stride 2, for
    // apples-to-apples comparison against production's interleaved buffer)
    std::vector<int32_t> tmp(w.samples.size());
    const double planarSec = bestSeconds(REPS, [&] {
      for (unsigned c = 0; c < w.channels; c++)
        decodeChannel(planarStreams[c], tmp.data(), w.channels, c);
      volatile int32_t sink = tmp[0];
      (void)sink;
    });
    const double planarNsFrame = planarSec / nFrames * 1e9;

    // --- planar: timed decode, REALISTIC variant - each channel decodes
    // into its own separate contiguous buffer (stride 1), matching how the
    // planar AVX2 FIR kernel already consumes data (see
    // StreamCacheReadAheadWindow) - no artificial interleaving needed
    // downstream, so no reason to pay for it during decode either.
    std::vector<std::vector<int32_t>> tmpPlanar(w.channels);
    for (unsigned c = 0; c < w.channels; c++)
      tmpPlanar[c].resize(nFrames);
    const double planarContigSec = bestSeconds(REPS, [&] {
      for (unsigned c = 0; c < w.channels; c++)
        decodeChannel(planarStreams[c], tmpPlanar[c].data(), 1, 0);
      volatile int32_t sink = tmpPlanar[0][0];
      (void)sink;
    });
    const double planarContigNsFrame = planarContigSec / nFrames * 1e9;

    // --- interleaved + fixed block width: single combined loop, matching
    // production's shape (stereo only, given the hardcoded dispatch above)
    double interNsFrame = 0;
    size_t interBytes = 0;
    if (w.channels == 2) {
      InterleavedBlockStream is = encodeInterleavedBlockWidth(channelResiduals);
      interBytes = is.bytes.size();

      std::vector<int32_t> interOut(w.samples.size());
      decodeInterleavedBlockWidth(is, interOut.data());
      bool interOk = true;
      for (size_t i = 0; i < w.samples.size() && interOk; i++)
        if (interOut[i] != w.samples[i])
          interOk = false;
      if (!interOk) {
        printf("%s: INTERLEAVED ROUNDTRIP MISMATCH!\n", argv[a]);
      } else {
        const double interSec = bestSeconds(REPS, [&] {
          decodeInterleavedBlockWidth(is, interOut.data());
          volatile int32_t sink = interOut[0];
          (void)sink;
        });
        interNsFrame = interSec / nFrames * 1e9;
      }
    }

    // --- mid/side + fixed block width: same architecture, but channel 0/1
    // are mid=(L+R)>>1 / side=L-R instead of L/R directly, predictor and
    // block-width packing applied to mid/side residuals.
    double msNsFrame = 0;
    double msNoTransformNsFrame = 0;
    size_t msBytes = 0;
    if (w.channels == 2) {
      std::vector<int32_t> midR(nFrames), sideR(nFrames);
      {
        int32_t value[2] = {0, 0}, prev[2] = {0, 0}, last[2] = {0, 0};
        for (unsigned f = 0; f < nFrames; f++) {
          int32_t mid, side;
          msForward(
            w.samples[f * 2], w.samples[f * 2 + 1], mid, side);
          const int32_t ms[2] = {mid, side};
          int32_t residual[2];
          for (int c = 0; c < 2; c++) {
            last[c] = prev[c];
            prev[c] = value[c];
            value[c] = ms[c];
            const int32_t pred = prev[c] + (prev[c] - last[c]) / 2;
            residual[c] = value[c] - pred;
          }
          midR[f] = residual[0];
          sideR[f] = residual[1];
        }
      }
      std::vector<std::vector<int32_t>> msResiduals = {midR, sideR};
      InterleavedBlockStream ms = encodeInterleavedBlockWidth(msResiduals);
      msBytes = ms.bytes.size();

      std::vector<int32_t> msOut(w.samples.size());
      decodeInterleavedBlockWidthMS(ms, msOut.data());
      bool msOk = true;
      for (size_t i = 0; i < w.samples.size() && msOk; i++)
        if (msOut[i] != w.samples[i])
          msOk = false;
      if (!msOk) {
        printf("%s: MID/SIDE ROUNDTRIP MISMATCH!\n", argv[a]);
      } else {
        const double msSec = bestSeconds(REPS, [&] {
          decodeInterleavedBlockWidthMS(ms, msOut.data());
          volatile int32_t sink = msOut[0];
          (void)sink;
        });
        msNsFrame = msSec / nFrames * 1e9;

        // Isolate the transform's own cost: decode the SAME ms-encoded
        // bytes with the plain (no-transform) decoder - output is
        // meaningless (mid/side, not L/R) but timing isolates pure
        // extraction+reconstruction cost without msInverse().
        const double msNoTransformSec = bestSeconds(REPS, [&] {
          decodeInterleavedBlockWidth(ms, msOut.data());
          volatile int32_t sink = msOut[0];
          (void)sink;
        });
        msNoTransformNsFrame = msNoTransformSec / nFrames * 1e9;
      }
    }

    const char *name = strrchr(argv[a], '/');
    name = name ? name + 1 : argv[a];
    printf(
      "%-14s raw %8zu B | prod %8zu B (%.3fx, %5.2f ns/frame) | "
      "planar-blkw %8zu B (%.3fx, contig-out %5.2f ns/frame) | "
      "inter-blkw %8zu B (%.3fx, %5.2f ns/frame) | "
      "ms-blkw %8zu B (%.3fx, %5.2f ns/frame, no-transform %5.2f ns/frame)\n",
      name,
      rawBytes,
      prodLen,
      (double)rawBytes / prodLen,
      prodNsFrame,
      planarBytes,
      (double)rawBytes / planarBytes,
      planarContigNsFrame,
      interBytes,
      interBytes ? (double)rawBytes / interBytes : 0.0,
      interNsFrame,
      msBytes,
      msBytes ? (double)rawBytes / msBytes : 0.0,
      msNsFrame,
      msNoTransformNsFrame);

    totalRawBytes += rawBytes;
    totalProdBytes += prodLen;
    totalPlanarBytes += planarBytes;
    totalProdNsFrame += prodNsFrame * nFrames;
    totalPlanarNsFrame += planarNsFrame * nFrames;
    totalPlanarContigNsFrame += planarContigNsFrame * nFrames;
    totalInterNsFrame += interNsFrame * nFrames;
    totalInterBytes += interBytes;
    totalMsNsFrame += msNsFrame * nFrames;
    totalMsBytes += msBytes;
    totalFrames += nFrames;
    nFiles++;
  }

  if (nFiles > 0) {
    printf("\n=== TOTAL across %d files ===\n", nFiles);
    printf(
      "  ratio:  production %.3fx   planar-blockwidth %.3fx\n",
      totalRawBytes / totalProdBytes,
      totalRawBytes / totalPlanarBytes);
    printf(
      "  decode: production %.2f ns/frame   "
      "planar-blockwidth(interleaved-out) %.2f ns/frame   "
      "planar-blockwidth(contig-out) %.2f ns/frame\n",
      totalProdNsFrame / totalFrames,
      totalPlanarNsFrame / totalFrames,
      totalPlanarContigNsFrame / totalFrames);
    printf(
      "  speedup vs production: interleaved-out %.2fx   contig-out %.2fx\n",
      totalProdNsFrame / totalPlanarNsFrame,
      totalProdNsFrame / totalPlanarContigNsFrame);
    printf(
      "\n  interleaved+fixed-blockwidth (single combined loop, stereo only):\n"
      "    ratio %.3fx   decode %.2f ns/frame   speedup vs production "
      "%.2fx\n",
      totalRawBytes / totalInterBytes,
      totalInterNsFrame / totalFrames,
      totalProdNsFrame / totalInterNsFrame);
    printf(
      "\n  mid/side + fixed-blockwidth (stereo only):\n"
      "    ratio %.3fx   decode %.2f ns/frame   speedup vs production "
      "%.2fx   vs plain-inter %.2fx\n",
      totalRawBytes / totalMsBytes,
      totalMsNsFrame / totalFrames,
      totalProdNsFrame / totalMsNsFrame,
      totalInterNsFrame / totalMsNsFrame);
  }
  return 0;
}
