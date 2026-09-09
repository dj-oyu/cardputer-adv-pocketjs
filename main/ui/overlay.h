#pragma once
#include <stdbool.h>
#include <stdint.h>

// The shell's side of docs/common-api.md 3.1: which overlay may run, whether it
// is allowed to start, and when it is stopped.
//
// The valves live here rather than in the overlay itself, which is the point of
// the section: an overlay that has gone wrong cannot be trusted to switch
// itself off, and the screen the person switches it off from is the one it is
// drawing on.

// Reads the arming setting and the crash flag and decides whether this boot may
// auto-start. Call after nvs_init() and shell_init(), before the UI task.
void overlay_init(void);

// The Settings row. get() is the stored arming bit, set() records it; nothing
// starts here -- the start happens on the UI task, in overlay_tick().
unsigned overlay_armed_get(void);
void     overlay_armed_set(unsigned armed);

// The two value names the Settings row shows. The second is a live buffer:
// 3.1 asks for the frame cost to be somewhere the person can see it and decide,
// and the row they would turn it off from is that place. It also carries the
// refusal ("NO ROOM"), the block ("BLOCKED") and the stop ("OVER BUDGET"), so a
// stop is never silent.
extern const char *const overlay_toggle_names[2];

// One home-screen frame. Starts the overlay if it is armed and not up, runs one
// guest turn, charges it against the frame budget, and stops it if it has been
// over budget too long. Only ever called with no foreground guest running.
void overlay_tick(void);

// Ends the session. Called before anything else takes the guest -- a foreground
// app, a diagnostic, another screen -- and idempotent.
void overlay_release(void);

// Stands the overlay down because something else needs the memory now. NOT a
// fault and not a budget stop: the overlay was well behaved and is getting out
// of the way, so the row says YIELDED and the person can turn it on again.
//
// Intended for the moment a large recurring claim is actually made -- the radio
// coming up, an audio stream starting -- rather than for a forecast taken when
// the overlay started. It has no call sites yet; see the definition for why.
// Idempotent.
void overlay_yield(const char *claimant);

// Composites the overlay's display list. Called from shell_draw() per strip,
// AFTER the scene and BEFORE the shell's own labels.
//
// That order was 3.1's old rule, and 3.1 no longer asks for it: as of
// 2026-09-09 an overlay is meant to end the menu and stand in its place, with
// only the shell's MODAL surfaces above it. What the menu was protecting was
// reachability, and a reserved key the shell never delegates gives that
// directly -- see overlay_yield(), which still has no caller. The code is
// behind the section; shell.c says the same at the call site.
void overlay_paint(uint16_t *strip, int strip_y, int strip_h);
