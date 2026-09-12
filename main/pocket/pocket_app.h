#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "quickjs.h"

// pocket.app / pocket.time / pocket.log — section 5 of docs/common-api.md plus
// the "ログと診断" part of section 7, on top of pocket_api.c.
//
// Everything here runs on the JS owner task. Install this surface AFTER the
// console one: it wraps print and console.log/warn/error so that section 7's
// "console.log ... も同じ経路へ接続する" holds, and it can only wrap what is
// already there.

esp_err_t pocket_app_install(JSContext *ctx, void *user_data);

// One turn's host work, before the frame. It posts the sleeps whose deadline
// has passed -- so call it BEFORE pocket_api_pump(), which is what turns those
// posts into settled Promises in the same turn -- and it is also where the
// Starting -> Running transition of section 5 happens. Cheap when a program
// uses none of this: two loads and two branches.
void pocket_app_pump(void);

// Runs the stop hook, then drops every callback the realm gave us. Call from
// the JS task while the guest is still alive, FIRST among app_stop()'s resets:
// section 5 puts the stop hook ahead of I/O cancellation and unsubscription,
// and a hook that wants to save something needs the surfaces it is about to
// lose. Bounded by POCKET_APP_STOP_MS whatever the hook does.
// Whether the guest called pocket.app.exit(). Hoisted out of
// pocket_app_pump() so a continuation turn honours it too; see the definition.
bool pocket_app_exit_requested(void);
void pocket_app_reset(void);
