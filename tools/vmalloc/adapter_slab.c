// adapter_slab.c — a size-class slab allocator for the whole JS object heap,
// written against the same vmalloc.h interface as tlsf/estalloc/segment so
// the question docs/vm/vm-ledger/08-slab-study.md asks ("does a slab keep
// the pool's largest free extent bigger than TLSF does?") is answered on the
// same traces, the same pool and the same --verify checks.
//
//   pool (the arena replay.c hands to init())
//     └─ ledger: every segment's [start,end), sorted by start. No per-block
//        header anywhere: free() binary-searches the ledger for the owner.
//          ├─ slab segment   (seg_size, one size class, one used-bit per slot)
//          ├─ var segment    (seg_size, 8 B grains, "block starts here" and
//          │                  "block is used" bit per grain; mid sizes)
//          ├─ dedicated      (anything above var_max; size = end - start)
//          └─ cached         (an empty standard segment kept for reuse)
//
// Why no block header: QuickJS's js_malloc_usable_size() is called on every
// js_free_rt(), and js_realloc2() turns (usable - requested) into free
// capacity. A slab can answer usable_size from the class alone, so the
// slack is real instead of the zero guest.c reports today
// (docs/vm/vm-ledger/05-allocation.md sec.6).
//
// The risk this study exists to measure is not speed but placement: one
// long-lived object pins a whole 4 KiB segment, and pinned segments scattered
// across the pool cut its largest free extent. Two independent knobs set the
// placement policy so the effect can be compared rather than argued:
//   carve = low   the lowest-addressed pool extent that fits
//           best  the smallest extent that fits (lowest address on a tie)
//           split standard segments lowest-first, dedicated ones carved from
//                 the END of the highest-addressed extent that fits, so big
//                 short-lived blocks do not leave holes among the slabs
//   pick  = low   allocate from the lowest-addressed non-full segment of the
//                 class, so high segments drain and can be returned
//           recent the most recently touched non-full segment (LIFO), the
//                 usual slab choice for cache locality
// Empty standard segments are retained up to `cache` (default 2, as in
// adapter_segment.c); under pick=low a lower empty segment displaces the
// highest cached one, which is returned instead.
//
// Device width: the in-segment header (8 B fixed + bitmaps) holds no
// pointers and has the same size on the host and on the ESP32-S3. The ledger
// and the per-class lists are host structures here; on the device they are
// a small table (8 B per segment: two uint32_t) whose bytes are added to
// used_bytes at that width but not placed in the pool (see the doc).
#include "vmalloc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
// Free slots, free grains and returned segments are poisoned. Every such
// range starts on an 8-byte boundary and is a multiple of 8 long, so ASan's
// 8-byte shadow granularity never leaves a partially poisoned word.
#define POISON(p, n) __asan_poison_memory_region((p), (n))
#define UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
#define POISON(p, n) ((void)0)
#define UNPOISON(p, n) ((void)0)
#endif

#define SEG_ALIGN 16u
#define GRAIN 8u
#define HDR_FIXED 8u      // [0] class, [1] kind, [2..7] reserved; bitmaps follow
#define LEDGER_ENTRY 8u   // device-width ledger entry: uint32_t start, end
#define MAX_CLASSES 32
#define ROUND_UP(x, a) (((x) + ((a) - 1)) & ~(size_t)((a) - 1))

enum { K_SLAB, K_VAR, K_DED, K_CACHED };
enum { CARVE_LOW, CARVE_BEST, CARVE_SPLIT };
enum { PICK_LOW, PICK_RECENT };
enum { CAT_VAR = -1, CAT_DED = -2 };
enum fault_mode {
  FAULT_NONE = 0,
  FAULT_EARLY_RETURN, // retire a segment that still holds one live block
  FAULT_OVERLAP,      // every 7th slab alloc forgets to set its used bit
  FAULT_POOL_OVERLAP, // every 7th carve forgets to consume its extent
  FAULT_LEDGER_ORDER, // every 5th new segment is appended, not inserted in order
  FAULT_BITMAP,       // slab free clears the neighbouring slot's bit instead
};

typedef struct seg {
  uint8_t *start, *end;
  int kind;
  int cls;          // K_SLAB: class index
  uint32_t live;    // live blocks
  uint32_t cap;     // K_SLAB: slots; K_VAR: grains
  uint32_t hdr;     // bytes before slot / grain 0
  uint32_t used;    // usable bytes of live blocks
  uint32_t largest; // K_VAR: largest free run, grains
  uint32_t hint;    // K_SLAB: no free slot below this index
  int listed;       // on a partial (slab) or var list
} seg_t;

typedef struct {
  seg_t **v;
  size_t n, cap;
} list_t;

typedef struct {
  uint8_t *base;
  size_t size;
} extent_t;

static struct {
  uint8_t *pool;
  size_t pool_size;
  size_t seg_size, cache_max, var_max;
  int ncls;
  uint32_t cls_size[MAX_CLASSES], cls_cap[MAX_CLASSES], cls_hdr[MAX_CLASSES];
  uint32_t var_grains, var_hdr, var_bm; // var_bm: bytes per bitmap
  int carve, pick, fault;
  unsigned long carve_count, slab_allocs, new_segs;

  extent_t *ext; size_t ext_n, ext_cap;
  seg_t **led; size_t led_n, led_cap;
  list_t part[MAX_CLASSES], vars;
  size_t cached;
  unsigned long events, n_added, n_returned, n_reused, n_dedicated;
} G = {
    .seg_size = 4096, .cache_max = 2, .var_max = 1024,
    .ncls = 12, .cls_size = {8, 16, 24, 32, 48, 56, 64, 72, 96, 104, 120, 144},
};

static void die(const char *what) {
  fprintf(stderr, "slab: %s\n", what);
  abort();
}

// ------------------------------------------------------------------- bits

static inline int bit_get(const uint8_t *bm, size_t i) { return (bm[i >> 3] >> (i & 7)) & 1; }
static inline void bit_set(uint8_t *bm, size_t i) { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bit_clr(uint8_t *bm, size_t i) { bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

// Next set bit in (i, n), or n. Steps are counted per 32-bit word touched,
// the unit a device implementation would scan in.
static size_t next_set(const uint8_t *bm, size_t i, size_t n, unsigned *steps) {
  size_t j = i + 1;
  size_t last_word = SIZE_MAX;
  while (j < n) {
    if (steps && (j >> 5) != last_word) { (*steps)++; last_word = j >> 5; }
    if ((j & 7) == 0 && bm[j >> 3] == 0) { j += 8; continue; }
    if (bit_get(bm, j)) return j;
    j++;
  }
  return n;
}

static size_t prev_set(const uint8_t *bm, size_t i) {
  while (i > 0) {
    i--;
    if (bit_get(bm, i)) return i;
  }
  return SIZE_MAX;
}

// ------------------------------------------------------------------- lists

static int list_push(list_t *l, size_t at, seg_t *s) {
  if (l->n == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 16;
    seg_t **v = realloc(l->v, cap * sizeof *v);
    if (!v) die("host out of memory (list)");
    l->v = v; l->cap = cap;
  }
  memmove(&l->v[at + 1], &l->v[at], (l->n - at) * sizeof *l->v);
  l->v[at] = s;
  l->n++;
  return 0;
}

// pick=low keeps a list in address order and takes v[0]; pick=recent keeps
// it in touch order and takes v[n-1]. On the device both are an intrusive
// linked list threaded through the segment headers, so list maintenance is
// not counted as search steps.
static void list_add(list_t *l, seg_t *s) {
  size_t at = l->n;
  if (G.pick == PICK_LOW) {
    size_t lo = 0, hi = l->n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (l->v[m]->start < s->start) lo = m + 1; else hi = m; }
    at = lo;
  }
  list_push(l, at, s);
  s->listed = 1;
}

static void list_remove(list_t *l, seg_t *s) {
  for (size_t i = 0; i < l->n; i++) {
    if (l->v[i] == s) {
      memmove(&l->v[i], &l->v[i + 1], (l->n - i - 1) * sizeof *l->v);
      l->n--;
      s->listed = 0;
      return;
    }
  }
  die("list corrupt: segment not on its list");
}

static void list_touch(list_t *l, seg_t *s) {
  if (G.pick != PICK_RECENT || !s->listed || (l->n && l->v[l->n - 1] == s)) return;
  list_remove(l, s);
  list_add(l, s);
}

// -------------------------------------------------------------- pool layer

static void ext_insert(size_t at, extent_t e) {
  if (G.ext_n == G.ext_cap) {
    size_t cap = G.ext_cap ? G.ext_cap * 2 : 64;
    extent_t *n = realloc(G.ext, cap * sizeof *n);
    if (!n) die("host out of memory (extents)");
    G.ext = n; G.ext_cap = cap;
  }
  memmove(&G.ext[at + 1], &G.ext[at], (G.ext_n - at) * sizeof *G.ext);
  G.ext[at] = e;
  G.ext_n++;
}

static void ext_remove(size_t i) {
  memmove(&G.ext[i], &G.ext[i + 1], (G.ext_n - i - 1) * sizeof *G.ext);
  G.ext_n--;
}

static uint8_t *pool_carve(size_t size, int dedicated, unsigned *steps) {
  size_t pick = SIZE_MAX;
  int from_end = 0;
  if (G.carve == CARVE_SPLIT && dedicated) {
    for (size_t i = G.ext_n; i-- > 0;) {
      if (steps) (*steps)++;
      if (G.ext[i].size >= size) { pick = i; from_end = 1; break; }
    }
  } else if (G.carve == CARVE_BEST) {
    for (size_t i = 0; i < G.ext_n; i++) {
      if (steps) (*steps)++;
      if (G.ext[i].size >= size && (pick == SIZE_MAX || G.ext[i].size < G.ext[pick].size)) pick = i;
    }
  } else {
    for (size_t i = 0; i < G.ext_n; i++) {
      if (steps) (*steps)++;
      if (G.ext[i].size >= size) { pick = i; break; }
    }
  }
  if (pick == SIZE_MAX) return NULL;
  extent_t *e = &G.ext[pick];
  uint8_t *p = from_end ? e->base + e->size - size : e->base;
  G.carve_count++;
  if (G.fault == FAULT_POOL_OVERLAP && (G.carve_count % 7) == 0) return p; // extent not consumed
  if (!from_end) e->base += size;
  e->size -= size;
  if (e->size == 0) ext_remove(pick);
  return p;
}

static void pool_return(uint8_t *base, size_t size) {
  size_t lo = 0, hi = G.ext_n;
  while (lo < hi) { size_t m = (lo + hi) / 2; if (G.ext[m].base < base) lo = m + 1; else hi = m; }
  size_t i = lo;
  int merge_prev = i > 0 && G.ext[i - 1].base + G.ext[i - 1].size == base;
  int merge_next = i < G.ext_n && base + size == G.ext[i].base;
  if (merge_prev && merge_next) {
    G.ext[i - 1].size += size + G.ext[i].size;
    ext_remove(i);
  } else if (merge_prev) {
    G.ext[i - 1].size += size;
  } else if (merge_next) {
    G.ext[i].base = base;
    G.ext[i].size += size;
  } else {
    ext_insert(i, (extent_t){base, size});
  }
}

// Takes `extra` bytes from the front of the extent that starts exactly at
// `at`, if there is one that large: how a dedicated segment grows in place.
static int pool_take_at(uint8_t *at, size_t extra) {
  size_t lo = 0, hi = G.ext_n;
  while (lo < hi) { size_t m = (lo + hi) / 2; if (G.ext[m].base < at) lo = m + 1; else hi = m; }
  if (lo == G.ext_n || G.ext[lo].base != at || G.ext[lo].size < extra) return 0;
  G.ext[lo].base += extra;
  G.ext[lo].size -= extra;
  if (G.ext[lo].size == 0) ext_remove(lo);
  return 1;
}

// ------------------------------------------------------------------ ledger

static size_t led_upper(const uint8_t *p, unsigned *steps) {
  size_t lo = 0, hi = G.led_n;
  while (lo < hi) {
    if (steps) (*steps)++;
    size_t m = lo + (hi - lo) / 2;
    if (G.led[m]->start <= p) lo = m + 1; else hi = m;
  }
  return lo; // first segment whose start is above p
}

static seg_t *led_owner(const uint8_t *p, unsigned *steps) {
  size_t i = led_upper(p, steps);
  if (i == 0) return NULL;
  seg_t *s = G.led[i - 1];
  return p < s->end ? s : NULL;
}

static void led_insert(seg_t *s) {
  if (G.led_n == G.led_cap) {
    size_t cap = G.led_cap ? G.led_cap * 2 : 64;
    seg_t **n = realloc(G.led, cap * sizeof *n);
    if (!n) die("host out of memory (ledger)");
    G.led = n; G.led_cap = cap;
  }
  size_t i = led_upper(s->start, NULL);
  if (G.fault == FAULT_LEDGER_ORDER && (++G.new_segs % 5) == 0) i = G.led_n;
  memmove(&G.led[i + 1], &G.led[i], (G.led_n - i) * sizeof *G.led);
  G.led[i] = s;
  G.led_n++;
}

static void led_remove(seg_t *s) {
  for (size_t i = G.led_n; i-- > 0;) { // linear: must also work when a fault broke the order
    if (G.led[i] == s) {
      memmove(&G.led[i], &G.led[i + 1], (G.led_n - i - 1) * sizeof *G.led);
      G.led_n--;
      return;
    }
  }
  die("ledger corrupt: segment not in ledger");
}

// ---------------------------------------------------------------- segments

static seg_t *seg_new(int kind, size_t size, unsigned *steps) {
  uint8_t *b = pool_carve(size, kind == K_DED, steps);
  if (!b) return NULL;
  seg_t *s = calloc(1, sizeof *s);
  if (!s) die("host out of memory (segment)");
  s->start = b;
  s->end = b + size;
  s->kind = kind;
  UNPOISON(b, size);
  led_insert(s);
  G.events++;
  if (kind == K_DED) G.n_dedicated++; else G.n_added++;
  return s;
}

static void seg_return(seg_t *s) {
  led_remove(s);
  UNPOISON(s->start, (size_t)(s->end - s->start)); // free slots are poisoned; the scribble below writes them
  memset(s->start, 0xDD, (size_t)(s->end - s->start));
  POISON(s->start, (size_t)(s->end - s->start));
  pool_return(s->start, (size_t)(s->end - s->start));
  free(s);
  G.events++;
  G.n_returned++;
}

static void make_cached(seg_t *s) {
  UNPOISON(s->start, (size_t)(s->end - s->start));
  s->kind = K_CACHED;
  s->live = s->used = s->cap = 0;
  s->hdr = HDR_FIXED;
  memset(s->start, 0, HDR_FIXED);
  s->start[1] = K_CACHED;
  memset(s->start + HDR_FIXED, 0xDD, (size_t)(s->end - s->start) - HDR_FIXED);
  POISON(s->start + HDR_FIXED, (size_t)(s->end - s->start) - HDR_FIXED);
  G.cached++;
  G.events++;
}

static void seg_retire(seg_t *s) {
  if (s->listed) list_remove(s->kind == K_SLAB ? &G.part[s->cls] : &G.vars, s);
  if (s->kind == K_DED) { seg_return(s); return; }
  if (G.cached < G.cache_max) { make_cached(s); return; }
  if (G.pick == PICK_LOW && G.cache_max > 0) {
    seg_t *hi = NULL;
    for (size_t i = G.led_n; i-- > 0;)
      if (G.led[i]->kind == K_CACHED) { hi = G.led[i]; break; }
    if (hi && hi->start > s->start) {
      G.cached--;
      seg_return(hi);
      make_cached(s);
      return;
    }
  }
  seg_return(s);
}

// A standard segment: a cached one if any (the lowest under pick=low, else
// the highest -- which is where pick=recent tends to have just freed one),
// otherwise freshly carved.
static seg_t *std_seg(unsigned *steps) {
  if (G.cached) {
    if (steps) (*steps)++;
    seg_t *s = NULL;
    if (G.pick == PICK_LOW) {
      for (size_t i = 0; i < G.led_n && !s; i++) if (G.led[i]->kind == K_CACHED) s = G.led[i];
    } else {
      for (size_t i = G.led_n; i-- > 0 && !s;) if (G.led[i]->kind == K_CACHED) s = G.led[i];
    }
    if (s) {
      UNPOISON(s->start, (size_t)(s->end - s->start));
      G.cached--;
      G.events++;
      G.n_reused++;
      return s;
    }
  }
  seg_t *s = seg_new(K_SLAB, G.seg_size, steps);
  return s;
}

// -------------------------------------------------------------- slab kind

static void format_slab(seg_t *s, int cls) {
  size_t size = (size_t)(s->end - s->start);
  s->kind = K_SLAB;
  s->cls = cls;
  s->cap = G.cls_cap[cls];
  s->hdr = G.cls_hdr[cls];
  s->live = s->used = s->hint = 0;
  memset(s->start, 0, s->hdr);
  s->start[0] = (uint8_t)cls;
  s->start[1] = K_SLAB;
  memset(s->start + s->hdr, 0xDD, size - s->hdr);
  POISON(s->start + s->hdr, size - s->hdr);
}

static void *slab_alloc(int cls, unsigned *steps) {
  list_t *l = &G.part[cls];
  seg_t *s;
  (*steps)++;
  if (l->n) {
    s = G.pick == PICK_LOW ? l->v[0] : l->v[l->n - 1];
  } else {
    s = std_seg(steps);
    if (!s) return NULL;
    format_slab(s, cls);
    list_add(l, s);
  }
  uint8_t *bm = s->start + HDR_FIXED;
  size_t i = s->hint;
  size_t last_word = SIZE_MAX;
  while (i < s->cap) {
    if ((i >> 5) != last_word) { (*steps)++; last_word = i >> 5; }
    if ((i & 7) == 0 && bm[i >> 3] == 0xFF && i + 8 <= s->cap) { i += 8; continue; }
    if (!bit_get(bm, i)) break;
    i++;
  }
  if (i >= s->cap) die("partial slab has no free slot");
  if (!(G.fault == FAULT_OVERLAP && (++G.slab_allocs % 7) == 0)) bit_set(bm, i);
  s->hint = (uint32_t)i + 1;
  s->live++;
  s->used += G.cls_size[cls];
  if (s->live == s->cap) list_remove(l, s);
  else list_touch(l, s);
  uint8_t *p = s->start + s->hdr + i * G.cls_size[cls];
  UNPOISON(p, G.cls_size[cls]);
  return p;
}

static void slab_free(seg_t *s, uint8_t *p) {
  uint32_t c = G.cls_size[s->cls];
  size_t off = (size_t)(p - (s->start + s->hdr));
  if (p < s->start + s->hdr || off % c || off / c >= s->cap) die("free of a pointer that is not a slot");
  size_t i = off / c;
  uint8_t *bm = s->start + HDR_FIXED;
  if (!bit_get(bm, i)) die("double free (slot bit clear)");
  if (G.fault == FAULT_BITMAP && i + 1 < s->cap && bit_get(bm, i + 1)) bit_clr(bm, i + 1);
  else bit_clr(bm, i);
  if (i < s->hint) s->hint = (uint32_t)i;
  if (s->live == s->cap) list_add(&G.part[s->cls], s);
  s->live--;
  s->used -= c;
  memset(p, 0xDD, c);
  POISON(p, c);
  if (s->live == 0 || (G.fault == FAULT_EARLY_RETURN && s->live == 1)) seg_retire(s);
  else list_touch(&G.part[s->cls], s);
}

// --------------------------------------------------------------- var kind

static inline uint8_t *var_begin(seg_t *s) { return s->start + HDR_FIXED; }
static inline uint8_t *var_usedbm(seg_t *s) { return s->start + HDR_FIXED + G.var_bm; }
static inline size_t var_len(seg_t *s, size_t i, unsigned *steps) {
  return next_set(var_begin(s), i, s->cap, steps) - i;
}

static void var_recount(seg_t *s) {
  uint32_t best = 0;
  for (size_t i = 0; i < s->cap;) {
    size_t len = var_len(s, i, NULL);
    if (!bit_get(var_usedbm(s), i) && len > best) best = (uint32_t)len;
    i += len;
  }
  s->largest = best;
}

static void format_var(seg_t *s) {
  size_t size = (size_t)(s->end - s->start);
  s->kind = K_VAR;
  s->cls = -1;
  s->cap = G.var_grains;
  s->hdr = G.var_hdr;
  s->live = s->used = 0;
  memset(s->start, 0, s->hdr);
  s->start[1] = K_VAR;
  bit_set(var_begin(s), 0);
  s->largest = s->cap;
  memset(s->start + s->hdr, 0xDD, size - s->hdr);
  POISON(s->start + s->hdr, size - s->hdr);
}

static void *var_take(seg_t *s, size_t i, size_t n) {
  size_t len = var_len(s, i, NULL);
  if (len > n) bit_set(var_begin(s), i + n);
  bit_set(var_usedbm(s), i);
  s->live++;
  s->used += (uint32_t)(n * GRAIN);
  var_recount(s);
  uint8_t *p = s->start + s->hdr + i * GRAIN;
  UNPOISON(p, n * GRAIN);
  return p;
}

static void *var_alloc(size_t size, unsigned *steps) {
  size_t n = ROUND_UP(size, GRAIN) / GRAIN;
  list_t *l = &G.vars;
  for (size_t k = 0; k < l->n; k++) {
    seg_t *s = G.pick == PICK_LOW ? l->v[k] : l->v[l->n - 1 - k];
    (*steps)++;
    if (s->largest < n) continue;
    for (size_t i = 0; i < s->cap;) {
      (*steps)++;
      size_t len = var_len(s, i, steps);
      if (!bit_get(var_usedbm(s), i) && len >= n) {
        void *p = var_take(s, i, n);
        list_touch(l, s);
        return p;
      }
      i += len;
    }
    die("var segment largest-run hint is wrong");
  }
  seg_t *s = std_seg(steps);
  if (!s) return NULL;
  format_var(s);
  list_add(l, s);
  return var_take(s, 0, n);
}

static void var_free(seg_t *s, uint8_t *p) {
  size_t off = (size_t)(p - (s->start + s->hdr));
  if (p < s->start + s->hdr || off % GRAIN || off / GRAIN >= s->cap) die("free of a pointer that is not a grain");
  size_t i = off / GRAIN;
  uint8_t *bb = var_begin(s), *ub = var_usedbm(s);
  if (!bit_get(bb, i) || !bit_get(ub, i)) die("double free or interior pointer (var)");
  size_t len = var_len(s, i, NULL);
  bit_clr(ub, i);
  s->live--;
  s->used -= (uint32_t)(len * GRAIN);
  memset(p, 0xDD, len * GRAIN);
  POISON(p, len * GRAIN);
  if (i + len < s->cap && !bit_get(ub, i + len)) bit_clr(bb, i + len);
  if (i > 0) {
    size_t k = prev_set(bb, i);
    if (!bit_get(ub, k)) bit_clr(bb, i);
  }
  var_recount(s);
  if (s->live == 0 || (G.fault == FAULT_EARLY_RETURN && s->live == 1)) seg_retire(s);
  else list_touch(&G.vars, s);
}

// Resizes block i in place when the grains allow it; 0 if the caller must move.
static int var_resize(seg_t *s, uint8_t *p, size_t size) {
  size_t i = (size_t)(p - (s->start + s->hdr)) / GRAIN;
  size_t n = ROUND_UP(size, GRAIN) / GRAIN;
  uint8_t *bb = var_begin(s), *ub = var_usedbm(s);
  size_t len = var_len(s, i, NULL);
  if (n == len) return 1;
  if (n < len) {
    bit_set(bb, i + n);
    if (i + len < s->cap && !bit_get(ub, i + len)) bit_clr(bb, i + len);
    memset(p + n * GRAIN, 0xDD, (len - n) * GRAIN);
    POISON(p + n * GRAIN, (len - n) * GRAIN);
  } else {
    size_t j = i + len;
    if (j >= s->cap || bit_get(ub, j)) return 0;
    size_t len2 = var_len(s, j, NULL);
    if (len + len2 < n) return 0;
    bit_clr(bb, j);
    if (len + len2 > n) bit_set(bb, i + n);
    UNPOISON(p + len * GRAIN, (n - len) * GRAIN);
  }
  s->used = s->used - (uint32_t)(len * GRAIN) + (uint32_t)(n * GRAIN);
  var_recount(s);
  return 1;
}

// --------------------------------------------------------------- interface

static int classify(size_t size) {
  for (int c = 0; c < G.ncls; c++) if (size <= G.cls_size[c]) return c;
  return size <= G.var_max ? CAT_VAR : CAT_DED;
}

static void *slab_do_malloc(size_t size, unsigned *out_steps) {
  unsigned steps = 0;
  if (size == 0) size = 1;
  int cat = classify(size);
  void *p;
  if (cat >= 0) p = slab_alloc(cat, &steps);
  else if (cat == CAT_VAR) p = var_alloc(size, &steps);
  else {
    seg_t *s = seg_new(K_DED, ROUND_UP(size, SEG_ALIGN), &steps);
    p = NULL;
    if (s) { s->live = 1; s->used = (uint32_t)(s->end - s->start); p = s->start; }
  }
  if (out_steps) *out_steps += steps;
  return p;
}

static size_t usable_in(seg_t *s, const uint8_t *p) {
  switch (s->kind) {
  case K_SLAB: return G.cls_size[s->cls];
  case K_VAR: return var_len(s, (size_t)(p - (s->start + s->hdr)) / GRAIN, NULL) * GRAIN;
  default: return (size_t)(s->end - s->start);
  }
}

static void slab_do_free(void *ptr, unsigned *out_steps) {
  if (!ptr) return;
  unsigned steps = 0;
  seg_t *s = led_owner(ptr, &steps);
  if (out_steps) *out_steps += steps;
  if (!s) die("free of a pointer outside every segment");
  switch (s->kind) {
  case K_SLAB: slab_free(s, ptr); break;
  case K_VAR: var_free(s, ptr); break;
  case K_DED:
    if (ptr != s->start) die("free of an interior pointer (dedicated)");
    s->live = 0;
    seg_retire(s);
    break;
  default: die("free into a cached segment");
  }
}

static void *slab_do_realloc(void *ptr, size_t size, unsigned *out_steps) {
  if (!ptr) return slab_do_malloc(size, out_steps);
  if (size == 0) { slab_do_free(ptr, out_steps); return NULL; }
  unsigned steps = 0;
  seg_t *s = led_owner(ptr, &steps);
  if (!s) die("realloc of a pointer outside every segment");
  int cat = classify(size);
  uint8_t *p = ptr;
  if (s->kind == K_SLAB) {
    // Same class, or a smaller class that would still use over half the
    // slot: stay. The caller keeps the slack either way.
    uint32_t c = G.cls_size[s->cls];
    if (size <= c && cat >= 0 && size * 2 > c) goto in_place;
  } else if (s->kind == K_VAR) {
    if (cat == CAT_VAR && var_resize(s, p, size)) goto in_place;
  } else if (s->kind == K_DED && cat == CAT_DED) {
    size_t have = (size_t)(s->end - s->start), want = ROUND_UP(size, SEG_ALIGN);
    if (want < have) {
      // Give the tail back: it merges with the extent after it, if any.
      memset(s->start + want, 0xDD, have - want);
      POISON(s->start + want, have - want);
      pool_return(s->start + want, have - want);
      s->end = s->start + want;
      s->used = (uint32_t)want;
      G.events++;
      goto in_place;
    }
    if (want == have) goto in_place;
    if (pool_take_at(s->end, want - have)) {
      UNPOISON(s->end, want - have);
      s->end = s->start + want;
      s->used = (uint32_t)want;
      G.events++;
      goto in_place;
    }
  }
  {
    size_t old = usable_in(s, p);
    void *n = slab_do_malloc(size, &steps);
    if (out_steps) *out_steps += steps;
    if (!n) return NULL;
    memcpy(n, p, old < size ? old : size);
    slab_do_free(p, NULL);
    return n;
  }
in_place:
  if (out_steps) *out_steps += steps;
  return ptr;
}

static size_t slab_usable_size(const void *ptr) {
  seg_t *s = led_owner(ptr, NULL);
  return s ? usable_in(s, ptr) : 0;
}

// ------------------------------------------------------------------ stats

static void slab_stats(vmalloc_stats_t *out) {
  size_t used = 0, payload = 0, blocks = 0, reserved = 0, hdrs = 0, inside = 0, cached_b = 0;
  size_t live = 0, cachedn = 0, dedicated = 0, largest = 0;
  for (size_t i = 0; i < G.led_n; i++) {
    seg_t *s = G.led[i];
    size_t size = (size_t)(s->end - s->start);
    reserved += size;
    if (s->kind == K_CACHED) { cachedn++; cached_b += size; continue; }
    live++;
    blocks += s->live;
    payload += s->used;
    if (s->kind == K_DED) { dedicated++; used += size; continue; }
    // Header = everything in the segment that can never hold a block: the
    // fixed prefix, the bitmaps, and a slab's tail shorter than one slot.
    size_t capacity = s->kind == K_SLAB ? (size_t)s->cap * G.cls_size[s->cls] : (size_t)s->cap * GRAIN;
    hdrs += size - capacity;
    used += size - capacity + s->used;
    inside += capacity - s->used;
    if (s->kind == K_VAR && (size_t)s->largest * GRAIN > largest) largest = (size_t)s->largest * GRAIN;
  }
  used += G.led_n * LEDGER_ENTRY;
  size_t pool_free = 0, pool_largest = 0;
  for (size_t i = 0; i < G.ext_n; i++) {
    pool_free += G.ext[i].size;
    if (G.ext[i].size > pool_largest) pool_largest = G.ext[i].size;
  }
  if (pool_largest > largest) largest = pool_largest; // a dedicated segment can take a whole extent
  out->used_bytes = used;
  out->used_payload_bytes = payload;
  out->free_bytes = pool_free + inside + cached_b;
  out->largest_free_block = largest;
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

// ------------------------------------------------------------------ check

static int fail(const char *what) {
  fprintf(stderr, "slab check: %s\n", what);
  return 0;
}

static int check_list(list_t *l, int kind, int cls) {
  for (size_t i = 0; i < l->n; i++) {
    seg_t *s = l->v[i];
    if (!s->listed || s->kind != kind || (kind == K_SLAB && s->cls != cls)) return fail("list holds a segment of the wrong kind or class");
    if (led_owner(s->start, NULL) != s) return fail("listed segment not found through the ledger");
    if (G.pick == PICK_LOW && i && l->v[i - 1]->start >= s->start) return fail("pick=low list out of address order");
  }
  return 1;
}

static int slab_check(void) {
  // Ledger: sorted, disjoint, aligned, and together with the free extents
  // tiling the pool exactly.
  const uint8_t *prev_end = G.pool;
  size_t ei = 0, accounted = 0, cachedn = 0, listed = 0;
  for (size_t i = 0; i < G.ext_n; i++) {
    if (G.ext[i].size == 0 || (G.ext[i].size % SEG_ALIGN)) return fail("extent size 0 or not a multiple of 16");
    if (i && G.ext[i - 1].base + G.ext[i - 1].size >= G.ext[i].base) return fail("extents out of order, overlapping or not coalesced");
  }
  for (size_t i = 0; i < G.led_n; i++) {
    seg_t *s = G.led[i];
    if (i && G.led[i - 1]->start >= s->start) return fail("ledger not sorted by start");
    while (ei < G.ext_n && G.ext[ei].base < s->start) {
      if (G.ext[ei].base != prev_end) return fail("gap or overlap before extent");
      prev_end = G.ext[ei].base + G.ext[ei].size;
      accounted += G.ext[ei].size;
      ei++;
    }
    if (s->start != prev_end) return fail("gap or overlap before segment");
    if (((uintptr_t)s->start & (SEG_ALIGN - 1)) || s->end <= s->start || ((size_t)(s->end - s->start) % SEG_ALIGN))
      return fail("segment not 16-aligned or empty");
    prev_end = s->end;
    size_t size = (size_t)(s->end - s->start);
    accounted += size;
    if (s->listed) listed++;
    switch (s->kind) {
    case K_SLAB: {
      if (size != G.seg_size || s->cls < 0 || s->cls >= G.ncls) return fail("slab size or class out of range");
      if (s->start[0] != (uint8_t)s->cls || s->start[1] != K_SLAB) return fail("slab header disagrees with the ledger");
      if (s->cap != G.cls_cap[s->cls] || s->hdr != G.cls_hdr[s->cls]) return fail("slab geometry drift");
      const uint8_t *bm = s->start + HDR_FIXED;
      size_t pop = 0;
      for (size_t k = 0; k < s->cap; k++) pop += (size_t)bit_get(bm, k);
      for (size_t k = s->cap; k < (s->hdr - HDR_FIXED) * 8; k++) if (bit_get(bm, k)) return fail("bit set past slab capacity");
      if (pop != s->live) return fail("slab used bits != live count");
      if (s->used != s->live * G.cls_size[s->cls]) return fail("slab used bytes drift");
      if (s->live == 0) return fail("empty slab not retired");
      if (s->listed != (s->live < s->cap)) return fail("slab partial-list membership wrong");
      for (size_t k = 0; k < s->hint && k < s->cap; k++) if (!bit_get(bm, k)) return fail("free slot below the hint");
      break;
    }
    case K_VAR: {
      if (size != G.seg_size || s->start[1] != K_VAR || s->cap != G.var_grains) return fail("var segment header or geometry wrong");
      const uint8_t *bb = s->start + HDR_FIXED, *ub = bb + G.var_bm;
      if (!bit_get(bb, 0)) return fail("var segment has no block at grain 0");
      size_t live = 0, usedg = 0, best = 0;
      int prev_free = 0;
      for (size_t k = 0; k < s->cap;) {
        size_t j = next_set(bb, k, s->cap, NULL), len = j - k;
        for (size_t m = k + 1; m < j; m++) if (bit_get(ub, m)) return fail("used bit inside a block");
        if (bit_get(ub, k)) { live++; usedg += len; prev_free = 0; }
        else {
          if (prev_free) return fail("two adjacent free var blocks");
          if (len > best) best = len;
          prev_free = 1;
        }
        k = j;
      }
      if (live != s->live || usedg * GRAIN != s->used || best != s->largest) return fail("var counts drift");
      if (live == 0) return fail("empty var segment not retired");
      if (!s->listed) return fail("var segment not on the var list");
      break;
    }
    case K_DED:
      if (s->live != 1 || s->used != size || s->listed) return fail("dedicated segment state wrong");
      break;
    case K_CACHED:
      if (size != G.seg_size || s->live || s->listed || s->start[1] != K_CACHED) return fail("cached segment state wrong");
      cachedn++;
      break;
    default:
      return fail("unknown segment kind");
    }
  }
  while (ei < G.ext_n) {
    if (G.ext[ei].base != prev_end) return fail("gap or overlap before trailing extent");
    prev_end = G.ext[ei].base + G.ext[ei].size;
    accounted += G.ext[ei].size;
    ei++;
  }
  if (accounted != G.pool_size) return fail("segments + extents do not tile the pool");
  if (cachedn != G.cached || cachedn > G.cache_max) return fail("cache count drift");
  size_t on_lists = G.vars.n;
  for (int c = 0; c < G.ncls; c++) {
    if (!check_list(&G.part[c], K_SLAB, c)) return 0;
    on_lists += G.part[c].n;
  }
  if (!check_list(&G.vars, K_VAR, -1)) return 0;
  if (on_lists != listed) return fail("list lengths disagree with listed flags");
  return 1;
}

// ----------------------------------------------------------- init/config

static int geometry(void) {
  size_t S = G.seg_size;
  if (S < 256 || S % SEG_ALIGN || S > 65536) return -1;
  for (int c = 0; c < G.ncls; c++) {
    size_t sz = G.cls_size[c];
    if (sz == 0 || sz % GRAIN || (c && sz <= G.cls_size[c - 1])) return -1;
    size_t cap = (S - HDR_FIXED) / sz;
    while (cap && HDR_FIXED + ROUND_UP((cap + 7) / 8, GRAIN) + cap * sz > S) cap--;
    if (cap < 2) return -1;
    G.cls_cap[c] = (uint32_t)cap;
    G.cls_hdr[c] = (uint32_t)(HDR_FIXED + ROUND_UP((cap + 7) / 8, GRAIN));
  }
  size_t g = (S - HDR_FIXED) / GRAIN;
  while (HDR_FIXED + 2 * ROUND_UP((g + 7) / 8, GRAIN) + g * GRAIN > S) g--;
  G.var_grains = (uint32_t)g;
  G.var_bm = (uint32_t)ROUND_UP((g + 7) / 8, GRAIN);
  G.var_hdr = HDR_FIXED + 2 * G.var_bm;
  // A mid-size block larger than a var segment can hold goes dedicated.
  if (G.var_max > (size_t)g * GRAIN) G.var_max = (size_t)g * GRAIN;
  return 0;
}

static int slab_init(void *pool, size_t pool_size) {
  if (((uintptr_t)pool & (SEG_ALIGN - 1)) != 0) return -1;
  if (geometry() != 0) return -1;
  if (G.pool) UNPOISON(G.pool, G.pool_size); // a previous bisect trial's leftovers
  for (size_t i = 0; i < G.led_n; i++) free(G.led[i]);
  G.pool = pool;
  G.pool_size = pool_size & ~(size_t)(SEG_ALIGN - 1);
  G.ext_n = G.led_n = 0;
  for (int c = 0; c < MAX_CLASSES; c++) G.part[c].n = 0;
  G.vars.n = 0;
  G.cached = 0;
  G.carve_count = G.slab_allocs = G.new_segs = 0;
  G.events = G.n_added = G.n_returned = G.n_reused = G.n_dedicated = 0;
  if (G.pool_size) {
    ext_insert(0, (extent_t){G.pool, G.pool_size});
    POISON(G.pool, G.pool_size);
  }
  return 0;
}

static int seg_kind_public(const seg_t *s) {
  return s->kind == K_DED ? 1 : s->kind == K_CACHED ? 2 : 0;
}

static int slab_owner_q(const void *p, vmalloc_seg_t *out) {
  seg_t *s = led_owner(p, NULL);
  if (!s) return 0;
  out->base = s->start; out->size = (size_t)(s->end - s->start);
  out->kind = seg_kind_public(s); out->live_blocks = s->live;
  return 1;
}

static int slab_segment_at(size_t i, vmalloc_seg_t *out) {
  if (i >= G.led_n) return 0;
  seg_t *s = G.led[i];
  out->base = s->start; out->size = (size_t)(s->end - s->start);
  out->kind = seg_kind_public(s); out->live_blocks = s->live;
  return 1;
}

static unsigned long slab_events(void) { return G.events; }

static int slab_configure(const char *key, const char *value) {
  if (!strcmp(key, "seg_size")) { G.seg_size = strtoull(value, NULL, 0); return 0; }
  if (!strcmp(key, "cache")) { G.cache_max = strtoull(value, NULL, 0); return 0; }
  if (!strcmp(key, "var_max")) { G.var_max = strtoull(value, NULL, 0); return 0; }
  if (!strcmp(key, "classes")) {
    if (!strcmp(value, "none")) { G.ncls = 0; return 0; } // every size up to var_max goes to var segments
    int n = 0;
    const char *p = value;
    while (*p && n < MAX_CLASSES) {
      char *e;
      unsigned long v = strtoul(p, &e, 0);
      if (e == p) return -1;
      G.cls_size[n++] = (uint32_t)v;
      p = *e == ',' ? e + 1 : e;
      if (*e && *e != ',') return -1;
    }
    if (*p || n == 0) return -1;
    G.ncls = n;
    return 0;
  }
  if (!strcmp(key, "carve")) {
    if (!strcmp(value, "low")) G.carve = CARVE_LOW;
    else if (!strcmp(value, "best")) G.carve = CARVE_BEST;
    else if (!strcmp(value, "split")) G.carve = CARVE_SPLIT;
    else return -1;
    return 0;
  }
  if (!strcmp(key, "pick")) {
    if (!strcmp(value, "low")) G.pick = PICK_LOW;
    else if (!strcmp(value, "recent")) G.pick = PICK_RECENT;
    else return -1;
    return 0;
  }
  if (!strcmp(key, "fault")) {
    if (!strcmp(value, "none")) G.fault = FAULT_NONE;
    else if (!strcmp(value, "early-return")) G.fault = FAULT_EARLY_RETURN;
    else if (!strcmp(value, "overlap")) G.fault = FAULT_OVERLAP;
    else if (!strcmp(value, "pool-overlap")) G.fault = FAULT_POOL_OVERLAP;
    else if (!strcmp(value, "ledger-order")) G.fault = FAULT_LEDGER_ORDER;
    else if (!strcmp(value, "bitmap")) G.fault = FAULT_BITMAP;
    else return -1;
    return 0;
  }
  return -1;
}

static const vmalloc_backend_t BACKEND = {
    .name = "slab",
    .init = slab_init,
    .do_malloc = slab_do_malloc,
    .do_realloc = slab_do_realloc,
    .do_free = slab_do_free,
    .stats = slab_stats,
    .check = slab_check,
    .owner = slab_owner_q,
    .segment_at = slab_segment_at,
    .events = slab_events,
    .configure = slab_configure,
    .usable_size = slab_usable_size,
};

const vmalloc_backend_t *vmalloc_slab_backend(void) { return &BACKEND; }
