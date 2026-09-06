#pragma once
#include <stdbool.h>
#include "keymap.h"

// The screen that gives wifi_time.c its credentials and starts a sync.
//
// wifi_time.c has no way in of its own: the SSID and the passphrase live in
// NVS and nothing was ever able to put them there. This is that screen, and it
// is the only caller of wifi_time_sync_start().
//
// The passphrase is write-only here as well as in wifi_time.c. It is typed
// into a buffer this module wipes when it leaves, drawn as one dot per byte,
// and never logged — not its bytes and not its length.

void wifi_ui_open(void);

// Feed one keystroke. Returns false when the screen wants to close.
bool wifi_ui_key(const keystroke_t *k);

bool wifi_ui_dirty(void);
void wifi_ui_draw(void);
