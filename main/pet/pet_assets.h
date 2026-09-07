#pragma once
// pet_hub.c publishes this as maxSpeechChars, so say() and the limits table
// cannot drift apart.
#define PET_SPEECH_CHARS 22
#include "pocketjs/guest_quickjs.h"
#include "pocketjs/ui_core.h"

esp_err_t pet_assets_install(JSContext *ctx, void *core);
void pet_assets_reset(void);
void pet_assets_overlay(uint16_t *pixels, int y, int rows);
void pet_assets_tick(void);
extern const uint8_t pet_compact_start[] asm("_binary_pets_compact_bin_start");
