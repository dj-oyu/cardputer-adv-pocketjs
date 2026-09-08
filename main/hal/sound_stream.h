#pragma once
#include "ima_adpcm.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The ring between a producer that reads a source and the audio task that plays
// it, and the walk that turns slots back into samples.
//
// It lives in a header of its own for the same reason ima_adpcm.h does: this is
// the part of streamed playback whose mistakes are inaudible rather than
// obvious. A slot boundary in the wrong place produces a click or a burst of
// noise, not a crash, and this board cannot hear itself -- board_capture sees
// the framebuffer, not what reached the ES8311. tools/test_stream.py compiles
// these exact lines on a host and checks that a source cut into slots decodes
// to the same samples as the same source decoded whole.
//
// No ESP dependency, so keep it that way: stdatomic and the decoder, nothing
// else.

// The ring, and why it is these two numbers.
//
// The producer is the JS/ui task's pump, which runs once a frame -- 33 ms of
// budget that has been measured at 39.9 ms. The consumer is the audio task,
// which drains a slot in SOUND_STREAM_SLOT_BYTES/48000 s of PCM16. Three slots
// of 2,048 bytes is 6,144 bytes: 128 ms of PCM16 or 512 ms of ADPCM in flight,
// of which about 85 ms is margin over the slot being drained. That is two of
// the slowest frames this firmware has been measured to draw.
//
// A slot holds whole ADPCM blocks, which is what makes the decoder resumable
// across slots without carrying state between them: a block's first four bytes
// reseed the predictor, so a slot boundary that is also a block boundary needs
// nothing handed over. A block larger than a slot cannot be served and is
// refused rather than split.
//
// 6,144 bytes is also what the capture DMA costs, and for the same reason: it
// exists only while the feature is running.
#define SOUND_STREAM_SLOTS      3
#define SOUND_STREAM_SLOT_BYTES 2048u

// The handshake between the two tasks, and the whole of it. The caller owns
// this struct and the bytes it points at; both must stay put until
// sound_stream_stop() returns true.
//
// Single producer, single consumer, two monotonic counters: the producer fills
// slot `filled % SLOTS`, writes its length, then publishes by advancing
// `filled`; the consumer takes slot `drained % SLOTS` and advances `drained`
// when it is through with it. Slots free = SLOTS - (filled - drained). The
// lengths are plain because the counter that publishes them is not: a
// sequentially consistent store to `filled` orders the write that preceded it.
typedef struct {
    uint8_t     *bytes;                      // SLOTS * SLOT_BYTES, the caller's
    uint32_t     length[SOUND_STREAM_SLOTS]; // valid bytes in each slot
    atomic_uint  filled;                     // slots published, never reset
    atomic_uint  drained;                    // slots consumed, never reset
    atomic_bool  eof;                        // the last slot has been published
} sound_stream_t;

// ---- the producer's three calls

// Room for another slot, and where to put it. NULL when the ring is full.
static inline uint8_t *sound_stream_slot(sound_stream_t *s) {
    unsigned filled=atomic_load(&s->filled), drained=atomic_load(&s->drained);
    if(filled-drained>=SOUND_STREAM_SLOTS) return NULL;
    return s->bytes+(size_t)(filled%SOUND_STREAM_SLOTS)*SOUND_STREAM_SLOT_BYTES;
}

// Publishes the slot sound_stream_slot() handed back, `length` bytes of it, and
// says whether it is the last one the source has.
static inline void sound_stream_publish(sound_stream_t *s, uint32_t length,
                                        bool last) {
    unsigned filled=atomic_load(&s->filled);
    s->length[filled%SOUND_STREAM_SLOTS]=length;
    if(last) atomic_store(&s->eof,true);
    // Last, and the reason the length above needs no atomic of its own.
    atomic_store(&s->filled,filled+1);
}

// Empties the ring. Only safe when no stream is running -- before a start, or
// after a stop that returned true.
static inline void sound_stream_rewind(sound_stream_t *s) {
    atomic_store(&s->filled,0);
    atomic_store(&s->drained,0);
    atomic_store(&s->eof,false);
}

// ---- the consumer's walk

// What the decoder is currently inside. Rebuilt at every slot boundary, which
// is exactly what makes a slot boundary free for ADPCM: a block reseeds the
// predictor from its own first four bytes, so nothing carries over as long as a
// slot ends where a block does.
typedef struct {
    const uint8_t *at;      // the current slot
    uint32_t bytes, used;   // its length, and how much has been decoded
    bool held;              // a slot is checked out and not yet given back
    ima_t ima;
} stream_read_t;

// Takes the next slot if the producer has published one. False means the ring
// is empty, which is an underrun and not an end -- `eof` is what says end.
static inline bool stream_take(sound_stream_t *s, stream_read_t *r,
                               uint16_t block) {
    unsigned drained=atomic_load(&s->drained);
    if(atomic_load(&s->filled)==drained) return false;
    unsigned index=drained%SOUND_STREAM_SLOTS;
    r->at=s->bytes+(size_t)index*SOUND_STREAM_SLOT_BYTES;
    r->bytes=s->length[index];
    r->used=0;
    r->held=true;
    if(block) r->ima=(ima_t){.data=r->at,.bytes=r->bytes,.block=block};
    return true;
}

// Gives the slot back, which is what lets the producer refill it.
static inline void stream_release(sound_stream_t *s, stream_read_t *r) {
    if(!r->held) return;
    r->held=false;
    atomic_store(&s->drained,atomic_load(&s->drained)+1);
}

// One output sample, and the whole of the slot bookkeeping.
//
// True with *out set is a sample. False means no sample was produced, and
// *ended is what separates the two reasons: the producer has published its last
// slot and the source is finished, or the producer is simply behind, which is
// an underrun -- silence for this sample and the same position next time. The
// caller must not advance its frame counter on a false.
//
// The loop rather than an if is for a zero-length last slot: a producer whose
// source stopped answering mid-file publishes one to say so, and it has to be
// walked past rather than decoded from.
static inline bool stream_sample(sound_stream_t *s, stream_read_t *r,
                                 uint16_t block, int *out, bool *ended) {
    *ended=false; *out=0;
    while(!r->held||r->used>=r->bytes) {
        stream_release(s,r);
        if(!stream_take(s,r,block)) {
            if(atomic_load(&s->eof)) *ended=true;
            return false;
        }
    }
    if(block) {
        *out=ima_next(&r->ima);
        // The decoder walks a block at a time and `at` is where it has got to,
        // so the slot is spent exactly when its bytes are. The second line is
        // the short last block of a file, where the nibbles run out before the
        // block does.
        r->used=r->ima.at;
        if(!r->ima.left&&r->ima.at>=r->bytes) r->used=r->bytes;
    } else {
        *out=r->used+1<r->bytes
            ?(int16_t)(r->at[r->used]|(r->at[r->used+1]<<8)):0;
        r->used+=2;
    }
    return true;
}
