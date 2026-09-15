#pragma once
#include <stdint.h>
#include <string.h>

// The WAV chunk walk, pulled out of pocket_av.c's wav_parse() so a host test
// can run the exact bytes the firmware runs without dragging in
// pocket_fs_read_at() and everything ESP-IDF that sits behind it. Same reason
// sound_stream.h and ima_adpcm.h live in headers of their own: this is the
// part of playback whose mistakes are a file that plays wrong or a refusal
// that should have been an open, not a crash -- easy to get wrong and easy to
// leave untested. No ESP dependency, so keep it that way.
//
// The caller supplies the read: pocket_av.c's is player_at(), which ranges
// against pocket_fs_read_at() and turns a short read into a refusal; a host
// test's is a bounds-checked read over an in-memory buffer. Either way the
// walk itself -- chunk order, chunk size arithmetic, the fmt/data checks, the
// two codecs this host accepts -- is the same code and the same behaviour.

typedef const char *(*wav_read_fn)(void *ctx, uint32_t at, uint8_t *out,
                                   uint32_t want);

typedef struct {
    uint32_t offset, bytes;  // the data chunk within the file
    uint16_t block;          // ADPCM block size, 0 for PCM16
    uint32_t per_block;      // output frames one ADPCM block is worth
    uint32_t frames;         // the clip's length in output frames
    int      ima;            // 0: PCM16, 1: IMA ADPCM
} wav_scan_t;

// How many chunks the walk may cross before it calls the file malformed. The
// walk is ranged rather than over a buffer, so a big LIST in front of data costs reads and
// not RAM, and this stops a hostile or corrupt file from making the reads
// unbounded.
#define WAV_SCAN_MAX_CHUNKS 64

// `want_rate` and `max_block` are the two device facts the pure scan needs
// and cannot know on its own: the output sample rate (no resampler exists) and
// how large an ADPCM block the streaming ring can serve whole (a block larger
// than a slot has nothing to reseed the predictor from at the slot boundary).
// pocket_av.c passes SOUND_SAMPLE_RATE and SOUND_STREAM_SLOT_BYTES; a host
// test passes whatever it is exercising.
static inline const char *wav_scan(wav_read_fn read, void *ctx, uint32_t size,
                                   uint32_t want_rate, uint32_t max_block,
                                   wav_scan_t *out) {
    memset(out,0,sizeof *out);
    uint8_t head[24];
    if(size<44) return "not a WAV file";
    const char *why=read(ctx,0,head,12);
    if(why) return why;
    if(memcmp(head,"RIFF",4)||memcmp(head+8,"WAVE",4)) return "not a WAV file";
    uint32_t at=12, rate=0, per_block=0;
    uint16_t format=0, channels=0, bits=0, align=0;
    int have_fmt=0;
    for(unsigned n=0;n<WAV_SCAN_MAX_CHUNKS&&at+8<=size;n++) {
        if((why=read(ctx,at,head,8))) return why;
        uint32_t body=(uint32_t)head[4]|((uint32_t)head[5]<<8)|
                      ((uint32_t)head[6]<<16)|((uint32_t)head[7]<<24);
        if(body>size-at-8) return "a chunk runs past the end of the file";
        if(!memcmp(head,"fmt ",4)&&body>=16) {
            uint8_t f[20];
            uint32_t w=body>=20?20:16;
            if((why=read(ctx,at+8,f,w))) return why;
            format=(uint16_t)(f[0]|(f[1]<<8));
            channels=(uint16_t)(f[2]|(f[3]<<8));
            rate=(uint32_t)f[4]|((uint32_t)f[5]<<8)|
                 ((uint32_t)f[6]<<16)|((uint32_t)f[7]<<24);
            align=(uint16_t)(f[12]|(f[13]<<8));
            bits=(uint16_t)(f[14]|(f[15]<<8));
            // IMA carries its samples-per-block in the fmt extension. Trusting
            // it rather than deriving it is what lets a file made by a tool
            // that pads its blocks still decode where its blocks really begin.
            if(w==20) per_block=(uint32_t)(f[18]|(f[19]<<8));
            have_fmt=1;
        } else if(!memcmp(head,"data",4)) {
            out->offset=at+8; out->bytes=body;
        }
        at+=8+body+(body&1);    // chunks are padded to an even length
    }
    if(!have_fmt) return "the file has no fmt chunk";
    if(!out->bytes) return "the file has no audio in it";
    if(channels!=1) return "this host plays one channel";
    if(rate!=want_rate) return "this host plays 24000 Hz and has no resampler";
    if(format==1&&bits==16) {
        out->block=0; out->per_block=1;
        out->frames=out->bytes/2;
        out->ima=0;
    } else if(format==0x11&&bits==4) {
        out->ima=1;
        if(align<8||(align&1)||align>out->bytes)
            return "the ADPCM block size is not usable";
        // A block has to fit one slot whole or a slot boundary would land
        // mid-block, where there is nothing to reseed the predictor from. This
        // is the one thing streaming refuses that the RAM clip did not, and it
        // is 2,048 bytes: 4,093 output frames, 170 ms of audio in one block.
        if(align>max_block) return "the ADPCM block is larger than the stream slot";
        out->block=align;
        out->per_block=per_block?per_block:(uint32_t)(align-4)*2+1;
        uint32_t blocks=out->bytes/align, tail=out->bytes%align;
        out->frames=blocks*out->per_block;
        // A short last block is still worth its header sample and its nibbles.
        if(tail>=4) out->frames+=1+(tail-4)*2;
    } else return "the codec is not one this host decodes";
    if(!out->frames) return "the file has no audio in it";
    return NULL;
}
