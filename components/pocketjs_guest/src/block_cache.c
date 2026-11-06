#include "pocketjs/block_cache.h"

#include <string.h>

// Under ASan a cached block is poisoned: to the sanitizer it stays "freed"
// until the cache hands it out again, so a use-after-free the cache would
// otherwise hide (the block is still allocated as far as malloc knows) is
// still reported. The first word is the list link and is unpoisoned only
// while the cache itself reads or writes it.
#if defined(__SANITIZE_ADDRESS__)
#define BC_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BC_ASAN 1
#endif
#endif
#ifdef BC_ASAN
#include <sanitizer/asan_interface.h>
#define BC_POISON(p, n) ASAN_POISON_MEMORY_REGION((p), (n))
#define BC_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define BC_POISON(p, n) ((void)0)
#define BC_UNPOISON(p, n) ((void)0)
#endif

// The list link is the block's first word. Blocks come from the backend's
// malloc, so the word is pointer aligned; may_alias keeps the accesses legal
// under strict aliasing (docs/vm/aliasing-types.md). memcpy said the same,
// but -Os GCC on Xtensa made every pointer-sized memcpy a call to ROM memcpy
// (236 calls a STRESS turn, docs/vm/qjs-observe.md sec.3.3.1).
typedef struct {
  void *next;
} __attribute__((may_alias)) bc_link_t;
#define BC_NEXT(block) (((bc_link_t *)(block))->next)

void block_cache_init(block_cache_t *cache, const block_cache_backend_t *backend,
                      const uint16_t *sizes, unsigned n, bool enabled) {
  memset(cache, 0, sizeof(*cache));
  cache->backend = backend;
  cache->enabled = enabled;
  if (n > BLOCK_CACHE_CLASSES) n = BLOCK_CACHE_CLASSES;
  for (unsigned i = 0; i < n; i++) {
    const uint16_t s = sizes[i];
    if (s < sizeof(void *) || s > BLOCK_CACHE_MAX_SIZE || (s & 3U)) continue;
    cache->size[i] = s;
    cache->class_of[s / 4U] = (uint8_t)(i + 1U);
  }
}

void block_cache_set_enabled(block_cache_t *cache, bool enabled) {
  if (cache->enabled == enabled) return;
  if (!enabled) block_cache_flush(cache);
  cache->enabled = enabled;
}

static int class_index(const block_cache_t *cache, size_t length) {
  if (length > BLOCK_CACHE_MAX_SIZE || (length & 3U)) return -1;
  return (int)cache->class_of[length / 4U] - 1;
}

void *block_cache_malloc(block_cache_t *cache, size_t size, size_t rounded) {
  const block_cache_backend_t *b = cache->backend;
  if (cache->enabled) {
    const int c = class_index(cache, rounded);
    if (c >= 0 && cache->head[c] != NULL) {
      void *block = cache->head[c];
      BC_UNPOISON(block, cache->size[c]);
      cache->head[c] = BC_NEXT(block);
      cache->count[c]--;
      cache->hits++;
      return block;
    }
    cache->misses++;
  }
  void *block = b->malloc(b->ctx, size);
  if (block == NULL && cache->enabled && block_cache_held(cache) != 0) {
    // Small blocks held here may be what keeps the heap from having room
    // (they cannot merge with their free neighbours): give them back first.
    block_cache_flush(cache);
    block = b->malloc(b->ctx, size);
  }
  return block;
}

void block_cache_free(block_cache_t *cache, void *block) {
  const block_cache_backend_t *b = cache->backend;
  if (block == NULL) return;
  if (cache->enabled) {
    const int c = class_index(cache, b->length(b->ctx, block));
    if (c >= 0 && cache->count[c] < BLOCK_CACHE_DEPTH) {
      BC_NEXT(block) = cache->head[c];
      cache->head[c] = block;
      cache->count[c]++;
      cache->pushes++;
      BC_POISON(block, cache->size[c]);
      return;
    }
  }
  b->free(b->ctx, block);
}

void block_cache_flush(block_cache_t *cache) {
  const block_cache_backend_t *b = cache->backend;
  for (unsigned c = 0; c < BLOCK_CACHE_CLASSES; c++) {
    void *block = cache->head[c];
    while (block != NULL) {
      BC_UNPOISON(block, cache->size[c]);
      void *next = BC_NEXT(block);
      b->free(b->ctx, block);
      block = next;
    }
    cache->head[c] = NULL;
    cache->count[c] = 0;
  }
  cache->flushes++;
}

size_t block_cache_held(const block_cache_t *cache) {
  size_t held = 0;
  for (unsigned c = 0; c < BLOCK_CACHE_CLASSES; c++)
    held += (size_t)cache->count[c] * cache->size[c];
  return held;
}
