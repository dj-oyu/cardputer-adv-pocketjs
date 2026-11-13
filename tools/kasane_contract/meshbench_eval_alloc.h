/* Host guest-only allocation measurements with TLSF-style usable lengths.
 * Four-byte rounding and a 12-byte minimum mirror guest.c, but pointers and
 * JSValues remain host-sized. libc supplies storage: this does NOT simulate
 * QuickJS per-block quota overhead, target fragmentation, the small-block cache, physical RAM, or TLSF metadata.
 * The optional single-request ceiling is a regression stressor, not a device
 * largest-free-block measurement. Only the startup JS_Eval window is tracked. */
#ifndef MESHBENCH_EVAL_ALLOC_H
#define MESHBENCH_EVAL_ALLOC_H
#include <stddef.h>
#include <stdint.h>
typedef union {max_align_t alignment;size_t bytes;} mb_header;
static size_t mb_live,mb_peak,mb_max_request,mb_ceiling,mb_rejected;
static bool mb_tracking;
static size_t mb_length(size_t n){return n<12?12:(n+3)&~(size_t)3;}
static bool mb_request(size_t n){
 if(!mb_tracking)return true;
 if(n>mb_max_request)mb_max_request=n;
 if(mb_ceiling&&n>mb_ceiling){mb_rejected++;return false;}
 return true;
}
static void mb_charge(size_t old,size_t n){mb_live=mb_live-old+n;if(mb_tracking&&mb_live>mb_peak)mb_peak=mb_live;}
static size_t mb_usable(const void *p){return p?((const mb_header*)p-1)->bytes:0;}
static void *mb_malloc(void *opaque,size_t n){
 (void)opaque;
 if(!n||n>SIZE_MAX-sizeof(mb_header)-3||!mb_request(n))return NULL;
 size_t length=mb_length(n);mb_header*h=malloc(sizeof *h+length);if(!h)return NULL;
 h->bytes=length;mb_charge(0,length);return h+1;
}
static void mb_free(void *opaque,void*p){(void)opaque;if(!p)return;mb_header*h=(mb_header*)p-1;mb_charge(h->bytes,0);free(h);}
static void *mb_calloc(void *opaque,size_t count,size_t n){
 if(n&&count>SIZE_MAX/n)return NULL;
 void*p=mb_malloc(opaque,count*n);if(p)memset(p,0,count*n);return p;
}
static void *mb_realloc(void *opaque,void*p,size_t n){
 if(!p)return mb_malloc(opaque,n);
 if(!n){mb_free(opaque,p);return NULL;}
 if(n>SIZE_MAX-sizeof(mb_header)-3||!mb_request(n))return NULL;
 mb_header*h=(mb_header*)p-1;size_t old=h->bytes,length=mb_length(n);
 mb_header*q=realloc(h,sizeof *q+length);if(!q)return NULL;
 q->bytes=length;mb_charge(old,length);return q+1;
}
static const JSMallocFunctions MB_ALLOC={mb_calloc,mb_malloc,mb_free,mb_realloc,mb_usable};
#endif
