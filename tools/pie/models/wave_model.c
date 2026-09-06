/* Exhaustive proof that the wave kernel's arithmetic equals wave_row_scalar
 * (main/shell.c): all 135 rows x all three |y-ribbon| in 0..70, both signs
 * (386,543,880 pixels). Models the lookup table layout light | (weighted<<16),
 * the min(d,64) clamp and the SAR=11 multiply pack. Build: see run_models.py. */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
/* Lane models with TRM semantics: VADDS/VSUBS saturate (1.8.70/198), VMAX/VMIN.S16 (1.8.130/1.8.141),
   VMUL.U16 = 32-bit product >> SAR logical, low 16 (1.8.128). */
static int16_t sat(int v){return v>32767?32767:v<-32768?-32768:v;}
static int16_t adds(int16_t a,int16_t b){return sat(a+b);}
static int16_t subs(int16_t a,int16_t b){return sat(a-b);}
static int16_t vmax(int16_t a,int16_t b){return a>b?a:b;}
static int16_t vmin(int16_t a,int16_t b){return a<b?a:b;}
static uint16_t mulu(uint16_t a,uint16_t b,int sar){return (uint16_t)(((uint32_t)a*(uint32_t)b)>>sar);}
static uint16_t rgb(unsigned r,unsigned g,unsigned b){return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
int main(){
  uint8_t softness[3][64]; uint32_t lut[3][65];
  const float widths[]={18,5,24};const float brightness[]={14,32,21};
  for(int l=0;l<3;l++)for(int d=0;d<64;d++)softness[l][d]=(uint8_t)(brightness[l]*expf(-d*d/(2*widths[l]*widths[l])));
  for(int l=0;l<3;l++){for(int d=0;d<64;d++){unsigned lo=softness[l][d],hi=l==0?lo/4:l==1?lo/3:lo/2; lut[l][d]=lo|(hi<<16);} lut[l][64]=0;}
  long total=0,mism=0;
  for(int y=0;y<135;y++){
    unsigned green=14+y/7, blue=30+y/5;
    for(int s0=-1;s0<=1;s0+=2)for(int s1=-1;s1<=1;s1+=2)for(int s2=-1;s2<=1;s2+=2)
    for(int d0=0;d0<=70;d0++)for(int d1=0;d1<=70;d1++)for(int d2=0;d2<=70;d2++){
      int16_t rib[3]={(int16_t)(y+s0*d0),(int16_t)(y+s1*d1),(int16_t)(y+s2*d2)};
      /* scalar reference */
      unsigned light[3];
      for(int l=0;l<3;l++){int d=y-rib[l];if(d<0)d=-d;light[l]=d<64?softness[l][d]:0;}
      unsigned sum=light[0]+light[1];
      uint16_t ref=rgb(5+light[0]/4+light[1]/3+light[2]/2,green+sum+light[2]/2,blue+sum+light[2]);
      /* vector model */
      int16_t lo[3],hi[3];
      for(int l=0;l<3;l++){
        int16_t q0=rib[l], q2=(int16_t)y;
        int16_t q1=subs(q2,q0); q0=subs(q0,q2); q0=vmax(q0,q1); q0=vmin(q0,64);
        uint32_t e=lut[l][q0]; lo[l]=(int16_t)(e&0xFFFF); hi[l]=(int16_t)(e>>16);
      }
      int16_t acclo=adds(lo[0],lo[1]), acchi=adds(hi[0],hi[1]);
      int16_t b=adds(adds(acclo,lo[2]),(int16_t)blue);
      int16_t g=adds(adds(acclo,hi[2]),(int16_t)green);
      int16_t r=adds(adds(acchi,hi[2]),5);
      uint16_t pr=mulu(mulu((uint16_t)(r&0xF8),32768,11),32768,11);
      uint16_t pg=mulu((uint16_t)(g&0xFC),16384,11);
      uint16_t pb=mulu((uint16_t)b,256,11);
      uint16_t out=pr|pg|pb;
      total++; if(out!=ref){if(mism<5)printf("y=%d rib=%d,%d,%d ref=%04x out=%04x\n",y,rib[0],rib[1],rib[2],ref,out);mism++;}
    }
  }
  printf("total=%ld mismatches=%ld\n",total,mism);
  return 0;
}
