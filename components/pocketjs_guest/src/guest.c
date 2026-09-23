#include "pocketjs/guest.h"
#include "pocketjs/guest_quickjs.h"
#include "pocketjs/vm_sched.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "quickjs-libc.h"
#include "quickjs-vm.h"
#if defined(CONFIG_POCKET_VM_YIELD) || defined(CONFIG_POCKET_VM_PROBE)
#include "esp_timer.h"
#endif
#ifdef CONFIG_POCKET_VM_YIELD
#include "freertos/FreeRTOS.h"
#endif
#ifdef CONFIG_POCKET_VM_PROBE
#include <stdio.h>
#include "quickjs-vm.h"
#endif

static const char *TAG = "pocketjs_guest";

#ifdef CONFIG_POCKET_VM_PROBE
/* VM_PROBE (docs/vm/quickjs-freertos-vm-spec.md sec.5): frame() time and drain
 * time, kept apart. main/pocket/vmprobe.c times the whole pocketjs_ui_turn(),
 * which is frame() + drain + the UI core's tick and draw; sec.5 asks for drain
 * time on its own and this is the only file that can see where drain starts.
 * File scope, not a guest field: one guest runs at a time (sec.3 rule 1), and a
 * field would change the struct layout between probe and shipping builds. */
#include "esp_timer.h"
static uint32_t vmprobe_call_us, vmprobe_drain_us;

void pocketjs_guest_vmprobe_take(uint32_t *call_us, uint32_t *drain_us) {
  if (call_us != NULL)
    *call_us = vmprobe_call_us;
  if (drain_us != NULL)
    *drain_us = vmprobe_drain_us;
  vmprobe_call_us = 0;
  vmprobe_drain_us = 0;
}

/* vm-l1-tuning (docs/vm/vm-L1-report.md sec.10): jobs actually returned by ONE
 * vm_sched_drain() call, recorded from drain_jobs() -- the single choke
 * point both pocketjs_guest_frame() (the frame()-triggering call) and
 * pocketjs_guest_continue() (a continuation of the same logical drain, sec.2.1)
 * go through. The existing "jobs" vmprobe sample cannot answer "does the
 * floor/stride check ever actually fire": it is a per-APP_TICK sample, and a
 * continuation tick never reaches vmprobe_frame_sample() (main/app_session.c's
 * early return at a pending queue), so its job count is invisible until it
 * gets folded into the NEXT frame-tick's sample -- 8 (floor-cut) + 33
 * (continuation, unreported) reads as one 41, indistinguishable from a drain
 * that never yielded. Raw per-call values, not a bucketed count, for the same
 * reason vmprobe.c gives every other metric raw: the shape (not just the
 * median) is the question. */
#define VMPROBE_DRAIN_CALL_CAP 128
static uint16_t vmprobe_drain_calls[VMPROBE_DRAIN_CALL_CAP];
static unsigned vmprobe_drain_call_count, vmprobe_drain_call_dropped;

static void vmprobe_drain_call_record(unsigned ran) {
  if (vmprobe_drain_call_count < VMPROBE_DRAIN_CALL_CAP)
    vmprobe_drain_calls[vmprobe_drain_call_count++] =
        ran > 0xffffu ? 0xffffu : (uint16_t)ran;
  else
    vmprobe_drain_call_dropped++;
}

unsigned pocketjs_guest_vmprobe_drain_calls(uint16_t *out, unsigned cap,
                                            unsigned *dropped) {
  unsigned n = vmprobe_drain_call_count < cap ? vmprobe_drain_call_count : cap;
  if (out != NULL)
    for (unsigned i = 0; i < n; i++)
      out[i] = vmprobe_drain_calls[i];
  if (dropped != NULL)
    *dropped = vmprobe_drain_call_dropped;
  vmprobe_drain_call_count = 0;
  vmprobe_drain_call_dropped = 0;
  return n;
}
#endif

/* Guest allocations. On the part there is no header in front of a block
 * (vm/backlog.md items 8 and 9): QuickJS's usable size is the block length
 * tlsf actually handed out, read back with heap_caps_get_allocated_size(),
 * and realloc is heap_caps_realloc().
 *
 * Why the real length and not the requested one: QuickJS treats
 * `usable - requested` as slack (js_realloc2 hands it back to dbuf, strings
 * and arrays; the string concatenation fast path extends into it), so
 * reporting the request threw that slack away and turned every growth that
 * would have fitted into a realloc. And malloc_limit is charged in usable
 * sizes, so the 160 KiB limit now counts what the heap really gives the guest
 * instead of the requests: tlsf rounds a request up to 4 B with a 12 B
 * minimum, and keeps up to 15 B more when the rest of the free block is too
 * small to split off (tlsf_control_functions.h adjust_request_size,
 * block_can_split). The old 4 B header was outside the usable size; only
 * QuickJS's flat per-block MALLOC_OVERHEAD stood for it. One consequence: the
 * charged size of a block now depends on the heap's free-block layout, so the
 * exact byte at which the limit trips is no longer a function of the program
 * alone.
 *
 * Why heap_caps_realloc: tlsf_realloc grows a block in place when the
 * physical block after it is free and large enough, and shrinks in place
 * always. The old malloc+memcpy+free held both copies at once and copied
 * every time. On failure heap_caps_realloc returns NULL and leaves the
 * original block allocated and unchanged (heap_caps_base.c: the in-heap
 * tlsf_realloc fails without freeing, and the cross-heap fallback frees the
 * old block only after the new one exists), which is what js_realloc_rt
 * expects.
 *
 * Host builds keep a size header: there is no tlsf to ask. */
#ifndef ESP_PLATFORM
typedef union {
  size_t size;
  max_align_t alignment;
} allocation_header_t;
#endif

typedef struct rejection {
  JSValue promise;
  JSValue reason;
  struct rejection *next;
} rejection_t;

typedef struct surface {
  char *name;
  struct surface *next;
} surface_t;

struct pocketjs_guest {
  JSRuntime *runtime;
  JSContext *context;
  JSValue frame;
  size_t heap_limit;
  bool prefer_psram;
  atomic_uint interrupt_epoch;
  unsigned int handled_interrupt_epoch;
  rejection_t *rejections;
  bool rejection_tracking_failed;
  surface_t *surfaces;
  uint32_t frames;
  uint32_t frame_errors;
  uint32_t jobs;
  /* L1 (docs/vm/vm-L1-design.md). The budget is armed by the host once per turn
   * and read by every drain in that turn, so the frame()'s drain and the next
   * turn's continuation drain share one deadline measured from turn start. */
  vm_budget_t budget;
  bool jobs_pending;
#ifdef CONFIG_POCKET_VM_YIELD
  bool suspended;
  JSVMOrigin origin; /* HOST means the logical frame call, including async */
  int64_t frame_us;
  esp_timer_handle_t yield_timer;
  bool yield_disabled;
#ifdef CONFIG_POCKET_VM_SELFTEST
  bool trace_frame;
#endif
#ifdef CONFIG_POCKET_VM_RELOC
  /* L3a on the device (docs/vm/vm-L3-design.md sec.7). `reloc_armed` is off
   * until a host asks for it, so a RELOC build that nobody arms runs the
   * same code path as a build without it -- the switch decides whether the
   * call happens, not whether it is compiled.
   *
   * Counted rather than assumed, the same reason vmrun counts them: refusals
   * are legitimate (D55 -- a park underneath an outer JS activation cannot
   * move) and a run of all refusals would otherwise look exactly like a run
   * of successful moves. `reloc_max_us` is the worst single move, which is
   * what a stop-the-world budget would have to be written against; the mean
   * hides it. */
  bool reloc_armed;
  uint32_t reloc_moves, reloc_refused;
  /* The third outcome, which the first device run showed is NOT rare: parked,
   * allowed to move, and nothing to move -- the live segment chain is empty
   * because the parked frames are all coroutine frames in their own
   * JSAsyncFunctionState, not in segments. Diagnostic '6' (an endless promise
   * chain) parks 31 times and lands here every time. Counted separately
   * because moves=0 refused=0 otherwise reads as "the call never happened",
   * which is what it looked like until this counter existed. */
  uint32_t reloc_empty;
  uint32_t reloc_frames, reloc_var_refs;
  uint32_t reloc_max_us;
  uint64_t reloc_total_us, reloc_bytes;
  /* What the heap looked like around the moves. L3a does NOT compact: a move
   * allocates blocks of the SAME sizes, copies into them, and frees the old
   * ones, so both are held at once and the transient cost is exactly the
   * chain's own size (reloc_bytes). Whether the heap ends up better or worse
   * laid out afterwards is a side effect nobody designed, which is precisely
   * why it has to be measured rather than argued.
   *
   * Largest free block, not free size: free size barely moves here (the same
   * bytes are given back), and the number that decides whether an app can
   * still get a 9.9 KiB Kasane arena is the largest CONTIGUOUS one.
   *
   * Sampled outside the timed region so that reloc_max_us stays a measurement
   * of the move rather than of heap_caps_get_largest_free_block(). */
  /* How far apart the heap put the pieces of one stack (JSVMRelocStats.span
   * minus .resident: bytes of OTHER allocations wedged between this stack's
   * segments). On the host this turned out to track chain DEPTH rather than
   * park count -- a corpus file that parked 5,807 times kept a 2 KB gap,
   * while one that only went deep reached 29 KB. The device is the case the
   * host cannot answer, because here the firmware runs native work in the
   * SAME pool while the chain sits parked, and vmrun's park runs nothing. */
  uint32_t reloc_gap_max;
  uint32_t reloc_gap_segments;   /* chain depth when that gap was seen */
  uint32_t reloc_largest_first;  /* before the first move */
  uint32_t reloc_largest_last;   /* after the last one */
  uint32_t reloc_largest_min;    /* worst sample either side of any move */
#endif
#endif
  uint32_t yields;
  uint32_t continuations;
  /* What ONE LOGICAL DRAIN has cost so far (sec.5.2): summed over the drain
   * the budget cut and every continuation of it, cleared the moment the queue
   * empties. The runaway guard is a predicate on these two and on nothing
   * else -- not on a turn count, which measures the machine's contention
   * rather than the guest's appetite. `drain_us` stays 0 in count mode, where
   * the clock is never read, and the job total is what catches a runaway
   * there. */
  int64_t drain_us;
  uint64_t drain_jobs;
  /* One bit, not a count: counting what JS_FreeRuntime discards would need a
   * hook in JS_EnqueueJob, which is a VM change L1 is not allowed to make. */
  bool jobs_dropped;
  /* sec.5.3: JS_SetInterruptHandler has a single slot and three callers used
   * to overwrite each other. The registration stays here; the host swaps the
   * predicate instead. NULL means the guest's own epoch handler. */
  int (*watchdog)(void *);
  void *watchdog_opaque;
};

#ifdef CONFIG_POCKET_VM_YIELD
/* The callback never keeps a guest pointer. Clearing this slot under the
 * same lock joins its last runtime access even if stop races a fired timer.
 * Only one guest executes JS at a time, as required by the host contract. */
static portMUX_TYPE yield_mux = portMUX_INITIALIZER_UNLOCKED;
static JSRuntime *yield_runtime;
static int64_t yield_deadline;

static void yield_alarm(void *unused) {
  (void)unused;
  portENTER_CRITICAL(&yield_mux);
  if (yield_runtime && esp_timer_get_time() >= yield_deadline)
    JS_VMRequestYield(yield_runtime);
  portEXIT_CRITICAL(&yield_mux);
}

static void guest_run_end(pocketjs_guest_t *guest) {
  if (!guest || !guest->runtime) return;
  portENTER_CRITICAL(&yield_mux);
  if (yield_runtime == guest->runtime) yield_runtime = NULL;
  portEXIT_CRITICAL(&yield_mux);
  if (guest->yield_timer) (void)esp_timer_stop(guest->yield_timer);
  JS_VMClearYield(guest->runtime);
}

static esp_err_t guest_run_begin(pocketjs_guest_t *guest) {
  if (!guest || !guest->runtime || guest->yield_disabled ||
      guest->budget.limit_ticks == 0) return ESP_OK;
  if (!guest->yield_timer) {
    const esp_timer_create_args_t args = {
      .callback = yield_alarm, .name = "vm-yield"};
    esp_err_t err = esp_timer_create(&args, &guest->yield_timer);
    if (err != ESP_OK) return err;
  }
  const vm_clock_fn clock = guest->budget.clock ? guest->budget.clock : vm_clock_now;
  const vm_tick_t elapsed = clock() - guest->budget.start;
  if (elapsed >= guest->budget.limit_ticks) {
    JS_VMRequestYield(guest->runtime);
    return ESP_OK;
  }
  uint64_t delay = (guest->budget.limit_ticks - elapsed) / guest->budget.ticks_per_us;
  if (!delay) delay = 1;
  portENTER_CRITICAL(&yield_mux);
  yield_runtime = guest->runtime;
  yield_deadline = esp_timer_get_time() + delay;
  portEXIT_CRITICAL(&yield_mux);
  esp_err_t err = esp_timer_start_once(guest->yield_timer, delay);
  if (err != ESP_OK) guest_run_end(guest);
  return err;
}
#else
static esp_err_t guest_run_begin(pocketjs_guest_t *guest) {
  (void)guest;
  return ESP_OK;
}
static void guest_run_end(pocketjs_guest_t *guest) { (void)guest; }
#endif

#ifdef ESP_PLATFORM
#define GUEST_CAPS_INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define GUEST_CAPS_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static void *guest_malloc(void *opaque, size_t size) {
  pocketjs_guest_t *guest = opaque;
  if (size == 0U) {
    return NULL;
  }
  void *memory = NULL;
  if (guest != NULL && guest->prefer_psram) {
    memory = heap_caps_malloc(size, GUEST_CAPS_PSRAM);
  }
  if (memory == NULL) {
    memory = heap_caps_malloc(size, GUEST_CAPS_INTERNAL);
  }
  return memory;
}

static void *guest_calloc(void *opaque, size_t count, size_t size) {
  if (count != 0U && size > SIZE_MAX / count) {
    return NULL;
  }
  const size_t total = count * size;
  void *memory = guest_malloc(opaque, total);
  if (memory != NULL) {
    memset(memory, 0, total);
  }
  return memory;
}

static void guest_free(void *opaque, void *pointer) {
  (void)opaque;
  heap_caps_free(pointer);
}

static size_t guest_usable_size(const void *pointer) {
  return pointer == NULL ? 0U : heap_caps_get_allocated_size((void *)pointer);
}

static void *guest_realloc(void *opaque, void *pointer, size_t size) {
  pocketjs_guest_t *guest = opaque;
  if (pointer == NULL) {
    return guest_malloc(opaque, size);
  }
  if (size == 0U) {
    guest_free(opaque, pointer);
    return NULL;
  }
  if (guest != NULL && guest->prefer_psram) {
    return heap_caps_realloc_prefer(pointer, size, 2, GUEST_CAPS_PSRAM,
                                    GUEST_CAPS_INTERNAL);
  }
  return heap_caps_realloc(pointer, size, GUEST_CAPS_INTERNAL);
}
#else
static void *guest_malloc(void *opaque, size_t size) {
  pocketjs_guest_t *guest = opaque;
  if (size == 0U || size > SIZE_MAX - sizeof(allocation_header_t)) {
    return NULL;
  }
  const size_t total = sizeof(allocation_header_t) + size;
  allocation_header_t *header = NULL;
  if (guest != NULL && guest->prefer_psram) {
    header = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (header == NULL) {
    header = heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (header == NULL) {
    return NULL;
  }
  header->size = size;
  return header + 1;
}

static void *guest_calloc(void *opaque, size_t count, size_t size) {
  if (count != 0U && size > SIZE_MAX / count) {
    return NULL;
  }
  const size_t total = count * size;
  void *memory = guest_malloc(opaque, total);
  if (memory != NULL) {
    memset(memory, 0, total);
  }
  return memory;
}

static void guest_free(void *opaque, void *pointer) {
  (void)opaque;
  if (pointer != NULL) {
    heap_caps_free(((allocation_header_t *)pointer) - 1);
  }
}

static size_t guest_usable_size(const void *pointer) {
  return pointer == NULL ? 0U
                         : (((const allocation_header_t *)pointer) - 1)->size;
}

static void *guest_realloc(void *opaque, void *pointer, size_t size) {
  if (pointer == NULL) {
    return guest_malloc(opaque, size);
  }
  if (size == 0U) {
    guest_free(opaque, pointer);
    return NULL;
  }
  const size_t previous_size = guest_usable_size(pointer);
  void *next = guest_malloc(opaque, size);
  if (next == NULL) {
    return NULL;
  }
  memcpy(next, pointer, previous_size < size ? previous_size : size);
  guest_free(opaque, pointer);
  return next;
}

#endif

static const JSMallocFunctions GUEST_ALLOCATOR = {
    .js_calloc = guest_calloc,
    .js_malloc = guest_malloc,
    .js_free = guest_free,
    .js_realloc = guest_realloc,
    .js_malloc_usable_size = guest_usable_size,
};

static int guest_interrupt(JSRuntime *runtime, void *opaque) {
  (void)runtime;
  pocketjs_guest_t *guest = opaque;
  if (guest == NULL)
    return 0;
  /* sec.5.3: one registration, a swappable predicate. The host's watchdog (the
   * 250 ms deadline, or the stop hook's 200 ms one) answers for the whole turn
   * when it is installed; the epoch handler below is what is left when nobody
   * has claimed the slot. */
  if (guest->watchdog != NULL)
    return guest->watchdog(guest->watchdog_opaque);
  const unsigned int requested =
      atomic_load_explicit(&guest->interrupt_epoch, memory_order_relaxed);
  if (requested == guest->handled_interrupt_epoch)
    return 0;
  guest->handled_interrupt_epoch = requested;
  return 1;
}

static void promise_rejection(JSContext *context, JSValueConst promise,
                              JSValueConst reason, bool handled, void *opaque) {
  pocketjs_guest_t *guest = opaque;
  rejection_t **slot = &guest->rejections;
  while (*slot &&
         JS_VALUE_GET_PTR((*slot)->promise) != JS_VALUE_GET_PTR(promise))
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
  if (*slot)
    return;
  rejection_t *entry = calloc(1, sizeof(*entry));
  if (!entry) {
    guest->rejection_tracking_failed = true;
    return;
  }
  entry->promise = JS_DupValue(context, promise);
  entry->reason = JS_DupValue(context, reason);
  *slot = entry;
}

/* The report point, sec.3.1: only at a boundary where the queue actually went
 * empty. Reporting at a budget boundary would call a rejection unhandled that
 * a catch two jobs later in the SAME logical drain is about to handle --
 * rejections.js case 2 is exactly that shape. */
static esp_err_t report_rejections(pocketjs_guest_t *guest) {
  bool failed = guest->rejection_tracking_failed;
  guest->rejection_tracking_failed = false;
  size_t pending = 0;
  for (rejection_t *entry = guest->rejections; entry; entry = entry->next)
    ++pending;
  while (pending--) {
    rejection_t *entry = guest->rejections;
    if (!entry)
      break;
    guest->rejections = entry->next;
    failed = true;
    /* Detach before toString can reenter the tracker. Reported promises no
     * longer need a retained reference; a later handled event is ignored. */
    const char *text = JS_ToCString(guest->context, entry->reason);
    ESP_LOGE(TAG, "Unhandled Promise rejection: %s", text ? text : "<value>");
    if (text)
      JS_FreeCString(guest->context, text);
    else
      JS_FreeValue(guest->context, JS_GetException(guest->context));
    JS_FreeValue(guest->context, entry->reason);
    JS_FreeValue(guest->context, entry->promise);
    free(entry);
  }
  return failed ? ESP_FAIL : ESP_OK;
}

/* One pass of the drain under whatever budget the host armed. With an
 * unlimited budget (limit_us <= 0, backstop 0 -- what the Kconfig switch off
 * produces) the loop cannot yield, so this is byte-for-byte the pre-L1
 * behaviour: drain to empty, then report. */
static esp_err_t drain_jobs(pocketjs_guest_t *guest) {
  JSContext *context = NULL;
  unsigned ran = 0;
  const vm_drain_status_t status =
      vm_sched_drain(guest->runtime, &guest->budget, &ran, &context);
#ifdef CONFIG_POCKET_VM_PROBE
  vmprobe_drain_call_record(ran);
#endif
  guest->jobs += ran;
  guest->drain_us += guest->budget.elapsed;
  guest->drain_jobs += ran;
  guest->jobs_pending = (status == VM_DRAIN_YIELDED);
  if (status == VM_DRAIN_SUSPENDED) {
#ifdef CONFIG_POCKET_VM_YIELD
    guest->suspended = true;
    guest->origin = JS_VMSuspendedOrigin(guest->runtime);
    guest->jobs_pending = JS_IsJobPending(guest->runtime) ||
                          guest->origin == JS_VM_ORIGIN_JOB_HELD;
    guest->yields++;
    return ESP_OK;
#else
    return ESP_FAIL;
#endif
  }
  if (status == VM_DRAIN_THREW) {
    if (context != NULL) {
      js_std_dump_error(context);
    }
    /* The session ends on this path (main.c end_run), and the queue that is
     * left is the next drain's, not this logical one's: start the count over
     * rather than charge the survivor for it. */
    guest->drain_us = 0;
    guest->drain_jobs = 0;
    JS_VMStackTrim(guest->runtime);
    return ESP_FAIL;
  }
  if (status == VM_DRAIN_YIELDED) {
    guest->yields++;
    /* Not a failure: nothing was dropped and nothing threw. The caller runs
     * its native work and comes back through pocketjs_guest_continue(). */
    return ESP_OK;
  }
  /* The queue emptied: this logical drain is over and the next one starts at
   * zero. Cleared before the report, which can itself fail the turn. */
  guest->drain_us = 0;
  guest->drain_jobs = 0;
  /* D43 (docs/vm/vm-L2-design.md sec.7.3): the turn is over, so the frame
   * segments the stack kept for reuse during it go back to the heap. Kept
   * across a YIELDED drain above, which is the same logical turn. */
  JS_VMStackTrim(guest->runtime);
  return report_rejections(guest);
}

void pocketjs_guest_config_defaults(pocketjs_guest_config_t *config) {
  if (config == NULL) {
    return;
  }
  *config = (pocketjs_guest_config_t){
      .struct_size = sizeof(*config),
      .heap_limit = 4U * 1024U * 1024U,
      .stack_limit = 256U * 1024U,
      .prefer_psram = true,
  };
}

esp_err_t pocketjs_guest_create(const pocketjs_guest_config_t *config,
                                pocketjs_guest_t **out_guest) {
  if (config == NULL || out_guest == NULL ||
      config->struct_size < sizeof(*config) || config->heap_limit == 0U ||
      config->stack_limit == 0U) {
    return ESP_ERR_INVALID_ARG;
  }
  *out_guest = NULL;
  pocketjs_guest_t *guest = calloc(1, sizeof(*guest));
  if (guest == NULL) {
    return ESP_ERR_NO_MEM;
  }
  guest->frame = JS_UNDEFINED;
  guest->heap_limit = config->heap_limit;
  guest->prefer_psram = config->prefer_psram;
  atomic_init(&guest->interrupt_epoch, 0U);
  /* Unlimited until a host arms a turn. Evaluation is not a turn: the source
   * is parsed once, before any frame, and cutting its drain would leave an app
   * half-initialised before it ever ran. */
  vm_budget_begin(&guest->budget, 0);
  guest->runtime = JS_NewRuntime2(&GUEST_ALLOCATOR, guest);
  if (guest->runtime == NULL) {
    pocketjs_guest_destroy(guest);
    return ESP_ERR_NO_MEM;
  }
  JS_SetMemoryLimit(guest->runtime, config->heap_limit);
  /* quickjs-ng starts the cycle collector's threshold at 256 KiB, above the
   * device's 160 KiB limit, so before this no collection ever ran and cyclic
   * garbage grew into an OOM (docs/vm/backlog.md #5). Half the limit puts the
   * first collection well inside the heap, so cyclic garbage is first
   * collected at half the limit of the shared DRAM rather than near all of it
   * (later thresholds follow upstream's 1.5x of the survivors); quickjs.c's
   * js_gc_effective_threshold keeps every collection under the limit. Only
   * lowered, never raised: a large host limit keeps upstream's timing, which
   * tools/vmtest's expected outputs depend on.
   * tools/vmtest/vmrun.c mirrors this call. */
  if (config->heap_limit / 2U < JS_GetGCThreshold(guest->runtime)) {
    JS_SetGCThreshold(guest->runtime, config->heap_limit / 2U);
  }
  JS_SetMaxStackSize(guest->runtime, config->stack_limit);
  JS_SetRuntimeInfo(guest->runtime, "PocketJS ESP-IDF guest");
  JS_SetInterruptHandler(guest->runtime, guest_interrupt, guest);
  JS_SetHostPromiseRejectionTracker(guest->runtime, promise_rejection, guest);
  js_std_init_handlers(guest->runtime);
  guest->context = JS_NewContext(guest->runtime);
  if (guest->context == NULL) {
    pocketjs_guest_destroy(guest);
    return ESP_ERR_NO_MEM;
  }
  js_std_add_helpers(guest->context, 0, NULL);
  *out_guest = guest;
  return ESP_OK;
}

esp_err_t
pocketjs_guest_quickjs_install(pocketjs_guest_t *guest,
                               pocketjs_guest_quickjs_install_fn install,
                               void *user_data) {
  if (guest == NULL || guest->context == NULL || install == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  return install(guest->context, user_data);
}

JSContext *pocketjs_guest_quickjs_context(pocketjs_guest_t *guest) {
  return guest != NULL ? guest->context : NULL;
}

esp_err_t
pocketjs_guest_quickjs_install_once(pocketjs_guest_t *guest, const char *name,
                                    pocketjs_guest_quickjs_install_fn install,
                                    void *user_data) {
  if (!guest || !name || !*name || !install)
    return ESP_ERR_INVALID_ARG;
  for (surface_t *entry = guest->surfaces; entry; entry = entry->next)
    if (!strcmp(entry->name, name))
      return ESP_ERR_INVALID_STATE;
  surface_t *entry = calloc(1, sizeof(*entry));
  if (!entry)
    return ESP_ERR_NO_MEM;
  entry->name = strdup(name);
  if (!entry->name) {
    free(entry);
    return ESP_ERR_NO_MEM;
  }
  entry->next = guest->surfaces;
  guest->surfaces = entry;
  esp_err_t result = pocketjs_guest_quickjs_install(guest, install, user_data);
  if (result != ESP_OK) {
    if (JS_HasException(guest->context))
      JS_FreeValue(guest->context, JS_GetException(guest->context));
    surface_t **slot = &guest->surfaces;
    while (*slot && *slot != entry)
      slot = &(*slot)->next;
    if (*slot)
      *slot = entry->next;
    free(entry->name);
    free(entry);
  }
  return result;
}

esp_err_t pocketjs_guest_eval(pocketjs_guest_t *guest, const char *source,
                              size_t source_size, const char *label) {
  if (guest == NULL || guest->context == NULL || source == NULL ||
      source_size == 0U) {
    return ESP_ERR_INVALID_ARG;
  }
  JSValue result =
      JS_Eval(guest->context, source, source_size,
              label != NULL ? label : "<pocket-app>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(result)) {
    js_std_dump_error(guest->context);
    JS_FreeValue(guest->context, result);
    return ESP_FAIL;
  }
  JS_FreeValue(guest->context, result);
  JS_FreeValue(guest->context, guest->frame);
  JSValue global = JS_GetGlobalObject(guest->context);
  guest->frame = JS_GetPropertyStr(guest->context, global, "frame");
  JS_FreeValue(guest->context, global);
  if (!JS_IsFunction(guest->context, guest->frame)) {
    return ESP_ERR_NOT_FOUND;
  }
  return drain_jobs(guest);
}

static JSValue make_u32_array(JSContext *context, const uint32_t *values,
                              size_t count) {
  JSValue array = JS_NewArray(context);
  if (JS_IsException(array)) {
    return array;
  }
  for (size_t index = 0; index < count; ++index) {
    if (JS_SetPropertyUint32(context, array, (uint32_t)index,
                             JS_NewUint32(context, values[index])) < 0) {
      JS_FreeValue(context, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

static JSValue make_i32_array(JSContext *context, const int32_t *values,
                              size_t count) {
  JSValue array = JS_NewArray(context);
  if (JS_IsException(array)) {
    return array;
  }
  for (size_t index = 0; index < count; ++index) {
    if (JS_SetPropertyUint32(context, array, (uint32_t)index,
                             JS_NewInt32(context, values[index])) < 0) {
      JS_FreeValue(context, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

static esp_err_t guest_frame_impl(pocketjs_guest_t *guest,
                               const pocketjs_guest_frame_t *frame) {
  if (guest == NULL || guest->context == NULL || frame == NULL ||
      frame->struct_size < sizeof(*frame) ||
      frame->touch_count > POCKETJS_GUEST_MAX_TOUCHES ||
      (frame->touch_count != 0U && frame->touches == NULL)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!JS_IsFunction(guest->context, guest->frame)) {
    return ESP_ERR_INVALID_STATE;
  }
  if (pocketjs_guest_suspended(guest))
    return ESP_ERR_INVALID_STATE;
  JSValue arguments[4] = {
      JS_NewUint32(guest->context, frame->buttons),
      JS_NewUint32(guest->context, frame->analog),
      JS_UNDEFINED,
      JS_UNDEFINED,
  };
  int argument_count = 2;
  if (frame->touch_count != 0U) {
    arguments[2] =
        make_u32_array(guest->context, frame->touches, frame->touch_count);
    if (JS_IsException(arguments[2])) {
      js_std_dump_error(guest->context);
      JS_FreeValue(guest->context, arguments[1]);
      JS_FreeValue(guest->context, arguments[0]);
      guest->frame_errors++;
      return ESP_ERR_NO_MEM;
    }
    argument_count = 3;
    if (frame->touch_hits != NULL) {
      arguments[3] =
          make_i32_array(guest->context, frame->touch_hits, frame->touch_count);
      if (JS_IsException(arguments[3])) {
        js_std_dump_error(guest->context);
        JS_FreeValue(guest->context, arguments[2]);
        JS_FreeValue(guest->context, arguments[1]);
        JS_FreeValue(guest->context, arguments[0]);
        guest->frame_errors++;
        return ESP_ERR_NO_MEM;
      }
      argument_count = 4;
    }
  }
#ifdef CONFIG_POCKET_VM_PROBE
  /* VM_PROBE: the clock is read around JS_Call and around drain_jobs only --
   * the argument marshalling above and the free below stay inside neither, so
   * the two numbers add up to less than the turn rather than more. */
  const int64_t vmprobe_call_begin = esp_timer_get_time();
#endif
#ifdef CONFIG_POCKET_VM_YIELD
  const int64_t frame_begin = esp_timer_get_time();
#endif
  JSValue result = JS_VMCall(guest->context, guest->frame, JS_UNDEFINED,
                           argument_count, arguments);
#ifdef CONFIG_POCKET_VM_YIELD
  guest->frame_us = esp_timer_get_time() - frame_begin;
#endif
#ifdef CONFIG_POCKET_VM_PROBE
  vmprobe_call_us += (uint32_t)(esp_timer_get_time() - vmprobe_call_begin);
#endif
  for (int index = argument_count - 1; index >= 0; --index) {
    JS_FreeValue(guest->context, arguments[index]);
  }
  guest->frames++;
#ifdef CONFIG_POCKET_VM_YIELD
  if (JS_VMSuspended(guest->runtime)) {
    guest->suspended = true;
    guest->origin = JS_VM_ORIGIN_HOST;
    guest->jobs_pending = JS_IsJobPending(guest->runtime);
    guest->yields++;
    JS_FreeValue(guest->context, result);
    return ESP_OK;
  }
#ifdef CONFIG_POCKET_VM_SELFTEST
  if (guest->trace_frame) {
    guest->trace_frame = false;
    ESP_LOGI(TAG, "VM_FRAME_COMPLETE execution_us=%lld exception=%d",
             (long long)guest->frame_us, JS_IsException(result));
  }
#endif
  guest->frame_us = 0;
#endif
  if (JS_IsException(result)) {
    js_std_dump_error(guest->context);
    JS_FreeValue(guest->context, result);
    guest->frame_errors++;
    return ESP_FAIL;
  }
  JS_FreeValue(guest->context, result);
#ifdef CONFIG_POCKET_VM_PROBE
  const int64_t vmprobe_drain_begin = esp_timer_get_time();
#endif
  const esp_err_t jobs = drain_jobs(guest);
#ifdef CONFIG_POCKET_VM_PROBE
  vmprobe_drain_us += (uint32_t)(esp_timer_get_time() - vmprobe_drain_begin);
#endif
  if (jobs != ESP_OK) {
    guest->frame_errors++;
  }
  return jobs;
}

esp_err_t pocketjs_guest_frame(pocketjs_guest_t *guest,
                               const pocketjs_guest_frame_t *frame) {
  esp_err_t err = guest_run_begin(guest);
  if (err == ESP_OK) err = guest_frame_impl(guest, frame);
  guest_run_end(guest);
  return err;
}

/* L1 sec.1.3: one budget per turn, armed by the host, shared by the frame()'s
 * drain and by every continuation drain of the same turn. Copied rather than
 * aliased -- the host's struct is a stack local of app_tick(). */
void pocketjs_guest_budget(pocketjs_guest_t *guest, const vm_budget_t *budget) {
  if (guest == NULL)
    return;
  if (budget != NULL)
    guest->budget = *budget;
  else
    vm_budget_begin(&guest->budget, 0);
}

bool pocketjs_guest_jobs_pending(const pocketjs_guest_t *guest) {
  return guest != NULL && guest->jobs_pending;
}

bool pocketjs_guest_suspended(const pocketjs_guest_t *guest) {
#ifdef CONFIG_POCKET_VM_YIELD
  return guest != NULL && guest->suspended;
#else
  (void)guest;
  return false;
#endif
}

bool pocketjs_guest_work_pending(const pocketjs_guest_t *guest) {
  return pocketjs_guest_jobs_pending(guest) || pocketjs_guest_suspended(guest);
}

int64_t pocketjs_guest_frame_total(const pocketjs_guest_t *guest) {
#ifdef CONFIG_POCKET_VM_YIELD
  return guest != NULL ? guest->frame_us : 0;
#else
  (void)guest;
  return 0;
#endif
}

#if defined(CONFIG_POCKET_VM_SELFTEST) && defined(CONFIG_POCKET_VM_YIELD)
void pocketjs_guest_trace_frame(pocketjs_guest_t *guest) {
  if (guest != NULL) guest->trace_frame = true;
}
#endif

#ifdef CONFIG_POCKET_VM_RELOC
void pocketjs_guest_reloc_arm(pocketjs_guest_t *guest, bool on) {
  if (guest == NULL) return;
  guest->reloc_armed = on;
  if (!on) return;
  guest->reloc_moves = guest->reloc_refused = guest->reloc_empty = 0;
  guest->reloc_frames = guest->reloc_var_refs = 0;
  guest->reloc_max_us = 0;
  guest->reloc_total_us = guest->reloc_bytes = 0;
  guest->reloc_gap_max = guest->reloc_gap_segments = 0;
  guest->reloc_largest_first = guest->reloc_largest_last = 0;
  guest->reloc_largest_min = 0;
}

void pocketjs_guest_reloc_report(const pocketjs_guest_t *guest) {
  if (guest == NULL || !guest->reloc_armed) return;
  /* One uppercase marker, like every other contract this firmware has with
   * tools/ (CLAUDE.md). tools/vm_reloc_device.py parses this line.
   *
   * moves=0 with refused>0 is a real outcome, not a failure: the app never
   * parked anywhere a move was legal. The script has to be able to tell that
   * from "it moved and nothing broke", which is why both are printed. */
  ESP_LOGI(TAG,
           "VM_RELOC moves=%lu refused=%lu empty=%lu frames=%lu var_refs=%lu "
           "bytes=%llu max_us=%lu total_us=%llu "
           "largest_first=%lu largest_last=%lu largest_min=%lu "
           "gap_max=%lu gap_segments=%lu",
           (unsigned long)guest->reloc_moves,
           (unsigned long)guest->reloc_refused,
           (unsigned long)guest->reloc_empty,
           (unsigned long)guest->reloc_frames,
           (unsigned long)guest->reloc_var_refs,
           (unsigned long long)guest->reloc_bytes,
           (unsigned long)guest->reloc_max_us,
           (unsigned long long)guest->reloc_total_us,
           (unsigned long)guest->reloc_largest_first,
           (unsigned long)guest->reloc_largest_last,
           (unsigned long)guest->reloc_largest_min,
           (unsigned long)guest->reloc_gap_max,
           (unsigned long)guest->reloc_gap_segments);
}
#endif

void pocketjs_guest_prepare_stop(pocketjs_guest_t *guest) {
  if (!guest || !guest->runtime)
    return;
  guest_run_end(guest);
#ifdef CONFIG_POCKET_VM_YIELD
  if (JS_VMSuspended(guest->runtime)) {
    JS_VMTerminate(guest->runtime);
    JSValue result = JS_VMResume(guest->context);
    JS_FreeValue(guest->context, result);
    if (JS_HasException(guest->context))
      JS_FreeValue(guest->context, JS_GetException(guest->context));
    /* An async owner's C entry can reject a resume for stack exhaustion.
     * Teardown must still close the chain before invoking any stop hook. */
    if (JS_VMSuspended(guest->runtime))
      JS_VMDiscard(guest->runtime);
  }
  guest->suspended = JS_VMSuspended(guest->runtime);
  guest->origin = JS_VM_ORIGIN_NONE;
  guest->frame_us = 0;
#endif
  guest->jobs_pending = JS_IsJobPending(guest->runtime);
}

void pocketjs_guest_drain_total(const pocketjs_guest_t *guest, int64_t *us,
                                uint64_t *jobs) {
  if (us != NULL)
    *us = guest != NULL ? guest->drain_us : 0;
  if (jobs != NULL)
    *jobs = guest != NULL ? guest->drain_jobs : 0;
}

/* The continuation drain of sec.2.1. It is the SAME drain as the one the
 * budget cut: no host code has called into JS between the two, so the job
 * order the guest observes is the order the pre-L1 single drain produced. */
static esp_err_t guest_continue_impl(pocketjs_guest_t *guest) {
  if (guest == NULL || guest->context == NULL)
    return ESP_ERR_INVALID_STATE;
  if (!pocketjs_guest_work_pending(guest))
    return ESP_OK;
  guest->continuations++;
#ifdef CONFIG_POCKET_VM_YIELD
  if (guest->suspended) {
    const bool frame = guest->origin == JS_VM_ORIGIN_HOST;
    const bool held = guest->origin == JS_VM_ORIGIN_JOB_HELD;
#ifdef CONFIG_POCKET_VM_RELOC
    /* L3a's one caller on the device. Here and nowhere else: this is the
     * only place the firmware resumes a chain it means to keep running, so
     * it is the only place where the VM is parked AND has a future. The
     * resume in pocketjs_guest_prepare_stop() is parked too, but it is
     * terminating the chain -- moving it would copy bytes on their way to
     * being freed.
     *
     * Charged separately from the resume it precedes, so the frame and
     * drain totals the runaway guard reads keep meaning "time the guest
     * spent running" rather than quietly including relocation. */
    if (guest->reloc_armed) {
      JSVMRelocStats rs;
      const uint32_t before = (uint32_t)heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const int64_t reloc_began = esp_timer_get_time();
      const int moved = JS_VMStackRelocate(guest->runtime, &rs);
      const uint32_t reloc_us = (uint32_t)(esp_timer_get_time() - reloc_began);
      if (moved == 0 && rs.segments != 0U) {
        const uint32_t after = (uint32_t)heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (guest->reloc_moves == 0U) guest->reloc_largest_first = before;
        guest->reloc_largest_last = after;
        if (guest->reloc_largest_min == 0U || before < guest->reloc_largest_min)
          guest->reloc_largest_min = before;
        if (after < guest->reloc_largest_min) guest->reloc_largest_min = after;
        guest->reloc_moves++;
        guest->reloc_frames += rs.frames;
        guest->reloc_var_refs += rs.var_refs;
        guest->reloc_bytes += rs.bytes;
        if (rs.span > rs.resident &&
            (uint32_t)(rs.span - rs.resident) > guest->reloc_gap_max) {
          guest->reloc_gap_max = (uint32_t)(rs.span - rs.resident);
          guest->reloc_gap_segments = rs.segments;
        }
        guest->reloc_total_us += reloc_us;
        if (reloc_us > guest->reloc_max_us) guest->reloc_max_us = reloc_us;
      } else if (moved != 0) {
        guest->reloc_refused++;
      } else {
        guest->reloc_empty++;
      }
    }
#endif
    const int64_t began = esp_timer_get_time();
    JSValue result = JS_VMResume(guest->context);
    const int64_t elapsed = esp_timer_get_time() - began;
    if (frame)
      guest->frame_us += elapsed;
    else if (guest->budget.limit_ticks != 0)
      guest->drain_us += elapsed;
#ifdef CONFIG_POCKET_VM_PROBE
    if (frame) vmprobe_call_us += (uint32_t)elapsed;
    else vmprobe_drain_us += (uint32_t)elapsed;
#endif
    guest->suspended = JS_VMSuspended(guest->runtime);
    guest->jobs_pending = JS_IsJobPending(guest->runtime) ||
                          (guest->suspended && held);
    if (guest->suspended) {
      guest->yields++;
      JS_FreeValue(guest->context, result);
      return ESP_OK;
    }
    guest->origin = JS_VM_ORIGIN_NONE;
#ifdef CONFIG_POCKET_VM_SELFTEST
    if (frame && guest->trace_frame) {
      guest->trace_frame = false;
      ESP_LOGI(TAG, "VM_FRAME_COMPLETE execution_us=%lld exception=%d",
               (long long)guest->frame_us, JS_IsException(result));
    }
#endif
    guest->frame_us = 0;
    if (JS_IsException(result)) {
      js_std_dump_error(guest->context);
      JS_FreeValue(guest->context, result);
      guest->frame_errors++;
      return ESP_FAIL;
    }
    JS_FreeValue(guest->context, result);
    if (held) {
      guest->jobs++;
      guest->drain_jobs++;
    }
  }
#endif
  return drain_jobs(guest);
}

esp_err_t pocketjs_guest_continue(pocketjs_guest_t *guest) {
  esp_err_t err = guest_run_begin(guest);
  if (err == ESP_OK) err = guest_continue_impl(guest);
  guest_run_end(guest);
  return err;
}

void pocketjs_guest_yield_enabled(pocketjs_guest_t *guest, bool enabled) {
#ifdef CONFIG_POCKET_VM_YIELD
  if (!guest) return;
  guest->yield_disabled = !enabled;
  if (!enabled) guest_run_end(guest);
#else
  (void)guest; (void)enabled;
#endif
}

void pocketjs_guest_set_watchdog(pocketjs_guest_t *guest, int (*fn)(void *),
                                 void *opaque) {
  if (guest == NULL)
    return;
  guest->watchdog = fn;
  guest->watchdog_opaque = opaque;
}

void pocketjs_guest_interrupt(pocketjs_guest_t *guest) {
  if (guest != NULL) {
    (void)atomic_fetch_add_explicit(&guest->interrupt_epoch, 1U,
                                    memory_order_relaxed);
  }
}

esp_err_t pocketjs_guest_stats(pocketjs_guest_t *guest,
                               pocketjs_guest_stats_t *out_stats) {
  if (guest == NULL || out_stats == NULL ||
      out_stats->struct_size < sizeof(*out_stats)) {
    return ESP_ERR_INVALID_ARG;
  }
  JSMemoryUsage usage = {0};
  JS_ComputeMemoryUsage(guest->runtime, &usage);
  const size_t output_size = out_stats->struct_size;
  *out_stats = (pocketjs_guest_stats_t){
      .struct_size = output_size,
      .frames = guest->frames,
      .frame_errors = guest->frame_errors,
      .jobs = guest->jobs,
      .heap_used = usage.malloc_size,
      .heap_limit = guest->heap_limit,
      .yields = guest->yields,
      .continuations = guest->continuations,
      .jobs_pending = guest->jobs_pending,
      /* Live, not latched: app_report() runs before destroy, so the only
       * honest answer there is "is anything queued right now". */
      .jobs_dropped = guest->jobs_dropped || guest->jobs_pending || JS_IsJobPending(guest->runtime),
  };
  return ESP_OK;
}

/* Read-and-clear, same shape as pocketjs_guest_vmprobe_take(): the host
 * (app_session.c) calls this after every turn so a `caught null` from the
 * SAME turn can be told apart from a script's own `throw null`. See
 * JS_TakeOOMCanary for why the count has to come from inside QuickJS rather
 * than from guest_malloc's own NULL returns -- the malloc_limit accounting
 * check rejects most device OOMs before guest_malloc is ever called. */
void pocketjs_guest_take_oom(pocketjs_guest_t *guest, uint32_t *count,
                             size_t *first_req, size_t *first_used) {
  JSOOMCanary canary = {0};
  if (guest != NULL) {
    JS_TakeOOMCanary(guest->runtime, &canary);
  }
  if (count != NULL) *count = canary.count;
  if (first_req != NULL) *first_req = canary.first_req;
  if (first_used != NULL) *first_used = canary.first_used;
}

void pocketjs_guest_destroy(pocketjs_guest_t *guest) {
  if (guest == NULL) {
    return;
  }
  guest_run_end(guest);
#ifdef CONFIG_POCKET_VM_YIELD
  if (guest->yield_timer) (void)esp_timer_delete(guest->yield_timer);
#endif
  if (guest->runtime != NULL) {
    /* sec.3.2: whatever is still queued is discarded UNRUN, which is what
     * JS_FreeRuntime does anyway (ledger 03 fact 9) and what pocket_api_reset()
     * already chose for in-flight promises. Recorded as one bit because
     * counting would need a VM hook. */
    guest->jobs_dropped = guest->jobs_pending || JS_IsJobPending(guest->runtime);
#ifdef CONFIG_POCKET_VM_PROBE
    /* D42 sizing: the frame segments' peak for this session, the same line
     * vmrun --stats prints on the host, so the standard segment size can be
     * chosen from device frame sizes (8 B JSValue, 48 B frame header) rather
     * than host ones. Printed before teardown, while the counters still
     * describe the app rather than JS_FreeRuntime's own pops. */
    vmtest_vmstack_report(guest->runtime, stdout);
#endif
    js_std_free_handlers(guest->runtime);
  }
  if (guest->context != NULL) {
    while (guest->rejections) {
      rejection_t *entry = guest->rejections;
      guest->rejections = entry->next;
      JS_FreeValue(guest->context, entry->promise);
      JS_FreeValue(guest->context, entry->reason);
      free(entry);
    }
    JS_FreeValue(guest->context, guest->frame);
    JS_FreeContext(guest->context);
  }
  if (guest->runtime != NULL) {
    JS_FreeRuntime(guest->runtime);
  }
  while (guest->surfaces) {
    surface_t *entry = guest->surfaces;
    guest->surfaces = entry->next;
    free(entry->name);
    free(entry);
  }
  free(guest);
}
