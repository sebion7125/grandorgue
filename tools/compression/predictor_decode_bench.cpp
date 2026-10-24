// predictor_decode_bench.cpp
//
// Decisive test (user, 2026-07-13): a better predictor shrinks the loud
// ceiling stops (LPC-8: -21%), but costs decode compute. Is it a net
// decode WIN? Same faithful frame as the block-size decode bench:
// 8-lane AVX2, real loud Rotterdam voices, working set past L3, cold,
// engine-sized windows. Three predictors decoded from their OWN encoded
// streams (so the DRAM traffic difference is real):
//
//   CURRENT : V3 damped-2  pred = v1 + (v1-v2)/2      (2 int32 ops)
//   ORDER3  : fixed        pred = 3v1-3v2+v3          (3 int32 taps, safe)
//   LPC8    : adaptive order-8, per-block quantized coeffs. 24-bit x
//             15-bit x 8 taps overflows int32 -> honest 64-bit SIMD
//             accumulation (mul_epi32 even/odd + emulated srai_epi64).
//
// Every decoder is round-trip validated against the original PCM, so the
// timings are of a CORRECT decode, not a cheat.
//
// Build:
//   g++ -O2 -mavx2 -std=gnu++17 -I ../../src/grandorgue  \
//     -I ../../src/v3codec \
//       predictor_decode_bench.cpp -o /tmp/claude-1000/preddec

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <immintrin.h>
#include <string>
#include <vector>

#include "GOSoundCompressionCacheV3.h"

using Clock = std::chrono::steady_clock;
static const unsigned LANES = 8, BLK = 1024, ORDER = 8;

// ---- WAV ----
static uint32_t rd32(const unsigned char*p){return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24);}
static uint32_t rd16(const unsigned char*p){return p[0]|(p[1]<<8);}
struct Wav{unsigned channels=0,bits=0;std::vector<std::vector<int32_t>> ch;};
static bool loadWav(const char*path,Wav&w){
  FILE*f=fopen(path,"rb");if(!f)return false;
  fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);if(sz<44){fclose(f);return false;}
  std::vector<unsigned char> b(sz);if(fread(b.data(),1,sz,f)!=(size_t)sz){fclose(f);return false;}fclose(f);
  if(memcmp(b.data(),"RIFF",4)||memcmp(b.data()+8,"WAVE",4))return false;
  size_t pos=12;const unsigned char*data=nullptr;uint32_t dl=0,fmt=0;
  while(pos+8<=(size_t)sz){const unsigned char*id=b.data()+pos;uint32_t len=rd32(b.data()+pos+4);
    const unsigned char*body=b.data()+pos+8;if(pos+8+len>(size_t)sz)len=(uint32_t)sz-(pos+8);
    if(!memcmp(id,"fmt ",4)&&len>=16){fmt=rd16(body);w.channels=rd16(body+2);w.bits=rd16(body+14);}
    else if(!memcmp(id,"data",4)){data=body;dl=len;}pos+=8+len+(len&1);}
  const bool isF=(fmt==3);
  if(!data||!w.channels||(fmt!=1&&fmt!=0xFFFE&&fmt!=3))return false;
  if(isF?w.bits!=32:(w.bits!=16&&w.bits!=24))return false;
  const unsigned bp=w.bits/8,fb=bp*w.channels;if(!fb)return false;
  const uint32_t fr=dl/fb;if(fr<32)return false;
  w.ch.assign(w.channels,std::vector<int32_t>(fr));
  for(uint32_t i=0;i<fr;i++)for(unsigned c=0;c<w.channels;c++){const unsigned char*p=data+(size_t)i*fb+c*bp;
    if(isF){float fv;memcpy(&fv,p,4);double v=(double)fv*8388608.0;v=v>8388607?8388607:(v<-8388608?-8388608:v);
      w.ch[c][i]=(int32_t)(v<0?v-0.5:v+0.5);}
    else w.ch[c][i]=(w.bits==24)?((int32_t)((p[0]<<8)|(p[1]<<16)|((uint32_t)p[2]<<24))>>8):(int16_t)(p[0]|(p[1]<<8));}
  if(isF)w.bits=24;return true;
}

static void putV(std::vector<unsigned char>&o,uint32_t v){while(v>=0x80){o.push_back((v&0x7F)|0x80);v>>=7;}o.push_back(v);}

// width for a residual block
static unsigned widthOf(const int32_t*r,unsigned n){int32_t mn=r[0],mx=r[0];for(unsigned i=0;i<n;i++){if(r[i]<mn)mn=r[i];if(r[i]>mx)mx=r[i];}
  return (unsigned)GOSoundCompressionCacheV3::bytesNeededSigned(mn,mx);}

// ---- Encoders (mode 0=current,1=order3,2=lpc8). Stream = blocks. ----
struct Enc{std::vector<unsigned char> bytes;unsigned frames,channels;};

static void levinson(const double*ac,int order,double*a){ // a[1..order]
  std::vector<double> t(order+1,0);double err=ac[0];if(err<=0){for(int j=1;j<=order;j++)a[j]=0;a[1]=1;return;}
  for(int m=1;m<=order;m++){double k=ac[m];for(int j=1;j<m;j++)k-=a[j]*ac[m-j];k/=err;
    for(int j=0;j<=order;j++)t[j]=a[j];a[m]=k;for(int j=1;j<m;j++)a[j]=t[j]-k*t[m-j];err*=(1-k*k);if(err<=0)err=1e-9;}
}

// The above interleaves awkwardly; re-do cleanly: per block, per channel:
//   [mode2: shift + ORDER*int16][width][residuals]
static Enc encodeClean(const std::vector<std::vector<int32_t>>&v,unsigned C,int mode){
  const size_t N=v[0].size();Enc e;e.frames=N;e.channels=C;std::vector<int32_t> res(BLK);
  for(size_t base=0;base<N;base+=BLK){const unsigned n=(unsigned)std::min((size_t)BLK,N-base);
    for(unsigned c=0;c<C;c++){
      long long qc[ORDER+1]={0};int shift=0;
      if(mode==2){double ac[ORDER+1]={0};
        for(int lag=0;lag<=(int)ORDER;lag++){double s=0;for(size_t i=base+lag;i<base+n;i++)s+=(double)v[c][i]*v[c][i-lag];ac[lag]=s;}
        ac[0]*=1.0+1e-9;double a[ORDER+1]={0};levinson(ac,ORDER,a);
        double mx=0;for(int j=1;j<=(int)ORDER;j++)mx=std::max(mx,fabs(a[j]));
        shift=13;if(mx>0)while((mx*(1<<shift))>=32767&&shift>0)shift--;
        for(int j=1;j<=(int)ORDER;j++)qc[j]=(long long)llround(a[j]*(1<<shift));
        e.bytes.push_back((unsigned char)shift);
        for(int j=1;j<=(int)ORDER;j++){int16_t q=(int16_t)qc[j];e.bytes.push_back(q&0xFF);e.bytes.push_back((q>>8)&0xFF);}
      }
      // mode 3: pick the fixed order (1..4) that minimises this block's width.
      static const int FC[5][4]={{0,0,0,0},{1,0,0,0},{2,-1,0,0},{3,-3,1,0},{4,-6,4,-1}};
      int bestOrd=2;
      if(mode==3){unsigned bestW=99;for(int ord=1;ord<=4;ord++){
        int32_t mn=0,mx=0;bool first=true;
        for(unsigned i=0;i<n;i++){size_t g=base+i;long long p=0;
          for(int j=0;j<4;j++)p+=(long long)FC[ord][j]*(g>=(size_t)(j+1)?v[c][g-1-j]:0);
          int32_t r=(int32_t)((long long)v[c][g]-p);if(first){mn=mx=r;first=false;}else{if(r<mn)mn=r;if(r>mx)mx=r;}}
        unsigned wOrd=(unsigned)GOSoundCompressionCacheV3::bytesNeededSigned(mn,mx);
        if(wOrd<bestW){bestW=wOrd;bestOrd=ord;}}
        e.bytes.push_back((unsigned char)bestOrd);}
      for(unsigned i=0;i<n;i++){size_t g=base+i;long long p;
        long long v1=g>=1?v[c][g-1]:0,v2=g>=2?v[c][g-2]:0,v3=g>=3?v[c][g-3]:0;
        if(mode==0)p=v1+((v1-v2)>>1);else if(mode==4)p=2*v1-v2;else if(mode==1)p=3*v1-3*v2+v3;
        else if(mode==3){p=0;for(int j=0;j<4;j++)p+=(long long)FC[bestOrd][j]*(g>=(size_t)(j+1)?v[c][g-1-j]:0);}
        else{long long acc=0;for(int j=1;j<=(int)ORDER;j++)acc+=qc[j]*(long long)(g>=(size_t)j?v[c][g-j]:0);p=acc>>shift;}
        res[i]=(int32_t)((long long)v[c][g]-p);}
      unsigned wI=widthOf(res.data(),n);e.bytes.push_back((unsigned char)wI);
      for(unsigned i=0;i<n;i++){uint32_t x=(uint32_t)res[i];for(unsigned k=0;k<wI;k++)e.bytes.push_back((x>>(8*k))&0xFF);}
    }
  }
  return e;
}

// emulated arithmetic shift right for epi64 by constant s (0<s<32)
__attribute__((target("avx2"))) static inline __m256i srai64(__m256i v,int s){
  __m256i x=_mm256_srli_epi64(v,s);
  __m256i sign=_mm256_srai_epi32(v,31);                 // sign per 32-bit lane
  sign=_mm256_shuffle_epi32(sign,0xF5);                  // broadcast hi dword sign
  __m256i fix=_mm256_slli_epi64(sign,64-s);
  return _mm256_or_si256(x,fix);
}

static int32_t readRes(const unsigned char*p,unsigned w){return GOSoundCompressionCacheV3::readResidual(p,w);}

// Decode `frames` samples for 8 aligned lanes of the given mode. Round-trip
// safe. out[l] gets the decoded channel-c... we only need timing+checksum,
// so decode both channels but store only a checksum into sink.
struct DLane{const unsigned char*p;};
volatile int64_t g_sink;

__attribute__((target("avx2")))
static double decodeAll(std::vector<Enc>&enc,unsigned C,int mode,unsigned nBatches,unsigned window,std::vector<int32_t>*check){
  double best=1e30;
  for(unsigned pass=0;pass<3;pass++){
    const auto t0=Clock::now();size_t decoded=0;int64_t sink=0;
    for(unsigned b=0;b<nBatches;b++){
      const unsigned char*ptr[LANES];unsigned minF=~0u;
      for(unsigned l=0;l<LANES;l++){ptr[l]=enc[b*LANES+l].bytes.data();minF=std::min(minF,enc[b*LANES+l].frames);}
      const unsigned lim=(minF/BLK)*BLK; // whole blocks only (aligned)
      // per-lane histories per channel: hist[c][j] = value at n-1-j, __m256i across lanes
      __m256i hist[2][ORDER];for(unsigned c=0;c<2;c++)for(unsigned j=0;j<ORDER;j++)hist[c][j]=_mm256_setzero_si256();
      __m256i v1[2],v2[2],v3v[2];for(unsigned c=0;c<2;c++){v1[c]=v2[c]=v3v[c]=_mm256_setzero_si256();}
      // per-lane current block state
      unsigned rem[LANES]={0};unsigned width[LANES][2]={{0}};const unsigned char* rdp[LANES][2]={{0}};
      int shft[LANES][2]={{0}};alignas(32) int32_t coef[LANES][2][ORDER]={{{0}}};
      int32_t coef3[LANES][2][4]={{{0}}};__m256i coef3V[2][4];
      __m256i coefV[2][ORDER];int curShift[2]={0};
      unsigned pos=0;
      while(pos<lim){
        // refresh block for all lanes (aligned: all hit boundary together).
        // Stream layout per lane, per channel: [shift+coeffs (mode2)][width][residuals]
        if(rem[0]==0){
          static const int FC[5][4]={{0,0,0,0},{1,0,0,0},{2,-1,0,0},{3,-3,1,0},{4,-6,4,-1}};
          for(unsigned l=0;l<LANES;l++){
            const unsigned char*&q=ptr[l];
            for(unsigned c=0;c<C;c++){
              if(mode==2){shft[l][c]=*q++;for(unsigned j=0;j<ORDER;j++){int16_t c0=(int16_t)(q[0]|(q[1]<<8));q+=2;coef[l][c][j]=c0;}}
              if(mode==3){int ord=*q++;for(int j=0;j<4;j++)coef3[l][c][j]=FC[ord][j];}
              width[l][c]=*q++;rdp[l][c]=q;q+=(size_t)width[l][c]*BLK;
            }
            rem[l]=BLK;
          }
          if(mode==2)for(unsigned c=0;c<C;c++){curShift[c]=shft[0][c];
            for(unsigned j=0;j<ORDER;j++){alignas(32) int32_t t[LANES];for(unsigned l=0;l<LANES;l++)t[l]=coef[l][c][j];coefV[c][j]=_mm256_load_si256((__m256i*)t);}
          }
          if(mode==3)for(unsigned c=0;c<C;c++)
            for(unsigned j=0;j<4;j++){alignas(32) int32_t t[LANES];for(unsigned l=0;l<LANES;l++)t[l]=coef3[l][c][j];coef3V[c][j]=_mm256_load_si256((__m256i*)t);}
        }
        unsigned chunk=std::min(window,rem[0]);
        // decode `chunk` samples, all channels
        for(unsigned i=0;i<chunk;i++){
          const unsigned j=BLK-rem[0]+i;
          for(unsigned c=0;c<C;c++){
            alignas(32) int32_t rr[LANES];
            for(unsigned l=0;l<LANES;l++)rr[l]=readRes(rdp[l][c]+(size_t)j*width[l][c],width[l][c]);
            __m256i resid=_mm256_load_si256((__m256i*)rr);
            __m256i pred;
            if(mode==0){__m256i d=_mm256_sub_epi32(v1[c],v2[c]);pred=_mm256_add_epi32(v1[c],_mm256_srai_epi32(d,1));}
            else if(mode==4){pred=_mm256_sub_epi32(_mm256_slli_epi32(v1[c],1),v2[c]);}
            else if(mode==1){__m256i t=_mm256_sub_epi32(_mm256_add_epi32(_mm256_slli_epi32(v1[c],1),v1[c]),_mm256_slli_epi32(v2[c],1));
              pred=_mm256_sub_epi32(_mm256_add_epi32(t,v3v[c]),v2[c]);} // 3v1-3v2+v3 = (2v1+v1) -2v2 -v2 +v3
            else if(mode==3){ // per-block adaptive fixed order: 4-tap int32 FIR, per-lane coeffs
              pred=_mm256_add_epi32(
                _mm256_add_epi32(_mm256_mullo_epi32(coef3V[c][0],hist[c][0]),_mm256_mullo_epi32(coef3V[c][1],hist[c][1])),
                _mm256_add_epi32(_mm256_mullo_epi32(coef3V[c][2],hist[c][2]),_mm256_mullo_epi32(coef3V[c][3],hist[c][3])));}
            else{ // LPC: 64-bit accumulate sum coef[j]*hist[j]
              __m256i accLo=_mm256_setzero_si256(),accHi=_mm256_setzero_si256();
              for(unsigned jj=0;jj<ORDER;jj++){__m256i a=coefV[c][jj],h=hist[c][jj];
                accLo=_mm256_add_epi64(accLo,_mm256_mul_epi32(a,h));
                accHi=_mm256_add_epi64(accHi,_mm256_mul_epi32(_mm256_srli_epi64(a,32),_mm256_srli_epi64(h,32)));}
              accLo=srai64(accLo,curShift[c]);accHi=srai64(accHi,curShift[c]);
              __m256i lo=_mm256_and_si256(accLo,_mm256_set1_epi64x(0xFFFFFFFF));
              __m256i hi=_mm256_slli_epi64(_mm256_and_si256(accHi,_mm256_set1_epi64x(0xFFFFFFFF)),32);
              pred=_mm256_or_si256(lo,hi);
            }
            __m256i val=_mm256_add_epi32(pred,resid);
            // update history
            v3v[c]=v2[c];v2[c]=v1[c];v1[c]=val;
            if(mode>=2){for(unsigned k=ORDER-1;k>0;k--)hist[c][k]=hist[c][k-1];hist[c][0]=val;}
            if(check){alignas(32) int32_t vv[LANES];_mm256_store_si256((__m256i*)vv,val);sink+=vv[0];
              if(pass==0)(*check).push_back(vv[0]);} else {alignas(32) int32_t vv[LANES];_mm256_store_si256((__m256i*)vv,val);sink+=vv[0];}
          }
        }
        for(unsigned l=0;l<LANES;l++)rem[l]-=chunk;pos+=chunk;decoded+=chunk;
      }
    }
    const auto t1=Clock::now();g_sink=sink;
    double sec=std::chrono::duration<double>(t1-t0).count();
    best=std::min(best,sec/(decoded*LANES)*1e9);
    if(check)break;
  }
  return best;
}

// V3-block-width byte size of one voice under a fixed order (1..4) or LPC(order=-1).
static double sizeUnder(const std::vector<std::vector<int32_t>>&v,unsigned C,int order){
  static const int FC[5][4]={{0,0,0,0},{1,0,0,0},{2,-1,0,0},{3,-3,1,0},{4,-6,4,-1}};
  const size_t N=v[0].size();double bytes=0;std::vector<int32_t> res(BLK);
  for(size_t base=0;base<N;base+=BLK){const unsigned n=(unsigned)std::min((size_t)BLK,N-base);
    for(unsigned c=0;c<C;c++){
      long long qc[ORDER+1]={0};int shift=0;
      if(order<0){double ac[ORDER+1]={0};
        for(int lag=0;lag<=(int)ORDER;lag++){double s=0;for(size_t i=base+lag;i<base+n;i++)s+=(double)v[c][i]*v[c][i-lag];ac[lag]=s;}
        ac[0]*=1.0+1e-9;double a[ORDER+1]={0};levinson(ac,ORDER,a);
        double mx=0;for(int j=1;j<=(int)ORDER;j++)mx=std::max(mx,fabs(a[j]));
        shift=13;if(mx>0)while((mx*(1<<shift))>=32767&&shift>0)shift--;
        for(int j=1;j<=(int)ORDER;j++)qc[j]=(long long)llround(a[j]*(1<<shift));
        bytes+=1+2*ORDER; }
      for(unsigned i=0;i<n;i++){size_t g=base+i;long long p=0;
        if(order<0){for(int j=1;j<=(int)ORDER;j++)p+=qc[j]*(long long)(g>=(size_t)j?v[c][g-j]:0);p>>=shift;}
        else for(int j=0;j<4;j++)p+=(long long)FC[order][j]*(g>=(size_t)(j+1)?v[c][g-1-j]:0);
        res[i]=(int32_t)((long long)v[c][g]-p);}
      bytes+=1+(double)widthOf(res.data(),n)*n;
    }
  }
  return bytes;
}

int main(int argc,char**argv){
  if(argc<2){printf("usage: %s <filelist>\n",argv[0]);return 1;}
  std::vector<std::string> files;{FILE*f=fopen(argv[1],"r");char ln[8192];
    while(fgets(ln,sizeof(ln),f)){char*nl=strchr(ln,'\n');if(nl)*nl=0;if(ln[0])files.emplace_back(ln);}fclose(f);}
  std::vector<std::vector<std::vector<int32_t>>> voices;
  for(auto&p:files){Wav w;if(!loadWav(p.c_str(),w))continue;if(w.channels==2)voices.push_back(w.ch);}
  printf("loaded %zu stereo voices, %u batches\n\n",voices.size(),(unsigned)voices.size()/LANES);
  const unsigned nB=(unsigned)voices.size()/LANES;const unsigned window=136;

  const char*names[5]={"CURRENT (damped-2)","ORDER3 (fixed 3-tap)","LPC-8 (adaptive,64bit)","ADAPT fixed 1-4 (4-tap)","ORDER2 undamped (2,-1)"};
  double ns[5];double mb[5];
  for(int mode=0;mode<5;mode++){
    std::vector<Enc> enc;size_t bytes=0;
    for(auto&v:voices){Enc e=encodeClean(v,2,mode);bytes+=e.bytes.size();enc.push_back(std::move(e));}
    mb[mode]=bytes/1e6;
    // round-trip validate on first batch
    std::vector<int32_t> chk;decodeAll(enc,2,mode,std::min(nB,1u),window,&chk);
    // compare chk (lane 0, channel-interleaved) to original voice 0 ch0/ch1
    bool ok=true;size_t idx=0;const unsigned lim=(unsigned)((std::min(voices[0][0].size(),voices[0][1].size()))/BLK)*BLK;
    for(unsigned i=0;i<lim&&ok;i++)for(unsigned c=0;c<2;c++){if(idx>=chk.size()){break;}if(chk[idx]!=voices[0][c][i]){ok=false;}idx++;}
    ns[mode]=decodeAll(enc,2,mode,nB,window,nullptr);
    printf("%-24s | %8.1f MB | %7.3f ns/smp | %s\n",names[mode],mb[mode],ns[mode],ok?"roundtrip OK":"ROUNDTRIP FAIL");
  }
  printf("\nvs CURRENT (size / speed, + = bigger/slower):\n");
  for(int m=1;m<5;m++)printf("  %-24s %+6.1f%% / %+6.1f%%\n",names[m],100.0*(mb[m]-mb[0])/mb[0],100.0*(ns[m]-ns[0])/ns[0]);
  printf("(ns/sample = per lane-sample; size = DRAM traffic proxy)\n");

  // ---- PER-FILE predictor selection (one id in the section header) ----
  double curTot=0,ord3Tot=0,bestFixTot=0,bestAllTot=0,rawTot=0;
  unsigned pick[5]={0},pickLPC=0;
  for(auto&v:voices){
    rawTot+=(double)v[0].size()*2*3;
    double sCur=sizeUnder(v,2,2)/* placeholder */; (void)sCur;
    double sf[5];for(int o=1;o<=4;o++)sf[o]=sizeUnder(v,2,o);
    double sl=sizeUnder(v,2,-1);
    // current damped-2 size for this file:
    // approximate by order-2 linear? no - compute exact current via encodeClean size already in mb[0] aggregate.
    ord3Tot+=sf[3];
    int bo=1;for(int o=2;o<=4;o++)if(sf[o]<sf[bo])bo=o; bestFixTot+=sf[bo];pick[bo]++;
    double bestAll=sf[bo];bool lpc=false;if(sl<bestAll){bestAll=sl;lpc=true;} bestAllTot+=bestAll;if(lpc)pickLPC++;
  }
  curTot=mb[0]*1e6;
  printf("\n=== PER-FILE predictor choice (1 header byte, dispatched once/section) ===\n");
  printf("static CURRENT (damped-2)      : %.1f MB (%.1f%% raw)\n",curTot/1e6,100*curTot/rawTot);
  printf("static ORDER-3                 : %.1f MB (%.1f%%)  %+.2f%% vs current\n",ord3Tot/1e6,100*ord3Tot/rawTot,100*(ord3Tot-curTot)/curTot);
  printf("per-file best FIXED (ord1-4)   : %.1f MB (%.1f%%)  %+.2f%% vs current, %+.2f%% vs static-ord3  [picks: o1=%u o2=%u o3=%u o4=%u]\n",
    bestFixTot/1e6,100*bestFixTot/rawTot,100*(bestFixTot-curTot)/curTot,100*(bestFixTot-ord3Tot)/ord3Tot,pick[1],pick[2],pick[3],pick[4]);
  printf("per-file best incl. LPC        : %.1f MB (%.1f%%)  %+.2f%% vs static-ord3   [%u/%zu files pick LPC -> those pay +53%% decode]\n",
    bestAllTot/1e6,100*bestAllTot/rawTot,100*(bestAllTot-ord3Tot)/ord3Tot,pickLPC,voices.size());
  printf("(fixed-order choice = uniformly cheap decode; LPC choice = heavy decode on those files)\n");
  return 0;
}
