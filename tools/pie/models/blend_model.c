/* Proof of the blend kernel's per-channel arithmetic against the Rust software
 * blend (engine/core/src/raster.rs blend_rgb565_pixel):
 *   - every source s (256) x every 5/6-bit destination value x every alpha (256),
 *     through the min/|diff|/flipped-alpha rewrite and the QACC (x + (x>>8) + 1)>>8
 *     divide-by-255,
 *   - the RGB565 -> 888 bit-replicating unpack for all 65536 pixel values,
 *   - the range over which (x + (x>>8) + 1)>>8 == x/255 holds (must cover 65152).
 * Build: see run_models.py. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
static int16_t sat(int v){return v>32767?32767:v<-32768?-32768:v;}
static int16_t adds(int16_t a,int16_t b){return sat(a+b);}
static int16_t subs(int16_t a,int16_t b){return sat(a-b);}
static int16_t vmax(int16_t a,int16_t b){return a>b?a:b;}
static int16_t vmin(int16_t a,int16_t b){return a<b?a:b;}
static uint16_t mulu(uint16_t a,uint16_t b,int sar){return (uint16_t)(((uint32_t)a*(uint32_t)b)>>sar);}
static uint16_t cmplt(int16_t a,int16_t b){return a<b?0xFFFF:0;}
/* QACC lane: 40-bit unsigned-saturating accumulate of u16*u16 (1.8.163); SRCMB: >>n, saturate s16, QACC keeps shifted (1.8.54) */
static int64_t qacc;
static void mac_u16(uint16_t a,uint16_t b){qacc+= (int64_t)a*b; if(qacc>(1LL<<40)-1)qacc=(1LL<<40)-1;}
static int16_t srcmb(int n){qacc>>=n; return sat((int)qacc);}
/* Rust reference: (s*a + d*(255-a) + 127)/255 on the 888-expanded destination */
static unsigned ref_mix(unsigned s,unsigned d,unsigned a){return (s*a+d*(255-a)+127)/255;}
/* vector model for one channel, given s (0..255), d8 (expanded dst channel), a (0..255) */
static uint16_t vec_mix(uint16_t s,uint16_t d8,uint16_t a){
  int16_t lo=vmin(s,d8), hi=vmax(s,d8), diff=subs(hi,lo);
  uint16_t m=cmplt(s,d8)&0x00FF; uint16_t ap=a^m;       /* a' = s<d ? 255-a : a */
  qacc=0; mac_u16(diff,ap); mac_u16(127,1); srcmb(8);   /* QACC = t = (diff*a'+127)>>8 */
  mac_u16(diff,ap); mac_u16(128,1); int16_t q=srcmb(8); /* q = (x + t + 1)>>8 with x=diff*a'+127 */
  return adds(lo,q);
}
int main(){
  int bad=0;
  /* per-channel exhaustive: s 0..255, dst 5/6-bit value, a 0..255 */
  for(int s=0;s<256;s++)for(int a=0;a<256;a++){
    for(int r5=0;r5<32;r5++){unsigned d8=(r5<<3)|(r5>>2); if(vec_mix(s,d8,a)!=ref_mix(s,d8,a)){bad++;if(bad<5)printf("R s=%d d=%u a=%d\n",s,d8,a);} }
    for(int g6=0;g6<64;g6++){unsigned d8=(g6<<2)|(g6>>4); if(vec_mix(s,d8,a)!=ref_mix(s,d8,a)){bad++;if(bad<5)printf("G s=%d d=%u a=%d\n",s,d8,a);} }
  }
  printf("channel mismatches=%d\n",bad);
  /* unpack model with SAR=11: r5=p*1>>11, g6=(p*64>>11)&63, b5=p&31 ; x8 = x*16384>>11 | x*512>>11 (r,b), g8 = g6*8192>>11 | g6*128>>11 */
  int bad2=0;
  for(unsigned p=0;p<65536;p++){
    uint16_t r5=mulu(p,1,11), g6=mulu(p,64,11)&63, b5=p&31;
    uint16_t r8=mulu(r5,16384,11)|mulu(r5,512,11), g8=mulu(g6,8192,11)|mulu(g6,128,11), b8=mulu(b5,16384,11)|mulu(b5,512,11);
    unsigned R5=(p>>11)&31,G6=(p>>5)&63,B5=p&31;
    if(r8!=((R5<<3)|(R5>>2))||g8!=((G6<<2)|(G6>>4))||b8!=((B5<<3)|(B5>>2)))bad2++;
  }
  printf("unpack mismatches=%d\n",bad2);
  /* identity bound */
  int lim=0; for(int x=0;x<200000;x++){int t=x>>8; if(((x+t+1)>>8)!=x/255){lim=x;break;}} printf("(x+(x>>8)+1)>>8 == x/255 holds for x < %d (need 65153)\n",lim);
  return 0;
}
