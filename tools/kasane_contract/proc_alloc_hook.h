/* Force-included (-include) into test_pocket_proc_turn_qjs's own sources, not
 * into the prebuilt QuickJS objects: the engine keeps allocating normally while
 * pocket_proc.c's malloc/calloc can be made to fail or be measured. */
#ifndef PROC_ALLOC_HOOK_H
#define PROC_ALLOC_HOOK_H
#include <stddef.h>
#include <stdlib.h>
void *proc_test_malloc(size_t size);
void *proc_test_calloc(size_t count,size_t size);
#define malloc(n) proc_test_malloc(n)
#define calloc(n,s) proc_test_calloc(n,s)
#endif
