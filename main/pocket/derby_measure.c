#include "derby_measure.h"
#include <string.h>
#define DM_DEPTH 24u
static dm_snapshot totals;
static struct { enum dm_stage stage; uint64_t start,child; } stack[DM_DEPTH];
static unsigned depth;
static uint64_t last_completed;
dm_scope dm_enter(enum dm_stage stage){
    if((unsigned)stage>=DM_STAGES||depth==DM_DEPTH){totals.faults++;return (dm_scope){0};}
    stack[depth].stage=stage;stack[depth].start=dm_now_us();stack[depth].child=0;
    return (dm_scope){.depth=++depth,.active=1};
}
void dm_leave(dm_scope *scope){
    if(!scope->active)return;
    scope->active=0;
    if(!depth||scope->depth!=depth){totals.faults++;return;}
    uint64_t now=dm_now_us(),start=stack[depth-1].start;
    uint64_t elapsed=now>=start?now-start:0,child=stack[depth-1].child;
    if(now<start||child>elapsed)totals.faults++;
    dm_counter *c=&totals.stage[stack[--depth].stage];
    uint64_t self=elapsed>=child?elapsed-child:0;
    c->calls++;c->inclusive_us+=elapsed;c->self_us+=self;
    if(elapsed>c->max_inclusive_us)c->max_inclusive_us=elapsed;
    if(self>c->max_self_us)c->max_self_us=self;
    totals.scope_pairs++;
    if(depth)stack[depth-1].child+=elapsed;
}
void dm_reset(void){memset(&totals,0,sizeof totals);depth=0;last_completed=0;}
void dm_refused(void){totals.output_refused++;}
int dm_read(dm_snapshot *out){if(depth||!out)return 0;*out=totals;return 1;}
void dm_heap(size_t used,size_t cap){
    if(used>totals.sampled_heap_max)totals.sampled_heap_max=used;
    totals.sampled_heap_cap=cap;
}
void dm_batch(int pie){if(pie)totals.pie_batches++;else totals.scalar_batches++;}
void dm_calibrate(void){
    totals.clock_pair_min_us=UINT64_MAX;
    for(unsigned i=0;i<32;i++){
        uint64_t a=dm_now_us(),b=dm_now_us(),dt=b>=a?b-a:0;
        if(dt<totals.clock_pair_min_us)totals.clock_pair_min_us=dt;
        if(dt>totals.clock_pair_max_us)totals.clock_pair_max_us=dt;
    }
}

void dm_dispatch(int continuing,int failed){
    if(continuing)totals.continuations++;else totals.contexts++;
    if(failed)totals.dispatch_failures++;
}
void dm_present(int work_done,int capture){
    if(capture)totals.capture_samples++;
    if(work_done&&totals.contexts&&last_completed!=totals.contexts){
        totals.completed_contexts++;last_completed=totals.contexts;
    }
}
void dm_transfer(size_t bytes,int failed,int capture){
    if(capture)totals.capture_samples++;
    if(failed)totals.transfer_failures++;
    else {totals.transfer_success++;totals.transfer_bytes+=bytes;}
}
