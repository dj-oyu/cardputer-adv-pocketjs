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
#ifdef KASANE_P0_PROBE
    uint32_t    queued_at_us; // input-task observation, not physical key-down
#endif
} keystroke_t;

// Drain one hardware event. Returns false when nothing was produced — a
// release, or a modifier press, still updates the held state and returns false.
bool keymap_poll(keystroke_t *out);

// Modifier state, for callers that want to show it.
bool keymap_shift(void);
bool keymap_fn(void);

// Physical key names for pocket.input.keys (docs/api/common-api.md section 6),
// indexed row*14+col as in keystate.h. keymap_key_name writes the canonical
// name ("e", ";", "enter", "shift"...) into buf and returns false outside the
// matrix; keymap_key_index takes a canonical name or an alias (up/down/left/
// right/esc/back), ASCII case-insensitively, and returns -1 for anything else.
bool keymap_key_name(int index, char buf[8]);
int keymap_key_index(const char *name);

#ifdef POCKET_KEYTEST
// Diagnostic builds: one USB byte of a key-injection frame (keymap.c). True
// when the byte was taken.
bool keymap_inject_usb(uint8_t c);
#endif
