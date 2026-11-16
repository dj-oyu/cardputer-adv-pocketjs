/* Host fragment-level span experiment, not production triangle raster/LCD.
 * The immutable sample array represents raster outputs, not a proposed runtime
 * framebuffer-sized input. Actual integration must preflight/replay geometry. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define WIDTH 240
#define HEIGHT 8
#define CAP 54
#define SLOTS 4
typedef struct {float z;uint32_t order;uint16_t rgb;uint8_t alpha,pad;} fragment;
typedef struct {unsigned x,y;fragment f;} sample;
typedef struct {unsigned left,width;} span;
typedef struct {fragment slots[CAP*SLOTS];uint8_t counts[CAP];uint16_t rgb[CAP];} work;
static uint16_t blend(uint16_t s,uint16_t d,unsigned a){unsigned b=255-a;
 return (uint16_t)(((((s>>11)*a+(d>>11)*b+127)/255)<<11)|(((((s>>5)&63)*a+((d>>5)&63)*b+127)/255)<<5)|(((s&31)*a+(d&31)*b+127)/255));}
static int bounds(const sample*in,unsigned n,span*out){
 for(unsigned y=0;y<HEIGHT;y++)out[y]=(span){WIDTH,0};
 for(unsigned i=0;i<n;i++){
  const sample*s=in+i;if(s->x>=WIDTH||s->y>=HEIGHT||!isfinite(s->f.z))return 0;
  if(!s->f.alpha)continue;
  span*b=out+s->y;
  unsigned right=b->width?b->left+b->width:s->x+1;
  if(s->x<b->left)b->left=s->x;
  if(s->x+1>right)right=s->x+1;
  b->width=right-b->left;if(b->width>CAP)return 0;
 }
 return 1;
}
static int overlaps(const void*a,size_t na,const void*b,size_t nb){
 uintptr_t aa=(uintptr_t)a,bb=(uintptr_t)b;
 if(na>UINTPTR_MAX-aa||nb>UINTPTR_MAX-bb)return 1;
 return aa<bb+nb&&bb<aa+na;
}
static int render(const sample*in,unsigned n,uint16_t*pixels,const float*opaque,unsigned rows){
 if(!in||!pixels||!opaque||(rows!=1&&rows!=4&&rows!=8))return 0;
 /* Caller exclusively owns immutable inputs for this synchronous call. No
  * callback or retained pointer; reject aliasing before any output writes. */
 if(n>65536||
    overlaps(in,(size_t)n*sizeof(sample),pixels,WIDTH*HEIGHT*sizeof(*pixels))||
    overlaps(opaque,WIDTH*HEIGHT*sizeof(*opaque),pixels,WIDTH*HEIGHT*sizeof(*pixels)))return 0;
 span ranges[HEIGHT];work w;
 if(!bounds(in,n,ranges))return 0;
 /* Whole-frame preflight: no pixel mutation before all count/depth checks. */
 for(unsigned y=0;y<HEIGHT;y++){
  memset(w.counts,0,sizeof w.counts);span b=ranges[y];
  for(unsigned i=0;i<n;i++)if(in[i].y==y&&in[i].f.alpha){
   unsigned x=in[i].x;if(!isfinite(opaque[y*WIDTH+x]))return 0;
   if(in[i].f.z<=opaque[y*WIDTH+x])continue;
   if(x<b.left||x-b.left>=b.width||x-b.left>=CAP)return 0;
   if(w.counts[x-b.left]==SLOTS)return 0;
   w.counts[x-b.left]++;
  }
 }
 /* Only immutable same-owner inputs may be replayed after preflight. */
 for(unsigned band=0;band<HEIGHT;band+=rows)for(unsigned y=band;y<HEIGHT&&y<band+rows;y++){
  span b=ranges[y];if(!b.width)continue;memset(w.counts,0,sizeof w.counts);
  memcpy(w.rgb,pixels+y*WIDTH+b.left,b.width*sizeof(uint16_t));
  for(unsigned i=0;i<n;i++)if(in[i].y==y&&in[i].f.alpha&&in[i].f.z>opaque[y*WIDTH+in[i].x]){
   unsigned x=in[i].x-b.left,at=w.counts[x];fragment*a=w.slots+x*SLOTS,f=in[i].f;
   while(at&&(a[at-1].z>f.z||(a[at-1].z==f.z&&a[at-1].order<f.order))){a[at]=a[at-1];at--;}
   a[at]=f;w.counts[x]++;
  }
  for(unsigned x=0;x<b.width;x++)for(unsigned k=0;k<w.counts[x];k++){fragment f=w.slots[x*SLOTS+k];w.rgb[x]=blend(f.rgb,w.rgb[x],f.alpha);}
  memcpy(pixels+y*WIDTH+b.left,w.rgb,b.width*sizeof(uint16_t));
 }
 return 1;
}
int main(void){
 sample input[HEIGHT*CAP*2+8];unsigned n=0;
 float z[WIDTH*HEIGHT];uint16_t p[WIDTH*HEIGHT]={0},q[WIDTH*HEIGHT]={0},r[WIDTH*HEIGHT]={0},old[WIDTH*HEIGHT];
 for(unsigned i=0;i<WIDTH*HEIGHT;i++)z[i]=-100;
 for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<CAP;x++){
  input[n++]=(sample){80+x,y,{(float)x,1,0xf800,128,0}};
  input[n++]=(sample){80+x,y,{(float)(CAP-1-x),2,0x001f,128,0}};
 }
 assert(render(input,n,p,z,1)&&render(input,n,q,z,4)&&render(input,n,r,z,8));assert(!memcmp(p,q,sizeof p)&&!memcmp(p,r,sizeof p));
 assert(p[79]==0&&p[134]==0&&p[80]!=p[133]);memcpy(old,p,sizeof p);
 input[n]=(sample){134,HEIGHT-1,{1,3,0xffff,128,0}};assert(!render(input,n+1,p,z,1));assert(!memcmp(p,old,sizeof p)); /* pose extends beyond width */
 input[n].x=WIDTH;assert(!render(input,n+1,p,z,1));assert(!memcmp(p,old,sizeof p));
 for(unsigned i=0;i<3;i++)input[n+i]=(sample){133,HEIGHT-1,{90+(float)i,3+i,0xffff,128,0}};
 assert(!render(input,n+3,p,z,1));assert(!memcmp(p,old,sizeof p));assert(render(input,n,p,z,1));
 input[n]=(sample){0,0,{NAN,0,0,0,0}};memcpy(old,p,sizeof p);assert(!render(input,n+1,p,z,1));assert(!memcmp(old,p,sizeof p));
 memcpy(old,p,sizeof p);assert(!render((const sample*)p,1,p,z,1));assert(!memcmp(p,old,sizeof p));
 assert(!render(input,n,p,(const float*)p,1));assert(!memcmp(p,old,sizeof p));
 printf("PASS alias rejection/span54/crossing band identity and out-of-view/width/late slot overflow atomic+retry; work=%zu ranges=%zu bytes; no hardware claim\n",sizeof(work),sizeof(span)*HEIGHT);
}
