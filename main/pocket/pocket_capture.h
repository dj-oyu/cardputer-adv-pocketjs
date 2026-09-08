#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"

// pocket.audio.capture -- the microphone half of docs/common-api.md section 9.
//
// A separate file from pocket_av.c, which owns the rest of the audio namespace,
// for two reasons. The substrate lets several contributors add to one namespace
// (pocket_api_lazy), so nothing is gained by sharing a file; and pocket_av.c
// links against pocket_fs.c for the player's source file, which is 2,800 lines
// a host test would have to drag in behind it. Everything here can be compiled
// and driven on a host against the real QuickJS -- tools/test_pocket_capture.c
// does exactly that.
//
// Section 9's prose, not its type block, is where the requirements are:
//   - recording is valid only during an authorised session (app_stop closes it)
//   - the host shows a recording indicator while it is live (the overlay below)
//   - read returns time-contiguous PCM
//   - on overflow, do not splice: fail with LIMIT_EXCEEDED and close
//   - recording and playback are exclusive, and cues do not sound while it runs
//
// Everything here runs on the JS owner task, apart from the overlay, which runs
// on the drawing task inside board_present().

esp_err_t pocket_capture_install(JSContext *ctx, void *user_data);

// Settles a read that has been waiting for the microphone to produce frames.
// Call before pocket_api_pump() so a read fills and resolves in one turn.
void pocket_capture_pump(void);

// Closes any open recorder. From app_stop(), before pocket_api_reset(): a
// recorder holds the I2S channel and the codec's ADC, and nothing else in the
// firmware would give them back.
void pocket_capture_reset(void);

// The recording indicator. Drawn from board_present(), which is the only path
// to the panel, so it lands over whatever the app drew and the app cannot paint
// it away. Section 9 makes showing it the host's obligation.
void pocket_capture_overlay(uint16_t *pixels, int y, int rows);

// True at most twice a second while a recording is open: an app that stops
// drawing must not be able to leave a screen with no indicator on it, and a
// strip is only overlaid when something presents it.
bool pocket_capture_take_dirty(void);
