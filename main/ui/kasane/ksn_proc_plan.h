#ifndef KSN_PROC_PLAN_H
#define KSN_PROC_PLAN_H
#include <stddef.h>
#include "ksn_proc_analysis.h"
#include "ksn_proc_points_dispatch.h"

/* One bit per pc: a fused arithmetic pair starts there. Two words, so the
 * plan stays 4-byte aligned on Xtensa (a uint64_t would force 8). */
_Static_assert(KSN_PROC_CODE <= 64, "fused marks are a 64-bit set");
#define KSN_PROC_FUSED_WORDS 2u

/* Registration-time, bounded scalar plan. The plan owns its validated IR.
 * A span starts at pc only when two adjacent arithmetic instructions have a
 * def-use dependence and occupy the same basic block. All other PCs use VM
 * stepping. No code is moved or elided. Prepare may reuse the plan's own IR.
 * Begin binds code directly, so a copied plan owns its execution too; program
 * is a preparation-time view and its code pointer must not be used after a
 * struct copy. Keep a plan immutable between prepare and run.
 * This full-size form (64 instructions whatever the count) is for stack and
 * static plans; a heap plan that pays for its own count is below. */
typedef struct {
    ksn_proc_inst code[KSN_PROC_CODE];
    ksn_proc_program program;
    uint32_t fused[KSN_PROC_FUSED_WORDS];
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

/* The same plan, allocated for its own instruction count: the header, then
 * `capacity` instructions in a flexible array. pocket.kasane.procedural keeps
 * up to 32 of these at once, and the full-size form cost 872 B each however
 * short the program (DERBY registers seven one-instruction plans), all of it
 * internal DRAM shared with the guest heap (docs/kasane/plan-sized-alloc.md).
 * The caller allocates ksn_proc_sized_plan_bytes(capacity) and prepares with
 * that capacity; prepare never touches memory past it. Semantics, limits and
 * fusion are the full-size plan's, byte for byte: both run the same code.
 * Not copyable by assignment (the instructions are not part of the type). */
typedef struct {
    KsnProcAffineQ14 points_coeff;
    KsnProcPointsPolicy points_policy;
    uint32_t fused[KSN_PROC_FUSED_WORDS];
    uint8_t capacity,count,fused_count;
    bool valid,points_registered;
    ksn_proc_inst code[];
} ksn_proc_sized_plan;
_Static_assert(sizeof(ksn_proc_sized_plan)==offsetof(ksn_proc_sized_plan,code),
               "no tail padding: the byte count below is exact");

/* 0 for a count outside 1..KSN_PROC_CODE. */
size_t ksn_proc_sized_plan_bytes(unsigned capacity);
/* program->count must be 1..capacity and capacity 1..KSN_PROC_CODE. On
 * failure the plan is invalid (runs take the VM's reference path, as for the
 * full-size plan). program->code may point into plan->code. */
bool ksn_proc_sized_plan_prepare(ksn_proc_sized_plan *plan,unsigned capacity,
                                 const ksn_proc_program *program);
ksn_proc_status ksn_proc_sized_plan_begin(ksn_proc_vm *vm,const ksn_proc_sized_plan *plan,
                                          const float input[KSN_PROC_INPUTS],
                                          ksn_proc_frame *frame);
ksn_proc_status ksn_proc_sized_plan_run(ksn_proc_vm *vm,const ksn_proc_sized_plan *plan,
                                        bool debug_step);
bool ksn_proc_sized_plan_register_points_affine(ksn_proc_sized_plan *plan,
                                                 const KsnProcAffineQ14 *coeff,
                                                 const KsnProcPointsPolicy *policy);
bool ksn_proc_sized_plan_run_points_affine(const ksn_proc_sized_plan *plan,
                                            KsnProcPointDst dst, KsnProcPointSrc src,
                                            size_t n, KsnProcPointsDecision *decision);
#endif
