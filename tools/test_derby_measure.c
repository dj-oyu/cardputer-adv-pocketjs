#include "derby_measure.h"
#include <assert.h>
#include <stdio.h>
static uint64_t now;
uint64_t dm_now_us(void){return now;}
static void early(void){DM_SCOPE(DM_VM);now+=5;return;}
static void nested(void){
    DM_SCOPE(DM_GUEST);now+=10;
    {DM_SCOPE(DM_BRIDGE);now+=3;
        {DM_SCOPE(DM_NATIVE);now+=7;early();now+=2;}
        now+=4;
    }
    now+=9;
}
static void recursive(unsigned n){DM_SCOPE(DM_VM);if(n)recursive(n-1);now++;}
int main(void){
    dm_reset();dm_calibrate();nested();dm_snapshot s;
    assert(dm_read(&s));
    assert(s.stage[DM_GUEST].inclusive_us==40&&s.stage[DM_GUEST].self_us==19);
    assert(s.stage[DM_BRIDGE].inclusive_us==21&&s.stage[DM_BRIDGE].self_us==7);
    assert(s.stage[DM_NATIVE].inclusive_us==14&&s.stage[DM_NATIVE].self_us==9);
    assert(s.stage[DM_VM].inclusive_us==5&&s.stage[DM_VM].self_us==5);
    uint64_t self=0;for(unsigned i=0;i<DM_STAGES;i++)self+=s.stage[i].self_us;
    assert(self==40&&s.scope_pairs==4&&!s.faults);
    assert(s.stage[DM_GUEST].max_inclusive_us==40&&s.stage[DM_GUEST].max_self_us==19);
    dm_dispatch(0,0);dm_dispatch(1,0);dm_present(0,0);dm_present(1,1);dm_present(1,0);
    dm_transfer(480,0,0);dm_transfer(480,1,0);
    assert(dm_read(&s)&&s.contexts==1&&s.continuations==1&&s.completed_contexts==1&&
           s.capture_samples==1&&s.transfer_bytes==480&&s.transfer_success==1&&s.transfer_failures==1);
    dm_heap(70,100);dm_heap(60,100);dm_batch(0);dm_batch(1);dm_refused();
    assert(dm_read(&s)&&s.sampled_heap_max==70&&s.scalar_batches==1&&s.pie_batches==1&&s.output_refused==1);
    {DM_SCOPE(DM_BRIDGE);assert(!dm_read(&s));}
    dm_reset();recursive(30);assert(dm_read(&s)&&s.faults==7);
    dm_reset();{DM_SCOPE(DM_NATIVE);now=0;}assert(dm_read(&s)&&s.faults==1);
    puts("PASS exclusive scopes, early return, recursion overflow, clock regression, heap and backend counters");
}
