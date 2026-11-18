/* Streaming, bounded eye fragments for a compact, opaque scene. Input order
 * is the unique monotone order of the source packet stream (including flat
 * packets), retained unchanged in compact packet.order. */
#ifndef HONE_EYE_LEDGER_H
#define HONE_EYE_LEDGER_H
#include "graph_compact_tile_candidate.h"
typedef struct { float z; uint16_t xy,order,color,next; } hone_eye_pixel;
enum { HONE_EYE_SLOTS=512, HONE_EYE_EMPTY=UINT16_MAX };
typedef struct {
    hone_eye_pixel slots[HONE_EYE_SLOTS];
    uint16_t heads[17],count,last_order;
    bool has_order,failed,sealed;
    uint64_t revision;
} hone_eye_ledger;

static inline void hone_eye_begin(hone_eye_ledger *s,uint64_t revision) {
    memset(s,0,sizeof *s);memset(s->heads,0xff,sizeof s->heads);
    s->revision=revision;s->failed=!revision;
}
static inline bool hone_eye_store(hone_eye_ledger *s,unsigned x,unsigned y,unsigned order,float z,uint16_t color) {
    unsigned key=240*y+x+1;
    unsigned at=(key*2654435761u)>>(32-9);
    for(unsigned i=0;i<HONE_EYE_SLOTS;i++,at=(at+1)&(HONE_EYE_SLOTS-1)) {
        hone_eye_pixel *p=s->slots+at;
        if(!p->xy) {
            *p=(hone_eye_pixel){z,(uint16_t)key,(uint16_t)order,color,s->heads[y/8]};
            s->heads[y/8]=(uint16_t)at;s->count++;return true;
        }
        if(p->xy==key) {
            if(z>p->z || (z==p->z && order<p->order)) {
                p->z=z;p->order=(uint16_t)order;p->color=color;
            }
            return true;
        }
    }
    s->failed=true;return false;
}
typedef bool (*hone_eye_shader)(void *,unsigned,const float[3],float *,float[3]);
/* Only evaluated integer samples may contribute. All source coordinates must
 * have been validated before viewport clipping and remain finite after it.
 * The shader must return alpha exactly zero/one (fractional rejects atomically).
 * Every covered fragment is evaluated, including fragments behind opaque ones. */
static __attribute__((noinline)) bool hone_eye_emit_general(hone_eye_ledger *s,const hone_graph_triangle *t,unsigned material,
        unsigned order,hone_eye_shader shade,void *shader_context) {
    if(!s || s->failed || s->sealed) return false;
    if(!t || !shade || order>=UINT16_MAX || (s->has_order && order<=s->last_order)) goto fail;
    s->has_order=true;s->last_order=(uint16_t)order;
    hone_graph_bounds b;
    if(!hone_graph_index_bounds(t,&b))goto fail;
    if(b.x0>b.x1 || b.y0>b.y1)return true;
    const hone_graph_vertex *v[3]={t->v,t->v+1,t->v+2};
    double area=hone_graph_edge(*v[0],*v[1],v[2]->x,v[2]->y);
    if(!area)return true;
    if(area<0){const hone_graph_vertex *swap=v[1];v[1]=v[2];v[2]=swap;area=-area;}
    for(unsigned y=b.y0;y<=b.y1;y++)for(unsigned x=b.x0;x<=b.x1;x++) {
        double weights[3];unsigned k;
        for(k=0;k<3;k++) {
            const hone_graph_vertex *a=v[(k+1)%3],*c=v[(k+2)%3];
            double e=hone_graph_edge(*a,*c,x,y);
            if(!isfinite(e))goto fail;
            if(e<0 || (e==0 && !(c->y<a->y || (c->y==a->y && c->x>a->x))))break;
            weights[k]=e/area;
        }
        if(k<3)continue;
        double z=0,local[3]={0,0,0};
        for(k=0;k<3;k++){
            z+=weights[k]*v[k]->z;
            for(unsigned j=0;j<3;j++)local[j]+=weights[k]*v[k]->local[j];
        }
        if(!isfinite(z)||fabs(z)>FLT_MAX)goto fail;
        float xyz[3];for(k=0;k<3;k++){if(!isfinite(local[k])||fabs(local[k])>FLT_MAX)goto fail;xyz[k]=(float)local[k];}
        float alpha,rgb[3];
        if(!shade(shader_context,material,xyz,&alpha,rgb))goto fail;
        if(alpha==0)continue;
        if(alpha!=1)goto fail;
        for(k=0;k<3;k++)if(!(rgb[k]>=0 && rgb[k]<=1))goto fail;
        uint16_t packed=(uint16_t)(((unsigned)floorf(rgb[0]*31+.5f)<<11)|
                       ((unsigned)floorf(rgb[1]*63+.5f)<<5)|(unsigned)floorf(rgb[2]*31+.5f));
        if(!hone_eye_store(s,x,y,order,(float)z,packed))return false;
    }
    return true;
fail:
    s->failed=true;return false;
}
/* Q18 coverage and all graph interpolation use the same winding/order.
 * Edges and the area are exact integers < 2^53: multiplying either operand
 * of the reference weight division by the common factor 2^36 leaves the
 * correctly rounded binary64 result unchanged. */
static bool hone_eye_emit(hone_eye_ledger *s,const hone_graph_triangle *t,unsigned material,
        unsigned order,hone_eye_shader shade,void *shader_context) {
    if(!s || s->failed || s->sealed)return false;
    if(!t || !t->alpha || !t->color || !shade || order>=UINT16_MAX ||
       (s->has_order && order<=s->last_order))goto fail;
    hone_flat_vertex v[3];const hone_graph_vertex *src[3]={t->v,t->v+1,t->v+2};
    int32_t xmin=INT32_MAX,xmax=0,ymin=INT32_MAX,ymax=0;
    for(unsigned k=0;k<3;k++){
        const hone_graph_vertex *a=src[k];
        if(!hone_flat_coord(a->x)||!hone_flat_coord(a->y) || !(fabsf(a->z)<=1e20f))
            return hone_eye_emit_general(s,t,material,order,shade,shader_context);
        for(unsigned j=0;j<3;j++)if(!(fabsf(a->local[j])<=1e20f))
            return hone_eye_emit_general(s,t,material,order,shade,shader_context);
        int32_t ix=(int32_t)(a->x*HONE_FLAT_SCALE),iy=(int32_t)(a->y*HONE_FLAT_SCALE);
        v[k]=(hone_flat_vertex){ix,iy,a->z};
        if(ix<xmin)xmin=ix;if(ix>xmax)xmax=ix;
        if(iy<ymin)ymin=iy;if(iy>ymax)ymax=iy;
    }
    s->has_order=true;s->last_order=(uint16_t)order;
    unsigned x0=(unsigned)(xmin+HONE_FLAT_SCALE-1)/HONE_FLAT_SCALE,x1=(unsigned)xmax/HONE_FLAT_SCALE;
    unsigned y0=(unsigned)(ymin+HONE_FLAT_SCALE-1)/HONE_FLAT_SCALE,y1=(unsigned)ymax/HONE_FLAT_SCALE;
    if(x1>239)x1=239;if(y1>134)y1=134;
    if(x0>x1 || y0>y1)return true;
    int64_t area=hone_flat_edge(v[0],v[1],v[2].x,v[2].y);
    if(!area)return true;
    if(area<0){
        hone_flat_vertex temp=v[1];v[1]=v[2];v[2]=temp;
        const hone_graph_vertex *a=src[1];src[1]=src[2];src[2]=a;
        area=-area;
    }
    double attr[3][4],den=(double)area;
    int64_t first[3],dx[3],dy[3],limit[3];
    for(unsigned k=0;k<3;k++){
        hone_flat_vertex a=v[(k+1)%3],b=v[(k+2)%3];
        first[k]=hone_flat_edge(a,b,x0*HONE_FLAT_SCALE,y0*HONE_FLAT_SCALE);
        dx[k]=(int64_t)(a.y-b.y)*HONE_FLAT_SCALE;
        dy[k]=(int64_t)(b.x-a.x)*HONE_FLAT_SCALE;
        limit[k]=!(b.y<a.y || (b.y==a.y && b.x>a.x));
        attr[k][0]=src[k]->local[0];attr[k][1]=src[k]->local[1];attr[k][2]=src[k]->local[2];attr[k][3]=src[k]->z;
    }
    for(unsigned y=y0;y<=y1;y++){
        int64_t e[3]={first[0],first[1],first[2]};
        for(unsigned x=x0;x<=x1;x++){
            if(e[0]>=limit[0] && e[1]>=limit[1] && e[2]>=limit[2]){
                double weight[3]={(double)e[0]/den,(double)e[1]/den,(double)e[2]/den};
                float xyz[4];
                for(unsigned j=0;j<4;j++){
                    double value=0.;
                    for(unsigned k=0;k<3;k++)value+=weight[k]*attr[k][j];
                    xyz[j]=(float)value;
                }
                float alpha,rgb[3];
                if(!shade(shader_context,material,xyz,&alpha,rgb))goto fail;
                if(alpha!=0.f){
                    if(alpha!=1.f)goto fail;
                    for(unsigned j=0;j<3;j++)if(!(rgb[j]>=0.f && rgb[j]<=1.f))goto fail;
                    uint16_t packed=(uint16_t)(((unsigned)floorf(rgb[0]*31+.5f)<<11)|
                            ((unsigned)floorf(rgb[1]*63+.5f)<<5)|(unsigned)floorf(rgb[2]*31+.5f));
                    if(!hone_eye_store(s,x,y,order,xyz[3],packed))return false;
                }
            }
            for(unsigned k=0;k<3;k++)e[k]+=dx[k];
        }
        for(unsigned k=0;k<3;k++)first[k]+=dy[k];
    }
    return true;
fail:
    s->failed=true;return false;
}

static inline bool hone_eye_seal(hone_eye_ledger *s){
    if(!s || s->failed || s->sealed)return false;
    s->sealed=true;return true;
}

/* Resolve after the compact kernel writes ordinary winner IDs, before RGB565
 * conversion. output is a private full-width band; errors can change private
 * scratch only. Includes masks outside every ordinary opaque packet bound. */
static inline bool hone_eye_resolve(hone_eye_ledger const *s,hone_compact_index const *idx,
        unsigned y,unsigned rows,uint16_t *pixels){
    for(unsigned at=s->heads[y/8];at!=HONE_EYE_EMPTY;at=s->slots[at].next){
        const hone_eye_pixel *p=s->slots+at;
        unsigned key=p->xy-1,yy=key/240;
        if(yy<y || yy>=y+rows)continue;
        unsigned x=key%240;uint16_t *id=pixels+(yy-y)*240+x;
        if(*id!=UINT16_MAX) {
            const hone_projected_packet *flat=idx->source->packets+*id;
            float z;
            if(!hone_compact_sample(flat,x,yy,&z))return false;
            if(z>p->z || (z==p->z && flat->order<p->order))continue;
        }
        *id=(uint16_t)(0x8000u|at);
    }
    return true;
}
static inline bool hone_eye_render_band(const hone_eye_ledger *s,const hone_compact_index *idx,
        const void *owner,uint32_t generation,uint64_t revision,hone_compact_tile_work *w,
        unsigned y,unsigned rows,uint16_t *pixels){
    if(!s || !idx || !w || !s->sealed || s->failed || s->revision!=revision ||
       y>=135 || (y&7u) || !rows || rows>8 || rows>135-y || !pixels ||
       !hone_compact_readable(idx->source) || idx->revision!=revision || idx->source->revision!=revision ||
       idx->owner!=owner || idx->source->owner!=owner || idx->generation!=generation ||
       idx->source->generation!=generation || idx->source->count>=0x8000)return false;
    size_t bytes=rows*240*sizeof *pixels;
    if(!hone_compact_disjoint(idx,w,sizeof *w) || !hone_compact_disjoint(idx,pixels,bytes) ||
       !hone_alpha_disjoint(w,sizeof *w,pixels,bytes) || !hone_alpha_disjoint(s,sizeof *s,pixels,bytes) ||
       !hone_alpha_disjoint(s,sizeof *s,w,sizeof *w))return false;
    if(s->heads[y/8]==HONE_EYE_EMPTY)return hone_compact_tile_render(idx,owner,generation,revision,w,y,rows,pixels);
    memset(pixels,0xff,bytes);
    size_t begin=idx->offsets[y/8],end=idx->offsets[y/8+1];
    unsigned xmin=240,xend=0;
    for(size_t at=begin;at<end;at++){
        hone_compact_bounds b=idx->bounds[idx->ids[at]];
        if(b.x0<xmin)xmin=b.x0;if(b.x1>=xend)xend=b.x1+1u;
    }
    for(unsigned tile=xmin;tile<xend;tile+=HONE_COMPACT_TILE_W){
        unsigned last=tile+HONE_COMPACT_TILE_W-1;
        if(last>=xend)last=xend-1;
        for(size_t at=begin;at<end;at++){
            unsigned id=idx->ids[at];hone_compact_bounds b=idx->bounds[id];
            unsigned x0=b.x0>tile?b.x0:tile,x1=b.x1<last?b.x1:last;
            unsigned y0=b.y0>y?b.y0:y,y1=b.y1<y+rows?b.y1:y+rows-1;
            if(x0>x1 || y0>y1)continue;
            unsigned row=y0-y;
            if(b.grid)hone_compact_tile_grid(idx,id,x0,x1,y0,y1,
                    w->depth+row*HONE_COMPACT_TILE_W+(x0-tile),pixels+row*240+x0);
            else {
                const hone_projected_packet *p=idx->source->packets+id;
                for(unsigned yy=y0;yy<=y1;yy++)for(unsigned x=x0;x<=x1;x++){
                    float z;if(hone_compact_sample(p,x,yy,&z))
                        hone_compact_tile_hit(idx,id,x,yy,z,w->depth+(yy-y)*HONE_COMPACT_TILE_W+(x-tile),
                                              pixels+(yy-y)*240+x);
                }
            }
        }
    }
    if(!hone_eye_resolve(s,idx,y,rows,pixels))return false;
    hone_compact_tile_color cache[4];
    for(unsigned k=0;k<4;k++)cache[k].material=UINT32_MAX;
    for(unsigned i=0;i<rows*240;i++) {
        unsigned id=pixels[i];
        if(id==UINT16_MAX)pixels[i]=0;
        else if(id&0x8000u)pixels[i]=s->slots[id&0x7fffu].color;
        else pixels[i]=hone_compact_tile_rgb(idx->source,idx->source->packets[id].material,cache);
    }
    return true;
}
#endif
