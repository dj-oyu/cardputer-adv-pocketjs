#ifndef POCKET_DERBY_H
#define POCKET_DERBY_H
#include "quickjs.h"
#include "esp_err.h"
#include <stdint.h>
#include "derby_ser_int.h"

/* pocket.derby: DERBY WATCH's panning series in C (docs/apps/derby-ser-native.md).
 * Not part of the common API: app_session.c installs it only into a session
 * whose manifest names derby.series, as it does pocket.pet. */
esp_err_t pocket_derby_install(JSContext *ctx, void *unused);
/* The namespace's functions (course, view, ser) on ns; what install's lazy
 * builder runs. The host harness calls it directly. */
esp_err_t pocket_derby_fill(JSContext *ctx, JSValueConst ns, void *unused);
/* Frees the course and forgets the view. Holds no guest values, so its place
 * in app_stop() only has to be before the next session. */
void pocket_derby_reset(void);
#ifdef DERBY_SER_CHECK
/* Host checks only: the integer series' counters since the process began
 * (derby_ser_int.h: saturations, skipped draws and the invariants, all of
 * which must stay 0). The firmware keeps none (no .bss). */
void pocket_derby_ser_stats(derby_ser_stats *out);
#endif
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(CONFIG_POCKET_DERBY_SER_PROBE) && !defined(POCKET_DERBY_SER_PROBE)
#define POCKET_DERBY_SER_PROBE 1
#endif
#ifdef POCKET_DERBY_SER_PROBE
/* Measurement builds only (CONFIG_POCKET_DERBY_SER_PROBE): logs the series'
 * path counts and cycles since the last call as one DERBY_SER line (nothing
 * when DERBY drew no series), cycles per panning frame (view() call), ticks
 * (the window's host turns) as given, and starts again. Also enabled by
 * defining POCKET_DERBY_SER_PROBE above this point (a measurement commit). */
void pocket_derby_ser_probe_log(unsigned ticks);
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
