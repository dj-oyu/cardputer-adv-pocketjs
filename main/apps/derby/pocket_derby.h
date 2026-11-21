#ifndef POCKET_DERBY_H
#define POCKET_DERBY_H
#include "quickjs.h"
#include "esp_err.h"
#include <stdint.h>
#include "derby_ser_int.h"
/* pocket_derby_install / _reset / _ser_probe_log, and POCKET_DERBY_SER_PROBE:
 * the entry points the system calls are declared there (CLAUDE.md). */
#include "apps/apps.h"

/* pocket.derby: DERBY WATCH's panning series in C (docs/apps/derby-ser-native.md). */
/* The namespace's functions (course, view, ser) on ns; what install's lazy
 * builder runs. The host harness calls it directly. */
esp_err_t pocket_derby_fill(JSContext *ctx, JSValueConst ns, void *unused);
#ifdef DERBY_SER_CHECK
/* Host checks only: the integer series' counters since the process began
 * (derby_ser_int.h: saturations, skipped draws and the invariants, all of
 * which must stay 0). The firmware keeps none (no .bss). */
void pocket_derby_ser_stats(derby_ser_stats *out);
#endif
#if defined(POCKET_DERBY_COURSE_CACHE) || defined(POCKET_DERBY_PROJECT_BATCH)
typedef struct {
    uint32_t course_replacements,cache_bytes,registration_poses,view_poses;
    uint32_t project_calls,project_points,input_numbers,output_numbers;
} DerbyProjectionCounts;
/* Candidate-only owner-task counters. No device access and no timed observer. */
void pocket_derby_projection_counts(DerbyProjectionCounts *out);
#endif
#endif
