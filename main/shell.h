#pragma once
#include "board.h"

// A settings entry can be an action: selecting it hands the display to another
// screen instead of opening a value list. shell_key() cannot say so — its bool
// already means "launch the app shell_app() names" — so the request is left
// here for the caller to collect after the key is handled.
typedef enum {
    SHELL_SCREEN_NONE = 0,
    SHELL_SCREEN_WIFI_TIME,   // no entry asks for this yet; the shape is ready
} shell_screen_t;

void shell_init(void);
bool shell_key(board_key_t key);
unsigned shell_app(void);   // which Apps entry Enter would launch
// The screen the last key press asked for, and clears it. Poll once per key,
// after shell_key(); SHELL_SCREEN_NONE means stay on the home screen.
shell_screen_t shell_pending_screen(void);
void shell_draw(const char *error, unsigned phase);
void shell_change_background(int direction);
