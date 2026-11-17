#ifndef HONE_ALPHA_GRAPH_OCCLUSION_BAND_CANDIDATE_H
#define HONE_ALPHA_GRAPH_OCCLUSION_BAND_CANDIDATE_H
#include "alpha_graph_band_candidate.h"
#include "alpha_graph_occlusion_candidate.h"
/* Explicit opt-in band backend; original band functions remain available.
 * Same payload, scratch disjointness and immutable preflight/replay contract.
 * Full preflight before display; no near clipping, lighting or device claims. */
static inline int hone_graph_occlusion_band_row(const ksn_mesh_snapshot*opaque,const hone_graph_band_payload*p,
    hone_graph_band_work*w,unsigned y){
    for(unsigned x=0;x<240;x++)w->row[x]=0;
    if(!ksn_mesh_render(opaque,w->row,y,1,w->depth,1))return 0;
    unsigned left=240,right=0;
    for(unsigned x=0;x<240;x++){
        hone_graph_ordered fragments[4];
        int n=hone_graph_fragments_occlusion(&p->graph,x,y,w->depth[x],w->scratch,w->scratch_count,fragments);
        if(n<0)return 0;if(!n)continue;if(x<left)left=x;right=x+1;
        float rgb[3]={(w->row[x]>>11)/31.f,((w->row[x]>>5)&63)/63.f,(w->row[x]&31)/31.f};
        for(int i=0;i<n;i++)for(unsigned k=0;k<3;k++)
            rgb[k]=fragments[i].fragment.rgb[k]*fragments[i].fragment.alpha+rgb[k]*(1-fragments[i].fragment.alpha);
        w->row[x]=(uint16_t)(((unsigned)floorf(rgb[0]*31+.5f)<<11)|
            ((unsigned)floorf(rgb[1]*63+.5f)<<5)|(unsigned)floorf(rgb[2]*31+.5f));
    }
    return !right||right-left<=p->span_limit;
}
static inline int hone_graph_occlusion_band_preflight(const ksn_mesh_snapshot*opaque,const hone_graph_band_payload*p,
    hone_graph_band_work*w){
    if(!hone_graph_band_valid(opaque,p,w,NULL,0))return 0;
    for(unsigned y=0;y<135;y++)if(!hone_graph_occlusion_band_row(opaque,p,w,y))return 0;
    return 1;
}
/* Failure can leave prior rows in this private output band; the display owner
 * must drain/rollback and repair if an immutable-input contract was violated. */
static inline int hone_graph_occlusion_band_render(const ksn_mesh_snapshot*opaque,const hone_graph_band_payload*p,
    hone_graph_band_work*w,unsigned y,unsigned rows,uint16_t*pixels){
    if(y>135||rows>8||rows>135-y||!hone_graph_band_valid(opaque,p,w,pixels,(size_t)rows*240))return 0;
    for(unsigned j=0;j<rows;j++){
        if(!hone_graph_occlusion_band_row(opaque,p,w,y+j))return 0;
        memcpy(pixels+(size_t)j*240,w->row,240*sizeof *pixels);
    }
    return 1;
}
#endif
