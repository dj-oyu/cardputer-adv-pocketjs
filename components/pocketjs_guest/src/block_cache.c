#include "pocketjs/block_cache.h"

#include <string.h>

// Poisoning, the link word and the hit paths are in block_cache.h (inline,
// shared with the guest's allocator).
#define BC_POISON BLOCK_CACHE_POISON
#define BC_UNPOISON BLOCK_CACHE_UNPOISON
#define BC_NEXT(block) (((block_cache_link_t *)(block))->next)

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

void *block_cache_malloc(block_cache_t *cache, size_t size, size_t rounded) {
  const block_cache_backend_t *b = cache->backend;
  void *block = block_cache_pop(cache, rounded);
  if (block != NULL) return block;
  block = b->malloc(b->ctx, size);
  if (block == NULL && cache->enabled && block_cache_held(cache) != 0) {
    // Small blocks held here may be what keeps the heap from having room
    // (they cannot merge with their free neighbours): give them back first.
    block_cache_flush(cache);
    block = b->malloc(b->ctx, size);
  }
  return block;
}

void block_cache_free(block_cache_t *cache, void *block) {
  if (block == NULL) return;
  const block_cache_backend_t *b = cache->backend;
  block_cache_free_sized(cache, block, cache->enabled ? b->length(b->ctx, block) : 0);
}

void block_cache_free_sized(block_cache_t *cache, void *block, size_t length) {
  const block_cache_backend_t *b = cache->backend;
  if (block == NULL) return;
  if (!block_cache_push(cache, block, length)) b->free(b->ctx, block);
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
