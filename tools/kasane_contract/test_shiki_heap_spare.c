/* The Shiki region's spare room (main/pocket/shiki_heap.h, docs/kasane/
 * shiki-jit.md sec.15): a spare block is refused when it would leave less
 * than keep bytes in one piece, and when one of the region's own
 * allocations fails, the reclaim hook gives spare blocks back and the
 * allocation is tried once more. */
#include "shiki_heap.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);fails++;}}while(0)

static void *spare[8];
static unsigned nspare,reclaims;
static void reclaim(void){
    reclaims++;
    for(unsigned k=0;k<nspare;k++)shiki_heap_free(spare[k]);
    nspare=0;
}

int main(void){
    CHECK(shiki_heap_spare(64,0)==NULL);              /* no region */
    CHECK(shiki_heap_begin(16384));
    void *own=shiki_heap_malloc(8192);
    CHECK(own!=NULL);
    /* About 8 KB is left: 2 KB with 4 KB kept is fine, 6 KB is not. */
    CHECK((spare[nspare++]=shiki_heap_spare(2048,4096))!=NULL);
    CHECK(((uintptr_t)spare[0]&15u)==0);
    CHECK(shiki_heap_spare(6144,4096)==NULL);
    CHECK((spare[nspare++]=shiki_heap_spare(3072,0))!=NULL);
    uint32_t bytes,used,largest;
    shiki_heap_info(&bytes,&used,&largest);
    CHECK(largest<4096);
    /* Without a hook the region's own allocation simply fails. */
    CHECK(shiki_heap_malloc(4096)==NULL);
    shiki_heap_set_reclaim(reclaim);
    void *own2=shiki_heap_malloc(4096);
    CHECK(own2!=NULL);
    CHECK(reclaims==1&&nspare==0);
    /* A success does not call the hook. */
    void *small=shiki_heap_malloc(16);
    CHECK(small!=NULL&&reclaims==1);
    shiki_heap_free(small);shiki_heap_free(own2);shiki_heap_free(own);
    shiki_heap_set_reclaim(NULL);
    shiki_heap_end();
    CHECK(!shiki_heap_active());
    if(fails)return 1;
    printf("SHIKI HEAP SPARE PASS\n");
    return 0;
}
