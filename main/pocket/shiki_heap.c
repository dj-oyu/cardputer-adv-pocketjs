// The Shiki heap: shiki_heap.h. One task (the UI task) uses it; no lock.
#include "shiki_heap.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "multi_heap.h"
typedef multi_heap_handle_t region_heap;
static void *sys_region(size_t n){return heap_caps_malloc(n,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);}
static void sys_region_free(void *p){heap_caps_free(p);}
#else
// Host tests: a first-fit list over the region, enough to check budgets,
// alignment and ownership without IDF.
typedef struct hblk { size_t off,size; struct hblk *next; } hblk;
typedef struct { uint8_t *base; size_t size; hblk *used; } *region_heap;
static void *sys_region(size_t n){return malloc(n);}
static void sys_region_free(void *p){free(p);}
#endif

static void *region;
static size_t region_bytes;
static region_heap heap;
static bool closing;

#ifdef ESP_PLATFORM
static region_heap heap_open(void *p,size_t n){return multi_heap_register(p,n);}
static void *heap_alloc(size_t n,size_t align){
    return align>4?multi_heap_aligned_alloc(heap,n,align):multi_heap_malloc(heap,n);
}
static void heap_release(void *p){multi_heap_free(heap,p);}
static void heap_stats(uint32_t *used,uint32_t *largest){
    multi_heap_info_t info;
    multi_heap_get_info(heap,&info);
    *used=(uint32_t)info.total_allocated_bytes;
    *largest=(uint32_t)info.largest_free_block;
}
#else
static region_heap heap_open(void *p,size_t n){
    region_heap h=calloc(1,sizeof *h);
    if(h){h->base=p;h->size=n;}
    return h;
}
static void *heap_alloc(size_t n,size_t align){
    if(!n)n=1;
    if(align<_Alignof(max_align_t))align=_Alignof(max_align_t);   // 8-byte pointers here
    size_t at=0;
    for(;;){
        at=(at+align-1)&~(align-1);
        hblk *hit=NULL;
        for(hblk *b=heap->used;b;b=b->next)if(at<b->off+b->size&&b->off<at+n)hit=b;
        if(!hit)break;
        at=hit->off+hit->size;
    }
    if(at+n>heap->size)return NULL;
    hblk *b=malloc(sizeof *b);
    if(!b)return NULL;
    *b=(hblk){at,n,heap->used};
    heap->used=b;
    return heap->base+at;
}
static void heap_release(void *p){
    for(hblk **b=&heap->used;*b;b=&(*b)->next)if(heap->base+(*b)->off==p){
        hblk *d=*b;*b=d->next;free(d);return;
    }
}
static void heap_stats(uint32_t *used,uint32_t *largest){
    size_t u=0,best=0,at=0;
    for(;;){                                     // gaps in address order
        hblk *next=NULL;
        for(hblk *b=heap->used;b;b=b->next)if(b->off>=at&&(!next||b->off<next->off))next=b;
        const size_t end=next?next->off:heap->size;
        if(end-at>best)best=end-at;
        if(!next)break;
        u+=next->size;at=next->off+next->size;
    }
    *used=(uint32_t)u;*largest=(uint32_t)best;
}
#endif

static bool empty(void){
    uint32_t used=0,largest=0;
    heap_stats(&used,&largest);
    return used==0;
}
static void close_region(void){
#ifndef ESP_PLATFORM
    free(heap);
#endif
    sys_region_free(region);
    region=NULL;heap=NULL;region_bytes=0;closing=false;
}

bool shiki_heap_begin(size_t bytes){
    if(region)return false;                     // the previous one is still closing
    if(!bytes)bytes=SHIKI_HEAP_DEFAULT_BYTES;
    region=sys_region(bytes);
    if(!region)return false;
    heap=heap_open(region,bytes);
    if(!heap){sys_region_free(region);region=NULL;return false;}
    region_bytes=bytes;closing=false;
#ifdef ESP_PLATFORM
    ESP_LOGI("shiki","SHIKI_HEAP open bytes=%u",(unsigned)bytes);
#endif
    return true;
}
void shiki_heap_end(void){
    if(!region)return;
    if(empty())close_region();
    else closing=true;
}
bool shiki_heap_active(void){return region&&!closing;}
bool shiki_heap_owns(const void *p){
    return region&&(const uint8_t *)p>=(const uint8_t *)region&&
           (const uint8_t *)p<(const uint8_t *)region+region_bytes;
}
void *shiki_heap_malloc(size_t n){return shiki_heap_active()?heap_alloc(n,4):NULL;}
void *shiki_heap_calloc(size_t count,size_t n){
    if(n&&count>SIZE_MAX/n)return NULL;
    void *p=shiki_heap_malloc(count*n);
    if(p)memset(p,0,count*n);
    return p;
}
void *shiki_heap_aligned(size_t n){return shiki_heap_active()?heap_alloc(n,16):NULL;}
void shiki_heap_free(void *p){
    if(!p||!shiki_heap_owns(p))return;
    heap_release(p);
    if(closing&&empty())close_region();
}
void shiki_heap_info(uint32_t *bytes,uint32_t *used,uint32_t *largest){
    *bytes=*used=*largest=0;
    if(!region)return;
    *bytes=(uint32_t)region_bytes;
    heap_stats(used,largest);
}
void *shiki_or_sys_malloc(size_t n){return shiki_heap_active()?shiki_heap_malloc(n):malloc(n);}
void *shiki_or_sys_calloc(size_t count,size_t n){
    return shiki_heap_active()?shiki_heap_calloc(count,n):calloc(count,n);
}
void shiki_or_sys_free(void *p){
    if(shiki_heap_owns(p))shiki_heap_free(p);
    else free(p);
}
