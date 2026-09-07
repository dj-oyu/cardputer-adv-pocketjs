#pragma once
#include <stdbool.h>
#include "keymap.h"

// The SKK practice screen: a target phrase, a text buffer the user types into,
// and the IME's preedit and candidates drawn underneath.
void editor_open(void);

// Feed one keystroke. Returns false when the editor wants to close (Esc with
// nothing being composed).
bool editor_key(const keystroke_t *k);

// True when something changed since the last editor_draw(). The screen is
// static between keystrokes, so repainting it at 30 Hz would spend two thirds
// of the CPU redrawing identical pixels.
bool editor_dirty(void);
void editor_draw(void);
