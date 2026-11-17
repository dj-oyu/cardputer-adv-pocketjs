#ifndef HONE_GRAPH_COMPACT_RASTER_CANDIDATE_H
#define HONE_GRAPH_COMPACT_RASTER_CANDIDATE_H
#include "projected_snapshot_candidate.h"
#include "graph_flat_band_candidate.h"

typedef struct {uint8_t x0,x1,y0,y1,grid;} hone_compact_bounds;
typedef struct {
    const hone_projected_snapshot *source;
    hone_compact_bounds *bounds;
    uint16_t *ids;
    size_t bounds_capacity,ids_capacity,used,offsets[18];
    const void *owner;
    uint32_t generation;
    uint64_t revision;
    unsigned bbox_span,off_grid;
    float margin;
} hone_compact_index;
typedef struct {uint16_t winner[240]; float depth[240];} hone_compact_row_work;

static inline bool hone_compact_readable(const hone_projected_snapshot *s) {
    return s && s->initialized && s->ready && !s->building && !s->failed && s->owner &&
           s->revision && s->generation && s->count<=s->capacity && s->capacity<=65535 &&
           s->packets && s->materials && s->material_count && s->material_count<=65535;
}
static inline bool hone_compact_source_disjoint(const hone_projected_snapshot *s,const void *ptr,size_t n) {
    return !n || (ptr && hone_alpha_disjoint(ptr,n,s,sizeof *s) &&
           hone_alpha_disjoint(ptr,n,s->packets,s->capacity*sizeof *s->packets) &&
           hone_alpha_disjoint(ptr,n,s->materials,s->material_count*sizeof *s->materials));
}
/* Build into exclusive scratch. On failure, scratch may contain a partial
 * build and the index is invalidated after passing the alias checks. */
static inline bool hone_compact_build(hone_compact_index *out,const hone_projected_snapshot *s,
        hone_compact_bounds *bounds,size_t capacity,uint16_t *ids,size_t ids_capacity) {
    if (!hone_compact_readable(s) || !out || capacity<s->count || capacity>65535 ||
        (capacity && !bounds) || (ids_capacity && !ids) || ids_capacity>17u*65535) return false;
    const void *ptr[]={out,bounds,ids};
    size_t bytes[]={sizeof *out,capacity*sizeof *bounds,ids_capacity*sizeof *ids};
    for (unsigned i=0;i<3;i++) {
        if (!hone_compact_source_disjoint(s,ptr[i],bytes[i])) return false;
        for (unsigned j=0;j<i;j++) if (bytes[i] && bytes[j] &&
            !hone_alpha_disjoint(ptr[i],bytes[i],ptr[j],bytes[j])) return false;
    }
    *out=(hone_compact_index){0};
    hone_compact_index result={.bounds=bounds,.bounds_capacity=capacity,.ids=ids,.ids_capacity=ids_capacity,
                              .source=s,.owner=s->owner,.generation=s->generation,.revision=s->revision};
    unsigned left=240,right=0;
    float largest=0;
    for (size_t j=0;j<s->count;j++) {
        const hone_projected_packet *p=&s->packets[j];
        const hone_projected_material *m=hone_projected_color(s,p);
        if (!m) return false;
        for (unsigned k=0;k<3;k++) if (!(m->rgb[k]>=0 && m->rgb[k]<=1)) return false;
        float xmin=INFINITY,xmax=-INFINITY,ymin=INFINITY,ymax=-INFINITY;
        unsigned grid=1;
        for (unsigned k=0;k<3;k++) {
            const float *v=p->xyz+3*k;
            if (!isfinite(v[0]) || !isfinite(v[1]) || !(fabsf(v[2])<=1e20f)) return false;
            if (!hone_flat_coord(v[0]) || !hone_flat_coord(v[1])) grid=0;
            if (v[0]<xmin) xmin=v[0];
            if (v[0]>xmax) xmax=v[0];
            if (v[1]<ymin) ymin=v[1];
            if (v[1]>ymax) ymax=v[1];
            if (fabsf(v[2])>largest) largest=fabsf(v[2]);
        }
        /* The producer emits viewport-clipped packets. Restrict the precise
         * fallback to that domain to keep arithmetic/exact admission bounded. */
        if (xmin<0 || ymin<0 || xmax>240 || ymax>135) return false;
        if (!grid) result.off_grid++;
        hone_compact_bounds b={(uint8_t)ceilf(xmin),(uint8_t)floorf(fminf(xmax,239)),
                               (uint8_t)ceilf(ymin),(uint8_t)floorf(fminf(ymax,134)),(uint8_t)grid};
        bounds[j]=b;
        if (b.x0>b.x1 || b.y0>b.y1) continue;
        if (b.x0<left) left=b.x0;
        if (b.x1>=right) right=b.x1+1u;
        for (unsigned band=b.y0/8;band<=b.y1/8;band++) result.offsets[band+1]++;
    }
    for (unsigned j=0;j<17;j++) result.offsets[j+1]+=result.offsets[j];
    result.used=result.offsets[17];
    if (result.used>ids_capacity) return false;
    size_t cursor[17]; memcpy(cursor,result.offsets,sizeof cursor);
    for (size_t j=0;j<s->count;j++) {
        hone_compact_bounds b=bounds[j];
        if (b.x0<=b.x1 && b.y0<=b.y1)
            for(unsigned band=b.y0/8;band<=b.y1/8;band++) ids[cursor[band]++]=(uint16_t)j;
    }
    result.bbox_span=right ? right-left : 0;
    result.margin=largest*(128.f*FLT_EPSILON)+1e-28f;
    *out=result;
    return true;
}

static inline double hone_compact_edge(const float *a,const float *b,double x,double y) {
    return ((double)b[0]-a[0])*(y-a[1])-((double)b[1]-a[1])*(x-a[0]);
}
/* Direct original arithmetic for sub-grid coverage, and for ambiguous depth.
 * Uses vertex pointers into the 44-byte source, with no graph expansion. */
static inline bool hone_compact_sample(const hone_projected_packet *p,unsigned x,unsigned y,float *z) {
    const float *v[3]={p->xyz,p->xyz+3,p->xyz+6};
    double a=hone_compact_edge(v[0],v[1],v[2][0],v[2][1]);
    if (!a) return false;
    if (a<0) { const float *temp=v[1];v[1]=v[2];v[2]=temp;a=-a; }
    double depth=0;
    for(unsigned k=0;k<3;k++) {
        const float *u=v[(k+1)%3],*w=v[(k+2)%3];
        double e=hone_compact_edge(u,w,x,y);
        if(e<0 || (e==0 && !(w[1]<u[1] || (w[1]==u[1] && w[0]>u[0])))) return false;
        depth+=(e/a)*v[k][2];
    }
    *z=(float)depth;
    return true;
}

static inline void hone_compact_hit(const hone_compact_index *idx,hone_compact_row_work *w,
        unsigned id,unsigned x,unsigned y,float z,uint16_t color,uint16_t *row) {
    unsigned old=w->winner[x];
    if (old!=UINT16_MAX) {
        float diff=z-w->depth[x];
        if (fabsf(diff)>idx->margin) { if (diff<0) return; }
        else {
            const hone_projected_packet *a=&idx->source->packets[id],*b=&idx->source->packets[old];
            float za,zb;
            if (!hone_compact_sample(a,x,y,&za) || !hone_compact_sample(b,x,y,&zb)) return;
            if (za<zb || (za==zb && a->order>=b->order)) return;
        }
    }
    w->winner[x]=(uint16_t)id;w->depth[x]=z;row[x]=color;
}
static inline void hone_compact_grid_span(const hone_compact_index *idx,hone_compact_row_work *w,
        unsigned id,hone_compact_bounds b,unsigned y,uint16_t color,uint16_t *row) {
    const float *xyz=idx->source->packets[id].xyz;
    hone_flat_vertex v[3];
    for (unsigned k=0;k<3;k++)
        v[k]=(hone_flat_vertex){(int32_t)(xyz[3*k]*HONE_FLAT_SCALE),(int32_t)(xyz[3*k+1]*HONE_FLAT_SCALE),xyz[3*k+2]};
    int64_t area=hone_flat_edge(v[0],v[1],v[2].x,v[2].y);
    if (!area) return;
    if (area<0) { hone_flat_vertex temp=v[1]; v[1]=v[2]; v[2]=temp; area=-area; }
    float inv=1.f/(float)area;
    int64_t e[3],step[3],closed_min[3];
    for (unsigned k=0;k<3;k++) {
        hone_flat_vertex a=v[(k+1)%3],c=v[(k+2)%3];
        e[k]=hone_flat_edge(a,c,b.x0*HONE_FLAT_SCALE,y*HONE_FLAT_SCALE);
        step[k]=(int64_t)(a.y-c.y)*HONE_FLAT_SCALE;
        closed_min[k]=!(c.y<a.y || (c.y==a.y && c.x>a.x));
    }
    for (unsigned x=b.x0;x<=b.x1;x++) {
        if (e[0]>=closed_min[0] && e[1]>=closed_min[1] && e[2]>=closed_min[2]) {
            float z=((float)e[0]*inv)*v[0].z+((float)e[1]*inv)*v[1].z+((float)e[2]*inv)*v[2].z;
            hone_compact_hit(idx,w,id,x,y,z,color,row);
        }
        for (unsigned k=0;k<3;k++) e[k]+=step[k];
    }
}
static inline bool hone_compact_disjoint(const hone_compact_index *idx,const void *p,size_t bytes) {
    return hone_compact_source_disjoint(idx->source,p,bytes) && hone_alpha_disjoint(idx,sizeof *idx,p,bytes) &&
           (!idx->bounds_capacity || hone_alpha_disjoint(idx->bounds,idx->bounds_capacity*sizeof *idx->bounds,p,bytes)) &&
           (!idx->ids_capacity || hone_alpha_disjoint(idx->ids,idx->ids_capacity*sizeof *idx->ids,p,bytes));
}
/* O(1) epoch/range validation per band. Sealed frame and index remain immutable
 * under the caller's exclusive/pinned ownership through transfer completion. */
static inline bool hone_compact_render(const hone_compact_index *idx,const void *owner,uint32_t generation,
        uint64_t revision,hone_compact_row_work *w,unsigned y,unsigned rows,uint16_t *pixels) {
    if (!idx || !w || !hone_compact_readable(idx->source) || idx->revision!=revision ||
        idx->generation!=generation || idx->owner!=owner || idx->source->revision!=revision ||
        idx->source->generation!=generation || idx->source->owner!=owner ||
        y>135 || rows>8 || rows>135-y || (rows && !pixels)) return false;
    size_t bytes=(size_t)rows*240*sizeof *pixels;
    if (!hone_compact_disjoint(idx,w,sizeof *w) || (bytes &&
       (!hone_compact_disjoint(idx,pixels,bytes) || !hone_alpha_disjoint(w,sizeof *w,pixels,bytes)))) return false;
    for (unsigned j=0;j<rows;j++) {
        unsigned yy=y+j;
        uint16_t *row=pixels+j*240;
        memset(row,0,240*sizeof *row); memset(w->winner,0xff,sizeof w->winner);
        const hone_projected_snapshot *s=idx->source;
        for (size_t at=idx->offsets[yy/8];at<idx->offsets[yy/8+1];at++) {
            unsigned id=idx->ids[at];
            hone_compact_bounds b=idx->bounds[id];
            if (yy<b.y0 || yy>b.y1) continue;
            const hone_projected_packet *p=&s->packets[id];
            const float *c=s->materials[p->material].rgb;
            uint16_t color=(uint16_t)(((unsigned)floorf(c[0]*31+.5f)<<11) |
                           ((unsigned)floorf(c[1]*63+.5f)<<5) | (unsigned)floorf(c[2]*31+.5f));
            if (b.grid) hone_compact_grid_span(idx,w,id,b,yy,color,row);
            else for (unsigned x=b.x0;x<=b.x1;x++) {
                float z;
                if (hone_compact_sample(p,x,yy,&z)) hone_compact_hit(idx,w,id,x,yy,z,color,row);
            }
        }
    }
    return true;
}
#endif
