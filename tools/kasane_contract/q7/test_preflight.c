#include "ui/kasane/ksn_mesh_q7_preflight.h"
#include <assert.h>
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
/* Host-only fault injection: both arms make the same forbidden write on a
 * rejected call. Equality alone must not let this regression pass. */
static unsigned corrupt_rejection;
static unsigned render_calls;
static bool verify_numeric_env;
static int numeric_mode,numeric_divzero;
extern bool __real_ksn_mesh_q7_render(const ksn_mesh_q7_snapshot *,uint16_t *,
    unsigned,unsigned,float *,uint8_t *,unsigned,ksn_mesh_q7_stats *);
bool __wrap_ksn_mesh_q7_render(const ksn_mesh_q7_snapshot *s,uint16_t *rgb,
    unsigned y,unsigned h,float *depth,uint8_t *owner,unsigned rows,ksn_mesh_q7_stats *stats){
    render_calls++;
    if(verify_numeric_env){
        assert(fegetround()==numeric_mode);
        assert(fetestexcept(FE_DIVBYZERO)==numeric_divzero);
    }
    bool result=__real_ksn_mesh_q7_render(s,rgb,y,h,depth,owner,rows,stats);
    if(y==KSN_PROC_H && !result){
        if(corrupt_rejection==1)rgb[0]=0;
        if(corrupt_rejection==2)depth[0]=0;
        if(corrupt_rejection==3)owner[0]=0;
    }
    return result;
}
static void run(void *memory){
    ksn_mesh_q7_preflight_report r;
    int mode=fegetround(),flags=fetestexcept(FE_ALL_EXCEPT);
    bool ok=ksn_mesh_q7_preflight_run(memory,ksn_mesh_q7_preflight_bytes(),&r);
    if(!ok)fprintf(stderr,"FAIL cases=%llu inv=%llu mismatch=%llu result=%llu\n",(unsigned long long)r.cases,(unsigned long long)r.invariant_failures,(unsigned long long)r.mismatch_count,(unsigned long long)r.failure_result_mismatches);
    assert(ok);
    assert(r.environment_restored && r.round_before==mode && r.round_after==mode);
    assert(fetestexcept(FE_ALL_EXCEPT)==flags);
    assert(r.cases==26 && r.pixels==810240 && r.render_failures>=1);
    if(mode==FE_TONEAREST && r.subnormal_ok){
        assert(r.enabled.depth_fast && r.enabled.depth_exact);
        assert(r.enabled.mask_fast && r.enabled.mask_exact);
        assert(r.safe_fallback_cases && r.certificate_cases);
    }
    printf("PREFLIGHT_PASS bytes=%zu alignment=%zu cases=%llu pixels=%llu mismatch=%llu failures=%llu safe_fallback=%llu cert_cases=%llu round_before=%d round_after=%d subnormal=%d restored=%d depth_fast=%llu depth_exact=%llu mask_fast=%llu mask_exact=%llu\n",
        ksn_mesh_q7_preflight_bytes(),ksn_mesh_q7_preflight_alignment(),
        (unsigned long long)r.cases,(unsigned long long)r.pixels,(unsigned long long)r.mismatch_count,
        (unsigned long long)r.render_failures,(unsigned long long)r.safe_fallback_cases,
        (unsigned long long)r.certificate_cases,r.round_before,r.round_after,r.subnormal_ok,r.environment_restored,
        (unsigned long long)r.enabled.depth_fast,(unsigned long long)r.enabled.depth_exact,
        (unsigned long long)r.enabled.mask_fast,(unsigned long long)r.enabled.mask_exact);
}
typedef struct { unsigned calls, cancel_at; int mode, flags; } callback_state;
static bool mutate_environment(void *context){
    callback_state *state=context;
    assert(fegetround()==state->mode);
    assert(fetestexcept(FE_ALL_EXCEPT)==state->flags);
    state->calls++;
    assert(fesetround(state->mode==FE_DOWNWARD?FE_UPWARD:FE_DOWNWARD)==0);
    assert(feclearexcept(FE_ALL_EXCEPT)==0);
    assert(feraiseexcept(FE_DIVBYZERO)==0);
    return state->calls!=state->cancel_at;
}
static void cooperative_tests(void *memory,size_t n){
    ksn_mesh_q7_preflight_report sync,cooperative;
    int mode=fegetround(),flags=fetestexcept(FE_ALL_EXCEPT);
    assert(ksn_mesh_q7_preflight_run(memory,n,&sync));
    assert(sync.checkpoints==3376 && sync.yield_count==0 && !sync.cancelled);
    callback_state state={.mode=mode,.flags=flags};
    verify_numeric_env=true;numeric_mode=mode;numeric_divzero=flags&FE_DIVBYZERO;
    assert(ksn_mesh_q7_preflight_run_cooperative(memory,n,&cooperative,mutate_environment,&state));
    assert(state.calls==3376 && cooperative.checkpoints==3376 && cooperative.yield_count==3376);
    assert(fegetround()==mode && fetestexcept(FE_ALL_EXCEPT)==flags);
    cooperative.yield_count=0;
    assert(memcmp(&sync,&cooperative,sizeof sync)==0);
    const unsigned cancel_at[]={1,2,135,136,3375,3376};
    for(unsigned k=0;k<sizeof cancel_at/sizeof cancel_at[0];k++){
        state.calls=0;state.cancel_at=cancel_at[k];
        assert(!ksn_mesh_q7_preflight_run_cooperative(memory,n,&cooperative,mutate_environment,&state));
        assert(cooperative.cancelled && cooperative.environment_restored);
        assert(!cooperative.invariant_failures && !cooperative.mismatch_count &&
               !cooperative.failure_result_mismatches);
        assert(state.calls==state.cancel_at && cooperative.checkpoints==state.calls &&
               cooperative.yield_count==state.calls && cooperative.pixels==state.calls*240u);
        assert(fegetround()==mode && fetestexcept(FE_ALL_EXCEPT)==flags);
    }
    verify_numeric_env=false;
    puts("PREFLIGHT_COOPERATIVE_PASS checkpoints=3376 exact_totals caller_numeric_fenv cancellation=1,2,135,136,3375,3376");
}
static void incremental_tests(void *memory,size_t n){
    ksn_mesh_q7_preflight_report sync,incremental,other;
    fenv_t original;assert(fegetenv(&original)==0);
    int original_mode=fegetround(),original_flags=fetestexcept(FE_ALL_EXCEPT);
    assert(ksn_mesh_q7_preflight_run(memory,n,&sync));
    memset(memory,0,n);
    assert(ksn_mesh_q7_preflight_step(memory,n,&incremental)==KSN_MESH_Q7_PREFLIGHT_FAILED);
    unsigned before=render_calls;
    assert(ksn_mesh_q7_preflight_begin(memory,n,&incremental));
    assert(render_calls==before && incremental.checkpoints==0);
    assert(fegetround()==original_mode && fetestexcept(FE_ALL_EXCEPT)==original_flags);
    assert(ksn_mesh_q7_preflight_step(memory,n,&other)==KSN_MESH_Q7_PREFLIGHT_FAILED);
    assert(ksn_mesh_q7_preflight_step(memory,n-1,&incremental)==KSN_MESH_Q7_PREFLIGHT_FAILED);
    const int modes[]={FE_UPWARD,FE_TOWARDZERO,FE_DOWNWARD,FE_TONEAREST};
    verify_numeric_env=true;numeric_mode=original_mode;numeric_divzero=original_flags&FE_DIVBYZERO;
    for(unsigned i=0;i<3376;i++){
        assert(fesetround(modes[i%4])==0);
        assert(feclearexcept(FE_ALL_EXCEPT)==0);
        assert(feraiseexcept(FE_DIVBYZERO)==0);
        int flags=fetestexcept(FE_ALL_EXCEPT);
        before=render_calls;
        unsigned cases=(unsigned)incremental.cases;
        ksn_mesh_q7_preflight_step_result result=ksn_mesh_q7_preflight_step(memory,n,&incremental);
        assert(result==(i==3375?KSN_MESH_Q7_PREFLIGHT_DONE:KSN_MESH_Q7_PREFLIGHT_MORE));
        assert(render_calls-before==2 && incremental.checkpoints==i+1 &&
               incremental.pixels==(i+1)*240u && incremental.cases-cases<=1);
        assert(fegetround()==modes[i%4] && fetestexcept(FE_ALL_EXCEPT)==flags);
    }
    verify_numeric_env=false;
    assert(memcmp(&sync,&incremental,sizeof sync)==0);
    before=render_calls;
    assert(ksn_mesh_q7_preflight_step(memory,n,&incremental)==KSN_MESH_Q7_PREFLIGHT_FAILED);
    assert(render_calls==before);
    assert(!ksn_mesh_q7_preflight_begin(NULL,n,&incremental));
    assert(!ksn_mesh_q7_preflight_begin(memory,n-1,&incremental));
    assert(!ksn_mesh_q7_preflight_begin((char *)memory+1,n-1,&incremental));
    assert(!ksn_mesh_q7_preflight_begin(memory,n,(void *)memory));
    assert(fesetenv(&original)==0);
    /* Cancel by discarding/reinitializing state at scene boundaries and in
     * a scene; no finalizer or outstanding numeric environment is needed. */
    const unsigned stops[]={1,2,135,136,3375};
    for(unsigned k=0;k<sizeof stops/sizeof stops[0];k++){
        assert(ksn_mesh_q7_preflight_begin(memory,n,&incremental));
        for(unsigned i=0;i<stops[k];i++)
            assert(ksn_mesh_q7_preflight_step(memory,n,&incremental)==KSN_MESH_Q7_PREFLIGHT_MORE);
        assert(fegetround()==original_mode && fetestexcept(FE_ALL_EXCEPT)==original_flags);
        memset(memory,0,n);
        assert(ksn_mesh_q7_preflight_step(memory,n,&incremental)==KSN_MESH_Q7_PREFLIGHT_FAILED);
    }
    assert(ksn_mesh_q7_preflight_begin(memory,n,&incremental));
    corrupt_rejection=1;
    for(unsigned i=0;i<3376;i++)
        assert(ksn_mesh_q7_preflight_step(memory,n,&incremental)==
               (i==3375?KSN_MESH_Q7_PREFLIGHT_FAILED:KSN_MESH_Q7_PREFLIGHT_MORE));
    corrupt_rejection=0;
    assert(incremental.invariant_failures==2 && incremental.environment_restored);
    assert(ksn_mesh_q7_preflight_step(memory,n,&incremental)==KSN_MESH_Q7_PREFLIGHT_FAILED);
    assert(fegetround()==original_mode && fetestexcept(FE_ALL_EXCEPT)==original_flags);
    puts("PREFLIGHT_INCREMENTAL_PASS steps=3376 two_renders_per_step exact_totals guest_fenv_isolation lifecycle cancellation");
}
static void quantum_tests(void *memory,size_t n){
    ksn_mesh_q7_preflight_report oracle,r;
    assert(ksn_mesh_q7_preflight_run(memory,n,&oracle));
    assert(ksn_mesh_q7_preflight_begin(memory,n,&r));
    unsigned phase_counts[KSN_MESH_Q7_PHASE_COUNT]={0};
    for(unsigned i=0;i<10153;i++){
        ksn_mesh_q7_preflight_work work;
        assert(ksn_mesh_q7_preflight_progress(memory,n,&r,&work));
        assert(work.scene==r.checkpoints/KSN_PROC_H);
        assert(work.fixture==(work.scene==25?39:work.scene<15?work.scene:work.scene+15));
        assert(work.row==(work.scene==25?KSN_PROC_H:r.checkpoints%KSN_PROC_H));
        if(work.phase==KSN_MESH_Q7_SETUP)assert(work.row==0 && work.scene<25);
        phase_counts[work.phase]++;
        unsigned before=render_calls;uint64_t checkpoints=r.checkpoints;
        int mode=fegetround(),flags=fetestexcept(FE_ALL_EXCEPT);
        assert(ksn_mesh_q7_preflight_quantum(memory,n,&r)==
            (i==10152?KSN_MESH_Q7_PREFLIGHT_DONE:KSN_MESH_Q7_PREFLIGHT_MORE));
        assert(render_calls-before==(unsigned)(work.phase==KSN_MESH_Q7_RENDER_ENABLED ||
                                               work.phase==KSN_MESH_Q7_RENDER_DISABLED));
        assert(r.checkpoints==checkpoints+(work.phase==KSN_MESH_Q7_COMPARE));
        assert(fegetround()==mode && fetestexcept(FE_ALL_EXCEPT)==flags);
    }
    assert(phase_counts[0]==25 && phase_counts[1]==3376 && phase_counts[2]==3376 && phase_counts[3]==3376);
    assert(memcmp(&oracle,&r,sizeof r)==0);
    ksn_mesh_q7_preflight_work work;
    assert(!ksn_mesh_q7_preflight_progress(memory,n,&r,&work));
    assert(ksn_mesh_q7_preflight_quantum(memory,n,&r)==KSN_MESH_Q7_PREFLIGHT_FAILED);
    /* Legacy callers may finish any partial quantum row without skipping it. */
    for(unsigned partial=1;partial<=3;partial++){
        assert(ksn_mesh_q7_preflight_begin(memory,n,&r));
        for(unsigned i=0;i<partial;i++)assert(ksn_mesh_q7_preflight_quantum(memory,n,&r)==KSN_MESH_Q7_PREFLIGHT_MORE);
        assert(r.checkpoints==0);
        assert(ksn_mesh_q7_preflight_step(memory,n,&r)==KSN_MESH_Q7_PREFLIGHT_MORE);
        assert(r.checkpoints==1 && r.pixels==KSN_PROC_W);
    }
    puts("PREFLIGHT_QUANTUM_PASS 25 setup + 3376 per render/compare; exact corpus; partial row compatibility");
}
int main(void){
    fenv_t saved;assert(fegetenv(&saved)==0);
    size_t n=ksn_mesh_q7_preflight_bytes();void *memory=malloc(n);assert(memory);
    assert(n<16384 && (size_t)memory%ksn_mesh_q7_preflight_alignment()==0);
    assert(fesetround(FE_TONEAREST)==0);assert(feraiseexcept(FE_INVALID)==0);run(memory);cooperative_tests(memory,n);incremental_tests(memory,n);quantum_tests(memory,n);
    assert(fesetround(FE_DOWNWARD)==0);run(memory);cooperative_tests(memory,n);incremental_tests(memory,n);quantum_tests(memory,n);
    assert(fesetround(FE_UPWARD)==0);run(memory);
    assert(fesetround(FE_TOWARDZERO)==0);run(memory);
    assert(fesetround(FE_TONEAREST)==0);
#if defined(__SSE__)
    unsigned csr=_mm_getcsr();_mm_setcsr(csr|0x8040u);run(memory);assert(_mm_getcsr()==(csr|0x8040u));_mm_setcsr(csr);
#endif
    ksn_mesh_q7_preflight_report r;
    assert(!ksn_mesh_q7_preflight_run(memory,n-1,&r) && r.invariant_failures);
    assert(!ksn_mesh_q7_preflight_run((char *)memory+1,n-1,&r));
    assert(!ksn_mesh_q7_preflight_run(NULL,n,&r));
    for(corrupt_rejection=1;corrupt_rejection<=3;corrupt_rejection++){
        assert(!ksn_mesh_q7_preflight_run(memory,n,&r));
        assert(r.invariant_failures==2 && r.mismatch_count==0 &&
               r.failure_result_mismatches==0 && r.environment_restored);
    }
    corrupt_rejection=0;
    puts("PREFLIGHT_REJECTION_SENTINELS_PASS rgb depth owner common-mode writes rejected");
    free(memory);assert(fesetenv(&saved)==0);return 0;
}
