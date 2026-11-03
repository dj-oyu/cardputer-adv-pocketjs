/* The Shiki region's spare room (main/pocket/shiki_heap.h, docs/kasane/
 * shiki-jit.md sec.15): a spare block is refused when it would leave less
 * than keep bytes in one piece, and when one of the region's own
 * allocations fails, the reclaim hook gives spare blocks back, one call at
 * a time, until the allocation fits (sec.21.4). */
#include "shiki_heap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);fails++;}}while(0)

static void *spare[8];
static unsigned nspare,reclaims;
/* Gives back one spare block a call, the last taken first. */
static bool reclaim(void){
    if(!nspare)return false;
    reclaims++;
    shiki_heap_free(spare[--nspare]);
    return true;
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
    CHECK(reclaims==1&&nspare==1);                 /* one block was enough */
    /* A success does not call the hook. */
    void *small=shiki_heap_malloc(16);
    CHECK(small!=NULL&&reclaims==1);
    shiki_heap_free(small);shiki_heap_free(own2);
    while(nspare)shiki_heap_free(spare[--nspare]);
    /* Shrinking gives the end back: the block keeps its address and its
     * first bytes, and the freed tail is room again. */
    uint8_t *big=shiki_heap_spare(4096,0);
    CHECK(big!=NULL);
    for(unsigned k=0;k<64;k++)big[k]=(uint8_t)k;
    shiki_heap_info(&bytes,&used,&largest);
    const uint32_t before=used;
    uint8_t *small2=shiki_heap_shrink(big,64);
    CHECK(small2==big);
    for(unsigned k=0;k<64;k++)CHECK(small2[k]==(uint8_t)k);
    shiki_heap_info(&bytes,&used,&largest);
    CHECK(used+4096-64==before);
    shiki_heap_free(small2);
    shiki_heap_free(own);
    /* sec.21: owners from the bottom, spare users from the top, so the two
     * do not interleave; freed memory comes back zero. */
    {
        const void *base;size_t rb;
        shiki_heap_region(&base,&rb);
        uint8_t *o1=shiki_heap_malloc(1000),*s1=shiki_heap_spare(1000,0),*o2=shiki_heap_malloc(1000);
        CHECK(o1&&s1&&o2);
        CHECK(o1==(const uint8_t *)base);              /* the lowest place */
        CHECK(o2>o1&&o2<s1);                           /* owners stay below */
        CHECK(s1+1000+16>(const uint8_t *)base+rb);    /* the highest place */
        memset(o1,0xa5,1000);memset(s1,0x5a,1000);
        shiki_heap_free(o1);shiki_heap_free(s1);
        uint8_t *z=shiki_heap_calloc(1,1000);
        CHECK(z==(const uint8_t *)base);
        unsigned nz=0;for(unsigned k=0;k<1000;k++)nz+=z[k]!=0;
        CHECK(nz==0);
        /* A shrunk block's tail comes back zero too. */
        memset(z,0x11,1000);
        CHECK(shiki_heap_shrink(z,100)==z);
        uint8_t *after=shiki_heap_malloc(800);
        CHECK(after>=z+100&&after<z+100+16);         /* right after the kept part */
        nz=0;for(unsigned k=0;k<800;k++)nz+=after[k]!=0;
        CHECK(nz==0);
        shiki_heap_free(after);shiki_heap_free(z);
        /* A spare block shrunk upward keeps its end: its first bytes move
         * up, 16-aligned, the head comes back zero, and no hole is left
         * under the block above it. */
        uint8_t *top=shiki_heap_spare(512,0),*sp=shiki_heap_spare(1024,0);
        CHECK(top&&sp&&sp+1024<=top);
        for(unsigned k=0;k<100;k++)sp[k]=(uint8_t)(k+1);
        uint8_t *up=shiki_heap_shrink_up(sp,100,16);
        CHECK(up>sp&&((uintptr_t)up&15u)==0&&up+112>=sp+1024&&up+100<=sp+1024);
        for(unsigned k=0;k<100;k++)CHECK(up[k]==(uint8_t)(k+1));
        nz=0;for(uint8_t *q=sp;q<up;q++)nz+=*q!=0;
        CHECK(nz==0);
        shiki_heap_free(up);shiki_heap_free(top);
        shiki_heap_free(o2);
        uint32_t b2,u2,l2;
        shiki_heap_info(&b2,&u2,&l2);
        CHECK(u2==0&&l2==b2);                          /* nothing left, one piece */
    }
    shiki_heap_set_reclaim(NULL);
    shiki_heap_end();
    CHECK(!shiki_heap_active());
    if(fails)return 1;
    printf("SHIKI HEAP SPARE PASS\n");
    return 0;
}
