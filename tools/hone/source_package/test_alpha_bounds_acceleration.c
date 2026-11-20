/* Public synthetic experiment only; no runtime admission or device timing.
 * Bounds are built AFTER packet finite/range validation and belong to the same
 * immutable snapshot revision. All potentially covered samples retain the
 * original arithmetic, masks, depth and tie ordering. */
#include "alpha_raster_candidate.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
typedef struct {uint8_t x0,x1,y0,y1;} bounds;
static bounds prepare_bounds(const hone_alpha_packet *p){
 float x0=p->v[0].x,x1=x0,y0=p->v[0].y,y1=y0;
 for(unsigned i=1;i<3;i++){
  x0=fminf(x0,p->v[i].x);x1=fmaxf(x1,p->v[i].x);
  y0=fminf(y0,p->v[i].y);y1=fmaxf(y1,p->v[i].y);
 }
 /* Inclusive extrema deliberately keep top-left boundary decisions in sample.
  * Empty bounds use inverted interval. Clamp before integer conversion. */
 if(x1<0||x0>239||y1<0||y0>134)return (bounds){1,0,1,0};
 return (bounds){(uint8_t)ceilf(fmaxf(0,x0)),(uint8_t)floorf(fminf(239,x1)),
                 (uint8_t)ceilf(fmaxf(0,y0)),(uint8_t)floorf(fminf(134,y1))};
}
static uint64_t samples,reference_samples;
static int accelerated(const hone_alpha_snapshot*s,const bounds*b,unsigned x,unsigned y,float opaque,hone_alpha_fragment f[4]){
 unsigned n=0;
 for(size_t i=0;i<s->count;i++){
  if(x<b[i].x0||x>b[i].x1||y<b[i].y0||y>b[i].y1)continue;
  samples++;
  hone_alpha_fragment q;if(!hone_alpha_sample(s->packets+i,x,y,opaque,&q))continue;
  if(n==4)return -1;
  unsigned at=n;while(at&&(f[at-1].z>q.z||(f[at-1].z==q.z&&f[at-1].order<=q.order))){f[at]=f[at-1];at--;}
  f[at]=q;n++;
 }
 return (int)n;
}
static uint32_t rng=0x719a321u;
static float rnd(void){rng=rng*1664525u+1013904223u;return (float)(rng>>8)/16777216.f;}
int main(void){
 hone_alpha_packet p[32];bounds b[32];
 hone_alpha_snapshot s={.packets=p,.count=32,.capacity=32};
 unsigned overflow=0,hit=0;
 for(unsigned scene=0;scene<12;scene++){
  for(unsigned i=0;i<32;i++){
   float x=-20+280*rnd(),y=-20+175*rnd(),w=1+24*rnd(),h=1+24*rnd();
   p[i]=(hone_alpha_packet){.v={{x,y,1,0},{x+w,y,2,1},{x,y+h,3,2}},.rgb={.2f,.5f,.8f},.alpha=.4f,.mask_enabled=i%2,.mask_low=.5f,.mask_high=1.5f,.order=i%3};
   if(scene==1){p[i].v[0].x=10;p[i].v[0].y=10;p[i].v[1].x=50;p[i].v[1].y=10;p[i].v[2].x=10;p[i].v[2].y=50;}
   if(scene==2){hone_alpha_vertex t=p[i].v[1];p[i].v[1]=p[i].v[2];p[i].v[2]=t;}
   if(scene==3)p[i].alpha=0;
   if(scene==4)p[i].v[2]=p[i].v[1];
   if(scene==5){p[i].v[0].x=-1000000;p[i].v[1].x=1000000;}
   if(scene==6){p[i].v[0].x=10.1f;p[i].v[1].x=10.2f;p[i].v[2].x=10.3f;}
   if(scene==7)for(unsigned j=0;j<3;j++){p[i].v[j].x+=500;p[i].v[j].y-=500;}
   if(scene==8){p[i].v[0].x=0;p[i].v[0].y=0;p[i].v[1].x=240;p[i].v[1].y=0;p[i].v[2].x=0;p[i].v[2].y=135;}
   if(scene==9){p[i].v[0].x=239;p[i].v[0].y=134;p[i].v[1].x=239;p[i].v[1].y=-1;p[i].v[2].x=-1;p[i].v[2].y=134;}
   b[i]=prepare_bounds(p+i);
  }
  for(unsigned mode=0;mode<2;mode++)for(unsigned y=0;y<135;y++)for(unsigned x=0;x<240;x++){
   float opaque=mode?((x+y)%3==0?2.f:4.f):-INFINITY;
   hone_alpha_fragment a[4],c[4];
   int n=hone_alpha_fragments(&s,x,y,opaque,a),m=accelerated(&s,b,x,y,opaque,c);
   assert(n==m);
   if(n<0)overflow++;
   else if(n){assert(!memcmp(a,c,(size_t)n*sizeof *a));hit++;}
   /* Reconstruct actual reference visit count, including its early overflow.
    * Diagnostic work only, excluded from candidate sample_calls. */
   if(n>=0)reference_samples+=s.count;
   else {unsigned found=0;for(size_t i=0;i<s.count;i++){
    hone_alpha_fragment q;reference_samples++;
    if(hone_alpha_sample(s.packets+i,x,y,opaque,&q)&&++found==5)break;
   }}
  }
 }
 assert(overflow&&hit);
 printf("PASS scenes=12 pixels=777600 overflow=%u hit=%u bounds_bytes=%zu sample_calls=%llu reference_sample_calls=%llu\n",overflow,hit,sizeof b,(unsigned long long)samples,(unsigned long long)reference_samples);
 return 0;
}
