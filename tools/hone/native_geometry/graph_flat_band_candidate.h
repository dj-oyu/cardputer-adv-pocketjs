#ifndef HONE_GRAPH_FLAT_BAND_CANDIDATE_H
#define HONE_GRAPH_FLAT_BAND_CANDIDATE_H
#include "graph_block_band_candidate.h"

/* Immutable frame/index/programs until the final band is consumed. This
 * value owns no buffers. Rebuild after any packet, material or driver change. */
typedef struct {
    const hone_graph_band_index *index;
    uint64_t revision;
    float compare_margin;
    unsigned flat, bbox_span;
} hone_flat_plan;

enum { HONE_FLAT_SCALE = 262144 };

static inline int hone_flat_constant(const hone_alpha_program *p,float *value) {
    if (!p || !p->code || p->count != 1 || p->result != 0 || p->code[0].op != HONE_ALPHA_CONST) return 0;
    *value=p->code[0].value;
    return *value>=0 && *value<=1;
}

static inline int hone_flat_coord(float value) {
    if (!(value>=0 && value<=255)) return 0;
    float scaled=value*HONE_FLAT_SCALE;
    return scaled==(float)(int32_t)scaled;
}

static inline hone_flat_plan hone_flat_prepare(const hone_graph_band_payload *p,
                                               const hone_graph_band_index *index,uint64_t revision) {
    hone_flat_plan plan={.index=index,.revision=revision};
    if (!p || !hone_graph_index_matches(index,&p->graph,revision)) return plan;
    unsigned left=240,right=0;
    float largest=0;
    for (size_t j=0; j<p->graph.count; j++) {
        const hone_graph_triangle *t=&p->graph.packets[j].triangle;
        float v;
        if (!hone_flat_constant(t->alpha,&v) || v!=1) return plan;
        for (unsigned k=0;k<3;k++) {
            if (!hone_flat_constant(&t->color[k],&v)) return plan;
            const hone_graph_vertex *a=&t->v[k];
            if (!hone_flat_coord(a->x) || !hone_flat_coord(a->y) || !(fabsf(a->z)<=1e20f)) return plan;
            if (fabsf(a->z)>largest) largest=fabsf(a->z);
            for (unsigned q=0;q<3;q++) if (!(fabsf(a->local[q])<=1e20f)) return plan;
        }
        hone_graph_bounds b=index->bounds[j];
        if (b.x0>b.x1 || b.y0>b.y1) continue;
        if (left>b.x0) left=b.x0;
        if (right<=b.x1) right=b.x1+1u;
    }
    /* Each positive integer barycentric numerator and denominator rounds once
     * to binary32, followed by reciprocal, multiply, depth products and two
     * additions. Weights sum to one exactly in integer space. 128 epsilons
     * covers both estimates and their reference binary32 rounding, including
     * cancellation; the absolute floor also covers subnormal/FTZ errors. */
    plan.compare_margin=largest*(128.f*FLT_EPSILON)+1e-28f;
    plan.bbox_span=right ? right-left : 0;
    plan.flat=1;
    return plan;
}

typedef struct { int32_t x,y; float z; } hone_flat_vertex;
static inline int64_t hone_flat_edge(hone_flat_vertex a,hone_flat_vertex b,int32_t x,int32_t y) {
    return (int64_t)(b.x-a.x)*(y-a.y)-(int64_t)(b.y-a.y)*(x-a.x);
}

/* Collision slow path. Integer products and their differences have at most
 * 53 bits, so reference binary64 edges and these integer edges differ only
 * by a power of two. Preserve the reference's per-weight division and order. */
static inline float hone_flat_exact(const hone_graph_triangle *t,unsigned x,unsigned y) {
    const hone_graph_vertex *v[3]={t->v,t->v+1,t->v+2};
    double area=hone_graph_edge(*v[0],*v[1],v[2]->x,v[2]->y);
    if (area<0) { const hone_graph_vertex *swap=v[1];v[1]=v[2];v[2]=swap;area=-area; }
    double depth=0;
    for (unsigned i=0;i<3;i++) {
        double w=hone_graph_edge(*v[(i+1)%3],*v[(i+2)%3],x,y)/area;
        depth+=w*v[i]->z;
    }
    return (float)depth;
}

static inline uint16_t hone_flat_rgb(const hone_graph_triangle *t) {
    const hone_alpha_program *c=t->color;
    return (uint16_t)(((unsigned)floorf(c[0].code[0].value*31+.5f)<<11) |
                     ((unsigned)floorf(c[1].code[0].value*63+.5f)<<5) |
                      (unsigned)floorf(c[2].code[0].value*31+.5f));
}

/* Empty opaque background only. Scratch depth holds the current winner's
 * estimate. The row-local winner ID allows exact reevaluation in close cases. */
static inline int hone_flat_row(const hone_graph_band_payload *p,const hone_flat_plan *plan,
                               hone_graph_band_work *w,unsigned y) {
    uint16_t winner[240];
    memset(winner,0xff,sizeof winner);
    memset(w->row,0,240*sizeof *w->row);
    const hone_graph_band_index *index=plan->index;
    size_t begin=index->offsets[y/8],end=index->offsets[y/8+1];
    if (end<begin || end>index->used || index->used>index->ids_capacity) return 0;
    unsigned left=240,right=0;
    for (size_t at=begin;at<end;at++) {
        unsigned id=index->ids[at];
        if (id>=p->graph.count) return 0;
        hone_graph_bounds b=index->bounds[id];
        if (y<b.y0 || y>b.y1) continue;
        const hone_graph_packet *packet=&p->graph.packets[id];
        const hone_graph_triangle *t=&packet->triangle;
        hone_flat_vertex v[3];
        for (unsigned i=0;i<3;i++) {
            v[i]=(hone_flat_vertex){(int32_t)(t->v[i].x*HONE_FLAT_SCALE),
                                   (int32_t)(t->v[i].y*HONE_FLAT_SCALE),t->v[i].z};
        }
        int64_t area=hone_flat_edge(v[0],v[1],v[2].x,v[2].y);
        if (!area) continue;
        if (area<0) { hone_flat_vertex tmp=v[1]; v[1]=v[2]; v[2]=tmp; area=-area; }
        int64_t e[3],step[3],threshold[3];
        for (unsigned i=0;i<3;i++) {
            hone_flat_vertex a=v[(i+1)%3],c=v[(i+2)%3];
            e[i]=hone_flat_edge(a,c,b.x0*HONE_FLAT_SCALE,y*HONE_FLAT_SCALE);
            step[i]=(int64_t)(a.y-c.y)*HONE_FLAT_SCALE;
            threshold[i]=!(c.y<a.y || (c.y==a.y && c.x>a.x));
        }
        float inv=1.f/(float)area;
        uint16_t color=hone_flat_rgb(t);
        for (unsigned x=b.x0;x<=b.x1;x++) {
            if (e[0]>=threshold[0] && e[1]>=threshold[1] && e[2]>=threshold[2]) {
                float approx=((float)e[0]*inv)*v[0].z+((float)e[1]*inv)*v[1].z+((float)e[2]*inv)*v[2].z;
                unsigned old=winner[x];
                int replace=old==UINT16_MAX;
                if (!replace) {
                    float diff=approx-w->depth[x];
                    if (fabsf(diff)>plan->compare_margin) replace=diff>0;
                    else {
                        const hone_graph_packet *prev=&p->graph.packets[old];
                        float z=hone_flat_exact(t,x,y), prior=hone_flat_exact(&prev->triangle,x,y);
                        replace=z>prior || (z==prior && packet->order<prev->order);
                    }
                }
                if (replace) {
                    winner[x]=(uint16_t)id; w->row[x]=color; w->depth[x]=approx;
                    if (left>x) left=x;
                    if (right<=x) right=x+1;
                }
            }
            for (unsigned i=0;i<3;i++) e[i]+=step[i];
        }
    }
    return !right || right-left<=p->span_limit;
}

static inline int hone_flat_available(const ksn_mesh_snapshot *opaque,const hone_flat_plan *plan) {
    return plan->flat && opaque && opaque->enabled && !opaque->packets.size;
}

/* The plan remains live through replay, including the fallback path. */
static inline int hone_flat_plan_disjoint(const hone_flat_plan *plan,const hone_graph_band_work *w,
                                          const uint16_t *pixels,unsigned rows) {
    return plan && w && w->scratch_count<=1024 && rows<=8 &&
           hone_alpha_disjoint(plan,sizeof *plan,w->row,240*sizeof *w->row) &&
           hone_alpha_disjoint(plan,sizeof *plan,w->depth,240*sizeof *w->depth) &&
           hone_alpha_disjoint(plan,sizeof *plan,w->scratch,w->scratch_count*sizeof *w->scratch) &&
           (!rows || hone_alpha_disjoint(plan,sizeof *plan,pixels,(size_t)rows*240*sizeof *pixels));
}

static inline int hone_flat_band_preflight(const ksn_mesh_snapshot *opaque,const hone_graph_band_payload *p,
                                         const hone_flat_plan *plan,hone_graph_band_work *w) {
    if (!hone_flat_plan_disjoint(plan,w,NULL,0)) return 0;
    if (!hone_flat_available(opaque,plan))
        return hone_graph_block_band_preflight(opaque,p,plan->index,plan->revision,w);
    if (!hone_graph_index_band_valid(opaque,p,plan->index,plan->revision,w,NULL,0)) return 0;
    if (plan->bbox_span<=p->span_limit) return 1;
    for (unsigned y=0;y<135;y++) if (!hone_flat_row(p,plan,w,y)) return 0;
    return 1;
}

static inline int hone_flat_band_render(const ksn_mesh_snapshot *opaque,const hone_graph_band_payload *p,
                                       const hone_flat_plan *plan,hone_graph_band_work *w,
                                       unsigned y,unsigned rows,uint16_t *pixels) {
    if (!hone_flat_plan_disjoint(plan,w,pixels,rows)) return 0;
    if (!hone_flat_available(opaque,plan))
        return hone_graph_block_band_render(opaque,p,plan->index,plan->revision,w,y,rows,pixels);
    if (y>135 || rows>8 || rows>135-y ||
        !hone_graph_index_band_valid(opaque,p,plan->index,plan->revision,w,pixels,(size_t)rows*240)) return 0;
    for (unsigned j=0;j<rows;j++) {
        if (!hone_flat_row(p,plan,w,y+j)) return 0;
        memcpy(pixels+(size_t)j*240,w->row,240*sizeof *pixels);
    }
    return 1;
}
#endif
