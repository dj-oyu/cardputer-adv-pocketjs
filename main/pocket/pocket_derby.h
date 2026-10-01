#ifndef POCKET_DERBY_H
#define POCKET_DERBY_H
#include "quickjs.h"
#include "esp_err.h"

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
#endif
