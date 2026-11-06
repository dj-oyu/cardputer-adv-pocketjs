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
    uint64_t render_failures;
    int round_before, round_after;
    bool subnormal_ok, environment_restored;
    ksn_mesh_q7_stats enabled, disabled;
} ksn_mesh_q7_preflight_report;
size_t ksn_mesh_q7_preflight_bytes(void);
size_t ksn_mesh_q7_preflight_alignment(void);
bool ksn_mesh_q7_preflight_run(void *workspace, size_t bytes,
                             ksn_mesh_q7_preflight_report *report);
#endif
