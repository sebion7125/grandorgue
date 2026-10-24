// predictor_coeffs_study.cpp
//
// Question (user, 2026-07-13): on top of the fixed undamped-2 (2,-1) now
// shipping, what would OPTIMIZED coefficients add - either per-file, or a
// single set tuned for organ samples as a class? Measured in V3's real
// block-width format (not the optimistic per-sample entropy metric).
//
// Compares, as % of raw 24-bit, the V3-block residual size under:
//   fixed (2,-1)      : shipping - shift+sub, no multiply, cheapest decode
//   fixed order-3     : 3v1-3v2+v3 - small int taps, cheap
//   GLOBAL-opt 2/3    : ONE least-squares coeff set fit across ALL voices
//                       (an "organ-tuned" fixed predictor)
//   per-file opt 2/3  : least-squares coeffs fit per voice (adaptive)
// Optimized (non-integer) coeffs would need real multiplies + 64-bit
// accumulation to decode (like LPC); fixed ones do not - flagged below.
//
// Build:
//   g++ -O2 -std=gnu++17 -I ../../src/grandorgue  \
//     -I ../../src/v3codec predictor_coeffs_study.cpp \
//       -o /tmp/claude-1000/coeffs

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GOSoundCompressionCacheV3.h"

static const unsigned BLK = 1024;

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

// Solve small SPD-ish linear system R a = r (n<=3) by Gaussian elimination.
static void solve(double R[3][3],double r[3],double a[3],int n){
  double M[3][4];for(int i=0;i<n;i++){for(int j=0;j<n;j++)M[i][j]=R[i][j];M[i][n]=r[i];}
  for(int col=0;col<n;col++){
    int piv=col;for(int row=col+1;row<n;row++)if(fabs(M[row][col])>fabs(M[piv][col]))piv=row;
    for(int j=0;j<=n;j++)std::swap(M[col][j],M[piv][j]);
    if(fabs(M[col][col])<1e-12){a[col]=0;continue;}
    for(int row=0;row<n;row++)if(row!=col){double f=M[row][col]/M[col][col];for(int j=0;j<=n;j++)M[row][j]-=f*M[col][j];}
  }
  for(int i=0;i<n;i++)a[i]=fabs(M[i][i])<1e-12?0:M[i][n]/M[i][i];
}

// accumulate normal equations for order n (uses v[i-1..i-n] to predict v[i])
static void accumNormal(const std::vector<int32_t>&v,int n,double R[3][3],double r[3]){
  const size_t N=v.size();
  for(size_t i=n;i<N;i++){
    double x[3];for(int j=0;j<n;j++)x[j]=v[i-1-j];double y=v[i];
    for(int a=0;a<n;a++){for(int b=0;b<n;b++)R[a][b]+=x[a]*x[b];r[a]+=y*x[a];}
  }
}

// V3-block byte size of a voice under coeffs a[0..n-1] (pred = sum a[j]*v[i-1-j]).
static double sizeCoef(const std::vector<std::vector<int32_t>>&v,unsigned C,const double*a,int n){
  const size_t N=v[0].size();double bytes=0;std::vector<int32_t> res(BLK);
  for(size_t base=0;base<N;base+=BLK){const unsigned m=(unsigned)std::min((size_t)BLK,N-base);
    for(unsigned c=0;c<C;c++){
      for(unsigned i=0;i<m;i++){size_t g=base+i;double p=0;for(int j=0;j<n;j++)p+=a[j]*(g>=(size_t)(j+1)?v[c][g-1-j]:0);
        res[i]=(int32_t)llround((double)v[c][g]-p);}
      int32_t mn=res[0],mx=res[0];for(unsigned i=0;i<m;i++){if(res[i]<mn)mn=res[i];if(res[i]>mx)mx=res[i];}
      bytes+=1+(double)GOSoundCompressionCacheV3::bytesNeededSigned(mn,mx)*m;
    }
  }
  return bytes;
}

int main(int argc,char**argv){
  if(argc<2){printf("usage: %s <filelist>\n",argv[0]);return 1;}
  std::vector<std::string> files;{FILE*f=fopen(argv[1],"r");char ln[8192];
    while(fgets(ln,sizeof(ln),f)){char*nl=strchr(ln,'\n');if(nl)*nl=0;if(ln[0])files.emplace_back(ln);}fclose(f);}
  std::vector<Wav> vs;for(auto&p:files){Wav w;if(loadWav(p.c_str(),w)&&w.channels==2)vs.push_back(std::move(w));}
  printf("loaded %zu voices\n",vs.size());

  // GLOBAL normal equations (order 2 and 3) across all voices/channels.
  double R2[3][3]={{0}},r2[3]={0},R3[3][3]={{0}},r3[3]={0};
  for(auto&w:vs)for(unsigned c=0;c<2;c++){accumNormal(w.ch[c],2,R2,r2);accumNormal(w.ch[c],3,R3,r3);}
  double g2[3],g3[3];solve(R2,r2,g2,2);solve(R3,r3,g3,3);
  printf("\nGLOBAL organ-optimal coeffs:\n");
  printf("  order2: a=%.4f b=%.4f   (fixed (2,-1) for comparison)\n",g2[0],g2[1]);
  printf("  order3: a=%.4f b=%.4f c=%.4f   (fixed (3,-3,1))\n",g3[0],g3[1],g3[2]);

  const double fx2[2]={2,-1}, fx3[3]={3,-3,1};
  double rawT=0,fix2T=0,fix3T=0,g2T=0,g3T=0,pf2T=0,pf3T=0;
  for(auto&w:vs){
    rawT+=(double)w.ch[0].size()*2*3;
    fix2T+=sizeCoef(w.ch,2,fx2,2); fix3T+=sizeCoef(w.ch,2,fx3,3);
    g2T+=sizeCoef(w.ch,2,g2,2);    g3T+=sizeCoef(w.ch,2,g3,3);
    // per-file optimal
    double lR2[3][3]={{0}},lr2[3]={0},lR3[3][3]={{0}},lr3[3]={0};
    for(unsigned c=0;c<2;c++){accumNormal(w.ch[c],2,lR2,lr2);accumNormal(w.ch[c],3,lR3,lr3);}
    double a2[3],a3[3];solve(lR2,lr2,a2,2);solve(lR3,lr3,a3,3);
    pf2T+=sizeCoef(w.ch,2,a2,2); pf3T+=sizeCoef(w.ch,2,a3,3);
  }
  auto pct=[&](double x){return 100.0*x/rawT;};
  auto vsf2=[&](double x){return 100.0*(x-fix2T)/fix2T;};
  printf("\n%-28s %8s %10s   decode\n","predictor","%raw","vs (2,-1)");
  printf("fixed (2,-1)  [shipping]     %7.2f%% %9s   shift+sub (cheapest)\n",pct(fix2T),"--");
  printf("fixed order-3 (3,-3,1)       %7.2f%% %+8.2f%%   small int taps (cheap)\n",pct(fix3T),vsf2(fix3T));
  printf("GLOBAL organ-opt 2-tap       %7.2f%% %+8.2f%%   2 mul + 64bit acc\n",pct(g2T),vsf2(g2T));
  printf("GLOBAL organ-opt 3-tap       %7.2f%% %+8.2f%%   3 mul + 64bit acc\n",pct(g3T),vsf2(g3T));
  printf("per-file opt 2-tap           %7.2f%% %+8.2f%%   2 mul + 64bit acc + coeffs/file\n",pct(pf2T),vsf2(pf2T));
  printf("per-file opt 3-tap           %7.2f%% %+8.2f%%   3 mul + 64bit acc + coeffs/file\n",pct(pf3T),vsf2(pf3T));
  return 0;
}
