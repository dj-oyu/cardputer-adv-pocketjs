/* Exact band-entry phase oracle over int16 coordinates and valid panel bands.
 * Includes the implementation to compare its private arithmetic with the
 * pinned pre-change int64 formula. No owner/buffer/lifetime changes. */

#include "ksn_procedural.c"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
static inline __attribute__((always_inline)) int reference_entry(const ksn_proc_segment *s,int x0,int y,int height,
                                                            int *x,int *yy,int *err){
    const int dx=abs(s->x1-x0),dy=abs(s->y1-s->y0),sx=x0<s->x1?1:-1,sy=s->y0<s->y1?1:-1;
    const int row=sy>0?y-s->y0:s->y0-(y+height-1);
    if(!dy||row<=0)return 0;
    int major,minor,x_steps,y_steps;
    if(dx>=dy){
        int64_t numerator=(int64_t)row*dx-(dx-1)/2;
        major=numerator>0?(int)((numerator+dy-1)/dy):0;
        minor=(int)(((int64_t)major*dy+(dx-1)/2)/dx);
        x_steps=major;y_steps=minor;
    }else{
        major=row;
        minor=(int)(((int64_t)major*dx+(dy-1)/2)/dy);
        x_steps=minor;y_steps=major;
    }
    *x+=sx*x_steps;*yy+=sy*y_steps;
    *err=(int)((int64_t)dx-dy-(int64_t)x_steps*dy+(int64_t)y_steps*dx);
    return major;
}
static unsigned long long count,max_numerator;
static void check(ksn_proc_segment s,int x0,int y,int h){
 int lo=s.y0<s.y1?s.y0:s.y1,hi=s.y0>s.y1?s.y0:s.y1;
 if(hi<y||lo>=y+h)return;
 int dx=abs(s.x1-x0),dy=abs(s.y1-s.y0),x=x0,yy=s.y0,e=dx-dy;
 int rx=x,ry=yy,re=e;
 int k=band_entry(&s,x0,y,h,&x,&yy,&e),r=reference_entry(&s,x0,y,h,&rx,&ry,&re);
 if(k!=r||x!=rx||yy!=ry||e!=re){fprintf(stderr,"mismatch %d %d %d %d band%d/%d\n",x0,s.y0,s.x1,s.y1,y,h);abort();}
 int row=s.y0<s.y1?y-s.y0:s.y0-(y+h-1);
 if(dy&&row>0){
  assert(row<=32768+KSN_PROC_H);assert(dx<=65535&&dy<=65535);assert(row<=dy);
  uint64_t n;
  if(dx>=dy){n=(uint64_t)row*dx-(dx-1)/2+dy-1;assert(n<=UINT32_MAX);if(n>max_numerator)max_numerator=n;
   n=(uint64_t)k*dy+(dx-1)/2;assert(n<=UINT32_MAX);if(n>max_numerator)max_numerator=n;
  }else{n=(uint64_t)row*dx+(dy-1)/2;assert(n<=UINT32_MAX);if(n>max_numerator)max_numerator=n;}
 }
 count++;
}
static uint32_t rng=0x73a19b45;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
int main(void){
 const int xs[]={-32768,-32767,-480,-1,0,1,239,240,720,2047,2048,28671,32766,32767};
 const int ys[]={-32768,-32767,-600,-1,0,1,7,8,134,135,136,600,32766,32767};
 for(unsigned a=0;a<sizeof(xs)/sizeof(*xs);a++)for(unsigned b=0;b<sizeof(xs)/sizeof(*xs);b++)
 for(unsigned c=0;c<sizeof(ys)/sizeof(*ys);c++)for(unsigned d=0;d<sizeof(ys)/sizeof(*ys);d++)
 for(int y=0;y<=KSN_PROC_H;y++){
  int hh[]={0,1,8,KSN_PROC_H-y};
  for(int h=0;h<4;h++)if(hh[h]<=KSN_PROC_H-y)
   check((ksn_proc_segment){xs[a],ys[c],xs[b],ys[d],1},xs[a],y,hh[h]);
 }
 for(unsigned i=0;i<2000000;i++){
  ksn_proc_segment s={(int16_t)next(),(int16_t)next(),(int16_t)next(),(int16_t)next(),1};
  int y=next()%(KSN_PROC_H+1),h=next()%(KSN_PROC_H-y+1);check(s,s.x0,y,h);
  /* Encoded pattern coordinates, using the actual bias-removal range. */
  s.x0=KSN_PROC_EXT_MIN+next()%(32768-KSN_PROC_EXT_MIN);check(s,s.x0-KSN_PROC_EXT_PATTERN,y,h);
 }
 printf("band-entry u32: %llu cases max numerator %llu PASS\n",count,max_numerator);
}
