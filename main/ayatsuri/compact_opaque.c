#include "compact_opaque.h"
#include <float.h>
#include <math.h>
#include <string.h>
_Static_assert(sizeof(float)==4 && FLT_RADIX==2 && FLT_MANT_DIG==24,"IEEE binary32 required");
/* Wire v1: nv,nt,mode,reserved,u16 node,u16 reserved,u16 color,u16 bytes;
 * mode0 = x/y/mask/depth F32 + A/B U16 (20B), mode1 = x/y/depth (12B).
 * U8 triangle indices, then zero padding to four bytes. All fields little-endian.
 * Constant mode is legal only when all referenced vertices have identical A=B.
 * Depth is preserved exactly; interpretation/tie policy belongs to the renderer. */
static unsigned u16(const uint8_t*p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
static void w16(uint8_t*p,unsigned v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static float rf(const uint8_t*p){uint32_t u=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;float f;memcpy(&f,&u,4);return f;}
static void wf(uint8_t*p,float f){uint32_t u;memcpy(&u,&f,4);for(unsigned k=0;k<4;k++)p[k]=(uint8_t)(u>>(8*k));}
static size_t aligned(size_t n){return (n+3u)&~(size_t)3u;}
static bool vertex_valid(const co_vertex*v){return isfinite(v->x)&&isfinite(v->y)&&isfinite(v->depth)&&isfinite(v->mask)&&fabsf(v->x)<=4096&&fabsf(v->y)<=4096;}
bool co_append(co_buffer*b,const co_input*in){
    if(!b||!in||!b->data||!in->vertices||!in->indices||b->size>b->capacity||b->packets>=CO_MAX_PACKETS||
       in->vertex_count<3||in->vertex_count>CO_MAX_VERTICES||!in->triangle_count||in->triangle_count>CO_MAX_TRIANGLES)return false;
    bool used[CO_MAX_VERTICES]={0},solid=true;uint8_t remap[CO_MAX_VERTICES]={0};unsigned count=0;
    for(unsigned t=0;t<in->triangle_count;t++){
        const uint16_t*ix=in->indices+3*t;
        if(ix[0]>=in->vertex_count||ix[1]>=in->vertex_count||ix[2]>=in->vertex_count||ix[0]==ix[1]||ix[1]==ix[2]||ix[0]==ix[2])return false;
        for(unsigned j=0;j<3;j++)used[ix[j]]=true;
    }
    const uint16_t color=in->vertices[in->indices[0]].color_a;
    for(unsigned v=0;v<in->vertex_count;v++)if(used[v]){
        const co_vertex*p=in->vertices+v;if(!vertex_valid(p))return false;
        remap[v]=(uint8_t)count++;if(p->color_a!=color||p->color_b!=color)solid=false;
    }
    const unsigned stride=solid?12:20;const size_t raw=12u+stride*count+3u*in->triangle_count,bytes=aligned(raw);
    if(bytes>b->capacity-b->size)return false;
    uint8_t*out=b->data+b->size;memset(out,0,bytes);out[0]=(uint8_t)count;out[1]=in->triangle_count;out[2]=(uint8_t)solid;
    w16(out+4,in->node);w16(out+8,solid?color:0);w16(out+10,(unsigned)bytes);
    for(unsigned v=0;v<in->vertex_count;v++)if(used[v]){
        const co_vertex*p=in->vertices+v;uint8_t*d=out+12+stride*remap[v];wf(d,p->x);wf(d+4,p->y);
        if(solid)wf(d+8,p->depth);else{wf(d+8,p->mask);wf(d+12,p->depth);w16(d+16,p->color_a);w16(d+18,p->color_b);}
    }
    uint8_t*ix=out+12+stride*count;for(unsigned j=0;j<3u*in->triangle_count;j++)ix[j]=remap[in->indices[j]];
    b->size+=bytes;b->packets++;return true;
}
bool co_vertex_at(const co_view*v,unsigned index,co_vertex*out){
    if(!v||!out||!v->data||v->mode>1||index>=v->vertex_count)return false;
    const unsigned stride=v->mode?12:20;const size_t at=12u+stride*index;if(at>v->size||stride>v->size-at)return false;
    const uint8_t*d=v->data+at;out->x=rf(d);out->y=rf(d+4);
    if(v->mode){out->mask=-1;out->depth=rf(d+8);out->color_a=out->color_b=v->color;}
    else{out->mask=rf(d+8);out->depth=rf(d+12);out->color_a=(uint16_t)u16(d+16);out->color_b=(uint16_t)u16(d+18);}
    return true;
}
bool co_triangle_at(const co_view*v,unsigned index,uint8_t out[3]){
    if(!v||!out||!v->data||v->mode>1||index>=v->triangle_count)return false;
    size_t at=12u+(v->mode?12u:20u)*v->vertex_count+3u*index;if(at>v->size||3u>v->size-at)return false;
    for(unsigned k=0;k<3;k++){out[k]=v->data[at+k];if(out[k]>=v->vertex_count)return false;}return true;
}
bool co_read(const uint8_t*d,size_t available,co_view*out){
    if(!d||!out||available<12||d[0]<3||d[0]>CO_MAX_VERTICES||!d[1]||d[1]>CO_MAX_TRIANGLES||d[2]>1||d[3]||u16(d+6)||(!d[2]&&u16(d+8)))return false;
    const size_t raw=12u+(d[2]?12u:20u)*d[0]+3u*d[1],bytes=aligned(raw);
    if(u16(d+10)!=bytes||bytes>available)return false;
    co_view v={d,bytes,(uint16_t)u16(d+4),(uint16_t)u16(d+8),d[0],d[1],d[2]};
    for(unsigned i=0;i<v.vertex_count;i++){co_vertex p;if(!co_vertex_at(&v,i,&p)||!vertex_valid(&p))return false;}
    for(unsigned t=0;t<v.triangle_count;t++){uint8_t ix[3];if(!co_triangle_at(&v,t,ix)||ix[0]==ix[1]||ix[1]==ix[2]||ix[0]==ix[2])return false;}
    for(size_t i=raw;i<bytes;i++)if(d[i])return false;
    *out=v;return true;
}
