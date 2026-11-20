/* Synthetic fragment-stage experiment only; no triangle raster or LCD caller. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#define W 16
#define H 8
#define K 4
typedef struct {float z; uint32_t order; uint16_t rgb; uint8_t alpha,pad;} frag;
typedef struct {frag slots[W*K];uint8_t count[W];} row_work;
static uint16_t blend(uint16_t s,uint16_t d,unsigned a){
 unsigned b=255-a;
 return (uint16_t)(((((s>>11)*a+(d>>11)*b+127)/255)<<11)|
 (((((s>>5)&63)*a+((d>>5)&63)*b+127)/255)<<5)|(((s&31)*a+(d&31)*b+127)/255));
}
static int add(row_work*w,unsigned x,frag f,float opaque){
 if(x>=W||!isfinite(f.z))return 0;
 if(!f.alpha||f.z<=opaque)return 1;
 unsigned n=w->count[x];if(n==K)return 0;
 frag *a=w->slots+x*K;unsigned at=n;
 /* Far to near; lower submission ID goes on top for an exact depth tie. */
 while(at&&(a[at-1].z>f.z||(a[at-1].z==f.z&&a[at-1].order<f.order))){a[at]=a[at-1];at--;}
 a[at]=f;w->count[x]++;return 1;
}
static int count_only(row_work*w,unsigned x,frag f,float opaque){
 if(x>=W||!isfinite(f.z))return 0;
 if(!f.alpha||f.z<=opaque)return 1;
 if(w->count[x]==K)return 0;
 w->count[x]++;return 1;
}
static int scan(uint16_t*dst,int overflow,unsigned strip_rows,int write){
 uint16_t candidate[W];
 for(unsigned band=0;band<H;band+=strip_rows)for(unsigned y=band;y<H&&y<band+strip_rows;y++){
  row_work row={0};if(write)memcpy(candidate,dst+y*W,sizeof candidate);
  for(unsigned x=0;x<W;x++){
   float opaque=x==0?100:-100;
   frag a={(float)x,1,0xf800,128,0},b={(float)(W-1-x),2,0x001f,128,0};
   if(write){if(!add(&row,x,a,opaque)||!add(&row,x,b,opaque))return 0;}
   else if(!count_only(&row,x,a,opaque)||!count_only(&row,x,b,opaque))return 0;
   if(overflow&&y==H-1&&x==W-1)for(unsigned k=0;k<K;k++)if(!(write?add(&row,x,(frag){30+(float)k,3+k,0xffff,64,0},opaque):count_only(&row,x,(frag){30+(float)k,3+k,0xffff,64,0},opaque)))return 0;
   if(!write)continue;
   uint16_t color=candidate[x];
   for(unsigned k=0;k<row.count[x];k++)color=blend(row.slots[x*K+k].rgb,color,row.slots[x*K+k].alpha);
   candidate[x]=color;
  }
  if(write)memcpy(dst+y*W,candidate,sizeof candidate);
 }
 return 1;
}
/* Replay requires immutable packets/controls and deterministic scan. Real LCD
 * caller must preserve that owner through both passes; not proved here. */
static int render(uint16_t*dst,int overflow,unsigned rows){
 if(!scan(dst,overflow,rows,0))return 0;
 return scan(dst,overflow,rows,1);
}
static volatile uint64_t sink;
static void opaque_fragment_fixture(uint16_t *dst){
 for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++){
  float z=x==0?100:-100;uint16_t color=0;
  if((float)x>z){z=(float)x;color=0xf800;}
  if((float)(W-1-x)>z){color=0x001f;}
  dst[y*W+x]=color;
 }
}
int main(void){
 assert(sizeof(frag)==12);
 row_work row={0};assert(add(&row,0,(frag){1,1,0xffff,0,0},-100)&&row.count[0]==0);
 assert(add(&row,0,(frag){1,1,0xffff,255,0},1)&&row.count[0]==0);
 assert(add(&row,0,(frag){2,1,0xf800,255,0},1));assert(add(&row,0,(frag){2,2,0x001f,255,0},1));
 uint16_t color=0;for(unsigned k=0;k<2;k++)color=blend(row.slots[k].rgb,color,row.slots[k].alpha);assert(color==0xf800);
 uint16_t a[W*H]={0},b[W*H]={0},c[W*H]={0},old[W*H];
 assert(render(a,0,1)&&render(b,0,4)&&render(c,0,8));assert(!memcmp(a,b,sizeof a)&&!memcmp(a,c,sizeof a));
 assert(a[0]==0&&a[1]!=a[W-1]); /* crossing depths reverse over order */
 memcpy(old,a,sizeof a);assert(!render(a,1,1));assert(!memcmp(a,old,sizeof a));assert(render(a,0,1));
 const unsigned loops=100000;clock_t t=clock();
 for(unsigned n=0;n<loops;n++){uint16_t out[W*H]={0};render(out,0,1);sink+=out[7];}
 double seconds=(double)(clock()-t)/CLOCKS_PER_SEC;
 t=clock();
 for(unsigned n=0;n<loops;n++){uint16_t out[W*H]={0};opaque_fragment_fixture(out);sink+=out[n%(W*H)];}
 double opaque_seconds=(double)(clock()-t)/CLOCKS_PER_SEC;
 printf("host synthetic opaque-only fragment baseline %.1f ns/pixel; alpha %.2fx; excludes triangle raster/LCD\n",opaque_seconds*1e9/(loops*W*H),seconds/opaque_seconds);
 printf("PASS crossing depth, ties, opaque occlusion, alpha0/255, rows1/4/8, late overflow and retry; fragment=%zu row240=%zu avoided_fullframe_bytes=%u; host fragment fixture %.1f ns/pixel\n",sizeof(frag),(size_t)240*K*sizeof(frag)+240,240*135*2,seconds*1e9/(loops*W*H));
}
