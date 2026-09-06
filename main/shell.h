#pragma once
#include "board.h"
void shell_init(void);
bool shell_key(board_key_t key);
unsigned shell_app(void);   // which Apps entry Enter would launch
void shell_draw(const char *error, unsigned phase);
void shell_change_background(int direction);
