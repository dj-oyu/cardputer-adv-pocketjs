#include "ui/kasane/ksn_mesh_q7_preflight.h"
#include <assert.h>
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
/* Host-only fault injection: both arms make the same forbidden write on a
 * rejected call. Equality alone must not let this regression pass. */
static unsigned corrupt_rejection;
extern bool __real_ksn_mesh_q7_render(const ksn_mesh_q7_snapshot *,uint16_t *,
    unsigned,unsigned,float *,uint8_t *,unsigned,ksn_mesh_q7_stats *);
bool __wrap_ksn_mesh_q7_render(const ksn_mesh_q7_snapshot *s,uint16_t *rgb,
    unsigned y,unsigned h,float *depth,uint8_t *owner,unsigned rows,ksn_mesh_q7_stats *stats){
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
int main(void){
    fenv_t saved;assert(fegetenv(&saved)==0);
    size_t n=ksn_mesh_q7_preflight_bytes();void *memory=malloc(n);assert(memory);
    assert(n<16384 && (size_t)memory%ksn_mesh_q7_preflight_alignment()==0);
    assert(fesetround(FE_TONEAREST)==0);assert(feraiseexcept(FE_INVALID)==0);run(memory);
    assert(fesetround(FE_DOWNWARD)==0);run(memory);
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
