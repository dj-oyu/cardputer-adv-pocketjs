// vmalloc.h — common interface the trace replayer drives three candidate
// allocators (TLSF/multi_heap, estalloc, naive first-fit) through. One
// process, one pool, one allocator selected at startup (not linked
// concurrently) so each backend's global/static state never collides with
// another's.
//
// "steps" is a best-effort per-call search-cost counter: exact for naive
// (free-list nodes visited) and estalloc (FLI/SLI/bitmap hits count 1,
// first-fit fallback counts 1 per node), structurally 0-or-fixed for TLSF
// (its malloc path is O(1) bitmap lookups with no per-block scan — see
// docs/vm-ledger/06-allocator-baseline.md). Wall-clock ns is measured for
// all three the same way regardless, since it is the only comparable unit
// when one candidate has no scan to count.
#ifndef VMALLOC_H
#define VMALLOC_H

#include <stddef.h>

typedef struct {
  size_t used_bytes;         // application bytes + allocator's own per-block overhead, currently live
  size_t free_bytes;         // pool_size - used_bytes - (any allocator-fixed overhead not counted in used_bytes)
  size_t largest_free_block; // largest single free extent an app malloc could use
  size_t blocks_used;
} vmalloc_stats_t;

typedef struct {
  const char *name;
  // Returns 0 on success. pool must be aligned to 16 (satisfies every
  // backend's own alignment requirement, checked below).
  int (*init)(void *pool, size_t pool_size);
  // Every op below also increments *out_steps by this call's search cost
  // (see file comment) if out_steps != NULL.
  void *(*do_malloc)(size_t size, unsigned *out_steps);
  void *(*do_realloc)(void *ptr, size_t size, unsigned *out_steps);
  void (*do_free)(void *ptr, unsigned *out_steps);
  void (*stats)(vmalloc_stats_t *out);
  // Whole-pool structural integrity check; returns 1 ok / 0 corrupt. NULL
  // if the backend has none (never true for our three).
  int (*check)(void);
} vmalloc_backend_t;

const vmalloc_backend_t *vmalloc_tlsf_backend(void);
const vmalloc_backend_t *vmalloc_estalloc_backend(void);
const vmalloc_backend_t *vmalloc_naive_backend(void);

#endif
