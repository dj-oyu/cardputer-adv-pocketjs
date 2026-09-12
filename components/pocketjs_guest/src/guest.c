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

static const char *TAG = "pocketjs_guest";

#ifdef CONFIG_POCKET_VM_PROBE
/* VM_PROBE (docs/quickjs-freertos-vm-spec.md sec.5): frame() time and drain
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

/* vm-l1-tuning (docs/vm-l1-tuning.md): jobs actually returned by ONE
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

typedef union {
  size_t size;
  max_align_t alignment;
} allocation_header_t;

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
  /* L1 (docs/vm-L1-design.md). The budget is armed by the host once per turn
   * and read by every drain in that turn, so the frame()'s drain and the next
   * turn's continuation drain share one deadline measured from turn start. */
  vm_budget_t budget;
  bool jobs_pending;
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
  if (status == VM_DRAIN_THREW) {
    if (context != NULL) {
      js_std_dump_error(context);
    }
    /* The session ends on this path (main.c end_run), and the queue that is
     * left is the next drain's, not this logical one's: start the count over
     * rather than charge the survivor for it. */
    guest->drain_us = 0;
    guest->drain_jobs = 0;
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

esp_err_t pocketjs_guest_frame(pocketjs_guest_t *guest,
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
  JSValue result = JS_Call(guest->context, guest->frame, JS_UNDEFINED,
                           argument_count, arguments);
#ifdef CONFIG_POCKET_VM_PROBE
  vmprobe_call_us += (uint32_t)(esp_timer_get_time() - vmprobe_call_begin);
#endif
  for (int index = argument_count - 1; index >= 0; --index) {
    JS_FreeValue(guest->context, arguments[index]);
  }
  guest->frames++;
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
esp_err_t pocketjs_guest_continue(pocketjs_guest_t *guest) {
  if (guest == NULL || guest->context == NULL)
    return ESP_ERR_INVALID_STATE;
  if (!guest->jobs_pending)
    return ESP_OK;
  guest->continuations++;
  return drain_jobs(guest);
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
      .jobs_dropped = guest->jobs_dropped || JS_IsJobPending(guest->runtime),
  };
  return ESP_OK;
}

void pocketjs_guest_destroy(pocketjs_guest_t *guest) {
  if (guest == NULL) {
    return;
  }
  if (guest->runtime != NULL) {
    /* sec.3.2: whatever is still queued is discarded UNRUN, which is what
     * JS_FreeRuntime does anyway (ledger 03 fact 9) and what pocket_api_reset()
     * already chose for in-flight promises. Recorded as one bit because
     * counting would need a VM hook. */
    guest->jobs_dropped = JS_IsJobPending(guest->runtime);
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
