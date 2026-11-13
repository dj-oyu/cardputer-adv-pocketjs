#include "shiki_heap.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 assert(shiki_heap_begin(49152));
 void *p=shiki_heap_malloc(480);assert(p);
 shiki_heap_end();
 assert(!shiki_heap_begin(49152));
 shiki_heap_free(p);
 assert(shiki_heap_begin(49152));
 unsigned b,u,l;shiki_heap_info(&b,&u,&l);
 assert(b==49152&&u==0&&l==49152);
 shiki_heap_end();assert(!shiki_heap_active());
 puts("closing rejects restart; last free permits clean restart: PASS");
}
