#ifndef SHIKI_HEAP_H
#define SHIKI_HEAP_H
/* The Shiki heap (docs/kasane/shiki-spec.md sec.9.2, docs/kasane/shiki.md
 * sec.4.3): one contiguous region taken when a session whose manifest names
 * kasane.shiki starts, while the heap is still clean, and carved by a heap
 * of its own. Frames, the engine's state, programs and data regions of such
 * a session come from it; a session without kasane.shiki has no region and
 * its allocations go to the system heap as before.
 *
 * The region is given back only when it is empty and the session is over:
 * shiki_heap_end() at app_stop() frees it, or, if something still lives in it
 * (a surface the presenter would not let go of yet), marks it closing, and
 * the last shiki_heap_free() frees it then. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The default budget, decided after measuring (sec.9.2: 48 KiB). */
#define SHIKI_HEAP_DEFAULT_BYTES (48u*1024u)

/* Takes the region (bytes, 0 = the default). false when it cannot be had in
 * one piece, or a previous session's region is still closing. */
bool shiki_heap_begin(size_t bytes);
/* The session is over (see above). */
void shiki_heap_end(void);
/* A region is open for the running session. */
bool shiki_heap_active(void);
void *shiki_heap_malloc(size_t n);
void *shiki_heap_calloc(size_t count,size_t n);
/* 16-byte aligned (data regions, sec.5.1). */
void *shiki_heap_aligned(size_t n);
void shiki_heap_free(void *p);
bool shiki_heap_owns(const void *p);
/* shiki.heap(): the region's size, what is allocated, the largest free
 * block. Zeros without a region. */
void shiki_heap_info(uint32_t *bytes,uint32_t *used,uint32_t *largest);
/* The region itself (NULL, 0 without one): for memory diagnostics. */
void shiki_heap_region(const void **p,size_t *bytes);
/* Spare room for a user that can give its blocks back (the JIT's code,
 * docs/kasane/shiki-jit.md sec.15): 16-byte aligned, and only while the
 * largest free block left after it is at least keep bytes. The region's own
 * users come first: when one of their allocations fails, the reclaim hook
 * is called (it frees spare blocks) and the allocation is tried once more. */
void *shiki_heap_spare(size_t n,size_t keep);
void shiki_heap_set_reclaim(void (*reclaim)(void));
/* Gives back the end of a block (n bytes stay). The block normally keeps its
 * address; the result is where it is now (its first n bytes kept). */
void *shiki_heap_shrink(void *p,size_t n);

/* The allocators for code that serves both kinds of session: the region when
 * one is open, the system heap otherwise; free goes wherever the block came
 * from. */
void *shiki_or_sys_malloc(size_t n);
void *shiki_or_sys_calloc(size_t count,size_t n);
void shiki_or_sys_free(void *p);
#endif
