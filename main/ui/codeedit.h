#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "keymap.h"

// The JavaScript Playground: a text editor over one source, and the run that
// hands it to the guest.
typedef enum {
    CODE_EDIT = 0,   // typing
    CODE_RUNNING,    // the guest owns the screen
} code_state_t;

// Opens the person's own program, in the slot nothing else writes to.
void code_open(void);

// Opens a lesson's working copy instead, seeded with `template` the first time
// that slot is used. The person's own program is left untouched: a worked
// example must never cost someone what they were keeping.
void code_open_lesson(unsigned slot, const char *seed, size_t seed_len);

code_state_t code_state(void);

// Feed one keystroke. Returns false when the Playground wants to close.
bool code_key(const keystroke_t *k);

bool code_dirty(void);
void code_draw(void);

// Forget what the panel is believed to be showing, so the next code_draw()
// sends every strip. Required before a capture -- board_capture prints only
// the strips that are sent -- and whenever something outside this screen draws
// over it: the pet overlay rides on board_present, so a frame that presents
// three strips moves the pet on three strips only.
void code_repaint_all(void);

// The source the run should evaluate.
const char *code_source(size_t *len);

// Called when the guest stops, with the reason to show (NULL on a clean exit).
void code_returned(const char *error);

// Leaving the screen for good. The 8 KB source buffer goes back to the heap;
// the next open reads the slot out of flash again.
void code_close(void);

// The same buffer, given back for the length of a run and taken again at the
// end of it. Release must not be called until the guest has finished parsing:
// app_start_source() reads straight out of the caller's bytes.
void code_run_release(void);
void code_run_restore(void);
