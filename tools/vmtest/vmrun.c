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
} guest_t;

static guest_t G;

static int vm_interrupt(JSRuntime *rt, void *opaque) {
  (void)rt;
  (void)opaque;
  return 0;
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

// Copied from guest.c drain_jobs(). Returns 0 on ESP_OK, -1 on ESP_FAIL. The
// report line keeps the guest's wording; the "E pocketjs_guest:" prefix stands
// in for ESP_LOGE's "E (ticks) pocketjs_guest:" without the timestamp.
static int drain_jobs(guest_t *guest) {
  JSContext *context = NULL;
  int result = 0;
  size_t run = 0;
  while ((result = JS_ExecutePendingJob(guest->runtime, &context)) > 0) {
    guest->jobs++;
    run++;
  }
  if (run > guest->max_queue_run) guest->max_queue_run = run;
  if (result < 0) {
    fflush(stdout);
    if (context != NULL) js_std_dump_error(context);
    return -1;
  }
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

// Placeholder for L1/L2: a VM that grows checkpoints defines this symbol and
// yields at every checkpoint when it is switched on. Weak, so an L0 VM links
// and the request is reported as unserviced instead of silently ignored. The
// corpus must print the same bytes with and without it; that is the test.
extern void vmtest_vm_set_force_yield(JSRuntime *rt, int on) __attribute__((weak));

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
          "  --force-yield          ask the VM to yield at every checkpoint (L1+; env VMTEST_FORCE_YIELD=1)\n"
          "  --time                 print '#info time_ns=...' (eval + drains + frames)\n"
          "  --stats                print '#info' allocator/queue statistics\n"
          "exit: 0 ok, 1 uncaught exception in eval, 2 job threw or unhandled rejection, 3 usage\n");
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
  bool force_yield = false, want_time = false, want_stats = false;
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
    if (vmtest_vm_set_force_yield) vmtest_vm_set_force_yield(G.runtime, 1);
    else fprintf(stderr, "vmrun: note: force-yield requested; this VM has no checkpoint hook (L0), running unmodified\n");
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
        if (drain_jobs(&G) != 0) status = 2;
      }
      // A module's evaluation promise: a rejected top-level await is the
      // module equivalent of an uncaught exception.
      if (module && JS_IsObject(result) && JS_PromiseState(G.context, result) == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(G.context, result);
        JS_Throw(G.context, reason);
        dump_exception(G.context, "runtime", test262);
        status = 1;
      }
      for (int f = 0; f < frames && has_frame; f++) {
        JSValue r = JS_Call(G.context, frame, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) {
          dump_exception(G.context, "frame", test262);
          if (status == 0) status = 1;
        }
        JS_FreeValue(G.context, r);
        if (drain_jobs(&G) != 0 && status == 0) status = 2;
      }
      JS_FreeValue(G.context, frame);
    }
    JS_FreeValue(G.context, result);
  }
  free(src);
  int64_t t1 = now_ns();

  fflush(stdout);
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

  // Same teardown order as pocketjs_guest_destroy().
  if (A.trace) {
    JS_FreeValue(G.context, probe);
    fprintf(A.trace, "# teardown\n");
  }
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
