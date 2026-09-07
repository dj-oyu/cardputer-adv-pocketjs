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

// The source to run: what the learner wrote, which is the editor's own buffer.
// Valid until the run releases it.
const char *tutorial_source(size_t *len);

// The eight lines the chapter needs before the learner's own, or NULL. They are
// evaluated separately and in the same realm rather than pasted onto the front
// of the source, which is what keeps an error in a one-line lesson reported as
// line 1 instead of line 9.
const char *tutorial_prelude(size_t *len);

// Leaving the screen, and the two ends of a run. Both carry the editor's
// buffer with them: while a lesson is being written the editor is this screen.
void tutorial_close(void);
void tutorial_run_release(void);
void tutorial_run_restore(void);

// Told what the run did, so the chapter can decide whether it was reached.
// `started` is what app_start_source returned; `error` is app_error().
void tutorial_ran(esp_err_t started, const char *error);

// Feed one keystroke to whichever screen is up. Returns false to leave.
bool tutorial_key(const keystroke_t *k);

bool tutorial_dirty(void);
void tutorial_draw(void);
