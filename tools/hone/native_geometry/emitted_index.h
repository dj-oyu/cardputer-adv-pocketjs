#ifndef HONE_EMITTED_INDEX_H
#define HONE_EMITTED_INDEX_H
#include "graph_compact_raster_candidate.h"
/* Bounds and band membership counts collected by the only writer of this
 * unpublished snapshot. The final prefix sum preserves packet order. */
typedef struct {
 unsigned bins[17],count,off_grid,left,right;
 float largest;
} emitted_index;

/* Closed viewport implies unsigned truncation equals floor. A lower bound
 * can be 240/135 even though the last drawable sample is 239/134. */
static inline bool sample_interval(float a,float b,float c,unsigned last,
                                   uint8_t *first,uint8_t *final,bool *grid){
 float lo=a<b?a:b,hi=a>b?a:b;
 if(c<lo)lo=c;if(c>hi)hi=c;
 unsigned upper=(unsigned)hi;
 if(upper>last)upper=last;
 if(lo>(float)upper)return false;
 unsigned lower=(unsigned)lo;
 *first=(uint8_t)(lower+(lo>(float)lower));*final=(uint8_t)upper;
 /* At and above 32 the least significant binary32 bit is already Q18. */
 if(lo<32.f && (!hone_flat_coord(a)||!hone_flat_coord(b)||!hone_flat_coord(c)))*grid=false;
 return true;
}

/* Called after all projected vertices were validated and clipped. */
static inline bool emitted_bounds(const hone_projected_packet *p,hone_compact_bounds *b) {
 const float *v=p->xyz;bool grid=true;
 if(!sample_interval(v[0],v[3],v[6],239,&b->x0,&b->x1,&grid)||
    !sample_interval(v[1],v[4],v[7],134,&b->y0,&b->y1,&grid))return false;
 b->grid=grid;return true;
}

static inline void emitted_account(emitted_index *e,hone_compact_bounds b,const float xyz[9]){
 if(!e->count||b.x0<e->left)e->left=b.x0;
 if(b.x1>=e->right)e->right=b.x1+1u;
 e->count++;e->off_grid+=!b.grid;
 for(unsigned k=2;k<9;k+=3){
  float z=fabsf(xyz[k]);if(z>e->largest)e->largest=z;
 }
 for(unsigned band=b.y0/8;band<=b.y1/8;band++)e->bins[band]++;
}

/* All arrays belong to the private fixture. No caller may mutate their
 * contents between emitting the packets and sealing and indexing the frame. */
static bool emitted_finish(const emitted_index *e,const hone_projected_snapshot *s,hone_compact_index *out,
                           hone_compact_bounds *bounds,size_t capacity,uint16_t *ids,size_t id_capacity){
 *out=(hone_compact_index){0};
 if(!hone_compact_readable(s)||e->count!=s->count||e->count>capacity)return false;
 hone_compact_index result={.source=s,.bounds=bounds,.ids=ids,.bounds_capacity=capacity,.ids_capacity=id_capacity,
    .owner=s->owner,.generation=s->generation,.revision=s->revision,.off_grid=e->off_grid,
    .bbox_span=e->count?e->right-e->left:0,.margin=e->largest*(128.f*FLT_EPSILON)+1e-28f};
 for(unsigned band=0;band<17;band++)result.offsets[band+1]=result.offsets[band]+e->bins[band];
 result.used=result.offsets[17];
 if(result.used>id_capacity)return false;
 size_t cursor[17];memcpy(cursor,result.offsets,sizeof cursor);
 for(unsigned i=0;i<e->count;i++)
  for(unsigned band=bounds[i].y0/8;band<=bounds[i].y1/8;band++)ids[cursor[band]++]=(uint16_t)i;
 *out=result;return true;
}
#endif
