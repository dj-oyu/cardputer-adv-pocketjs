// adapter_segment.c — the L2a shape from docs/quickjs-freertos-vm-spec.md
// sec.7 ("non-moving segments") written as a general allocator so the same
// traces that drove tlsf/estalloc/naive can drive it, and so the replayer's
// --verify can watch references across segment add / boundary / return.
//
//   runtime-owned pool (the arena replay.c hands to init())
//     └─ segments, carved first-fit from a sorted free-extent list
//          └─ blocks, first-fit inside a segment, coalesced on free
//
// Two layers on purpose: the pool layer never touches block contents, the
// block layer never moves a block. Every guarantee the L2 completion
// condition #6 asks for ("segment add / boundary crossing / return leave
// values and closure references intact") therefore reduces to "a segment is
// only returned when it holds no live block, and no two blocks overlap" —
// which is what replay.c --verify checks from the outside, and what the
// fault modes at the bottom of this file deliberately break so that the
// checker can be shown to fire.
//
// Alignment follows docs/vm-L2-design.md sec.3.3 (D7): segment bases on 16
// bytes (so an L4 compactor can use 128-bit PIE transfers), individual
// blocks on 4 (Xtensa has no 64-bit load; a JSValue at a 4-byte address is
// two l32i.n either way). Block header is one uint32_t, so payloads land on
// 4-byte boundaries and never on 8 — deliberately, since that is what the
// device will do and the host build must not hide a dependency on 8.
//
// Layout of a segment (base is 16-aligned):
//   [seg_t, padded to 16][blk hdr 4][payload][blk hdr 4][payload]... to base+size
// Block header: payload bytes (multiple of 4) | F_USED | F_PREV_USED.
// A free block keeps {next,prev} free-list links at the start of its payload
// and its payload size as a footer in its last 4 bytes, so the block after
// it can find its header when coalescing backwards (F_PREV_USED clear).
//
// Standard segment size, cache depth and fault mode come from configure()
// before init(); replay.c exposes them as --seg-size / --seg-cache / --fault.
#include "vmalloc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
// A returned segment's bytes are poisoned so that a stale pointer into it
// is caught by ASan at the instruction that dereferences it, not only by the
// pattern check that runs later. Segment bases and sizes are multiples of
// 16, so the 8-byte shadow granularity never leaves a partially poisoned
// word at either end.
#define POISON(p, n) __asan_poison_memory_region((p), (n))
#define UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
#define POISON(p, n) ((void)0)
#define UNPOISON(p, n) ((void)0)
#endif

#define GRAIN 4u
#define SEG_ALIGN 16u
#define BLK_HDR 4u
#define F_USED 1u
#define F_PREV_USED 2u
#define F_MASK 3u

#define ROUND_UP(x, a) (((x) + ((a) - 1)) & ~(size_t)((a) - 1))

enum { KIND_STD = 0, KIND_DEDICATED = 1, KIND_CACHED = 2 };

// packed: the links live at a payload address, which is 4-aligned and not
// 8-aligned (D7). On the device a pointer is 4 bytes and this is moot; on
// the 64-bit host it is exactly the underalignment UBSan would otherwise
// flag, and the compiler must emit access code that tolerates it.
typedef struct __attribute__((packed)) freelink {
  struct freelink *next, *prev;
} freelink_t;

typedef struct seg {
  uint32_t size;        // whole segment, header included, multiple of 16
  uint32_t live_blocks;
  uint32_t used_bytes;  // BLK_HDR + payload of used blocks
  uint32_t used_payload;
  uint32_t kind;
  uint32_t free_bytes;  // BLK_HDR + payload of free blocks
  freelink_t *free_head;
} seg_t;

// sizeof(seg_t) is 32 on a 64-bit host (7 words + one pointer, padded) and
// 28 on the device; both round to a 16-multiple so the first block header
// sits at base + SEG_HDR and every payload at a 4-byte offset from there.
#define SEG_HDR ((uint32_t)ROUND_UP(sizeof(seg_t), SEG_ALIGN))
// A free block must hold its two links plus the footer.
#define MIN_PAYLOAD ((uint32_t)ROUND_UP(sizeof(freelink_t) + 4, GRAIN))

typedef struct {
  uint8_t *base;
  size_t size;
} extent_t;

enum fault_mode {
  FAULT_NONE = 0,
  FAULT_EARLY_RETURN, // return a segment that still holds one live block
  FAULT_OVERLAP,      // a split places the remainder 4 bytes too early
  FAULT_MISALIGN,     // segment bases land on 16k+8
  FAULT_POOL_OVERLAP, // every 7th carve forgets to consume its extent
  FAULT_COMPACT,      // free() slides the next live block down into the hole
};

static struct {
  uint8_t *pool;
  size_t pool_size;
  size_t seg_size;     // standard segment size, multiple of 16
  size_t cache_max;    // empty standard segments retained
  int fault;
  unsigned carve_count;

  extent_t *ext; size_t ext_n, ext_cap;   // pool free extents, sorted by base, coalesced
  seg_t **segs; size_t seg_n, seg_cap;    // every segment (all kinds), sorted by base
  seg_t *current;                         // last segment a block was carved from
  size_t cached;                          // how many segs have kind == KIND_CACHED
  unsigned long events, n_added, n_returned, n_reused, n_dedicated;
} G = { .seg_size = 4096, .cache_max = 2 };

// ---------------------------------------------------------------- pool layer

static int ext_insert(size_t at, extent_t e) {
  if (G.ext_n == G.ext_cap) {
    size_t cap = G.ext_cap ? G.ext_cap * 2 : 64;
    extent_t *n = realloc(G.ext, cap * sizeof *n);
    if (!n) return -1;
    G.ext = n; G.ext_cap = cap;
  }
  memmove(&G.ext[at + 1], &G.ext[at], (G.ext_n - at) * sizeof *G.ext);
  G.ext[at] = e;
  G.ext_n++;
  return 0;
}

// First-fit over address-sorted extents: the lowest-addressed hole that
// fits, carved from its front. Lowest-address-first keeps new segments
// packed toward the bottom of the pool so the top stays one large extent as
// long as possible (that extent is what pool_largest_free reports).
static uint8_t *pool_carve(size_t size, unsigned *steps) {
  for (size_t i = 0; i < G.ext_n; i++) {
    if (steps) (*steps)++;
    if (G.ext[i].size < size) continue;
    uint8_t *p = G.ext[i].base;
    G.carve_count++;
    if (G.fault == FAULT_POOL_OVERLAP && (G.carve_count % 7) == 0) return p; // extent not consumed
    G.ext[i].base += size;
    G.ext[i].size -= size;
    if (G.ext[i].size == 0) {
      memmove(&G.ext[i], &G.ext[i + 1], (G.ext_n - i - 1) * sizeof *G.ext);
      G.ext_n--;
    }
    return p;
  }
  return NULL;
}

static void pool_return(uint8_t *base, size_t size) {
  size_t i = 0;
  while (i < G.ext_n && G.ext[i].base < base) i++;
  int merge_prev = i > 0 && G.ext[i - 1].base + G.ext[i - 1].size == base;
  int merge_next = i < G.ext_n && base + size == G.ext[i].base;
  if (merge_prev && merge_next) {
    G.ext[i - 1].size += size + G.ext[i].size;
    memmove(&G.ext[i], &G.ext[i + 1], (G.ext_n - i - 1) * sizeof *G.ext);
    G.ext_n--;
  } else if (merge_prev) {
    G.ext[i - 1].size += size;
  } else if (merge_next) {
    G.ext[i].base = base;
    G.ext[i].size += size;
  } else {
    if (ext_insert(i, (extent_t){base, size}) != 0) {
      fprintf(stderr, "segment: extent table exhausted\n");
      abort();
    }
  }
}

// ------------------------------------------------------------- segment table

static size_t seg_lower_bound(const uint8_t *p) {
  size_t lo = 0, hi = G.seg_n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if ((const uint8_t *)G.segs[mid] <= p) lo = mid + 1; else hi = mid;
  }
  return lo; // first segment with base > p
}

static seg_t *seg_owner(const void *p) {
  size_t i = seg_lower_bound(p);
  if (i == 0) return NULL;
  seg_t *s = G.segs[i - 1];
  if ((const uint8_t *)p < (const uint8_t *)s + s->size) return s;
  return NULL;
}

static int seg_table_insert(seg_t *s) {
  if (G.seg_n == G.seg_cap) {
    size_t cap = G.seg_cap ? G.seg_cap * 2 : 64;
    seg_t **n = realloc(G.segs, cap * sizeof *n);
    if (!n) return -1;
    G.segs = n; G.seg_cap = cap;
  }
  size_t i = seg_lower_bound((const uint8_t *)s);
  memmove(&G.segs[i + 1], &G.segs[i], (G.seg_n - i) * sizeof *G.segs);
  G.segs[i] = s;
  G.seg_n++;
  return 0;
}

static void seg_table_remove(seg_t *s) {
  size_t i = seg_lower_bound((const uint8_t *)s);
  // lower_bound returns the slot AFTER s (base <= p), so s is at i-1 --
  // unless a fault mode produced two segments with one base, in which case
  // it is a little further down. Found by scanning so the fault surfaces
  // in the replayer's checks rather than as an abort here.
  while (i > 0 && G.segs[i - 1] != s) i--;
  if (i == 0) { fprintf(stderr, "segment: table corrupt\n"); abort(); }
  memmove(&G.segs[i - 1], &G.segs[i], (G.seg_n - i) * sizeof *G.segs);
  G.seg_n--;
}

// -------------------------------------------------------------- block layer

static inline uint32_t *blk_hdr(void *payload) { return (uint32_t *)((uint8_t *)payload - BLK_HDR); }
static inline uint32_t blk_size(const uint32_t *h) { return *h & ~F_MASK; }
static inline uint8_t *blk_payload(uint32_t *h) { return (uint8_t *)h + BLK_HDR; }
static inline uint32_t *blk_next(seg_t *s, uint32_t *h) {
  uint8_t *n = blk_payload(h) + blk_size(h);
  return n < (uint8_t *)s + s->size ? (uint32_t *)n : NULL;
}
static inline uint32_t *blk_footer(uint32_t *h) { return (uint32_t *)(blk_payload(h) + blk_size(h) - 4); }
static inline uint32_t *blk_prev(uint32_t *h) {
  // The footer occupies the previous payload's last 4 bytes, i.e. the 4
  // bytes right before this header; the previous header is one payload
  // plus one header before that.
  uint32_t prev_size = *(uint32_t *)((uint8_t *)h - 4);
  return (uint32_t *)((uint8_t *)h - prev_size - BLK_HDR);
}

static void free_push(seg_t *s, uint32_t *h) {
  freelink_t *l = (freelink_t *)blk_payload(h);
  l->prev = NULL;
  l->next = s->free_head;
  if (s->free_head) s->free_head->prev = l;
  s->free_head = l;
  *blk_footer(h) = blk_size(h);
  s->free_bytes += BLK_HDR + blk_size(h);
}

static void free_unlink(seg_t *s, uint32_t *h) {
  freelink_t *l = (freelink_t *)blk_payload(h);
  if (l->prev) l->prev->next = l->next; else s->free_head = l->next;
  if (l->next) l->next->prev = l->prev;
  s->free_bytes -= BLK_HDR + blk_size(h);
}

// Free payload beyond the links and footer is scribbled so that a block
// that was "moved" or freed while still referenced cannot keep looking
// intact to the pattern check just because nobody reused the bytes yet.
static void scribble_free(uint32_t *h) {
  uint32_t sz = blk_size(h);
  size_t lo = sizeof(freelink_t), hi = sz - 4;
  if (hi > lo) memset(blk_payload(h) + lo, 0xDD, hi - lo);
}

static void seg_format(seg_t *s, uint32_t size, int kind) {
  s->size = size;
  s->live_blocks = 0;
  s->used_bytes = 0;
  s->used_payload = 0;
  s->kind = (uint32_t)kind;
  s->free_bytes = 0;
  s->free_head = NULL;
  uint32_t *h = (uint32_t *)((uint8_t *)s + SEG_HDR);
  *h = (size - SEG_HDR - BLK_HDR) | F_PREV_USED;
  free_push(s, h);
}

static seg_t *seg_add(size_t size, int kind, unsigned *steps) {
  uint8_t *base = pool_carve(size, steps);
  if (!base) return NULL;
  UNPOISON(base, size);
  if (G.fault == FAULT_MISALIGN) { base += 8; size -= 16; }
  seg_t *s = (seg_t *)base;
  seg_format(s, (uint32_t)size, kind);
  if (seg_table_insert(s) != 0) { pool_return(base, size); return NULL; }
  G.events++;
  if (kind == KIND_DEDICATED) G.n_dedicated++; else G.n_added++;
  return s;
}

static void seg_return(seg_t *s) {
  uint8_t *base = (uint8_t *)s;
  size_t size = s->size;
  if (G.fault == FAULT_MISALIGN) { base -= 8; size += 16; }
  if (G.current == s) G.current = NULL;
  seg_table_remove(s);
  memset(base, 0xDD, size);
  POISON(base, size);
  pool_return(base, size);
  G.events++;
  G.n_returned++;
}

static void seg_retire(seg_t *s) {
  if (s->kind == KIND_STD && G.cached < G.cache_max) {
    seg_format(s, s->size, KIND_CACHED);
    G.cached++;
    if (G.current == s) G.current = NULL;
    G.events++;
    return;
  }
  seg_return(s);
}

static seg_t *seg_take_cached(void) {
  for (size_t i = 0; i < G.seg_n; i++) {
    if (G.segs[i]->kind == KIND_CACHED) {
      seg_t *s = G.segs[i];
      s->kind = KIND_STD;
      G.cached--;
      G.events++;
      G.n_reused++;
      return s;
    }
  }
  return NULL;
}

// Splits h (free, unlinked) so it holds exactly `need` if the remainder can
// still be a block; the remainder goes back on the free list.
static void split_after(seg_t *s, uint32_t *h, uint32_t need) {
  uint32_t have = blk_size(h);
  if (have < need + BLK_HDR + MIN_PAYLOAD) return;
  uint32_t *rem = (uint32_t *)(blk_payload(h) + need);
  uint32_t rem_size = have - need - BLK_HDR;
  if (G.fault == FAULT_OVERLAP) { rem = (uint32_t *)((uint8_t *)rem - 4); rem_size += 4; }
  *h = need | (*h & F_MASK);
  *rem = rem_size | F_PREV_USED; // h is about to be marked used by the caller
  free_push(s, rem);
  // The block after `rem` (if any) now follows a free block.
  uint32_t *after = blk_next(s, rem);
  if (after) *after &= ~F_PREV_USED;
}

static void *carve_from_seg(seg_t *s, uint32_t *h, uint32_t need) {
  free_unlink(s, h);
  split_after(s, h, need);
  *h |= F_USED;
  uint32_t *after = blk_next(s, h);
  if (after) *after |= F_PREV_USED;
  s->live_blocks++;
  s->used_bytes += BLK_HDR + blk_size(h);
  s->used_payload += blk_size(h);
  G.current = s;
  return blk_payload(h);
}

static uint32_t *seg_find_fit(seg_t *s, uint32_t need, unsigned *steps) {
  for (freelink_t *l = s->free_head; l; l = l->next) {
    if (steps) (*steps)++;
    uint32_t *h = blk_hdr(l);
    if (blk_size(h) >= need) return h;
  }
  return NULL;
}

static void *segment_do_malloc(size_t size, unsigned *out_steps) {
  unsigned steps = 0;
  if (size == 0) size = 1;
  size_t need = ROUND_UP(size, GRAIN);
  if (need < MIN_PAYLOAD) need = MIN_PAYLOAD;
  void *ret = NULL;

  size_t std_max = G.seg_size - SEG_HDR - BLK_HDR;
  if (G.fault == FAULT_MISALIGN) std_max -= 16;
  if (need > std_max) {
    // Boundary crossing: a block that no standard segment can hold gets a
    // segment of exactly its own size (spec sec.7 "大きいフレームには要求量を
    // 満たす専用サイズ"), one block, returned to the pool the moment it dies.
    size_t seg_size = ROUND_UP(SEG_HDR + BLK_HDR + need, SEG_ALIGN);
    seg_t *cur = G.current;
    seg_t *s = seg_add(seg_size, KIND_DEDICATED, &steps);
    if (s) {
      uint32_t *h = (uint32_t *)((uint8_t *)s + SEG_HDR);
      ret = carve_from_seg(s, h, blk_size(h)); // whole payload; no split possible
      G.current = cur;                         // never carve a second block here
    }
    goto out;
  }

  // The last segment we carved from first: after a burst of same-size
  // allocations it is the one with a fitting hole at its top.
  if (G.current && G.current->kind == KIND_STD) {
    steps++;
    uint32_t *h = seg_find_fit(G.current, (uint32_t)need, &steps);
    if (h) { ret = carve_from_seg(G.current, h, (uint32_t)need); goto out; }
  }
  for (size_t i = 0; i < G.seg_n; i++) {
    seg_t *s = G.segs[i];
    if (s->kind != KIND_STD || s == G.current) continue;
    steps++;
    uint32_t *h = seg_find_fit(s, (uint32_t)need, &steps);
    if (h) { ret = carve_from_seg(s, h, (uint32_t)need); goto out; }
  }
  {
    seg_t *s = seg_take_cached();
    if (!s) s = seg_add(G.seg_size, KIND_STD, &steps);
    if (s) {
      uint32_t *h = seg_find_fit(s, (uint32_t)need, &steps);
      ret = carve_from_seg(s, h, (uint32_t)need);
    }
  }
out:
  if (out_steps) *out_steps += steps;
  return ret;
}

// Coalesces h (used, about to be freed) with free neighbours; returns the
// merged header, not yet on the free list.
static uint32_t *coalesce(seg_t *s, uint32_t *h) {
  uint32_t *n = blk_next(s, h);
  if (n && !(*n & F_USED)) {
    free_unlink(s, n);
    *h = (blk_size(h) + BLK_HDR + blk_size(n)) | (*h & F_MASK);
  }
  if (!(*h & F_PREV_USED)) {
    uint32_t *p = blk_prev(h);
    free_unlink(s, p);
    *p = (blk_size(p) + BLK_HDR + blk_size(h)) | (*p & F_MASK);
    h = p;
  }
  return h;
}

// FAULT_COMPACT: the bug L2 is designed to make impossible. After freeing
// block F, the used block N right after it is slid down into F's place and
// the hole moves up. Every pointer the caller held to N is now stale, and
// nothing told it. The free block that remains is scribbled like any other.
static uint32_t *fault_compact(seg_t *s, uint32_t *h) {
  uint32_t *n = blk_next(s, h);
  if (!n || !(*n & F_USED)) return h;
  uint32_t f = blk_size(h), nsz = blk_size(n);
  uint32_t hflags = *h & F_PREV_USED;
  memmove(blk_payload(h), blk_payload(n), nsz);
  *h = nsz | F_USED | hflags;
  uint32_t *hole = (uint32_t *)(blk_payload(h) + nsz);
  *hole = f | F_PREV_USED; // used block N' precedes it; F_USED clear
  uint32_t *after = blk_next(s, hole);
  if (after) *after &= ~F_PREV_USED;
  return hole;
}

static void segment_do_free(void *ptr, unsigned *out_steps) {
  (void)out_steps; // free never searches
  if (!ptr) return;
  seg_t *s = seg_owner(ptr);
  if (!s) { fprintf(stderr, "segment: free of pointer outside any segment\n"); abort(); }
  uint32_t *h = blk_hdr(ptr);
  if (!(*h & F_USED)) { fprintf(stderr, "segment: double free\n"); abort(); }
  s->live_blocks--;
  s->used_bytes -= BLK_HDR + blk_size(h);
  s->used_payload -= blk_size(h);
  *h &= ~F_USED;

  if (s->kind == KIND_DEDICATED) { seg_return(s); return; }

  if (G.fault == FAULT_COMPACT) h = fault_compact(s, h);
  h = coalesce(s, h);
  uint32_t *after = blk_next(s, h);
  if (after) *after &= ~F_PREV_USED;
  free_push(s, h);
  scribble_free(h);

  int empty = s->live_blocks == 0;
  if (G.fault == FAULT_EARLY_RETURN && s->live_blocks == 1) empty = 1;
  if (empty) seg_retire(s);
}

static void *segment_do_realloc(void *ptr, size_t size, unsigned *out_steps) {
  if (!ptr) return segment_do_malloc(size, out_steps);
  if (size == 0) { segment_do_free(ptr, out_steps); return NULL; }
  seg_t *s = seg_owner(ptr);
  if (!s) { fprintf(stderr, "segment: realloc of pointer outside any segment\n"); abort(); }
  uint32_t *h = blk_hdr(ptr);
  size_t need = ROUND_UP(size, GRAIN);
  if (need < MIN_PAYLOAD) need = MIN_PAYLOAD;
  uint32_t have = blk_size(h);

  if (s->kind == KIND_STD) {
    if (need <= have) {
      // Shrink in place; give the tail back if it can be a block. A free
      // neighbour is absorbed first so the tail never ends up adjacent to
      // another free block (the coalescing invariant check() enforces).
      s->used_bytes -= BLK_HDR + have; s->used_payload -= have;
      uint32_t *n = blk_next(s, h);
      if (n && !(*n & F_USED)) {
        free_unlink(s, n);
        *h = (have + BLK_HDR + blk_size(n)) | (*h & F_MASK);
      }
      split_after(s, h, (uint32_t)need);
      // Whether the tail became a block (already F_PREV_USED) or was kept,
      // whatever follows h now follows a used block.
      uint32_t *after = blk_next(s, h);
      if (after) *after |= F_PREV_USED;
      s->used_bytes += BLK_HDR + blk_size(h); s->used_payload += blk_size(h);
      return ptr;
    }
    uint32_t *n = blk_next(s, h);
    if (n && !(*n & F_USED) && have + BLK_HDR + blk_size(n) >= need) {
      // Grow in place into the free neighbour: the address is preserved.
      free_unlink(s, n);
      s->used_bytes -= BLK_HDR + have; s->used_payload -= have;
      *h = (have + BLK_HDR + blk_size(n)) | (*h & F_MASK);
      split_after(s, h, (uint32_t)need);
      uint32_t *after = blk_next(s, h);
      if (after) *after |= F_PREV_USED;
      s->used_bytes += BLK_HDR + blk_size(h); s->used_payload += blk_size(h);
      return ptr;
    }
  } else if (need <= have) {
    return ptr; // dedicated: shrinking keeps the segment (its size was set once)
  }
  unsigned steps = 0;
  void *n = segment_do_malloc(size, &steps);
  if (out_steps) *out_steps += steps;
  if (!n) return NULL;
  memcpy(n, ptr, have < need ? have : need);
  segment_do_free(ptr, NULL);
  return n;
}

// -------------------------------------------------------------------- stats

static void segment_stats(vmalloc_stats_t *out) {
  size_t used = 0, payload = 0, blocks = 0, reserved = 0, hdrs = 0, inside = 0, cached_b = 0;
  size_t live = 0, cachedn = 0, dedicated = 0, largest = 0;
  for (size_t i = 0; i < G.seg_n; i++) {
    seg_t *s = G.segs[i];
    reserved += s->size;
    hdrs += SEG_HDR;
    if (s->kind == KIND_CACHED) {
      cachedn++;
      cached_b += s->size;
      if (s->size - SEG_HDR - BLK_HDR > largest) largest = s->size - SEG_HDR - BLK_HDR;
      continue;
    }
    live++;
    if (s->kind == KIND_DEDICATED) dedicated++;
    used += s->used_bytes + SEG_HDR;
    payload += s->used_payload;
    blocks += s->live_blocks;
    inside += s->free_bytes;
    for (freelink_t *l = s->free_head; l; l = l->next)
      if (blk_size(blk_hdr(l)) > largest) largest = blk_size(blk_hdr(l));
  }
  size_t pool_free = 0, pool_largest = 0;
  for (size_t i = 0; i < G.ext_n; i++) {
    pool_free += G.ext[i].size;
    if (G.ext[i].size > pool_largest) pool_largest = G.ext[i].size;
  }
  if (pool_largest > SEG_HDR + BLK_HDR && pool_largest - SEG_HDR - BLK_HDR > largest)
    largest = pool_largest - SEG_HDR - BLK_HDR; // a dedicated segment could take the whole extent
  out->used_bytes = used;
  out->used_payload_bytes = payload;
  out->free_bytes = pool_free + inside + cached_b;
  out->largest_free_block = largest & ~(size_t)3;
  out->blocks_used = blocks;
  out->reserved_bytes = reserved;
  out->seg_header_bytes = hdrs;
  out->seg_free_inside = inside;
  out->seg_cached_bytes = cached_b;
  out->pool_free_bytes = pool_free;
  out->pool_largest_free = pool_largest;
  out->segments_live = live;
  out->segments_cached = cachedn;
  out->segments_dedicated = dedicated;
  out->events = G.events;
  out->seg_added = G.n_added;
  out->seg_returned = G.n_returned;
  out->seg_reused = G.n_reused;
  out->seg_dedicated_added = G.n_dedicated;
}

// -------------------------------------------------------------------- check

static int fail(const char *what) {
  fprintf(stderr, "segment check: %s\n", what);
  return 0;
}

static int segment_check(void) {
  size_t accounted = 0;
  const uint8_t *prev_end = G.pool;
  size_t ei = 0;
  for (size_t i = 0; i < G.seg_n; i++) {
    seg_t *s = G.segs[i];
    const uint8_t *base = (const uint8_t *)s;
    // Interleave with extents to prove segments + extents tile the pool.
    while (ei < G.ext_n && G.ext[ei].base < base) {
      if (G.ext[ei].base != prev_end) return fail("gap or overlap before extent");
      prev_end = G.ext[ei].base + G.ext[ei].size;
      accounted += G.ext[ei].size;
      ei++;
    }
    if (G.fault != FAULT_MISALIGN) {
      if (base != prev_end) return fail("gap or overlap before segment");
      if (((uintptr_t)base & (SEG_ALIGN - 1)) != 0) return fail("segment base not 16-aligned");
    }
    if (s->size % SEG_ALIGN != 0 && G.fault != FAULT_MISALIGN) return fail("segment size not a multiple of 16");
    prev_end = base + s->size;
    if (G.fault == FAULT_MISALIGN) prev_end += 8;
    accounted += s->size + (G.fault == FAULT_MISALIGN ? 16 : 0);

    // Walk blocks.
    size_t live = 0, used = 0, usedp = 0, freeb = 0, prev_used = 1;
    uint32_t *h = (uint32_t *)(base + SEG_HDR);
    for (; h; h = blk_next(s, h)) {
      if (blk_size(h) % GRAIN != 0) return fail("block size not a multiple of 4");
      if (((*h & F_PREV_USED) != 0) != prev_used) return fail("F_PREV_USED inconsistent");
      if (blk_payload(h) + blk_size(h) > base + s->size) return fail("block runs past segment end");
      if (*h & F_USED) { live++; used += BLK_HDR + blk_size(h); usedp += blk_size(h); prev_used = 1; }
      else {
        if (!prev_used) return fail("two adjacent free blocks");
        if (*blk_footer(h) != blk_size(h)) return fail("free footer mismatch");
        int found = 0;
        for (freelink_t *l = s->free_head; l; l = l->next) if (blk_hdr(l) == h) { found = 1; break; }
        if (!found) return fail("free block not on its segment's list");
        freeb += BLK_HDR + blk_size(h);
        prev_used = 0;
      }
    }
    if (live != s->live_blocks) return fail("live_blocks miscount");
    if (used != s->used_bytes || usedp != s->used_payload) return fail("used_bytes miscount");
    if (freeb != s->free_bytes) return fail("free_bytes miscount");
    if (s->kind == KIND_CACHED && live != 0) return fail("cached segment holds a live block");
    if (s->kind == KIND_DEDICATED && live > 1) return fail("dedicated segment holds two blocks");
    // Every free-list node must be a block of this segment.
    for (freelink_t *l = s->free_head; l; l = l->next) {
      const uint8_t *p = (const uint8_t *)l;
      if (p < base + SEG_HDR || p >= base + s->size) return fail("free-list node outside segment");
      if (*blk_hdr(l) & F_USED) return fail("used block on free list");
    }
  }
  while (ei < G.ext_n) {
    if (G.ext[ei].base != prev_end) return fail("gap or overlap before trailing extent");
    prev_end = G.ext[ei].base + G.ext[ei].size;
    accounted += G.ext[ei].size;
    ei++;
  }
  if (accounted != G.pool_size) return fail("segments + extents do not tile the pool");
  size_t cachedn = 0;
  for (size_t i = 0; i < G.seg_n; i++) if (G.segs[i]->kind == KIND_CACHED) cachedn++;
  if (cachedn != G.cached) return fail("cache count drift");
  return 1;
}

// ---------------------------------------------------------------- interface

static int segment_init(void *pool, size_t pool_size) {
  if (((uintptr_t)pool & (SEG_ALIGN - 1)) != 0) return -1;
  if (G.seg_size < SEG_HDR + BLK_HDR + MIN_PAYLOAD || G.seg_size % SEG_ALIGN) return -1;
  if (G.pool) UNPOISON(G.pool, G.pool_size); // a previous trial's leftovers (bisect)
  G.pool = pool;
  G.pool_size = pool_size & ~(size_t)(SEG_ALIGN - 1);
  G.ext_n = 0;
  G.seg_n = 0;
  G.current = NULL;
  G.cached = 0;
  G.carve_count = 0;
  G.events = G.n_added = G.n_returned = G.n_reused = G.n_dedicated = 0;
  if (G.pool_size) {
    if (ext_insert(0, (extent_t){G.pool, G.pool_size}) != 0) return -1;
    POISON(G.pool, G.pool_size);
  }
  return 0;
}

static int segment_owner_q(const void *p, vmalloc_seg_t *out) {
  seg_t *s = seg_owner(p);
  if (!s) return 0;
  out->base = s; out->size = s->size; out->kind = (int)s->kind; out->live_blocks = s->live_blocks;
  return 1;
}

static int segment_at(size_t i, vmalloc_seg_t *out) {
  if (i >= G.seg_n) return 0;
  seg_t *s = G.segs[i];
  out->base = s; out->size = s->size; out->kind = (int)s->kind; out->live_blocks = s->live_blocks;
  return 1;
}

static unsigned long segment_events(void) { return G.events; }

static int segment_configure(const char *key, const char *value) {
  if (!strcmp(key, "seg_size")) { G.seg_size = strtoull(value, NULL, 0); return 0; }
  if (!strcmp(key, "cache")) { G.cache_max = strtoull(value, NULL, 0); return 0; }
  if (!strcmp(key, "fault")) {
    if (!strcmp(value, "none")) G.fault = FAULT_NONE;
    else if (!strcmp(value, "early-return")) G.fault = FAULT_EARLY_RETURN;
    else if (!strcmp(value, "overlap")) G.fault = FAULT_OVERLAP;
    else if (!strcmp(value, "misalign")) G.fault = FAULT_MISALIGN;
    else if (!strcmp(value, "pool-overlap")) G.fault = FAULT_POOL_OVERLAP;
    else if (!strcmp(value, "compact")) G.fault = FAULT_COMPACT;
    else return -1;
    return 0;
  }
  return -1;
}

static const vmalloc_backend_t BACKEND = {
    .name = "segment",
    .init = segment_init,
    .do_malloc = segment_do_malloc,
    .do_realloc = segment_do_realloc,
    .do_free = segment_do_free,
    .stats = segment_stats,
    .check = segment_check,
    .owner = segment_owner_q,
    .segment_at = segment_at,
    .events = segment_events,
    .configure = segment_configure,
};

const vmalloc_backend_t *vmalloc_segment_backend(void) { return &BACKEND; }
