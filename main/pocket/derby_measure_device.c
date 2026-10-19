#include "derby_measure.h"
#include "esp_timer.h"
#include "usb_console.h"
#include <inttypes.h>
#include <stdio.h>
uint64_t dm_now_us(void){return (uint64_t)esp_timer_get_time();}
static uint64_t last;static unsigned attempts;
void dm_output_reset(void){last=0;attempts=0;}
/* Keep each attempt within usb_console_frame's 1024-byte contract. A wide
 * snapshot becomes an explicit invalid record, never silent truncation. */
size_t dm_format_record(char *line,size_t capacity,unsigned seq,uint64_t now,
                        const dm_snapshot *s,int *overflow){
    *overflow=0;
    int n=snprintf(line,capacity,
        "DERBY_MEASURE v=1 seq=%u us=%" PRIu64 " heap_sample_max=%" PRIu64
        " cap=%" PRIu64 " scalar=%" PRIu64 " pie=%" PRIu64
        " ctx=%" PRIu64 " cont=%" PRIu64 " done=%" PRIu64 " dispatch_fail=%" PRIu64
        " bytes=%" PRIu64 " tx_ok=%" PRIu64 " tx_fail=%" PRIu64 " capture=%" PRIu64
        " refused=%" PRIu64 " faults=%" PRIu64 " clock_min=%" PRIu64 " clock_max=%" PRIu64 " pairs=%" PRIu64,
        seq,now,s->sampled_heap_max,s->sampled_heap_cap,s->scalar_batches,s->pie_batches,
        s->contexts,s->continuations,s->completed_contexts,s->dispatch_failures,
        s->transfer_bytes,s->transfer_success,s->transfer_failures,s->capture_samples,
        s->output_refused,s->faults,s->clock_pair_min_us,s->clock_pair_max_us,s->scope_pairs);
    for(unsigned i=0;i<DM_STAGES&&n>0&&(size_t)n<capacity;i++)
        n+=snprintf(line+n,capacity-(size_t)n," s%u=%" PRIu64 ":%" PRIu64 ":%" PRIu64 ":%" PRIu64 ":%" PRIu64,
                    i,s->stage[i].inclusive_us,s->stage[i].self_us,s->stage[i].calls,s->stage[i].max_inclusive_us,s->stage[i].max_self_us);
    if(n<=0||(size_t)n+1>=capacity){
        *overflow=1;
        n=snprintf(line,capacity,"DERBY_MEASURE v=1 seq=%u us=%" PRIu64 " overflow=1\n",seq,now);
        return n>0&&(size_t)n<capacity?(size_t)n:0;
    }
    line[n++]='\n';
    return (size_t)n;
}
void dm_emit(size_t used,size_t cap){
    uint64_t now=dm_now_us();
    dm_heap(used,cap);
    if(now<last||now-last<1000000||attempts>=300)return;
    dm_snapshot s;if(!dm_read(&s))return;
    DM_SCOPE(DM_OBSERVER);
    last=now;attempts++;
    char line[1024];int overflow=0;
    size_t n=dm_format_record(line,sizeof line,attempts,now,&s,&overflow);
    if(overflow)dm_refused();
    if(!n)return;
    DM_SCOPE(DM_OUTPUT);
    if(usb_console_frame(line,(size_t)n)!=0)dm_refused();
}
