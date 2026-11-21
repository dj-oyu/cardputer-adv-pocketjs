#pragma once
/* The one header of main/apps/ that system code includes (CLAUDE.md: native
 * code that only one app uses lives in main/apps/<app>/ and stays out of the
 * component's INCLUDE_DIRS). It declares only the entry points the system
 * calls -- installing a namespace, resetting it at app_stop(), a view
 * provider -- so that clangd, which offers every symbol of every header an
 * indexed file includes, does not offer an app's internals in system code.
 * Each app's own headers (apps/derby/pocket_derby.h, ...) are for its files and
 * its host checks; .clangd keeps main/apps/ out of the background index. */
#include "esp_err.h"
#include "quickjs.h"
#include "app_view_provider.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* DERBY WATCH: pocket.derby, the panning series in C
 * (apps/derby/pocket_derby.c, docs/apps/derby-ser-native.md). Not part of the
 * common API: app_session.c installs it only into a session whose manifest
 * names derby.series, as it does pocket.pet. */
esp_err_t pocket_derby_install(JSContext *ctx, void *unused);
/* Frees the course and forgets the view. Holds no guest values, so its place
 * in app_stop() only has to be before the next session. */
void pocket_derby_reset(void);
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

/* MUSIC PLAYER: its native view (apps/player/app_music_view.c), listed by
 * app_view_provider.c. */
extern const pocket_app_view_provider pocket_app_music_view;
#ifdef KASANE_P0_PROBE
void pocket_app_music_view_probe_reset_counters(void);
#endif
