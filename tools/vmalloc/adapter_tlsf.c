// adapter_tlsf.c — wraps vendor/{multi_heap.c,tlsf/tlsf.c}, the actual
// ESP-IDF v6.0.1 heap component sources (components/heap/{multi_heap.c,
// tlsf/tlsf.c,tlsf/tlsf_block_functions.h,tlsf/tlsf_control_functions.h}),
// copied verbatim except for three small guarded patches explained in
// vendor/tlsf/tlsf.c and vendor/tlsf/tlsf_control_functions.h. Read those
// comments before trusting any number from this backend.
//
// Device path this models: guest.c's heap_caps_malloc() ultimately calls
// multi_heap_malloc() on the region esp_heap_caps_init() registered from
// the linker's free DRAM, i.e. exactly multi_heap_register() +
// multi_heap_malloc/realloc/free on one contiguous pool, which is what
// this adapter does.
#include "vmalloc.h"
#include "vendor/include/multi_heap.h"
#include <string.h>

static multi_heap_handle_t g_heap;

static int tlsf_init(void *pool, size_t pool_size) {
  g_heap = multi_heap_register(pool, pool_size);
  return g_heap ? 0 : -1;
}

// TLSF's malloc path is a fixed number of clz/bitmap lookups to find a
// non-empty free-list bucket, then an O(1) pop of that bucket's head - no
// per-block scan happens (tlsf.c:593-601 in the vendored copy is estalloc's
// fallback, not TLSF's; TLSF has no equivalent loop). "steps" is reported
// as 1 uniformly to mean exactly that: structurally step-free, not "found
// on the first free block" the way a 1 from naive or estalloc would mean.
static void *tlsf_do_malloc(size_t size, unsigned *out_steps) {
  if (out_steps) *out_steps = 1;
  return multi_heap_malloc(g_heap, size);
}

static void *tlsf_do_realloc(void *ptr, size_t size, unsigned *out_steps) {
  if (out_steps) *out_steps = 1;
  return multi_heap_realloc(g_heap, ptr, size);
}

static void tlsf_do_free(void *ptr, unsigned *out_steps) {
  if (out_steps) *out_steps = 1;
  multi_heap_free(g_heap, ptr);
}

static void tlsf_stats(vmalloc_stats_t *out) {
  multi_heap_info_t info;
  multi_heap_get_info(g_heap, &info);
  out->used_bytes = info.total_allocated_bytes;
  out->free_bytes = info.total_free_bytes;
  out->largest_free_block = info.largest_free_block;
  out->blocks_used = info.allocated_blocks;
}

static int tlsf_do_check(void) { return multi_heap_check(g_heap, true) ? 1 : 0; }

static const vmalloc_backend_t BACKEND = {
    .name = "tlsf",
    .init = tlsf_init,
    .do_malloc = tlsf_do_malloc,
    .do_realloc = tlsf_do_realloc,
    .do_free = tlsf_do_free,
    .stats = tlsf_stats,
    .check = tlsf_do_check,
};

const vmalloc_backend_t *vmalloc_tlsf_backend(void) { return &BACKEND; }
