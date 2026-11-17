#ifndef HONE_ALPHA_GRAPH_BAND_CANDIDATE_H
#define HONE_ALPHA_GRAPH_BAND_CANDIDATE_H
#include "alpha_graph_raster_candidate.h"
#include "ui/kasane/ksn_mesh_depth.h"
/* Experimental unlit graph backend for the native material-hook boundary.
 * Coordinates are already orthographically clipped with local XYZ preserved.
 * Snapshot/programs/drivers are immutable frame-owned dependencies. This is not
 * Principled shading, a package loader, perspective/near clip or target admission.
 * No full-screen depth allocation: one row of depth/pixels and graph scratch.
 * Full preflight is required before replay and any LCD submission. */
typedef struct {
    hone_graph_snapshot graph;
    const void *binding;
    uint32_t generation;
    unsigned span_limit;
} hone_graph_band_payload;
typedef struct {
    const void *binding;
    uint32_t generation;
    uint16_t *row;float *depth,*scratch;size_t scratch_count;
} hone_graph_band_work;
static inline int hone_graph_band_span(const hone_graph_band_payload*p,const void*w,size_t bytes){
    const hone_graph_snapshot*s=&p->graph;
    if(!hone_alpha_disjoint(w,bytes,p,sizeof *p)||
       (s->count&&!hone_alpha_disjoint(w,bytes,s->packets,s->count*sizeof *s->packets))||
       (s->driver_count&&!hone_alpha_disjoint(w,bytes,s->drivers,s->driver_count*sizeof *s->drivers)))return 0;
    for(size_t i=0;i<s->count;i++){
        const hone_alpha_program*a=s->packets[i].triangle.alpha,*c=s->packets[i].triangle.color;
        if(!a||!c||!hone_alpha_disjoint(w,bytes,a,sizeof *a)||!hone_alpha_disjoint(w,bytes,c,3*sizeof *c))return 0;
        for(unsigned j=0;j<4;j++){
            const hone_alpha_program*q=j?c+j-1:a;
            if(!q->code||!q->count||q->count>1024||q->result>=q->count||
               !hone_alpha_disjoint(w,bytes,q->code,q->count*sizeof *q->code))return 0;
        }
    }
    return 1;
}
static inline int hone_graph_band_valid(const ksn_mesh_snapshot*opaque,const hone_graph_band_payload*p,
    const hone_graph_band_work*w,uint16_t*pixels,size_t pixel_count){
    if(!opaque||!p||!w||!w->binding||p->binding!=w->binding||!w->generation||p->generation!=w->generation||
       p->graph.owner!=w->binding||p->graph.generation!=w->generation||!p->graph.ready||
       p->graph.count>p->graph.capacity||p->graph.count>65536||p->graph.driver_count>65536||
       (p->graph.count&&!p->graph.packets)||(p->graph.driver_count&&!p->graph.drivers)||
       !p->span_limit||p->span_limit>240||!w->row||!w->depth||!w->scratch||
       !w->scratch_count||w->scratch_count>1024||pixel_count>240*8)return 0;
    const void*mut[]={w->row,w->depth,w->scratch,pixels};
    size_t len[]={240*sizeof *w->row,240*sizeof *w->depth,w->scratch_count*sizeof *w->scratch,pixel_count*sizeof *pixels};
    for(unsigned i=0;i<4;i++){
        if(!len[i])continue;if(!mut[i]||!hone_graph_band_span(p,mut[i],len[i])||
           !hone_alpha_disjoint(mut[i],len[i],opaque,sizeof *opaque)||
           !hone_alpha_disjoint(mut[i],len[i],opaque->packets.data,opaque->packets.capacity)||
           !hone_alpha_disjoint(mut[i],len[i],w,sizeof *w))return 0;
        for(unsigned j=0;j<i;j++)if(len[j]&&!hone_alpha_disjoint(mut[i],len[i],mut[j],len[j]))return 0;
    }
    return 1;
}
static inline int hone_graph_band_row(const ksn_mesh_snapshot*opaque,const hone_graph_band_payload*p,
    hone_graph_band_work*w,unsigned y){
    for(unsigned x=0;x<240;x++)w->row[x]=0;
    if(!ksn_mesh_render(opaque,w->row,y,1,w->depth,1))return 0;
    unsigned left=240,right=0;
    for(unsigned x=0;x<240;x++){
        hone_graph_ordered fragments[4];
        int n=hone_graph_fragments(&p->graph,x,y,w->depth[x],w->scratch,w->scratch_count,fragments);
        if(n<0)return 0;if(!n)continue;if(x<left)left=x;right=x+1;
        float rgb[3]={(w->row[x]>>11)/31.f,((w->row[x]>>5)&63)/63.f,(w->row[x]&31)/31.f};
        for(int i=0;i<n;i++)for(unsigned k=0;k<3;k++)
            rgb[k]=fragments[i].fragment.rgb[k]*fragments[i].fragment.alpha+rgb[k]*(1-fragments[i].fragment.alpha);
        w->row[x]=(uint16_t)(((unsigned)floorf(rgb[0]*31+.5f)<<11)|
            ((unsigned)floorf(rgb[1]*63+.5f)<<5)|(unsigned)floorf(rgb[2]*31+.5f));
    }
    return !right||right-left<=p->span_limit;
}
static inline int hone_graph_band_preflight(const ksn_mesh_snapshot*opaque,const hone_graph_band_payload*p,
    hone_graph_band_work*w){
    if(!hone_graph_band_valid(opaque,p,w,NULL,0))return 0;
    for(unsigned y=0;y<135;y++)if(!hone_graph_band_row(opaque,p,w,y))return 0;
    return 1;
}
/* Failure can leave prior rows in this private output band; the display owner
 * must drain/rollback and repair if an immutable-input contract was violated. */
static inline int hone_graph_band_render(const ksn_mesh_snapshot*opaque,const hone_graph_band_payload*p,
    hone_graph_band_work*w,unsigned y,unsigned rows,uint16_t*pixels){
    if(y>135||rows>8||rows>135-y||!hone_graph_band_valid(opaque,p,w,pixels,(size_t)rows*240))return 0;
    for(unsigned j=0;j<rows;j++){
        if(!hone_graph_band_row(opaque,p,w,y+j))return 0;
        memcpy(pixels+(size_t)j*240,w->row,240*sizeof *pixels);
    }
    return 1;
}
#endif
