#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "minimp3.h"
#include "fir_pie.h"

// No ESP dependencies: the host tests run the actual decoder and rate converter.
typedef struct {
    unsigned bytes, rate, channels, samples;
} pocket_mp3_header_t;
bool pocket_mp3_header(const uint8_t h[4], pocket_mp3_header_t *out);

// The total number of MPEG frames in the file, from the Xing/Info or VBRI tag
// that encoders put in the FIRST frame. `first` is that frame's bytes and
// `len` is how many of them are available (64 is enough for both tags).
// Returns 0 when neither tag is there.
//
// THIS IS THE ONLY CHEAP WAY TO KNOW A SONG'S LENGTH, and knowing is not
// optional-but-nice: without it the position a player shows is a number with no
// scale, and a progress bar drawn from it would be inventing its own end.
//
// The alternative is counting frames, which means reading the whole file --
// megabytes off a card, inside the turn that was asked to open it. A CBR
// estimate from bitrate x size is the other alternative and is a GUESS: it is
// exactly right for constant bitrate and quietly wrong for variable, with
// nothing in the file to say which one you have. So: report the count when the
// file states it, and report nothing when it does not. A player can draw
// something honest for "unknown"; it cannot un-draw a bar that lied.
uint32_t pocket_mp3_total_frames(const uint8_t *first, unsigned len,
                                 const pocket_mp3_header_t *header);

typedef bool (*pocket_mp3_emit_t)(void *ctx, int16_t sample);
typedef struct {
    mp3dec_t decoder;
    int16_t *pcm;
    // FIR_RING_SPAN (80), not 32: the ring holds FIR_TAPS + FIR_LANES slots (the block
    // of eight outputs must not overwrite any sample it reads) and is stored twice so a
    // window is contiguous (fir_pie.h). The 16-byte alignment is the kernel's
    // requirement -- the window it is handed must land on a 16-byte boundary, and
    // 2*(FIR_RING_SLOTS + block_start) with block_start % 8 == 0 only gets there if the
    // ring itself starts aligned.
    int16_t history[FIR_RING_SPAN] __attribute__((aligned(16))), filter[32];
    unsigned rate, phase, cursor;
    int previous;
} pocket_mp3_decoder_t;
// The A/B arm for the block kernel: 0 keeps the scalar per-sample filter that the
// rate converter's rounding was tuned against, 1 uses fir8_pie. Runtime, so one
// binary can be measured both ways.
extern int g_mp3_fir_pie;
void pocket_mp3_init(pocket_mp3_decoder_t *d, int16_t *pcm);
// One complete MPEG Layer III frame. Free-format and changing rates are refused.
bool pocket_mp3_decode(pocket_mp3_decoder_t *d, const uint8_t *frame,
                      unsigned bytes, pocket_mp3_emit_t emit, void *ctx);
