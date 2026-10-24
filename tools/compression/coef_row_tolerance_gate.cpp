// Gatekeeper v2: Mini-Tabellen-Qualitaet, sauber aufgebaut.
// Kleine Tabelle wird EXPLIZIT gebaut (dezimierte Zeilen der Produktions-
// tabelle), Lerp interpoliert zwischen ihren echten Nachbarzeilen.
// Referenz: volle 8192er-Tabelle, exakte Phase. Positionen immer exakt.
#include "sound/playing/GOSoundResample.h"
#include <cmath>
#include <cstdio>
#include <vector>
using GR = GOSoundResample;
static const GR g_res;

static float srcSig(long i){
  double p=i*0.017;
  return (float)(0.6*sin(p)+0.25*sin(3.01*p)+0.15*sin(5.02*p)
               +0.08*sin(9.03*p)+0.05*sin(13.05*p));
}
static double rms(const std::vector<float>&v){double s=0;for(float x:v)s+=(double)x*x;return sqrt(s/v.size());}

// kleine Tabelle: 2^bits Zeilen, Zeile i = Produktionszeile i<<(13-bits)
struct SmallTable {
  unsigned bits, n, shift;
  std::vector<float> rows; // [n+1][8], letzte Zeile = wrap auf Zeile 0 der naechsten Periode? Nein:
  // Phase wrappt mod 8192; Nachbar von Zeile n-1 ist Zeile 0 MIT Index+1-Versatz.
  // Fuer den Lerp am oberen Rand nutzen wir gross[(i+1)<<shift mod 8192] - korrekt
  // ist der Filter periodisch in der Phase mit Tap-Shift; wir bauen n+1 Zeilen
  // und Zeile n = gross[0] um EINEN TAP verschoben? Vereinfachung: Zeile n =
  // gross[8191]-nahe Zeile; der Randfehler betrifft 1/n der Frames und wird
  // mitgemessen statt schoengerechnet.
  void build(unsigned b){
    bits=b; n=1u<<b; shift=GR::UPSAMPLE_BITS-b;
    rows.resize((size_t)(n+1)*8);
    for(unsigned i=0;i<n;i++){
      unsigned gi=i<<shift;
      for(unsigned k=0;k<8;k++) rows[(size_t)i*8+k]=g_res.m_PolyphaseCoefs[gi][k];
    }
    /* Obere Nachbarzeile = virtuelle Phase 8192: Polyphase-Identitaet
     * coefs[p+8192][k] = coefs[p][k-1] (Tap-Shift beim Integer-Sprung);
     * k=0 liegt jenseits des Filterendes -> 0 (Lanczos-Fenster). */
    for(unsigned k=0;k<8;k++)
      rows[(size_t)n*8+k] = k>=1 ? g_res.m_PolyphaseCoefs[0][k-1] : 0.0f;
  }
  const float* rowPtr(unsigned i)const{ return &rows[(size_t)i*8]; }
};

static std::vector<float> run(double factor,int mode,const SmallTable*st){
  const unsigned nOut=48000;
  const unsigned incr=(unsigned)(factor*GR::UPSAMPLE_FACTOR+0.5);
  unsigned idx=1000, frac=0; std::vector<float> out(nOut);
  for(unsigned j=0;j<nOut;j++){
    float cbuf[8]; const float*c;
    if(mode==0){ c=g_res.m_PolyphaseCoefs[frac]; }               // exakt
    else if(mode==1){ c=st->rowPtr(frac>>st->shift); }           // nearest klein
    else { unsigned i=frac>>st->shift;                            // lerp klein
      float w=(float)(frac-(i<<st->shift))/(float)(1u<<st->shift);
      const float*a=st->rowPtr(i),*b=st->rowPtr(i+1);
      for(int k=0;k<8;k++) cbuf[k]=a[k]+(b[k]-a[k])*w; c=cbuf; }
    float acc=0; for(int k=0;k<8;k++) acc+=srcSig((long)idx+k)*c[k];
    out[j]=acc;
    frac+=incr; idx+=frac>>GR::UPSAMPLE_BITS; frac&=GR::UPSAMPLE_FACTOR-1;
  }
  return out;
}

int main(){
  const double centsList[]={0.5,2,10,100,700}; // bis Quinte Repitch
  printf("bits |");
  for(double c:centsList) printf("  %5.1fct nearest/lerp |",c);
  printf("\n");
  for(unsigned bits=5;bits<=9;bits++){
    SmallTable st; st.build(bits);
    printf("  %u  |",bits);
    for(double ct:centsList){
      double f=pow(2.0,ct/1200.0);
      auto ref=run(f,0,nullptr), N=run(f,1,&st), L=run(f,2,&st);
      auto err=[&](std::vector<float>&x){ std::vector<float>d(x.size());
        for(size_t i=0;i<x.size();i++)d[i]=x[i]-ref[i];
        return 20*log10(rms(d)/(rms(ref)+1e-30)+1e-30); };
      printf("  %6.1f / %6.1f dB |",err(N),err(L));
    }
    printf("   (Tabelle: %u B)\n",(1u<<bits)*32);
  }
  return 0;
}
