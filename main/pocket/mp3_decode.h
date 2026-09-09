#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "minimp3.h"

// No ESP dependencies: the host tests run the actual decoder and rate converter.
typedef struct {
    unsigned bytes, rate, channels, samples;
} pocket_mp3_header_t;
bool pocket_mp3_header(const uint8_t h[4], pocket_mp3_header_t *out);

typedef bool (*pocket_mp3_emit_t)(void *ctx, int16_t sample);
typedef struct {
    mp3dec_t decoder;
    int16_t *pcm;
    int16_t history[32], filter[32];
    unsigned rate, phase, cursor;
    int previous;
} pocket_mp3_decoder_t;
void pocket_mp3_init(pocket_mp3_decoder_t *d, int16_t *pcm);
// One complete MPEG Layer III frame. Free-format and changing rates are refused.
bool pocket_mp3_decode(pocket_mp3_decoder_t *d, const uint8_t *frame,
                      unsigned bytes, pocket_mp3_emit_t emit, void *ctx);
