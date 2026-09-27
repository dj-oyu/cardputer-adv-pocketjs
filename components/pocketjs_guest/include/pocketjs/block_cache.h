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
#define BLOCK_CACHE_DEPTH 8
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

// `rounded`: the length the backend would give the request (the caller's
// rounding, e.g. tlsf's 4 B / 12 B minimum). A miss, or a length not listed,
// goes to the backend; a backend NULL flushes the cache and retries once.
void *block_cache_malloc(block_cache_t *cache, size_t size, size_t rounded);

// Pushes the block if its real length is a listed class with room, else frees
// it to the backend.
void block_cache_free(block_cache_t *cache, void *block);

// Every cached block back to the backend.
void block_cache_flush(block_cache_t *cache);

// Bytes the cache holds right now (outside the guest's accounting).
size_t block_cache_held(const block_cache_t *cache);

#ifdef __cplusplus
}
#endif
