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
//   audio.capture.open              NOT here -- pocket_capture.c contributes
//                                   it to the same namespace
//   audio.player.open(spec)         Promise<Player> — one clip, in RAM, in the
//                                   host's 24 kHz mono, as PCM16 or IMA ADPCM
//                                   inside a WAV. Section 9.1's MP3/Opus/FLAC
//                                   off sd: is not reachable on this board and
//                                   the section carries the measurements.
//   power.status()                  battery millivolts; percent and charging
//                                   are null because the board cannot read them
//   power.onChange(fn)              Subscription, fired when the reading moves
//   power.keepAwake(options)        present and throws UNSUPPORTED
//
// Everything here runs on the JS owner task except two callbacks: sound.c calls
// the tone's and the clip's `done` on the audio task at priority 7. Each posts
// one completion and does nothing else; pocket_api_pump() is what turns it into a
// Promise resolution on the JS task, as section 5 requires.

esp_err_t pocket_av_install(JSContext *ctx, void *user_data);

// Delivers battery changes. Call once per frame from the JS task, next to
// pocket_imu_pump() and pocket_api_pump(); the last of those is what settles a
// finished tone, and this one is what turns a finished clip into onState.
// Cheap when nothing is subscribed: it returns after one load.
void pocket_av_pump(void);

// Drops every power and player subscription, so its callbacks are released,
// and stops and frees a clip the audio task may still be reading. Call from the
// JS task while the guest is still alive -- app_stop() before it destroys the
// guest. A tone still sounding is pocket_api_reset()'s to end, which app_stop()
// calls right after this one.
void pocket_av_reset(void);
