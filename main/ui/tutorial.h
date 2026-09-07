#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "keymap.h"

// The tutorial: a chapter screen that explains, and the Playground that the
// chapter hands its example to. They alternate rather than share the display —
// the Playground already uses all 135 px.
typedef enum {
    TUTORIAL_READING = 0,   // the chapter screen owns the display
    TUTORIAL_WRITING,       // the Playground does
} tutorial_state_t;

void tutorial_open(void);
tutorial_state_t tutorial_state(void);

// The source to run: the chapter's hidden prelude followed by what the learner
// wrote. Valid until the next call.
const char *tutorial_source(size_t *len);

// Told what the run did, so the chapter can decide whether it was reached.
// `started` is what app_start_source returned; `error` is app_error().
void tutorial_ran(esp_err_t started, const char *error);

// Feed one keystroke to whichever screen is up. Returns false to leave.
bool tutorial_key(const keystroke_t *k);

bool tutorial_dirty(void);
void tutorial_draw(void);
