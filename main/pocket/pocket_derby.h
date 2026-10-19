#ifndef POCKET_DERBY_H
#define POCKET_DERBY_H
#include "quickjs.h"
#include "esp_err.h"
#include <stdint.h>

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
#if defined(POCKET_DERBY_COURSE_CACHE) || defined(POCKET_DERBY_PROJECT_BATCH)
typedef struct {
    uint32_t course_replacements,cache_bytes,registration_poses,view_poses;
    uint32_t project_calls,project_points,input_numbers,output_numbers;
} DerbyProjectionCounts;
/* Candidate-only owner-task counters. No device access and no timed observer. */
void pocket_derby_projection_counts(DerbyProjectionCounts *out);
#endif
#endif
