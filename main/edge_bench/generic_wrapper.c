#include <stdint.h>
typedef struct{_Alignas(16) int32_t h[12],l[12],d[24],outside[4];uint32_t residue[3];} edge_block;
static const _Alignas(16) int32_t mask[4]={4095,4095,4095,4095};
void edge3x4_sched_step(int32_t*,int32_t*,const int32_t*,const int32_t*,int32_t*);
static int64_t floor_div(int64_t n,int64_t d){return n/d-(n%d<0);}
/* Contract |E|<=2^61, S multiple2^20 and |S|<=2^50;
 * openmask low3 bits. Caller confines row traversal to <=240 pixels.
 * E/S/out are disjoint. No runtime validation; internal admitted inputs only. */
void edge_block_init(const int64_t E[3],const int64_t S[3],unsigned openmask,edge_block*b){
for(unsigned i=0;i<4;i++)b->outside[i]=0;
for(unsigned k=0;k<3;k++){
 int64_t q=floor_div(E[k]-((openmask>>k)&1),1048576),s=S[k]/1048576;
 int64_t delta=4*s,dh=floor_div(delta,4096),dl=delta-dh*4096;
 b->residue[k]=(uint32_t)(E[k]-q*1048576);
 for(unsigned i=0;i<4;i++){
  int64_t v=q+i*s,h=floor_div(v,4096);
  b->h[k*4+i]=h;b->l[k*4+i]=v-h*4096;
  b->d[k*8+i]=dl;b->d[k*8+4+i]=dh;b->outside[i]|=h<0?-1:0;
 }
}}
void edge_block_next(edge_block*b){edge3x4_sched_step(b->h,b->l,b->d,mask,b->outside);}
/* lane0..3; reconstruct only after outside[lane]==0. */
void edge_block_weights(const edge_block*b,unsigned lane,int64_t w[3]){
for(unsigned k=0;k<3;k++)w[k]=((int64_t)b->h[k*4+lane]*4096+b->l[k*4+lane])*1048576+b->residue[k];
}
_Static_assert(sizeof(edge_block)==224,"block layout");
