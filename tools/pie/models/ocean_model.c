/* Exhaustive proof that the ocean kernel's arithmetic equals ocean_row_scalar
 * (main/scene/ocean.c) for every input: 256 swell indices x 256 ripple indices x
 * 161 |x-160| x 98 rows = 1,034,027,008 pixels. Models each PIE lane operation
 * with its TRM 1.8 semantics (saturating adds, 32-bit product >> SAR kept to
 * 16 bits) and the folded constants the kernel uses (sine*16, (sine/6)*16-2880,
 * ceil(262144/span), floor(swell/32)+8). Run it again whenever the formula, a
 * table or a constant of ocean_row_pie changes. Takes a few seconds.
 * Build: see run_models.py. */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
/* Lane models with the exact TRM semantics (1.8.70/198 saturate, 1.8.122/128 32-bit product >> SAR, low 16) */
static int16_t sat(int v){return v>32767?32767:v<-32768?-32768:v;}
static int16_t adds(int16_t a,int16_t b){return sat(a+b);}
static int16_t subs(int16_t a,int16_t b){return sat(a-b);}
static int16_t muls(int16_t a,int16_t b,int sar){return (int16_t)(((int32_t)a*(int32_t)b)>>sar);}
static uint16_t mulu(uint16_t a,uint16_t b,int sar){return (uint16_t)(((uint32_t)a*(uint32_t)b)>>sar);}
static int16_t relu(int16_t a){return a<=0?0:a;}
static unsigned clamp(int v){return v<0?0:v>255?255:(unsigned)v;}
static uint16_t rgb(unsigned r,unsigned g,unsigned b){return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
int main(){
  int16_t sine[256]; int32_t A[256],B[256];
  for(int i=0;i<256;i++){sine[i]=(int16_t)(sinf(i*6.2831853f/256)*256); A[i]=sine[i]*16; B[i]=(sine[i]/6)*16-2880;}
  long mism=0; int maxd[3]={0,0,0}; long total=0;
  for(int y=37;y<135;y++){
    int span=12+(y-36)/3, haze=24-(y-36)/5;
    int16_t m=(int16_t)((262144+span-1)/span);
    for(int i1=0;i1<256;i1++)for(int i2=0;i2<256;i2++)for(int dx=0;dx<=160;dx++){
      /* scalar reference */
      int swell=sine[i1],ripple=sine[i2];
      int crest=swell-180+ripple/6;if(crest<0)crest=0;
      int reflection=dx<span?(span-dx)*128/span:0;
      int glint=crest*(40+reflection)/128;
      int shade=(swell+256)/32; int lift=shade+haze+glint;
      uint16_t ref=rgb(clamp(3+glint),clamp(20+lift),clamp(39+lift));
      int R=clamp(3+glint),G=clamp(20+lift),Bc=clamp(39+lift);
      /* vector model, SAR=11 */
      int16_t q3=(int16_t)A[i1], q4=(int16_t)B[i2];
      q4=adds(q4,q3); q4=relu(q4);            /* crest16 */
      int16_t q1=subs((int16_t)span,(int16_t)dx); q1=relu(q1);   /* d */
      q1=muls(q1,m,11); q1=adds(q1,40);                          /* 40+refl */
      q1=muls(q4,q1,11);                                         /* glint */
      q3=muls(q3,4,11);                        /* shade */
      q3=adds(q3,q1);
      int16_t r=adds(q1,3), g=adds(q3,(int16_t)(haze+28)), b=adds(q3,(int16_t)(haze+47));
      if(r!=R||g!=G||b!=Bc){ int d;
        d=abs(r-R);if(d>maxd[0])maxd[0]=d; d=abs(g-G);if(d>maxd[1])maxd[1]=d; d=abs(b-Bc);if(d>maxd[2])maxd[2]=d; }
      uint16_t pr=mulu(mulu((uint16_t)(r&0xF8),32768,11),32768,11);
      uint16_t pg=mulu((uint16_t)(g&0xFC),16384,11);
      uint16_t pb=mulu((uint16_t)b,256,11);
      uint16_t out=pr|pg|pb;
      total++; if(out!=ref){ if(mism<5)printf("y=%d i1=%d i2=%d dx=%d ref=%04x out=%04x rgb=%d,%d,%d vs %d,%d,%d\n",y,i1,i2,dx,ref,out,R,G,Bc,r,g,b); mism++; }
    }
  }
  printf("total=%ld mismatches=%ld max channel diff r=%d g=%d b=%d\n",total,mism,maxd[0],maxd[1],maxd[2]);
  /* bounds */
  int mx[3]={0,0,0};
  for(int i1=0;i1<256;i1++)for(int i2=0;i2<256;i2++){int swell=sine[i1],ripple=sine[i2];int crest=swell-180+ripple/6;if(crest<0)crest=0;
    int glint=crest*168/128;int shade=(swell+256)/32;int lift=shade+24+glint; if(3+glint>mx[0])mx[0]=3+glint; if(20+lift>mx[1])mx[1]=20+lift; if(39+lift>mx[2])mx[2]=39+lift;}
  printf("max r=%d g=%d b=%d (clamp never triggers if all <=255)\n",mx[0],mx[1],mx[2]);
  return 0;
}
