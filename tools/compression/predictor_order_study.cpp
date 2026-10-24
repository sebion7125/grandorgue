// predictor_order_study.cpp
//
// Beyond-the-plate question (user, 2026-07-13): V3 uses a damped 2nd-
// order predictor  pred = v[-1] + (v[-1]-v[-2])/2  (= 1.5 v[-1] - 0.5
// v[-2], NOT the full linear 2 v[-1] - v[-2]). Are higher orders worth
// it? Measured on real Rotterdam samples.
//
// For each predictor we compute the residual stream and its cost two
// ways: LEGACY variable-length bytes (the real per-sample codec cost, a
// clean magnitude proxy) and mean bits = mean of ceil(log2(|r|+1))+1.
// Lower = better prediction = smaller file.
//
// Predictors:
//   ord0..ord4   fixed polynomial (Shorten/FLAC fixed modes)
//   V3           the shipping damped-2nd-order
//   LS2          per-file least-squares optimal [a,b] (float, exact)
//   LPC-k        per-file Levinson-Durbin, FLAC-style quantized integer
//                coefficients (k = 4, 8, 12) - adaptive higher order
//
// Build:
//   g++ -O2 -std=gnu++17 -I ../../src/grandorgue predictor_order_study.cpp \
//       -o /tmp/claude-1000/predord

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static uint32_t rd32(const unsigned char *p){return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24);}
static uint32_t rd16(const unsigned char *p){return p[0]|(p[1]<<8);}
struct Wav { unsigned channels=0,bits=0; std::vector<std::vector<int32_t>> ch; };

static bool loadWav(const char *path, Wav &w){
  FILE*f=fopen(path,"rb"); if(!f)return false;
  fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
  if(sz<44){fclose(f);return false;}
  std::vector<unsigned char> b(sz);
  if(fread(b.data(),1,sz,f)!=(size_t)sz){fclose(f);return false;} fclose(f);
  if(memcmp(b.data(),"RIFF",4)||memcmp(b.data()+8,"WAVE",4))return false;
  size_t pos=12; const unsigned char*data=nullptr; uint32_t dl=0,fmt=0;
  while(pos+8<=(size_t)sz){const unsigned char*id=b.data()+pos;uint32_t len=rd32(b.data()+pos+4);
    const unsigned char*body=b.data()+pos+8; if(pos+8+len>(size_t)sz)len=(uint32_t)sz-(pos+8);
    if(!memcmp(id,"fmt ",4)&&len>=16){fmt=rd16(body);w.channels=rd16(body+2);w.bits=rd16(body+14);}
    else if(!memcmp(id,"data",4)){data=body;dl=len;} pos+=8+len+(len&1);}
  const bool isF=(fmt==3);
  if(!data||!w.channels||(fmt!=1&&fmt!=0xFFFE&&fmt!=3))return false;
  if(isF?w.bits!=32:(w.bits!=16&&w.bits!=24))return false;
  const unsigned bp=w.bits/8,fb=bp*w.channels; if(!fb)return false;
  const uint32_t fr=dl/fb; if(fr<8)return false;
  w.ch.assign(w.channels,std::vector<int32_t>(fr));
  for(uint32_t i=0;i<fr;i++)for(unsigned c=0;c<w.channels;c++){
    const unsigned char*p=data+(size_t)i*fb+c*bp;
    if(isF){float fv;memcpy(&fv,p,4);double v=(double)fv*8388608.0;
      v=v>8388607?8388607:(v<-8388608?-8388608:v); w.ch[c][i]=(int32_t)(v<0?v-0.5:v+0.5);}
    else w.ch[c][i]=(w.bits==24)?((int32_t)((p[0]<<8)|(p[1]<<16)|((uint32_t)p[2]<<24))>>8)
      :(int16_t)(p[0]|(p[1]<<8));}
  if(isF)w.bits=24; return true;
}

static inline unsigned legacyBytes(long long enc){
  long long a = enc<0?-enc:enc;
  if(a<=16383)return 2; if(a<=2097151)return 3; return 4; // 24-bit format16
}
static inline double bitsOf(long long r){ unsigned long long a=r<0?-r:r; unsigned b=1; while(a){b++;a>>=1;} return b; }

// Accumulate cost of a residual into totals.
struct Cost { double bytes=0, bits=0; size_t n=0;
  void add(long long r){ bytes+=legacyBytes(r); bits+=bitsOf(r); n++; } };

// ---- fixed predictors: return residual for sample i given history ----
// v is one channel. i>=order guaranteed by caller.
static void fixedResiduals(const std::vector<int32_t>&v, int which, Cost&c){
  const size_t N=v.size();
  for(size_t i=0;i<N;i++){
    long long p=0;
    long long v1=i>=1?v[i-1]:0, v2=i>=2?v[i-2]:0, v3=i>=3?v[i-3]:0, v4=i>=4?v[i-4]:0;
    switch(which){
      case 0: p=0; break;                          // order 0
      case 1: p=v1; break;                          // order 1
      case 100: p=v1+((v1-v2)>>1); break;           // V3 damped 2nd
      case 2: p=2*v1-v2; break;                      // order 2 (linear)
      case 3: p=3*v1-3*v2+v3; break;                 // order 3
      case 4: p=4*v1-6*v2+4*v3-v4; break;            // order 4
    }
    c.add((long long)v[i]-p);
  }
}

// ---- least-squares optimal 2nd order: pred = a*v1 + b*v2 ----
static void ls2Residuals(const std::vector<int32_t>&v, Cost&c){
  // Solve 2x2 normal equations over the file.
  double s11=0,s12=0,s22=0,sy1=0,sy2=0;
  for(size_t i=2;i<v.size();i++){
    double v1=v[i-1],v2=v[i-2],y=v[i];
    s11+=v1*v1; s12+=v1*v2; s22+=v2*v2; sy1+=y*v1; sy2+=y*v2;
  }
  double det=s11*s22-s12*s12; double a=1.5,b=-0.5;
  if(fabs(det)>1e-6){ a=(sy1*s22-sy2*s12)/det; b=(s11*sy2-s12*sy1)/det; }
  for(size_t i=0;i<v.size();i++){
    double v1=i>=1?v[i-1]:0, v2=i>=2?v[i-2]:0;
    long long p=(long long)llround(a*v1+b*v2);
    c.add((long long)v[i]-p);
  }
}

// ---- LPC order k via Levinson-Durbin, FLAC-style quantized integer ----
static void lpcResiduals(const std::vector<int32_t>&v, int order, Cost&c){
  const size_t N=v.size();
  // autocorrelation 0..order (windowed lightly for stability)
  std::vector<double> r(order+1,0.0);
  for(int lag=0;lag<=order;lag++){ double s=0; for(size_t i=lag;i<N;i++) s+=(double)v[i]*v[i-lag]; r[lag]=s; }
  if(r[0]<=0){ fixedResiduals(v,1,c); return; }
  // Levinson-Durbin
  std::vector<double> a(order+1,0.0), tmp(order+1,0.0); double err=r[0];
  for(int m=1;m<=order;m++){
    double k=r[m]; for(int j=1;j<m;j++) k-=a[j]*r[m-j]; k/=err;
    tmp=a; a[m]=k; for(int j=1;j<m;j++) a[j]=tmp[j]-k*tmp[m-j];
    err*=(1-k*k); if(err<=0){err=1e-9;}
  }
  // predictor coeffs: pred = sum_{j=1..order} a[j]*v[i-j]
  // quantize (FLAC-style): precision ~14 bits
  double maxc=0; for(int j=1;j<=order;j++) maxc=std::max(maxc,fabs(a[j]));
  int shift=14; if(maxc>0){ while((maxc*(1<<shift))>=(1<<15) && shift>0) shift--; }
  std::vector<long long> q(order+1,0);
  for(int j=1;j<=order;j++) q[j]=llround(a[j]*(1<<shift));
  for(size_t i=0;i<N;i++){
    long long p=0;
    if(i>=(size_t)order){ for(int j=1;j<=order;j++) p+=q[j]*(long long)v[i-j]; p>>=shift; }
    else { p=i>=1?v[i-1]:0; }
    c.add((long long)v[i]-p);
  }
}

int main(int argc,char**argv){
  if(argc<2){printf("usage: %s <filelist>\n",argv[0]);return 1;}
  std::vector<std::string> files;
  { FILE*f=fopen(argv[1],"r"); char ln[8192];
    while(fgets(ln,sizeof(ln),f)){char*nl=strchr(ln,'\n');if(nl)*nl=0;if(ln[0])files.emplace_back(ln);} fclose(f);}

  struct Pred{const char*name;int kind;int param;}; // kind:0 fixed,1 ls2,2 lpc
  Pred preds[]={
    {"order 0 (pred=0)",0,0},{"order 1 (v[-1])",0,1},
    {"V3 damped-2 (1.5,-0.5)",0,100},{"order 2 linear (2,-1)",0,2},
    {"order 3",0,3},{"order 4",0,4},
    {"LS-optimal order 2",1,0},
    {"LPC-4 (adaptive)",2,4},{"LPC-8 (adaptive)",2,8},{"LPC-12 (adaptive)",2,12},
  };
  const int NP=sizeof(preds)/sizeof(preds[0]);
  std::vector<Cost> tot(NP);
  double rawBytes=0; size_t voices=0;

  for(auto&path:files){
    Wav w; if(!loadWav(path.c_str(),w))continue; voices++;
    rawBytes+=(double)w.ch[0].size()*w.channels*3;
    for(unsigned c=0;c<w.channels;c++)
      for(int pi=0;pi<NP;pi++){
        Cost&C=tot[pi];
        if(preds[pi].kind==0) fixedResiduals(w.ch[c],preds[pi].param,C);
        else if(preds[pi].kind==1) ls2Residuals(w.ch[c],C);
        else lpcResiduals(w.ch[c],preds[pi].param,C);
      }
  }
  printf("predictor study over %zu voices, %.2f GB raw 24-bit\n\n",voices,rawBytes/1e9);
  printf("%-26s | %10s | %8s | %9s\n","predictor","legacy%","mean bits","vs V3");
  printf("%s\n",std::string(64,'-').c_str());
  double v3bytes=0; for(int pi=0;pi<NP;pi++) if(preds[pi].param==100&&preds[pi].kind==0) v3bytes=tot[pi].bytes;
  for(int pi=0;pi<NP;pi++){
    Cost&C=tot[pi];
    printf("%-26s | %9.2f%% | %8.3f | %+8.2f%%%s\n",
      preds[pi].name, 100.0*C.bytes/rawBytes, C.bits/C.n,
      100.0*(C.bytes-v3bytes)/v3bytes,
      (preds[pi].kind==0&&preds[pi].param==100)?"  <- shipping":"");
  }
  printf("\nlegacy%% = variable-length bytes as %% of raw; lower=better prediction\n");
  printf("(fixed orders: ~same decode cost as V3; LPC: order-k MACs/sample + coeff storage)\n");
  return 0;
}
