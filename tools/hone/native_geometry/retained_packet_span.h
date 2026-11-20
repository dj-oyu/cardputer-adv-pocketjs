#ifndef HONE_RETAINED_PACKET_SPAN_H
#define HONE_RETAINED_PACKET_SPAN_H
#include "emitted_index.h"

/* Only the current writer's prefix can have overwritten the previous frame.
 * The caller proves equality of geometry, projection, visibility, material
 * and authored order before asking to retain any span. The previous complete
 * frame and the current unpublished frame share exclusive packet/bounds arrays.
 * first/count address retained opaque packets; order/orders additionally
 * count empty boxes that consumed a global order without emitting a packet. */
typedef struct {uint16_t first,count,order,orders;} hone_retained_span;

static inline bool hone_retained_append(hone_retained_span old,hone_projected_snapshot *s,
                                        hone_compact_bounds *bounds,emitted_index *index,
                                        unsigned *next_order) {
    size_t first=s->count;
    unsigned order=*next_order;
    if(first>old.first || old.first>s->capacity || old.count>s->capacity-old.first ||
       index->count!=first || order>UINT16_MAX || old.orders>UINT16_MAX-order)return false;
    if(first!=old.first){
        memmove(s->packets+first,s->packets+old.first,old.count*sizeof *s->packets);
        memmove(bounds+first,bounds+old.first,old.count*sizeof *bounds);
    }
    for(unsigned i=0;i<old.count;i++){
        hone_projected_packet *p=s->packets+first+i;
        p->order=order+(p->order-old.order);
        emitted_account(index,bounds[first+i],p->xyz);
    }
    s->count+=old.count;*next_order+=old.orders;return true;
}
#endif
