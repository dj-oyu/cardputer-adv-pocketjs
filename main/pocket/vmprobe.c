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
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "vmprobe";

// WHY RAW SAMPLES AND NOT min/median/max ON THE DEVICE.
//
// The first version of this file kept a 48-entry ring per metric and reported
// each window's own min/median/max. That cannot produce an exact p95 over a
// capture: a median of per-window medians is an estimate, and p95 is not
// composable from per-window summaries at all. sec.5 asks for median AND p95
// AND max, so the device now hands every individual sample to the host and
// tools/vm_l0_capture.py computes all three exactly over the whole capture.
//
// The cost of being exact is bounded on both sides:
//   * RAM: the arrays below, all of it in a probe build only (a shipping
//     build does not compile this file at all).
//   * Time: one write per window, not per sample. The window closes on
//     VMPROBE_WINDOW_US *or* when any array fills, whichever comes first, so
//     a sample is never dropped and never overwritten -- "exact" is a
//     property of the capture, not a hope about the sample rate. lat is the
//     one metric that can fire more than once a frame (one per completion),
//     hence its own cap and its own early flush.
//
// The whole window -- the header line and the sample lines -- is built into
// one buffer and printed with one ESP_LOGI. One call is one vprintf, which
// holds stdout's lock for its whole length, so another task's log line cannot
// land inside a sample line and corrupt it. Each sample line still carries its
// own count so the host can prove nothing was lost.
#define VMPROBE_FRAME_CAP 64
#define VMPROBE_LAT_CAP   64
// vm-l1-tuning (docs/vm-l1-tuning.md): jobs returned by ONE vm_sched_drain()
// CALL, not one app tick. "jobs" above is folded across a frame() tick and
// the continuation tick(s) it may spawn (a continuation never reaches
// vmprobe_frame_sample -- app_session.c returns early while the queue is
// still non-empty), so it cannot show whether VM_JOB_FLOOR/VM_JOB_STRIDE ever
// actually cut a call short. This can, and needs its own cap: a floor-limited
// frame tick plus its continuation is already 2 calls per tick.
#define VMPROBE_DRAINRUN_CAP 128
#define VMPROBE_WINDOW_US 1000000
#define VMPROBE_SAMPLE_EVERY_N_FRAMES 8   // ~4 Hz at a 33 ms frame pace
#define VMPROBE_LINE 4096

static uint32_t frame_us[VMPROBE_FRAME_CAP];   // whole pocketjs_ui_turn()
static uint32_t call_us[VMPROBE_FRAME_CAP];    // frame() alone (guest.c)
static uint32_t drain_us[VMPROBE_FRAME_CAP];   // job drain alone (guest.c)
static uint16_t jobs_n[VMPROBE_FRAME_CAP];     // jobs executed in that frame
static uint32_t lat_us[VMPROBE_LAT_CAP];       // completion -> resolve/reject
static uint16_t drainrun_n[VMPROBE_DRAINRUN_CAP]; // ran, one per drain CALL
static unsigned frame_count, lat_count, lat_dropped;
static unsigned drainrun_count, drainrun_dropped_device;
static unsigned qpeak_max;

static uint64_t jobs_executed_base;
static int64_t  window_start_us;
static unsigned window_ticks, window_seq;
static uint32_t flush_us_last;   // the probe's own cost, last window

// Sampled at a low rate (sec.5 asks for this explicitly): JS_ComputeMemoryUsage
// walks live QuickJS structures and heap_caps_get_largest_free_block walks the
// allocator's free lists, so calling either every frame would put its own cost
// inside the very frame time being measured. Kept as the window's extreme --
// the worst moment, not the last one, because a budget is set by the worst.
static size_t   js_used_max, js_limit_last;
static unsigned heap_free_min, heap_largest_min;
static UBaseType_t stack_hw_min;

// Set from the input task (main.c's usb_stroke), read on the ui task at
// session start. Plain atomic: it is one word and the two tasks never need
// more than "the last letter the host sent".
static atomic_uint condition_mask;

void vmprobe_condition_set(unsigned mask) {
    atomic_store(&condition_mask, mask & VMPROBE_COND_ALL);
    ESP_LOGI(TAG, "VMPROBE COND mask=%u", mask & VMPROBE_COND_ALL);
}
unsigned vmprobe_condition(void) { return atomic_load(&condition_mask); }

static void window_reset(void) {
    frame_count = 0; lat_count = 0; lat_dropped = 0; qpeak_max = 0;
    drainrun_count = 0; drainrun_dropped_device = 0;
    js_used_max = 0; heap_free_min = 0; heap_largest_min = 0; stack_hw_min = 0;
    window_start_us = esp_timer_get_time();
    window_ticks = 0;
}

// Appends "name count v,v,v\n" for one metric. Bounds are checked against the
// buffer rather than assumed from the caps, because a future cap change must
// truncate rather than run off the end.
static size_t put_line(char *out, size_t at, unsigned seq, const char *name,
                       const uint32_t *values, const uint16_t *small,
                       unsigned count) {
    if (!count) return at;
    int n = snprintf(out + at, VMPROBE_LINE - at, "VMPROBE S %u %s %u ",
                     seq, name, count);
    if (n < 0 || (size_t)n >= VMPROBE_LINE - at) return at;
    at += (size_t)n;
    for (unsigned i = 0; i < count; i++) {
        unsigned v = values ? values[i] : small[i];
        n = snprintf(out + at, VMPROBE_LINE - at, i ? ",%u" : "%u", v);
        if (n < 0 || (size_t)n >= VMPROBE_LINE - at) break;
        at += (size_t)n;
    }
    if (at < VMPROBE_LINE - 1) out[at++] = '\n';
    out[at] = '\0';
    return at;
}

static void flush_window(void) {
    int64_t began = esp_timer_get_time();
    static char line[VMPROBE_LINE];
    size_t at = 0;
    int n = snprintf(line, sizeof line,
        "VMPROBE WINDOW seq=%u cond=%u ms=%u frames=%u lat_n=%u lat_drop=%u "
        "qpeak_max=%u heap_free_min=%u heap_largest_min=%u js_used_max=%u "
        "js_limit=%u stack_hw_min=%u flush_us=%u drainrun_drop=%u\n",
        window_seq, vmprobe_condition(),
        (unsigned)((began - window_start_us) / 1000),
        frame_count, lat_count, lat_dropped, qpeak_max,
        heap_free_min, heap_largest_min, (unsigned)js_used_max,
        (unsigned)js_limit_last, (unsigned)stack_hw_min, (unsigned)flush_us_last,
        drainrun_dropped_device);
    at = (n > 0 && (size_t)n < sizeof line) ? (size_t)n : 0;
    at = put_line(line, at, window_seq, "frame", frame_us, NULL, frame_count);
    at = put_line(line, at, window_seq, "call",  call_us,  NULL, frame_count);
    at = put_line(line, at, window_seq, "drain", drain_us, NULL, frame_count);
    at = put_line(line, at, window_seq, "jobs",  NULL, jobs_n, frame_count);
    at = put_line(line, at, window_seq, "lat",   lat_us,   NULL, lat_count);
    at = put_line(line, at, window_seq, "drainrun", NULL, drainrun_n, drainrun_count);
    // One call, one vprintf, one lock: see the note at the top of this file.
    ESP_LOGI(TAG, "%s", line);
    window_seq++;
    window_reset();
    flush_us_last = (uint32_t)(esp_timer_get_time() - began);
}

void vmprobe_static_report(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG,
        "VMPROBE STATIC engine=quickjs-ng-0.14.0+immutable-buffer-patch compiler=%s opt=%s "
        "sizeof_jsvalue=%u sizeof_stackframe=%u sizeof_varref=%u fw=%s cond=%u",
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
        desc ? desc->version : "?",
        vmprobe_condition());
    jobs_executed_base = qjs_vmprobe_jobs_executed_get();
    (void)qjs_vmprobe_job_queue_peak_take();   // rebase before the first window
    window_seq = 0;
    pocketjs_guest_vmprobe_take(NULL, NULL);
    (void)pocketjs_guest_vmprobe_drain_calls(NULL, 0, NULL);
    window_reset();
}

void vmprobe_frame_sample(pocketjs_guest_t *guest, int64_t turn_us) {
    uint32_t call = 0, drain = 0;
    pocketjs_guest_vmprobe_take(&call, &drain);
    // vm-l1-tuning: pull whatever accumulated since the last tick that
    // reached here -- a continuation tick's own call(s) included, since they
    // wrote through drain_jobs() same as this tick's did, just never got a
    // vmprobe_frame_sample() of their own to be read at.
    {
        unsigned room = VMPROBE_DRAINRUN_CAP - drainrun_count;
        unsigned dropped = 0;
        unsigned got = pocketjs_guest_vmprobe_drain_calls(
            drainrun_n + drainrun_count, room, &dropped);
        drainrun_count += got;
        drainrun_dropped_device += dropped;
    }
    uint64_t now_jobs = qjs_vmprobe_jobs_executed_get();
    unsigned jobs = (unsigned)(now_jobs - jobs_executed_base);
    jobs_executed_base = now_jobs;
    unsigned peak = (unsigned)qjs_vmprobe_job_queue_peak_take();
    if (peak > qpeak_max) qpeak_max = peak;
    if (frame_count < VMPROBE_FRAME_CAP) {
        frame_us[frame_count] = (uint32_t)(turn_us < 0 ? 0 : turn_us);
        call_us[frame_count] = call;
        drain_us[frame_count] = drain;
        jobs_n[frame_count] = jobs > 0xffffu ? 0xffffu : (uint16_t)jobs;
        frame_count++;
    }
    window_ticks++;
    if ((window_ticks % VMPROBE_SAMPLE_EVERY_N_FRAMES) == 0) {
        unsigned free_now =
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        unsigned largest =
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        UBaseType_t stack = uxTaskGetStackHighWaterMark(NULL);
        if (!heap_free_min || free_now < heap_free_min) heap_free_min = free_now;
        if (!heap_largest_min || largest < heap_largest_min) heap_largest_min = largest;
        if (!stack_hw_min || stack < stack_hw_min) stack_hw_min = stack;
        if (guest) {
            pocketjs_guest_stats_t stats = {.struct_size = sizeof(stats)};
            if (pocketjs_guest_stats(guest, &stats) == ESP_OK) {
                if (stats.heap_used > js_used_max) js_used_max = stats.heap_used;
                js_limit_last = stats.heap_limit;
            }
        }
    }
    // Early flush on a full array is what makes the capture exact: the window
    // is "1 s or 64 frames", never "1 s and whatever fitted".
    if (frame_count >= VMPROBE_FRAME_CAP ||
        lat_count >= VMPROBE_LAT_CAP ||
        drainrun_count >= VMPROBE_DRAINRUN_CAP ||
        esp_timer_get_time() - window_start_us >= VMPROBE_WINDOW_US)
        flush_window();
}

void vmprobe_completion_sample(int64_t latency_us) {
    if (lat_count < VMPROBE_LAT_CAP)
        lat_us[lat_count++] = (uint32_t)(latency_us < 0 ? 0 : latency_us);
    else
        // Only reachable if more than VMPROBE_LAT_CAP completions settle inside
        // one frame, since the frame hook flushes as soon as the array is full.
        // Counted and reported rather than silently dropped: a non-zero
        // lat_drop tells the host its p95 for that window is not exact.
        lat_dropped++;
}

void vmprobe_session_reset(void) {
    jobs_executed_base = qjs_vmprobe_jobs_executed_get();
    (void)qjs_vmprobe_job_queue_peak_take();
    pocketjs_guest_vmprobe_take(NULL, NULL);
    (void)pocketjs_guest_vmprobe_drain_calls(NULL, 0, NULL);
    window_reset();
}

#endif // CONFIG_POCKET_VM_PROBE
