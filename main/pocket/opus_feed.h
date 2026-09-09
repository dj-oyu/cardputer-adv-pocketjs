#pragma once
#include "sound_stream.h"
#include <stdbool.h>
#include <stdint.h>

// Opus playback: the container, the packet gate, and the decode task.
//
// The inline half of this header has no ESP dependency -- same rule as
// sound_stream.h and for the same reason. tools/test_opus_pak.c compiles these
// exact lines on a host and checks the container walk against a file the
// generator wrote, because a container bug is silent here: a length prefix read
// one byte late is a packet the decoder refuses, not a crash.
//
// ---------------------------------------------------------------- the container
//
// It is not Ogg, and the reason is what it costs ON THE DEVICE. Ogg would put a
// 27-byte page header plus a lacing table plus a CRC32 in front of every ~4 KB,
// and the player would have to CRC every byte it feeds -- work in the same task
// that is already the tightest thing in this firmware, to protect against
// corruption that flash and SD already checksum underneath us. Ogg's real payoff
// is streams that arrive from a network with unknown length and possible resync;
// ours arrive from a file whose length the filesystem states.
//
// So: a header, an index, and length-prefixed packets. We own the generator
// (tools/make_opus_asset.py, the way tools/make_wav_asset.py is the precedent),
// so what the device does not need is simply not written.
//
//   0   "POK1"
//   4   u16 version = 1
//   6   u16 channels = 1
//   8   u32 sampleRate = 24000
//   12  u16 frameSamples       output frames one packet decodes to (480 = 20 ms)
//   14  u16 maxPacketBytes     the largest packet in the file
//   16  u32 packetCount
//   20  u32 totalFrames        playable output frames, preSkip already removed
//   24  u16 preSkip            encoder lookahead, discarded from the first packet
//   26  u16 indexInterval      packets between index entries
//   28  u32 indexCount
//   32  u32 dataOffset         first byte of the first packet
//   36  u32 index[indexCount]  file offset of packet number i*indexInterval
//   ..  [u16 length][payload] ...
//
// THE INDEX IS WHAT BUYS seek(). Without it a seek is a walk of every length
// prefix from byte one, which is a read of the whole file on the drawing task.
// One entry per indexInterval packets makes it one read, and makes the seek
// resolution indexInterval*20 ms rather than exact -- the same shape of rounding
// an ADPCM block already imposes, and reported the same way. At the interval the
// generator uses (5 packets) an 8-second asset spends 324 bytes on its index,
// 1.3% of the file, to avoid spending the whole file on every seek.
#define OPUS_PAK_HEADER  36u
#define OPUS_PAK_VERSION 1u

// The output frames one packet is worth, and the only value this host accepts:
// 20 ms at 24 kHz. Everything downstream is sized from it.
#define OPUS_PAK_FRAME_SAMPLES 480u

typedef struct {
    uint32_t sample_rate, packet_count, total_frames;
    uint32_t index_count, index_offset, data_offset, file_bytes;
    uint16_t version, channels, frame_samples, max_packet, pre_skip, index_interval;
} opus_pak_t;

// ------------------------------------------------------------- the packet gate
//
// WHY THIS EXISTS AT ALL, because refusing input is a design decision and not a
// shortcut. The decode stack this firmware sizes for is 10,420 bytes, MEASURED,
// and measured for exactly one thing: CELT, wideband, 20 ms, mono. That figure
// is a lower bound for SILK and for hybrid, which run different code with a
// different peak. Nobody has measured those on this part. A stack sized from an
// unmeasured bound is a stack that overflows in front of somebody.
//
// So the player accepts the mode it has a measurement for and refuses the rest
// by name, the way it already refuses an ADPCM block bigger than a slot. That
// turns 10,420 from a guess into a bound, and it removes SILK and hybrid from
// the critical path entirely.
//
// The TOC byte is the first byte of every Opus packet (RFC 6716 3.1):
//   bits 7..3  config   0-11 SILK, 12-15 hybrid, 16-31 CELT-only
//   bit  2     stereo
//   bits 1..0  code     0 = exactly one frame in this packet
// Within CELT-only, config%4 selects 2.5/5/10/20 ms, so config%4==3 is 20 ms at
// every bandwidth. Which bandwidth does not matter: opus_decode produces the
// decoder's rate, so NB, WB, SWB and FB all come out as 480 frames at 24 kHz.
static inline bool opus_pak_toc_ok(uint8_t toc) {
    unsigned config=toc>>3, stereo=(toc>>2)&1, code=toc&3;
    return config>=16 && (config&3)==3 && !stereo && code==0;
}

// ------------------------------------------------------------ the header walk
//
// NULL when the header describes something this host can play, and the sentence
// to hand the app when it does not. Every length is checked against the file
// size before anything is derived from it.
static inline const char *opus_pak_parse(const uint8_t *h, uint32_t size,
                                         opus_pak_t *out) {
    if(size<OPUS_PAK_HEADER) return "not an Opus packet file";
    if(h[0]!='P'||h[1]!='O'||h[2]!='K'||h[3]!='1') return "not an Opus packet file";
    #define PAK_U16(i) ((uint32_t)h[i]|((uint32_t)h[(i)+1]<<8))
    #define PAK_U32(i) (PAK_U16(i)|(PAK_U16((i)+2)<<16))
    out->version=(uint16_t)PAK_U16(4);
    out->channels=(uint16_t)PAK_U16(6);
    out->sample_rate=PAK_U32(8);
    out->frame_samples=(uint16_t)PAK_U16(12);
    out->max_packet=(uint16_t)PAK_U16(14);
    out->packet_count=PAK_U32(16);
    out->total_frames=PAK_U32(20);
    out->pre_skip=(uint16_t)PAK_U16(24);
    out->index_interval=(uint16_t)PAK_U16(26);
    out->index_count=PAK_U32(28);
    out->data_offset=PAK_U32(32);
    #undef PAK_U32
    #undef PAK_U16
    out->index_offset=OPUS_PAK_HEADER;
    out->file_bytes=size;
    if(out->version!=OPUS_PAK_VERSION) return "this Opus container is a later version";
    if(out->channels!=1) return "this host plays one channel";
    if(out->sample_rate!=24000) return "this host plays 24000 Hz and has no resampler";
    if(out->frame_samples!=OPUS_PAK_FRAME_SAMPLES)
        return "this host decodes 20 ms Opus packets only";
    // A packet has to fit one slot whole WITH its length prefix, for the same
    // reason an ADPCM block does: a slot boundary inside a packet would leave the
    // decoder holding half of one and nothing to reseed from.
    if(out->max_packet<1||out->max_packet>SOUND_STREAM_SLOT_BYTES-2)
        return "an Opus packet is larger than the stream slot";
    if(!out->packet_count||!out->total_frames) return "the file has no audio in it";
    if(out->packet_count>size||
       out->total_frames>out->packet_count*(uint32_t)out->frame_samples)
        return "the header claims more audio than its packets hold";
    if(!out->index_count||!out->index_interval) return "the file has no seek index";
    if(out->index_count>(out->packet_count+out->index_interval-1)/out->index_interval)
        return "the seek index is longer than the file";
    if(out->index_count>(size-OPUS_PAK_HEADER)/4) return "the seek index runs past the end";
    if(out->data_offset!=OPUS_PAK_HEADER+out->index_count*4u||out->data_offset>=size)
        return "the packets do not begin where the header says";
    return NULL;
}

// How much of `buf` is whole [u16 length][payload] packets. The producer reads a
// slot-full starting at a packet boundary and publishes only this much, so a slot
// always ends where a packet does; `feed` advances by the return value and the
// leftover bytes are read again as the head of the next slot. Re-reading at most
// one packet is cheaper than carrying a partial packet across a slot boundary.
//
// Zero means the first packet in `buf` does not fit a slot, which
// opus_pak_parse has already refused through maxPacketBytes -- so the caller
// treats a zero as a corrupt file rather than as backpressure.
static inline uint32_t opus_pak_whole(const uint8_t *buf, uint32_t len) {
    uint32_t at=0;
    for(;;) {
        if(at+2>len) return at;
        uint32_t n=(uint32_t)buf[at]|((uint32_t)buf[at+1]<<8);
        if(!n) return at;                 // a zero-length packet is corruption
        if(at+2+n>len) return at;
        at+=2+n;
    }
}

// ------------------------------------------------------------- the decode task
//
// Created when playback starts and deleted when it ends, so Opus costs nothing
// while nothing is playing -- which is the whole difference between this and
// enlarging the "sfx" task's 4,096-byte stack, because that stack is heap taken
// once at sound_init and never given back.
//
// It is not the ui task either: decoding there would add 5.3 ms to a frame
// already measured at 39.9 ms against a 33.3 ms budget.
//
// Two rings, and the shape is producer -> ring -> transform -> ring -> consumer:
//   ui/JS task   reads the file          -> `packets` ring (compressed)
//   decode task  opus_decode             -> `pcm` ring (PCM16, 24 kHz mono)
//   audio task   the existing consumer, which sees SOUND_STREAM_PCM16 and cannot
//                tell that a codec was involved.
// That last line is the point: sound.c is not edited for this at all.
//
// `skip` is the encoder lookahead to discard from the first decoded packet; pass
// 0 when resuming from a seek index, which is already well past it.
//
// WHY THIS IS NOT A bool ANY MORE. Both of this feature's large allocations --
// the decoder's 18,436 and the task's 14,336, each one contiguous block -- are
// made here, and running out of room for them is the single most likely way
// Opus fails on this board. It used to be indistinguishable from every other
// refusal, and worse than that: the decoder was built INSIDE the task, after
// this function had already returned true, so an out-of-memory could only
// surface as a fault and reached the app as IO_ERROR. Telling a caller "no
// room" apart from "busy" is what lets pocket_av.c answer OUT_OF_MEMORY with
// the two numbers that decide it.
typedef enum {
    OPUS_FEED_OK=0,
    OPUS_FEED_NOMEM,    // the decoder or the task stack would not fit
    OPUS_FEED_BUSY,     // a stream is already decoding
} opus_feed_start_t;
opus_feed_start_t opus_feed_start(sound_stream_t *pcm, sound_stream_t *packets,
                                  uint32_t frames, uint16_t skip);
// Waits for the task to be gone before returning, so the caller may then free
// both rings. False means it did not stop and the rings must not be freed.
bool     opus_feed_stop(void);
// Packets the gate refused, plus decode failures. Nonzero means the stream ended
// as an error rather than as an end.
uint32_t opus_feed_faults(void);
