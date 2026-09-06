#pragma once
#include <stdbool.h>
#include <stddef.h>

// The tutorial's content, kept apart from the screen that shows it.
//
// Wording and chapter order come from a teaching pass where one agent held the
// implementation and another played a beginner who was not allowed to read it;
// every phrase the beginner could not follow was rewritten. The result is the
// data below, which the screen renders verbatim — `body` lines are already
// broken to fit 18 full-width characters, so nothing wraps them at runtime.

#define LESSON_BODY_ROWS 5

// How a chapter decides the learner got there.
typedef enum {
    // The run defined no frame (an expression, not an app) and a console line
    // matches `want` exactly.
    CHECK_PRINTS = 0,
    // First an exception containing `want`, then a clean run printing `want2`.
    CHECK_ERROR_THEN_PRINTS,
    // Two console lines, in any order.
    CHECK_PRINTS_BOTH,
    // The run started and stayed clean, and the source no longer contains
    // `want` — the value that was in the template.
    CHECK_CHANGED,
    // The run started and stayed clean, and a console line starts with `want`.
    CHECK_PRINTS_PREFIX,
    // The run started and stayed clean, the source contains `want`, and it
    // holds at least one byte above U+007F.
    CHECK_JAPANESE,
} lesson_check_t;

typedef struct {
    const char *title;
    const char *body[LESSON_BODY_ROWS];
    const char *hint;
    const char *code;       // seeded into the chapter's slot when it is empty
    bool        preload;    // false: the learner types it from what they have
    const char *prelude;    // run before `code`, never shown; NULL for none
    lesson_check_t check;
    const char *want;
    const char *want2;
} lesson_t;

unsigned lesson_count(void);
const lesson_t *lesson_at(unsigned index);

// The reference page the chapter screen opens with Tab. Same 18-character
// budget as `body`.
#define LESSON_REF_ROWS 7
const char *lesson_reference(unsigned row);
