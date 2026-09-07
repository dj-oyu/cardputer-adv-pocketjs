#pragma once
#include "board.h"
#include <stdbool.h>
#include <stdint.h>

// One translated keystroke. `nav` is the shell's navigation key (KEY_NONE when
// the stroke is text), `text`/`len` is what ime_feed() and the editor consume:
// either UTF-8 bytes, or a "\0name" token whose leading NUL is load-bearing —
// which is why `len` travels with it and strlen() must not be used.
typedef struct {
    board_key_t nav;
    char        text[8];
    uint8_t     len;
    bool        toggle_ime;   // Ctrl+J or opt+Space
    bool        force_stop;   // Ctrl+Alt+Del
} keystroke_t;

// Drain one hardware event. Returns false when nothing was produced — a
// release, or a modifier press, still updates the held state and returns false.
bool keymap_poll(keystroke_t *out);

// Modifier state, for callers that want to show it.
bool keymap_shift(void);
bool keymap_fn(void);
