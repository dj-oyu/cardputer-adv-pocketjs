"""Exercise the exact diagnostic wrapper with host ESP/JS boundary stubs.

Synthetic clock costs test scheduling decisions, not target timing. The real
numeric engine and an independent synchronous run verify the complete corpus.
This does not execute app_session, QuickJS, FreeRTOS, or a device watchdog.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
source = (ROOT / 'main/pocket/pocket_shiki.c').read_text()
block = source[
    source.index('/* Bounded adaptive native batch per guest frame'):
    source.index('\n#endif\n\nesp_err_t pocket_shiki_debug_fill')
]
prefix = r'''
#include "ui/kasane/ksn_mesh_q7_preflight.h"
#include "ui/kasane/ksn_mesh_q7_workspace.h"
#include <assert.h>
#include <fenv.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef int JSValue;
typedef int JSValueConst;
typedef int JSContext;
enum { MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2, OBJECT=1, EXCEPTION=-1,
       STEPS=3376, RENDERS=STEPS*2, BATCH_LIMIT=32 };
static bool done,ok,heap_oom,object_oom;
static unsigned active,allocations,frees,allocation_attempts;
static size_t available=100000,largest=100000;
static unsigned stack_watermark=16000;
static unsigned char *base,*raw_pointer;
static int64_t now,tick=1,step_cost,begin_cost,object_cost;
static unsigned slow_at;
static int64_t slow_cost,pending_sample_gap,after_sample_gap;
static unsigned sample_gap_at;
static unsigned render_calls,step_calls,begin_calls,guest_calls;
static bool record_oracle;
static uint64_t oracle_trace[RENDERS];
static ksn_mesh_q7_preflight_report oracle;
static ksn_mesh_q7_stats emitted_stats;
static struct { const char *key; double value; } numbers[64];
static unsigned number_count;

static JSValue JS_NewObject(JSContext *c){
    (void)c;now+=object_cost;
    if(object_oom)return EXCEPTION;
    done=ok=false;number_count=0;
    memset(&emitted_stats,0,sizeof emitted_stats);
    return OBJECT;
}
static bool JS_IsException(JSValue v){return v==EXCEPTION;}
static JSValue JS_NewBool(JSContext *c,bool v){(void)c;return v;}
static void JS_SetPropertyStr(JSContext *c,JSValue o,const char *k,JSValue v){
    (void)c;assert(o==OBJECT);
    if(!strcmp(k,"done"))done=v;
    else if(!strcmp(k,"ok"))ok=v;
    else assert(false);
}
static void q7_num(JSContext *c,JSValue o,const char *k,double n){
    (void)c;assert(o==OBJECT && number_count<64);
    numbers[number_count].key=k;numbers[number_count++].value=n;
}
static void q7_heap(JSContext *c,JSValue o){(void)c;assert(o==OBJECT);}
static void q7_stats(JSContext *c,JSValue o,const ksn_mesh_q7_stats *s){
    (void)c;assert(o==OBJECT);emitted_stats=*s;
}
static double number(const char *key){
    for(unsigned i=0;i<number_count;i++)
        if(!strcmp(numbers[i].key,key))return numbers[i].value;
    fprintf(stderr,"missing telemetry: %s\n",key);abort();
}
static size_t heap_caps_get_free_size(unsigned c){
    assert(c==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));return available;
}
static size_t heap_caps_get_largest_free_block(unsigned c){
    assert(c==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));return largest;
}
static unsigned uxTaskGetStackHighWaterMark(void *p){
    assert(!p);return stack_watermark;
}
static void *heap_caps_malloc(size_t n,unsigned c){
    assert(c==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));allocation_attempts++;
    if(heap_oom)return NULL;
    assert(!active);
    base=malloc(n+16);assert(base);
    /* Deliberately only 4-aligned, never accidentally 8-aligned. */
    raw_pointer=base+((4-(uintptr_t)base%8+8)%8);
    assert((uintptr_t)raw_pointer%8==4);
    active++;allocations++;
    return raw_pointer;
}
static void heap_caps_free(void *p){
    if(!p)return;
    assert(active==1 && p==raw_pointer);
    free(base);active--;frees++;base=raw_pointer=NULL;
}
static int64_t esp_timer_get_time(void){
    int64_t sampled=now+=tick;
    /* Model a scheduler pause immediately after the clock was sampled. The
     * next loop must read fresh time before starting another numeric step. */
    now+=pending_sample_gap;pending_sample_gap=0;
    return sampled;
}
'''
suffix = r'''
extern bool __real_ksn_mesh_q7_preflight_begin(void *,size_t,ksn_mesh_q7_preflight_report *);
extern ksn_mesh_q7_preflight_step_result __real_ksn_mesh_q7_preflight_step(
    void *,size_t,ksn_mesh_q7_preflight_report *);
extern bool __real_ksn_mesh_q7_render(const ksn_mesh_q7_snapshot *,uint16_t *,
    unsigned,unsigned,float *,uint8_t *,unsigned,ksn_mesh_q7_stats *);

static uint64_t hash_bytes(uint64_t hash,const void *data,size_t size){
    const unsigned char *bytes=data;
    for(size_t i=0;i<size;i++){hash^=bytes[i];hash*=UINT64_C(1099511628211);}
    return hash;
}
bool __wrap_ksn_mesh_q7_render(const ksn_mesh_q7_snapshot *snapshot,uint16_t *rgb,
    unsigned y,unsigned height,float *depth,uint8_t *owner,unsigned rows,
    ksn_mesh_q7_stats *stats){
    assert(render_calls<RENDERS && height==1 && rows==1);
    unsigned checkpoint=render_calls/2;
    assert(y==(checkpoint==STEPS-1?KSN_PROC_H:checkpoint%KSN_PROC_H));
    /* Guest rounding and exception changes must not enter the numeric stream. */
    assert(fegetround()==FE_TONEAREST);
    assert(fetestexcept(FE_DIVBYZERO)==0);
    bool result=__real_ksn_mesh_q7_render(snapshot,rgb,y,height,depth,owner,rows,stats);
    uint64_t hash=UINT64_C(14695981039346656037);
    hash=hash_bytes(hash,&y,sizeof y);
    hash=hash_bytes(hash,&result,sizeof result);
    hash=hash_bytes(hash,rgb,KSN_PROC_W*sizeof *rgb);
    hash=hash_bytes(hash,depth,KSN_PROC_W*sizeof *depth);
    hash=hash_bytes(hash,owner,KSN_PROC_W*sizeof *owner);
    hash=hash_bytes(hash,stats,sizeof *stats);
    if(record_oracle)oracle_trace[render_calls]=hash;
    else assert(hash==oracle_trace[render_calls]);
    render_calls++;
    return result;
}
bool __wrap_ksn_mesh_q7_preflight_begin(void *memory,size_t size,
    ksn_mesh_q7_preflight_report *report){
    unsigned before=render_calls;
    int mode=fegetround(),flags=fetestexcept(FE_ALL_EXCEPT);
    begin_calls++;
    bool result=__real_ksn_mesh_q7_preflight_begin(memory,size,report);
    assert(render_calls==before);
    assert(fegetround()==mode && fetestexcept(FE_ALL_EXCEPT)==flags);
    now+=begin_cost;
    return result;
}
ksn_mesh_q7_preflight_step_result __wrap_ksn_mesh_q7_preflight_step(
    void *memory,size_t size,ksn_mesh_q7_preflight_report *report){
    unsigned before=render_calls;
    uint64_t checkpoint=report->checkpoints;
    int mode=fegetround(),flags=fetestexcept(FE_ALL_EXCEPT);
    assert(checkpoint==step_calls && checkpoint<STEPS);
    ksn_mesh_q7_preflight_step_result result=
        __real_ksn_mesh_q7_preflight_step(memory,size,report);
    step_calls++;
    assert(report->checkpoints==checkpoint+1 && render_calls==before+2);
    assert(result==(step_calls==STEPS?KSN_MESH_Q7_PREFLIGHT_DONE:KSN_MESH_Q7_PREFLIGHT_MORE));
    assert(fegetround()==mode && fetestexcept(FE_ALL_EXCEPT)==flags);
    now+=slow_at==step_calls?slow_cost:step_cost;
    if(sample_gap_at==step_calls)pending_sample_gap=after_sample_gap;
    return result;
}
static void environment(unsigned batch){
    const int modes[]={FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO};
    const int flags[]={FE_INVALID,FE_DIVBYZERO,0,FE_INVALID|FE_DIVBYZERO};
    assert(fesetround(modes[batch%4])==0);
    assert(feclearexcept(FE_ALL_EXCEPT)==0);
    if(flags[batch%4])assert(feraiseexcept(flags[batch%4])==0);
}
static void make_oracle(void){
    size_t bytes=ksn_mesh_q7_preflight_bytes(),allocation;
    size_t alignment=ksn_mesh_q7_preflight_alignment();
    assert(ksn_mesh_q7_workspace_allocation(bytes,alignment,&allocation));
    void *raw=malloc(allocation);assert(raw);
    void *memory=ksn_mesh_q7_workspace_align(raw,allocation,bytes,alignment);assert(memory);
    environment(0);record_oracle=true;
    assert(ksn_mesh_q7_preflight_run(memory,bytes,&oracle));
    assert(oracle.checkpoints==STEPS && oracle.cases==26 && oracle.pixels==810240);
    assert(oracle.render_failures==1 && render_calls==RENDERS);
    assert(!oracle.mismatch_count && !oracle.invariant_failures && !oracle.failure_result_mismatches);
    assert(oracle.enabled.depth_fast && oracle.enabled.mask_fast);
    record_oracle=false;free(raw);
}
static void fresh(void){
    pocket_shiki_debug_reset();assert(!active && allocations==frees);
    assert(!q7_preflight_state.started_once && !q7_preflight_state.raw);
    available=largest=100000;stack_watermark=16000;
    heap_oom=object_oom=false;
    now=0;tick=1;step_cost=begin_cost=object_cost=slow_cost=0;slow_at=0;
    pending_sample_gap=after_sample_gap=0;sample_gap_at=0;
    render_calls=step_calls=begin_calls=guest_calls=allocation_attempts=0;
    environment(0);
}
static unsigned call(void){
    environment(guest_calls++);
    int mode=fegetround(),flags=fetestexcept(FE_ALL_EXCEPT);
    unsigned before=step_calls,batches=q7_preflight_state.batches;
    bool finished=q7_preflight_state.finished;
    assert(js_mesh_q7_preflight(NULL,0,0,NULL)==OBJECT);
    assert(fegetround()==mode && fetestexcept(FE_ALL_EXCEPT)==flags);
    unsigned delta=step_calls-before;
    assert(delta<=BATCH_LIMIT && render_calls==2*step_calls);
    assert(number("checkpoints")==step_calls);
    assert(q7_preflight_state.report.checkpoints==step_calls);
    assert(q7_preflight_state.report.pixels==(uint64_t)step_calls*KSN_PROC_W);
    assert(q7_preflight_state.max_batch_steps<=BATCH_LIMIT);
    if(finished)assert(!delta && q7_preflight_state.batches==batches);
    if(!done){
        assert(ok && delta && active==1);
        assert((uintptr_t)q7_preflight_state.workspace%ksn_mesh_q7_preflight_alignment()==0);
        assert(q7_preflight_state.raw==raw_pointer && q7_preflight_state.max_batch<=12000);
        assert(q7_preflight_state.batches==batches+1);
    }else assert(!active && !q7_preflight_state.raw && !q7_preflight_state.workspace);
    return delta;
}
static void assert_complete(void){
    assert(done && ok && !active && begin_calls==1);
    assert(step_calls==STEPS && render_calls==RENDERS);
    assert(memcmp(&oracle,&q7_preflight_state.report,sizeof oracle)==0);
    assert(memcmp(&oracle.enabled,&emitted_stats,sizeof emitted_stats)==0);
    assert(number("batches")==q7_preflight_state.batches);
    assert(number("maxBatchSteps")==q7_preflight_state.max_batch_steps);
    assert(number("maxBatchUs")==q7_preflight_state.max_batch);
    assert(number("maxStepUs")==q7_preflight_state.max_step);
    assert(number("stepEstimateUs")==q7_preflight_state.step_estimate);
    assert(number("batchStepLimit")==32 && number("softBatchUs")==4000);
    assert(number("hardBatchUs")==12000 && number("totalLimitUs")==45000000);
    assert(number("registersRestored")==1 && number("environment_restored")==1);
    assert(number("cases")==26 && number("pixels")==810240);
    assert(number("mismatch_count")==0 && number("invariant_failures")==0);
    assert(number("failure_result_mismatches")==0 && number("render_failures")==1);
    assert(number("preflightBytes")==ksn_mesh_q7_preflight_bytes());
    assert(number("allocationBytes")==q7_preflight_state.allocation);
    assert(number("alignment")==ksn_mesh_q7_preflight_alignment());
    assert(number("preflightUs")<=45000000);
    unsigned allocations_before=allocations;
    assert(call()==0 && done && ok && allocations==allocations_before);
}
static void fast_test(void){
    fresh();
    for(unsigned i=0;i<(STEPS+BATCH_LIMIT-1)/BATCH_LIMIT;i++){
        unsigned remaining=STEPS-step_calls;
        assert(call()==(remaining<BATCH_LIMIT?remaining:BATCH_LIMIT));
    }
    assert(q7_preflight_state.batches==106 && q7_preflight_state.max_batch_steps==32);
    assert(q7_preflight_state.max_step==1 && q7_preflight_state.max_batch<4000);
    assert_complete();
    puts("Q7_DEBUG_FAST_PASS 3376 steps, 106 batches, full synchronous numeric/output oracle");
}
static void adaptive_test(void){
    fresh();step_cost=650;
    unsigned calls=0;
    do{
        unsigned rows=call();
        assert(rows>=1 && rows<32 && ++calls<=STEPS);
        assert(q7_preflight_state.max_batch<=4000);
    }while(!done);
    assert(calls>106 && q7_preflight_state.max_batch_steps==4);
    assert_complete();
    /* An isolated expensive step initially shortens batches. The conservative
     * estimate must recover as cheaper rows are observed, while maxStepUs
     * continues to report the actual outlier and the complete corpus runs. */
    fresh();slow_at=1;slow_cost=5000;
    assert(call()==1 && !done);
    assert(q7_preflight_state.step_estimate==q7_preflight_state.max_step);
    calls=1;bool recovered=false;
    do{
        unsigned rows=call();
        assert(rows>=1 && rows<=32 && ++calls<=STEPS);
        if(rows==32)recovered=true;
        if(calls==8)assert(recovered);
        assert(q7_preflight_state.max_step==5001);
    }while(!done);
    assert(recovered && calls>106 && calls<128);
    assert(q7_preflight_state.max_batch_steps==32);
    assert(q7_preflight_state.step_estimate<q7_preflight_state.max_step);
    assert(q7_preflight_state.max_batch>4000 && q7_preflight_state.max_batch<12000);
    assert_complete();
    /* Even a sustained row cost above the soft budget must make progress. */
    fresh();step_cost=5000;
    for(unsigned i=0;i<8;i++)assert(call()==1 && !done);
    assert(q7_preflight_state.max_batch_steps==1);
    puts("Q7_DEBUG_ADAPTIVE_PASS soft budget, spike recovery, forward progress, full oracle");
}
static void assert_stopped(unsigned expected_steps){
    assert(done && !ok && !active && step_calls==expected_steps);
    unsigned allocations_before=allocations;
    assert(call()==0 && done && !ok && allocations==allocations_before);
}
static void timing_tests(void){
    fresh();assert(call()==32);
    now=q7_preflight_state.started+45000001;
    assert(call()==0);assert_stopped(32);
    /* Expiry can happen during a batch, or during setup before its first row. */
    fresh();assert(call()==32);
    now=q7_preflight_state.started+45000000-10;step_cost=50;
    assert(call()==1);assert_stopped(33);
    fresh();begin_cost=45000001;
    assert(call()==0 && begin_calls==1);assert_stopped(0);
    fresh();object_cost=12001;
    assert(call()==0);assert_stopped(0);
    /* The hard batch limit is stricter than the per-step limit. Both reject
     * overrun, without attempting another row or claiming timing success. */
    fresh();step_cost=12001;
    assert(call()==1 && q7_preflight_state.max_step<20000);
    assert(q7_preflight_state.max_batch>12000);assert_stopped(1);
    fresh();step_cost=20001;
    assert(call()==1 && q7_preflight_state.max_step>20000);assert_stopped(1);
    fresh();slow_at=2;slow_cost=12001;
    assert(call()==2 && q7_preflight_state.max_batch_steps==2);assert_stopped(2);
    /* Detect time spent descheduled between rows before starting more work,
     * even though the previous post-step sample was still within budget. */
    fresh();sample_gap_at=1;after_sample_gap=12001;
    assert(call()==1 && q7_preflight_state.max_step<12000);assert_stopped(1);
    fresh();sample_gap_at=1;after_sample_gap=45000001;
    assert(call()==1);assert_stopped(1);
    fresh();step_cost=-2;
    assert(call()==1);assert_stopped(1);
    puts("Q7_DEBUG_TIMING_PASS expired entry has zero work; setup/mid-batch expiry, hard/step stops");
}
static void oom_tests(void){
    for(unsigned kind=0;kind<4;kind++){
        fresh();
        if(kind==0)available=0;
        if(kind==1)largest=0;
        if(kind==2)stack_watermark=8191;
        if(kind==3)heap_oom=true;
        assert(call()==0 && !begin_calls);
        assert(allocation_attempts==(kind==3));assert_stopped(0);
    }
    fresh();object_oom=true;
    unsigned char saved[sizeof q7_preflight_state];
    memcpy(saved,&q7_preflight_state,sizeof saved);
    assert(js_mesh_q7_preflight(NULL,0,0,NULL)==EXCEPTION);
    assert(!memcmp(saved,&q7_preflight_state,sizeof saved));
    assert(!active && !step_calls && !begin_calls && !allocation_attempts);
    object_oom=false;assert(call()==32);
    memcpy(saved,&q7_preflight_state,sizeof saved);
    object_oom=true;
    for(unsigned i=0;i<3;i++){
        assert(js_mesh_q7_preflight(NULL,0,0,NULL)==EXCEPTION);
        assert(!memcmp(saved,&q7_preflight_state,sizeof saved));
        assert(active==1 && step_calls==32 && render_calls==64);
    }
    object_oom=false;assert(call()==32 && active==1 && begin_calls==1);
    object_oom=true;
    assert(js_mesh_q7_preflight(NULL,0,0,NULL)==EXCEPTION);
    pocket_shiki_debug_reset();assert(!active && allocations==frees);
    puts("Q7_DEBUG_OOM_PASS heap/stack admission, allocation OOM, JS object OOM retains/retries/resets");
}
static void reset_tests(void){
    /* Reset only between serialized native calls, including a soft-shortened
     * batch in the middle of a scene. Reentrant reset is not a supported API. */
    const unsigned stops[]={1,4,33,34,843};
    for(unsigned i=0;i<sizeof stops/sizeof stops[0];i++){
        fresh();step_cost=650;
        for(unsigned j=0;j<stops[i];j++)assert(call()==4 && !done);
        assert(step_calls==4*stops[i] && step_calls<STEPS);
        pocket_shiki_debug_reset();assert(!active && allocations==frees);
        pocket_shiki_debug_reset();assert(!active && allocations==frees);
    }
    /* A teardown after stopped execution must also allow a complete new run. */
    fresh();step_cost=12001;assert(call()==1);assert_stopped(1);
    fresh();
    do{call();}while(!done);
    assert_complete();
    pocket_shiki_debug_reset();assert(!active && allocations==frees);
    puts("Q7_DEBUG_RESET_PASS partial batch, scene boundary, near-final stop, idempotent reset/restart");
}
int main(void){
    fenv_t saved;assert(fegetenv(&saved)==0);
    make_oracle();fast_test();adaptive_test();timing_tests();oom_tests();reset_tests();
    assert(fesetenv(&saved)==0);
    puts("Q7_DEBUG_LIFECYCLE_PASS deliberately4aligned allocations; caller fenv every batch; host stubs only");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='q7-debug-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(prefix + block + suffix)
    binary = Path(directory) / 'test'
    sources = [
        'main/ayatsuri/compact_opaque.c',
        'main/ui/kasane/ksn_mesh_depth.c',
        'main/ui/kasane/ksn_shiki_frame.c',
        'main/ui/kasane/ksn_mesh_prepared.c',
        'main/ui/kasane/ksn_mesh_q7_certified.c',
        'main/ui/kasane/ksn_mesh_q7_preflight.c',
    ]
    subprocess.run([
        os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-g', '-Wall', '-Wextra',
        '-Werror', '-fno-fast-math', '-ffp-contract=off', '-frounding-math',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        '-I' + str(ROOT / 'main'), str(path),
        *[str(ROOT / item) for item in sources],
        '-Wl,--wrap=ksn_mesh_q7_preflight_begin',
        '-Wl,--wrap=ksn_mesh_q7_preflight_step',
        '-Wl,--wrap=ksn_mesh_q7_render', '-lm', '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, env=dict(
        os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
        UBSAN_OPTIONS='halt_on_error=1'))
