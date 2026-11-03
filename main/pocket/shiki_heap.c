// The Shiki heap: shiki_heap.h. One task (the UI task) uses it; no lock.
//
// The region is carved by a fixed table of blocks, the same code on the host
// and on the device (docs/kasane/shiki-jit.md sec.21). Not TLSF: Shiki's
// owners are few, sized when they are made and freed together at scene
// changes, and TLSF's size classes, block headers and aligned splits left
// DERBY's race with 7 KB free but 3.3 KB in one piece (sec.19.3).
//
// - The table holds every live block (start and length in words), sorted by
//   address. The region holds nothing but the blocks' contents.
// - The region's owners take the lowest gap that fits; spare users (the
//   JIT's code) take the highest, so the code that comes and goes does not
//   cut the owners' room into pieces.
// - Free memory is always zero: the region is zeroed once when it is taken,
//   and a freed block (or a shrunk block's tail) is zeroed when it is given
//   back. calloc costs nothing, and on the host every allocation checks that
//   its memory is still zero, so a write after free is caught there.
// - free() of an address that does not start a live block is refused (a
//   double free or a stranger), not passed on.
#include "shiki_heap.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
static void *sys_region(size_t n){return heap_caps_malloc(n,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);}
static void sys_region_free(void *p){heap_caps_free(p);}
#define HEAP_REFUSED(what,p) ESP_LOGE("shiki","SHIKI_HEAP_REFUSED %s %p",what,p)
#else
#include <stdio.h>
static void *sys_region(size_t n){return malloc(n);}
static void sys_region_free(void *p){free(p);}
// The host's tests run every allocation through the zero check; a refusal
// there is a bug in the caller, so it stops the test.
#define SHIKI_HEAP_CHECK 1
#define HEAP_REFUSED(what,p) do{fprintf(stderr,"SHIKI_HEAP_REFUSED %s %p\n",what,(void *)(p));abort();}while(0)
#endif
/* malloc's alignment, as the C library's: 8-byte members (int64_t, the
 * host's pointers) need it; a few bytes of padding a block. */
#define MALLOC_ALIGN _Alignof(max_align_t)

/* 128 blocks: 16 data regions, 8 lists, 32 programs, the surface's two
 * frames, the workspace and the VM state, and the JIT's code and weights
 * (two per program), with room left. Words of 4 bytes, so 16 bits address
 * 256 KiB. */
#define SLOTS 128u
typedef struct { uint16_t at,words; } blk;
static blk blocks[SLOTS];
static unsigned nblocks;

static uint8_t *region;
static size_t region_bytes;
static uint32_t region_words;
static bool closing;

static uint32_t words_of(size_t n){return (uint32_t)((n+3u)/4u);}
static uint8_t *addr(uint32_t w){return region+4u*w;}
/* The first word at or after w whose address is align-aligned. */
static uint32_t align_up(uint32_t w,size_t align){
    const uintptr_t a=(uintptr_t)addr(w),r=(a+align-1u)&~(uintptr_t)(align-1u);
    return w+(uint32_t)((r-a)/4u);
}
/* The last word at or before w whose address is align-aligned. */
static uint32_t align_down(uint32_t w,size_t align){
    const uintptr_t a=(uintptr_t)addr(w),r=a&~(uintptr_t)(align-1u);
    const uint32_t back=(uint32_t)((a-r)/4u);
    return back>w?UINT32_MAX:w-back;
}
/* The gap before block i (i==nblocks: the gap after the last). */
static uint32_t gap_start(unsigned i){return i?blocks[i-1].at+blocks[i-1].words:0u;}
static uint32_t gap_end(unsigned i){return i<nblocks?blocks[i].at:region_words;}

static void insert(unsigned i,uint32_t at,uint32_t words){
    memmove(&blocks[i+1],&blocks[i],(nblocks-i)*sizeof *blocks);
    blocks[i]=(blk){(uint16_t)at,(uint16_t)words};
    nblocks++;
}
static int find(const void *p){
    if(!region||(const uint8_t *)p<region||(const uint8_t *)p>=region+region_bytes)return -1;
    const uintptr_t off=(uintptr_t)((const uint8_t *)p-region);
    if(off&3u)return -1;
    unsigned lo=0,hi=nblocks;                 /* sorted by address */
    while(lo<hi){
        const unsigned mid=(lo+hi)/2u;
        if(blocks[mid].at<off/4u)lo=mid+1u;else hi=mid;
    }
    return lo<nblocks&&blocks[lo].at==off/4u?(int)lo:-1;
}
#ifdef SHIKI_HEAP_CHECK
static void check_zero(const uint8_t *p,size_t n){
    for(size_t k=0;k<n;k++)if(p[k]){HEAP_REFUSED("write after free at",p+k);return;}
}
#else
#define check_zero(p,n) ((void)0)
#endif
static void *place(unsigned i,uint32_t at,uint32_t words){
    insert(i,at,words);
    check_zero(addr(at),4u*words);
    return addr(at);
}
/* The lowest place that fits (the region's owners). */
static void *alloc_low(size_t n,size_t align){
    if(!n)n=1;
    const uint32_t words=words_of(n);
    if(nblocks>=SLOTS||!words||words>region_words)return NULL;
    for(unsigned i=0;i<=nblocks;i++){
        const uint32_t at=align_up(gap_start(i),align);
        if(at+words<=gap_end(i))return place(i,at,words);
    }
    return NULL;
}
/* The highest place that fits (spare users). */
static void *alloc_high(size_t n,size_t align){
    if(!n)n=1;
    const uint32_t words=words_of(n);
    if(nblocks>=SLOTS||!words||words>region_words)return NULL;
    for(unsigned i=nblocks+1u;i-->0;){
        const uint32_t s=gap_start(i),e=gap_end(i);
        if(e-s<words)continue;
        const uint32_t at=align_down(e-words,align);
        if(at!=UINT32_MAX&&at>=s)return place(i,at,words);
    }
    return NULL;
}
static void release(unsigned i){
    memset(addr(blocks[i].at),0,4u*blocks[i].words);
    memmove(&blocks[i],&blocks[i+1],(nblocks-i-1u)*sizeof *blocks);
    nblocks--;
}
static void stats(uint32_t *used,uint32_t *largest){
    uint32_t u=0,best=0;
    for(unsigned i=0;i<=nblocks;i++){
        const uint32_t g=gap_end(i)-gap_start(i);
        if(g>best)best=g;
        if(i<nblocks)u+=blocks[i].words;
    }
    *used=4u*u;*largest=4u*best;
}

static void close_region(void){
    sys_region_free(region);
    region=NULL;region_bytes=0;region_words=0;nblocks=0;closing=false;
}

bool shiki_heap_begin(size_t bytes){
    if(region)return false;                     // the previous one is still closing
    if(!bytes)bytes=SHIKI_HEAP_DEFAULT_BYTES;
    bytes&=~(size_t)3u;
    if(bytes/4u>UINT16_MAX)return false;        // the table's 16-bit words
    region=sys_region(bytes);
    if(!region)return false;
    memset(region,0,bytes);                     // once: free memory stays zero
    region_bytes=bytes;region_words=(uint32_t)(bytes/4u);nblocks=0;closing=false;
#ifdef ESP_PLATFORM
    ESP_LOGI("shiki","SHIKI_HEAP open bytes=%u",(unsigned)bytes);
#endif
    return true;
}
void shiki_heap_end(void){
    if(!region)return;
    if(!nblocks)close_region();
    else closing=true;
}
bool shiki_heap_active(void){return region&&!closing;}
void shiki_heap_region(const void **p,size_t *bytes){*p=region;*bytes=region?region_bytes:0;}
bool shiki_heap_owns(const void *p){
    return region&&(const uint8_t *)p>=region&&(const uint8_t *)p<region+region_bytes;
}
static void (*reclaim_hook)(void);
void shiki_heap_set_reclaim(void (*reclaim)(void)){reclaim_hook=reclaim;}
/* The region's own allocations: a failure first asks the spare users to
 * give their blocks back. */
static void *owned_alloc(size_t n,size_t align){
    if(!shiki_heap_active())return NULL;
    void *p=alloc_low(n,align);
    if(!p&&reclaim_hook){reclaim_hook();p=alloc_low(n,align);}
    return p;
}
void *shiki_heap_malloc(size_t n){return owned_alloc(n,MALLOC_ALIGN);}
void *shiki_heap_calloc(size_t count,size_t n){
    if(n&&count>SIZE_MAX/n)return NULL;
    return shiki_heap_malloc(count*n);          // free memory is zero
}
void *shiki_heap_aligned(size_t n){return owned_alloc(n,16);}
void *shiki_heap_shrink(void *p,size_t n){
    if(!shiki_heap_owns(p)||!n)return p;
    const int i=find(p);
    if(i<0){HEAP_REFUSED("shrink of",p);return NULL;}
    const uint32_t words=words_of(n);
    if(words<blocks[i].words){
        memset(addr(blocks[i].at+words),0,4u*(blocks[i].words-words));
        blocks[i].words=(uint16_t)words;
    }
    return p;
}
void *shiki_heap_spare(size_t n,size_t keep){
    if(!shiki_heap_active())return NULL;
    void *p=alloc_high(n,16);
    if(!p)return NULL;
    uint32_t used,largest;
    stats(&used,&largest);
    if(largest<keep){release((unsigned)find(p));return NULL;}
    return p;
}
void shiki_heap_free(void *p){
    if(!p||!shiki_heap_owns(p))return;
    const int i=find(p);
    if(i<0){HEAP_REFUSED("free of",p);return;}
    release((unsigned)i);
    if(closing&&!nblocks)close_region();
}
void shiki_heap_info(uint32_t *bytes,uint32_t *used,uint32_t *largest){
    *bytes=*used=*largest=0;
    if(!region)return;
    *bytes=(uint32_t)region_bytes;
    stats(used,largest);
}
void *shiki_or_sys_malloc(size_t n){return shiki_heap_active()?shiki_heap_malloc(n):malloc(n);}
void *shiki_or_sys_calloc(size_t count,size_t n){
    return shiki_heap_active()?shiki_heap_calloc(count,n):calloc(count,n);
}
void shiki_or_sys_free(void *p){
    if(shiki_heap_owns(p))shiki_heap_free(p);
    else free(p);
}
