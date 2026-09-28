#ifndef KSN_PROC_GRID_PIE_H
#define KSN_PROC_GRID_PIE_H

#include "ksn_proc_grid.h"

#define KSN_GRID_PIE_BACKEND_REV 1u

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if defined(__XTENSA__) && defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
#define KSN_GRID_HAS_PIE 1
#else
#define KSN_GRID_HAS_PIE 0
#endif

typedef struct {
    bool enable_pie;
    uint16_t min_outputs; /* at least one constant operand; zero disables */
    uint16_t min_loaded_weight_outputs; /* two loads; zero disables */
    uint16_t min_scan_rows; /* independent rows with intra-row dependence */
} ksn_grid_pie_policy;

/* Static instruction-template cost only; excludes C address work, memory
 * latency, tails and preemption. Per-block guards can still choose gather. */
typedef struct {
    ksn_grid_access_kind input, coefficient;
    bool pair_candidate, pair_selected, fused_selected;
    uint8_t candidate_mask;
    ksn_grid_pie_load_strategy selected;
    ksn_grid_selection_reason reason;
    uint8_t tap_issues, tap_stalls;
    uint8_t pair_issues, pair_stalls, split_issues, split_stalls;
} ksn_grid_pie_access_info;

bool ksn_grid_pie_backend_available(void);
bool ksn_grid_pie_describe_access(const ksn_grid_execution *execution,
                                  ksn_grid_pie_access_info *info);
uint8_t ksn_grid_pie_candidates(const ksn_grid_execution *execution);
bool ksn_grid_pie_select(ksn_grid_execution *execution);
uint64_t ksn_grid_pie_profile_key(const ksn_grid_execution *execution);
#ifdef KSN_GRID_PIE_MODEL
extern uint32_t ksn_grid_pie_direct_taps, ksn_grid_pie_stride2_taps;
extern uint32_t ksn_grid_pie_unzip_pairs;
extern uint32_t ksn_grid_pie_fused_pairs;
extern uint32_t ksn_grid_pie_gather_taps;
extern uint32_t ksn_grid_pie_broadcast_taps;
typedef enum {
    KSN_GRID_TRACE_START_FIXED, KSN_GRID_TRACE_START_DYNAMIC,
    KSN_GRID_TRACE_TAP, KSN_GRID_TRACE_PAIR, KSN_GRID_TRACE_FUSED_PAIR,
    KSN_GRID_TRACE_STRIDE2,
    KSN_GRID_TRACE_BROADCAST_INPUT, KSN_GRID_TRACE_BROADCAST_COEFFICIENT,
    KSN_GRID_TRACE_DYNAMIC, KSN_GRID_TRACE_SCAN, KSN_GRID_TRACE_FINISH
} ksn_grid_trace_kind;
typedef void (*ksn_grid_trace_fn)(ksn_grid_trace_kind kind,
                                   const int16_t *a, const int16_t *b,
                                   int32_t argument);
extern ksn_grid_trace_fn g_ksn_grid_pie_trace;
#endif
/* True only for a proved, aligned load * constant + accumulator lowering. */
bool ksn_grid_pie_eligible(const ksn_grid_execution *execution);
bool ksn_grid_scan_pie_eligible(const ksn_grid_execution *execution);
ksn_grid_status ksn_grid_run_pie(ksn_grid_execution *execution);
ksn_grid_status ksn_grid_run_scan_pie(ksn_grid_execution *execution);
ksn_grid_status ksn_grid_run_auto(ksn_grid_execution *execution,
                                  const ksn_grid_pie_policy *policy);

#endif
