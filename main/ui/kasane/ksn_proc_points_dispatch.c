#include "ksn_proc_points_dispatch.h"

#include "ksn_proc_points_pie.h"

#include <stdint.h>

bool ksn_proc_points_pie_backend_available(void)
{
#if KSN_PROC_POINTS_HAS_PIE
    return true;
#else
    return false;
#endif
}

static bool aligned16(const void *p)
{
    return p != NULL && ((uintptr_t)p & (uintptr_t)15) == 0;
}

static KsnProcPointsDecision scalar(KsnProcPointsReason reason)
{
    KsnProcPointsDecision d = { KSN_PROC_POINTS_SCALAR, reason };
    return d;
}

KsnProcPointsDecision ksn_proc_points_select_for_backend(
    KsnProcPointDst dst, KsnProcPointSrc src, size_t n,
    const KsnProcPointsPolicy *policy, bool backend_available)
{
    if (n == 0) return scalar(KSN_PROC_POINTS_EMPTY);
    if (policy == NULL || !policy->enable_pie)
        return scalar(KSN_PROC_POINTS_DISABLED);
    if (policy->min_pie_points == 0)
        return scalar(KSN_PROC_POINTS_UNCALIBRATED);
    if (!backend_available)
        return scalar(KSN_PROC_POINTS_BACKEND_UNAVAILABLE);
    if (!aligned16(dst.x) || !aligned16(dst.y) ||
        !aligned16(src.x) || !aligned16(src.y))
        return scalar(KSN_PROC_POINTS_UNALIGNED);
    if (n < 8 || n < policy->min_pie_points)
        return scalar(KSN_PROC_POINTS_BELOW_MIN_POINTS);
    KsnProcPointsDecision d = { KSN_PROC_POINTS_PIE,
                                KSN_PROC_POINTS_PIE_SELECTED };
    return d;
}

KsnProcPointsDecision ksn_proc_points_affine_dispatch(
    KsnProcPointDst dst, KsnProcPointSrc src, size_t n,
    const KsnProcAffineQ14 *coeff, const KsnProcPointsPolicy *policy)
{
    KsnProcPointsDecision d = ksn_proc_points_select_for_backend(
        dst, src, n, policy, ksn_proc_points_pie_backend_available());
    if (n == 0) return d;
    if (d.backend == KSN_PROC_POINTS_PIE)
        ksn_proc_points_affine_pie(dst, src, n, coeff);
    else
        ksn_proc_points_affine_scalar(dst, src, n, coeff);
    return d;
}
