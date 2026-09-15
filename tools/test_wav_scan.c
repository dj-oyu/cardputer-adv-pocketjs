// The WAV chunk walk that audio.playback's open() runs before anything plays,
// compiled on a host from main/pocket/wav_scan.h -- the same lines
// pocket_av.c's wav_parse() calls. docs/api/backlog.md (9 節, 音声) named this
// gap: tools/test_stream.c covers the ring and the slot walk once a file has
// already been accepted, and nothing host-side had ever driven the header
// scan itself. A JUNK chunk in front of fmt, or a data chunk that starts past
// the first few hundred bytes, had only ever been exercised by a real file on
// real hardware.
//
// Every case here builds a WAV as bytes in memory and hands wav_scan() a
// read callback over that buffer -- the same shape as pocket_av.c's
// wav_scan_read(), which wraps player_at() over pocket_fs_read_at(). A
// `truncated` source models a short read (a file that got smaller than what
// stat() reported, which is exactly the failure player_at() turns into "the
// source could not be read") rather than a read past a hard end.
//
// Build and run (WSL; there is no gcc on the Windows side):
//   gcc -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined
//       -I main/pocket tools/test_wav_scan.c -o /tmp/t && /tmp/t
#include "wav_scan.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if(!(cond)) { \
    printf("FAIL %s:%d ", __func__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); failures++; } } while(0)

// The two device facts wav_scan() needs and pocket_av.c supplies from
// SOUND_SAMPLE_RATE and SOUND_STREAM_SLOT_BYTES. Kept as plain numbers here so
// this file has no dependency on main/hal at all, per wav_scan.h's own rule.
#define RATE 24000u
#define MAX_BLOCK 2048u

// ---- a WAV built a chunk at a time, in memory

typedef struct { uint8_t *data; uint32_t len, cap; } buf_t;

static void buf_put(buf_t *b, const void *p, uint32_t n) {
    if(b->len+n>b->cap) {
        b->cap=(b->len+n)*2+64;
        b->data=realloc(b->data,b->cap);
    }
    memcpy(b->data+b->len,p,n);
    b->len+=n;
}
static void put_u32(buf_t *b, uint32_t v) {
    uint8_t x[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
    buf_put(b,x,4);
}
static void put_u16(buf_t *b, uint16_t v) {
    uint8_t x[2]={(uint8_t)v,(uint8_t)(v>>8)};
    buf_put(b,x,2);
}
// A RIFF chunk: four-byte tag, little-endian size, body, and the pad byte an
// odd-sized body owes the next chunk. wav_scan() never reads the pad byte --
// only its own `at+=8+body+(body&1)` skip has to agree it is there.
static void put_chunk(buf_t *b, const char *tag, const void *body, uint32_t n) {
    buf_put(b,tag,4);
    put_u32(b,n);
    if(n) buf_put(b,body,n);
    if(n&1) { uint8_t z=0; buf_put(b,&z,1); }
}
static buf_t riff_start(void) {
    buf_t b={0};
    buf_put(&b,"RIFF",4);
    put_u32(&b,0);            // the overall size field; wav_scan() never reads it
    buf_put(&b,"WAVE",4);
    return b;
}

// A standard PCM/IMA fmt body. `ext` is the two-byte samplesPerBlock IMA
// carries past cbSize; pass 0 and want_ext=false for a plain 16-byte PCM fmt.
static void put_fmt(buf_t *b, uint16_t format, uint16_t channels, uint32_t rate,
                    uint16_t block_align, uint16_t bits, bool want_ext,
                    uint16_t samples_per_block) {
    buf_t f={0};
    put_u16(&f,format);
    put_u16(&f,channels);
    put_u32(&f,rate);
    put_u32(&f,rate*channels*(bits/8));   // byteRate: never read by wav_scan()
    put_u16(&f,block_align);
    put_u16(&f,bits);
    if(want_ext) {
        put_u16(&f,2);                    // cbSize
        put_u16(&f,samples_per_block);
    }
    put_chunk(b,"fmt ",f.data,f.len);
    free(f.data);
}

// ---- the read callback: a bounds-checked window over one of these buffers,
// same failure shape as player_at() over a short pocket_fs_read_at().
typedef struct { const uint8_t *data; uint32_t real_len; } src_t;
static const char *src_read(void *ctx, uint32_t at, uint8_t *out, uint32_t want) {
    src_t *s=ctx;
    if((uint64_t)at+want>s->real_len) return "the source could not be read";
    memcpy(out,s->data+at,want);
    return NULL;
}

// Runs wav_scan() over `f`, claiming the file is `claim_size` bytes (defaults
// to f->len when 0) so a case can lie about the size independently of what
// bytes actually back it -- that is how the truncated-header case is built.
static const char *scan(buf_t *f, uint32_t claim_size, wav_scan_t *out) {
    src_t s={.data=f->data,.real_len=f->len};
    return wav_scan(src_read,&s,claim_size?claim_size:f->len,RATE,MAX_BLOCK,out);
}

// ---- the cases

static void case_pcm_minimal(void) {
    buf_t f=riff_start();
    put_fmt(&f,1,1,RATE,2,16,false,0);
    uint8_t pcm[8]={1,2,3,4,5,6,7,8};      // 4 frames of PCM16
    put_chunk(&f,"data",pcm,sizeof pcm);
    wav_scan_t r;
    const char *why=scan(&f,0,&r);
    CHECK(!why,"minimal PCM16 refused: %s",why?why:"");
    CHECK(r.ima==0&&r.block==0,"minimal PCM16 codec wrong: ima=%d block=%u",
          r.ima,r.block);
    CHECK(r.offset==f.len-sizeof pcm,"minimal PCM16 offset=%u want=%u",
          r.offset,(unsigned)(f.len-sizeof pcm));
    CHECK(r.bytes==sizeof pcm&&r.frames==4,
          "minimal PCM16 bytes=%u frames=%u",r.bytes,r.frames);
    printf("%-32s ok offset=%u frames=%u\n","pcm16 minimal",r.offset,r.frames);
    free(f.data);
}

static void case_ima_with_ext(void) {
    buf_t f=riff_start();
    const uint16_t block=256;
    // The real per-block frame count a 256-byte IMA block holds, so this case
    // also proves the fmt extension is TRUSTED over the derived value (a
    // wrong extension would show up as a frame count that does not match
    // either formula).
    uint16_t spb=(uint16_t)((block-4)*2+1);
    put_fmt(&f,0x11,1,RATE,block,4,true,spb);
    uint32_t blocks=3;
    uint8_t *ima=calloc(1,blocks*block);
    put_chunk(&f,"data",ima,blocks*block);
    free(ima);
    wav_scan_t r;
    const char *why=scan(&f,0,&r);
    CHECK(!why,"IMA with fmt extension refused: %s",why?why:"");
    CHECK(r.ima==1&&r.block==block,"IMA codec/block wrong: ima=%d block=%u",
          r.ima,r.block);
    CHECK(r.per_block==spb,"IMA per_block=%u want=%u (extension not trusted)",
          r.per_block,spb);
    CHECK(r.frames==blocks*spb,"IMA frames=%u want=%u",r.frames,
          (unsigned)(blocks*spb));
    printf("%-32s ok per_block=%u frames=%u\n","ima with fmt extension",
           r.per_block,r.frames);
    free(f.data);
}

static void case_junk_before_fmt(void) {
    buf_t f=riff_start();
    uint8_t junk[13]; memset(junk,0xAA,sizeof junk);   // odd size on purpose
    put_chunk(&f,"JUNK",junk,sizeof junk);
    put_fmt(&f,1,1,RATE,2,16,false,0);
    uint8_t pcm[4]={9,9,9,9};
    put_chunk(&f,"data",pcm,sizeof pcm);
    wav_scan_t r;
    const char *why=scan(&f,0,&r);
    CHECK(!why,"JUNK-then-fmt-then-data refused: %s",why?why:"");
    CHECK(r.bytes==sizeof pcm&&r.frames==2,
          "JUNK case bytes=%u frames=%u",r.bytes,r.frames);
    printf("%-32s ok offset=%u\n","JUNK chunk before fmt",r.offset);
    free(f.data);
}

// data starting past the 4 KB mark: a big LIST in front, which is exactly the
// shape PLAYER_MAX_CHUNKS/WAV_SCAN_MAX_CHUNKS's comment is about -- the walk
// must skip over it by its size field, not by scanning its bytes.
static void case_data_past_4k(void) {
    buf_t f=riff_start();
    put_fmt(&f,1,1,RATE,2,16,false,0);
    uint32_t pad=4200;
    uint8_t *list=calloc(1,pad);
    put_chunk(&f,"LIST",list,pad);
    free(list);
    uint8_t pcm[6]={1,2,3,4,5,6};
    put_chunk(&f,"data",pcm,sizeof pcm);
    wav_scan_t r;
    const char *why=scan(&f,0,&r);
    CHECK(!why,"data past 4KB refused: %s",why?why:"");
    CHECK(r.offset>4096,"data offset=%u did not land past 4KB",r.offset);
    CHECK(r.bytes==sizeof pcm&&r.frames==3,
          "data past 4KB bytes=%u frames=%u",r.bytes,r.frames);
    printf("%-32s ok offset=%u\n","data chunk past 4KB",r.offset);
    free(f.data);
}

static void case_odd_chunk_pad(void) {
    buf_t f=riff_start();
    put_fmt(&f,1,1,RATE,2,16,false,0);
    uint8_t junk[7]; memset(junk,0x5A,sizeof junk);   // odd -> one pad byte
    put_chunk(&f,"JUNK",junk,sizeof junk);
    uint8_t pcm[4]={1,2,3,4};
    put_chunk(&f,"data",pcm,sizeof pcm);
    wav_scan_t r;
    const char *why=scan(&f,0,&r);
    CHECK(!why,"odd chunk with pad byte refused: %s",why?why:"");
    CHECK(r.bytes==sizeof pcm&&r.frames==2,
          "odd chunk pad case bytes=%u frames=%u",r.bytes,r.frames);
    printf("%-32s ok offset=%u\n","odd-sized chunk + pad byte",r.offset);
    free(f.data);
}

// A source that got shorter than what stat() reported: `claim_size` says the
// next chunk header is there (at+8<=size passes) but the buffer backing it
// does not actually hold 8 bytes, so the read inside the loop fails. This is
// the shape player_at() turns into "the source could not be read", not the
// "runs past the end" arithmetic check below.
static void case_truncated_chunk_header(void) {
    buf_t f=riff_start();
    put_fmt(&f,1,1,RATE,2,16,false,0);
    uint8_t pcm[4]={1,2,3,4};
    put_chunk(&f,"data",pcm,sizeof pcm);
    uint32_t claim=f.len+8;   // promises one more full chunk header
    // f.data itself is not extended: only 0 of those 8 bytes really exist.
    wav_scan_t r;
    const char *why=scan(&f,claim,&r);
    CHECK(why&&!strcmp(why,"the source could not be read"),
          "truncated chunk header: got %s",why?why:"(accepted)");
    printf("%-32s refused: %s\n","truncated chunk header",why);
    free(f.data);
}

// A chunk whose size field claims more bytes than remain in the file at all --
// the arithmetic check, not a short read. It comes after a file that would
// otherwise parse (valid fmt+data already seen), so this also proves a chunk
// error found late still overrides an earlier valid data chunk.
static void case_chunk_past_eof(void) {
    buf_t f=riff_start();
    put_fmt(&f,1,1,RATE,2,16,false,0);
    uint8_t pcm[4]={1,2,3,4};
    put_chunk(&f,"data",pcm,sizeof pcm);
    // Append just a header -- tag + a size field that claims far more body
    // than any file could hold -- and nothing else. claim_size covers exactly
    // this header (8 bytes), so the loop attempts to read it (real bytes are
    // present for the header itself) and then the size check fires.
    buf_put(&f,"AAAA",4);
    put_u32(&f,0xFFFFFFF0u);
    wav_scan_t r;
    const char *why=scan(&f,f.len,&r);
    CHECK(why&&!strcmp(why,"a chunk runs past the end of the file"),
          "chunk past EOF: got %s",why?why:"(accepted)");
    printf("%-32s refused: %s\n","chunk size past EOF",why);
    free(f.data);
}

static void case_wrong_rate(void) {
    buf_t f=riff_start();
    put_fmt(&f,1,1,44100,2,16,false,0);   // not RATE (24000)
    uint8_t pcm[4]={1,2,3,4};
    put_chunk(&f,"data",pcm,sizeof pcm);
    wav_scan_t r;
    const char *why=scan(&f,0,&r);
    CHECK(why&&!strcmp(why,"this host plays 24000 Hz and has no resampler"),
          "wrong sample rate: got %s",why?why:"(accepted)");
    printf("%-32s refused: %s\n","wrong sample rate",why);
    free(f.data);
}

static void case_wrong_channels(void) {
    buf_t f=riff_start();
    put_fmt(&f,1,2,RATE,4,16,false,0);    // stereo
    uint8_t pcm[8]={1,2,3,4,5,6,7,8};
    put_chunk(&f,"data",pcm,sizeof pcm);
    wav_scan_t r;
    const char *why=scan(&f,0,&r);
    CHECK(why&&!strcmp(why,"this host plays one channel"),
          "wrong channels: got %s",why?why:"(accepted)");
    printf("%-32s refused: %s\n","wrong channel count",why);
    free(f.data);
}

// wav_scan() refuses anything under 44 bytes outright ("not a WAV file"),
// which is a real RIFF+fmt+data's minimum size and not the thing this case is
// after -- so both of these pad with an unrelated chunk to clear that floor
// without supplying the chunk that is actually missing.
static void case_missing_fmt(void) {
    buf_t f=riff_start();
    uint8_t pcm[4]={1,2,3,4};
    put_chunk(&f,"data",pcm,sizeof pcm);
    uint8_t pad[24]; memset(pad,0,sizeof pad);
    put_chunk(&f,"JUNK",pad,sizeof pad);
    wav_scan_t r;
    const char *why=scan(&f,0,&r);
    CHECK(why&&!strcmp(why,"the file has no fmt chunk"),
          "missing fmt: got %s",why?why:"(accepted)");
    printf("%-32s refused: %s\n","missing fmt chunk",why);
    free(f.data);
}

static void case_missing_data(void) {
    buf_t f=riff_start();
    put_fmt(&f,1,1,RATE,2,16,false,0);
    uint8_t pad[16]; memset(pad,0,sizeof pad);
    put_chunk(&f,"JUNK",pad,sizeof pad);
    wav_scan_t r;
    const char *why=scan(&f,0,&r);
    CHECK(why&&!strcmp(why,"the file has no audio in it"),
          "missing data: got %s",why?why:"(accepted)");
    printf("%-32s refused: %s\n","missing data chunk",why);
    free(f.data);
}

int main(void) {
    printf("wav_scan.h: the header walk audio.playback's open() runs\n");
    case_pcm_minimal();
    case_ima_with_ext();
    case_junk_before_fmt();
    case_data_past_4k();
    case_odd_chunk_pad();
    case_truncated_chunk_header();
    case_chunk_past_eof();
    case_wrong_rate();
    case_wrong_channels();
    case_missing_fmt();
    case_missing_data();
    printf(failures?"FAILED (%d)\n":"OK (%d failures)\n",failures);
    return failures?1:0;
}
