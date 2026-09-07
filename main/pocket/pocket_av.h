#pragma once
#include "esp_err.h"
#include "quickjs.h"

// pocket.audio and pocket.power — sections 9 and 8 of docs/common-api.md, on
// top of sound.c and board.c's battery ADC.
//
// The two surfaces share a file because they were written together: the tone
// was the first Promise on this host to wait on a driver task, and the battery
// the second subscription to need a per-frame poll. Both of those now belong to
// pocket_api.c -- the tone waits on a promise slot, the subscriptions live in a
// pocket_sub_table_t -- and what is left here is the audio and the ADC.
//
// What is published:
//   audio.cue(name)                 sound_play, synchronous, returns bool
//   audio.tone(spec, options)       sound_tone, Promise<void>
//   audio.capture.open / audio.player.open   present and reject UNSUPPORTED
//   power.status()                  battery millivolts; percent and charging
//                                   are null because the board cannot read them
//   power.onChange(fn)              Subscription, fired when the reading moves
//   power.keepAwake(options)        present and throws UNSUPPORTED
//
// Everything here runs on the JS owner task except one callback: sound.c calls
// the tone's `done` on the audio task at priority 7. That callback posts one
// completion and does nothing else; pocket_api_pump() is what turns it into a
// Promise resolution on the JS task, as section 5 requires.

esp_err_t pocket_av_install(JSContext *ctx, void *user_data);

// Delivers battery changes. Call once per frame from the JS task, next to
// pocket_imu_pump() and pocket_api_pump(); the last of those is what settles a
// finished tone. Cheap when nothing is subscribed: it returns after one load.
void pocket_av_pump(void);

// Drops every power subscription, so its callbacks are released. Call from the
// JS task while the guest is still alive -- app_stop() before it destroys the
// guest. A tone still sounding is pocket_api_reset()'s to end, which app_stop()
// calls right after this one.
void pocket_av_reset(void);
