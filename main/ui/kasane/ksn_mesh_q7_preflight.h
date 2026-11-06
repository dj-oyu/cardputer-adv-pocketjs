#ifndef KSN_MESH_Q7_PREFLIGHT_H
#define KSN_MESH_Q7_PREFLIGHT_H
#include "ksn_mesh_q7_certified.h"
/* Synthetic numeric gate, not a hardware qualification or original-coverage
 * oracle. Run before timing. All buffers are caller owned and disjoint from
 * report; workspace requires the queried alignment. No allocation or I/O.
 * Safe fallback (including unavailable IEEE environment) is not an error.
 * Caller serializes access and keeps the floating environment stable. */
typedef struct {
    uint64_t cases, pixels, mismatch_count, failure_result_mismatches;
    uint64_t invariant_failures, safe_fallback_cases, certificate_cases;
    uint64_t render_failures, checkpoints, yield_count;
    int round_before, round_after;
    bool subnormal_ok, environment_restored, cancelled;
    ksn_mesh_q7_stats enabled, disabled;
} ksn_mesh_q7_preflight_report;
size_t ksn_mesh_q7_preflight_bytes(void);
size_t ksn_mesh_q7_preflight_alignment(void);
bool ksn_mesh_q7_preflight_run(void *workspace, size_t bytes,
                             ksn_mesh_q7_preflight_report *report);
/* Called after every compared row, including the final rejected-call test.
 * Return false to cancel. Callback sees the original caller floating
 * environment; any changes it makes are discarded before numeric work resumes.
 * It must return normally and must not access workspace/report or reenter this
 * run. Each interval has at most one two-face scene preparation and two
 * one-row renders. No callback means synchronous operation, with no yields. */
typedef bool (*ksn_mesh_q7_preflight_checkpoint_fn)(void *context);
bool ksn_mesh_q7_preflight_run_cooperative(void *workspace, size_t bytes,
    ksn_mesh_q7_preflight_report *report,
    ksn_mesh_q7_preflight_checkpoint_fn checkpoint, void *context);
/* Incremental variant: begin/reset does no rendering. Each step performs
 * exactly one row comparison, with at most one two-face scene preparation.
 * Keep workspace and report alive, unmodified and at the same addresses until
 * DONE/FAILED. Freeing the workspace cancels safely; no finalizer is required.
 * A step before begin or after DONE/FAILED returns FAILED. Begin may restart.
 * Each call preserves that call's caller fenv. Numeric fenv persists separately
 * across steps; round_before/after describe that numeric stream, while
 * environment_restored covers restoration of each call's current caller env. */
typedef enum {
    KSN_MESH_Q7_PREFLIGHT_MORE,
    KSN_MESH_Q7_PREFLIGHT_DONE,
    KSN_MESH_Q7_PREFLIGHT_FAILED
} ksn_mesh_q7_preflight_step_result;
bool ksn_mesh_q7_preflight_begin(void *workspace,size_t bytes,
                               ksn_mesh_q7_preflight_report *report);
ksn_mesh_q7_preflight_step_result ksn_mesh_q7_preflight_step(
    void *workspace,size_t bytes,ksn_mesh_q7_preflight_report *report);
#endif
