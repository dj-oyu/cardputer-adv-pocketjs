#pragma once
#include "esp_err.h"
#include "quickjs.h"

// pocket.audio and pocket.power — sections 9 and 8 of docs/common-api.md, on
// top of sound.c and board.c's battery ADC.
//
// The two surfaces share a file because they share the one thing that is new
// here: a per-frame pump that both a Promise waiting on the audio task and a
// battery subscription are driven from. Splitting them would duplicate that
// pump and its reset for the sake of a namespace boundary neither side needs.
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
// the tone's `done` on the audio task at priority 7. That callback touches two
// atomics and nothing else; pocket_av_pump() is what turns them into a Promise
// resolution on the JS task, as section 5 requires.

esp_err_t pocket_av_install(JSContext *ctx, void *user_data);

// Settles a finished tone and delivers battery changes. Call once per frame
// from the JS task, next to pocket_imu_pump(). Cheap when nothing is waiting:
// it returns after two loads of static state.
void pocket_av_pump(void);

// Drops the pending tone and every power subscription. Call from the JS task
// while the guest is still alive -- app_stop() before it destroys the guest --
// so the callbacks and the Promise's resolvers are released. A tone that is
// still sounding is asked to stop and is left to the audio task, which holds no
// JS value of its own; the guest does not wait for it.
void pocket_av_reset(void);
