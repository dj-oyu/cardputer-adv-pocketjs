// R3: a small-block cache under the guest allocator (docs/vm/r3-small-block-cache.md).
//
// The guest's JS frees most blocks the moment their refcount drops, and a few
// sizes (QuickJS's 32-bit structs) are most of every frame's allocations. A
// heap_caps malloc/free pair is ~1,800 cycles on the device; this keeps up to
// BLOCK_CACHE_DEPTH freed blocks of each listed size and hands them straight
// back to the next allocation of that size.
//
// No ESP-IDF here: the backend (malloc, free, block length) is passed in, so
// the device (heap_caps) and the host runner (glibc under a size header) run
// the same code. The cache owns nothing beyond its lists; blocks in it are
// ordinary backend blocks, returned by block_cache_flush().
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLOCK_CACHE_CLASSES 8
// 16 since 2026-11-06 (8 before): fewer misses into the heap's deep call
// chain for ~2.2 KB more held at most (docs/vm/qjs-observe.md sec.17).
#ifndef BLOCK_CACHE_DEPTH
#define BLOCK_CACHE_DEPTH 16
#endif
// Under native-heap pressure the cache keeps this many per class instead of
// stopping: at most ~1.1 KB held instead of 4.4 KB, and in the host replay of
// STRESS/DERBY 82%/89% of the full depth's hits (docs/perf/frame-breakdown.md
// sec.9). Stopping it cost the JS turn 7-13% (sec.7).
#ifndef BLOCK_CACHE_PRESSURE_DEPTH
#define BLOCK_CACHE_PRESSURE_DEPTH 4
#endif
#define BLOCK_CACHE_MAX_SIZE 252  // largest class a lookup table covers (4 B steps)

typedef struct {
  void *(*malloc)(void *ctx, size_t size);
  void (*free)(void *ctx, void *block);
  size_t (*length)(void *ctx, const void *block);  // the block's real length
  void *ctx;
} block_cache_backend_t;

typedef struct {
  const block_cache_backend_t *backend;
  bool enabled;
  uint8_t depth;                                   // blocks kept per class, <= BLOCK_CACHE_DEPTH
  uint8_t class_of[BLOCK_CACHE_MAX_SIZE / 4 + 1];  // length/4 -> class+1, 0 = not cached
  uint16_t size[BLOCK_CACHE_CLASSES];
  uint8_t count[BLOCK_CACHE_CLASSES];
  void *head[BLOCK_CACHE_CLASSES];                 // singly linked through each block's first word
  uint32_t hits, misses, pushes, flushes;          // for the probe build and the host test
} block_cache_t;

// `sizes`: up to BLOCK_CACHE_CLASSES block lengths, multiples of 4, each
// <= BLOCK_CACHE_MAX_SIZE and >= sizeof(void *). `enabled` false makes every
// call go straight to the backend.
void block_cache_init(block_cache_t *cache, const block_cache_backend_t *backend,
                      const uint16_t *sizes, unsigned n, bool enabled);

// Disabling returns held blocks and routes subsequent calls to the backend.
// Re-enabling starts empty. The caller retains any backend eligibility policy.
void block_cache_set_enabled(block_cache_t *cache, bool enabled);

// Blocks kept per class from now on (1..BLOCK_CACHE_DEPTH); lowering it
// returns each class's excess to the backend at once.
void block_cache_set_depth(block_cache_t *cache, unsigned depth);

// `rounded`: the length the backend would give the request (the caller's
// rounding, e.g. tlsf's 4 B / 12 B minimum). A miss, or a length not listed,
// goes to the backend; a backend NULL flushes the cache and retries once.
void *block_cache_malloc(block_cache_t *cache, size_t size, size_t rounded);

// Pushes the block if its real length is a listed class with room, else frees
// it to the backend.
void block_cache_free(block_cache_t *cache, void *block);

// The same, for a caller that already knows the block's real length (what the
// backend's length() would return): the guest reads it inline from the tlsf
// header, saving the indirect call on every free.
void block_cache_free_sized(block_cache_t *cache, void *block, size_t length);

// Every cached block back to the backend.
void block_cache_flush(block_cache_t *cache);

// ---- the hit paths, inline (docs/vm/qjs-observe.md sec.13). The guest's
// allocator calls these and goes to heap_caps itself on a miss: a hit is no
// call at all, and a miss is two levels shallower than block_cache_malloc's
// backend call (fewer window overflows on the deepest chains).
// Under ASan a cached block is poisoned: to the sanitizer it stays "freed"
// until the cache hands it out again. The first word is the list link.
#if defined(__SANITIZE_ADDRESS__)
#define BLOCK_CACHE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BLOCK_CACHE_ASAN 1
#endif
#endif
#ifdef BLOCK_CACHE_ASAN
#include <sanitizer/asan_interface.h>
#define BLOCK_CACHE_POISON(p, n) ASAN_POISON_MEMORY_REGION((p), (n))
#define BLOCK_CACHE_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define BLOCK_CACHE_POISON(p, n) ((void)0)
#define BLOCK_CACHE_UNPOISON(p, n) ((void)0)
#endif
// The link is the block's first word; blocks come from malloc, so it is
// pointer aligned, and may_alias keeps the access legal (aliasing-types.md).
typedef struct {
  void *next;
} __attribute__((may_alias)) block_cache_link_t;

static inline __attribute__((always_inline)) int block_cache_class(const block_cache_t *cache,
                                                                    size_t length) {
  if (length > BLOCK_CACHE_MAX_SIZE || (length & 3U)) return -1;
  return (int)cache->class_of[length / 4U] - 1;
}

// A cached block of `rounded` length, or NULL (a miss, counted).
static inline __attribute__((always_inline)) void *block_cache_pop(block_cache_t *cache,
                                                                   size_t rounded) {
  if (!cache->enabled) return NULL;
  const int c = block_cache_class(cache, rounded);
  void *block = c >= 0 ? cache->head[c] : NULL;
  if (block == NULL) {
    cache->misses++;
    return NULL;
  }
  BLOCK_CACHE_UNPOISON(block, cache->size[c]);
  cache->head[c] = ((block_cache_link_t *)block)->next;
  cache->count[c]--;
  cache->hits++;
  return block;
}

// Keeps a block of real length `length` if its class has room; false: the
// caller frees it to the backend.
static inline __attribute__((always_inline)) bool block_cache_push(block_cache_t *cache, void *block,
                                                                   size_t length) {
  if (!cache->enabled) return false;
  const int c = block_cache_class(cache, length);
  if (c < 0 || cache->count[c] >= cache->depth) return false;
  ((block_cache_link_t *)block)->next = cache->head[c];
  cache->head[c] = block;
  cache->count[c]++;
  cache->pushes++;
  BLOCK_CACHE_POISON(block, cache->size[c]);
  return true;
}

// Bytes the cache holds right now (outside the guest's accounting).
size_t block_cache_held(const block_cache_t *cache);

#ifdef __cplusplus
}
#endif
