#include "ayatsuri_cert_heap_trace.h"

#if CONFIG_AYATSURI_CERT_HEAP_TRACE
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/cache_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "multi_heap.h"

#if !CONFIG_IDF_TARGET_ESP32S3 || CONFIG_HEAP_TLSF_USE_ROM_IMPL || !CONFIG_HEAP_POISONING_DISABLED || CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH
#error "Certificate allocation trace requires the ESP32-S3 unpoisoned IDF 6.0.1 IRAM allocator"
#endif

/* IDF 6.0.1 multi_heap_register_impl returns the region start as its handle.
 * Check the registered region before the first explicit use of this handle. */
#define CERT_REGION ((multi_heap_handle_t)(uintptr_t)UINT32_C(0x3fcf0000))
enum { PC_COUNT = 20, OTHER_LARGE_BYTES = 512 };
void multi_heap_internal_lock(multi_heap_handle_t heap);
void multi_heap_internal_unlock(multi_heap_handle_t heap);
extern __typeof__(multi_heap_malloc) __real_multi_heap_malloc;
extern __typeof__(multi_heap_realloc) __real_multi_heap_realloc;
extern __typeof__(multi_heap_aligned_alloc_offs) __real_multi_heap_aligned_alloc_offs;

enum alloc_op { OP_MALLOC, OP_REALLOC, OP_ALIGNED };
typedef struct {
    unsigned req, delta, free_before, free_after, sdk_before, sdk_after;
    enum alloc_op op;
    bool tls, isr;
    unsigned core, depth;
    TaskHandle_t task;
    char name[configMAX_TASK_NAME_LEN];
    uint32_t pcs[PC_COUNT];
} alloc_event;

static bool region_found;
static TaskHandle_t owner;
static struct {
    unsigned events, skipped, free_in, free_out, sdk_in, sdk_out;
    alloc_event floor, largest_step, other_large;
} trace;

static bool find_region(walker_heap_into_t h, walker_block_info_t b, void *arg)
{
    (void)b;
    (void)arg;
    if ((uintptr_t)h.start == (uintptr_t)CERT_REGION) region_found = true;
    return false; /* Only the first block of each region is needed. */
}

void ay_cert_heap_trace_begin(void)
{
    if (!region_found) heap_caps_walk(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, find_region, NULL);
    if (!region_found) return;
    multi_heap_internal_lock(CERT_REGION);
    memset(&trace, 0, sizeof(trace));
    trace.free_in = multi_heap_free_size(CERT_REGION);
    trace.sdk_in = multi_heap_minimum_free_size(CERT_REGION);
    owner = xTaskGetCurrentTaskHandle();
    multi_heap_internal_unlock(CERT_REGION);
}

void ay_cert_heap_trace_end(void)
{
    if (!region_found) return;
    multi_heap_internal_lock(CERT_REGION);
    trace.free_out = multi_heap_free_size(CERT_REGION);
    trace.sdk_out = multi_heap_minimum_free_size(CERT_REGION);
    owner = NULL;
    multi_heap_internal_unlock(CERT_REGION);
}

/* All current-free and SDK-minimum reads for one allocation share the real
 * allocator's recursive lock with allocations AND frees on either core. Avoid
 * SDK size accessors in cache-off context; report any resulting coverage gap. */
typedef struct { unsigned free_before, sdk_before; bool sample; } before_alloc;

static inline __attribute__((always_inline)) before_alloc before_locked(void)
{
    before_alloc b = {0};
    if (owner) {
        if (spi_flash_cache_enabled()) {
            b.free_before = multi_heap_free_size(CERT_REGION);
            b.sdk_before = multi_heap_minimum_free_size(CERT_REGION);
            b.sample = true;
        } else {
            ++trace.skipped;
        }
    }
    return b;
}

/* Only reached from a cache-enabled task. Store instruction PCs ready for
 * addr2line, and stop if a frame fails IDF's stack/PC sanity checks. */
static void __attribute__((noinline)) capture_callers(alloc_event *event)
{
    const char *name = pcTaskGetName(event->task);
    for (size_t i = 0; i + 1 < sizeof(event->name) && name[i]; ++i) {
        event->name[i] = name[i];
        event->name[i + 1] = '\0';
    }
    esp_backtrace_frame_t frame = {0};
    esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);
    while (event->depth < PC_COUNT && frame.next_pc && esp_stack_ptr_is_sane(frame.sp) &&
           esp_backtrace_get_next_frame(&frame)) {
        event->pcs[event->depth++] = esp_cpu_process_stack_pc(frame.pc);
    }
}

static void __attribute__((noinline)) observe_alloc(before_alloc b, size_t req, enum alloc_op op)
{
    unsigned after = multi_heap_free_size(CERT_REGION);
    if (after >= b.free_before) return;
    unsigned delta = b.free_before - after;
    bool new_low = after < (trace.events ? trace.floor.free_after : trace.free_in);
    bool isr = xPortInIsrContext();
    TaskHandle_t task = isr ? NULL : xTaskGetCurrentTaskHandle();
    bool tls = task == owner;
    bool large_other = !tls && delta >= OTHER_LARGE_BYTES && delta > trace.other_large.delta;
    if (!new_low && !large_other) return;

    alloc_event *e = new_low ? &trace.floor : &trace.other_large;
    e->req = req;
    e->delta = delta;
    e->free_before = b.free_before;
    e->free_after = after;
    e->sdk_before = b.sdk_before;
    e->sdk_after = multi_heap_minimum_free_size(CERT_REGION);
    e->op = op;
    e->isr = isr;
    e->tls = tls;
    e->task = task;
    e->core = xPortGetCoreID();
    e->name[0] = '\0';
    e->depth = 0;
    if (!isr) capture_callers(e);
    if (new_low) {
        ++trace.events;
        if (delta > trace.largest_step.delta) trace.largest_step = *e;
        if (large_other) trace.other_large = *e;
    }
}

/* Serialize target-region allocations across the stage boundary too. Acquiring
 * the same portMUX once outside IDF prevents a task that entered just before
 * begin() from lowering the stage floor between a bypass check and __real. */
void *IRAM_ATTR __wrap_multi_heap_malloc(multi_heap_handle_t heap, size_t size)
{
    if (heap != CERT_REGION) return __real_multi_heap_malloc(heap, size);
    multi_heap_internal_lock(heap);
    before_alloc b = before_locked();
    void *p = __real_multi_heap_malloc(heap, size);
    if (b.sample && p) observe_alloc(b, size, OP_MALLOC);
    multi_heap_internal_unlock(heap);
    return p;
}

void *IRAM_ATTR __wrap_multi_heap_realloc(multi_heap_handle_t heap, void *ptr, size_t size)
{
    if (heap != CERT_REGION) return __real_multi_heap_realloc(heap, ptr, size);
    multi_heap_internal_lock(heap);
    before_alloc b = before_locked();
    void *p = __real_multi_heap_realloc(heap, ptr, size);
    if (b.sample && p) observe_alloc(b, size, OP_REALLOC);
    multi_heap_internal_unlock(heap);
    return p;
}

void *IRAM_ATTR __wrap_multi_heap_aligned_alloc_offs(multi_heap_handle_t heap, size_t size,
                                                     size_t alignment, size_t offset)
{
    if (heap != CERT_REGION) return __real_multi_heap_aligned_alloc_offs(heap, size, alignment, offset);
    multi_heap_internal_lock(heap);
    before_alloc b = before_locked();
    void *p = __real_multi_heap_aligned_alloc_offs(heap, size, alignment, offset);
    if (b.sample && p) observe_alloc(b, size, OP_ALIGNED);
    multi_heap_internal_unlock(heap);
    return p;
}

static void report_event(const char *phase, const char *kind, const alloc_event *e)
{
    if (!e->delta) return;
    char pcs[PC_COUNT * 9 + 1] = "";
    for (unsigned i = 0; i < e->depth; ++i) {
        snprintf(pcs + 9*i, sizeof(pcs) - 9*i, "%08" PRIx32 "%s", e->pcs[i],
                 i + 1 < e->depth ? "," : "");
    }
    static const char *const ops[] = {"malloc", "realloc", "aligned"};
    ESP_LOGI("CERT_HEAP", "phase=%s kind=%s op=%s req=%u delta=%u free_before=%u free_after=%u"
             " sdk_before=%u sdk_after=%u tls=%u isr=%u core=%u task=%p name=%s pcs=%s",
             phase, kind, ops[e->op], e->req, e->delta, e->free_before, e->free_after,
             e->sdk_before, e->sdk_after, (unsigned)e->tls, (unsigned)e->isr, e->core,
             (void *)e->task, e->name[0] ? e->name : "-", e->depth ? pcs : "-");
}

void ay_cert_heap_trace_report(const char *phase)
{
    ESP_LOGI("CERT_HEAP", "phase=%s region=3fcf0000 found=%u events=%u skipped=%u free_in=%u free_out=%u"
             " sdk_in=%u sdk_out=%u", phase, (unsigned)region_found, trace.events, trace.skipped,
             trace.free_in, trace.free_out, trace.sdk_in, trace.sdk_out);
    report_event(phase, "floor", &trace.floor);
    report_event(phase, "largest_step", &trace.largest_step);
    report_event(phase, "other_large", &trace.other_large);
}
#endif
