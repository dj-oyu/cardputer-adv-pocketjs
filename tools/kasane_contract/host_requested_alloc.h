/* Host requested-byte accounting; not allocator overhead or device RAM. */
#ifndef HOST_REQUESTED_ALLOC_H
#define HOST_REQUESTED_ALLOC_H
#include <stddef.h>
#include <stdlib.h>
typedef struct {void *p;size_t n;} allocation;
static allocation ledger[32768];
static size_t live_bytes,peak_bytes,live_blocks,alloc_calls;
void *__real_malloc(size_t);void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);void __real_free(void *);
static size_t find(void *p){for(size_t k=0;k<32768;k++)if(ledger[k].p==p)return k;abort();}
static void add(void *p,size_t n){if(!p)return;size_t k=find(NULL);ledger[k]=(allocation){p,n};live_bytes+=n;live_blocks++;alloc_calls++;if(live_bytes>peak_bytes)peak_bytes=live_bytes;}
static void del(void *p){if(!p)return;size_t k=find(p);live_bytes-=ledger[k].n;live_blocks--;ledger[k]=(allocation){0};}
void *__wrap_malloc(size_t n){void *p=__real_malloc(n);add(p,n);return p;}
void *__wrap_calloc(size_t n,size_t z){void *p=__real_calloc(n,z);add(p,n*z);return p;}
void *__wrap_realloc(void *p,size_t n){
 size_t k=p?find(p):0,old=p?ledger[k].n:0;void *q=__real_realloc(p,n);
 if(!q&&n)return q;
 if(p){live_bytes-=old;live_blocks--;ledger[k]=(allocation){0};}add(q,n);return q;
}
void __wrap_free(void *p){del(p);__real_free(p);}
#endif
