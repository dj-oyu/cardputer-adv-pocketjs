#include "bench_core.h"
#include "generic_wrapper.c"
#include <string.h>
/* Synthetic arithmetic only. No model, user data, packet or renderer assets. */
typedef struct{int64_t e[3],s[3];unsigned open,n;} sample;
#define U INT64_C(1048576)
static const sample cases[]={
 {{0,0,0},{0,0,0},0,1},{{0,0,0},{0,0,0},7,3},
 {{-1,1,0},{U,-U,0},4,7},{{U,U,U},{0,0,0},0,240},
 {{-U,-U,-U},{0,0,0},0,239},{{-20*U,100*U,40*U},{U,-U,0},3,240},
 {{-INT64_C(2305843009213693952),INT64_C(2305843009213693952),0},{INT64_C(1125899906842624),-INT64_C(1125899906842624),U},4,240},
 {{INT64_C(4294967295),-INT64_C(4294967296),1},{-U,INT64_C(4293918720),U},1,239}
};
static uint64_t hash_add(uint64_t h,uint64_t x){return (h^x)*UINT64_C(1099511628211);}
__attribute__((noinline,noipa)) static uint64_t scalar_row(const sample*p){
 int64_t e[3]={p->e[0],p->e[1],p->e[2]};uint64_t h=0;
 for(unsigned x=0;x<p->n;x++){
  unsigned outside=0;for(unsigned k=0;k<3;k++)outside|=e[k]<0||(e[k]==0&&((p->open>>k)&1));
  h=hash_add(h,outside);if(!outside)for(unsigned k=0;k<3;k++)h=hash_add(h,(uint64_t)e[k]);
  if(x+1<p->n)for(unsigned k=0;k<3;k++)e[k]+=p->s[k];
 }return h;
}
__attribute__((noinline,noipa)) static uint64_t pie_row(const sample*p){
 edge_block b;edge_block_init(p->e,p->s,p->open,&b);uint64_t h=0;
 for(unsigned x=0;x<p->n;x+=4){if(x)edge_block_next(&b);
  for(unsigned i=0;i<4&&x+i<p->n;i++){
   h=hash_add(h,b.outside[i]!=0);if(!b.outside[i]){int64_t e[3];edge_block_weights(&b,i,e);for(unsigned k=0;k<3;k++)h=hash_add(h,(uint64_t)e[k]);}
  }
 }return h;
}
/* Exact device preflight, outside timed windows; hash is only run sanity. */
static int verify_row(const sample*p){
 edge_block b;edge_block_init(p->e,p->s,p->open,&b);
 for(unsigned x=0;x<p->n;x+=4){if(x)edge_block_next(&b);
  for(unsigned i=0;i<4&&x+i<p->n;i++){
   int64_t expected[3],got[3];unsigned outside=0;
   for(unsigned k=0;k<3;k++){expected[k]=p->e[k]+(int64_t)(x+i)*p->s[k];outside|=expected[k]<0||(expected[k]==0&&((p->open>>k)&1));}
   if((b.outside[i]!=0)!=outside)return 0;
   edge_block_weights(&b,i,got);
   for(unsigned k=0;k<3;k++)if(got[k]!=expected[k])return 0;
  }
 }return 1;
}
static uint64_t batch(unsigned mode){uint64_t h=0;for(unsigned repeat=0;repeat<4;repeat++)for(unsigned k=0;k<sizeof cases/sizeof cases[0];k++)h=hash_add(h,mode?pie_row(&cases[k]):scalar_row(&cases[k]));return h;}
int edge_bench_run_controlled(edge_bench_clock clock,void (*yield_hook)(void),edge_bench_result*out,const edge_bench_control*control){
 if(!clock||!out)return -1;
 memset(out,0,sizeof*out);volatile uint64_t sink=0;
 /* Before timing: force initial CP3 use, exact outputs checked case-by-case. */
 for(unsigned k=0;k<sizeof cases/sizeof cases[0];k++){
  if(control&&control->cancelled&&control->cancelled(control->context)){out->error=4;return -1;}
  if(!verify_row(&cases[k])||scalar_row(&cases[k])!=pie_row(&cases[k])){out->error=1;return -1;}
  if(control&&control->preflight_yield)control->preflight_yield(control->context);
 }
 uint64_t wanted=batch(0);
 if(control&&control->preflight_yield)control->preflight_yield(control->context);
 if(control&&control->cancelled&&control->cancelled(control->context)){out->error=4;return -1;}
 if(batch(1)!=wanted){out->error=2;return -1;}
 if(control&&control->preflight_yield)control->preflight_yield(control->context);
 if(control&&control->cancelled&&control->cancelled(control->context)){out->error=4;return -1;}
 for(unsigned r=0;r<31;r++){
  if(control&&control->cancelled&&control->cancelled(control->context)){out->error=4;return -1;}
  uint32_t a=clock(),b=clock();out->clock_overhead[r]=b-a;
  for(unsigned order=0;order<2;order++){
   if(control&&control->cancelled&&control->cancelled(control->context)){out->error=4;return -1;}
   unsigned mode=(r+order)&1;
   __asm__ volatile("":::"memory");a=clock();uint64_t got=batch(mode);b=clock();__asm__ volatile("":::"memory");sink=got;
   if(got!=wanted){out->error=3;return -1;}if(mode)out->pie[r]=b-a;else out->scalar[r]=b-a;
   if(yield_hook)yield_hook();
  }out->completed=r+1;
 }out->checksum=wanted;(void)sink;return 0;
}

int edge_bench_run(edge_bench_clock clock,void (*yield_hook)(void),edge_bench_result*out){return edge_bench_run_controlled(clock,yield_hook,out,NULL);}
