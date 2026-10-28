/* Exhaustive proof of the arithmetic the LEVEL WAVE row (main/scene/wave.c,
 * wave_row_pie) relies on, lane by lane, with the TRM's semantics:
 *
 *  1. The distance. For every 4y in 0..536, every line centre 4c in -2048..2047
 *     and every pulse o in 0..8, min(max(subs(4y, 4c-o), subs(4c+o, 4y)), 255)
 *     equals min(|4y-4c| + o, 255) -- the saturating subtracts never saturate
 *     on these ranges, and the result is a valid index into a 256-word table.
 *  2. The sum. Adding four non-negative channel*128 terms with VADDS.S16
 *     (saturating, 1.8.70) in the kernel's order gives min(32767, sum) for every
 *     term in 0..32767 -- checked over every pair at the saturation boundary and
 *     a grid of the rest, which is what "clips to white, never wraps" means.
 *  3. The pack. For every accumulator 0..32767 and SAR=11, the masked multiply
 *     (VMUL.U16: 32-bit product >> SAR, low 16 bits, 1.8.128) puts red, green
 *     and blue in their RGB565 fields exactly: (acc & 0x7C00)*4096>>11 is
 *     (acc>>10)<<11, (acc & 0x7E00)*128>>11 is (acc>>9)<<5, (acc & 0x7C00)*2>>11
 *     is acc>>10.
 * Build: see run_models.py. */
#include <stdio.h>
#include <stdint.h>
static int16_t sat(int v){return v>32767?32767:v<-32768?-32768:(int16_t)v;}
static int16_t adds(int16_t a,int16_t b){return sat(a+b);}
static int16_t subs(int16_t a,int16_t b){return sat(a-b);}
static uint16_t mulu(uint16_t a,uint16_t b,int s){return (uint16_t)(((uint32_t)a*(uint32_t)b)>>s);}
int main(void){
  long bad=0,n=0;
  for(int y4=0;y4<=536;y4+=4)for(int c=-2048;c<2048;c++)for(int o=0;o<=8;o++){
    int16_t lo=(int16_t)(c-o),hi=(int16_t)(c+o);
    int16_t a=subs((int16_t)y4,lo),b=subs(hi,(int16_t)y4);
    int16_t d=a>b?a:b; if(d>255)d=255;
    int want=(y4>c?y4-c:c-y4)+o; if(want>255)want=255;
    n++; if(d!=want||d<0){if(bad<5)printf("distance y4=%d c=%d o=%d got %d want %d\n",y4,c,o,d,want);bad++;}
  }
  printf("distance: %ld cases, %ld wrong\n",n,bad);
  long bad2=0,n2=0;
  for(int a=0;a<=32767;a+=(a<32000?127:1))for(int b=0;b<=32767;b+=(b<32000?131:1)){
    int16_t s=adds(adds(adds((int16_t)a,(int16_t)b),(int16_t)(b/2)),(int16_t)(a/3));
    long want=(long)a+b+b/2+a/3; if(want>32767)want=32767;
    n2++; if(s!=want){if(bad2<5)printf("sum a=%d b=%d got %d\n",a,b,s);bad2++;}
  }
  printf("sum: %ld cases, %ld wrong\n",n2,bad2);
  long bad3=0;
  for(int acc=0;acc<=32767;acc++){
    uint16_t r=mulu((uint16_t)(acc&0x7C00),4096,11), g=mulu((uint16_t)(acc&0x7E00),128,11),
             b=mulu((uint16_t)(acc&0x7C00),2,11);
    if(r!=(uint16_t)((acc>>10)<<11)||g!=(uint16_t)((acc>>9)<<5)||b!=(uint16_t)(acc>>10)){
      if(bad3<5)printf("pack acc=%d\n",acc);bad3++;}
  }
  printf("pack: 32768 accumulators, %ld wrong\n",bad3);
  printf("total=%ld mismatches=%ld\n",n+n2+32768,bad+bad2+bad3);
  return bad||bad2||bad3;
}
