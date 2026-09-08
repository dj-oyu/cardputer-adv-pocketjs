// The ring and the slot walk that streamed playback rests on, compiled on a
// host from main/hal/sound_stream.h -- the same lines the audio task runs.
//
// Why this exists. Playback used to hold the whole sound in one buffer, so the
// decoder saw one flat array and there was nothing to get wrong about where it
// resumed. It now sees the source in 2,048-byte slots that a producer on
// another task refills underneath it, and a slot boundary in the wrong place
// does not crash and does not show on screen: it is a click or a burst of noise
// out of the speaker, and this board cannot hear itself (board_capture sees the
// framebuffer, not what reached the ES8311).
//
// So the claim under test is one sentence: CUTTING A SOURCE INTO SLOTS CHANGES
// NOTHING ABOUT THE SAMPLES THAT COME OUT. Everything below is that sentence
// with a different adversary each time --
//
//   1. ADPCM, slots of whole blocks, a producer that is never late.
//   2. ADPCM, a producer that is late at random, so the underrun path runs:
//      an underrun must stretch the sound and never drop or repeat a sample.
//   3. ADPCM with a short last block, which is a real file's last block.
//   4. PCM16, both producers.
//   5. The two ends: a zero-length last slot (the producer's source stopped
//      answering) and eof, which must stop the walk rather than decode past it.
//
// The payload is pseudorandom bytes on purpose. The reference is the same
// decoder over the same bytes in one piece, so what is being compared is the
// slot machinery and not the codec -- tools/test_ima.py is where the codec
// itself is checked against an independent implementation.
//
// Build and run (WSL; there is no gcc on the Windows side):
//   gcc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -I main/hal
//       tools/test_stream.c -o /tmp/t && /tmp/t
#include "sound_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if(!(cond)) { \
    printf("FAIL %s:%d ", __func__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); failures++; } } while(0)

// xorshift32, so the payload is the same on every host and every run: a test
// that fails once in twenty runs on somebody else's machine is a test nobody
// believes.
static uint32_t seed=0x1234567u;
static uint8_t next_byte(void) {
    seed^=seed<<13; seed^=seed>>17; seed^=seed<<5;
    return (uint8_t)(seed>>16);
}

// ---- the reference: the same decoder over the whole payload at once

static void decode_whole(const uint8_t *data, uint32_t bytes, uint16_t block,
                         int16_t *out, uint32_t frames) {
    if(block) {
        ima_t ima={.data=data,.bytes=bytes,.block=block};
        for(uint32_t i=0;i<frames;i++) out[i]=ima_next(&ima);
    } else {
        for(uint32_t i=0;i<frames;i++)
            out[i]=(int16_t)(data[i*2]|(data[i*2+1]<<8));
    }
}

// ---- the product path: a producer and the audio task's walk, interleaved
//
// The producer gets a turn once every WALK_PERIOD samples, because that is what
// it gets in the firmware: player_feed() runs from pocket_av_pump(), once a UI
// frame, and 800 samples is 33 ms at 24 kHz. Giving it a turn per sample -- the
// first version of this file -- made a "late" producer impossible to starve and
// reported zero underruns for a case whose whole purpose was to underrun.
//
// `lateness` is how often it then skips its turn, in 1/16ths. Zero is a
// producer that is never behind; 8 is one that misses half its frames, which is
// far worse than the pump can be and is the point.
#define WALK_PERIOD 800
typedef struct {
    uint32_t frames;      // samples produced
    uint32_t underruns;   // times the walk found the ring empty
    bool     ended;       // the walk reached eof
} walk_t;

static walk_t walk(const uint8_t *data, uint32_t bytes, uint16_t block,
                   uint32_t slot_bytes, uint32_t want, unsigned lateness,
                   int16_t *out, uint32_t truncate_at) {
    uint8_t ring_bytes[SOUND_STREAM_SLOTS*SOUND_STREAM_SLOT_BYTES];
    sound_stream_t s={.bytes=ring_bytes};
    sound_stream_rewind(&s);
    stream_read_t r={0};
    walk_t w={0};
    uint32_t fed=0, tick=0;
    bool stopped=false;   // the source stopped answering, once
    for(;;) {
        // The producer, standing in for pocket_av.c's player_feed().
        bool turn=(tick++%WALK_PERIOD)==0;
        if(turn&&!(lateness&&(next_byte()&15)<lateness)) {
            for(;;) {
                if(stopped) break;      // nothing follows the last slot
                if(fed>=bytes) break;
                uint8_t *slot=sound_stream_slot(&s);
                if(!slot) break;
                if(truncate_at&&fed>=truncate_at&&!stopped) {
                    // What player_feed() does when a read fails mid-source.
                    stopped=true;
                    sound_stream_publish(&s,0,true);
                    break;
                }
                uint32_t n=slot_bytes;
                if(n>bytes-fed) n=bytes-fed;
                memcpy(slot,data+fed,n);
                fed+=n;
                sound_stream_publish(&s,n,fed>=bytes);
            }
        }
        if(w.frames>=want) break;
        int sample=0; bool ended=false;
        if(stream_sample(&s,&r,block,&sample,&ended)) {
            out[w.frames++]=(int16_t)sample;
        } else if(ended) {
            w.ended=true; break;
        } else {
            w.underruns++;
            // A stalled producer must not spin this forever. In the firmware
            // that bound is STREAM_STARVE_LIMIT and 5.3 ms per block of
            // silence; here it is just a guard so a bug fails rather than hangs.
            if(w.underruns>1000000u) { printf("FAIL walk stalled\n"); failures++; break; }
        }
    }
    stream_release(&s,&r);
    return w;
}

// ---- the cases

static void case_adpcm(const char *label, uint16_t block, uint32_t blocks,
                       uint32_t tail, unsigned lateness) {
    uint32_t bytes=blocks*block+tail;
    uint8_t *data=malloc(bytes);
    for(uint32_t i=0;i<bytes;i++) data[i]=next_byte();
    // What pocket_av.c's wav_parse computes for the same payload.
    uint32_t per_block=(uint32_t)(block-4)*2+1;
    uint32_t frames=blocks*per_block;
    if(tail>=4) frames+=1+(tail-4)*2;

    int16_t *want=malloc(frames*sizeof(int16_t));
    int16_t *got=malloc(frames*sizeof(int16_t));
    decode_whole(data,bytes,block,want,frames);
    // The slot is what the player would use: whole blocks, at most a slot's
    // worth. A tail shorter than a block still goes in a slot of its own.
    uint32_t slot=SOUND_STREAM_SLOT_BYTES-SOUND_STREAM_SLOT_BYTES%block;
    walk_t w=walk(data,bytes,block,slot,frames,lateness,got,0);

    CHECK(w.frames==frames,"%s produced %u of %u frames",label,
          (unsigned)w.frames,(unsigned)frames);
    uint32_t bad=0, first=0;
    for(uint32_t i=0;i<w.frames;i++)
        if(got[i]!=want[i]) { if(!bad) first=i; bad++; }
    CHECK(!bad,"%s differs in %u of %u samples, first at %u (%d vs %d)",
          label,(unsigned)bad,(unsigned)frames,(unsigned)first,
          bad?got[first]:0,bad?want[first]:0);
    if(lateness) CHECK(w.underruns>0,"%s never underran, so the path is untested",label);
    printf("%-28s frames=%-7u slot=%-5u underruns=%u\n",
           label,(unsigned)w.frames,(unsigned)slot,(unsigned)w.underruns);
    free(data); free(want); free(got);
}

static void case_pcm(const char *label, uint32_t frames, unsigned lateness) {
    uint32_t bytes=frames*2;
    uint8_t *data=malloc(bytes);
    for(uint32_t i=0;i<bytes;i++) data[i]=next_byte();
    int16_t *want=malloc(frames*sizeof(int16_t));
    int16_t *got=malloc(frames*sizeof(int16_t));
    decode_whole(data,bytes,0,want,frames);
    walk_t w=walk(data,bytes,0,SOUND_STREAM_SLOT_BYTES,frames,lateness,got,0);
    CHECK(w.frames==frames,"%s produced %u of %u frames",label,
          (unsigned)w.frames,(unsigned)frames);
    CHECK(!memcmp(got,want,w.frames*sizeof(int16_t)),"%s differs",label);
    if(lateness) CHECK(w.underruns>0,"%s never underran, so the path is untested",label);
    printf("%-28s frames=%-7u underruns=%u\n",label,(unsigned)w.frames,
           (unsigned)w.underruns);
    free(data); free(want); free(got);
}

// The producer's source stops answering halfway. player_feed() publishes a
// zero-length last slot to say so; the walk has to step past it and report the
// end rather than decode from an empty slot or spin on it.
static void case_truncated(void) {
    const uint16_t block=256;
    uint32_t bytes=block*40;
    uint8_t *data=malloc(bytes);
    for(uint32_t i=0;i<bytes;i++) data[i]=next_byte();
    uint32_t per_block=(uint32_t)(block-4)*2+1, frames=40*per_block;
    int16_t *got=malloc(frames*sizeof(int16_t));
    uint32_t slot=SOUND_STREAM_SLOT_BYTES-SOUND_STREAM_SLOT_BYTES%block;
    // Cut off after 16 blocks, which is not a slot boundary (a slot is 8).
    walk_t w=walk(data,bytes,block,slot,frames,0,got,block*16+1);
    CHECK(w.ended,"a truncated source did not report an end");
    CHECK(w.frames>0&&w.frames<frames,
          "a truncated source produced %u frames of %u",(unsigned)w.frames,
          (unsigned)frames);
    // Whatever it did produce has to be the real first samples, not silence.
    int16_t *want=malloc(frames*sizeof(int16_t));
    decode_whole(data,bytes,block,want,frames);
    CHECK(!memcmp(got,want,w.frames*sizeof(int16_t)),
          "a truncated source changed the samples before the cut");
    printf("%-28s frames=%-7u ended=%d\n","truncated adpcm",
           (unsigned)w.frames,(int)w.ended);
    free(data); free(got); free(want);
}

int main(void) {
    printf("sound_stream.h: slots must not change the samples\n");
    // A block of 256 bytes is what an encoder produces for 24 kHz mono and what
    // pocket_av.c's seekBlockAligned is stated against; 1024 exercises a slot
    // that holds two blocks, and 2048 one that holds exactly one -- the largest
    // block wav_parse accepts.
    case_adpcm("adpcm 256, prompt",      256, 40, 0, 0);
    case_adpcm("adpcm 256, late",        256, 40, 0, 8);
    case_adpcm("adpcm 256, short tail",  256, 40, 71, 0);
    case_adpcm("adpcm 1024, late",      1024, 11, 0, 8);
    // A 2,048-byte block IS a slot, so three of them buffer 12,279 frames --
    // more than a producer missing half its turns can drain. It takes one
    // missing fifteen in sixteen to starve, which is what this row is.
    case_adpcm("adpcm 2048, late",      2048,  5, 0, 15);
    case_adpcm("adpcm 2048, tail only", 2048,  0, 903, 0);
    case_pcm  ("pcm16, prompt",              12000, 0);
    case_pcm  ("pcm16, late",                12000, 8);
    case_truncated();
    printf(failures?"FAILED (%d)\n":"OK (%d failures)\n",failures);
    return failures?1:0;
}
