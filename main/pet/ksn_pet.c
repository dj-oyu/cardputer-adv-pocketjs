#include "ksn_pet.h"
#include "pet_pixels.h"

#ifdef KSN_PET_ROW_STATS
/* Host-only counters for the contract tests (ksn_pet.h). The firmware build
 * does not define KSN_PET_ROW_STATS, so the shipped provider carries none. */
unsigned long long g_ksn_pet_fetches,g_ksn_pet_decodes;
#define COUNT_FETCH() (g_ksn_pet_fetches++)
#define COUNT_DECODE() (g_ksn_pet_decodes++)
#else
#define COUNT_FETCH() ((void)0)
#define COUNT_DECODE() ((void)0)
#endif

/* Decoded-row cache. A rotated span walks the source rows in sweeps: the
 * 64x64 source window of the contract below is 64 rows, and the rotated scene
 * asks for 256 to 3,874 fetches per frame (17,590 over seven angles) while the
 * 120-frame animated transform track makes 330,420 fetches over them. Answering
 * from one decoded row (128 B, the first cut) covers 5% of those requests,
 * sixteen rows 15%, thirty-two 34%: a sweep re-visits every row of the image,
 * so the working set is the whole source and the smallest cache that removes
 * the duplicate decoding is all 64 rows (8,192 B). Counts and the byte cost are
 * in docs/perf/kasane-pet-row-cache.md. The face decoder is applied when a row
 * is filled, so the mood is part of the key.
 *
 * Staleness: the key is the borrowed image bytes, the variant and the frame. A
 * request whose key differs from the cached one drops every row before the next
 * fill, and a row is readable only while its bit is set in `valid`; a different
 * row is a different slot. The borrowed PPT2 bytes are immutable for the life
 * of this port (ksn_pet.h), so no other event can make a row stale. Worst case
 * (a key that changes every request) is the uncached path, which decodes
 * exactly what it would have decoded anyway.
 * Owner task only; no heap, no I/O, no owner mutation. */
enum { CACHE_ROWS=64 };
static uint16_t cache_rows[CACHE_ROWS][64];
static uint64_t cache_valid;
static const uint8_t *cache_data;
static uint16_t cache_pet,cache_mood;
bool g_ksn_pet_row_cache=true;

/* The one place a row is decoded, so a fetch and a decode stay comparable. */
static void decode_row(const uint8_t *data,unsigned pet,unsigned y,unsigned mood,uint16_t row[64]){
    COUNT_DECODE();
    pet_pixels_row(data,pet,y,row);pet_pixels_face(pet,y,mood,row);
}

/* Returns the decoded row of the slot key, filling it on a miss. The pointer
 * stays valid until the next key change. */
static const uint16_t *cached_row(void *ctx,uint16_t pet,uint16_t mood,uint16_t y){
    if(ctx!=cache_data||pet!=cache_pet||mood!=cache_mood){
        cache_data=ctx;cache_pet=pet;cache_mood=mood;cache_valid=0;
    }
    uint16_t *row=cache_rows[y];
    if(!((cache_valid>>y)&1u)){
        decode_row(ctx,pet,y,mood,row);
        cache_valid|=1ull<<y;
    }
    return row;
}

static ksn_result read_span(void *ctx,uint16_t pet,uint16_t mood,uint16_t y,uint16_t x,
                            uint16_t count,uint16_t *rgb,uint8_t *alpha){
    if(!ctx||pet>=12||mood>=6||y>=64||x>64||count>64-x||
       (count&&(!rgb||!alpha)))return KSN_INVALID;
    if(!count)return KSN_OK;
    COUNT_FETCH();
    /* Function scope, not the else block: the row is read after the branch. */
    uint16_t decoded[64]; // 128-byte provider row, included in compositor scratch.
    const uint16_t *row;
    if(g_ksn_pet_row_cache)row=cached_row(ctx,pet,mood,y);
    else{decode_row(ctx,pet,y,mood,decoded);row=decoded;}
    for(unsigned i=0;i<count;i++){
        unsigned v=row[x+i],r=v&15,g=(v>>4)&15,b=(v>>8)&15;
        rgb[i]=(uint16_t)(((r<<1|r>>3)<<11)|((g<<2|g>>2)<<5)|(b<<1|b>>3));
        alpha[i]=(uint8_t)((v>>12)*17);
    }
    return KSN_OK;
}
ksn_result ksn_pet_image(const uint8_t *data,size_t bytes,ksn_image_port *out){
    if(!out||!pet_pixels_valid(data,bytes))return KSN_INVALID;
    *out=(ksn_image_port){(void *)data,64,64,12,6,read_span};return KSN_OK;
}
