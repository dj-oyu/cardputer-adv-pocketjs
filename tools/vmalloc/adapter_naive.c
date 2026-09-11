// adapter_naive.c — the floor: an explicit doubly-linked free list, linear
// first-fit search, immediate coalescing of physically-adjacent free
// neighbors on free(). No segregated buckets, no bitmap, no boundary tags
// beyond what coalescing itself needs. Written for this comparison, not
// vendored from anywhere.
//
// Block layout, all sizes rounded up to 8:
//   [header][payload...]
//   header = { size_t size;      // payload capacity, header excluded
//              int used;
//              block_t *phys_prev, *phys_next;  // physical neighbors, for coalescing
//              block_t *free_prev, *free_next; } // free-list links, used only when !used
#include "vmalloc.h"
#include <stdint.h>
#include <string.h>

typedef struct block {
  size_t size;
  int used;
  struct block *phys_prev, *phys_next;
  struct block *free_prev, *free_next;
} block_t;

#define ALIGN8(x) (((x) + 7u) & ~(size_t)7u)
#define HDR sizeof(block_t)

static block_t *g_free_head; // unordered free list
static size_t g_pool_bytes;
static block_t *g_first, *g_last;

static void free_list_remove(block_t *b) {
  if (b->free_prev) b->free_prev->free_next = b->free_next; else g_free_head = b->free_next;
  if (b->free_next) b->free_next->free_prev = b->free_prev;
  b->free_prev = b->free_next = NULL;
}

static void free_list_push(block_t *b) {
  b->free_prev = NULL;
  b->free_next = g_free_head;
  if (g_free_head) g_free_head->free_prev = b;
  g_free_head = b;
  b->used = 0;
}

static int naive_init(void *pool, size_t pool_size) {
  if (pool_size < HDR) return -1;
  memset(pool, 0, sizeof(block_t)); // only the first block's header; rest is payload, untouched
  g_first = (block_t *)pool;
  g_first->size = pool_size - HDR;
  g_first->phys_prev = NULL;
  g_first->phys_next = NULL;
  g_last = g_first;
  g_pool_bytes = pool_size;
  g_free_head = NULL;
  free_list_push(g_first);
  return 0;
}

// Splits `b` (currently off the free list, used=1 already not yet set by
// caller) if the remainder is big enough to hold another header + 8 bytes.
static void maybe_split(block_t *b, size_t need) {
  if (b->size < need + HDR + 8) return;
  uint8_t *raw = (uint8_t *)b;
  block_t *rem = (block_t *)(raw + HDR + need);
  rem->size = b->size - need - HDR;
  rem->phys_prev = b;
  rem->phys_next = b->phys_next;
  if (rem->phys_next) rem->phys_next->phys_prev = rem;
  else g_last = rem;
  b->phys_next = rem;
  b->size = need;
  free_list_push(rem);
}

static void *naive_do_malloc(size_t size, unsigned *out_steps) {
  if (size == 0) size = 1;
  size_t need = ALIGN8(size);
  unsigned steps = 0;
  block_t *b = g_free_head;
  while (b) {
    steps++;
    if (b->size >= need) {
      free_list_remove(b);
      b->used = 1;
      maybe_split(b, need);
      if (out_steps) *out_steps = steps;
      return (uint8_t *)b + HDR;
    }
    b = b->free_next;
  }
  if (out_steps) *out_steps = steps;
  return NULL;
}

static block_t *coalesce(block_t *b) {
  // Merge with the physical next if it is free.
  if (b->phys_next && !b->phys_next->used) {
    block_t *n = b->phys_next;
    free_list_remove(n);
    b->size += HDR + n->size;
    b->phys_next = n->phys_next;
    if (b->phys_next) b->phys_next->phys_prev = b;
    else g_last = b;
  }
  // Merge with the physical prev if it is free.
  if (b->phys_prev && !b->phys_prev->used) {
    block_t *p = b->phys_prev;
    free_list_remove(p);
    p->size += HDR + b->size;
    p->phys_next = b->phys_next;
    if (p->phys_next) p->phys_next->phys_prev = p;
    else g_last = p;
    b = p;
  }
  return b;
}

static void naive_do_free(void *ptr, unsigned *out_steps) {
  if (out_steps) *out_steps = 0;
  if (!ptr) return;
  block_t *b = (block_t *)((uint8_t *)ptr - HDR);
  b = coalesce(b);
  free_list_push(b);
}

static void *naive_do_realloc(void *ptr, size_t size, unsigned *out_steps) {
  if (!ptr) return naive_do_malloc(size, out_steps);
  if (size == 0) { naive_do_free(ptr, out_steps); return NULL; }
  block_t *b = (block_t *)((uint8_t *)ptr - HDR);
  size_t need = ALIGN8(size);
  if (b->size >= need) { if (out_steps) *out_steps = 0; maybe_split(b, need); return ptr; }
  // Try growing in place into a free physical-next neighbor before moving.
  if (b->phys_next && !b->phys_next->used && b->size + HDR + b->phys_next->size >= need) {
    block_t *n = b->phys_next;
    free_list_remove(n);
    b->size += HDR + n->size;
    b->phys_next = n->phys_next;
    if (b->phys_next) b->phys_next->phys_prev = b;
    else g_last = b;
    maybe_split(b, need);
    if (out_steps) *out_steps = 0;
    return ptr;
  }
  unsigned steps = 0;
  void *n = naive_do_malloc(size, &steps);
  if (!n) { if (out_steps) *out_steps = steps; return NULL; }
  memcpy(n, ptr, b->size < need ? b->size : need);
  naive_do_free(ptr, NULL);
  if (out_steps) *out_steps = steps;
  return n;
}

static void naive_stats(vmalloc_stats_t *out) {
  size_t used = 0, free_b = 0, largest = 0, blocks = 0;
  for (block_t *b = g_first; b; b = b->phys_next) {
    if (b->used) { used += HDR + b->size; blocks++; }
    else { free_b += HDR + b->size; if (b->size > largest) largest = b->size; }
  }
  out->used_bytes = used;
  out->free_bytes = free_b;
  out->largest_free_block = largest;
  out->blocks_used = blocks;
}

static int naive_check(void) {
  size_t total = 0;
  block_t *prev = NULL;
  for (block_t *b = g_first; b; b = b->phys_next) {
    if (b->phys_prev != prev) return 0;
    if (!b->used) {
      // must be reachable from g_free_head
      int found = 0;
      for (block_t *f = g_free_head; f; f = f->free_next) if (f == b) { found = 1; break; }
      if (!found) return 0;
      // no two adjacent free blocks (coalescing invariant)
      if (b->phys_next && !b->phys_next->used) return 0;
    }
    total += HDR + b->size;
    prev = b;
  }
  return total == g_pool_bytes;
}

static const vmalloc_backend_t BACKEND = {
    .name = "naive",
    .init = naive_init,
    .do_malloc = naive_do_malloc,
    .do_realloc = naive_do_realloc,
    .do_free = naive_do_free,
    .stats = naive_stats,
    .check = naive_check,
};

const vmalloc_backend_t *vmalloc_naive_backend(void) { return &BACKEND; }
