// G12 fragmentation probe; see oomprobe.h and docs/vm/vm-L3-results.md.
//
// The question is not "total free >= request > largest free block". The
// internal heap is several separate regions (DRAM split by static data and
// the ROM's reserved ranges), and no compaction can join free space that sits
// in two different regions. So the hook walks the heap and asks it per
// region: is there ONE region whose free bytes would hold the request if they
// were contiguous, while its largest free block does not? Only that failure is
// one moving blocks (L3/L4) could have prevented. The global free/largest pair
// is printed as well, because it is what the backlog's criterion literally
// names and the difference between the two readings is itself a result.
//
// A candidate is still not a case for L3/L4: those move the VM's frame
// segments and nothing else, so what matters is what splits the free space.
// The walk therefore also treats every segment block as if it were free --
// moved elsewhere -- and reports the longest run of free-or-segment blocks
// ("noseg"). A refusal is one moving segments could have fixed only if that
// run would have held the request ("segfix=1").
#include "oomprobe.h"
#include "pocket_memory.h"
#include "quickjs.h"
#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>

#define OOMPROBE_CAP 16
#define OOMPROBE_BT 5
// More than any chain the device has shown (L3a's deepest was 9 live
// segments); a longer one is reported as truncated rather than misread.
#define OOMPROBE_SEGS 48

typedef struct {
    uint32_t req, caps;
    uint32_t free, largest;           // over every heap matching caps
    uint32_t rfree, rlargest, rsize;  // the region with the most free bytes
    uint32_t noseg;                   // longest free-or-segment run, any region
    uint16_t regions, walk_us;
    uint16_t segs, segs_seen;         // segments the VM has / the walk found
    bool candidate;                   // some region: free >= req > largest
    bool segfix;                      // some region: noseg >= req > largest
    uint32_t bt[OOMPROBE_BT];
} oom_rec_t;

static oom_rec_t recs[OOMPROBE_CAP];
static unsigned rec_n, rec_dropped;
static unsigned sess_fails, sess_candidates, sess_segfix, sess_canary, sess_canary_turns;
static portMUX_TYPE rec_mux = portMUX_INITIALIZER_UNLOCKED;
static bool installed;
static JSRuntime *vm_rt;
static TaskHandle_t vm_task;

typedef struct {
    intptr_t start;                   // region being summed
    uint32_t free, largest, size, blocks;
    uint32_t run, run_max;            // free-or-segment run, in this region
    oom_rec_t *out;
    uint32_t req;
    const void **segs;
    uint32_t nsegs;
} walk_t;

static void region_close(walk_t *w) {
    if(!w->start) return;
    oom_rec_t *r = w->out;
    r->regions++;
    // Joined, the free blocks would also give back all but one of their
    // headers (tlsf: one size_t each), so a region a few bytes short of the
    // request by its payload sum can still be a candidate.
    uint32_t joined = w->free + (w->blocks ? (w->blocks - 1) * (uint32_t)sizeof(size_t) : 0);
    if(joined >= w->req && w->largest < w->req) r->candidate = true;
    if(w->run_max >= w->req && w->largest < w->req) r->segfix = true;
    if(w->run_max > r->noseg) r->noseg = w->run_max;
    if(w->free > r->rfree) {
        r->rfree = w->free; r->rlargest = w->largest; r->rsize = w->size;
    }
}

static bool is_segment(const walk_t *w, const void *p) {
    for(uint32_t i = 0; i < w->nsegs; i++)
        if(w->segs[i] == p) return true;
    return false;
}

// Runs under the heap's own lock (multi_heap_walk): no allocation, no logging.
// tlsf walks a region's blocks in address order, which is what makes "run"
// mean physically adjacent.
static bool walk_block(walker_heap_into_t heap, walker_block_info_t block, void *user) {
    walk_t *w = user;
    if(heap.start != w->start) {
        region_close(w);
        w->start = heap.start;
        w->free = w->largest = w->blocks = w->run = w->run_max = 0;
        w->size = (uint32_t)(heap.end - heap.start);
    }
    bool seg = block.used && is_segment(w, block.ptr);
    if(seg) w->out->segs_seen++;
    if(!block.used || seg) {
        // Each block after the first in a run also gives back its header.
        w->run += block.size + (w->run ? (uint32_t)sizeof(size_t) : 0);
        if(w->run > w->run_max) w->run_max = w->run;
    } else {
        w->run = 0;
    }
    if(!block.used) {
        w->blocks++;
        w->free += block.size;
        if(block.size > w->largest) w->largest = block.size;
    }
    return true;
}

static void failed_alloc(size_t size, uint32_t caps, const char *function_name) {
    pocket_memory_native_failure(size,caps);
    (void)function_name;  // always a heap_caps_* entry point; the backtrace says who
    oom_rec_t r = {.req = (uint32_t)size, .caps = caps};
    // Reject-time state, before anything the caller does next frees memory.
    uint32_t walk_caps = caps ? caps : MALLOC_CAP_DEFAULT;
    r.free = heap_caps_get_free_size(walk_caps);
    r.largest = heap_caps_get_largest_free_block(walk_caps);
    int64_t t0 = esp_timer_get_time();
    const void *segs[OOMPROBE_SEGS];
    walk_t w = {.out = &r, .req = (uint32_t)size, .segs = segs};
    // Only on the VM's own task: anywhere else the chain may be mid-push.
    if(vm_rt && xTaskGetCurrentTaskHandle() == vm_task) {
        uint32_t n = JS_VMStackBlocks(vm_rt, segs, OOMPROBE_SEGS);
        r.segs = n > 0xffff ? 0xffff : (uint16_t)n;
        w.nsegs = n < OOMPROBE_SEGS ? n : OOMPROBE_SEGS;
    }
    heap_caps_walk(walk_caps, walk_block, &w);
    region_close(&w);
    int64_t dt = esp_timer_get_time() - t0;
    r.walk_us = dt > 0xffff ? 0xffff : (uint16_t)dt;
    // Skip this hook and heap_caps_alloc_failed; the third frame on is the
    // allocator entry point and its caller.
    esp_backtrace_frame_t f;
    esp_backtrace_get_start(&f.pc, &f.sp, &f.next_pc);
    for(int i = 0, kept = 0; kept < OOMPROBE_BT && i < OOMPROBE_BT + 2; i++) {
        if(i >= 2) r.bt[kept++] = esp_cpu_process_stack_pc(f.pc);
        if(!esp_backtrace_get_next_frame(&f)) break;
    }
    portENTER_CRITICAL(&rec_mux);
    sess_fails++;
    if(r.candidate) sess_candidates++;
    if(r.segfix) sess_segfix++;
    if(rec_n < OOMPROBE_CAP) recs[rec_n++] = r;
    else rec_dropped++;
    portEXIT_CRITICAL(&rec_mux);
}

void oomprobe_set_runtime(void *rt) {
    portENTER_CRITICAL(&rec_mux);
    vm_rt = rt;
    vm_task = rt ? xTaskGetCurrentTaskHandle() : NULL;
    portEXIT_CRITICAL(&rec_mux);
}

void oomprobe_init(void) {
    if(installed) return;
    installed = heap_caps_register_failed_alloc_callback(failed_alloc) == ESP_OK;
    ESP_LOGI("g12", "G12 PROBE %s", installed ? "on" : "FAILED");
}

void oomprobe_drain(void) {
    oom_rec_t local[OOMPROBE_CAP];
    unsigned n, dropped;
    portENTER_CRITICAL(&rec_mux);
    n = rec_n; dropped = rec_dropped;
    for(unsigned i = 0; i < n; i++) local[i] = recs[i];
    rec_n = rec_dropped = 0;
    portEXIT_CRITICAL(&rec_mux);
    for(unsigned i = 0; i < n; i++) {
        const oom_rec_t *r = &local[i];
        printf("G12 FAIL req=%lu caps=0x%lx free=%lu largest=%lu rfree=%lu rlargest=%lu"
               " rsize=%lu regions=%u cand=%u noseg=%lu segs=%u/%u segfix=%u walk_us=%u"
               " bt=0x%08lx,0x%08lx,0x%08lx,0x%08lx,0x%08lx\n",
               (unsigned long)r->req, (unsigned long)r->caps, (unsigned long)r->free,
               (unsigned long)r->largest, (unsigned long)r->rfree, (unsigned long)r->rlargest,
               (unsigned long)r->rsize, r->regions, r->candidate, (unsigned long)r->noseg,
               r->segs_seen, r->segs, r->segfix, r->walk_us,
               (unsigned long)r->bt[0], (unsigned long)r->bt[1], (unsigned long)r->bt[2],
               (unsigned long)r->bt[3], (unsigned long)r->bt[4]);
    }
    if(dropped) printf("G12 DROPPED %u\n", dropped);
}

void oomprobe_canary(uint32_t n, size_t first_req, size_t first_used) {
    sess_canary += n;
    sess_canary_turns++;
    // The heap as the turn left it, not as the rejection saw it: a limit
    // rejection never consulted the heap, so there is no reject-time state to
    // have kept. Printed so a request that was refused by the limit can still
    // be compared with what the heap could have given.
    printf("G12 CANARY n=%lu first_req=%lu used=%lu free=%lu largest=%lu\n",
           (unsigned long)n, (unsigned long)first_req, (unsigned long)first_used,
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void oomprobe_session_end(const char *label) {
    oomprobe_drain();
    unsigned fails, cand, segfix;
    portENTER_CRITICAL(&rec_mux);
    fails = sess_fails; cand = sess_candidates; segfix = sess_segfix;
    sess_fails = sess_candidates = sess_segfix = 0;
    portEXIT_CRITICAL(&rec_mux);
    printf("G12 SESSION %s heap_fails=%u candidates=%u segfix=%u canary_rejections=%u"
           " canary_turns=%u\n", label, fails, cand, segfix, sess_canary, sess_canary_turns);
    sess_canary = sess_canary_turns = 0;
}
