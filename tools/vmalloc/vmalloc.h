// vmalloc.h — common interface the trace replayer drives the candidate
// allocators (TLSF/multi_heap, estalloc, naive first-fit, and the L2a-style
// non-moving segment allocator) through. One process, one pool, one
// allocator selected at startup (not linked concurrently) so each backend's
// global/static state never collides with another's.
//
// "steps" is a best-effort per-call search-cost counter: exact for naive
// (free-list nodes visited), estalloc (FLI/SLI/bitmap hits count 1,
// first-fit fallback counts 1 per node) and segment (segments + free blocks
// visited), structurally 0-or-fixed for TLSF (its malloc path is O(1)
// bitmap lookups with no per-block scan — see
// docs/vm-ledger/06-allocator-baseline.md). Wall-clock ns is measured for
// all of them the same way regardless, since it is the only comparable unit
// when one candidate has no scan to count.
#ifndef VMALLOC_H
#define VMALLOC_H

#include <stddef.h>

typedef struct {
  size_t used_bytes;         // application bytes + allocator's own per-block overhead, currently live
  size_t free_bytes;         // pool_size - used_bytes - (any allocator-fixed overhead not counted in used_bytes)
  size_t largest_free_block; // largest single free extent an app malloc could use
  size_t blocks_used;

  // Segment-style backends only. The replayer zeroes the struct before
  // calling stats(), so backends without segments simply leave these 0.
  // The split below is what spec sec.7 asks for: "internal slack" (bytes a
  // segment holds but no block uses) and "external fragmentation" (pool
  // bytes no segment can be carved from) reported as separate quantities,
  // never summed into one "overhead" figure.
  size_t used_payload_bytes; // sum of payload capacities of live blocks (rounding waste = this - requested)
  size_t reserved_bytes;     // pool bytes held by segments (live + cached), headers included
  size_t seg_header_bytes;   // part of reserved_bytes that is segment descriptors
  size_t seg_free_inside;    // free-block bytes inside live segments: internal slack, reusable only from inside
  size_t seg_cached_bytes;   // empty segments retained by the cache instead of returned: internal slack of a second kind
  size_t pool_free_bytes;    // pool bytes held by no segment
  size_t pool_largest_free;  // largest contiguous pool extent: whether one more standard segment fits
  size_t segments_live, segments_cached, segments_dedicated;
  // Running event counter and its breakdown. The replayer's --verify sweeps
  // every live block whenever `events` changes, which is how "the check
  // crossed a segment add / boundary / return" becomes something the output
  // can show rather than assert.
  unsigned long events;
  unsigned long seg_added, seg_returned, seg_reused, seg_dedicated_added;
} vmalloc_stats_t;

typedef struct {
  const void *base;
  size_t size;
  int kind;            // 0 standard, 1 dedicated (single oversized block), 2 cached (empty, retained)
  size_t live_blocks;
} vmalloc_seg_t;

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
  // if the backend has none (never true for ours).
  int (*check)(void);

  // Optional, NULL when the backend has no notion of segments.
  // owner(): 1 and a descriptor if p lies inside a segment (any kind), else 0.
  int (*owner)(const void *p, vmalloc_seg_t *out);
  // segment_at(): enumerate segments in address order; 1 if i is in range.
  int (*segment_at)(size_t i, vmalloc_seg_t *out);
  // events(): O(1) read of the running add/return/reuse counter (the same
  // value stats() reports as `events`), polled by --verify after every op.
  unsigned long (*events)(void);
  // configure(): "seg_size" / "cache" / "fault" tuning before init(). Returns
  // 0 on success, -1 on an unknown key or value.
  int (*configure)(const char *key, const char *value);
} vmalloc_backend_t;

const vmalloc_backend_t *vmalloc_tlsf_backend(void);
const vmalloc_backend_t *vmalloc_estalloc_backend(void);
const vmalloc_backend_t *vmalloc_naive_backend(void);
const vmalloc_backend_t *vmalloc_segment_backend(void);

#endif
