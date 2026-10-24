// surround_preinterleave_check.cpp
//
// Tests the "surround sample set" scenario: N perspectives (e.g. 4 mic
// positions, up to 8 channels if stereo) of the SAME pipe, which are
// ALWAYS decoded together, perfectly time-aligned (same physical event) -
// unlike arbitrary runtime-chosen batches, this group is FIXED and known
// at compress time. That removes both blockers found earlier (no future
#include <array>
// prediction needed, no block-boundary desync risk).
//
// Encodes the group with COMPRESS-TIME interleaving: bytes laid out
// sample-major (all 8 lanes' current-sample bytes contiguous, in ONE
// combined stream) instead of lane-major (today's independent-per-lane
// format) - but each lane KEEPS its own independently-chosen per-block
// width (no ratio cost, unlike forcing a shared width). Decode then reads
// directly from this one combined stream, per sample, with no repack, no
// transpose, no gather - contrasted with v3 (independent per-lane format,
// decode-time planar+transpose).
//
// Build:
//   g++ -O3 -mavx2 -ffast-math -funroll-loops -std=gnu++17 \
//     surround_preinterleave_check.cpp -o surround_preinterleave_check

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
constexpr unsigned REPS = 9;
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

struct StereoWav { std::vector<int32_t> L, R; };
StereoWav loadWavStereo(const std::string &path) {
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
  StereoWav sw; sw.L.resize(nFrames); sw.R.resize(nFrames);
  const unsigned char* p=data.data();
  for (unsigned i=0;i<nFrames;i++){
    int32_t l=(int32_t)(p[0]|(p[1]<<8)|(p[2]<<16));
    if (l&0x800000) l|=0xFF000000;
    sw.L[i]=l;
    if (channels>1) {
      const unsigned char* pr=p+bps_;
      int32_t r=(int32_t)(pr[0]|(pr[1]<<8)|(pr[2]<<16));
      if (r&0x800000) r|=0xFF000000;
      sw.R[i]=r;
    } else sw.R[i]=l;
    p+=bps_*channels;
  }
  return sw;
}

inline int bytesNeededSigned(int32_t minV, int32_t maxV) {
  if (minV>=-128 && maxV<=127) return 1;
  if (minV>=-32768 && maxV<=32767) return 2;
  if (minV>=-8388608 && maxV<=8388607) return 3;
  return 4;
}

// ---- format A: today's independent-per-lane (v3 baseline) ----
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

// ---- format B: compress-time pre-interleaved, PER-LANE INDEPENDENT width
// kept (no ratio cost) - the "surround set" format. Layout per block:
// [8 width bytes][sample0: lane0's w0 bytes, lane1's w1 bytes, ..., lane7's
// w7 bytes][sample1: same layout]...
struct PreInterleaved { std::vector<uint8_t> bytes; std::vector<std::array<uint8_t,8>> blockWidths; };

PreInterleaved encodePreInterleaved(const std::vector<std::vector<int32_t>>& residuals, unsigned nSamples) {
  PreInterleaved out;
  for (unsigned i=0;i<nSamples;i+=BLOCK_SAMPLES) {
    unsigned n=std::min((unsigned)BLOCK_SAMPLES,nSamples-i);
    std::array<uint8_t,8> w{};
    for (int v=0;v<LANES;v++) {
      int32_t minV=residuals[v][i],maxV=residuals[v][i];
      for (unsigned j=0;j<n;j++){ minV=std::min(minV,residuals[v][i+j]); maxV=std::max(maxV,residuals[v][i+j]); }
      w[v]=(uint8_t)bytesNeededSigned(minV,maxV);
    }
    out.blockWidths.push_back(w);
    for (int v=0;v<LANES;v++) out.bytes.push_back(w[v]); // header
    unsigned recordBytes=0; for (int v=0;v<LANES;v++) recordBytes+=w[v];
    size_t base=out.bytes.size();
    out.bytes.resize(base+(size_t)n*recordBytes);
    uint8_t* rec=out.bytes.data()+base;
    for (unsigned j=0;j<n;j++) {
      uint8_t* p=rec+(size_t)j*recordBytes;
      for (int v=0;v<LANES;v++) {
        uint32_t val=(uint32_t)residuals[v][i+j];
        for (int b=0;b<w[v];b++) *p++=(uint8_t)(val>>(8*b));
      }
    }
  }
  return out;
}

} // namespace

int main(int argc, char** argv) {
  constexpr int PERSPECTIVES = LANES/2;
  if (argc < 1+PERSPECTIVES) { printf("need %d stereo wav files (fixed surround group, same pipe/note, different mic perspectives)\n", PERSPECTIVES); return 1; }
  std::vector<std::vector<int32_t>> voices(LANES);
  unsigned nSamples=UINT32_MAX;
  for (int pv=0;pv<PERSPECTIVES;pv++) {
    StereoWav sw = loadWavStereo(argv[pv+1]);
    voices[pv*2]=sw.L; voices[pv*2+1]=sw.R;
    nSamples=std::min(nSamples,(unsigned)sw.L.size());
  }
  printf("%u samples, %d lanes (%d real mic perspectives, same pipe/note)\n", nSamples, LANES, PERSPECTIVES);

  std::vector<std::vector<int32_t>> residuals(LANES, std::vector<int32_t>(nSamples));
  for (int v=0;v<LANES;v++) {
    int32_t value=0,prev=0,last=0;
    for (unsigned i=0;i<nSamples;i++){ last=prev; prev=value; value=voices[v][i]; int32_t pred=prev+((prev-last)>>1); residuals[v][i]=value-pred; }
  }

  // --- v3 baseline: independent per-lane, decode-time planar+transpose ---
  std::vector<EncodedLane> lanes(LANES);
  size_t indepBytes=0;
  for (int v=0;v<LANES;v++) { lanes[v]=encodeLaneIndependent(residuals[v],nSamples); indepBytes+=lanes[v].bytes.size(); }

  std::vector<int32_t> flatV3(nSamples*LANES);
  std::vector<int32_t> planar((size_t)LANES*BLOCK_SAMPLES);
  auto decodeV3 = [&] {
    std::vector<const uint8_t*> p(LANES);
    for (int v=0;v<LANES;v++) p[v]=lanes[v].bytes.data();
    __m256i value=_mm256_setzero_si256(), prev=_mm256_setzero_si256(), last=_mm256_setzero_si256();
    unsigned i=0, blk=0;
    while (i<nSamples) {
      unsigned n=std::min((unsigned)BLOCK_SAMPLES,nSamples-i);
      int width[LANES];
      for (int v=0;v<LANES;v++) width[v]=lanes[v].blockWidths[blk];
      for (int v=0;v<LANES;v++) {
        int32_t* row=planar.data()+(size_t)v*BLOCK_SAMPLES;
        switch(width[v]) {
          case 1: extractLanePlanar<1>(p[v],n,row); break;
          case 2: extractLanePlanar<2>(p[v],n,row); break;
          case 3: extractLanePlanar<3>(p[v],n,row); break;
          default: extractLanePlanar<4>(p[v],n,row); break;
        }
      }
      unsigned m8=n-(n%8);
      for (unsigned j=0;j<m8;j+=8) {
        __m256i rows[8];
        for (int v=0;v<LANES;v++) rows[v]=_mm256_loadu_si256((const __m256i*)(planar.data()+(size_t)v*BLOCK_SAMPLES+j));
        transpose8x8(rows);
        for (int k=0;k<8;k++) {
          last=prev; prev=value;
          __m256i diff=_mm256_sub_epi32(prev,last), half=_mm256_srai_epi32(diff,1), pred=_mm256_add_epi32(prev,half);
          value=_mm256_add_epi32(pred,rows[k]);
          alignas(32) int32_t outv[8]; _mm256_store_si256((__m256i*)outv,value);
          for (int v=0;v<LANES;v++) flatV3[(size_t)(i+j+k)*LANES+v]=outv[v];
        }
      }
      for (unsigned j=m8;j<n;j++) {
        alignas(32) int32_t tmp[LANES];
        for (int v=0;v<LANES;v++) tmp[v]=planar[(size_t)v*BLOCK_SAMPLES+j];
        __m256i residual=_mm256_load_si256((const __m256i*)tmp);
        last=prev; prev=value;
        __m256i diff=_mm256_sub_epi32(prev,last), half=_mm256_srai_epi32(diff,1), pred=_mm256_add_epi32(prev,half);
        value=_mm256_add_epi32(pred,residual);
        alignas(32) int32_t outv[8]; _mm256_store_si256((__m256i*)outv,value);
        for (int v=0;v<LANES;v++) flatV3[(size_t)(i+j)*LANES+v]=outv[v];
      }
      for (int v=0;v<LANES;v++) p[v]+=(size_t)n*width[v];
      i+=n; blk++;
    }
  };

  // --- surround format: compress-time interleaved, decode straight through ---
  PreInterleaved pre = encodePreInterleaved(residuals, nSamples);
  printf("independent-per-lane packed: %zu B | pre-interleaved packed: %zu B (should match - same widths, just reordered)\n",
    indepBytes, pre.bytes.size());

  std::vector<int32_t> flatPre(nSamples*LANES);
  auto decodePre = [&] {
    const uint8_t* p = pre.bytes.data();
    __m256i value=_mm256_setzero_si256(), prev=_mm256_setzero_si256(), last=_mm256_setzero_si256();
    unsigned i=0, blk=0;
    while (i<nSamples) {
      unsigned n=std::min((unsigned)BLOCK_SAMPLES,nSamples-i);
      const auto& w = pre.blockWidths[blk];
      p += 8; // skip header (already known via blockWidths, avoids re-reading)
      for (unsigned j=0;j<n;j++) {
        alignas(32) int32_t tmp[LANES];
        for (int v=0;v<LANES;v++) {
          int wv=w[v];
          if (wv==1) tmp[v]=(int32_t)(int8_t)p[0];
          else if (wv==2) { int16_t vv; memcpy(&vv,p,2); tmp[v]=vv; }
          else if (wv==3) { uint32_t raw=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16); tmp[v]=(int32_t)(raw<<8)>>8; }
          else { memcpy(&tmp[v],p,4); }
          p += wv;
        }
        __m256i residual=_mm256_load_si256((const __m256i*)tmp);
        last=prev; prev=value;
        __m256i diff=_mm256_sub_epi32(prev,last), half=_mm256_srai_epi32(diff,1), pred=_mm256_add_epi32(prev,half);
        value=_mm256_add_epi32(pred,residual);
        alignas(32) int32_t outv[8]; _mm256_store_si256((__m256i*)outv,value);
        for (int v=0;v<LANES;v++) flatPre[(size_t)(i+j)*LANES+v]=outv[v];
      }
      i+=n; blk++;
    }
  };

  decodeV3();
  decodePre();
  bool okV3=true, okPre=true;
  for (unsigned i=0;i<nSamples && (okV3||okPre); i++)
    for (int v=0;v<LANES;v++) {
      if (flatV3[(size_t)i*LANES+v]!=voices[v][i]) okV3=false;
      if (flatPre[(size_t)i*LANES+v]!=voices[v][i]) okPre=false;
    }
  printf("roundtrip: v3 %s, pre-interleaved %s\n", okV3?"OK":"MISMATCH!", okPre?"OK":"MISMATCH!");

  double secV3 = bestSeconds(REPS, decodeV3);
  double secPre = bestSeconds(REPS, decodePre);
  double nsV3 = secV3/nSamples/LANES*1e9;
  double nsPre = secPre/nSamples/LANES*1e9;

  printf("\nv3 (independent per-lane, decode-time planar+transpose): %.4f ns/channel\n", nsV3);
  printf("surround (compress-time interleaved, direct decode):     %.4f ns/channel  (%.2fx)\n", nsPre, nsV3/nsPre);

  return 0;
}
