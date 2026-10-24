// width_bucketing_check.cpp
//
// Tests the "Urgrossvater" idea: bucket real voices by their ACTUAL,
// already-known per-block width (read from each voice's own header byte -
// no guessing, no future knowledge, just grouping by a fact we already
// have at the block boundary) into batches of 8 sharing the SAME width,
// then decode each bucket with the ORIGINAL simple interleaved-shared-
// width kernel (single _mm256_cvtepi8/16/32_epi32 load across all 8 lanes,
// no transpose, no PSHUFB dispatch needed since width is uniform by
// construction) - vs our current v3 (planar+transpose, handles ANY width
// mix, no bucketing needed) on the SAME broad, unsorted real voice pool.
//
// Build:
//   g++ -O3 -mavx2 -ffast-math -funroll-loops -std=gnu++17 \
//     width_bucketing_check.cpp -o width_bucketing_check

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <immintrin.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned REPS = 5;
constexpr unsigned BLOCK_SAMPLES = 1024;
constexpr int LANES = 8;

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

std::vector<int32_t> loadWavMono(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  char riff[4]; f.read(riff,4); f.seekg(4,std::ios::cur);
  char wave[4]; f.read(wave,4);
  unsigned channels=0, bitsPerSample=0;
  std::vector<unsigned char> data;
  while (f) {
    char id[4]; f.read(id,4); if(!f) break;
    uint32_t chunkSize=0; f.read((char*)&chunkSize,4);
    if (memcmp(id,"fmt ",4)==0) {
      uint16_t audioFormat,nChannels,blockAlign,bps; uint32_t sr,byteRate;
      f.read((char*)&audioFormat,2); f.read((char*)&nChannels,2);
      f.read((char*)&sr,4); f.read((char*)&byteRate,4);
      f.read((char*)&blockAlign,2); f.read((char*)&bps,2);
      channels=nChannels; bitsPerSample=bps;
      if (chunkSize>16) f.seekg(chunkSize-16,std::ios::cur);
    } else if (memcmp(id,"data",4)==0) {
      data.resize(chunkSize); f.read((char*)data.data(),chunkSize);
    } else f.seekg(chunkSize,std::ios::cur);
  }
  const unsigned bps_=bitsPerSample/8;
  const unsigned nFrames=(unsigned)data.size()/(bps_*channels);
  std::vector<int32_t> L(nFrames);
  const unsigned char* p=data.data();
  for (unsigned i=0;i<nFrames;i++){
    int32_t v=(int32_t)(p[0]|(p[1]<<8)|(p[2]<<16));
    if (v&0x800000) v|=0xFF000000;
    L[i]=v; p+=bps_*channels;
  }
  return L;
}

inline int bytesNeededSigned(int32_t minV, int32_t maxV) {
  if (minV>=-128 && maxV<=127) return 1;
  if (minV>=-32768 && maxV<=32767) return 2;
  if (minV>=-8388608 && maxV<=8388607) return 3;
  return 4;
}

struct EncodedLane { std::vector<uint8_t> bytes; std::vector<uint8_t> blockWidths; };

EncodedLane encodeLaneIndependent(const std::vector<int32_t>& residuals, unsigned nSamples) {
  EncodedLane lane;
  for (unsigned i=0;i<nSamples;i+=BLOCK_SAMPLES){
    unsigned n=std::min((unsigned)BLOCK_SAMPLES,nSamples-i);
    int32_t minV=residuals[i],maxV=residuals[i];
    for (unsigned j=0;j<n;j++){ minV=std::min(minV,residuals[i+j]); maxV=std::max(maxV,residuals[i+j]); }
    int width=bytesNeededSigned(minV,maxV);
    lane.blockWidths.push_back((uint8_t)width);
    size_t base=lane.bytes.size();
    lane.bytes.resize(base+(size_t)n*width);
    uint8_t* out=lane.bytes.data()+base;
    for (unsigned j=0;j<n;j++){
      uint32_t v=(uint32_t)residuals[i+j];
      for(int b=0;b<width;b++) out[j*width+b]=(uint8_t)(v>>(8*b));
    }
  }
  lane.bytes.resize(lane.bytes.size() + 4, 0); // padding: safe 4-byte gather reads near the end
  return lane;
}

template <int WIDTH>
inline void extractLanePlanar(const uint8_t* __restrict ib, unsigned m, int32_t* __restrict row) {
  if constexpr (WIDTH == 3) {
    static const __m128i mask = _mm_setr_epi8((char)0x80,0,1,2,(char)0x80,3,4,5,(char)0x80,6,7,8,(char)0x80,9,10,11);
    unsigned j=0;
    for (; j+4<=m; j+=4) {
      __m128i in=_mm_loadu_si128((const __m128i*)(ib+(size_t)j*3));
      __m128i sh=_mm_shuffle_epi8(in,mask);
      _mm_storeu_si128((__m128i*)(row+j), _mm_srai_epi32(sh,8));
    }
    for (; j<m; j++) { const uint8_t*b=ib+(size_t)j*3; uint32_t raw=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16); row[j]=(int32_t)(raw<<8)>>8; }
    return;
  }
  for (unsigned j=0;j<m;j++) {
    const uint8_t* __restrict b = ib+(size_t)j*WIDTH;
    int32_t val;
    if constexpr (WIDTH==1) val=(int32_t)(int8_t)b[0];
    else if constexpr (WIDTH==2) { int16_t v; memcpy(&v,b,2); val=v; }
    else { memcpy(&val,b,4); }
    row[j]=val;
  }
}

inline void transpose8x8(__m256i* m) {
  __m256 r0=_mm256_castsi256_ps(m[0]),r1=_mm256_castsi256_ps(m[1]),r2=_mm256_castsi256_ps(m[2]),r3=_mm256_castsi256_ps(m[3]);
  __m256 r4=_mm256_castsi256_ps(m[4]),r5=_mm256_castsi256_ps(m[5]),r6=_mm256_castsi256_ps(m[6]),r7=_mm256_castsi256_ps(m[7]);
  __m256 t0=_mm256_unpacklo_ps(r0,r1),t1=_mm256_unpackhi_ps(r0,r1),t2=_mm256_unpacklo_ps(r2,r3),t3=_mm256_unpackhi_ps(r2,r3);
  __m256 t4=_mm256_unpacklo_ps(r4,r5),t5=_mm256_unpackhi_ps(r4,r5),t6=_mm256_unpacklo_ps(r6,r7),t7=_mm256_unpackhi_ps(r6,r7);
  __m256 tt0=_mm256_shuffle_ps(t0,t2,_MM_SHUFFLE(1,0,1,0)),tt1=_mm256_shuffle_ps(t0,t2,_MM_SHUFFLE(3,2,3,2));
  __m256 tt2=_mm256_shuffle_ps(t1,t3,_MM_SHUFFLE(1,0,1,0)),tt3=_mm256_shuffle_ps(t1,t3,_MM_SHUFFLE(3,2,3,2));
  __m256 tt4=_mm256_shuffle_ps(t4,t6,_MM_SHUFFLE(1,0,1,0)),tt5=_mm256_shuffle_ps(t4,t6,_MM_SHUFFLE(3,2,3,2));
  __m256 tt6=_mm256_shuffle_ps(t5,t7,_MM_SHUFFLE(1,0,1,0)),tt7=_mm256_shuffle_ps(t5,t7,_MM_SHUFFLE(3,2,3,2));
  m[0]=_mm256_castps_si256(_mm256_permute2f128_ps(tt0,tt4,0x20)); m[1]=_mm256_castps_si256(_mm256_permute2f128_ps(tt1,tt5,0x20));
  m[2]=_mm256_castps_si256(_mm256_permute2f128_ps(tt2,tt6,0x20)); m[3]=_mm256_castps_si256(_mm256_permute2f128_ps(tt3,tt7,0x20));
  m[4]=_mm256_castps_si256(_mm256_permute2f128_ps(tt0,tt4,0x31)); m[5]=_mm256_castps_si256(_mm256_permute2f128_ps(tt1,tt5,0x31));
  m[6]=_mm256_castps_si256(_mm256_permute2f128_ps(tt2,tt6,0x31)); m[7]=_mm256_castps_si256(_mm256_permute2f128_ps(tt3,tt7,0x31));
}

} // namespace

int main(int argc, char** argv) {
  const int NVOICES = argc - 1;
  if (NVOICES < LANES) { printf("need at least %d files\n", LANES); return 1; }

  std::vector<std::vector<int32_t>> voices(NVOICES);
  unsigned nSamples = UINT32_MAX;
  for (int v=0; v<NVOICES; v++) {
    voices[v] = loadWavMono(argv[v+1]);
    nSamples = std::min(nSamples, (unsigned)voices[v].size());
  }
  printf("%d voices, %u samples each\n", NVOICES, nSamples);

  std::vector<std::vector<int32_t>> residuals(NVOICES, std::vector<int32_t>(nSamples));
  for (int v=0; v<NVOICES; v++) {
    int32_t value=0, prev=0, last=0;
    for (unsigned i=0;i<nSamples;i++){
      last=prev; prev=value; value=voices[v][i];
      int32_t pred = prev + ((prev-last)>>1);
      residuals[v][i] = value - pred;
    }
  }
  std::vector<EncodedLane> lanes(NVOICES);
  for (int v=0; v<NVOICES; v++) lanes[v] = encodeLaneIndependent(residuals[v], nSamples);

  const unsigned nBlocks = (nSamples + BLOCK_SAMPLES - 1) / BLOCK_SAMPLES;

  // ---- Approach A: our current v3-style, UNSORTED groups of 8 (arbitrary
  // order, exactly as files were given), planar+transpose, PSHUFB for w=3 ----
  // (reuse simplified per-group decode, timed across ALL groups)
  std::vector<int32_t> unsortedOut((size_t)NVOICES * nSamples);
  auto decodeUnsortedGroup = [&](int base, bool storeOut) {
    std::vector<const uint8_t*> p(LANES);
    for (int v=0;v<LANES;v++) p[v]=lanes[base+v].bytes.data();
    __m256i value=_mm256_setzero_si256(), prev=_mm256_setzero_si256(), last=_mm256_setzero_si256();
    std::vector<int32_t> planar((size_t)LANES*BLOCK_SAMPLES);
    static const __m128i mask3 = _mm_setr_epi8((char)0x80,0,1,2,(char)0x80,3,4,5,(char)0x80,6,7,8,(char)0x80,9,10,11);
    unsigned i=0;
    unsigned blk=0;
    while (i<nSamples) {
      unsigned n=std::min((unsigned)BLOCK_SAMPLES,nSamples-i);
      int width[LANES];
      for (int v=0;v<LANES;v++) width[v]=lanes[base+v].blockWidths[blk];
      for (int v=0;v<LANES;v++) {
        const uint8_t* ib=p[v]; int w=width[v];
        int32_t* row=planar.data()+(size_t)v*BLOCK_SAMPLES;
        if (w==3) {
          unsigned j=0;
          for (;j+4<=n;j+=4){
            __m128i in=_mm_loadu_si128((const __m128i*)(ib+(size_t)j*3));
            __m128i sh=_mm_shuffle_epi8(in,mask3);
            _mm_storeu_si128((__m128i*)(row+j),_mm_srai_epi32(sh,8));
          }
          for(;j<n;j++){ const uint8_t*b=ib+(size_t)j*3; uint32_t raw=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16); row[j]=(int32_t)(raw<<8)>>8; }
        } else {
          for (unsigned j=0;j<n;j++){
            const uint8_t* b=ib+(size_t)j*w; int32_t val;
            if (w==1) val=(int32_t)(int8_t)b[0];
            else if (w==2) { int16_t vv; memcpy(&vv,b,2); val=vv; }
            else { memcpy(&val,b,4); }
            row[j]=val;
          }
        }
      }
      // transpose8x8 + arithmetic (8 at a time), tail scalar-gather
      unsigned m8 = n - (n%8);
      for (unsigned j=0;j<m8;j+=8){
        __m256i rows[8];
        for(int v=0;v<LANES;v++) rows[v]=_mm256_loadu_si256((const __m256i*)(planar.data()+(size_t)v*BLOCK_SAMPLES+j));
        // transpose (inline, same as before)
        __m256 r0=_mm256_castsi256_ps(rows[0]),r1=_mm256_castsi256_ps(rows[1]),r2=_mm256_castsi256_ps(rows[2]),r3=_mm256_castsi256_ps(rows[3]);
        __m256 r4=_mm256_castsi256_ps(rows[4]),r5=_mm256_castsi256_ps(rows[5]),r6=_mm256_castsi256_ps(rows[6]),r7=_mm256_castsi256_ps(rows[7]);
        __m256 t0=_mm256_unpacklo_ps(r0,r1),t1=_mm256_unpackhi_ps(r0,r1),t2=_mm256_unpacklo_ps(r2,r3),t3=_mm256_unpackhi_ps(r2,r3);
        __m256 t4=_mm256_unpacklo_ps(r4,r5),t5=_mm256_unpackhi_ps(r4,r5),t6=_mm256_unpacklo_ps(r6,r7),t7=_mm256_unpackhi_ps(r6,r7);
        __m256 tt0=_mm256_shuffle_ps(t0,t2,_MM_SHUFFLE(1,0,1,0)),tt1=_mm256_shuffle_ps(t0,t2,_MM_SHUFFLE(3,2,3,2));
        __m256 tt2=_mm256_shuffle_ps(t1,t3,_MM_SHUFFLE(1,0,1,0)),tt3=_mm256_shuffle_ps(t1,t3,_MM_SHUFFLE(3,2,3,2));
        __m256 tt4=_mm256_shuffle_ps(t4,t6,_MM_SHUFFLE(1,0,1,0)),tt5=_mm256_shuffle_ps(t4,t6,_MM_SHUFFLE(3,2,3,2));
        __m256 tt6=_mm256_shuffle_ps(t5,t7,_MM_SHUFFLE(1,0,1,0)),tt7=_mm256_shuffle_ps(t5,t7,_MM_SHUFFLE(3,2,3,2));
        rows[0]=_mm256_castps_si256(_mm256_permute2f128_ps(tt0,tt4,0x20)); rows[1]=_mm256_castps_si256(_mm256_permute2f128_ps(tt1,tt5,0x20));
        rows[2]=_mm256_castps_si256(_mm256_permute2f128_ps(tt2,tt6,0x20)); rows[3]=_mm256_castps_si256(_mm256_permute2f128_ps(tt3,tt7,0x20));
        rows[4]=_mm256_castps_si256(_mm256_permute2f128_ps(tt0,tt4,0x31)); rows[5]=_mm256_castps_si256(_mm256_permute2f128_ps(tt1,tt5,0x31));
        rows[6]=_mm256_castps_si256(_mm256_permute2f128_ps(tt2,tt6,0x31)); rows[7]=_mm256_castps_si256(_mm256_permute2f128_ps(tt3,tt7,0x31));
        for(int k=0;k<8;k++){
          last=prev; prev=value;
          __m256i diff=_mm256_sub_epi32(prev,last), half=_mm256_srai_epi32(diff,1), pred=_mm256_add_epi32(prev,half);
          value=_mm256_add_epi32(pred,rows[k]);
          if (storeOut) {
            alignas(32) int32_t outv[8];
            _mm256_store_si256((__m256i*)outv, value);
            for (int v=0;v<LANES;v++)
              unsortedOut[(size_t)(base+v)*nSamples + i + j + k] = outv[v];
          }
        }
      }
      for (unsigned j=m8;j<n;j++){
        alignas(32) int32_t tmp[LANES];
        for(int v=0;v<LANES;v++) tmp[v]=planar[(size_t)v*BLOCK_SAMPLES+j];
        __m256i residual=_mm256_load_si256((const __m256i*)tmp);
        last=prev; prev=value;
        __m256i diff=_mm256_sub_epi32(prev,last), half=_mm256_srai_epi32(diff,1), pred=_mm256_add_epi32(prev,half);
        value=_mm256_add_epi32(pred,residual);
        if (storeOut) {
          alignas(32) int32_t outv[8];
          _mm256_store_si256((__m256i*)outv, value);
          for (int v=0;v<LANES;v++)
            unsortedOut[(size_t)(base+v)*nSamples + i + j] = outv[v];
        }
      }
      for (int v=0;v<LANES;v++) p[v]+=(size_t)n*width[v];
      i+=n;
      blk++;
    }
  };

  // ---- Approach B: bucket by ACTUAL per-block width (known from header),
  // decode each same-width group of 8 with the simple interleaved kernel
  // (single cvtepi8/16/32_epi32 load, no transpose, no branch inside) ----
  // We simulate this per-block globally across all NVOICES: for each block,
  // partition voices into width buckets, process full groups of 8 within
  // each bucket with the simple kernel; leftover (<8) voices in a bucket
  // are decoded with a plain scalar step (rare, counted separately).
  std::vector<int32_t> bucketedOut((size_t)NVOICES * nSamples);
  auto decodeBucketed = [&](bool storeOut) {
    std::vector<int32_t> value(NVOICES,0), prev(NVOICES,0), last(NVOICES,0);
    std::vector<const uint8_t*> ptr(NVOICES);
    for (int v=0;v<NVOICES;v++) ptr[v]=lanes[v].bytes.data();
    // reused across blocks (.clear() keeps capacity) - avoids repeated
    // heap allocation/growth in the hot path.
    std::vector<int> buckets[5];
    for (auto &b : buckets) b.reserve(NVOICES);
    std::vector<int> widthOf(NVOICES);

    for (unsigned blk=0; blk<nBlocks; blk++) {
      unsigned base = blk*BLOCK_SAMPLES;
      unsigned n = std::min((unsigned)BLOCK_SAMPLES, nSamples-base);
      // bucket voices by this block's width
      for (auto &b : buckets) b.clear();
      for (int v=0;v<NVOICES;v++) {
        int w = lanes[v].blockWidths[blk];
        widthOf[v]=w;
        buckets[w].push_back(v);
      }
      for (int w=1; w<=4; w++) {
        auto& b = buckets[w];
        size_t g=0;
        for (; g+8<=b.size(); g+=8) {
          // simple interleaved kernel: since all 8 share width w, do a
          // straightforward per-sample loop reading w bytes/lane, using
          // the fast narrow-to-int32 SIMD load matching w (no branch
          // inside the sample loop - width fixed for this whole call).
          // State is LOADED from and STORED back to the persistent
          // per-voice arrays, so continuity holds across blocks even
          // though bucket membership (and thus SIMD-vs-scalar path) can
          // change from one block to the next for any given voice.
          alignas(32) int32_t v0[8], p0[8], l0[8];
          for (int k=0;k<8;k++) {
            v0[k]=value[b[g+k]]; p0[k]=prev[b[g+k]]; l0[k]=last[b[g+k]];
          }
          __m256i vval=_mm256_load_si256((const __m256i*)v0);
          __m256i vprev=_mm256_load_si256((const __m256i*)p0);
          __m256i vlast=_mm256_load_si256((const __m256i*)l0);
          const uint8_t* lp[8];
          for (int k=0;k<8;k++) lp[k]=ptr[b[g+k]];
          for (unsigned j=0;j<n;j++) {
            __m256i residual;
            if (w==1) {
              alignas(16) int8_t tmp[8];
              for(int k=0;k<8;k++) tmp[k]=(int8_t)lp[k][j];
              residual=_mm256_cvtepi8_epi32(_mm_loadl_epi64((const __m128i*)tmp));
            } else if (w==2) {
              alignas(16) int16_t tmp[8];
              for(int k=0;k<8;k++) memcpy(&tmp[k], lp[k]+j*2, 2);
              residual=_mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)tmp));
            } else if (w==3) {
              alignas(32) int32_t tmp[8];
              for(int k=0;k<8;k++){ const uint8_t*bb=lp[k]+j*3; uint32_t raw=(uint32_t)bb[0]|((uint32_t)bb[1]<<8)|((uint32_t)bb[2]<<16); tmp[k]=(int32_t)(raw<<8)>>8; }
              residual=_mm256_load_si256((const __m256i*)tmp);
            } else {
              alignas(32) int32_t tmp[8];
              for(int k=0;k<8;k++) memcpy(&tmp[k], lp[k]+j*4, 4);
              residual=_mm256_load_si256((const __m256i*)tmp);
            }
            vlast=vprev; vprev=vval;
            __m256i diff=_mm256_sub_epi32(vprev,vlast), half=_mm256_srai_epi32(diff,1), pred=_mm256_add_epi32(vprev,half);
            vval=_mm256_add_epi32(pred,residual);
            if (storeOut) {
              alignas(32) int32_t outv[8];
              _mm256_store_si256((__m256i*)outv, vval);
              for (int k=0;k<8;k++)
                bucketedOut[(size_t)b[g+k]*nSamples + base + j] = outv[k];
            }
          }
          // store final state back so continuation is correct even if
          // these voices land in a different bucket/path next block
          _mm256_store_si256((__m256i*)v0, vval);
          _mm256_store_si256((__m256i*)p0, vprev);
          _mm256_store_si256((__m256i*)l0, vlast);
          for (int k=0;k<8;k++) {
            value[b[g+k]]=v0[k]; prev[b[g+k]]=p0[k]; last[b[g+k]]=l0[k];
          }
        }
        // leftover in this bucket: plain scalar (rare, counted implicitly
        // in total time - realistic, since real batches won't always be
        // multiples of 8 either)
        for (; g<b.size(); g++) {
          int v=b[g];
          const uint8_t* lp=ptr[v];
          for (unsigned j=0;j<n;j++) {
            int32_t residual;
            if (w==1) residual=(int32_t)(int8_t)lp[j];
            else if (w==2) { int16_t vv; memcpy(&vv,lp+j*2,2); residual=vv; }
            else if (w==3) { const uint8_t*bb=lp+j*3; uint32_t raw=(uint32_t)bb[0]|((uint32_t)bb[1]<<8)|((uint32_t)bb[2]<<16); residual=(int32_t)(raw<<8)>>8; }
            else { memcpy(&residual,lp+j*4,4); }
            last[v]=prev[v]; prev[v]=value[v];
            int32_t pred=prev[v]+((prev[v]-last[v])>>1);
            value[v]=pred+residual;
            if (storeOut)
              bucketedOut[(size_t)v*nSamples + base + j] = value[v];
          }
        }
      }
      for (int v=0;v<NVOICES;v++) ptr[v]+=(size_t)n*widthOf[v];
    }
  };

  // ---- Approach C: same bucketing as B, but planar BULK extraction per
  // lane (whole block at once, contiguous - vectorizable, no per-sample
  // buffer hopping) + AVX2 8x8 transpose, instead of B's per-sample gather
  // across 8 scattered buffers. Width is uniform within a bucket-group, so
  // only ONE extraction routine is picked per GROUP (not per lane, unlike
  // v3's per-lane switch) - the fairest realization of the bucketing idea.
  std::vector<int32_t> bucketedPlanarOut((size_t)NVOICES * nSamples);
  auto decodeBucketedPlanar = [&](bool storeOut) {
    std::vector<int32_t> value(NVOICES,0), prev(NVOICES,0), last(NVOICES,0);
    std::vector<const uint8_t*> ptr(NVOICES);
    for (int v=0;v<NVOICES;v++) ptr[v]=lanes[v].bytes.data();
    std::vector<int> buckets[5];
    for (auto &b : buckets) b.reserve(NVOICES);
    std::vector<int> widthOf(NVOICES);
    std::vector<int32_t> planar((size_t)LANES*BLOCK_SAMPLES);

    for (unsigned blk=0; blk<nBlocks; blk++) {
      unsigned base = blk*BLOCK_SAMPLES;
      unsigned n = std::min((unsigned)BLOCK_SAMPLES, nSamples-base);
      for (auto &b : buckets) b.clear();
      for (int v=0;v<NVOICES;v++) {
        int w = lanes[v].blockWidths[blk];
        widthOf[v]=w;
        buckets[w].push_back(v);
      }
      for (int w=1; w<=4; w++) {
        auto& b = buckets[w];
        size_t g=0;
        for (; g+8<=b.size(); g+=8) {
          alignas(32) int32_t v0[8], p0[8], l0[8];
          for (int k=0;k<8;k++) { v0[k]=value[b[g+k]]; p0[k]=prev[b[g+k]]; l0[k]=last[b[g+k]]; }
          __m256i vval=_mm256_load_si256((const __m256i*)v0);
          __m256i vprev=_mm256_load_si256((const __m256i*)p0);
          __m256i vlast=_mm256_load_si256((const __m256i*)l0);

          // bulk planar extraction, one code path for all 8 lanes since
          // width is known and uniform for this whole group.
          for (int k=0;k<8;k++) {
            const uint8_t* ib = ptr[b[g+k]];
            int32_t* row = planar.data() + (size_t)k*BLOCK_SAMPLES;
            switch (w) {
              case 1: extractLanePlanar<1>(ib, n, row); break;
              case 2: extractLanePlanar<2>(ib, n, row); break;
              case 3: extractLanePlanar<3>(ib, n, row); break;
              default: extractLanePlanar<4>(ib, n, row); break;
            }
          }
          unsigned m8 = n - (n%8);
          for (unsigned j=0;j<m8;j+=8) {
            __m256i rows[8];
            for (int k=0;k<8;k++) rows[k]=_mm256_loadu_si256((const __m256i*)(planar.data()+(size_t)k*BLOCK_SAMPLES+j));
            transpose8x8(rows);
            for (int kk=0;kk<8;kk++) {
              vlast=vprev; vprev=vval;
              __m256i diff=_mm256_sub_epi32(vprev,vlast), half=_mm256_srai_epi32(diff,1), pred=_mm256_add_epi32(vprev,half);
              vval=_mm256_add_epi32(pred,rows[kk]);
              if (storeOut) {
                alignas(32) int32_t outv[8];
                _mm256_store_si256((__m256i*)outv, vval);
                for (int k=0;k<8;k++) bucketedPlanarOut[(size_t)b[g+k]*nSamples + base + j + kk] = outv[k];
              }
            }
          }
          for (unsigned j=m8;j<n;j++) {
            alignas(32) int32_t tmp[8];
            for (int k=0;k<8;k++) tmp[k]=planar[(size_t)k*BLOCK_SAMPLES+j];
            __m256i residual=_mm256_load_si256((const __m256i*)tmp);
            vlast=vprev; vprev=vval;
            __m256i diff=_mm256_sub_epi32(vprev,vlast), half=_mm256_srai_epi32(diff,1), pred=_mm256_add_epi32(vprev,half);
            vval=_mm256_add_epi32(pred,residual);
            if (storeOut) {
              alignas(32) int32_t outv[8];
              _mm256_store_si256((__m256i*)outv, vval);
              for (int k=0;k<8;k++) bucketedPlanarOut[(size_t)b[g+k]*nSamples + base + j] = outv[k];
            }
          }
          _mm256_store_si256((__m256i*)v0, vval);
          _mm256_store_si256((__m256i*)p0, vprev);
          _mm256_store_si256((__m256i*)l0, vlast);
          for (int k=0;k<8;k++) { value[b[g+k]]=v0[k]; prev[b[g+k]]=p0[k]; last[b[g+k]]=l0[k]; }
        }
        for (; g<b.size(); g++) {
          int v=b[g];
          const uint8_t* lp=ptr[v];
          for (unsigned j=0;j<n;j++) {
            int32_t residual;
            if (w==1) residual=(int32_t)(int8_t)lp[j];
            else if (w==2) { int16_t vv; memcpy(&vv,lp+j*2,2); residual=vv; }
            else if (w==3) { const uint8_t*bb=lp+j*3; uint32_t raw=(uint32_t)bb[0]|((uint32_t)bb[1]<<8)|((uint32_t)bb[2]<<16); residual=(int32_t)(raw<<8)>>8; }
            else { memcpy(&residual,lp+j*4,4); }
            last[v]=prev[v]; prev[v]=value[v];
            int32_t pred=prev[v]+((prev[v]-last[v])>>1);
            value[v]=pred+residual;
            if (storeOut) bucketedPlanarOut[(size_t)v*nSamples + base + j] = value[v];
          }
        }
      }
      for (int v=0;v<NVOICES;v++) ptr[v]+=(size_t)n*widthOf[v];
    }
  };

  // ---- Approach D: bucket by width like B/C, but instead of extracting to
  // planar int32 + transpose (24 shuffle/permute instructions per 8
  // samples), REPACK the native-width BYTES into an interleaved byte
  // buffer (lane-minor, step-major - exactly the layout the ORIGINAL
  // simd_multivoice_check.cpp kernel used, which never needed a transpose
  // because its data was encoded jointly). This repack is a cheap bulk
  // byte copy per lane (no widening, no shuffle), done once per block;
  // after that, decode uses the SAME single-load-per-step kernel as the
  // original test - no transpose in the hot loop at all.
  std::vector<int32_t> repackOut((size_t)NVOICES * nSamples);
  auto decodeRepacked = [&](bool storeOut) {
    std::vector<int32_t> value(NVOICES,0), prev(NVOICES,0), last(NVOICES,0);
    std::vector<const uint8_t*> ptr(NVOICES);
    for (int v=0;v<NVOICES;v++) ptr[v]=lanes[v].bytes.data();
    std::vector<int> buckets[5];
    for (auto &b : buckets) b.reserve(NVOICES);
    std::vector<int> widthOf(NVOICES);
    std::vector<uint8_t> interleaved((size_t)BLOCK_SAMPLES*LANES*4); // worst case width=4

    for (unsigned blk=0; blk<nBlocks; blk++) {
      unsigned base = blk*BLOCK_SAMPLES;
      unsigned n = std::min((unsigned)BLOCK_SAMPLES, nSamples-base);
      for (auto &b : buckets) b.clear();
      for (int v=0;v<NVOICES;v++) {
        int w = lanes[v].blockWidths[blk];
        widthOf[v]=w;
        buckets[w].push_back(v);
      }
      for (int w=1; w<=4; w++) {
        auto& b = buckets[w];
        size_t g=0;
        for (; g+8<=b.size(); g+=8) {
          alignas(32) int32_t v0[8], p0[8], l0[8];
          for (int k=0;k<8;k++) { v0[k]=value[b[g+k]]; p0[k]=prev[b[g+k]]; l0[k]=last[b[g+k]]; }
          __m256i vval=_mm256_load_si256((const __m256i*)v0);
          __m256i vprev=_mm256_load_si256((const __m256i*)p0);
          __m256i vlast=_mm256_load_si256((const __m256i*)l0);

          // repack: per lane, bulk-copy its n*w native-width bytes into
          // the interleaved buffer at stride LANES*w (cheap memcpy-style
          // copy, no widening/shuffling - width is uniform, no branch
          // needed inside this per-lane copy).
          for (int k=0;k<8;k++) {
            const uint8_t* __restrict src = ptr[b[g+k]];
            uint8_t* __restrict dst = interleaved.data() + (size_t)k*w;
            const size_t stride = (size_t)LANES*w;
            if (w==1) {
              for (unsigned j=0;j<n;j++) dst[j*stride] = src[j];
            } else if (w==2) {
              for (unsigned j=0;j<n;j++) { uint16_t v; memcpy(&v,src+j*2,2); memcpy(dst+j*stride,&v,2); }
            } else if (w==3) {
              for (unsigned j=0;j<n;j++) { dst[j*stride]=src[j*3]; dst[j*stride+1]=src[j*3+1]; dst[j*stride+2]=src[j*3+2]; }
            } else {
              for (unsigned j=0;j<n;j++) { uint32_t v; memcpy(&v,src+j*4,4); memcpy(dst+j*stride,&v,4); }
            }
          }

          const uint8_t* ip = interleaved.data();
          for (unsigned j=0;j<n;j++) {
            __m256i residual;
            const uint8_t* row = ip + (size_t)j*LANES*w;
            if (w==1) residual=_mm256_cvtepi8_epi32(_mm_loadl_epi64((const __m128i*)row));
            else if (w==2) residual=_mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)row));
            else if (w==3) {
              alignas(32) int32_t tmp[8];
              for (int k=0;k<8;k++) { const uint8_t*bb=row+k*3; uint32_t raw=(uint32_t)bb[0]|((uint32_t)bb[1]<<8)|((uint32_t)bb[2]<<16); tmp[k]=(int32_t)(raw<<8)>>8; }
              residual=_mm256_load_si256((const __m256i*)tmp);
            } else residual=_mm256_loadu_si256((const __m256i*)row);
            vlast=vprev; vprev=vval;
            __m256i diff=_mm256_sub_epi32(vprev,vlast), half=_mm256_srai_epi32(diff,1), pred=_mm256_add_epi32(vprev,half);
            vval=_mm256_add_epi32(pred,residual);
            if (storeOut) {
              alignas(32) int32_t outv[8];
              _mm256_store_si256((__m256i*)outv, vval);
              for (int k=0;k<8;k++) repackOut[(size_t)b[g+k]*nSamples + base + j] = outv[k];
            }
          }
          _mm256_store_si256((__m256i*)v0, vval);
          _mm256_store_si256((__m256i*)p0, vprev);
          _mm256_store_si256((__m256i*)l0, vlast);
          for (int k=0;k<8;k++) { value[b[g+k]]=v0[k]; prev[b[g+k]]=p0[k]; last[b[g+k]]=l0[k]; }
        }
        for (; g<b.size(); g++) {
          int v=b[g];
          const uint8_t* lp=ptr[v];
          for (unsigned j=0;j<n;j++) {
            int32_t residual;
            if (w==1) residual=(int32_t)(int8_t)lp[j];
            else if (w==2) { int16_t vv; memcpy(&vv,lp+j*2,2); residual=vv; }
            else if (w==3) { const uint8_t*bb=lp+j*3; uint32_t raw=(uint32_t)bb[0]|((uint32_t)bb[1]<<8)|((uint32_t)bb[2]<<16); residual=(int32_t)(raw<<8)>>8; }
            else { memcpy(&residual,lp+j*4,4); }
            last[v]=prev[v]; prev[v]=value[v];
            int32_t pred=prev[v]+((prev[v]-last[v])>>1);
            value[v]=pred+residual;
            if (storeOut) repackOut[(size_t)v*nSamples + base + j] = value[v];
          }
        }
      }
      for (int v=0;v<NVOICES;v++) ptr[v]+=(size_t)n*widthOf[v];
    }
  };

  // ---- Approach E: bucket by width like B/C/D, but use AVX2 GATHER
  // (_mm256_i64gather_epi32) to read directly from the 8 independent lane
  // pointers in one instruction pair - no repack, no transpose at all.
  // Base=nullptr, indices=the actual 64-bit pointer values (safe: no
  // overflow, unlike a 32-bit-offset-from-shared-base trick). Each gather
  // reads a raw 4-byte word per lane regardless of the real width; for
  // width<4 that reads a few garbage bytes beyond the intended value,
  // which are then discarded by a left-shift+arithmetic-right-shift pair
  // (same sign-extension trick as everywhere else) - this only works
  // because encodeLaneIndependent pads 4 bytes at the end of every lane,
  // so the occasional over-read near the end never leaves the allocation.
  std::vector<int32_t> gatherOut((size_t)NVOICES * nSamples);
  auto decodeGather = [&](bool storeOut) {
    std::vector<int32_t> value(NVOICES,0), prev(NVOICES,0), last(NVOICES,0);
    std::vector<const uint8_t*> ptr(NVOICES);
    for (int v=0;v<NVOICES;v++) ptr[v]=lanes[v].bytes.data();
    std::vector<int> buckets[5];
    for (auto &b : buckets) b.reserve(NVOICES);
    std::vector<int> widthOf(NVOICES);

    for (unsigned blk=0; blk<nBlocks; blk++) {
      unsigned base = blk*BLOCK_SAMPLES;
      unsigned n = std::min((unsigned)BLOCK_SAMPLES, nSamples-base);
      for (auto &b : buckets) b.clear();
      for (int v=0;v<NVOICES;v++) {
        int w = lanes[v].blockWidths[blk];
        widthOf[v]=w;
        buckets[w].push_back(v);
      }
      for (int w=1; w<=4; w++) {
        auto& b = buckets[w];
        size_t g=0;
        for (; g+8<=b.size(); g+=8) {
          alignas(32) int32_t v0[8], p0[8], l0[8];
          for (int k=0;k<8;k++) { v0[k]=value[b[g+k]]; p0[k]=prev[b[g+k]]; l0[k]=last[b[g+k]]; }
          __m256i vval=_mm256_load_si256((const __m256i*)v0);
          __m256i vprev=_mm256_load_si256((const __m256i*)p0);
          __m256i vlast=_mm256_load_si256((const __m256i*)l0);

          alignas(32) int64_t addrLo[4], addrHi[4];
          for (int k=0;k<4;k++) addrLo[k] = (int64_t)ptr[b[g+k]];
          for (int k=0;k<4;k++) addrHi[k] = (int64_t)ptr[b[g+4+k]];
          __m256i vAddrLo = _mm256_load_si256((const __m256i*)addrLo);
          __m256i vAddrHi = _mm256_load_si256((const __m256i*)addrHi);
          const __m256i vStep = _mm256_set1_epi64x(w);
          const int shiftBits = 32 - w*8;

          for (unsigned j=0;j<n;j++) {
            __m128i gLo = _mm256_i64gather_epi32((const int*)nullptr, vAddrLo, 1);
            __m128i gHi = _mm256_i64gather_epi32((const int*)nullptr, vAddrHi, 1);
            __m256i gathered = _mm256_set_m128i(gHi, gLo);
            __m256i residual = (w==4) ? gathered
              : _mm256_srai_epi32(_mm256_slli_epi32(gathered, shiftBits), shiftBits);
            vlast=vprev; vprev=vval;
            __m256i diff=_mm256_sub_epi32(vprev,vlast), half=_mm256_srai_epi32(diff,1), pred=_mm256_add_epi32(vprev,half);
            vval=_mm256_add_epi32(pred,residual);
            if (storeOut) {
              alignas(32) int32_t outv[8];
              _mm256_store_si256((__m256i*)outv, vval);
              for (int k=0;k<8;k++) gatherOut[(size_t)b[g+k]*nSamples + base + j] = outv[k];
            }
            vAddrLo = _mm256_add_epi64(vAddrLo, vStep);
            vAddrHi = _mm256_add_epi64(vAddrHi, vStep);
          }
          _mm256_store_si256((__m256i*)v0, vval);
          _mm256_store_si256((__m256i*)p0, vprev);
          _mm256_store_si256((__m256i*)l0, vlast);
          for (int k=0;k<8;k++) { value[b[g+k]]=v0[k]; prev[b[g+k]]=p0[k]; last[b[g+k]]=l0[k]; }
        }
        for (; g<b.size(); g++) {
          int v=b[g];
          const uint8_t* lp=ptr[v];
          for (unsigned j=0;j<n;j++) {
            int32_t residual;
            if (w==1) residual=(int32_t)(int8_t)lp[j];
            else if (w==2) { int16_t vv; memcpy(&vv,lp+j*2,2); residual=vv; }
            else if (w==3) { const uint8_t*bb=lp+j*3; uint32_t raw=(uint32_t)bb[0]|((uint32_t)bb[1]<<8)|((uint32_t)bb[2]<<16); residual=(int32_t)(raw<<8)>>8; }
            else { memcpy(&residual,lp+j*4,4); }
            last[v]=prev[v]; prev[v]=value[v];
            int32_t pred=prev[v]+((prev[v]-last[v])>>1);
            value[v]=pred+residual;
            if (storeOut) gatherOut[(size_t)v*nSamples + base + j] = value[v];
          }
        }
      }
      for (int v=0;v<NVOICES;v++) ptr[v]+=(size_t)n*widthOf[v];
    }
  };

  int nFullGroups = NVOICES/LANES;

  // one-time correctness check WITH output storage (slow due to poor
  // locality of the verification buffer layout - irrelevant here since
  // it's not timed)
  for (int g=0; g<nFullGroups; g++) decodeUnsortedGroup(g*LANES, true);
  decodeBucketed(true);
  decodeBucketedPlanar(true);
  decodeRepacked(true);
  decodeGather(true);
  bool okA = true, okB = true, okC = true, okD = true, okE = true;
  for (int v=0; v<nFullGroups*LANES && okA; v++)
    for (unsigned i=0; i<nSamples && okA; i++)
      if (unsortedOut[(size_t)v*nSamples+i] != voices[v][i]) okA = false;
  for (int v=0; v<NVOICES && okB; v++)
    for (unsigned i=0; i<nSamples && okB; i++)
      if (bucketedOut[(size_t)v*nSamples+i] != voices[v][i]) okB = false;
  for (int v=0; v<NVOICES && okC; v++)
    for (unsigned i=0; i<nSamples && okC; i++)
      if (bucketedPlanarOut[(size_t)v*nSamples+i] != voices[v][i]) okC = false;
  for (int v=0; v<NVOICES && okD; v++)
    for (unsigned i=0; i<nSamples && okD; i++)
      if (repackOut[(size_t)v*nSamples+i] != voices[v][i]) okD = false;
  for (int v=0; v<NVOICES && okE; v++)
    for (unsigned i=0; i<nSamples && okE; i++)
      if (gatherOut[(size_t)v*nSamples+i] != voices[v][i]) okE = false;
  printf("roundtrip: A %s, B %s, C %s, D %s, E %s\n", okA?"OK":"MISMATCH!", okB?"OK":"MISMATCH!", okC?"OK":"MISMATCH!", okD?"OK":"MISMATCH!", okE?"OK":"MISMATCH!");

  // actual timed runs WITHOUT output storage (matches how decode cost was
  // measured throughout this whole investigation - state-only, no full
  // output log, since that's not part of the real hot loop's job either)
  auto approachA = [&]{ for (int g=0; g<nFullGroups; g++) decodeUnsortedGroup(g*LANES, false); };
  auto approachB = [&]{ decodeBucketed(false); };
  auto approachC = [&]{ decodeBucketedPlanar(false); };
  auto approachD = [&]{ decodeRepacked(false); };
  auto approachE = [&]{ decodeGather(false); };
  approachA(); approachB(); approachC(); approachD(); approachE();
  double secA = bestSeconds(REPS, approachA);
  double secB = bestSeconds(REPS, approachB);
  double secC = bestSeconds(REPS, approachC);
  double secD = bestSeconds(REPS, approachD);
  double secE = bestSeconds(REPS, approachE);
  double nsPerChA = secA/(nSamples*(double)(nFullGroups*LANES))*1e9;
  double nsPerChB = secB/(nSamples*(double)NVOICES)*1e9;
  double nsPerChC = secC/(nSamples*(double)NVOICES)*1e9;
  double nsPerChD = secD/(nSamples*(double)NVOICES)*1e9;
  double nsPerChE = secE/(nSamples*(double)NVOICES)*1e9;

  printf("\nApproach A (unsorted, planar+transpose, current v3):        %.4f ns/channel\n", nsPerChA);
  printf("Approach B (bucketed, per-sample gather-across-buffers kernel): %.4f ns/channel  (%.2fx)\n", nsPerChB, nsPerChA/nsPerChB);
  printf("Approach C (bucketed, planar bulk-extract + transpose):      %.4f ns/channel  (%.2fx)\n", nsPerChC, nsPerChA/nsPerChC);
  printf("Approach D (bucketed, repack to interleaved bytes, no transpose): %.4f ns/channel  (%.2fx)\n", nsPerChD, nsPerChA/nsPerChD);
  printf("Approach E (bucketed, AVX2 hardware gather, no repack/transpose): %.4f ns/channel  (%.2fx)\n", nsPerChE, nsPerChA/nsPerChE);

  return 0;
}
