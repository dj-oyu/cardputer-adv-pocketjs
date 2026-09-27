#include "ksn_proc_plan.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

enum { CAP = 40 };
#define I(op,dst,a,b,v,c) {op,dst,a,b,v,c}

#ifdef KSN_PROC_PLAN_POINTS_STUB_PIE
static unsigned pie_calls;
/* Routing stub: verifies the plan calls the selected backend. The real PIE
 * arithmetic and block/tail behavior have their own host model test. */
void ksn_proc_points_affine_pie(KsnProcPointDst dst,KsnProcPointSrc src,
                                 size_t n,const KsnProcAffineQ14 *coeff){
    pie_calls++;
    ksn_proc_points_affine_scalar(dst,src,n,coeff);
}
#endif

static uint32_t next(uint32_t *seed){
    *seed=*seed*UINT32_C(1664525)+UINT32_C(1013904223);
    return *seed;
}

static void one_case(ksn_proc_plan *plan,size_t n,unsigned alias,
                     bool enable,uint32_t *seed){
    _Alignas(16) int16_t sx[CAP],sy[CAP],dx[CAP],dy[CAP],ex[CAP],ey[CAP];
    int16_t oldx[CAP],oldy[CAP];
    const KsnProcAffineQ14 coeff={
        (int16_t)(next(seed)>>16),(int16_t)(next(seed)>>16),
        (int16_t)(next(seed)>>16),(int16_t)(next(seed)>>16),
        (int32_t)next(seed),(int32_t)next(seed)
    };
    KsnProcPointsPolicy policy={enable,8};
    assert(ksn_proc_plan_register_points_affine(plan,&coeff,&policy));
    for(size_t i=0;i<CAP;i++){
        sx[i]=(int16_t)(next(seed)>>16);
        sy[i]=(int16_t)(next(seed)>>16);
        dx[i]=dy[i]=ex[i]=ey[i]=0x5a5a;
    }
    memcpy(oldx,sx,sizeof sx);memcpy(oldy,sy,sizeof sy);
    ksn_proc_points_affine_scalar((KsnProcPointDst){ex,ey},
        (KsnProcPointSrc){sx,sy},n,&coeff);
    KsnProcPointDst dst={(alias&1)?sx:dx,(alias&2)?sy:dy};
    KsnProcPointSrc src={sx,sy};
    /* The plan owns registration values, independent of caller storage. */
    policy.enable_pie=false;
    KsnProcPointsDecision d={KSN_PROC_POINTS_PIE,KSN_PROC_POINTS_PIE_SELECTED};
    #ifdef KSN_PROC_PLAN_POINTS_STUB_PIE
    unsigned before=pie_calls;
    #endif
    assert(ksn_proc_plan_run_points_affine(plan,dst,src,n,&d));
    KsnProcPointsDecision selected=ksn_proc_points_select_for_backend(
        dst,src,n,&plan->points_policy,ksn_proc_points_pie_backend_available());
    assert(d.backend==selected.backend && d.reason==selected.reason);
    if(n==0)assert(d.reason==KSN_PROC_POINTS_EMPTY);
    else if(!enable)assert(d.reason==KSN_PROC_POINTS_DISABLED);
    else if(ksn_proc_points_pie_backend_available())
        assert(d.reason==(n>=8?KSN_PROC_POINTS_PIE_SELECTED:
                                   KSN_PROC_POINTS_BELOW_MIN_POINTS));
    else assert(d.reason==KSN_PROC_POINTS_BACKEND_UNAVAILABLE);
    #ifdef KSN_PROC_PLAN_POINTS_STUB_PIE
    assert(pie_calls==before+(unsigned)(enable&&n>=8));
    #endif
    for(size_t i=0;i<n;i++)assert(dst.x[i]==ex[i] && dst.y[i]==ey[i]);
    for(size_t i=n;i<CAP;i++){
        assert(dst.x[i]==((alias&1)?oldx[i]:0x5a5a));
        assert(dst.y[i]==((alias&2)?oldy[i]:0x5a5a));
    }
    /* Eligibility can be decided for a compiled PIE backend using the same
     * typed descriptor; host execution still obeys its real device gate. */
    selected=ksn_proc_points_select_for_backend(dst,src,n,
        &plan->points_policy,true);
    assert(selected.backend==((enable&&n>=8)?KSN_PROC_POINTS_PIE:
                             KSN_PROC_POINTS_SCALAR));
}

static void lifecycle(void){
    const ksn_proc_inst code[]={I(KSN_PROC_SET,0,0,0,4,0),
                                I(KSN_PROC_ADD,1,0,0,0,0)};
    const ksn_proc_program program={code,2};
    const KsnProcAffineQ14 coeff={16384,0,0,16384,0,0};
    const float input[KSN_PROC_INPUTS]={0};
    ksn_proc_plan plan,copy;
    KsnProcPointsDecision decision={KSN_PROC_POINTS_PIE,
                                    KSN_PROC_POINTS_PIE_SELECTED};
    assert(!ksn_proc_plan_prepare(&plan,NULL));
    assert(!ksn_proc_plan_register_points_affine(&plan,&coeff,NULL));
    assert(!ksn_proc_plan_run_points_affine(&plan,(KsnProcPointDst){0},
        (KsnProcPointSrc){0},0,&decision));
    assert(decision.backend==KSN_PROC_POINTS_PIE);
    assert(ksn_proc_plan_prepare(&plan,&program));
    assert(!ksn_proc_plan_register_points_affine(&plan,NULL,NULL));
    assert(ksn_proc_plan_register_points_affine(&plan,&coeff,NULL));
    copy=plan;
    assert(ksn_proc_plan_run_points_affine(&copy,(KsnProcPointDst){0},
        (KsnProcPointSrc){0},0,&decision));
    assert(decision.reason==KSN_PROC_POINTS_EMPTY);
    /* Q14 registration cannot alter the scalar float program or debugger. */
    ksn_proc_vm a,b;ksn_proc_frame fa,fb;
    assert(ksn_proc_begin(&a,&program,input,&fa)==KSN_PROC_RUNNING);
    assert(ksn_proc_plan_begin(&b,&copy,input,&fb)==KSN_PROC_RUNNING);
    while(a.status==KSN_PROC_RUNNING){
        assert(ksn_proc_step(&a)==ksn_proc_step(&b));
        assert(a.pc==b.pc && a.last_pc==b.last_pc && a.steps==b.steps);
        assert(memcmp(a.reg,b.reg,sizeof a.reg)==0);
    }
    assert(ksn_proc_plan_prepare(&plan,&program));
    assert(!plan.points_registered);
    assert(!ksn_proc_plan_run_points_affine(&plan,(KsnProcPointDst){0},
        (KsnProcPointSrc){0},0,&decision));
    assert(copy.points_registered);
    assert(ksn_proc_plan_prepare(&copy,&copy.program));
    assert(!copy.points_registered);
    assert(!ksn_proc_plan_prepare(&copy,NULL));
    assert(!copy.points_registered && !copy.valid);
}

int main(void){
    lifecycle();
    const ksn_proc_inst code[]={I(KSN_PROC_SET,0,0,0,0,0)};
    const ksn_proc_program program={code,1};
    ksn_proc_plan plan;
    assert(ksn_proc_plan_prepare(&plan,&program));
    uint32_t seed=UINT32_C(0x89abcdef);
    const size_t counts[]={0,1,7,8,9,15,16,17,39};
    for(unsigned repeat=0;repeat<10;repeat++)
        for(unsigned i=0;i<sizeof counts/sizeof counts[0];i++)
            for(unsigned alias=0;alias<4;alias++)
                for(unsigned enabled=0;enabled<2;enabled++)
                    one_case(&plan,counts[i],alias,enabled!=0,&seed);
    return 0;
}
