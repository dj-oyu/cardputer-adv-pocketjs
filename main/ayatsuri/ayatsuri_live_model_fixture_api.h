#ifndef AYATSURI_LIVE_MODEL_FIXTURE_API_H
#define AYATSURI_LIVE_MODEL_FIXTURE_API_H

#include <stdbool.h>
#include <stdint.h>

#if defined(AYATSURI_TRACK_MODEL)
#include "companion_motion_config.h"
#elif __has_include("private/assets/live/companion_motion_config.h")
#include "private/assets/live/companion_motion_config.h"
#endif
#ifdef AY_COMPANION_MOTION_POSES
#define AY_LIVE_MODEL_POSE_COUNT AY_COMPANION_MOTION_POSES
#else
#define AY_LIVE_MODEL_POSE_COUNT 63u
#endif
#if defined(AYATSURI_TRACK_MODEL) && !defined(AY_LIVE_MODEL_SHA256)
#error "Track export must identify its model in companion_motion_config.h"
#endif
#ifndef AY_LIVE_MODEL_SHA256
#define AY_LIVE_MODEL_SHA256 "384ada0c37da16d625b1e6612a0d2f7ca538c29fe0d8dff43a929841ec63fba8"
#endif

/* A successful prepare pins one generated frame until a confirmed drain.
 * Nothing is submitted to the board here. Failed prepare acquires no new pin. */
bool ay_live_model_prepare(unsigned pose, unsigned *packets, bool *fast, uint32_t *geometry_us);
bool ay_live_model_row(unsigned y, uint16_t pixels[240]);
/* Aligned 8-row strip (7 rows at the bottom), RGB565 and half-open model x extent. */
bool ay_live_model_band(unsigned y, uint16_t *pixels, unsigned *left, unsigned *right);
/* Select the raster policy before prepare, after the preceding frame drains. */
void ay_live_model_serial_raster(bool serial);
uint32_t ay_live_model_band_wait_us(void);
/* A successful drain also accepts an idle frame. An uncertain drain retains it. */
bool ay_live_model_finish(bool drained);
/* Releases the runtime while idle. A pinned frame prevents closure. */
bool ay_live_model_close(void);

#endif
