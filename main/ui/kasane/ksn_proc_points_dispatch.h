#ifndef KSN_PROC_POINTS_DISPATCH_H
#define KSN_PROC_POINTS_DISPATCH_H

#include "ksn_proc_points.h"

#include <stdbool.h>
#include <stddef.h>

/* A threshold of zero means no device calibration has been supplied. The
 * caller must measure and set a positive threshold before PIE is eligible.
 * PIE also needs at least one complete eight-point block. */
typedef struct {
    bool enable_pie;
    size_t min_pie_points;
} KsnProcPointsPolicy;

typedef enum {
    KSN_PROC_POINTS_SCALAR,
    KSN_PROC_POINTS_PIE
} KsnProcPointsBackend;

typedef enum {
    KSN_PROC_POINTS_EMPTY,
    KSN_PROC_POINTS_DISABLED,
    KSN_PROC_POINTS_UNCALIBRATED,
    KSN_PROC_POINTS_BACKEND_UNAVAILABLE,
    KSN_PROC_POINTS_UNALIGNED,
    KSN_PROC_POINTS_BELOW_MIN_POINTS,
    KSN_PROC_POINTS_PIE_SELECTED
} KsnProcPointsReason;

typedef struct {
    KsnProcPointsBackend backend;
    KsnProcPointsReason reason;
} KsnProcPointsDecision;

/* Compile-time device/PIE gate. It is false for every host build. */
bool ksn_proc_points_pie_backend_available(void);

/* Pure policy decision, exposed so host tests can model both backend states.
 * The backend_available argument never overrides the device gate in the
 * executing entry point below. Pointers must otherwise obey the scalar point
 * contract; alignment is checked here as a defensive PIE eligibility gate. */
KsnProcPointsDecision ksn_proc_points_select_for_backend(
    KsnProcPointDst dst, KsnProcPointSrc src, size_t n,
    const KsnProcPointsPolicy *policy, bool backend_available);

/* Selects using the actual compiled backend, then executes. NULL policy uses
 * scalar. For n == 0, all pointers including coeff may be NULL and no memory
 * is accessed. For n > 0, the point and overlap contract in
 * ksn_proc_points.h applies. Returns the decision actually used. */
KsnProcPointsDecision ksn_proc_points_affine_dispatch(
    KsnProcPointDst dst, KsnProcPointSrc src, size_t n,
    const KsnProcAffineQ14 *coeff, const KsnProcPointsPolicy *policy);

#endif
