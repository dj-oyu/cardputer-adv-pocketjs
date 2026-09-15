#pragma once
#include "board.h"
#include "quickjs.h"
void pet_hub_init(void);
bool pet_hub_usb(uint8_t c);
bool pet_hub_pump(void);
bool pet_hub_key(board_key_t key);
void pet_hub_overlay(uint16_t *pixels, int y, int rows);
void pet_hub_overlay_suppress(bool suppress);
uint16_t pet_hub_selected(void);
esp_err_t pet_hub_install(JSContext *ctx, void *unused);
