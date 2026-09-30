#include "ksn_proc_plan.h"

/* Kept in a separate translation unit so scalar VM users can link the plan
 * without pulling in a point backend. Registration still lives in the same
 * owned plan and is invalidated by ksn_proc_plan_prepare. */
bool ksn_proc_plan_register_points_affine(ksn_proc_plan *plan,
                                           const KsnProcAffineQ14 *coeff,
                                           const KsnProcPointsPolicy *policy){
    if(!plan||!plan->valid||!coeff)return false;
    plan->points_registered=false;
    plan->points_coeff=*coeff;
    plan->points_policy=policy?*policy:(KsnProcPointsPolicy){false,0};
    plan->points_registered=true;
    return true;
}

bool ksn_proc_plan_run_points_affine(const ksn_proc_plan *plan,
                                      KsnProcPointDst dst,KsnProcPointSrc src,
                                      size_t n,KsnProcPointsDecision *decision){
    if(!plan||!plan->valid||!plan->points_registered)return false;
    KsnProcPointsDecision selected=ksn_proc_points_affine_dispatch(
        dst,src,n,&plan->points_coeff,&plan->points_policy);
    if(decision)*decision=selected;
    return true;
}

bool ksn_proc_sized_plan_register_points_affine(ksn_proc_sized_plan *plan,
                                                 const KsnProcAffineQ14 *coeff,
                                                 const KsnProcPointsPolicy *policy){
    if(!plan||!plan->valid||!coeff)return false;
    plan->points_registered=false;
    plan->points_coeff=*coeff;
    plan->points_policy=policy?*policy:(KsnProcPointsPolicy){false,0};
    plan->points_registered=true;
    return true;
}

bool ksn_proc_sized_plan_run_points_affine(const ksn_proc_sized_plan *plan,
                                            KsnProcPointDst dst,KsnProcPointSrc src,
                                            size_t n,KsnProcPointsDecision *decision){
    if(!plan||!plan->valid||!plan->points_registered)return false;
    KsnProcPointsDecision selected=ksn_proc_points_affine_dispatch(
        dst,src,n,&plan->points_coeff,&plan->points_policy);
    if(decision)*decision=selected;
    return true;
}
