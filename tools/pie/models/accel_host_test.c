/* Host test of the whole accelerator entry points in main/scene/render_accel.c
 * (accel_fill / accel_blend, including the scalar head and tail of every row and
 * the pointer arithmetic), compiled with -DRENDER_ACCEL_HOST_MODEL so the inline
 * assembly is replaced by the C lane model inside render_accel.c. 200,000 random
 * rectangles, masks and colours against a plain reference implementation.
 * Needs -Istub for a dependency-free pocketjs/render_rgb565.h. Build: see run_models.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../main/scene/render_accel.c"
static uint16_t ref_blend(uint16_t p, unsigned r, unsigned g, unsigned b, unsigned a){
  if(a==0)return p; if(a>=255)return pack565(r,g,b);
  unsigned r5=(p>>11)&31,g6=(p>>5)&63,b5=p&31; unsigned dr=(r5<<3)|(r5>>2),dg=(g6<<2)|(g6>>4),db=(b5<<3)|(b5>>2),ia=255-a;
  return pack565((r*a+dr*ia+127)/255,(g*a+dg*ia+127)/255,(b*a+db*ia+127)/255);
}
int main(){
  enum{W=240,H=8};
  static uint16_t dst[W*H] __attribute__((aligned(16))), ref[W*H];
  static uint8_t mask[W*H] __attribute__((aligned(16)));
  srand(1); long bad=0, calls=0;
  for(int it=0;it<200000;it++){
    for(int i=0;i<W*H;i++){dst[i]=(uint16_t)rand(); int m=rand()%6; mask[i]=m==0?0:m==1?255:(uint8_t)rand();}
    memcpy(ref,dst,sizeof dst);
    pocketjs_rgb565_rect_t r; r.x=rand()%W; r.y=rand()%H; r.width=1+rand()%(W-r.x); r.height=1+rand()%(H-r.y);
    if(rand()%4==0){r.x=0;r.width=W;}
    unsigned cr=rand()&255,cg=rand()&255,cb=rand()&255; uint16_t col=(uint16_t)rand();
    bool ok;
    if(it&1){ ok=accel_blend(NULL,dst,W*H,W,H,mask,W*H,r,cr,cg,cb,255);
      for(uint32_t y=r.y;y<r.y+r.height;y++)for(uint32_t x=r.x;x<r.x+r.width;x++)ref[y*W+x]=ref_blend(ref[y*W+x],cr,cg,cb,mask[y*W+x]);
    } else { ok=accel_fill(NULL,dst,W*H,W,H,r,col);
      for(uint32_t y=r.y;y<r.y+r.height;y++)for(uint32_t x=r.x;x<r.x+r.width;x++)ref[y*W+x]=col; }
    calls++; if(!ok){printf("returned false at it=%d\n",it);bad++;continue;}
    if(memcmp(dst,ref,sizeof dst)){bad++; if(bad<5)printf("mismatch it=%d rect=%u,%u %ux%u blend=%d\n",it,r.x,r.y,r.width,r.height,it&1);}
  }
  printf("calls=%ld mismatches=%ld\n",calls,bad);
  return bad!=0;
}
