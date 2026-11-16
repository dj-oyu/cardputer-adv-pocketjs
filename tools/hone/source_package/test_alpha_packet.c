#include <assert.h>
#include "alpha_packet_candidate.h"
int main(void){hone_alpha_packet a[8]={0},b[8]={0},p={{{0,0,-2,0},{4,0,2,4},{0,4,0,0}},{1,0,0},.37f,0,4,12,4,1},out[3];unsigned owner=1,wrong=2;int n=hone_alpha_clip(&p,-1,1,out);assert(n>=1&&n<=3);
for(int i=0;i<n;i++){assert(out[i].alpha==p.alpha&&out[i].order==12);for(int j=0;j<3;j++){assert(out[i].v[j].z>=-1&&out[i].v[j].z<=1);assert(out[i].v[j].x==out[i].v[j].mask);}}
hone_alpha_snapshot ready={a,0,8,&owner,1,0,1},pending={b,0,8,&owner,1,0,0};
assert(!hone_alpha_append(&pending,&wrong,1,&p,1,-1,1));assert(!hone_alpha_append(&pending,&owner,2,&p,1,-1,1));
assert(hone_alpha_append(&pending,&owner,1,&p,1,-1,1));assert(hone_alpha_publish(&ready,&pending,&owner,1));size_t old=ready.count;hone_alpha_packet saved[8];memcpy(saved,ready.packets,sizeof saved);
hone_alpha_packet pair[2]={p,p};pair[1].alpha=NAN;assert(!hone_alpha_append(&pending,&owner,1,pair,2,-1,1));assert(!pending.count&&!memcmp(saved,ready.packets,sizeof saved));
pending.capacity=1;assert(!hone_alpha_append(&pending,&owner,1,&p,1,-1,1));assert(!pending.count);pending.capacity=8;assert(hone_alpha_append(&pending,&owner,1,&p,1,-1,1));ready.pinned=1;assert(!hone_alpha_publish(&ready,&pending,&owner,1));assert(ready.count==old&&!memcmp(saved,ready.packets,sizeof saved));ready.pinned=0;assert(hone_alpha_publish(&ready,&pending,&owner,1));
assert(!hone_alpha_append(&pending,&owner,1,pending.packets,1,-1,1));return 0;}
