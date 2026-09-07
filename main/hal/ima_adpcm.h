#pragma once
#include <stdbool.h>
#include <stdint.h>

// IMA ADPCM as WAV lays it out, and nothing else: no file, no host, no driver.
// It lives in a header of its own so the audio task and tools/test_ima.py
// compile the same lines -- the decoder is the one part of clip playback whose
// mistakes are inaudible rather than obvious, and this board has no way to
// listen to itself (board_capture cannot see what the codec received).

static const int8_t IMA_INDEX[16]={-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
static const uint16_t IMA_STEP[89]={
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,
    80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,
    494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
    2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,
    8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767};

typedef struct {
    const uint8_t *data;
    uint32_t bytes, at;      // `at` is the next byte to read
    uint16_t block, left;    // block size, and samples left in this block
    int32_t predictor;
    int16_t index;
    bool low;                // the next nibble is the low half of data[at]
} ima_t;

// The next sample, or 0 once the payload runs out. Starting a block reseeds the
// predictor from the block header, which is what makes a clip resumable and
// seekable at block granularity without decoding what came before.
static int16_t ima_next(ima_t *s) {
    if(!s->left) {
        if(s->at+4>s->bytes) return 0;
        s->predictor=(int16_t)(s->data[s->at]|(s->data[s->at+1]<<8));
        s->index=s->data[s->at+2];
        if(s->index<0||s->index>88) s->index=0;
        s->at+=4; s->low=true;
        // The header's sample is the block's first output, so the nibbles that
        // follow are one fewer than the payload bytes suggest.
        s->left=(uint16_t)((s->block-4)*2);
        return (int16_t)s->predictor;
    }
    if(s->at>=s->bytes) { s->left=0; return 0; }
    unsigned nibble=s->low?(s->data[s->at]&0xf):(s->data[s->at]>>4);
    if(s->low) s->low=false; else { s->low=true; s->at++; }
    s->left--;
    int step=IMA_STEP[s->index];
    int diff=step>>3;
    if(nibble&1) diff+=step>>2;
    if(nibble&2) diff+=step>>1;
    if(nibble&4) diff+=step;
    s->predictor+=(nibble&8)?-diff:diff;
    if(s->predictor>32767) s->predictor=32767;
    if(s->predictor<-32768) s->predictor=-32768;
    s->index=(int16_t)(s->index+IMA_INDEX[nibble]);
    if(s->index<0) s->index=0;
    if(s->index>88) s->index=88;
    return (int16_t)s->predictor;
}
