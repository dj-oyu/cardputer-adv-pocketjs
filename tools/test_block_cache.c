// R3's small-block cache (components/pocketjs_guest/src/block_cache.c) on the
// host under ASan/UBSan: random malloc/free against a model, the flush-and-
// retry path on a backend failure, and (with an argument) that a use after
// free through a cached block is still reported. WSL:
//   gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
//     -I components/pocketjs_guest/include tools/test_block_cache.c
//     components/pocketjs_guest/src/block_cache.c -o /tmp/tbc && /tmp/tbc
//   /tmp/tbc uaf   # must abort with heap-use-after-free... from ASan
#include "pocketjs/block_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { size_t length; size_t pad; } hdr_t;
static unsigned backend_live, fail_next, backend_calls;
static void *be_malloc(void *ctx, size_t n) {
  (void)ctx;
  backend_calls++;
  if (fail_next) { fail_next--; return NULL; }
  size_t len = (n + 3) & ~(size_t)3; if (len < 12) len = 12;
  hdr_t *h = malloc(sizeof(hdr_t) + len);
  h->length = len; backend_live++;
  return h + 1;
}
static void be_free(void *ctx, void *p) { (void)ctx; backend_live--; free((hdr_t *)p - 1); }
static size_t be_length(void *ctx, const void *p) { (void)ctx; return ((const hdr_t *)p - 1)->length; }
static const block_cache_backend_t BE = {be_malloc, be_free, be_length, NULL};
static size_t rounded(size_t n) { n = (n + 3) & ~(size_t)3; return n < 12 ? 12 : n; }

static unsigned failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); failures++; } } while (0)

enum { SLOTS = 512 };
static struct { unsigned char *p; size_t n; unsigned char tag; } live[SLOTS];

static void pressure_retention(const uint16_t *sizes, unsigned n) {
  block_cache_t cache;
  block_cache_init(&cache, &BE, sizes, n, true);
  unsigned char *live_block = block_cache_malloc(&cache, 48, 48);
  memset(live_block, 0xa5, 48);
  void *held[BLOCK_CACHE_CLASSES][BLOCK_CACHE_DEPTH];
  size_t cap = 0;
  for (unsigned c = 0; c < n; c++) {
    cap += sizes[c] * BLOCK_CACHE_DEPTH;
    for (unsigned i = 0; i < BLOCK_CACHE_DEPTH; i++)
      held[c][i] = block_cache_malloc(&cache, sizes[c], sizes[c]);
  }
  for (unsigned c = 0; c < n; c++)
    for (unsigned i = 0; i < BLOCK_CACHE_DEPTH; i++)
      block_cache_free(&cache, held[c][i]);
  CHECK(block_cache_held(&cache) == cap, "all pressure-test classes not full");
  CHECK(backend_live == 1 + n * BLOCK_CACHE_DEPTH, "unexpected backend live count");

  block_cache_set_enabled(&cache, false);
  CHECK(!cache.enabled && block_cache_held(&cache) == 0 && backend_live == 1,
        "pressure did not release all idle blocks");
  unsigned flushes = cache.flushes;
  for (unsigned turn = 0; turn < 1000; turn++) {
    // A repeated pressure sample must not flush twice or let the next turn
    // retain its frees again. A live guest allocation remains untouched.
    block_cache_set_enabled(&cache, false);
    for (unsigned c = 0; c < n; c++) {
      void *p = block_cache_malloc(&cache, sizes[c], sizes[c]);
      CHECK(p != NULL, "backend allocation failed during pressure");
      block_cache_free(&cache, p);
    }
    CHECK(block_cache_held(&cache) == 0 && backend_live == 1,
          "cache retained a freed block during sustained pressure");
  }
  CHECK(cache.flushes == flushes, "repeated pressure flushed an empty cache");
  for (unsigned i = 0; i < 48; i++)
    CHECK(live_block[i] == 0xa5, "pressure changed a live guest block");
  unsigned calls = backend_calls;
  fail_next = 1;
  CHECK(block_cache_malloc(&cache, 48, 48) == NULL,
        "disabled empty cache retried a backend failure");
  CHECK(backend_calls == calls + 1 && cache.flushes == flushes,
        "disabled cache performed an unnecessary flush/retry");
  block_cache_flush(&cache); // Dormancy/cleanup never re-enables retention.
  block_cache_free(&cache, live_block);
  CHECK(!cache.enabled && backend_live == 0, "flush re-enabled or leaked");

  block_cache_set_enabled(&cache, true);
  void *p = block_cache_malloc(&cache, 48, 48);
  block_cache_free(&cache, p);
  CHECK(block_cache_held(&cache) == 48, "recovery did not restore retention");
  unsigned hits = cache.hits;
  void *q = block_cache_malloc(&cache, 48, 48);
  CHECK(q == p && cache.hits == hits + 1, "recovery did not restore cache hits");
  block_cache_set_enabled(&cache, true); // A live block survives this too.
  block_cache_free(&cache, q);
  block_cache_set_enabled(&cache, false);
  CHECK(backend_live == 0 && block_cache_held(&cache) == 0,
        "second pressure episode leaked");
  printf("PRESSURE_RETENTION released_usable=%zu held_during_pressure=0 turns=1000\n", cap);
}

int main(int argc, char **argv) {
  static const uint16_t sizes[] = {12, 16, 32, 36, 48, 72, 80, 88};
  block_cache_t cache;
  block_cache_init(&cache, &BE, sizes, 8, true);

  if (argc > 1 && !strcmp(argv[1], "uaf")) {
    unsigned char *p = block_cache_malloc(&cache, 40, rounded(40));
    block_cache_free(&cache, p);       // 40 -> 40 B, not a class: goes to the backend
    unsigned char *q = block_cache_malloc(&cache, 48, rounded(48));
    block_cache_free(&cache, q);       // 48 B: cached, poisoned
    printf("reading a cached block (ASan must stop here)\n");
    return q[8];
  }

  srand(12345);
  const size_t pick[] = {5, 12, 13, 16, 30, 32, 36, 40, 48, 60, 72, 80, 88, 100, 200};
  const size_t cap = 8 * (12 + 16 + 32 + 36 + 48 + 72 + 80 + 88);
  for (unsigned step = 0; step < 400000; step++) {
    unsigned s = (unsigned)rand() % SLOTS;
    if (live[s].p) {
      for (size_t i = 0; i < live[s].n; i++)
        if (live[s].p[i] != live[s].tag) { CHECK(0, "slot %u byte %zu corrupted", s, i); break; }
      block_cache_free(&cache, live[s].p);
      live[s].p = NULL;
    } else {
      size_t n = pick[(unsigned)rand() % (sizeof(pick) / sizeof(pick[0]))];
      if ((rand() & 1023) == 0) fail_next = 1;   // a backend failure now and then
      unsigned char *p = block_cache_malloc(&cache, n, rounded(n));
      if (!p) { CHECK(fail_next == 0, "NULL without a pending failure"); fail_next = 0; continue; }
      CHECK(be_length(NULL, p) >= n, "block shorter than the request");
      for (unsigned t = 0; t < SLOTS; t++) CHECK(live[t].p != p, "block handed out twice");
      live[s].p = p; live[s].n = n; live[s].tag = (unsigned char)(step * 7 + 1);
      memset(p, live[s].tag, n);
    }
    CHECK(block_cache_held(&cache) <= cap, "cache holds more than its bound");
  }
  // One backend failure with blocks cached: the cache must give them back
  // and the retry must succeed.
  unsigned char *keep[8];
  for (int i = 0; i < 8; i++) keep[i] = block_cache_malloc(&cache, 48, 48);
  for (int i = 0; i < 8; i++) block_cache_free(&cache, keep[i]);
  CHECK(block_cache_held(&cache) >= 8 * 48, "48 B class not full before the failure test");
  unsigned flushes = cache.flushes;
  fail_next = 1;
  unsigned char *r = block_cache_malloc(&cache, 200, 200);
  CHECK(r != NULL, "retry after flush failed");
  CHECK(cache.flushes == flushes + 1 && block_cache_held(&cache) == 0, "failure did not flush");
  block_cache_free(&cache, r);
  for (unsigned s = 0; s < SLOTS; s++) if (live[s].p) block_cache_free(&cache, live[s].p);
  block_cache_flush(&cache);
  CHECK(backend_live == 0, "%u backend blocks leaked", backend_live);
  // Disabled: nothing is ever cached.
  block_cache_t off;
  block_cache_init(&off, &BE, sizes, 8, false);
  void *x = block_cache_malloc(&off, 48, 48);
  block_cache_free(&off, x);
  CHECK(block_cache_held(&off) == 0 && backend_live == 0, "disabled cache kept a block");
  pressure_retention(sizes, sizeof(sizes) / sizeof(sizes[0]));
  printf("hits=%u misses=%u pushes=%u flushes=%u\n", cache.hits, cache.misses, cache.pushes, cache.flushes);
  printf("%s: %u failure(s)\n", failures ? "BLOCK_CACHE FAIL" : "BLOCK_CACHE PASS", failures);
  return failures ? 1 : 0;
}
