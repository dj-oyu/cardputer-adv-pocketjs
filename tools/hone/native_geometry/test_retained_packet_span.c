#include "retained_packet_span.h"
#include <assert.h>

int main(void){
    hone_projected_packet packets[8]={0};
    hone_compact_bounds bounds[8]={0};
    hone_projected_snapshot s={.capacity=8,.count=1,.packets=packets};
    emitted_index index={.count=1};
    for(unsigned n=0;n<8;n++){
        packets[n].order=11+n;
        packets[n].xyz[2]=(float)(10+n);
        bounds[n]=(hone_compact_bounds){2,4,(uint8_t)(n*8),(uint8_t)(n*8),1};
    }
    unsigned order=20;
    /* A left move can overlap unread packets in the current route. */
    hone_retained_span old={3,4,13,7};
    assert(hone_retained_append(old,&s,bounds,&index,&order));
    assert(s.count==5&&index.count==5&&order==27&&packets[0].order==11);
    for(unsigned n=0;n<4;n++){
        assert(packets[n+1].order==21+n&&packets[n+1].xyz[2]==(float)(13+n));
        assert(index.bins[n+3]==1);
    }
    hone_projected_packet copy[8];memcpy(copy,packets,sizeof copy);
    assert(!hone_retained_append(old,&s,bounds,&index,&order)&&!memcmp(copy,packets,sizeof copy));
    old=(hone_retained_span){6,4,0,1};
    assert(!hone_retained_append(old,&s,bounds,&index,&order));
    old=(hone_retained_span){5,0,0,1};order=UINT16_MAX;
    assert(!hone_retained_append(old,&s,bounds,&index,&order));
    order=20;index.count=4;assert(!hone_retained_append(old,&s,bounds,&index,&order));
    return 0;
}
