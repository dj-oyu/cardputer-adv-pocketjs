#pragma once
#include <stdbool.h>
#include <stdint.h>

// The decisions of docs/common-api.md 3.1 that do not need a board.
//
// Everything here is a pure function of its arguments, the way
// pet/pet_hub_core.c and pocket/app_registry.c are, so tools/test_overlay.c
// settles the safety valves on the host. That matters more here than usual:
// the valve this file exists for is the one whose failure mode is a device
// that boot-loops with no way in, and a valve that can only be tested by
// flashing the thing it protects is not tested.

typedef enum {
    OVERLAY_OFF = 0,   // the person has not asked for one
    OVERLAY_BLOCKED,   // asked for, but the last start did not survive: 3.1's
                       // "起動中に落ちるオーバーレイを自動で再武装しない"
    OVERLAY_REFUSED,   // asked for, but the up-front reservation failed
    OVERLAY_STOPPED,   // was running; the shell stopped it
    OVERLAY_STARTING,  // the non-volatile flag is written and the guest is
                       // being built. A reset in this state is what BLOCKED
                       // reads on the next boot.
    OVERLAY_RUNNING,
} overlay_state_t;

// Where on the LCD an overlay may draw. x/y are screen pixels; an overlay's
// own coordinates are relative to the box and are never translated by the
// guest, so a program cannot even express a pixel outside it -- and asking for
// one is refused rather than clipped (3.1: clipping hides the mistake from the
// author).
typedef struct { int16_t x, y, w, h; } overlay_region_t;

// True when a w*h box at (x,y) in region-local coordinates lies wholly inside
// the region. Zero and negative extents are refused: a program that computed a
// width of -1 has a bug, and drawing nothing would hide it.
bool overlay_region_holds(const overlay_region_t *region,
                          int x, int y, int w, int h);

// What to do at boot, from the two bits that survive a reset.
//
// `armed` is the person's setting. `starting` is the flag written immediately
// before the last start and cleared only after the overlay ran healthily for a
// while -- so finding it still set means the last start did not get that far,
// and the only safe reading of that is "it took the device down". Re-arming is
// then a human act.
overlay_state_t overlay_boot_state(bool armed, bool starting);

// Whether the machine is still able to do what it could do before the overlay
// started. `free_after` is the internal heap free once the guest is up and
// `floor` is the largest recurring claim any other subsystem makes.
//
// This is the POSTCONDITION, and it is a different thing from the reservation
// that precedes it. A reservation is a forecast: it says a block existed at
// the instant it was asked for, and the guest allocates a microsecond later
// under a scene that is still drawing. This is a measurement of what actually
// happened, taken after the fact, and an overlay is uniquely able to act on it
// -- nothing was displaced to start it, so standing down puts the machine back
// exactly where it was. A foreground app cannot do that: by the time it is up,
// the screen it replaced is gone.
bool overlay_room_left(uint32_t free_after, uint32_t floor);

typedef struct {
    uint32_t budget_us;    // what one overlay turn may cost
    uint16_t over_limit;   // consecutive over-budget turns that stop it
    uint32_t healthy_us;   // how long a healthy run is before the flag clears
    // state
    uint16_t over_run;
    uint64_t started_us;
    uint32_t last_us, worst_us;
    uint32_t turns;
} overlay_budget_t;

void overlay_budget_start(overlay_budget_t *b, uint64_t now_us);

// Records one turn. Returns true when the shell must stop the overlay: 3.1
// wants a repeatedly over-budget overlay stopped, because a home screen that
// cannot be operated is a home screen the person cannot switch it off from.
// One slow turn is not a fault -- a garbage collection is one turn -- so it is
// a run of them.
// `frame_us` is how long the WHOLE home frame took, and passing it is what
// makes this a share rather than a stopwatch.
//
// It was a stopwatch, and on 2026-09-09 that stopped a working music player
// under the FLOWER scene. Both this and the guest's interrupt deadline measure
// WALL CLOCK, so a turn is charged for every microsecond the ui task spent
// preempted by the decoder and the card. FLOWER draws at 79 ms a frame; the
// overlay's own compositing was 3-5 ms of it, and the turn still crossed 12 ms
// sixty times in a row and was stopped for it.
//
// What the stop is FOR is the sentence in 3.1: an overlay must not make the
// home screen unusable, because the row that turns it off is on the home
// screen. Under FLOWER the home screen was already at 14 fps WITHOUT the
// overlay -- stopping it bought nothing, which is the test of whether the rule
// fired for its own reason. So a turn counts against the overlay only when the
// overlay is a real share of the frame; a slow turn on a machine where
// everything is slow is not evidence about the overlay.
//
// Pass 0 for frame_us when the frame length is not known, and the share test is
// skipped -- the old behaviour, for a caller that has no frame.
bool overlay_budget_turn(overlay_budget_t *b, uint32_t turn_us,
                         uint32_t frame_us);

// The share an over-budget turn must reach before it is charged, as a divisor:
// the turn must be more than a QUARTER of the frame. Chosen so that an overlay
// eating most of a frame is caught in a few turns while one riding along behind
// a heavy scene is not caught at all -- and it is a ratio rather than a second
// millisecond number so that it does not have to be re-tuned per scene.
#define OVERLAY_SHARE_DIVISOR 4

// True once the run has been healthy long enough to clear the starting flag.
bool overlay_budget_healthy(const overlay_budget_t *b, uint64_t now_us);
