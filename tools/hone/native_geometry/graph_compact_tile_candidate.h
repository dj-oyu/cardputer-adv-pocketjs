#ifndef HONE_GRAPH_COMPACT_TILE_CANDIDATE_H
#define HONE_GRAPH_COMPACT_TILE_CANDIDATE_H
#include "graph_compact_raster_candidate.h"

/* 8 x 45 depth estimates. The pending private output stores winner IDs
 * until the tile has been completely resolved. */
enum { HONE_COMPACT_TILE_W=45, HONE_COMPACT_TILE_H=8 };
typedef struct {float depth[HONE_COMPACT_TILE_W*HONE_COMPACT_TILE_H];} hone_compact_tile_work;
_Static_assert(sizeof(hone_compact_tile_work)==sizeof(hone_compact_row_work),"tile scratch exceeds row budget");

typedef struct {uint32_t material;uint16_t rgb;} hone_compact_tile_color;
static inline uint16_t hone_compact_tile_rgb(const hone_projected_snapshot *s,uint32_t m,
                                             hone_compact_tile_color cache[4]) {
    hone_compact_tile_color *slot=cache+(m&3);
    if (slot->material!=m) {
        const float *c=s->materials[m].rgb;
        slot->material=m;
        slot->rgb=hone_compact_material_rgb565(c);
    }
    return slot->rgb;
}
static inline uint16_t hone_compact_tile_packet_rgb(const hone_projected_snapshot *s,unsigned id,
                                                    hone_compact_tile_color cache[4]) {
    return s->packet_rgb565 ? s->packet_rgb565[id] :
           hone_compact_tile_rgb(s,s->packets[id].material,cache);
}
/* Keep binary64 sampling out of the common integer/FPU span. */
static __attribute__((noinline)) bool hone_compact_nearer_exact(const hone_compact_index *idx,
        unsigned id,unsigned prior,unsigned x,unsigned y) {
    const hone_projected_packet *a=idx->source->packets+id,*b=idx->source->packets+prior;
    float za,zb;
    if (!hone_compact_sample(a,x,y,&za) || !hone_compact_sample(b,x,y,&zb)) return false;
    return za>zb || (za==zb && a->order<b->order);
}
static inline void hone_compact_tile_hit(const hone_compact_index *idx,unsigned id,unsigned x,unsigned y,
                                          float incoming,float *estimate,uint16_t *winner) {
    if (*winner!=UINT16_MAX) {
        float delta=incoming-*estimate;
        if (fabsf(delta)>idx->margin) { if (delta<0) return; }
        else if (!hone_compact_nearer_exact(idx,id,*winner,x,y)) return;
    }
    *winner=(uint16_t)id;
    *estimate=incoming;
}
/* p and z point to the upper-left of the packet/tile intersection. */
static inline void hone_compact_tile_grid64(const hone_compact_index *idx,unsigned id,unsigned x0,unsigned x1,
        unsigned y0,unsigned y1,float *z,uint16_t *p) {
    const float *xyz=idx->source->packets[id].xyz;
    hone_flat_vertex v[3];
    for (unsigned k=0;k<3;k++)
        v[k]=(hone_flat_vertex){(int32_t)(xyz[3*k]*HONE_FLAT_SCALE),
                                (int32_t)(xyz[3*k+1]*HONE_FLAT_SCALE),xyz[3*k+2]};
    int64_t area=hone_flat_edge(v[0],v[1],v[2].x,v[2].y);
    if (!area) return;
    if (area<0) {hone_flat_vertex tmp=v[1];v[1]=v[2];v[2]=tmp;area=-area;}
    float inv=1.f/(float)area;
    int64_t first[3],dx[3],dy[3],limit[3];
    for (unsigned k=0;k<3;k++) {
        hone_flat_vertex a=v[(k+1)%3],b=v[(k+2)%3];
        first[k]=hone_flat_edge(a,b,x0*HONE_FLAT_SCALE,y0*HONE_FLAT_SCALE);
        dx[k]=(int64_t)(a.y-b.y)*HONE_FLAT_SCALE;
        dy[k]=(int64_t)(b.x-a.x)*HONE_FLAT_SCALE;
        limit[k]=!(b.y<a.y || (a.y==b.y && b.x>a.x));
    }
    for (unsigned y=y0;y<=y1;y++) {
        const unsigned row=y-y0;
        uint16_t *win=p+240*row;
        float *depth=z+HONE_COMPACT_TILE_W*row;
        int64_t e[3]={first[0],first[1],first[2]};
        for (unsigned x=x0;x<=x1;x++) {
            if (e[0]>=limit[0] && e[1]>=limit[1] && e[2]>=limit[2]) {
                float approx=((float)e[0]*inv)*v[0].z+((float)e[1]*inv)*v[1].z+((float)e[2]*inv)*v[2].z;
                hone_compact_tile_hit(idx,id,x,y,approx,depth+(x-x0),win+(x-x0));
            }
            for(unsigned k=0;k<3;k++) e[k]+=dx[k];
        }
        for(unsigned k=0;k<3;k++) first[k]+=dy[k];
    }
}

static inline void hone_compact_tile_grid(const hone_compact_index *idx,unsigned id,unsigned x0,unsigned x1,
        unsigned y0,unsigned y1,float *z,uint16_t *p) {
    /* The determinant is bounded by twice the product of the axis spans.
     * The four pixels of padding cover both fractional endpoints and the
     * final unconsumed +x/+y increment in the integer loops. */
    hone_compact_bounds bounds=idx->bounds[id];
    unsigned box=(bounds.x1-bounds.x0+4u)*(bounds.y1-bounds.y0+4u);
    if(box>4000u) {hone_compact_tile_grid64(idx,id,x0,x1,y0,y1,z,p);return;}
    const float *xyz=idx->source->packets[id].xyz;
    hone_flat_vertex v[3];
    for (unsigned k=0;k<3;k++)
        v[k]=(hone_flat_vertex){(int32_t)(xyz[3*k]*HONE_FLAT_SCALE),
                                (int32_t)(xyz[3*k+1]*HONE_FLAT_SCALE),xyz[3*k+2]};
    int64_t area=hone_flat_edge(v[0],v[1],v[2].x,v[2].y);
    if (!area) return;
    if (area<0) {hone_flat_vertex tmp=v[1];v[1]=v[2];v[2]=tmp;area=-area;}
    if(area>(int64_t)(INT32_MAX/2)*HONE_FLAT_SCALE) {
        hone_compact_tile_grid64(idx,id,x0,x1,y0,y1,z,p);return;
    }
    /* With area/S <= 2^30, this ensures the nonzero coefficients stay
     * normal even on FTZ hardware. All rejected inputs use the original
     * estimate and exact collision fallback. */
    for (unsigned k=0;k<3;k++) if (v[k].z!=0.f && fabsf(v[k].z)<0x1p-80f) {
        hone_compact_tile_grid64(idx,id,x0,x1,y0,y1,z,p);return;
    }
    float inv=(float)HONE_FLAT_SCALE/(float)area;
    int32_t first[3],dx[3],dy[3],limit[3];
    float coefficient[3],bias=0;
    for (unsigned k=0;k<3;k++) {
        hone_flat_vertex a=v[(k+1)%3],b=v[(k+2)%3];
        int64_t e=hone_flat_edge(a,b,x0*HONE_FLAT_SCALE,y0*HONE_FLAT_SCALE);
        uint32_t remainder=(uint32_t)e & (HONE_FLAT_SCALE-1u);
        first[k]=(int32_t)((e-remainder)/HONE_FLAT_SCALE);
        coefficient[k]=inv*v[k].z;
        float fraction=(float)remainder*(1.f/HONE_FLAT_SCALE);
        bias+=fraction*coefficient[k];
        dx[k]=a.y-b.y;
        dy[k]=b.x-a.x;
        limit[k]=!remainder && !(b.y<a.y || (a.y==b.y && b.x>a.x));
    }
    for (unsigned y=y0;y<=y1;y++) {
        unsigned row=y-y0;
        uint16_t *win=p+240*row;
        float *depth=z+HONE_COMPACT_TILE_W*row;
        int32_t e[3]={first[0],first[1],first[2]};
        for (unsigned x=x0;x<=x1;x++) {
            if(e[0]>=limit[0] && e[1]>=limit[1] && e[2]>=limit[2]) {
                /* All integer quotients and residuals are nonnegative here.
                 * Their absolute contributions sum to at most max |z|, so
                 * subtraction between unlike z values does not amplify the
                 * absolute error used by the exact collision test. */
                float approx=(float)e[0]*coefficient[0]+(float)e[1]*coefficient[1]
                            +(float)e[2]*coefficient[2]+bias;
                hone_compact_tile_hit(idx,id,x,y,approx,depth+(x-x0),win+(x-x0));
            }
            for(unsigned k=0;k<3;k++) e[k]+=dx[k];
        }
        for(unsigned k=0;k<3;k++) first[k]+=dy[k];
    }
}
static inline void hone_compact_tile_strip(const hone_compact_index *idx,hone_compact_tile_work *w,
        unsigned y,unsigned rows,uint16_t *pixels,hone_compact_tile_color cache[4]) {
    size_t begin=idx->offsets[y/8],end=idx->offsets[y/8+1];
    unsigned xmin=240,xend=0;
    for (size_t at=begin;at<end;at++) {
        hone_compact_bounds b=idx->bounds[idx->ids[at]];
        if (b.y1<y || b.y0>=y+rows) continue;
        if (b.x0<xmin) xmin=b.x0;
        if (b.x1>=xend) xend=b.x1+1u;
    }
    memset(pixels,0,(size_t)rows*240*sizeof *pixels);
    for (unsigned tile=xmin;tile<xend;tile+=HONE_COMPACT_TILE_W) {
        unsigned last=tile+HONE_COMPACT_TILE_W-1;
        if (last>=xend) last=xend-1;
        for (unsigned r=0;r<rows;r++)
            memset(pixels+240*r+tile,0xff,(last-tile+1)*sizeof *pixels);
        for (size_t at=begin;at<end;at++) {
            unsigned id=idx->ids[at];
            hone_compact_bounds b=idx->bounds[id];
            unsigned x0=b.x0>tile ? b.x0 : tile, x1=b.x1<last ? b.x1 : last;
            unsigned y0=b.y0>y ? b.y0 : y, y1=b.y1<y+rows ? b.y1 : y+rows-1;
            if (x0>x1 || y0>y1) continue;
            unsigned row=y0-y;
            if (b.grid) {
                hone_compact_tile_grid(idx,id,x0,x1,y0,y1,
                    w->depth+row*HONE_COMPACT_TILE_W+(x0-tile),pixels+row*240+x0);
            } else {
                const hone_projected_packet *packet=idx->source->packets+id;
                for (unsigned yy=y0;yy<=y1;yy++) for (unsigned x=x0;x<=x1;x++) {
                    float z;
                    if (hone_compact_sample(packet,x,yy,&z))
                        hone_compact_tile_hit(idx,id,x,yy,z,w->depth+(yy-y)*HONE_COMPACT_TILE_W+(x-tile),
                                              pixels+(yy-y)*240+x);
                }
            }
        }
        for (unsigned r=0;r<rows;r++) {
            uint16_t *row=pixels+r*240;
            for (unsigned x=tile;x<=last;x++) {
                unsigned id=row[x];
                row[x]=id==UINT16_MAX ? 0 : hone_compact_tile_packet_rgb(idx->source,id,cache);
            }
        }
    }
}
static inline bool hone_compact_tile_render(const hone_compact_index *idx,const void *owner,uint32_t generation,
        uint64_t revision,hone_compact_tile_work *w,unsigned y,unsigned rows,uint16_t *pixels) {
    if (!idx || !w || !hone_compact_readable(idx->source) || idx->revision!=revision ||
        idx->generation!=generation || idx->owner!=owner || idx->source->revision!=revision ||
        idx->source->generation!=generation || idx->source->owner!=owner ||
        y>135 || rows>8 || rows>135-y || (rows && !pixels)) return false;
    size_t bytes=(size_t)rows*240*sizeof *pixels;
    if (!hone_compact_disjoint(idx,w,sizeof *w) || (bytes &&
       (!hone_compact_disjoint(idx,pixels,bytes) || !hone_alpha_disjoint(w,sizeof *w,pixels,bytes)))) return false;
    hone_compact_tile_color cache[4];
    for(unsigned k=0;k<4;k++) cache[k].material=UINT32_MAX;
    while (rows) {
        unsigned n=8-y%8;
        if (n>rows) n=rows;
        hone_compact_tile_strip(idx,w,y,n,pixels,cache);
        y+=n;rows-=n;pixels+=n*240;
    }
    return true;
}
#endif
