#ifndef AYATSURI_LIVE_MODEL_FIXTURE_API_H
#define AYATSURI_LIVE_MODEL_FIXTURE_API_H

#include <stdbool.h>
#include <stdint.h>

#if __has_include("private/assets/live/companion_motion_config.h")
#include "private/assets/live/companion_motion_config.h"
#define AY_LIVE_MODEL_POSE_COUNT AY_COMPANION_MOTION_POSES
#define AY_COMPANION_RESUME_DEMO (-2)
#else
#define AY_LIVE_MODEL_POSE_COUNT 63u
#endif
#define AY_LIVE_MODEL_SHA256 "384ada0c37da16d625b1e6612a0d2f7ca538c29fe0d8dff43a929841ec63fba8"

/* A successful prepare pins one generated frame until a confirmed drain.
 * Nothing is submitted to the board here. Failed prepare acquires no new pin. */
bool ay_live_model_prepare(unsigned pose, unsigned *packets, bool *fast, uint32_t *geometry_us);
bool ay_live_model_row(unsigned y, uint16_t pixels[240]);
/* Aligned 8-row strip (7 rows at the bottom), RGB565 and half-open model x extent. */
bool ay_live_model_band(unsigned y, uint16_t *pixels, unsigned *left, unsigned *right);
/* A successful drain also accepts an idle frame. An uncertain drain retains it. */
bool ay_live_model_finish(bool drained);
/* Releases the runtime while idle. A pinned frame prevents closure. */
bool ay_live_model_close(void);

#endif
