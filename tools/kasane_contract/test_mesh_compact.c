#include "ayatsuri/compact_opaque.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static bool samef(float a,float b){return !memcmp(&a,&b,4);}
int main(void){
    uint8_t storage[1024],copy[1024];memset(storage,0xa5,sizeof storage);
    co_vertex src[4]={{-0.0f,1,-1,3,0xf800,0xf800},{9,1,0,4,0xf800,0xf800},{1,9,1,5,0xf800,0xf800},{10,10,0,6,1,2}};
    uint16_t ix[3]={0,1,2};co_input in={src,ix,7,4,1};co_buffer b={storage,sizeof storage,0,0};
    assert(co_append(&b,&in));co_view v;assert(co_read(storage,b.size,&v));assert(v.mode==1&&v.vertex_count==3&&v.node==7);
    for(unsigned i=0;i<3;i++){co_vertex p;assert(co_vertex_at(&v,i,&p));assert(samef(p.x,src[i].x)&&samef(p.y,src[i].y)&&samef(p.depth,src[i].depth));assert(p.color_a==src[i].color_a&&p.color_b==src[i].color_b);}
    uint8_t tri[3];assert(co_triangle_at(&v,0,tri));assert(tri[0]==0&&tri[1]==1&&tri[2]==2);
    size_t first=b.size;memcpy(copy,storage,sizeof storage);b.capacity=b.size;
    assert(!co_append(&b,&in)&&b.size==first&&b.packets==1&&!memcmp(copy,storage,sizeof storage));b.capacity=sizeof storage;
    ix[0]=4;assert(!co_append(&b,&in)&&!memcmp(copy,storage,sizeof storage));ix[0]=0;
    src[0].depth=NAN;assert(!co_append(&b,&in)&&!memcmp(copy,storage,sizeof storage));src[0].depth=3;
    b.packets=CO_MAX_PACKETS;assert(!co_append(&b,&in));b.packets=1;
    src[0].color_b=0x001f;src[1].color_a=0x07e0;src[0].mask=-0.0f;
    assert(co_append(&b,&in));assert(co_read(storage+first,b.size-first,&v));assert(v.mode==0);
    for(unsigned i=0;i<3;i++){co_vertex p;assert(co_vertex_at(&v,i,&p));assert(samef(p.x,src[i].x)&&samef(p.y,src[i].y)&&samef(p.depth,src[i].depth)&&samef(p.mask,src[i].mask)&&p.color_a==src[i].color_a&&p.color_b==src[i].color_b);}
    memset(src,0x5a,sizeof src);assert(co_read(storage+first,b.size-first,&v));
    /* Reader failures do not replace a previous valid view. */
    co_view saved=v;uint8_t old=storage[0];storage[0]=33;assert(!co_read(storage,first,&v)&&v.data==saved.data&&v.size==saved.size&&v.mode==saved.mode);storage[0]=old;
    old=storage[2];storage[2]=2;assert(!co_read(storage,first,&v));storage[2]=old;
    storage[3]=1;assert(!co_read(storage,first,&v));storage[3]=0;
    storage[48]=3;assert(!co_read(storage,first,&v));storage[48]=0;
    storage[51]=1;assert(!co_read(storage,first,&v));storage[51]=0;
    assert(!co_read(storage,first-1,&v));assert(co_read(storage,first,&v));
    assert(!co_vertex_at(&v,3,&src[0])&&!co_triangle_at(&v,1,tri));
    assert(!co_read(NULL,first,&v)&&!co_append(NULL,&in));
    puts("compact opaque synthetic: PASS");return 0;
}
