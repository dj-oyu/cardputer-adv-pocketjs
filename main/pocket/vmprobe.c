// L0 measurement probes. See vmprobe.h and
// docs/quickjs-freertos-vm-spec.md sec.5. This whole file is empty (zero
// symbols) unless CONFIG_POCKET_VM_PROBE is on, and main/CMakeLists.txt only
// adds it to SRCS in that case -- a normal build never compiles it.
#include "vmprobe.h"

#ifdef CONFIG_POCKET_VM_PROBE

#include "quickjs-vmprobe.h"
#include "quickjs.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "vmprobe";

// A window's worth of one metric: online min/max/sum/count, plus a small
// capped ring of raw samples so a flush can report an approximate median
// without a heap allocation. At a 33 ms frame pace a 1 s window sees ~30
// frames, so the per-frame metrics (frame time, jobs, queue peak) fit
// entirely; completion latency can fire more than once a frame and is the
// one metric that can wrap the ring -- when it does, the median is over the
// most recent CAP samples rather than the whole window. That is what
// "median-ish" in the spec allows, not a silent approximation.
#define VMPROBE_SAMPLE_CAP 48
typedef struct {
    uint32_t count;
    double   sum;
    double   min, max;
    double   samples[VMPROBE_SAMPLE_CAP];
    uint32_t sample_n;    // valid entries in `samples`, saturates at CAP
    uint32_t sample_at;   // next write index (ring)
} vmprobe_window_t;

static void window_add(vmprobe_window_t *w, double v) {
    if (w->count == 0) { w->min = w->max = v; }
    else { if (v < w->min) w->min = v; if (v > w->max) w->max = v; }
    w->sum += v; w->count++;
    w->samples[w->sample_at] = v;
    w->sample_at = (w->sample_at + 1) % VMPROBE_SAMPLE_CAP;
    if (w->sample_n < VMPROBE_SAMPLE_CAP) w->sample_n++;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// Sorts a COPY of the ring (never the live one) and returns the middle
// value. Both this and window_add() only ever run on ui_task, so there is
// no concurrent writer to race -- no lock needed.
static double window_median(const vmprobe_window_t *w) {
    if (!w->sample_n) return 0.0;
    double copy[VMPROBE_SAMPLE_CAP];
    memcpy(copy, w->samples, w->sample_n * sizeof(double));
    qsort(copy, w->sample_n, sizeof(double), cmp_double);
    return copy[w->sample_n / 2];
}

static void window_reset(vmprobe_window_t *w) { memset(w, 0, sizeof(*w)); }

static vmprobe_window_t frame_ms_w, jobs_w, queue_peak_w, latency_ms_w;
static uint64_t jobs_executed_base;
static int64_t  window_start_us;
static unsigned window_ticks;
// Sampled at a low rate (sec.5 asks for this explicitly): JS_ComputeMemoryUsage
// walks live QuickJS structures, so calling it every frame would put its own
// cost inside the very frame time being measured. heap/stack reads are cheap
// enough to not need the same care, but are sampled alongside it for one
// consistent "last sample" story per window.
static size_t   js_used_last, js_limit_last;
static unsigned heap_free_last, heap_largest_last;
static UBaseType_t stack_hw_last;

#define VMPROBE_WINDOW_US 1000000
#define VMPROBE_SAMPLE_EVERY_N_FRAMES 8   // ~4 Hz at a 33 ms frame pace

static void flush_window(void) {
    ESP_LOGI(TAG,
        "VMPROBE WINDOW ms=%u frame_n=%u frame_min=%.2f frame_med=%.2f frame_max=%.2f "
        "jobs_n=%u jobs_min=%u jobs_med=%.1f jobs_max=%u qpeak_max=%u "
        "lat_n=%u lat_min=%.2f lat_med=%.2f lat_max=%.2f "
        "heap_free=%u heap_largest=%u js_used=%u js_limit=%u stack_hw=%u",
        (unsigned)((esp_timer_get_time() - window_start_us) / 1000),
        frame_ms_w.count, frame_ms_w.count ? frame_ms_w.min : 0.0,
        window_median(&frame_ms_w), frame_ms_w.count ? frame_ms_w.max : 0.0,
        jobs_w.count, jobs_w.count ? (unsigned)jobs_w.min : 0,
        window_median(&jobs_w), jobs_w.count ? (unsigned)jobs_w.max : 0,
        queue_peak_w.count ? (unsigned)queue_peak_w.max : 0,
        latency_ms_w.count, latency_ms_w.count ? latency_ms_w.min : 0.0,
        window_median(&latency_ms_w), latency_ms_w.count ? latency_ms_w.max : 0.0,
        heap_free_last, heap_largest_last,
        (unsigned)js_used_last, (unsigned)js_limit_last, (unsigned)stack_hw_last);
    window_reset(&frame_ms_w); window_reset(&jobs_w);
    window_reset(&queue_peak_w); window_reset(&latency_ms_w);
    window_start_us = esp_timer_get_time();
    window_ticks = 0;
}

void vmprobe_static_report(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG,
        "VMPROBE STATIC engine=quickjs-ng-0.14.0+immutable-buffer-patch compiler=%s opt=%s "
        "sizeof_jsvalue=%u sizeof_stackframe=%u sizeof_varref=%u fw=%s",
        __VERSION__,
#if defined(__OPTIMIZE_SIZE__)
        "Os",
#elif defined(__OPTIMIZE__)
        "O-not-size",
#else
        "O0",
#endif
        (unsigned)sizeof(JSValue),
        (unsigned)qjs_vmprobe_sizeof_stack_frame(),
        (unsigned)qjs_vmprobe_sizeof_var_ref(),
        desc ? desc->version : "?");
    jobs_executed_base = qjs_vmprobe_jobs_executed_get();
    (void)qjs_vmprobe_job_queue_peak_take();   // rebase before the first window
    window_start_us = esp_timer_get_time();
}

void vmprobe_frame_sample(pocketjs_guest_t *guest, int64_t turn_us) {
    window_add(&frame_ms_w, turn_us / 1000.0);
    uint64_t now_jobs = qjs_vmprobe_jobs_executed_get();
    window_add(&jobs_w, (double)(now_jobs - jobs_executed_base));
    jobs_executed_base = now_jobs;
    window_add(&queue_peak_w, (double)qjs_vmprobe_job_queue_peak_take());
    window_ticks++;
    if ((window_ticks % VMPROBE_SAMPLE_EVERY_N_FRAMES) == 0) {
        heap_free_last = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        heap_largest_last = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        stack_hw_last = uxTaskGetStackHighWaterMark(NULL);
        if (guest) {
            pocketjs_guest_stats_t stats = {.struct_size = sizeof(stats)};
            if (pocketjs_guest_stats(guest, &stats) == ESP_OK) {
                js_used_last = stats.heap_used;
                js_limit_last = stats.heap_limit;
            }
        }
    }
    if (esp_timer_get_time() - window_start_us >= VMPROBE_WINDOW_US) flush_window();
}

void vmprobe_completion_sample(int64_t latency_us) {
    window_add(&latency_ms_w, latency_us / 1000.0);
}

void vmprobe_session_reset(void) {
    window_reset(&frame_ms_w); window_reset(&jobs_w);
    window_reset(&queue_peak_w); window_reset(&latency_ms_w);
    jobs_executed_base = qjs_vmprobe_jobs_executed_get();
    (void)qjs_vmprobe_job_queue_peak_take();
    window_start_us = esp_timer_get_time();
    window_ticks = 0;
}

#endif // CONFIG_POCKET_VM_PROBE
