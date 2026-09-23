#pragma once
#include "esp_err.h"
#include "quickjs.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    POCKET_AV_UI_READY, POCKET_AV_UI_PLAYING, POCKET_AV_UI_PAUSED,
    POCKET_AV_UI_ENDED, POCKET_AV_UI_ERROR
} pocket_av_ui_state;
typedef struct {
    pocket_av_ui_state state;
    uint32_t position_ms,duration_ms,underruns;
} pocket_av_ui_snapshot;

/* UI-owner-task-only, read-only projection of the current player. The ID is
 * never reused while this task runs; a closed/replaced source cannot revive a
 * view binding. No JS value, callback, or audio buffer is retained. */
int32_t pocket_av_ui_current_player(void);
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out);

// pocket.audio and pocket.power — sections 9 and 8 of docs/api/common-api.md, on
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
//   audio.player.open(spec)         Promise<Player> — one source decoded to
//                                   24 kHz mono, including WAV, MP3 and Opus
//                                   from granted storage.
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

// Native-only audio refill/priming/completion. Call on the UI owner task before
// any modal, guest-continuation or presentation gate can skip a guest turn.
void pocket_av_service_stream(void);

// Drops every power and player subscription, so its callbacks are released,
// and stops and frees a clip the audio task may still be reading. Call from the
// JS task while the guest is still alive -- app_stop() before it destroys the
// guest. A tone still sounding is pocket_api_reset()'s to end, which app_stop()
// calls right after this one.
void pocket_av_reset(void);
