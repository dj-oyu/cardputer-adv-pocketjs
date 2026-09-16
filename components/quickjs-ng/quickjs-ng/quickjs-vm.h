// L2 VM harness hooks (docs/vm/vm-L2-design.md sec.1.2 / sec.1.2 G5 / sec.4).
//
// NOT part of the upstream quickjs-ng import. Like quickjs-vmprobe.h it sits
// next to the vendored sources so quickjs.c can reach it with a plain include,
// and it is kept out of quickjs.h so re-vendoring never has to route around it.
//
// What lives here is the seam between the interpreter loop and the two things
// the L2 gates need from it:
//
//   1. FORCED YIELD -- stop at every class-A safepoint (the seven existing
//      goto / if_* poll sites, design sec.7.2). Before L2c the VM has exactly
//      one way to stop, the uncatchable "interrupted" error (design sec.4.1),
//      so a forced yield today KILLS the running job. That is the point: the
//      gate `run.sh --force-yield` must go red until save-and-resume exists.
//   2. G5 -- the longest non-interruptible section. Every stop OPPORTUNITY
//      (a safepoint, or JS being entered from / returned to the host) closes
//      a gap; the maximum gap is the worst-case latency between a stop
//      request and the VM honouring it, for THIS set of safepoints.
//
// Cost when nothing is armed: one NULL pointer (file-scope in quickjs.c, so
// JSRuntime keeps its size and heap footprint). The fast path of every poll is
// byte-identical to upstream (the counter decrement); the only additions on
// the default path are a NULL test on `sf->prev_frame` at the four places a
// frame is popped, taken only for the outermost frame.
//
// Deliberately NOT routed through JS_SetInterruptHandler: that handler is the
// session's "stop for good" predicate (host_stopping in vmrun, the watchdog in
// guest.c). Sharing it would make "terminate" and "stop here, resume later"
// indistinguishable at the one place they must differ (design sec.1.2).
#pragma once

#include <stdint.h>
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Diagnostic-only, idle-runtime dispatch selection. No shipping symbol/field.
#ifdef CONFIG_POCKET_VM_CALLBENCH
int vmtest_call_mode(JSRuntime *rt, int recursive);
int vmtest_call_inputs_eager(JSRuntime *rt, int eager);
#endif

// Where a stop opportunity was: what ended (and what started) a gap.
typedef enum {
    JS_VM_OPP_NONE = 0,
    JS_VM_OPP_SAFEPOINT,   // class-A branch or class-B completed push
    JS_VM_OPP_ENTER,       // JS entered from the host (outermost JS_CallInternal)
    JS_VM_OPP_LEAVE,       // outermost frame popped, control back in the host
} JSVMOpportunity;

// One recorded gap. Atoms are owned (JS_DupAtomRT) so they can be resolved at
// report time even if the function that owned them has since been freed.
typedef struct {
    uint64_t ns;
    JSVMOpportunity start_kind, end_kind;
    JSAtom start_func, end_func;   // JS_ATOM_NULL when unknown / not bytecode
} JSVMGap;

#define JS_VM_TOP_GAPS 8

typedef uint64_t (*JSVMClock)(void);

typedef struct JSVMState {
    uint8_t force_yield;   // stop at every yieldable A/B safepoint
    uint8_t gap_on;        // record gaps (needs a clock)
    uint8_t in_js;         // between an outermost ENTER and its LEAVE
    // Host-handler cadence while armed. Armed mode forces every poll into the
    // slow path (ctx->interrupt_counter stays at 1) so safepoints are seen one
    // by one; this shadow counter keeps rt->interrupt_handler called at the
    // same JS_INTERRUPT_COUNTER_INIT stride as the unarmed VM, so arming does
    // not move WHEN a session stop is honoured.
    int host_poll_left;
    JSVMClock clock;
    // counters (reported as #info by the harness)
    uint64_t safepoints;   // yieldable A/B sites (A polls before L2c)
    uint64_t stops;        // forced stops accepted
    uint64_t enters, leaves;
    // H14: sampled only at actual parks while the harness is armed. Segment
    // bytes are live rounded frame bytes, not capacity or allocator overhead.
    uint64_t susp_samples;
    uint64_t susp_bytes_max;
    uint64_t susp_async_frames; // maximum heap frames in any one parked chain
    uint64_t gaps;         // gaps closed inside JS
    uint64_t gap_ns_total;
    // G5 proper
    uint64_t gap_start_ns;
    JSVMOpportunity gap_start_kind;
    JSAtom gap_start_func;   // owned
    JSVMGap top[JS_VM_TOP_GAPS];   // sorted, top[0] is the maximum
    int top_n;
} JSVMState;

// ---- called by quickjs.c (definitions in quickjs-vm.c) ----

// A class-A/B safepoint was reached while armed. `func` is the running
// function's name atom (JS_ATOM_NULL if not bytecode); borrowed, not owned.
// Returns nonzero when the VM must stop here.
int js_vm_safepoint(JSRuntime *rt, JSVMState *vm, JSAtom func);
// Outermost entry into JS (the JS_CallInternal prologue poll saw no frame).
void js_vm_enter(JSRuntime *rt, JSVMState *vm);
// Outermost frame popped. `func` as for js_vm_safepoint.
void js_vm_leave(JSRuntime *rt, JSVMState *vm, JSAtom func);
// The state block lives outside the guest heap (plain calloc/free): quickjs.c
// forbids malloc after JS_NewRuntime2 and the guest allocator would count it
// against JS_SetMemoryLimit. Free releases the owned atoms and the block.
JSVMState *js_vm_state_alloc(void);
void js_vm_state_free(JSRuntime *rt, JSVMState *vm);

// ---- defined in quickjs.c for quickjs-vm.c and the harness ----

// The armed state, or NULL. Arming allocates it (plain malloc: it must not
// count against the guest heap limit, and a yield must not allocate).
// `on` = 0 disarms and frees. Also re-seats every context's interrupt
// counter so the first safepoint after arming is already observed.
JSVMState *js_vm_arm(JSRuntime *rt, int on);
JSVMState *js_vm_state(JSRuntime *rt);

// ---- harness contract (defined in quickjs-vm.c) ----
//
// Weak-referenced from tools/vmtest/vmrun.c; the names are the contract.
void vmtest_vm_set_force_yield(JSRuntime *rt, int on);
// Turns G5 recording on with the given clock (monotonic ns).
void vmtest_vm_set_gap_clock(JSRuntime *rt, JSVMClock clock, int on);
// Prints "#info vm ..." / "#info g5 ..." lines to `out` (a FILE *) and
// resolves the recorded atoms through `ctx`. Does not disarm.
void vmtest_vm_report(JSRuntime *rt, JSContext *ctx, void *out);
// L2a segment stack (quickjs-vmstack.h). Configure only before the first
// call runs; returns -1 if the build keeps frames on the C stack or a
// segment already exists. Report prints one "#info vmstack ..." line
// (nothing when the build has no segment stack).
int vmtest_vmstack_configure(JSRuntime *rt, size_t seg_size, unsigned cache_max);
int vmtest_vmstack_configure_growth(JSRuntime *rt, size_t first, size_t maximum);
void vmtest_vmstack_report(JSRuntime *rt, void *out);
// D10: set the segment byte budget ALONE, leaving the C-stack limit where
// JS_SetMaxStackSize put it (which sets both). 0 = no budget. This is how
// the harness shows which of the two guards answered: with the C-stack
// limit raised out of the way, a run ends in RangeError only if the budget
// caught it, and in InternalError if the heap did. Returns -1 when the build
// keeps frames on the C stack (there is no budget to set).
int vmtest_vmstack_set_budget(JSRuntime *rt, size_t bytes);

// ---------------------------------------------------------------- L2c (D18r / D22r)
//
// The gate (docs/vm/vm-L2-design.md sec.11.3/13): a fixed set of C callers that
// can receive a "yielded" result from the VM and must know how to resume it.
// With CONFIG_POCKET_VM_YIELD disabled these entry points remain plain
// pass-throughs and JS_VMSuspended is always false.
//
// Where a suspended chain's resume ended up starting from. The four rows of
// sec.12.4's table: a host-owned SEG floor is either a plain eval/call
// (ORIGIN_HOST) or a job the scheduler put on hold mid-chain (ORIGIN_JOB_HELD,
// D36). An internal async continuation completes its job before the heap
// owner resumes (ORIGIN_JOB_ASYNC). An async handler's initial stretch can
// instead hold the enclosing job's tail, and reports ORIGIN_JOB_HELD.
typedef enum {
    JS_VM_ORIGIN_NONE = 0,       // not suspended (or never has been this call)
    JS_VM_ORIGIN_HOST,           // JS_VMCall / JS_VMEval floor
    JS_VM_ORIGIN_JOB_HELD,       // JS_VMCallJob floor, chain outlived the job
    JS_VM_ORIGIN_JOB_ASYNC,      // async function/generator floor
} JSVMOrigin;

// True while a chain is parked in rt->vm_susp. With POCKET_VM_YIELD off this
// is the stage-1 pass-through and always returns 0.
int JS_VMSuspended(JSRuntime *rt);

// Where the most recently completed (or still-parked) chain's floor sits.
// Pass-through build: always JS_VM_ORIGIN_NONE, since JS_VMSuspended is
// always false and there is never a floor to report.
JSVMOrigin JS_VMSuspendedOrigin(JSRuntime *rt);

// Resume a parked chain through its owner and finish any held job tail.
// Check JS_VMSuspended again: JS_EXCEPTION alone is not an exception while
// parked. A completed held job consumes its result and returns undefined.
JSValue JS_VMResume(JSContext *ctx);

// Terminate on the next resume, bypassing catch/finally even under OOM.
// Discard releases a parked chain without running JS (also used at teardown).
// Both are no-ops when no chain is parked, including pass-through builds.
void JS_VMTerminate(JSRuntime *rt);
void JS_VMDiscard(JSRuntime *rt);

// Request may be called by another thread/timer while the runtime is alive.
// Requests coalesce; only a retired A/B boundary on a yieldable floor accepts
// one. Clear before a non-yielding leave turn. Stop/join the producer before
// freeing the runtime. Neither function allocates or touches a JS context.
void JS_VMRequestYield(JSRuntime *rt);
void JS_VMClearYield(JSRuntime *rt);

// JS_Call, wrapped so a caller that receives a suspended chain back can tell
// it apart from a normal return. Pass-through build: identical to JS_Call.
JSValue JS_VMCall(JSContext *ctx, JSValueConst func_obj, JSValueConst this_obj,
                  int argc, JSValueConst *argv);

// JS_Eval, wrapped the same way. Pass-through build: identical to JS_Eval.
JSValue JS_VMEval(JSContext *ctx, const char *input, size_t input_len,
                  const char *filename, int eval_flags);

#ifdef __cplusplus
}
#endif
