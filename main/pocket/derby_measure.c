#include "derby_detail.h"
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

#ifdef POCKET_DERBY_DETAIL_MEASURE
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static TaskHandle_t dd_owner;
static int dd_owned(void){return !xPortInIsrContext()&&xTaskGetCurrentTaskHandle()==dd_owner;}
#else
static int dd_owned(void){return 1;}
#endif
#define DD_DEPTH 24u
static dd_snapshot dd_totals;
static struct { enum dd_stage stage; uint64_t start,child; int js; } dd_stack[DD_DEPTH];
static __typeof__(dd_stack) dd_saved;
static unsigned dd_depth,dd_saved_depth;
static uint64_t dd_suspended_at;
static int dd_active;
void dd_cancel(void){if(dd_active&&dd_owned()){dd_active=0;dd_depth=dd_saved_depth=0;}}
void dd_start(void){
#ifdef ESP_PLATFORM
    if(xPortInIsrContext())return;
    dd_owner=xTaskGetCurrentTaskHandle();
#endif
    memset(&dd_totals,0,sizeof dd_totals);dd_depth=dd_saved_depth=0;dd_active=1;
}
dd_scope dd_enter(enum dd_stage stage){
    if(!dd_active||!dd_owned())return (dd_scope){0};
    if((unsigned)stage>=DD_STAGES||dd_depth==DD_DEPTH){dd_totals.faults++;return (dd_scope){0};}
    dd_stack[dd_depth].stage=stage;dd_stack[dd_depth].start=dm_now_us();
    dd_stack[dd_depth].child=0;dd_stack[dd_depth].js=0;
    return (dd_scope){.depth=++dd_depth,.active=1};
}
void dd_leave(dd_scope *scope){
    if(!scope->active||!dd_active||!dd_owned())return;
    scope->active=0;
    if(!dd_depth||scope->depth!=dd_depth){dd_totals.faults++;return;}
    uint64_t now=dm_now_us(),start=dd_stack[dd_depth-1].start,child=dd_stack[dd_depth-1].child;
    uint64_t dt=now>=start?now-start:0;
    if(now<start||child>dt)dd_totals.faults++;
    dd_counter *c=&dd_totals.stage[dd_stack[--dd_depth].stage];
    c->inclusive_us+=dt;c->self_us+=dt>=child?dt-child:0;c->calls++;dd_totals.pairs++;
    if(dd_depth)dd_stack[dd_depth-1].child+=dt;
}
void dd_js(int stage){
    if(!dd_active||!dd_owned())return;
    if(stage>=0){dd_scope s=dd_enter((enum dd_stage)stage);if(s.active)dd_stack[dd_depth-1].js=1;}
    else {
        if(!dd_depth||!dd_stack[dd_depth-1].js){dd_totals.faults++;return;}
        dd_scope s={dd_depth,1};dd_leave(&s);
    }
}
void dd_add(enum dd_event event,uint64_t amount){
    if(!dd_active||!dd_owned())return;
    if((unsigned)event>=DD_EVENTS||UINT64_MAX-dd_totals.event[event]<amount){dd_totals.faults++;return;}
    dd_totals.event[event]+=amount;
}
/* JS scopes survive QuickJS park. Remove them from the live stack while the
 * owner renders/paces, then shift starts by the excluded interval on resume.
 * This preserves one invocation and excludes explicit host pacing from JS. */
void dd_suspend(void){
    if(!dd_active||!dd_owned()||!dd_depth)return;
    if(dd_saved_depth){dd_totals.faults++;return;}
    for(unsigned i=0;i<dd_depth;i++)if(!dd_stack[i].js){dd_totals.faults++;return;}
    dd_suspended_at=dm_now_us();dd_saved_depth=dd_depth;
    memcpy(dd_saved,dd_stack,dd_depth*sizeof dd_stack[0]);dd_depth=0;
}
void dd_resume(void){
    if(!dd_active||!dd_owned()||!dd_saved_depth)return;
    if(dd_depth){dd_totals.faults++;return;}
    uint64_t now=dm_now_us();
    if(now<dd_suspended_at){dd_totals.faults++;return;}
    uint64_t paused=now-dd_suspended_at;
    dd_depth=dd_saved_depth;dd_saved_depth=0;
    memcpy(dd_stack,dd_saved,dd_depth*sizeof dd_stack[0]);
    for(unsigned i=0;i<dd_depth;i++)dd_stack[i].start+=paused;
}
int dd_finish(dd_snapshot *out){
    if(!dd_active||!dd_owned()||!out||dd_depth||dd_saved_depth)return 0;
    *out=dd_totals;dd_active=0;return 1;
}
#endif
