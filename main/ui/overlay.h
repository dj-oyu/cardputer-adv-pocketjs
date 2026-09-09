#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "keymap.h"

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

// The Settings row. get() is the stored choice, set() records it; nothing
// starts here -- the start happens on the UI task, in overlay_tick(). 0 is off
// and 1..OVERLAY_APPS names one of the registered overlays.
unsigned overlay_armed_get(void);
void     overlay_armed_set(unsigned choice);

// How many overlays this build registers, and the value names the Settings row
// shows: "OFF" and then one per overlay.
//
// Each name is a LIVE BUFFER. 3.1 asks for the frame cost to be somewhere the
// person can see it and decide, and the row they would turn it off from is that
// place. The same buffer carries the refusal ("NO ROOM"), the block
// ("BLOCKED"), the stop ("OVER BUDGET") and the stand-down ("YIELDED"), so none
// of those is ever silent. Only the SELECTED overlay's name moves; the others
// keep their plain title, because a status on a row that is not running would
// be describing nothing.
#define OVERLAY_APPS 2
extern const char *const overlay_choice_names[1+OVERLAY_APPS];

// Whether an overlay session is up and drawing. 3.1: an overlay ENDS the menu
// and stands in its place, so this is what shell_draw() asks before laying the
// menu out at all -- not a question about where to draw, but about whether XMB
// exists this frame.
bool overlay_running(void);

// One keystroke for the running overlay, from the home screen's loop.
//
// The shell has already taken its reserved key before this is called, and that
// is the whole of the input rule: what arrives here is the residue. 3.1's older
// text declined to wire this because the residue was empty -- true of a shell
// running XMB, and no longer true of one that has ended it.
void overlay_key(const keystroke_t *k);

// One home-screen frame. Starts the overlay if it is armed and not up, runs one
// guest turn, charges it against the frame budget, and stops it if it has been
// over budget too long. Only ever called with no foreground guest running.
//
// `frame_us` is how long the PREVIOUS whole frame took. The charge is a share
// of it rather than a stopwatch reading, because both this measurement and the
// guest's interrupt deadline are wall clock and a busy machine would otherwise
// be charged to the overlay -- see overlay_core.h, and the FLOWER scene that
// proved it by stopping a working player.
void overlay_tick(uint32_t frame_us);

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
