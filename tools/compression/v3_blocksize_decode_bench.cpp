// v3_blocksize_decode_bench.cpp
//
// The decisive question (user, 2026-07-13): a smaller V3 block + RLE
// width header is ~3-4% smaller, and decode is DRAM-bound - so does the
// smaller stream decode FASTER (less traffic), or does the finer width
// granularity fragment the 8-lane SIMD chunks and cost more?
//
// A size scan cannot answer it. This decodes real Rotterdam voices with
// an 8-lane AVX2 kernel modelled on production (GOSoundCompressionCache
// V3Batch::DecodeBatchImpl), in an RLE-RUN format: width persists for a
// run of samples (the encoder merges equal-width B-blocks), and the
// decode chunk is bounded by the nearest RUN boundary of any lane - so
// finer B only fragments chunks where the width actually changes. The
// working set (all voices' encoded streams) exceeds L3, decoded in
// engine-sized windows, so reads are DRAM-cold like the real ceiling.
//
// Build:
//   g++ -O2 -mavx2 -std=gnu++17 -I ../../src/grandorgue  \
//     -I ../../src/v3codec \
//       v3_blocksize_decode_bench.cpp -o /tmp/claude-1000/decbench

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <immintrin.h>
#include <string>
#include <vector>

#include "GOSoundCompressionCacheV3.h"

using Clock = std::chrono::steady_clock;

// ---------------- WAV (16/24 int, 32 float) ----------------
static uint32_t rd32(const unsigned char *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
struct Wav { unsigned channels = 0, bits = 0; std::vector<std::vector<int32_t>> ch; };

static bool loadWav(const char *path, Wav &w) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz < 44) { fclose(f); return false; }
  std::vector<unsigned char> b(sz);
  if (fread(b.data(), 1, sz, f) != (size_t)sz) { fclose(f); return false; }
  fclose(f);
  if (memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) return false;
  size_t pos = 12; const unsigned char *data = nullptr; uint32_t dl = 0, fmt = 0;
  while (pos + 8 <= (size_t)sz) {
    const unsigned char *id = b.data() + pos; uint32_t len = rd32(b.data() + pos + 4);
    const unsigned char *body = b.data() + pos + 8;
    if (pos + 8 + len > (size_t)sz) len = (uint32_t)sz - (pos + 8);
    if (!memcmp(id, "fmt ", 4) && len >= 16) { fmt = rd16(body); w.channels = rd16(body + 2); w.bits = rd16(body + 14); }
    else if (!memcmp(id, "data", 4)) { data = body; dl = len; }
    pos += 8 + len + (len & 1);
  }
  const bool isF = (fmt == 3);
  if (!data || !w.channels || (fmt != 1 && fmt != 0xFFFE && fmt != 3)) return false;
  if (isF ? w.bits != 32 : (w.bits != 16 && w.bits != 24)) return false;
  const unsigned bp = w.bits / 8, fb = bp * w.channels; if (!fb) return false;
  const uint32_t fr = dl / fb; if (fr < 2) return false;
  w.ch.assign(w.channels, std::vector<int32_t>(fr));
  for (uint32_t i = 0; i < fr; i++)
    for (unsigned c = 0; c < w.channels; c++) {
      const unsigned char *p = data + (size_t)i * fb + c * bp;
      if (isF) { float fv; memcpy(&fv, p, 4); double v = (double)fv * 8388608.0;
        v = v > 8388607 ? 8388607 : (v < -8388608 ? -8388608 : v);
        w.ch[c][i] = (int32_t)(v < 0 ? v - 0.5 : v + 0.5); }
      else w.ch[c][i] = (w.bits == 24)
        ? ((int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8)
        : (int16_t)(p[0] | (p[1] << 8));
    }
  if (isF) w.bits = 24;
  return true;
}

// ---------------- RLE-run encode at block size B ----------------
// Stream = runs of [width:1][runSamples:varint][channel-planar residuals].
// Width per B-block chosen by bytesNeededSigned over both channels; equal
// consecutive B-blocks merged into one run.
struct Enc { std::vector<unsigned char> bytes; unsigned channels, frames; };

static void putVarint(std::vector<unsigned char> &o, uint32_t v) {
  while (v >= 0x80) { o.push_back((v & 0x7F) | 0x80); v >>= 7; }
  o.push_back((unsigned char)v);
}

static Enc encodeRLE(const std::vector<std::vector<int32_t>> &res, unsigned C, unsigned B) {
  const size_t frames = res[0].size();
  // residuals (V3 predictor) precomputed by caller.
  // 1) width per B-block.
  std::vector<unsigned> bw; bw.reserve(frames / B + 1);
  for (size_t base = 0; base < frames; base += B) {
    const size_t n = std::min((size_t)B, frames - base);
    int32_t mn = res[0][base], mx = res[0][base];
    for (unsigned c = 0; c < C; c++)
      for (size_t j = 0; j < n; j++) { int32_t v = res[c][base + j]; if (v < mn) mn = v; if (v > mx) mx = v; }
    bw.push_back((unsigned)GOSoundCompressionCacheV3::bytesNeededSigned(mn, mx));
  }
  // 2) merge equal-width neighbours into runs, emit.
  Enc e; e.channels = C; e.frames = (unsigned)frames;
  size_t blk = 0;
  while (blk < bw.size()) {
    const unsigned width = bw[blk];
    size_t run = blk; while (run < bw.size() && bw[run] == width) run++;
    const size_t startSample = blk * B;
    const size_t endSample = std::min((size_t)run * B, frames);
    const uint32_t runSamples = (uint32_t)(endSample - startSample);
    e.bytes.push_back((unsigned char)width);
    putVarint(e.bytes, runSamples);
    // channel-planar residuals for the run.
    for (unsigned c = 0; c < C; c++)
      for (size_t s = startSample; s < endSample; s++) {
        uint32_t v = (uint32_t)res[c][s];
        for (unsigned k = 0; k < width; k++) e.bytes.push_back((v >> (8 * k)) & 0xFF);
      }
    blk = run;
  }
  return e;
}

// ---------------- 8-lane AVX2 decoder (run-based) ----------------
static const unsigned LANES = 8;
struct LaneState {
  const unsigned char *ptr, *runData; uint32_t runRemaining, width, runSamples;
  int32_t value[2], prev[2], last[2];
};

__attribute__((target("avx2"))) static inline void transpose8(__m256i *m) {
  __m256 r0=_mm256_castsi256_ps(m[0]),r1=_mm256_castsi256_ps(m[1]),r2=_mm256_castsi256_ps(m[2]),
    r3=_mm256_castsi256_ps(m[3]),r4=_mm256_castsi256_ps(m[4]),r5=_mm256_castsi256_ps(m[5]),
    r6=_mm256_castsi256_ps(m[6]),r7=_mm256_castsi256_ps(m[7]);
  __m256 t0=_mm256_unpacklo_ps(r0,r1),t1=_mm256_unpackhi_ps(r0,r1),t2=_mm256_unpacklo_ps(r2,r3),
    t3=_mm256_unpackhi_ps(r2,r3),t4=_mm256_unpacklo_ps(r4,r5),t5=_mm256_unpackhi_ps(r4,r5),
    t6=_mm256_unpacklo_ps(r6,r7),t7=_mm256_unpackhi_ps(r6,r7);
  __m256 u0=_mm256_shuffle_ps(t0,t2,0x44),u1=_mm256_shuffle_ps(t0,t2,0xEE),u2=_mm256_shuffle_ps(t1,t3,0x44),
    u3=_mm256_shuffle_ps(t1,t3,0xEE),u4=_mm256_shuffle_ps(t4,t6,0x44),u5=_mm256_shuffle_ps(t4,t6,0xEE),
    u6=_mm256_shuffle_ps(t5,t7,0x44),u7=_mm256_shuffle_ps(t5,t7,0xEE);
  m[0]=_mm256_castps_si256(_mm256_permute2f128_ps(u0,u4,0x20));
  m[1]=_mm256_castps_si256(_mm256_permute2f128_ps(u1,u5,0x20));
  m[2]=_mm256_castps_si256(_mm256_permute2f128_ps(u2,u6,0x20));
  m[3]=_mm256_castps_si256(_mm256_permute2f128_ps(u3,u7,0x20));
  m[4]=_mm256_castps_si256(_mm256_permute2f128_ps(u0,u4,0x31));
  m[5]=_mm256_castps_si256(_mm256_permute2f128_ps(u1,u5,0x31));
  m[6]=_mm256_castps_si256(_mm256_permute2f128_ps(u2,u6,0x31));
  m[7]=_mm256_castps_si256(_mm256_permute2f128_ps(u3,u7,0x31));
}

template <unsigned W>
__attribute__((target("avx2"))) static inline void extract(const unsigned char *ib, unsigned n, int32_t *row) {
  unsigned j = 0;
  if constexpr (W == 1) { for (; j + 8 <= n; j += 8) { __m128i in = _mm_loadl_epi64((const __m128i*)(ib+j));
      _mm256_storeu_si256((__m256i*)(row+j), _mm256_cvtepi8_epi32(in)); }
    for (; j < n; j++) row[j] = (int8_t)ib[j]; }
  else if constexpr (W == 2) { for (; j + 8 <= n; j += 8) { __m128i in = _mm_loadu_si128((const __m128i*)(ib+2*j));
      _mm256_storeu_si256((__m256i*)(row+j), _mm256_cvtepi16_epi32(in)); }
    for (; j < n; j++) row[j] = GOSoundCompressionCacheV3::readResidual(ib+(size_t)j*2, 2); }
  else if constexpr (W == 4) { for (; j + 8 <= n; j += 8)
      _mm256_storeu_si256((__m256i*)(row+j), _mm256_loadu_si256((const __m256i*)(ib+4*j)));
    for (; j < n; j++) row[j] = GOSoundCompressionCacheV3::readResidual(ib+(size_t)j*4, 4); }
  else { for (; j < n; j++) row[j] = GOSoundCompressionCacheV3::readResidual(ib+(size_t)j*3, 3); }
}

volatile int32_t g_sink;

// Advance ONE lane by n samples (scalar predictor) - used to phase-
// misalign lanes before timing, so their run boundaries fall at
// different positions exactly as voices started at different times do.
static void skipOne(LaneState &s, unsigned C, unsigned n) {
  unsigned done = 0;
  while (done < n) {
    if (s.runRemaining == 0) {
      s.width = *s.ptr++;
      uint32_t rs = 0; unsigned sh = 0; unsigned char bb;
      do { bb = *s.ptr++; rs |= (uint32_t)(bb & 0x7F) << sh; sh += 7; } while (bb & 0x80);
      s.runSamples = rs; s.runRemaining = rs; s.runData = s.ptr;
      s.ptr = s.runData + (size_t)C * rs * s.width;
    }
    unsigned take = std::min(n - done, s.runRemaining);
    for (unsigned c = 0; c < C; c++) {
      const unsigned j0 = s.runSamples - s.runRemaining;
      for (unsigned k = 0; k < take; k++) {
        const unsigned char *ib = s.runData + ((size_t)c * s.runSamples + j0 + k) * s.width;
        int32_t r = GOSoundCompressionCacheV3::readResidual(ib, s.width);
        s.last[c] = s.prev[c]; s.prev[c] = s.value[c];
        s.value[c] = s.prev[c] + ((s.prev[c] - s.last[c]) >> 1) + r;
      }
    }
    s.runRemaining -= take; done += take;
  }
}

// Decode `window` samples across 8 lanes (interleaved out). Returns samples done.
__attribute__((target("avx2")))
static void decodeWindow(LaneState *L, unsigned C, unsigned window, int32_t *out) {
  alignas(32) int32_t planar[2][LANES][2048];
  __m256i value[2], prev[2], last[2];
  for (unsigned c = 0; c < C; c++) {
    int32_t v[LANES], p[LANES], la[LANES];
    for (unsigned l = 0; l < LANES; l++) { v[l]=L[l].value[c]; p[l]=L[l].prev[c]; la[l]=L[l].last[c]; }
    value[c]=_mm256_loadu_si256((const __m256i*)v); prev[c]=_mm256_loadu_si256((const __m256i*)p);
    last[c]=_mm256_loadu_si256((const __m256i*)la);
  }
  unsigned done = 0;
  while (done < window) {
    for (unsigned l = 0; l < LANES; l++) if (L[l].runRemaining == 0) {
      L[l].width = *L[l].ptr; L[l].ptr++;
      uint32_t rs = 0; unsigned sh = 0; unsigned char bb;
      do { bb = *L[l].ptr++; rs |= (uint32_t)(bb & 0x7F) << sh; sh += 7; } while (bb & 0x80);
      L[l].runSamples = rs; L[l].runRemaining = rs; L[l].runData = L[l].ptr;
      L[l].ptr = L[l].runData + (size_t)C * rs * L[l].width;
    }
    unsigned chunk = window - done;
    for (unsigned l = 0; l < LANES; l++) chunk = std::min(chunk, L[l].runRemaining);
    if (chunk > 2048) chunk = 2048;
    for (unsigned c = 0; c < C; c++)
      for (unsigned l = 0; l < LANES; l++) {
        const unsigned width = L[l].width;
        const unsigned j = L[l].runSamples - L[l].runRemaining;
        const unsigned char *ib = L[l].runData + ((size_t)c * L[l].runSamples + j) * width;
        int32_t *row = planar[c][l];
        switch (width) { case 1: extract<1>(ib,chunk,row); break; case 2: extract<2>(ib,chunk,row); break;
          case 3: extract<3>(ib,chunk,row); break; default: extract<4>(ib,chunk,row); }
      }
    const unsigned chunk8 = chunk - (chunk % 8);
    for (unsigned j = 0; j < chunk8; j += 8)
      for (unsigned c = 0; c < C; c++) {
        __m256i rows[8];
        for (unsigned l = 0; l < LANES; l++) rows[l]=_mm256_loadu_si256((const __m256i*)(planar[c][l]+j));
        transpose8(rows);
        for (unsigned k = 0; k < 8; k++) {
          last[c]=prev[c]; prev[c]=value[c];
          __m256i diff=_mm256_sub_epi32(prev[c],last[c]);
          __m256i pred=_mm256_add_epi32(prev[c],_mm256_srai_epi32(diff,1));
          value[c]=_mm256_add_epi32(pred,rows[k]);
          _mm256_storeu_si256((__m256i*)(out+(size_t)(done+j+k)*LANES), value[c]);
        }
      }
    for (unsigned j = chunk8; j < chunk; j++)
      for (unsigned c = 0; c < C; c++) {
        alignas(32) int32_t tmp[LANES];
        for (unsigned l = 0; l < LANES; l++) tmp[l]=planar[c][l][j];
        __m256i r=_mm256_load_si256((const __m256i*)tmp);
        last[c]=prev[c]; prev[c]=value[c];
        __m256i diff=_mm256_sub_epi32(prev[c],last[c]);
        __m256i pred=_mm256_add_epi32(prev[c],_mm256_srai_epi32(diff,1));
        value[c]=_mm256_add_epi32(pred,r);
        _mm256_storeu_si256((__m256i*)(out+(size_t)(done+j)*LANES), value[c]);
      }
    for (unsigned l = 0; l < LANES; l++) L[l].runRemaining -= chunk;
    done += chunk;
  }
  for (unsigned c = 0; c < C; c++) {
    alignas(32) int32_t v[LANES],p[LANES],la[LANES];
    _mm256_store_si256((__m256i*)v,value[c]); _mm256_store_si256((__m256i*)p,prev[c]); _mm256_store_si256((__m256i*)la,last[c]);
    for (unsigned l = 0; l < LANES; l++) { L[l].value[c]=v[l]; L[l].prev[c]=p[l]; L[l].last[c]=la[l]; }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) { printf("usage: %s <filelist> [aligned]\n", argv[0]); return 1; }
  const bool aligned = (argc >= 3 && !strcmp(argv[2], "aligned"));
  std::vector<std::string> files;
  { FILE *f = fopen(argv[1], "r"); char ln[8192];
    while (fgets(ln, sizeof(ln), f)) { char *nl=strchr(ln,'\n'); if(nl)*nl=0; if(ln[0]) files.emplace_back(ln); }
    fclose(f); }

  // Load voices, compute residuals once, keep only stereo (pad mono to 2 for kernel).
  struct Voice { unsigned C; std::vector<std::vector<int32_t>> res; };
  std::vector<Voice> voices;
  for (auto &p : files) {
    Wav w; if (!loadWav(p.c_str(), w)) continue;
    Voice v; v.C = w.channels; v.res.assign(w.channels, {});
    for (unsigned c = 0; c < w.channels; c++) {
      const auto &s = w.ch[c]; v.res[c].resize(s.size());
      int32_t last=0,prev=0,val=0;
      for (size_t i=0;i<s.size();i++){ last=prev; prev=val; int32_t x=s[i];
        v.res[c][i]=x-(prev+((prev-last)>>1)); val=x; }
    }
    if (v.C == 2) voices.push_back(std::move(v));
  }
  printf("loaded %zu stereo voices\n", voices.size());
  const unsigned nBatches = (unsigned)voices.size() / LANES;
  printf("%u full 8-lane batches\n\n", nBatches);

  const unsigned window = 136; // engine span+guard at 128 buffer
  const unsigned Bs[] = {16, 32, 64, 128, 256, 1024};
  printf("mode: %s\n%-6s | %12s | %10s | %9s\n",
    aligned ? "PHASE-ALIGNED (grouping ceiling)" : "phase-misaligned (production today)",
    "B", "encoded MB", "ns/sample", "M smp/s");
  double base = 0;
  for (unsigned bi = 0; bi < sizeof(Bs)/sizeof(Bs[0]); bi++) {
    const unsigned B = Bs[bi];
    // encode every voice at this B.
    std::vector<Enc> enc; enc.reserve(voices.size());
    size_t totalBytes = 0, totalSamples = 0;
    for (auto &v : voices) { Enc e = encodeRLE(v.res, v.C, B); totalBytes += e.bytes.size();
      totalSamples += (size_t)e.frames; enc.push_back(std::move(e)); }

    std::vector<int32_t> out(window * LANES * 2);
    // time: decode every batch fully in windows, a few passes.
    const unsigned PASSES = 3;
    double best = 1e30;
    for (unsigned pass = 0; pass < PASSES; pass++) {
      const auto t0 = Clock::now();
      size_t decoded = 0;
      for (unsigned b = 0; b < nBatches; b++) {
        LaneState L[LANES];
        unsigned minFrames = ~0u;
        for (unsigned l = 0; l < LANES; l++) {
          Enc &e = enc[b*LANES+l];
          L[l].ptr = e.bytes.data(); L[l].runRemaining = 0;
          for (int c=0;c<2;c++){L[l].value[c]=0;L[l].prev[c]=0;L[l].last[c]=0;}
          // Phase-misalign: stagger each lane's start so run boundaries
          // fall at different positions, exactly as voices started at
          // different times. Offsets coprime-ish, well under a block.
          const unsigned off = aligned ? 0u : ((l * 313u + 7u) % 1600u);
          skipOne(L[l], 2, off);
          minFrames = std::min(minFrames, e.frames - off);
        }
        const unsigned lim = (minFrames / window) * window;
        for (unsigned pos = 0; pos < lim; pos += window) {
          decodeWindow(L, 2, window, out.data());
          decoded += window;
        }
        g_sink = out[decoded % (window*LANES*2)];
      }
      const auto t1 = Clock::now();
      double sec = std::chrono::duration<double>(t1 - t0).count();
      double perSample = sec / (decoded * LANES) * 1e9; // ns per (lane-)sample
      best = std::min(best, perSample);
    }
    if (bi == sizeof(Bs)/sizeof(Bs[0]) - 1) base = best; // 1024 is last
    printf("%-6u | %12.1f | %10.3f | %9.1f | %s\n",
      B, totalBytes / 1e6, best, 1000.0 / best,
      bi == sizeof(Bs)/sizeof(Bs[0]) - 1 ? "  (ref)" : "");
    fflush(stdout);
  }
  // second pass to print vs1024 now that base is known: re-run compactly.
  printf("\n(ns/sample = per lane-sample decoded; working set = all encoded "
    "voices, DRAM-cold; window=136)\n");
  return 0;
}
