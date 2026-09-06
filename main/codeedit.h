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

// The source the run should evaluate.
const char *code_source(size_t *len);

// Called when the guest stops, with the reason to show (NULL on a clean exit).
void code_returned(const char *error);
