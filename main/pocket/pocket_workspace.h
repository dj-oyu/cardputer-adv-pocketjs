#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "quickjs.h"
#include "keymap.h"

// pocket.workspace — the works service of docs/common-api.md section 7, and the
// half of section 3 that decides what an app may reach.
//
// This is not a second filesystem and does not want to be one. A work is a
// srcstore slot: section 7 names srcstore as the initial backend and forbids
// the slot number reaching JS, so a SourceRef is a host object with the slot in
// its opaque and nothing an app can read. Revisions are srcstore's own sequence
// numbers, which is what keeps them right when the native Playground saves the
// same slot behind this surface's back. Only the titles are new, and they live
// in one small NVS blob rather than in a store of their own.
//
// pick() is a host screen. Section 3 asks for one — the person chooses the
// work, on the device, and that choice is the grant — so while the picker is up
// the guest is not ticked and the promise settles on the turn after it closes.
// main.c owns that diversion; everything else here is ordinary JS-task work.
//
// Everything runs on the JS owner task except nothing: there is no worker.

esp_err_t pocket_workspace_install(JSContext *ctx, void *user_data);

// Drops the session's picker and its picked-in-this-session grants. Call from
// app_stop() while the guest is alive, beside the other surfaces' resets. A
// launch that run() already accepted deliberately survives this: it is the
// request that ends the session, so it cannot be part of what the session's end
// throws away.
void pocket_workspace_reset(void);

// ------------------------------------------------------------- the host screen
//
// True while the picker owns the display. main.c hands it the keystrokes and
// the repaints for as long as it does, and ticks the guest again after.
bool pocket_workspace_modal(void);
void pocket_workspace_modal_key(const keystroke_t *key);
bool pocket_workspace_modal_dirty(void);
void pocket_workspace_modal_draw(void);

// ------------------------------------------------------------------ launching
//
// Section 7's run(): the call is validated synchronously, the current session
// ends, and the target takes the screen. These three are that handoff, in the
// order main.c calls them.

// True once run() has been accepted: the session should end.
bool pocket_workspace_run_requested(void);

// Takes the accepted launch and loads its source. `source` is borrowed until
// pocket_workspace_run_done(); false when nothing is pending or the source
// could not be read, in which case nothing is left pending either.
bool pocket_workspace_run_take(const char **source, size_t *length);

// Gives the source buffer back, after app_start_source() has copied what it
// needs out of it.
void pocket_workspace_run_done(void);
