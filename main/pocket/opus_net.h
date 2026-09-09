#pragma once
#include "opus_feed.h"
#include "sound_stream.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>   // memcpy, for the inline carry below

// Opus arriving over HTTP: the receive task that fills the compressed ring.
//
// WHY THIS EXISTS RATHER THAN pocket.net.http.request(). Measured on the board
// 2026-09-09, playing with a linked radio:
//
//   linked+play      free 11,632   largest 7,680
//   http.request     REFUSED at the gate -- 11,404 free against NET_PLAIN_MIN_FREE
//
// An independent request cannot fit beside a running player, and the reason is
// duplication rather than size: pocket_net.c's worker allocates a 6,144-byte
// task stack, a 2,048-byte request-header buffer, a 1,024-byte response-header
// buffer and a 1,024-byte chunk buffer -- and then the JS app copies that chunk
// into the packet ring. A receiver that IS the producer needs none of the three
// buffers: it reads straight into the ring slot it is going to publish. That is
// 4,096 bytes of the gap, and the reads stop crossing the JS task at all.
//
// The task stack stays. This work cannot go on the ui task (a socket read
// blocks for as long as the network feels like) and it cannot go on the decode
// task (a stall there is an underrun, which is the thing the ring exists to
// absorb). One task, and it is the one pocket_net.c would have created anyway.
//
// ------------------------------------------------------------- the container
//
// THIS SERVES .pok, AND THAT IS A STAGING DECISION, NOT THE CONTAINER DECISION.
// The container decision is Ogg, and the file-era reasoning for .pok does not
// survive a network:
//
//   "a per-page CRC32 costs the tightest task in the firmware" -- it named the
//     wrong task (the receiver is this one, not the audio task) and the wrong
//     order of magnitude: at Opus's 3,204 B/s a software CRC32 is about 0.013%
//     of a core, against PCM16's 48,000 B/s where the argument was real.
//   "flash and SD already checksum underneath us" -- nothing under a socket does.
//   "resync is pointless for a file whose length the filesystem states" -- a
//     stream has no length and stalls are normal.
//
// And Opus on the internet is Ogg or WebM; a .pok server is a server we would
// have to write. So Ogg is what this must end up speaking. It is not what this
// file speaks TODAY because the memory shape and the demuxer are two questions,
// and answering them in one build is how you get a result that cannot say which
// half was wrong. The Ogg demuxer goes in front of this, emitting the same
// [u16 length][payload] stream into the same ring; opus_feed.c does not change.
//
// The CELT 20 ms mono gate applies to every packet either way, at open and in
// the decode task. It is what makes the 10,476-byte decode stack a bound.
//
// ------------------------------------------------------- stall vs underrun
//
// They are different events and they are reported differently. An underrun is
// the audio task finding the PCM ring empty: audio was owed and silence was
// inserted. A STALL is this task finding the socket slow -- nothing has been
// lost yet, because the ring is exactly the thing standing between a stall and
// an underrun. 6,144 bytes of compressed ring is 2.0 seconds, so a stall
// shorter than that is inaudible and worth counting rather than reporting as a
// fault. Only when the ring drains does a stall become an underrun, and by then
// the underrun counter is already saying so.

// How much of a filled slot is publishable, and what carries to the next one.
// Kept inline and ESP-free for the same reason sound_stream.h is:
// tools/test_opus_stream.c compiles these exact lines and checks that a packet
// stream cut at arbitrary socket boundaries comes out identical to the same
// stream cut at slot boundaries. A container bug here is silent -- a length
// prefix read one byte late is a packet the decoder refuses, not a crash.
typedef struct {
    uint32_t publish;    // bytes of `slot` that are whole packets
    uint32_t carry_off;  // where the leftover starts (== publish)
    uint32_t carry_len;  // bytes that belong to the next slot
    bool     last;       // the source is finished
} opus_net_cut_t;

static inline opus_net_cut_t opus_net_cut(const uint8_t *slot, uint32_t have,
                                          bool eof) {
    opus_net_cut_t c;
    c.publish=opus_pak_whole(slot,have);
    c.carry_off=c.publish;
    c.carry_len=have-c.publish;
    // A leftover at eof is a packet the sender never finished. It can never be
    // completed, so the stream ends with the whole packets in front of it --
    // the same call player_feed_opus() makes about a truncated file.
    c.last=eof;
    return c;
}

// Where the leftover lives between slots, and it is not a buffer. The bytes stay
// in the slot they arrived in: with three slots the producer does not come back
// to `prev` until it has filled two more, and copying out of it is the first
// thing it does with the next one. So a partial packet costs no memory at all,
// which on this board is the difference between a design and a wish.
typedef struct {
    uint8_t *prev;        // the slot published last
    uint32_t carry_off;   // where its unconsumed tail starts
    uint32_t carry_len;   // how long that tail is
} opus_net_carry_t;

// Opens a slot: puts the previous slot's tail at its head and says how much is
// already in it.
static inline uint32_t opus_net_resume(opus_net_carry_t *k, uint8_t *slot) {
    if(!k->carry_len) return 0;
    memcpy(slot,k->prev+k->carry_off,k->carry_len);
    uint32_t n=k->carry_len;
    k->carry_len=0;
    return n;
}

// Closes a slot, once its cut has been published.
static inline void opus_net_advance(opus_net_carry_t *k, uint8_t *slot,
                                    opus_net_cut_t c) {
    k->prev=slot;
    k->carry_off=c.carry_off;
    k->carry_len=c.carry_len;
}

typedef enum {
    OPUS_NET_OK=0,
    OPUS_NET_NOMEM,      // no room for the task or the client
    OPUS_NET_BUSY,       // a receive is already running
    OPUS_NET_BAD_URL,    // not http/https, or too long
} opus_net_start_t;

// Connects, walks the .pok header, then fills `packets` until the body ends or
// opus_net_stop() is called. Returns as soon as the task exists: everything
// after that is reported through the three observers below, because a task
// cannot report a failure it has not yet had time to have. That sentence cost
// this project a board run; see the dec_handle comment in opus_feed.c.
opus_net_start_t opus_net_start(const char *url, sound_stream_t *packets);
// Waits for the task to be gone, so the caller may free the ring. False means
// it did not stop and the ring must not be freed.
bool opus_net_stop(void);

// Has the header arrived and been accepted? Until this is true the container is
// not known and nothing has been published.
bool opus_net_ready(void);
// The header, valid only once opus_net_ready(). Frame count and preSkip come
// from it exactly as they do for a file.
const opus_pak_t *opus_net_header(void);
// NULL while the stream is healthy, otherwise the sentence to hand the app.
// Set once and never cleared while running.
const char *opus_net_error(void);
// Socket reads that came back empty while the body was unfinished. Not a fault
// and not an underrun; see the header comment.
uint32_t opus_net_stalls(void);
