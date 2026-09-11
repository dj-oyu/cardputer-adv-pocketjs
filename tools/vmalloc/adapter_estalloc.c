// adapter_estalloc.c — wraps vendor/estalloc (picoruby/estalloc, pinned
// dd3988acb10ae5e14f1cbf3991753f12c4dcf9d2, BSD-3-Clause; NOT vendored into
// the firmware, this is a host comparison only). estalloc.c carries one
// documented VMALLOC PATCH (a step counter, see the file) and is otherwise
// unmodified from that commit.
#include "vmalloc.h"
#include "vendor/estalloc/estalloc.h"
#include <string.h>

extern unsigned int vmalloc_estalloc_last_steps(void);

static ESTALLOC *g_est;

static int est_backend_init(void *pool, size_t pool_size) {
  g_est = est_init(pool, (unsigned int)pool_size);
  return g_est ? 0 : -1;
}

static void *est_do_malloc(size_t size, unsigned *out_steps) {
  void *p = est_malloc(g_est, (unsigned int)size);
  if (out_steps) *out_steps = vmalloc_estalloc_last_steps();
  return p;
}

static void *est_do_realloc(void *ptr, size_t size, unsigned *out_steps) {
  // est_realloc grows/shrinks in place when the following block is free
  // and big enough (no search, out_steps left at the caller's default 0);
  // otherwise it falls through to est_malloc + copy + est_free, which is
  // where the step count comes from.
  if (out_steps) *out_steps = 0;
  void *p = est_realloc(g_est, ptr, (unsigned int)size);
  unsigned s = vmalloc_estalloc_last_steps();
  if (out_steps && s) *out_steps = s;
  return p;
}

static void est_do_free(void *ptr, unsigned *out_steps) {
  if (out_steps) *out_steps = 0;
  est_free(g_est, ptr);
}

static void est_stats(vmalloc_stats_t *out) {
  est_take_statistics(g_est);
  out->used_bytes = g_est->stat.used;
  out->free_bytes = g_est->stat.free;
  out->largest_free_block = g_est->stat.max_free;
  out->blocks_used = 0; // est_take_statistics does not count blocks; left 0, not fabricated.
}

static int est_do_check(void) {
  est_take_statistics(g_est);
  return g_est->error_message == NULL ? 1 : 0;
}

static const vmalloc_backend_t BACKEND = {
    .name = "estalloc",
    .init = est_backend_init,
    .do_malloc = est_do_malloc,
    .do_realloc = est_do_realloc,
    .do_free = est_do_free,
    .stats = est_stats,
    .check = est_do_check,
};

const vmalloc_backend_t *vmalloc_estalloc_backend(void) { return &BACKEND; }
