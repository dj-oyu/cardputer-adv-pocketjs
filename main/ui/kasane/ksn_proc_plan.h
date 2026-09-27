#ifndef KSN_PROC_PLAN_H
#define KSN_PROC_PLAN_H
#include "ksn_proc_analysis.h"
#include "ksn_proc_points_dispatch.h"

/* Registration-time, bounded scalar plan. The plan owns its validated IR.
 * A span starts at pc only when two adjacent arithmetic instructions have a
 * def-use dependence and occupy the same basic block. All other PCs use VM
 * stepping. No code is moved or elided. Prepare may reuse the plan's own IR.
 * Begin binds code directly, so a copied plan owns its execution too; program
 * is a preparation-time view and its code pointer must not be used after a
 * struct copy. Keep a plan immutable between prepare and run. */
typedef struct {
    ksn_proc_inst code[KSN_PROC_CODE];
    ksn_proc_program program;
    uint8_t fused_at[KSN_PROC_CODE];
    uint8_t fused_count;
    bool valid;
    /* Opt-in typed sidecar. It owns values only, never point buffers. */
    KsnProcAffineQ14 points_coeff;
    KsnProcPointsPolicy points_policy;
    bool points_registered;
} ksn_proc_plan;

bool ksn_proc_plan_prepare(ksn_proc_plan *plan,const ksn_proc_program *program);
ksn_proc_status ksn_proc_plan_begin(ksn_proc_vm *vm,const ksn_proc_plan *plan,
                                    const float input[KSN_PROC_INPUTS],ksn_proc_frame *frame);
ksn_proc_status ksn_proc_plan_begin_state(ksn_proc_vm *vm,const ksn_proc_plan *plan,
                                          const float input[KSN_PROC_INPUTS],
                                          const ksn_proc_state *state,ksn_proc_frame *frame);
/* debug_step keeps the original per-instruction VM path for observable steps. */
ksn_proc_status ksn_proc_plan_run(ksn_proc_vm *vm,const ksn_proc_plan *plan,bool debug_step);

/* Register an independent Q14 point batch on an already validated plan.
 * Re-preparing the IR clears this registration. NULL policy requests scalar.
 * Neither operation changes float VM instructions, state, limits or debug
 * stepping. The coefficients and policy are copied, so callers may release
 * their descriptors after registration. */
bool ksn_proc_plan_register_points_affine(ksn_proc_plan *plan,
                                           const KsnProcAffineQ14 *coeff,
                                           const KsnProcPointsPolicy *policy);

/* Executes the registered batch using the point/overlap contract in
 * ksn_proc_points.h. Returns false without writing for an unregistered plan;
 * decision is untouched on failure. On success it reports the actual backend
 * chosen by dispatch, including scalar fallback and the empty batch case. */
bool ksn_proc_plan_run_points_affine(const ksn_proc_plan *plan,
                                      KsnProcPointDst dst, KsnProcPointSrc src,
                                      size_t n, KsnProcPointsDecision *decision);
#endif
