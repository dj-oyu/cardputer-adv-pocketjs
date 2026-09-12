// replay.c — drives one allocator backend (tools/vmalloc/vmalloc.h) through
// a tools/vmtest trace (format: tools/vmtest/README.md, "トレース形式").
// One process = one allocator = one trace = one pool size, so a backend's
// static state never has to be reset between runs; the shell driver
// (run_all.sh) forks a fresh process per combination instead.
//
// --verify turns the replay into the G6 check of docs/vm-L2-design.md
// sec.1.3: every block handed out is filled with a pattern derived from its
// trace id, checked again right before it is freed or reallocated, and --
// whenever the backend reports a segment event (add / return / cache reuse)
// and at the end -- every live block is swept: pattern intact at the SAME
// address (a moved block fails here), 4-byte aligned, inside a live
// segment of a legal kind, no two live blocks overlapping, every segment
// 16-aligned and disjoint. A realloc must also preserve the old contents
// over min(old,new) bytes whether or not it moved. Backends with no
// segments get the same block checks on a fixed op stride.
//
// Exit code: 0 the trace replayed to completion at the given pool size
// (whether or not every malloc/realloc succeeded — that's `result=FAIL` on
// stdout, not a process failure); 1 a trace/arg problem; 2 the backend's
// own integrity check failed at the end (a real allocator bug, not a
// capacity issue); 3 --verify found a reference broken.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
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
// Op index at which the trace's "# teardown" marker sits (JS_FreeRuntime
// starts there). Everything after it is the runtime freeing every object in
// whatever order the GC lists hold them, which returns segments in address-
// random order and pushes the pool-level "external fragmentation" sample to
// a value that says nothing about running an app. Peaks are therefore kept
// twice: over the whole trace and over the ops before this marker.
static size_t g_teardown_op = SIZE_MAX;

static void load_trace(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); exit(1); }
  char line[512];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#') { if (!strncmp(line, "# teardown", 10)) g_teardown_op = g_op_count; continue; }
    if (line[0] == '\n') continue;
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

// Dense set of live ids for --verify sweeps (a sweep must not walk every id
// ever issued: bench_promise issues 5.8 million).
static uint64_t *g_live;
static size_t g_live_n;
static size_t *g_live_slot; // id -> index in g_live, valid while live

static void live_add(uint64_t id) { g_live_slot[id] = g_live_n; g_live[g_live_n++] = id; }
static void live_remove(uint64_t id) {
  size_t i = g_live_slot[id];
  uint64_t last = g_live[--g_live_n];
  g_live[i] = last;
  g_live_slot[last] = i;
}

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
  int ok;               // 1 = every op succeeded; 0 = a malloc/realloc returned NULL; 2 = --verify stopped it
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
  // Segment-style breakdown (all 0 for backends without segments). The
  // peaks are taken independently, each at its own worst sample; they do
  // not describe one moment and must not be summed.
  size_t peak_reserved;        // pool bytes held by segments at the worst sample
  size_t peak_seg_free_inside; // internal slack (free bytes inside live segments), worst sample
  size_t peak_seg_cached;      // internal slack of the second kind (retained empty segments), worst sample
  size_t peak_rounding_waste;  // used payload - requested bytes, worst sample
  size_t peak_external_frag;   // pool_free - pool_largest_free, worst sample
  size_t min_pool_largest;     // smallest largest-free-extent seen: can a standard segment still be added?
  size_t peak_segments_live, peak_segments_dedicated;
  size_t app_ext_frag, app_slack_inside, app_min_pool_largest; // the same three, sampled before "# teardown" only
  vmalloc_stats_t last;        // final stats() for the event counters
  // --verify accounting.
  unsigned long verify_sweeps, verify_errors;
  size_t first_error_op;
  char first_error[160];
} run_result_t;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static size_t g_live_requested; // sum of requested sizes of live blocks (rounding-waste baseline)

// sample_every: how often (in ops) to poll stats() for the fragmentation
// series. 0 disables sampling (bisection passes do, to stay fast); a real
// reporting run samples every op for small traces or every few ops for big
// ones (README.md: "largest free block and external fragmentation over
// time (sampled)").
static size_t g_cur_op; // for take_sample to tell pre- from post-teardown

static void take_sample(const vmalloc_backend_t *be, run_result_t *out) {
  vmalloc_stats_t st;
  memset(&st, 0, sizeof st); // backends without segments leave the segment fields 0
  be->stats(&st);
  if (st.used_bytes > out->peak_used_bytes) out->peak_used_bytes = st.used_bytes;
  if (st.blocks_used > out->peak_blocks) out->peak_blocks = st.blocks_used;
  if (st.largest_free_block < out->min_largest_free) out->min_largest_free = st.largest_free_block;
  if (st.largest_free_block > out->max_largest_free) out->max_largest_free = st.largest_free_block;
  if (st.reserved_bytes > out->peak_reserved) out->peak_reserved = st.reserved_bytes;
  if (st.seg_free_inside > out->peak_seg_free_inside) out->peak_seg_free_inside = st.seg_free_inside;
  if (st.seg_cached_bytes > out->peak_seg_cached) out->peak_seg_cached = st.seg_cached_bytes;
  if (st.used_payload_bytes > g_live_requested &&
      st.used_payload_bytes - g_live_requested > out->peak_rounding_waste)
    out->peak_rounding_waste = st.used_payload_bytes - g_live_requested;
  if (st.pool_free_bytes > st.pool_largest_free &&
      st.pool_free_bytes - st.pool_largest_free > out->peak_external_frag)
    out->peak_external_frag = st.pool_free_bytes - st.pool_largest_free;
  if (st.reserved_bytes && st.pool_largest_free < out->min_pool_largest) out->min_pool_largest = st.pool_largest_free;
  if (g_cur_op < g_teardown_op) {
    if (st.pool_free_bytes > st.pool_largest_free &&
        st.pool_free_bytes - st.pool_largest_free > out->app_ext_frag)
      out->app_ext_frag = st.pool_free_bytes - st.pool_largest_free;
    if (st.seg_free_inside > out->app_slack_inside) out->app_slack_inside = st.seg_free_inside;
    if (st.reserved_bytes && st.pool_largest_free < out->app_min_pool_largest) out->app_min_pool_largest = st.pool_largest_free;
  }
  if (st.segments_live > out->peak_segments_live) out->peak_segments_live = st.segments_live;
  if (st.segments_dedicated > out->peak_segments_dedicated) out->peak_segments_dedicated = st.segments_dedicated;
  out->last = st;
}

// A failing op returns early, and before this it returned WITHOUT sampling:
// with --sample-every 4001 (bench_* in run_all.sh) a failure at op 3052 then
// reported peak_used from op 0 alone (496 B), which read as "failed while
// nearly empty" when it was just an unsampled run. The state at the moment
// of failure is the one number a FAIL row most needs.
static int fail_at(const vmalloc_backend_t *be, run_result_t *out, size_t i) {
  take_sample(be, out);
  if (out->min_largest_free == SIZE_MAX) out->min_largest_free = 0;
  if (out->min_pool_largest == SIZE_MAX) out->min_pool_largest = 0;
  if (out->app_min_pool_largest == SIZE_MAX) out->app_min_pool_largest = 0;
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

// ------------------------------------------------------------------ verify

static int g_verify;
static unsigned g_verify_every;   // sweep stride in ops (0: only on backend events + end)
static unsigned g_verify_gap;     // minimum ops between event-driven sweeps (throttle for huge traces)

// Byte j of block `id`. Different ids differ in every byte position, and
// neighbouring bytes differ, so a block written over by a neighbour's
// pattern, by allocator metadata (a 4-byte header or a 16-byte link pair)
// or by the 0xDD free scribble fails the compare at the first touched byte.
static inline uint8_t pat(uint64_t id, size_t j) {
  uint32_t h = (uint32_t)id * 2654435761u + 0x9E3779B9u;
  return (uint8_t)((h >> ((j & 3) * 8)) ^ (uint8_t)(j * 31 + 7));
}

static void fill(void *p, uint64_t id, size_t n) {
  uint8_t *b = p;
  for (size_t j = 0; j < n; j++) b[j] = pat(id, j);
}

static void verify_error(run_result_t *r, size_t op, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void verify_error(run_result_t *r, size_t op, const char *fmt, ...) {
  va_list ap;
  char msg[160];
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  if (r->verify_errors == 0) {
    r->first_error_op = op;
    snprintf(r->first_error, sizeof r->first_error, "%s", msg);
  }
  if (r->verify_errors < 8) fprintf(stderr, "verify: op %zu: %s\n", op, msg);
  r->verify_errors++;
}

// Returns the index of the first mismatching byte, or n if intact.
static size_t check_pattern(const void *p, uint64_t id, size_t n) {
  const uint8_t *b = p;
  for (size_t j = 0; j < n; j++) if (b[j] != pat(id, j)) return j;
  return n;
}

static int cmp_live_ptr(const void *a, const void *b) {
  const uint8_t *pa = g_ptr_by_id[*(const uint64_t *)a], *pb = g_ptr_by_id[*(const uint64_t *)b];
  return pa < pb ? -1 : pa > pb;
}

static uint64_t *g_sorted; // scratch for the overlap check

static void sweep(const vmalloc_backend_t *be, run_result_t *r, size_t op, const uint8_t *pool, size_t pool_size) {
  r->verify_sweeps++;
  // The backend's own structural walk first: a block that overlaps
  // allocator METADATA (not another live block) is invisible to the pattern
  // checks below, because the pattern is written after the allocator
  // returns and simply overwrites the header it collided with.
  if (be->check && !be->check()) { verify_error(r, op, "backend check() failed"); return; } // do not walk a corrupt heap
  // Blocks: pattern at the recorded address, alignment, ownership.
  for (size_t i = 0; i < g_live_n; i++) {
    uint64_t id = g_live[i];
    const uint8_t *p = g_ptr_by_id[id];
    size_t n = g_appsize_by_id[id];
    if (((uintptr_t)p & 3) != 0) verify_error(r, op, "id %llu at %p not 4-aligned", (unsigned long long)id, (const void *)p);
    if (p < pool || p + n > pool + pool_size) verify_error(r, op, "id %llu outside the pool", (unsigned long long)id);
    size_t bad = check_pattern(p, id, n);
    if (bad != n) verify_error(r, op, "id %llu (%zu B) pattern broken at byte %zu", (unsigned long long)id, n, bad);
    if (be->owner) {
      vmalloc_seg_t sg;
      if (!be->owner(p, &sg)) verify_error(r, op, "id %llu lies in no segment", (unsigned long long)id);
      else {
        if (sg.kind == 2) verify_error(r, op, "id %llu lies in a CACHED (empty) segment", (unsigned long long)id);
        if (p + n > (const uint8_t *)sg.base + sg.size) verify_error(r, op, "id %llu crosses its segment's end", (unsigned long long)id);
        if (sg.live_blocks == 0) verify_error(r, op, "id %llu lies in a segment that counts 0 live blocks", (unsigned long long)id);
      }
    }
  }
  // Overlap: sort live blocks by address, adjacent pairs must not intersect.
  if (g_live_n > 1) {
    memcpy(g_sorted, g_live, g_live_n * sizeof *g_sorted);
    qsort(g_sorted, g_live_n, sizeof *g_sorted, cmp_live_ptr);
    for (size_t i = 1; i < g_live_n; i++) {
      uint64_t a = g_sorted[i - 1], b = g_sorted[i];
      const uint8_t *pa = g_ptr_by_id[a], *pb = g_ptr_by_id[b];
      if (pa + g_appsize_by_id[a] > pb)
        verify_error(r, op, "ids %llu and %llu overlap", (unsigned long long)a, (unsigned long long)b);
    }
  }
  // Segments: 16-aligned, inside the pool, address-ordered and disjoint.
  if (be->segment_at) {
    vmalloc_seg_t sg, prev = {0};
    for (size_t i = 0; be->segment_at(i, &sg); i++) {
      const uint8_t *base = sg.base;
      if (((uintptr_t)base & 15) != 0) verify_error(r, op, "segment %zu base %p not 16-aligned", i, (const void *)base);
      if (base < pool || base + sg.size > pool + pool_size) verify_error(r, op, "segment %zu outside the pool", i);
      if (i && (const uint8_t *)prev.base + prev.size > base) verify_error(r, op, "segments %zu and %zu overlap", i - 1, i);
      prev = sg;
    }
  }
}

// ---------------------------------------------------------------- replay

static int run_trace(const vmalloc_backend_t *be, void *pool, size_t pool_size,
                      unsigned sample_every, run_result_t *out) {
  memset(out, 0, sizeof *out);
  out->min_largest_free = SIZE_MAX;
  out->min_pool_largest = SIZE_MAX;
  out->app_min_pool_largest = SIZE_MAX;
  out->check_ok = -1;
  g_cur_op = 0;

  if (be->init(pool, pool_size) != 0) { out->ok = 0; out->fail_at_op = 0; return 0; }

  size_t idcap = g_max_id + 1;
  memset(g_ptr_by_id, 0, idcap * sizeof(void *));
  memset(g_appsize_by_id, 0, idcap * sizeof(size_t));
  g_live_n = 0;
  g_live_requested = 0;
  unsigned long last_events = 0;
  size_t last_sweep_op = 0;

  for (size_t i = 0; i < g_op_count; i++) {
    op_t *op = &g_ops[i];
    g_cur_op = i;
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
      g_live_requested += op->size;
      if (g_verify) { fill(p, op->id1, op->size); live_add(op->id1); }
    } else if (op->kind == OP_FREE) {
      void *p = g_ptr_by_id[op->id1];
      if (g_verify) {
        size_t n = g_appsize_by_id[op->id1], bad = check_pattern(p, op->id1, n);
        if (bad != n) verify_error(out, i, "id %llu (%zu B) pattern broken at byte %zu, found at free", (unsigned long long)op->id1, n, bad);
        live_remove(op->id1);
      }
      unsigned steps = 0;
      uint64_t t0 = now_ns();
      be->do_free(p, &steps);
      uint64_t dt = now_ns() - t0;
      series_push(&out->free_ns, (unsigned)dt);
      g_ptr_by_id[op->id1] = NULL;
      g_live_requested -= g_appsize_by_id[op->id1];
    } else { // OP_REALLOC
      void *old = g_ptr_by_id[op->id1];
      size_t old_app = g_appsize_by_id[op->id1];
      if (g_verify) {
        size_t bad = check_pattern(old, op->id1, old_app);
        if (bad != old_app) verify_error(out, i, "id %llu (%zu B) pattern broken at byte %zu, found at realloc", (unsigned long long)op->id1, old_app, bad);
        live_remove(op->id1);
      }
      unsigned steps = 0;
      uint64_t t0 = now_ns();
      void *n = be->do_realloc(old, op->size, &steps);
      uint64_t dt = now_ns() - t0;
      series_push(&out->realloc_steps, steps);
      series_push(&out->realloc_ns, (unsigned)dt);
      if (!n) {
        if (g_verify) live_add(op->id1); // the old block is still live after a failed realloc
        return fail_at(be, out, i);
      }
      if (n != old) out->realloc_copy_bytes += (old_app < op->size ? old_app : op->size);
      g_ptr_by_id[op->id1] = NULL;
      g_ptr_by_id[op->id2] = n;
      g_appsize_by_id[op->id2] = op->size;
      g_live_requested += op->size - old_app;
      if (g_verify) {
        // Contents must survive the realloc over the common prefix, moved or not.
        size_t keep = old_app < op->size ? old_app : op->size;
        size_t bad = check_pattern(n, op->id1, keep);
        if (bad != keep) verify_error(out, i, "realloc %llu->%llu lost contents at byte %zu (%s)", (unsigned long long)op->id1, (unsigned long long)op->id2, bad, n == old ? "in place" : "moved");
        fill(n, op->id2, op->size);
        live_add(op->id2);
      }
    }

    if (sample_every && (i % sample_every) == 0) take_sample(be, out);

    if (g_verify) {
      int due = 0;
      if (g_verify_every && (i % g_verify_every) == 0) due = 1;
      if (be->events) {
        unsigned long ev = be->events();
        if (ev != last_events && i - last_sweep_op >= g_verify_gap) { due = 1; last_events = ev; }
      }
      if (due) { sweep(be, out, i, pool, pool_size); last_sweep_op = i; }
      // Stop at the first broken reference: everything after it is either
      // a consequence of the same fault or a crash inside the allocator,
      // and neither says anything the first report did not.
      if (out->verify_errors) { out->ok = 2; out->fail_at_op = i; return 0; } // no stats() on a heap that failed check()
    }
  }

  // Always take a final sample regardless of sample_every, so peak/min/max
  // are never stale relative to the last op.
  g_cur_op = g_op_count;
  take_sample(be, out);
  if (g_verify) sweep(be, out, g_op_count, pool, pool_size);
  if (out->min_pool_largest == SIZE_MAX) out->min_pool_largest = 0;
  if (out->app_min_pool_largest == SIZE_MAX) out->app_min_pool_largest = 0;

  out->ok = out->verify_errors ? 2 : 1;
  if (be->check) out->check_ok = be->check() ? 1 : 0;
  return 1;
}

static const vmalloc_backend_t *pick_backend(const char *name) {
  if (!strcmp(name, "tlsf")) return vmalloc_tlsf_backend();
  if (!strcmp(name, "estalloc")) return vmalloc_estalloc_backend();
  if (!strcmp(name, "naive")) return vmalloc_naive_backend();
  if (!strcmp(name, "segment")) return vmalloc_segment_backend();
  return NULL;
}

static void print_segment_tail(const run_result_t *r) {
  printf(" peak_reserved=%zu peak_seg_free_inside=%zu peak_seg_cached=%zu peak_rounding_waste=%zu "
         "peak_external_frag=%zu min_pool_largest=%zu peak_segments_live=%zu peak_segments_dedicated=%zu "
         "app_ext_frag=%zu app_slack_inside=%zu app_min_pool_largest=%zu "
         "seg_added=%lu seg_returned=%lu seg_reused=%lu seg_dedicated_added=%lu",
         r->peak_reserved, r->peak_seg_free_inside, r->peak_seg_cached, r->peak_rounding_waste,
         r->peak_external_frag, r->min_pool_largest, r->peak_segments_live, r->peak_segments_dedicated,
         r->app_ext_frag, r->app_slack_inside, r->app_min_pool_largest,
         r->last.seg_added, r->last.seg_returned, r->last.seg_reused, r->last.seg_dedicated_added);
}

static void print_verify_tail(const run_result_t *r) {
  if (!g_verify) return;
  printf(" verify=%s verify_sweeps=%lu verify_errors=%lu", r->verify_errors ? "BROKEN" : "OK",
         r->verify_sweeps, r->verify_errors);
  if (r->verify_errors) printf(" first_error_op=%zu first_error=\"%s\"", r->first_error_op, r->first_error);
}

int main(int argc, char **argv) {
  const char *allocator = NULL, *trace_path = NULL;
  size_t pool = 0;
  int do_bisect = 0;
  size_t bisect_max = 4u * 1024 * 1024;
  unsigned sample_every = 1;
  const char *cfg_keys[16], *cfg_vals[16];
  int cfg_n = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--allocator") && i + 1 < argc) allocator = argv[++i];
    else if (!strcmp(argv[i], "--pool") && i + 1 < argc) pool = strtoull(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--bisect")) do_bisect = 1;
    else if (!strcmp(argv[i], "--bisect-max") && i + 1 < argc) bisect_max = strtoull(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--sample-every") && i + 1 < argc) sample_every = (unsigned)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--verify")) g_verify = 1;
    else if (!strcmp(argv[i], "--verify-every") && i + 1 < argc) { g_verify = 1; g_verify_every = (unsigned)strtoul(argv[++i], NULL, 0); }
    else if (!strcmp(argv[i], "--verify-gap") && i + 1 < argc) { g_verify = 1; g_verify_gap = (unsigned)strtoul(argv[++i], NULL, 0); }
    else if (!strcmp(argv[i], "--seg-size") && i + 1 < argc && cfg_n < 16) { cfg_keys[cfg_n] = "seg_size"; cfg_vals[cfg_n++] = argv[++i]; }
    else if (!strcmp(argv[i], "--seg-cache") && i + 1 < argc && cfg_n < 16) { cfg_keys[cfg_n] = "cache"; cfg_vals[cfg_n++] = argv[++i]; }
    else if (!strcmp(argv[i], "--fault") && i + 1 < argc && cfg_n < 16) { cfg_keys[cfg_n] = "fault"; cfg_vals[cfg_n++] = argv[++i]; }
    else trace_path = argv[i];
  }
  if (!allocator || !trace_path || (!pool && !do_bisect)) {
    fprintf(stderr, "usage: vmalloc_replay --allocator tlsf|estalloc|naive|segment (--pool BYTES | --bisect [--bisect-max BYTES])\n"
                    "         [--sample-every N] [--verify] [--verify-every N] [--verify-gap N]\n"
                    "         [--seg-size BYTES] [--seg-cache N] [--fault none|early-return|overlap|misalign|pool-overlap|compact] TRACE\n");
    return 1;
  }
  const vmalloc_backend_t *be = pick_backend(allocator);
  if (!be) { fprintf(stderr, "replay: unknown allocator '%s'\n", allocator); return 1; }
  for (int i = 0; i < cfg_n; i++) {
    if (!be->configure) { fprintf(stderr, "replay: %s takes no --%s\n", allocator, cfg_keys[i]); return 1; }
    if (be->configure(cfg_keys[i], cfg_vals[i]) != 0) { fprintf(stderr, "replay: bad %s=%s\n", cfg_keys[i], cfg_vals[i]); return 1; }
  }
  // A backend with no segment events gets a fixed stride so --verify still
  // sweeps somewhere other than the end.
  if (g_verify && !be->owner && !g_verify_every) g_verify_every = 4096;

  load_trace(trace_path);
  g_ptr_by_id = calloc(g_max_id + 1, sizeof(void *));
  g_appsize_by_id = calloc(g_max_id + 1, sizeof(size_t));
  if (g_verify) {
    g_live = calloc(g_max_id + 1, sizeof *g_live);
    g_sorted = calloc(g_max_id + 1, sizeof *g_sorted);
    g_live_slot = calloc(g_max_id + 1, sizeof *g_live_slot);
  }

  size_t pool_arena_cap = do_bisect ? bisect_max : pool;
  void *arena = aligned_alloc(16, ((pool_arena_cap + 15) / 16) * 16);
  if (!arena) { fprintf(stderr, "replay: cannot reserve %zu B arena\n", pool_arena_cap); return 1; }

  const char *trace_base = strrchr(trace_path, '/');
  trace_base = trace_base ? trace_base + 1 : trace_path;

  int rc = 0;
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
      goto done;
    }
    while (hi - lo > 64) {
      size_t mid = lo + (hi - lo) / 2;
      int ok = run_trace(be, arena, mid, 0, &r);
      result_free(&r);
      if (ok) hi = mid; else lo = mid;
    }
    // Final run at hi, with sampling, for the accompanying stats and check.
    run_trace(be, arena, hi, sample_every, &r);
    printf("allocator=%s trace=%s min_pool=%zu peak_used=%zu peak_blocks=%zu check=%d",
           allocator, trace_base, hi, r.peak_used_bytes, r.peak_blocks, r.check_ok);
    if (be->owner) print_segment_tail(&r);
    print_verify_tail(&r);
    printf("\n");
    if (r.check_ok == 0) rc = 2;
    if (r.verify_errors) rc = 3;
    result_free(&r);
    goto done;
  }

  {
    run_result_t r;
    run_trace(be, arena, pool, sample_every, &r);
    printf("allocator=%s trace=%s pool=%zu result=%s fail_at_op=%zu peak_used=%zu peak_blocks=%zu "
           "min_largest_free=%zu max_largest_free=%zu realloc_copy_bytes=%zu "
           "malloc_steps_max=%u malloc_steps_p50=%u malloc_ns_max=%u malloc_ns_p50=%u "
           "realloc_steps_max=%u realloc_ns_max=%u free_ns_max=%u check=%d",
           allocator, trace_base, pool, r.ok == 1 ? "OK" : r.ok == 2 ? "BROKEN" : "FAIL", r.fail_at_op, r.peak_used_bytes, r.peak_blocks,
           r.min_largest_free, r.max_largest_free, r.realloc_copy_bytes,
           pctl(&r.malloc_steps, 1.0), pctl(&r.malloc_steps, 0.5),
           pctl(&r.malloc_ns, 1.0), pctl(&r.malloc_ns, 0.5),
           pctl(&r.realloc_steps, 1.0), pctl(&r.realloc_ns, 1.0), pctl(&r.free_ns, 1.0),
           r.check_ok);
    if (be->owner) print_segment_tail(&r);
    print_verify_tail(&r);
    printf("\n");
    if (r.check_ok == 0) rc = 2;
    if (r.verify_errors) rc = 3;
    result_free(&r);
  }
done:
  free(g_ops); free(g_ptr_by_id); free(g_appsize_by_id); free(arena);
  free(g_live); free(g_sorted); free(g_live_slot);
  return rc; // capacity failure is data, not a tool error; see file comment
}
