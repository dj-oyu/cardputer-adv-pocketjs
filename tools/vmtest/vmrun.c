// vmrun: host runner for the vendored quickjs-ng, shaped like the device guest.
//
// Every later VM level (docs/quickjs-freertos-vm-spec.md L1..L5) is judged by
// running the same corpus and Test262 subset through this driver and comparing
// against the L0 baseline. So the parts that decide observable behaviour are
// copied from components/pocketjs_guest/src/guest.c rather than re-imagined:
//
//   - the allocator: size header in front of the block, usable_size returning
//     the REQUESTED size (not the malloc bucket), realloc as malloc+copy+free.
//     QuickJS accounts malloc_size from usable_size, so this is what makes
//     JS_SetMemoryLimit and the GC threshold trip at the same logical sizes as
//     on the device (modulo the 64-bit host: JSValue is 16 B here, 8 B there).
//   - the rejection tracker and drain_jobs(): same list discipline, same
//     "report only after the queue is empty" rule, same early return without
//     reporting when a job itself throws.
//   - from L1 on, the drain loop itself is not copied but LINKED:
//     components/pocketjs_guest/src/vm_sched.c is the same object file the
//     firmware builds. The harness therefore tests the scheduler that ships.
//   - JS_SetMemoryLimit / JS_SetMaxStackSize with the device's values
//     (main/app_session.c: 160 KiB heap, 20 KiB stack) unless overridden.
//
// Deliberate differences from the guest, all opt-in or harmless:
//   - after eval the queue is drained even when no frame() exists (the guest
//     returns ESP_ERR_NOT_FOUND first); --require-frame restores the guest's
//     early return.
//   - the allocator header also carries a sequence id for --trace; it still
//     fits inside max_align_t, so the header size is unchanged.
//   - $262 is installed only with --test262.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pocketjs/vm_clock.h"
#include "pocketjs/vm_sched.h"
#include "quickjs-libc.h"
#include "quickjs.h"

// ---------------------------------------------------------------- allocator

// Same union as guest.c plus an id. {size_t,uint64_t} is 16 B, max_align_t is
// 32 B on x86-64 glibc, so the id rides in padding the guest already pays for.
typedef union {
  struct {
    size_t size;
    uint64_t id;
  } h;
  max_align_t alignment;
} allocation_header_t;

typedef struct {
  FILE *trace;          // NULL unless --trace
  uint64_t next_id;     // sequence number of the next successful allocation
  uint64_t fail_at;     // --fail-alloc: this attempt number returns NULL
  uint64_t attempts;    // every malloc/calloc/realloc-grow attempt, 1-based
  size_t live_bytes;    // sum of requested sizes of live blocks
  size_t live_blocks;
  size_t peak_bytes;
  size_t peak_blocks;
  size_t max_block;
  uint64_t n_malloc, n_free, n_realloc, n_fail;
} alloc_state_t;

static alloc_state_t A;

static void account_add(size_t size) {
  A.live_bytes += size;
  A.live_blocks++;
  if (A.live_bytes > A.peak_bytes) A.peak_bytes = A.live_bytes;
  if (A.live_blocks > A.peak_blocks) A.peak_blocks = A.live_blocks;
  if (size > A.max_block) A.max_block = size;
}

static void account_sub(size_t size) {
  A.live_bytes -= size;
  A.live_blocks--;
}

// The raw allocation, shared by malloc and the malloc half of realloc so a
// realloc emits exactly one "~" line instead of a "+" and a "-".
static allocation_header_t *raw_alloc(size_t size) {
  if (size == 0U || size > SIZE_MAX - sizeof(allocation_header_t)) return NULL;
  A.attempts++;
  if (A.fail_at != 0 && A.attempts == A.fail_at) return NULL;
  allocation_header_t *header = malloc(sizeof(allocation_header_t) + size);
  if (header == NULL) return NULL;
  header->h.size = size;
  header->h.id = ++A.next_id;
  return header;
}

static void *vm_malloc(void *opaque, size_t size) {
  (void)opaque;
  allocation_header_t *header = raw_alloc(size);
  if (header == NULL) {
    A.n_fail++;
    if (A.trace) fprintf(A.trace, "! %zu\n", size);
    return NULL;
  }
  A.n_malloc++;
  account_add(size);
  if (A.trace) fprintf(A.trace, "+ %llu %zu\n", (unsigned long long)header->h.id, size);
  return header + 1;
}

static void *vm_calloc(void *opaque, size_t count, size_t size) {
  if (count != 0U && size > SIZE_MAX / count) return NULL;
  const size_t total = count * size;
  void *memory = vm_malloc(opaque, total);
  if (memory != NULL) memset(memory, 0, total);
  return memory;
}

static void vm_free(void *opaque, void *pointer) {
  (void)opaque;
  if (pointer == NULL) return;
  allocation_header_t *header = ((allocation_header_t *)pointer) - 1;
  A.n_free++;
  account_sub(header->h.size);
  if (A.trace) fprintf(A.trace, "- %llu\n", (unsigned long long)header->h.id);
  free(header);
}

static size_t vm_usable_size(const void *pointer) {
  return pointer == NULL ? 0U : (((const allocation_header_t *)pointer) - 1)->h.size;
}

// Mirrors guest_realloc: always a fresh block, never in-place. A VM level that
// assumes realloc keeps the address (L2/L3 segment growth) fails here as it
// would on the device.
static void *vm_realloc(void *opaque, void *pointer, size_t size) {
  if (pointer == NULL) return vm_malloc(opaque, size);
  if (size == 0U) {
    vm_free(opaque, pointer);
    return NULL;
  }
  allocation_header_t *old = ((allocation_header_t *)pointer) - 1;
  const size_t previous_size = old->h.size;
  allocation_header_t *next = raw_alloc(size);
  if (next == NULL) {
    A.n_fail++;
    if (A.trace) fprintf(A.trace, "!~ %llu %zu\n", (unsigned long long)old->h.id, size);
    return NULL;
  }
  memcpy(next + 1, pointer, previous_size < size ? previous_size : size);
  A.n_realloc++;
  account_sub(previous_size);
  account_add(size);
  if (A.trace)
    fprintf(A.trace, "~ %llu %llu %zu\n", (unsigned long long)old->h.id,
            (unsigned long long)next->h.id, size);
  free(old);
  return next + 1;
}

static const JSMallocFunctions VM_ALLOCATOR = {
    .js_calloc = vm_calloc,
    .js_malloc = vm_malloc,
    .js_free = vm_free,
    .js_realloc = vm_realloc,
    .js_malloc_usable_size = vm_usable_size,
};

// ---------------------------------------------------------------- GC probe

// GC markers without touching the VM: JS_RunGC's first pass (gc_decref) calls
// every live object's class gc_mark, and gc_scan calls it again with a
// different mark function. Latching the first mark function seen gives one
// callback per collection. The probe is one extra JSObject, created only when
// tracing, so untraced runs allocate exactly what the program does.
static JSClassID probe_class_id;
static JS_MarkFunc *probe_first_mark;
static uint64_t gc_count;

static void probe_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
  (void)rt;
  (void)val;
  if (probe_first_mark == NULL) probe_first_mark = mark_func;
  if (mark_func != probe_first_mark) return;
  gc_count++;
  if (A.trace) fprintf(A.trace, "# gc %zu %zu\n", A.live_bytes, A.live_blocks);
}

static const JSClassDef PROBE_CLASS = {.class_name = "VmtestGcProbe", .gc_mark = probe_mark};

// ---------------------------------------------------------------- guest state

typedef struct rejection {
  JSValue promise;
  JSValue reason;
  struct rejection *next;
} rejection_t;

typedef struct {
  JSRuntime *runtime;
  JSContext *context;
  rejection_t *rejections;
  bool rejection_tracking_failed;
  uint64_t jobs;
  size_t max_queue_run;  // longest single drain, in jobs
  JSContext *realms[16]; // $262.createRealm contexts, freed before the main one
  int n_realms;
  // L1: the budget armed once per turn, exactly as app_tick() arms it.
  vm_budget_t budget;
  uint64_t turns;          // continuation turns taken, over the whole run
  unsigned max_run_turns;  // most continuation turns spent on one logical drain
  bool jobs_dropped;       // the run ended with jobs still queued
  uint64_t runaway_jobs;   // jobs the logical drain had run when the guard fired
} guest_t;

static guest_t G;

// app_session.c's interrupt(): once the host has asked the session to stop,
// every subsequent call into JavaScript is killed with an uncatchable
// InternalError. Modelled here because that is exactly what makes WHERE the
// exit() is honoured observable -- see host_exit() below.
static bool host_stopping;

static int vm_interrupt(JSRuntime *rt, void *opaque) {
  (void)rt;
  (void)opaque;
  return host_stopping ? 1 : 0;
}

// Copied from guest.c promise_rejection(): one entry per promise, appended at
// the tail, dropped on a later "handled" event.
static void promise_rejection(JSContext *context, JSValueConst promise, JSValueConst reason,
                              bool handled, void *opaque) {
  guest_t *guest = opaque;
  rejection_t **slot = &guest->rejections;
  while (*slot && JS_VALUE_GET_PTR((*slot)->promise) != JS_VALUE_GET_PTR(promise))
    slot = &(*slot)->next;
  if (handled) {
    if (*slot) {
      rejection_t *entry = *slot;
      *slot = entry->next;
      JS_FreeValue(context, entry->promise);
      JS_FreeValue(context, entry->reason);
      free(entry);
    }
    return;
  }
  if (*slot) return;
  rejection_t *entry = calloc(1, sizeof(*entry));
  if (!entry) {
    guest->rejection_tracking_failed = true;
    return;
  }
  entry->promise = JS_DupValue(context, promise);
  entry->reason = JS_DupValue(context, reason);
  *slot = entry;
}

// Copied from guest.c report_rejections(). Returns 0 on ESP_OK, -1 on
// ESP_FAIL. The report line keeps the guest's wording; the "E pocketjs_guest:"
// prefix stands in for ESP_LOGE's "E (ticks) pocketjs_guest:" without the
// timestamp.
static int report_rejections(guest_t *guest) {
  bool failed = guest->rejection_tracking_failed;
  guest->rejection_tracking_failed = false;
  size_t pending = 0;
  for (rejection_t *entry = guest->rejections; entry; entry = entry->next) ++pending;
  while (pending--) {
    rejection_t *entry = guest->rejections;
    if (!entry) break;
    guest->rejections = entry->next;
    failed = true;
    const char *text = JS_ToCString(guest->context, entry->reason);
    fflush(stdout);
    fprintf(stderr, "E pocketjs_guest: Unhandled Promise rejection: %s\n", text ? text : "<value>");
    if (text)
      JS_FreeCString(guest->context, text);
    else
      JS_FreeValue(guest->context, JS_GetException(guest->context));
    JS_FreeValue(guest->context, entry->reason);
    JS_FreeValue(guest->context, entry->promise);
    free(entry);
  }
  return failed ? -1 : 0;
}

// L1 knobs. Defaults reproduce the pre-L1 drain exactly: no limit, no ceiling,
// so vm_sched_drain() can only return EMPTY or THREW.
static unsigned budget_jobs;                    // --budget-jobs / --force-yield
// The firmware guard (vm_sched.h VM_RUNAWAY_US / VM_RUNAWAY_JOBS) is a
// predicate on what ONE LOGICAL DRAIN has spent: microseconds first, jobs as
// the dead-clock fallback. The harness runs in count mode, where the clock is
// never read, so what it can model is the job total -- and that is the useful
// half here anyway, because it is the half that has to tell an honest long
// chain from a job that requeues itself forever. OFF by default: a corpus file
// that means to provoke the guard asks for it.
//
// It is NOT a turn count, and the difference is the defect this replaced:
// --budget-jobs 1 turns an honest 200-job drain into 200 continuation turns,
// and the device's 64-job backstop does the same thing to any drain of cheap
// jobs, so a turn count condemns programs for being long rather than for
// failing to finish.
static uint64_t runaway_jobs;                   // --runaway-jobs
static unsigned stop_turns;                     // --stop-turns (0 = never)
static bool host_events;                        // --host-events
// --fair: CONFIG_POCKET_VM_FAIR in miniature (main/app_session.c app_tick()).
// Off is compat ordering, the shipping default: no host call reaches JS until
// the queue is empty. On, a continuation turn whose drain yielded with work
// still queued runs the pump AFTER that drain, so a completion recorded while
// the drain was running is settled at a job boundary in the middle of one
// logical drain -- and its reaction is APPENDED, landing behind every job
// already queued, which is why FIFO inside the queue is unaffected. The exit
// check is deliberately NOT made fair (it arrives as an interrupt and would
// cut the drain); neither is frame().
static bool fair_mode;

// G1 (docs/vm-L2-design.md sec.1.3): does C stack use per JS call depend on
// depth? deep_recursion.js's max_depth answers "how many levels until
// overflow", which is silent on per-level cost -- a 200-byte frame and a
// 2000-byte frame both eventually overflow, they just do it at different
// depths. What actually distinguishes "proportional to depth" (today, since
// JS_CallInternal recurses in C for every JS call) from "flattened" (the L2
// goal, where a deep JS call chain uses O(1) C stack) is bytes consumed PER
// LEVEL, and whether that per-level figure holds steady as depth doubles.
//
// __vmtest_stack_probe(), called once per recursion level from JS, records
// the address of a local in ITS OWN C frame. That frame is nested inside
// every JS_CallInternal invocation on the current call chain, so its address
// falls (stacks grow down on every host this runs on: x86-64, arm64) by
// however many bytes each recursion level actually adds to the C stack --
// today, one JS_CallInternal frame's worth. Comparing the first and last
// probe addresses over N calls gives total C-stack growth for that run;
// dividing by N gives the per-level figure that answers the completion
// condition. Only first/last are kept (not a full series) because the
// question is "does the average change between N and 2N", not the shape in
// between -- G1's own analysis script (stack_probe.sh) is what runs this
// twice and compares.
static bool stack_probe_enabled;                // --stack-probe
static uintptr_t stack_probe_first, stack_probe_last;
static uint64_t stack_probe_calls;

static JSValue js_stack_probe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)ctx; (void)this_val; (void)argc; (void)argv;
  volatile char marker;
  uintptr_t here = (uintptr_t)&marker;
  if (stack_probe_calls == 0) stack_probe_first = here;
  stack_probe_last = here;
  stack_probe_calls++;
  return JS_UNDEFINED;
}

static void arm_budget(guest_t *guest) {
  // Count mode, the only deterministic one on a host: the clock is never read
  // (limit_us <= 0 switches the time check off inside vm_sched_drain), so the
  // yield points are a pure function of the program. A time budget on a host
  // running under ASan would put them wherever the scheduler felt like.
  vm_budget_begin_full(&guest->budget, 0, budget_jobs ? budget_jobs : 1,
                       budget_jobs, budget_jobs);
}

// ------------------------------------------------------- --host-events
//
// The device's completion path in miniature: pocket_api_complete() records a
// completion from outside JS at any moment, and pocket_api_pump() is what
// settles it -- and the pump does not run on a continuation turn. host.request(k)
// asks for a completion that becomes ready at the k-th job boundary, and
// host_pump() settles every ready one, in the order they were requested, only
// when the queue has actually emptied. Invariant 6 of the design's sec.7: a
// completion is never lost and never delivered inside somebody else's drain.
#define HOST_REQUESTS 32
typedef struct {
  JSValue resolve, reject;
  uint64_t due;   // boundary index at which the completion is "recorded"
  bool live;
} host_request_t;
static host_request_t host_requests[HOST_REQUESTS];
static int n_host_requests;
static uint64_t boundaries;
static uint64_t host_delivered;

static JSValue host_request(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)this_val;
  int64_t k = 0;
  if (argc > 0 && JS_ToInt64(ctx, &k, argv[0]) < 0) return JS_EXCEPTION;
  if (n_host_requests >= HOST_REQUESTS) return JS_ThrowInternalError(ctx, "vmrun: too many host requests");
  JSValue funcs[2];
  JSValue promise = JS_NewPromiseCapability(ctx, funcs);
  if (JS_IsException(promise)) return promise;
  host_request_t *r = &host_requests[n_host_requests++];
  r->resolve = funcs[0];
  r->reject = funcs[1];
  r->due = boundaries + (uint64_t)(k < 0 ? 0 : k);
  r->live = true;
  return promise;
}

// pocket.app.exit() in miniature. The flag is set from inside a job, and the
// stop it asks for is delivered as an interrupt -- so honouring it at the top
// of a CONTINUATION turn would kill job k+1 of a drain that pre-L1 ran to its
// end, and whether an app's .finally ran would depend on where the budget
// happened to fall. run_turn() therefore reads it only where app_tick() reads
// it: on a turn that began with an empty queue.
static bool host_exit_requested;

static JSValue host_exit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)ctx;
  (void)this_val;
  (void)argc;
  (void)argv;
  host_exit_requested = true;
  return JS_UNDEFINED;
}

static bool host_pump(guest_t *guest) {
  (void)guest;
  if (!host_events) return false;
  bool any = false;
  for (int i = 0; i < n_host_requests; i++) {
    host_request_t *r = &host_requests[i];
    if (!r->live || r->due > boundaries) continue;
    r->live = false;
    JSValue seq = JS_NewInt64(G.context, (int64_t)++host_delivered);
    JSValue done = JS_Call(G.context, r->resolve, JS_UNDEFINED, 1, (JSValueConst *)&seq);
    JS_FreeValue(G.context, done);
    JS_FreeValue(G.context, seq);
    JS_FreeValue(G.context, r->resolve);
    JS_FreeValue(G.context, r->reject);
    any = true;
  }
  return any;
}

static void host_requests_free(void) {
  for (int i = 0; i < n_host_requests; i++)
    if (host_requests[i].live) {
      JS_FreeValue(G.context, host_requests[i].resolve);
      JS_FreeValue(G.context, host_requests[i].reject);
      host_requests[i].live = false;
    }
}

static void install_host(JSContext *ctx) {
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "request", JS_NewCFunction(ctx, host_request, "request", 1));
  JS_SetPropertyStr(ctx, o, "exit", JS_NewCFunction(ctx, host_exit, "exit", 0));
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "host", o);
  JS_FreeValue(ctx, global);
}

// One host turn: finish whatever the budget cut last time, then -- and only
// once the queue is EMPTY -- let the host deliver completions, which is the
// ordering rule of docs/vm-L1-design.md sec.2.1. Returns 0 ok, -1 a job threw
// or a rejection went unhandled, -5 runaway, -6 stopped with a queue.
static int run_turn(guest_t *guest) {
  unsigned run_turns = 0;
  uint64_t drain_jobs = 0;   // this LOGICAL drain, cleared when the queue empties
  for (;;) {
    JSContext *context = NULL;
    unsigned ran = 0;
    arm_budget(guest);
    const vm_drain_status_t status =
        vm_sched_drain(guest->runtime, &guest->budget, &ran, &context);
    guest->jobs += ran;
    drain_jobs += ran;
    boundaries++;  // every return from the drain is one job boundary
    if (ran > guest->max_queue_run) guest->max_queue_run = ran;
    if (status == VM_DRAIN_THREW) {
      fflush(stdout);
      if (context != NULL) js_std_dump_error(context);
      return -1;
    }
    if (status == VM_DRAIN_YIELDED) {
      guest->turns++;
      if (++run_turns > guest->max_run_turns) guest->max_run_turns = run_turns;
      if (stop_turns != 0 && run_turns >= stop_turns) return -6;
      if (runaway_jobs != 0 && drain_jobs >= runaway_jobs) {
        guest->runaway_jobs = drain_jobs;
        return -5;
      }
      // Compat ordering: nothing host-side runs in between, and that includes
      // the exit check -- sec.2.1's rule is that no host call reaches
      // JavaScript until the queue is empty, and an exit() honoured here
      // reaches it through the interrupt.
      //
      // Fair ordering runs the pump here and only here: after the drain has
      // had its budget, and only on a boundary where work is still queued,
      // which is precisely the boundary compat ordering delivers nothing on.
      // host_pump() settles by calling a resolve function, and that appends;
      // the reaction therefore goes behind the jobs of the unfinished drain.
      // The exit check stays below in BOTH modes.
      if (fair_mode) host_pump(guest);
      continue;
    }
    drain_jobs = 0;            // the logical drain ended; the next starts at 0
    // Where app_tick() reads the flag: queue empty, ahead of the pumps.
    if (host_exit_requested) {
      host_stopping = true;
      return -7;
    }
    if (!host_pump(guest)) break;  // queue empty and no completion to deliver
  }
  return report_rejections(guest);
}

// ---------------------------------------------------------------- $262

static JSValue make_262(JSContext *ctx);

static JSValue t262_eval_script(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  (void)this_val;
  size_t len;
  const char *src = JS_ToCStringLen(ctx, &len, argc > 0 ? argv[0] : JS_UNDEFINED);
  if (!src) return JS_EXCEPTION;
  JSValue r = JS_Eval(ctx, src, len, "<evalScript>", JS_EVAL_TYPE_GLOBAL);
  JS_FreeCString(ctx, src);
  return r;
}

static JSValue t262_gc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  JS_RunGC(JS_GetRuntime(ctx));
  return JS_UNDEFINED;
}

static JSValue t262_detach(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc > 0) JS_DetachArrayBuffer(ctx, argv[0]);
  return JS_NULL;
}

static JSValue t262_create_realm(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  if (G.n_realms >= (int)(sizeof(G.realms) / sizeof(G.realms[0])))
    return JS_ThrowInternalError(ctx, "vmrun: too many realms");
  JSContext *realm = JS_NewContext(JS_GetRuntime(ctx));
  if (!realm) return JS_ThrowOutOfMemory(ctx);
  G.realms[G.n_realms++] = realm;
  js_std_add_helpers(realm, 0, NULL);
  JSValue r = make_262(realm);
  if (JS_IsException(r)) return r;
  JSValue global = JS_GetGlobalObject(realm);
  JS_SetPropertyStr(realm, global, "$262", JS_DupValue(realm, r));
  JS_FreeValue(realm, global);
  return r;
}

static JSValue make_262(JSContext *ctx) {
  JSValue o = JS_NewObject(ctx);
  if (JS_IsException(o)) return o;
  JS_SetPropertyStr(ctx, o, "global", JS_GetGlobalObject(ctx));
  JS_SetPropertyStr(ctx, o, "evalScript", JS_NewCFunction(ctx, t262_eval_script, "evalScript", 1));
  JS_SetPropertyStr(ctx, o, "gc", JS_NewCFunction(ctx, t262_gc, "gc", 0));
  JS_SetPropertyStr(ctx, o, "detachArrayBuffer",
                    JS_NewCFunction(ctx, t262_detach, "detachArrayBuffer", 1));
  JS_SetPropertyStr(ctx, o, "createRealm", JS_NewCFunction(ctx, t262_create_realm, "createRealm", 0));
  return o;
}

// ---------------------------------------------------------------- forced yield

// L2 hook: a VM that grows OPCODE checkpoints defines this symbol and yields at
// every one of them. Weak, so an L1 VM (which has no such checkpoints) links.
// From L1 on, --force-yield is serviced at job granularity by --budget-jobs 1
// regardless of whether this symbol exists -- the L1 checkpoint IS the job
// boundary -- so the request is never unserviced and the note is gone.
//
// Defined since L2 by components/quickjs-ng/quickjs-ng/quickjs-vm.c (linked by
// build.sh). With it, --force-yield stops at every one of the seven class-A
// opcode safepoints (docs/vm-L2-design.md sec.7.2). Until L2c gives the VM a
// way to resume, a stop there is the uncatchable "interrupted" error, so the
// corpus is EXPECTED to go red under --force-yield -- that is the gate having
// teeth, not a harness defect. "#info vm ... stops=N" says how often it bit.
extern void vmtest_vm_set_force_yield(JSRuntime *rt, int on) __attribute__((weak));
// G5 (design sec.1.3): the VM records the interval between consecutive stop
// opportunities (safepoint, host entering JS, outermost frame popped) while JS
// is running, and reports the maximum. Time, not instruction count, because
// the sections that matter (regex, native-only work, a for-in step, direct
// eval parse -- N1..N4) contain no bytecode to count. Host nanoseconds under
// ASan are NOT the device's; the mechanism carries over, the numbers do not.
extern void vmtest_vm_set_gap_clock(JSRuntime *rt, uint64_t (*clock)(void), int on)
    __attribute__((weak));
extern void vmtest_vm_report(JSRuntime *rt, JSContext *ctx, void *out) __attribute__((weak));
// L2a: the segment stack frames live on (quickjs-vmstack.h). --vm-seg-size
// picks the standard segment before any JS runs; the report line is what
// decides the size (segments added, peak resident, largest frame).
extern int vmtest_vmstack_configure(JSRuntime *rt, size_t seg_size, unsigned cache_max)
    __attribute__((weak));
extern void vmtest_vmstack_report(JSRuntime *rt, void *out) __attribute__((weak));

// ---------------------------------------------------------------- helpers

static char *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  *out_len = got;
  return buf;
}

static size_t parse_size(const char *s) {
  char *end = NULL;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 0);
  if (errno || end == s) {
    fprintf(stderr, "vmrun: bad size '%s'\n", s);
    exit(3);
  }
  if (*end == 'k' || *end == 'K') v *= 1024ULL, end++;
  else if (*end == 'm' || *end == 'M') v *= 1024ULL * 1024ULL, end++;
  if (*end) {
    fprintf(stderr, "vmrun: bad size '%s'\n", s);
    exit(3);
  }
  return (size_t)v;
}

static const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// Machine-readable companion to js_std_dump_error for test262.py: the error's
// "name" when it has one, so negative tests can check the type.
static void report_uncaught(JSContext *ctx, JSValueConst exc, const char *phase) {
  const char *name = NULL;
  JSValue n = JS_UNDEFINED;
  if (JS_IsObject(exc)) {
    n = JS_GetPropertyStr(ctx, exc, "name");
    if (JS_IsString(n)) name = JS_ToCString(ctx, n);
    else if (JS_IsException(n)) JS_FreeValue(ctx, JS_GetException(ctx));
  }
  fprintf(stderr, "vmrun: uncaught %s %s\n", phase, name ? name : "<non-error>");
  if (name) JS_FreeCString(ctx, name);
  JS_FreeValue(ctx, n);
}

static void dump_exception(JSContext *ctx, const char *phase, bool machine) {
  fflush(stdout);
  JSValue exc = JS_GetException(ctx);
  JS_Throw(ctx, JS_DupValue(ctx, exc));
  js_std_dump_error(ctx);
  if (machine) report_uncaught(ctx, exc, phase);
  JS_FreeValue(ctx, exc);
}

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// The clock the G5 recorder samples at every stop opportunity.
static uint64_t gap_clock(void) { return (uint64_t)now_ns(); }

static void usage(void) {
  fprintf(stderr,
          "usage: vmrun [options] [--include FILE]... FILE.js\n"
          "  --profile device|host  device: heap 160K stack 20K (main/app_session.c)\n"
          "                         host:   heap 64M  stack 7M  (for Test262)\n"
          "  --heap-limit N[K|M]    override JS_SetMemoryLimit (0 = unlimited)\n"
          "  --stack-limit N[K|M]   override JS_SetMaxStackSize\n"
          "  --trace OUT            allocator trace (tools/vmtest/README.md)\n"
          "  --fail-alloc N         make allocation attempt N return NULL\n"
          "  --frames N             call globalThis.frame() N times, draining after each\n"
          "  --require-frame        like the guest: no frame() -> no drain after eval\n"
          "  --module               evaluate FILE as a module\n"
          "  --strict               prefix FILE with \"use strict\";\n"
          "  --test262              install $262, report 'vmrun: uncaught <phase> <Name>'\n"
          "  --force-yield          yield at every checkpoint: every job (= --budget-jobs 1) and, since\n"
          "                         L2, every opcode safepoint (kills the job until L2c can resume)\n"
          "  --gaps                 G5: record the longest stop-free interval, print '#info g5 ...'\n"
          "  --budget-jobs N        L1 count-mode budget: yield after N jobs, resume next turn (0 = off)\n"
          "  --runaway-jobs N       end the run when ONE logical drain has run N jobs (default off)\n"
          "  --stop-turns N         end the SESSION after N continuation turns, dropping the queue\n"
          "  --fair                 fair ordering (CONFIG_POCKET_VM_FAIR): pump on a continuation\n"
          "                         turn too, so a completion is seen mid-drain (default: compat)\n"
          "  --stack-probe          install __vmtest_stack_probe(); print '#info stack_probe ...'\n"
          "                         (G1: bytes of C stack per JS recursion level, see stack_probe.sh)\n"
          "  --vm-seg-size N[K]     L2a: standard frame-segment payload (default: the build's)\n"
          "  --host-events          install host.request(k) (a completion recorded at the k-th job\n"
          "                         boundary) and host.exit() (pocket.app.exit)\n"
          "  --time                 print '#info time_ns=...' (eval + drains + frames)\n"
          "  --stats                print '#info' allocator/queue statistics\n"
          "exit: 0 ok, 1 uncaught exception in eval, 2 job threw or unhandled rejection, 3 usage,\n"
          "      4 runtime creation failed, 5 job queue runaway\n");
  exit(3);
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv) {
  size_t heap_limit = 160U * 1024U;  // main/app_session.c gc.heap_limit
  size_t stack_limit = 20U * 1024U;  // main/app_session.c gc.stack_limit
  const char *trace_path = NULL;
  const char *includes[32];
  int n_includes = 0;
  const char *file = NULL;
  int frames = 0;
  bool require_frame = false, module = false, strict = false, test262 = false;
  bool force_yield = false, want_gaps = false, want_time = false, want_stats = false;
  size_t vm_seg_size = 0;  // --vm-seg-size; 0 = whatever the build compiled in
  const char *env_fy = getenv("VMTEST_FORCE_YIELD");
  if (env_fy && *env_fy && strcmp(env_fy, "0") != 0) force_yield = true;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(), (char *)NULL))
    if (!strcmp(a, "--profile")) {
      const char *p = NEXT();
      if (!strcmp(p, "device")) heap_limit = 160U * 1024U, stack_limit = 20U * 1024U;
      else if (!strcmp(p, "host")) heap_limit = 64U << 20, stack_limit = 7U << 20;
      else usage();
    } else if (!strcmp(a, "--heap-limit")) heap_limit = parse_size(NEXT());
    else if (!strcmp(a, "--stack-limit")) stack_limit = parse_size(NEXT());
    else if (!strcmp(a, "--trace")) trace_path = NEXT();
    else if (!strcmp(a, "--fail-alloc")) A.fail_at = parse_size(NEXT());
    else if (!strcmp(a, "--frames")) frames = (int)parse_size(NEXT());
    else if (!strcmp(a, "--require-frame")) require_frame = true;
    else if (!strcmp(a, "--module")) module = true;
    else if (!strcmp(a, "--strict")) strict = true;
    else if (!strcmp(a, "--test262")) test262 = true;
    else if (!strcmp(a, "--force-yield")) force_yield = true;
    else if (!strcmp(a, "--gaps")) want_gaps = true;
    else if (!strcmp(a, "--budget-jobs")) budget_jobs = (unsigned)parse_size(NEXT());
    else if (!strcmp(a, "--runaway-jobs")) runaway_jobs = (uint64_t)parse_size(NEXT());
    else if (!strcmp(a, "--stop-turns")) stop_turns = (unsigned)parse_size(NEXT());
    else if (!strcmp(a, "--host-events")) host_events = true;
    else if (!strcmp(a, "--fair")) fair_mode = true;
    else if (!strcmp(a, "--stack-probe")) stack_probe_enabled = true;
    else if (!strcmp(a, "--vm-seg-size")) vm_seg_size = parse_size(NEXT());
    else if (!strcmp(a, "--time")) want_time = true;
    else if (!strcmp(a, "--stats")) want_stats = true;
    else if (!strcmp(a, "--include")) {
      if (n_includes >= 32) usage();
      includes[n_includes++] = NEXT();
    } else if (a[0] == '-' && a[1] == '-') usage();
    else if (!file) file = a;
    else usage();
#undef NEXT
  }
  if (!file) usage();

  if (trace_path) {
    A.trace = fopen(trace_path, "w");
    if (!A.trace) {
      fprintf(stderr, "vmrun: cannot open trace '%s'\n", trace_path);
      return 3;
    }
    static char trace_buf[1 << 16];
    setvbuf(A.trace, trace_buf, _IOFBF, sizeof(trace_buf));
    fprintf(A.trace,
            "# vmtrace 1 file=%s heap_limit=%zu stack_limit=%zu sizeof_JSValue=%zu "
            "sizeof_ptr=%zu header=%zu\n",
            base_name(file), heap_limit, stack_limit, sizeof(JSValue), sizeof(void *),
            sizeof(allocation_header_t));
  }

  // Same order as pocketjs_guest_create().
  memset(&G, 0, sizeof(G));
  G.runtime = JS_NewRuntime2(&VM_ALLOCATOR, &G);
  if (!G.runtime) return 4;
  JS_SetMemoryLimit(G.runtime, heap_limit);
  JS_SetMaxStackSize(G.runtime, stack_limit);
  JS_SetRuntimeInfo(G.runtime, "PocketJS ESP-IDF guest");
  if (vm_seg_size) {
    // Before any JS runs: the bottom segment is pushed by the first call.
    if (!vmtest_vmstack_configure || vmtest_vmstack_configure(G.runtime, vm_seg_size, 1) != 0)
      fprintf(stderr, "vmrun: note: --vm-seg-size ignored, this VM keeps frames on the C stack\n");
  }
  // The guest always installs one; it answers "no" until app_stop() bumps the
  // epoch. Installed here so js_poll_interrupts pays the same callback cost.
  JS_SetInterruptHandler(G.runtime, vm_interrupt, &G);
  JS_SetHostPromiseRejectionTracker(G.runtime, promise_rejection, &G);
  js_std_init_handlers(G.runtime);
  if (module) JS_SetModuleLoaderFunc2(G.runtime, NULL, js_module_loader, js_module_check_attributes, NULL);
  G.context = JS_NewContext(G.runtime);
  if (!G.context) return 4;
  js_std_add_helpers(G.context, 0, NULL);

  JSValue probe = JS_UNDEFINED;
  if (A.trace) {
    JS_NewClassID(G.runtime, &probe_class_id);
    JS_NewClass(G.runtime, probe_class_id, &PROBE_CLASS);
    probe = JS_NewObjectClass(G.context, probe_class_id);
    fprintf(A.trace, "# ready\n");
  }
  if (test262) {
    JSValue global = JS_GetGlobalObject(G.context);
    JS_SetPropertyStr(G.context, global, "$262", make_262(G.context));
    JS_FreeValue(G.context, global);
  }
  if (force_yield) {
    // L1's checkpoint is the job boundary, so the strongest yield this level
    // can be asked for is "one job per turn". An L2 VM additionally switches
    // on its opcode checkpoints through the weak symbol above.
    if (budget_jobs == 0) budget_jobs = 1;
    if (vmtest_vm_set_force_yield) vmtest_vm_set_force_yield(G.runtime, 1);
    // G5 is recorded whenever yields are forced (design sec.1.3: "always on
    // during force-yield"); until L2c the record ends at the first stop of
    // each job, so --gaps alone is how a full run is measured today.
    want_gaps = true;
  }
  if (want_gaps) {
    if (vmtest_vm_set_gap_clock) vmtest_vm_set_gap_clock(G.runtime, gap_clock, 1);
    else fprintf(stderr, "vmrun: note: --gaps ignored, this VM has no G5 recorder\n");
  }
  if (host_events) install_host(G.context);
  if (stack_probe_enabled) {
    JSValue global = JS_GetGlobalObject(G.context);
    JS_SetPropertyStr(G.context, global, "__vmtest_stack_probe",
                       JS_NewCFunction(G.context, js_stack_probe, "__vmtest_stack_probe", 0));
    JS_FreeValue(G.context, global);
  }

  int status = 0;
  int64_t t0 = now_ns();

  for (int i = 0; i < n_includes && status == 0; i++) {
    size_t len;
    char *src = read_file(includes[i], &len);
    if (!src) {
      fprintf(stderr, "vmrun: cannot read '%s'\n", includes[i]);
      status = 3;
      break;
    }
    JSValue r = JS_Eval(G.context, src, len, base_name(includes[i]), JS_EVAL_TYPE_GLOBAL);
    free(src);
    if (JS_IsException(r)) {
      dump_exception(G.context, "include", test262);
      status = 1;
    }
    JS_FreeValue(G.context, r);
  }

  size_t len = 0;
  char *src = status == 0 ? read_file(file, &len) : NULL;
  if (status == 0 && !src) {
    fprintf(stderr, "vmrun: cannot read '%s'\n", file);
    status = 3;
  }
  if (status == 0 && strict) {
    static const char prefix[] = "\"use strict\";\n";
    char *s = malloc(len + sizeof(prefix));
    memcpy(s, prefix, sizeof(prefix) - 1);
    memcpy(s + sizeof(prefix) - 1, src, len + 1);
    free(src);
    src = s;
    len += sizeof(prefix) - 1;
  }

  if (status == 0) {
    const char *label = module ? file : base_name(file);
    const int type = module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
    JSValue result;
    if (test262) {
      // Split compile from run so a negative test can tell a parse-phase
      // SyntaxError from one thrown at runtime. The device uses a single
      // JS_Eval; this path is test262-only for that reason.
      JSValue fn = JS_Eval(G.context, src, len, label, type | JS_EVAL_FLAG_COMPILE_ONLY);
      if (JS_IsException(fn)) {
        dump_exception(G.context, "parse", true);
        status = 1;
        result = JS_UNDEFINED;
      } else {
        if (module && JS_ResolveModule(G.context, fn) < 0) {
          JS_FreeValue(G.context, fn);
          dump_exception(G.context, "resolution", true);
          status = 1;
          result = JS_UNDEFINED;
        } else {
          result = JS_EvalFunction(G.context, fn);
        }
      }
    } else {
      result = JS_Eval(G.context, src, len, label, type);
    }
    if (status == 0 && JS_IsException(result)) {
      dump_exception(G.context, "runtime", test262);
      status = 1;
    }
    if (status == 0) {
      JSValue global = JS_GetGlobalObject(G.context);
      JSValue frame = JS_GetPropertyStr(G.context, global, "frame");
      JS_FreeValue(G.context, global);
      const bool has_frame = JS_IsFunction(G.context, frame);
      if (!require_frame || has_frame) {
        const int r = run_turn(&G);
        if (r == -5) status = 5;
        else if (r == -6 || r == -7) status = 0;   // stopped: queue dropped / exit()
        else if (r != 0) status = 2;
        if (r == -7) frames = 0;                   // no frame() after an exit()
      }
      // A module's evaluation promise: a rejected top-level await is the
      // module equivalent of an uncaught exception.
      if (module && JS_IsObject(result) && JS_PromiseState(G.context, result) == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(G.context, result);
        JS_Throw(G.context, reason);
        dump_exception(G.context, "runtime", test262);
        status = 1;
      }
      for (int f = 0; f < frames && has_frame && status != 5; f++) {
        // The device's rule: frame() is only ever called on a turn that began
        // with an empty queue (sec.2.1). run_turn() guarantees that -- it does
        // not return with work pending except on runaway or --stop-turns.
        JSValue r = JS_Call(G.context, frame, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) {
          dump_exception(G.context, "frame", test262);
          if (status == 0) status = 1;
        }
        JS_FreeValue(G.context, r);
        const int t = run_turn(&G);
        if (t == -5) status = 5;
        else if (t == -6) break;   // session ends here, with a queue (sec.3.2)
        else if (t == -7) break;   // the guest asked to exit; sec.2.2
        else if (t != 0 && status == 0) status = 2;
      }
      JS_FreeValue(G.context, frame);
    }
    JS_FreeValue(G.context, result);
  }
  free(src);
  int64_t t1 = now_ns();

  fflush(stdout);
  // Read before teardown, so "#info jobs_dropped" below is the answer for the
  // run rather than for the runtime that no longer exists.
  G.jobs_dropped = JS_IsJobPending(G.runtime);
  if (want_time) fprintf(stderr, "#info time_ns=%lld\n", (long long)(t1 - t0));
  if (want_stats) {
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(G.runtime, &usage);
    fprintf(stderr,
            "#info alloc peak_bytes=%zu peak_blocks=%zu max_block=%zu live_bytes=%zu "
            "mallocs=%llu frees=%llu reallocs=%llu fails=%llu qjs_malloc_size=%lld\n",
            A.peak_bytes, A.peak_blocks, A.max_block, A.live_bytes,
            (unsigned long long)A.n_malloc, (unsigned long long)A.n_free,
            (unsigned long long)A.n_realloc, (unsigned long long)A.n_fail,
            (long long)usage.malloc_size);
    fprintf(stderr, "#info jobs=%llu max_drain_run=%zu sizeof_JSValue=%zu header=%zu\n",
            (unsigned long long)G.jobs, G.max_queue_run, sizeof(JSValue),
            sizeof(allocation_header_t));
  }
  if (stack_probe_enabled) {
    // calls-1, not calls: the first probe already sits inside one level of
    // recursion relative to top-level code, so dividing by the call count
    // would fold that fixed offset into what should be a per-ADDITIONAL-level
    // figure. With 0 or 1 calls there is no level-to-level delta to report.
    uint64_t steps = stack_probe_calls > 1 ? stack_probe_calls - 1 : 0;
    uintptr_t hi = stack_probe_first > stack_probe_last ? stack_probe_first : stack_probe_last;
    uintptr_t lo = stack_probe_first > stack_probe_last ? stack_probe_last : stack_probe_first;
    uint64_t span = (uint64_t)(hi - lo);
    double per_call = steps ? (double)span / (double)steps : 0.0;
    fprintf(stderr, "#info stack_probe calls=%llu span_bytes=%llu bytes_per_call=%.3f\n",
            (unsigned long long)stack_probe_calls, (unsigned long long)span, per_call);
  }
  // Always printed, not only under --stats: the L1 budget cases read these,
  // and "#info" lines are excluded from the corpus diff, so they cost nothing
  // and cannot make an expected file depend on how the budget was set.
  fprintf(stderr, "#info turns=%llu max_run_turns=%u jobs_dropped=%d\n",
          (unsigned long long)G.turns, G.max_run_turns, G.jobs_dropped ? 1 : 0);
  // "#info vm ..." (safepoints seen, forced stops taken) and "#info g5 ..."
  // (the longest stop-free interval) whenever the VM was armed. Before
  // teardown: the report resolves function-name atoms through the context.
  if (vmtest_vm_report) vmtest_vm_report(G.runtime, G.context, stderr);
  // "#info vmstack ..." only under --stats: the corpus does not need it and
  // the info files stay readable.
  if (want_stats && vmtest_vmstack_report) vmtest_vmstack_report(G.runtime, stderr);
  // The count goes on an #info line, not on the diffed one: run.sh re-runs the
  // whole corpus at several budgets, and the job total at which the guard fires
  // is the first multiple of the budget past the limit. The FACT of the runaway
  // is what must not depend on where the budget fell.
  if (status == 5) {
    fprintf(stderr, "#info runaway_jobs=%llu\n", (unsigned long long)G.runaway_jobs);
    fprintf(stderr, "vmrun: job queue runaway: one drain never emptied\n");
  }

  // Same teardown order as pocketjs_guest_destroy().
  if (A.trace) {
    JS_FreeValue(G.context, probe);
    fprintf(A.trace, "# teardown\n");
  }
  // sec.3.2: what is still queued is discarded UNRUN. JS_FreeRuntime frees the
  // job arguments (ledger 03 fact 9), so this leaks nothing -- LSan is the
  // check -- and the leftover rejections below are freed WITHOUT being
  // reported, because a catch may well have been in one of the dropped jobs.
  host_requests_free();
  js_std_free_handlers(G.runtime);
  while (G.rejections) {
    rejection_t *entry = G.rejections;
    G.rejections = entry->next;
    JS_FreeValue(G.context, entry->promise);
    JS_FreeValue(G.context, entry->reason);
    free(entry);
  }
  for (int i = G.n_realms - 1; i >= 0; i--) JS_FreeContext(G.realms[i]);
  JS_FreeContext(G.context);
  JS_FreeRuntime(G.runtime);
  if (A.trace) {
    fprintf(A.trace, "# end live_bytes=%zu live_blocks=%zu gcs=%llu\n", A.live_bytes,
            A.live_blocks, (unsigned long long)gc_count);
    fclose(A.trace);
  }
  return status;
}
