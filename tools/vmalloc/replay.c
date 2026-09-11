// replay.c — drives one allocator backend (tools/vmalloc/vmalloc.h) through
// a tools/vmtest trace (format: tools/vmtest/README.md, "トレース形式").
// One process = one allocator = one trace = one pool size, so a backend's
// static state never has to be reset between runs; the shell driver
// (run_all.sh) forks a fresh process per combination instead.
//
// Exit code: 0 the trace replayed to completion at the given pool size
// (whether or not every malloc/realloc succeeded — that's `result=FAIL` on
// stdout, not a process failure); 1 a trace/arg problem; 2 the backend's
// own integrity check failed at the end (a real allocator bug, not a
// capacity issue).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "vmalloc.h"

typedef enum { OP_MALLOC, OP_FREE, OP_REALLOC } op_kind_t;

typedef struct {
  op_kind_t kind;
  uint64_t id1; // malloc/free: the id. realloc: the OLD id.
  uint64_t id2; // realloc only: the NEW id.
  size_t size;  // malloc/realloc: requested size.
} op_t;

static op_t *g_ops;
static size_t g_op_count, g_op_cap;

static void push_op(op_kind_t kind, uint64_t id1, uint64_t id2, size_t size) {
  if (g_op_count == g_op_cap) {
    g_op_cap = g_op_cap ? g_op_cap * 2 : 4096;
    g_ops = realloc(g_ops, g_op_cap * sizeof(op_t));
    if (!g_ops) { fprintf(stderr, "replay: out of memory loading trace\n"); exit(1); }
  }
  g_ops[g_op_count++] = (op_t){kind, id1, id2, size};
}

// Parses the trace once. Comment lines ('#') are skipped; '!' and '!~'
// (the original run's --fail-alloc-forced failures, or a host malloc
// failure that never happens in practice on this host) are skipped too -
// they performed no allocation in the run that produced the trace, so they
// leave nothing to replay.
static uint64_t g_max_id;

static void load_trace(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); exit(1); }
  char line[512];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    if (line[0] == '+') {
      uint64_t id; unsigned long long sz;
      if (sscanf(line + 1, "%llu %llu", (unsigned long long *)&id, &sz) != 2) continue;
      push_op(OP_MALLOC, id, 0, (size_t)sz);
      if (id > g_max_id) g_max_id = id;
    } else if (line[0] == '-') {
      uint64_t id;
      if (sscanf(line + 1, "%llu", (unsigned long long *)&id) != 1) continue;
      push_op(OP_FREE, id, 0, 0);
    } else if (line[0] == '~') {
      uint64_t oid, nid; unsigned long long sz;
      if (sscanf(line + 1, "%llu %llu %llu", (unsigned long long *)&oid,
                 (unsigned long long *)&nid, &sz) != 3) continue;
      push_op(OP_REALLOC, oid, nid, (size_t)sz);
      if (nid > g_max_id) g_max_id = nid;
    }
    // '!' and '!~' intentionally ignored, see comment above.
  }
  fclose(f);
}

// id -> live pointer. Ids are assigned sequentially and never reused (see
// tools/vmtest/README.md), so a flat array indexed by id works; freed/
// retired ids are left NULL.
static void **g_ptr_by_id;
static size_t *g_appsize_by_id; // requested size at last (re)alloc, for realloc copy-byte accounting

typedef struct {
  unsigned *v;
  size_t n, cap;
} u32series_t;
static void series_push(u32series_t *s, unsigned x) {
  if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 256; s->v = realloc(s->v, s->cap * sizeof(unsigned)); }
  s->v[s->n++] = x;
}
static int cmp_u(const void *a, const void *b) { return (int)*(const unsigned *)a - (int)*(const unsigned *)b; }
static unsigned pctl(u32series_t *s, double p) {
  if (!s->n) return 0;
  qsort(s->v, s->n, sizeof(unsigned), cmp_u);
  size_t idx = (size_t)(p * (double)(s->n - 1));
  return s->v[idx];
}

typedef struct {
  int ok;               // 1 = every op succeeded; 0 = a malloc/realloc returned NULL
  int check_ok;         // backend->check() result at the end, or -1 if no checker
  size_t peak_used_bytes;
  size_t peak_blocks;
  size_t min_largest_free;  // worst moment for "can I still fit a big object"
  size_t max_largest_free;
  size_t realloc_copy_bytes;
  size_t fail_at_op;     // 0-based index of the failing op, if !ok
  u32series_t malloc_steps, malloc_ns;
  u32series_t realloc_steps, realloc_ns;
  u32series_t free_ns;
} run_result_t;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// sample_every: how often (in ops) to poll stats() for the fragmentation
// series. 0 disables sampling (bisection passes do, to stay fast); a real
// reporting run samples every op for small traces or every few ops for big
// ones (README.md: "largest free block and external fragmentation over
// time (sampled)").
static void take_sample(const vmalloc_backend_t *be, run_result_t *out) {
  vmalloc_stats_t st;
  be->stats(&st);
  if (st.used_bytes > out->peak_used_bytes) out->peak_used_bytes = st.used_bytes;
  if (st.blocks_used > out->peak_blocks) out->peak_blocks = st.blocks_used;
  if (st.largest_free_block < out->min_largest_free) out->min_largest_free = st.largest_free_block;
  if (st.largest_free_block > out->max_largest_free) out->max_largest_free = st.largest_free_block;
}

// A failing op returns early, and before this it returned WITHOUT sampling:
// with --sample-every 4001 (bench_* in run_all.sh) a failure at op 3052 then
// reported peak_used from op 0 alone (496 B), which read as "failed while
// nearly empty" when it was just an unsampled run. The state at the moment
// of failure is the one number a FAIL row most needs.
static int fail_at(const vmalloc_backend_t *be, run_result_t *out, size_t i) {
  take_sample(be, out);
  if (out->min_largest_free == SIZE_MAX) out->min_largest_free = 0;
  out->ok = 0;
  out->fail_at_op = i;
  return 0;
}

static void result_free(run_result_t *r) {
  free(r->malloc_steps.v); free(r->malloc_ns.v);
  free(r->realloc_steps.v); free(r->realloc_ns.v); free(r->free_ns.v);
  memset(&r->malloc_steps, 0, sizeof r->malloc_steps);
  memset(&r->malloc_ns, 0, sizeof r->malloc_ns);
  memset(&r->realloc_steps, 0, sizeof r->realloc_steps);
  memset(&r->realloc_ns, 0, sizeof r->realloc_ns);
  memset(&r->free_ns, 0, sizeof r->free_ns);
}

static int run_trace(const vmalloc_backend_t *be, void *pool, size_t pool_size,
                      unsigned sample_every, run_result_t *out) {
  memset(out, 0, sizeof *out);
  out->min_largest_free = SIZE_MAX;
  out->check_ok = -1;

  if (be->init(pool, pool_size) != 0) { out->ok = 0; out->fail_at_op = 0; return 0; }

  size_t idcap = g_max_id + 1;
  memset(g_ptr_by_id, 0, idcap * sizeof(void *));
  memset(g_appsize_by_id, 0, idcap * sizeof(size_t));

  for (size_t i = 0; i < g_op_count; i++) {
    op_t *op = &g_ops[i];
    if (op->kind == OP_MALLOC) {
      unsigned steps = 0;
      uint64_t t0 = now_ns();
      void *p = be->do_malloc(op->size, &steps);
      uint64_t dt = now_ns() - t0;
      series_push(&out->malloc_steps, steps);
      series_push(&out->malloc_ns, (unsigned)dt);
      if (!p) return fail_at(be, out, i);
      g_ptr_by_id[op->id1] = p;
      g_appsize_by_id[op->id1] = op->size;
    } else if (op->kind == OP_FREE) {
      void *p = g_ptr_by_id[op->id1];
      unsigned steps = 0;
      uint64_t t0 = now_ns();
      be->do_free(p, &steps);
      uint64_t dt = now_ns() - t0;
      series_push(&out->free_ns, (unsigned)dt);
      g_ptr_by_id[op->id1] = NULL;
    } else { // OP_REALLOC
      void *old = g_ptr_by_id[op->id1];
      size_t old_app = g_appsize_by_id[op->id1];
      unsigned steps = 0;
      uint64_t t0 = now_ns();
      void *n = be->do_realloc(old, op->size, &steps);
      uint64_t dt = now_ns() - t0;
      series_push(&out->realloc_steps, steps);
      series_push(&out->realloc_ns, (unsigned)dt);
      if (!n) return fail_at(be, out, i);
      if (n != old) out->realloc_copy_bytes += (old_app < op->size ? old_app : op->size);
      g_ptr_by_id[op->id1] = NULL;
      g_ptr_by_id[op->id2] = n;
      g_appsize_by_id[op->id2] = op->size;
    }

    if (sample_every && (i % sample_every) == 0) take_sample(be, out);
  }

  // Always take a final sample regardless of sample_every, so peak/min/max
  // are never stale relative to the last op.
  take_sample(be, out);

  out->ok = 1;
  if (be->check) out->check_ok = be->check() ? 1 : 0;
  return 1;
}

static const vmalloc_backend_t *pick_backend(const char *name) {
  if (!strcmp(name, "tlsf")) return vmalloc_tlsf_backend();
  if (!strcmp(name, "estalloc")) return vmalloc_estalloc_backend();
  if (!strcmp(name, "naive")) return vmalloc_naive_backend();
  return NULL;
}

int main(int argc, char **argv) {
  const char *allocator = NULL, *trace_path = NULL;
  size_t pool = 0;
  int do_bisect = 0;
  size_t bisect_max = 4u * 1024 * 1024;
  unsigned sample_every = 1;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--allocator") && i + 1 < argc) allocator = argv[++i];
    else if (!strcmp(argv[i], "--pool") && i + 1 < argc) pool = strtoull(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--bisect")) do_bisect = 1;
    else if (!strcmp(argv[i], "--bisect-max") && i + 1 < argc) bisect_max = strtoull(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--sample-every") && i + 1 < argc) sample_every = (unsigned)strtoul(argv[++i], NULL, 0);
    else trace_path = argv[i];
  }
  if (!allocator || !trace_path || (!pool && !do_bisect)) {
    fprintf(stderr, "usage: vmalloc_replay --allocator tlsf|estalloc|naive (--pool BYTES | --bisect [--bisect-max BYTES]) [--sample-every N] TRACE\n");
    return 1;
  }
  const vmalloc_backend_t *be = pick_backend(allocator);
  if (!be) { fprintf(stderr, "replay: unknown allocator '%s'\n", allocator); return 1; }

  load_trace(trace_path);
  g_ptr_by_id = calloc(g_max_id + 1, sizeof(void *));
  g_appsize_by_id = calloc(g_max_id + 1, sizeof(size_t));

  size_t pool_arena_cap = do_bisect ? bisect_max : pool;
  void *arena = aligned_alloc(16, ((pool_arena_cap + 15) / 16) * 16);
  if (!arena) { fprintf(stderr, "replay: cannot reserve %zu B arena\n", pool_arena_cap); return 1; }

  const char *trace_base = strrchr(trace_path, '/');
  trace_base = trace_base ? trace_base + 1 : trace_path;

  if (do_bisect) {
    // Doubling search for a working upper bound, then binary search down
    // to 64-byte resolution. Every trial re-runs the whole trace from a
    // freshly zeroed id table; sampling is off (sample_every=0) to keep
    // O(log range) full replays cheap.
    // Every trial's series are freed right after it: run_trace() zeroes
    // `out` on entry, so a trial that failed in be->init() leaves NULL
    // pointers and result_free() is a no-op. (These used to be leaked on
    // purpose, on the theory that they were a few hundred bytes; LSan
    // counted ~300 KB per --bisect call and failed the ASan build.)
    size_t lo = 0, hi = 4096;
    run_result_t r;
    for (;;) {
      if (hi > bisect_max) break;
      int ok = run_trace(be, arena, hi, 0, &r);
      result_free(&r);
      if (ok) break;
      hi *= 2;
    }
    if (hi > bisect_max) {
      printf("allocator=%s trace=%s bisect=FAIL_ABOVE_MAX max_tried=%zu\n", allocator, trace_base, bisect_max);
      free(g_ops); free(g_ptr_by_id); free(g_appsize_by_id); free(arena);
      return 0;
    }
    while (hi - lo > 64) {
      size_t mid = lo + (hi - lo) / 2;
      int ok = run_trace(be, arena, mid, 0, &r);
      result_free(&r);
      if (ok) hi = mid; else lo = mid;
    }
    // Final run at hi, with sampling, for the accompanying stats and check.
    run_trace(be, arena, hi, sample_every, &r);
    printf("allocator=%s trace=%s min_pool=%zu peak_used=%zu peak_blocks=%zu check=%d\n",
           allocator, trace_base, hi, r.peak_used_bytes, r.peak_blocks, r.check_ok);
    free(r.malloc_steps.v); free(r.malloc_ns.v);
    free(r.realloc_steps.v); free(r.realloc_ns.v); free(r.free_ns.v);
    free(g_ops); free(g_ptr_by_id); free(g_appsize_by_id); free(arena);
    return 0;
  }

  run_result_t r;
  run_trace(be, arena, pool, sample_every, &r);
  printf("allocator=%s trace=%s pool=%zu result=%s fail_at_op=%zu peak_used=%zu peak_blocks=%zu "
         "min_largest_free=%zu max_largest_free=%zu realloc_copy_bytes=%zu "
         "malloc_steps_max=%u malloc_steps_p50=%u malloc_ns_max=%u malloc_ns_p50=%u "
         "realloc_steps_max=%u realloc_ns_max=%u free_ns_max=%u check=%d\n",
         allocator, trace_base, pool, r.ok ? "OK" : "FAIL", r.fail_at_op, r.peak_used_bytes, r.peak_blocks,
         r.min_largest_free, r.max_largest_free, r.realloc_copy_bytes,
         pctl(&r.malloc_steps, 1.0), pctl(&r.malloc_steps, 0.5),
         pctl(&r.malloc_ns, 1.0), pctl(&r.malloc_ns, 0.5),
         pctl(&r.realloc_steps, 1.0), pctl(&r.realloc_ns, 1.0), pctl(&r.free_ns, 1.0),
         r.check_ok);
  free(r.malloc_steps.v); free(r.malloc_ns.v);
  free(r.realloc_steps.v); free(r.realloc_ns.v); free(r.free_ns.v);
  free(g_ops); free(g_ptr_by_id); free(g_appsize_by_id); free(arena);
  return r.ok ? 0 : 0; // capacity failure is data, not a tool error; see file comment
}
