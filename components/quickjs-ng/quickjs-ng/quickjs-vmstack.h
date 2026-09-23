// L2a: bytecode-function frames in runtime-owned, non-moving segments
// (docs/vm/quickjs-freertos-vm-spec.md sec.7 "L2a", docs/vm/vm-L2-design.md sec.2).
//
// NOT part of the upstream quickjs-ng import. Header-only on purpose: the only
// includer that instantiates anything is quickjs.c (every function is `static
// inline`, so quickjs-vm.c can include it for the struct layout alone without
// pulling in a second copy), and no build list -- CMake, tools/vmtest/build.sh,
// tools/build_pocket_text_test.sh -- has to learn a new object.
//
// What moves. Upstream JS_CallInternal keeps two things on the C stack per JS
// call: `JSStackFrame sf_s` (a plain automatic) and one alloca() block holding
// arg_buf / var_buf / stack_buf / var_refs (ledger 02 sec.2a). Both are what a
// JSVarRef and rt->current_stack_frame point at with raw pointers, so both must
// live somewhere that neither moves nor disappears when this C frame returns
// early -- spec sec.7: "L2a moves the JSStackFrame itself, not only the
// alloca block". With CONFIG_POCKET_VM_SEGFRAMES the two become ONE block here,
// [JSStackFrame][JSValue slots...][JSVarRef *...], carved from a segment.
//
// What does not move: the C recursion. JS_CallInternal still calls itself for
// a JS->JS call (that is L2b), so frames are pushed and popped in strict LIFO
// order for as long as this stage lasts. That is the whole reason this can be
// a bump allocator: push is `top += size`, pop is `top = block`, and a segment
// is retired the moment its first block is popped. Generator / async frames
// never come through here -- async_func_init keeps them in their own
// js_malloc'd JSAsyncFunctionState (ledger 02 sec.2b) and JS_CallInternal is
// resumed with JS_CALL_FLAG_GENERATOR, a path that neither pushes nor pops.
//
// Where the memory comes from: js_malloc_rt, i.e. the guest heap, so a segment
// is subject to JS_SetMemoryLimit and shows up in JS_ComputeMemoryUsage. This
// is the "runtime-owned pool" of spec sec.7 with the pool being the guest heap
// itself; a separate arena (design D6, still open) would change only
// js_vm_seg_new / js_vm_seg_del.
//
// Segment size: LINEAR in position along the live chain, not fixed (design
// D42). The n-th segment on the chain (n = 1 for the bottom one) gets
// min(JS_VM_SEG_FIRST * n, JS_VM_SEG_MAX) bytes of payload. Most calls never
// go deep, so the segments that pay for most of the resident cost are small
// (JS_VM_SEG_FIRST, 512 on the shipped config); a recursion that commits to
// depth pays for larger segments only once it is already that deep, and
// JS_VM_SEG_MAX (4096, ledger 07's figure, chosen on OBJECT allocation
// histories, not frame lifetimes) caps how large any one of them gets --
// taffy's biggest contiguous request (59,296 B, the top node-count step in
// CLAUDE.md's taffy table) must still find room, so the standard segment can
// never grow past what it was before this change (docs/vm/vm-L2-results.md
// sec.3.4 measured 6,240 B of headroom against that step at today's 4,096).
// Position, not history: the size is read off
// st->seg_live (segments CURRENTLY on the chain), never a cumulative
// counter, so one deep recursion does not make a later, shallow one pay for
// a bigger first segment.
//
// A frame that does not fit the size its position would naturally get, but
// is still <= JS_VM_SEG_MAX, does NOT become a dedicated segment -- that
// would turn an ordinary large function called at shallow depth into a
// malloc/free pair per call. Instead the position is advanced to the first
// size in the sequence that fits (js_vm_seg_want_for_size), capped at
// JS_VM_SEG_MAX; only a frame that exceeds JS_VM_SEG_MAX itself gets a
// "dedicated" segment, sized to exactly that frame, freed on pop and never
// cached, the same rule tools/vmalloc/adapter_segment.c follows. When even a
// segment sized to the frame's own need cannot be obtained (memory limit)
// the push has nothing smaller left to retry with -- see js_vm_seg_new's
// caller in js_vm_stack_push_slow -- so a call fails only when the frame
// ITSELF does not fit, the boundary the alloca path had, rather than
// whenever the position's chosen size happens not to be available.
//
// vmtest_vmstack_configure / --vm-seg-size (a fixed size, unrelated to
// position) still work: js_vm_stack_configure sets seg_first == seg_max ==
// the configured size, which collapses the position formula to that one
// value for every n, exactly the old fixed-size behaviour.
//
// Alignment (design D7): the payload base is 16-byte aligned (the L4
// compactor's PIE 128-bit moves want it; it costs 15 bytes per segment, once).
// Individual blocks are 4-byte aligned on the target -- Xtensa LX7 has no
// 64-bit load, a JSValue is two l32i.n either way -- and ABI-aligned (8) on
// the host, where UBSan checks every JSValue / pointer access against the
// declared alignment and would report a 4-aligned JSStackFrame as UB. The
// difference is at most 4 bytes per frame with an odd var_ref_count.
#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cutils.h"
#include "quickjs.h"

// Same reason quickjs-vmprobe.h does this: the config macro lives in
// sdkconfig.h, which ESP-IDF puts on the include path but never force-
// includes; host builds have none (tools/vmtest/build.sh passes the macro
// with -D instead, tools/build_pocket_text_test.sh passes nothing and gets
// the alloca path).
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// L2b (CONFIG_POCKET_VM_FLATCALLS, docs/vm/vm-L2-design.md sec.9) is a
// property of frames that live here: a flat callee's frame is popped by the
// same JS_CallInternal activation that pushed it, which is only possible
// when the frame is not on that activation's C stack. Kconfig says
// "depends on"; the host passes -D by hand, so the header says it too.
#if defined(CONFIG_POCKET_VM_FLATCALLS) && !defined(CONFIG_POCKET_VM_SEGFRAMES)
#error "CONFIG_POCKET_VM_FLATCALLS requires CONFIG_POCKET_VM_SEGFRAMES"
#endif

#define JS_VM_SEG_ALIGN 16

// A 32-bit host (tools/vmtest/m32_sysroot.sh) takes the device's value: its
// frame link is one 4-byte pointer, and 8 would fail the assert below.
#if defined(ESP_PLATFORM) || UINTPTR_MAX == 0xFFFFFFFFu
#define JS_VM_FRAME_ALIGN 4
#else
#define JS_VM_FRAME_ALIGN 8
#endif

// Ceiling on standard segment payload. 4096 is ledger 07's figure for the
// general allocator; for frames the number that matters is "frames per
// segment": on the target a small function's frame is 48 + 8 * (args + vars
// + stack) bytes, ~136 B for a typical one (計算値), so 4096 holds ~30
// nesting levels before a second segment is needed. Must not grow past this:
// taffy needs a 59,296 B contiguous block on the device (CLAUDE.md's taffy
// node-count table, the 34-node-or-more step), and that request goes through
// js_malloc_rt like everything else here, so a bigger standard segment would
// eat into the same headroom (docs/vm/vm-L2-results.md sec.3.4 measured 6,240 B
// of headroom against that step at today's 4,096). "#info vmstack" from
// vmrun (--stats) reports what the corpus actually needed; see the L2a
// report.
#ifndef JS_VM_SEG_MAX
#define JS_VM_SEG_MAX 4096
#endif
// Payload of the bottom (n=1) standard segment; each position n up the
// chain gets min(JS_VM_SEG_FIRST * n, JS_VM_SEG_MAX) (design D42). 512 keeps
// hello's MEASURED live_max (456 B, device probe build fbc793b, 2026-09-13)
// inside the bottom segment; 148-172 B for the other single-segment vmprobe
// apps measured in the same run. Its cumulative resident advantage over a
// 1024 base -- 512/1,536/3,072 B at chain positions 1/2/3 against
// 1,024/3,072/6,144 -- is CALCULATED from the position formula itself, not
// separately measured: most calls never recurse past the first segment, so
// most of the resident cost is paid at this size, not at JS_VM_SEG_MAX.
#ifndef JS_VM_SEG_FIRST
#define JS_VM_SEG_FIRST 512
#endif
// Empty standard segments kept for reuse instead of being returned (D43).
// No count limit: with sizes that grow by position (D42) one cached segment
// only absorbs a depth oscillating across ONE boundary -- bench_calls at
// depth 31 crosses three and went from 2 to 129,940 segment mallocs with a
// cache of 1 (measured, host). The cache is a LIFO of what the returning
// calls emptied, so descending again takes them back in exactly the order
// the positions want. What bounds the memory is time, not count: the host
// calls JS_VMStackTrim when a turn's job queue is empty, so segments are
// held only within the turn that already had them live, and the heap never
// holds more between turns than the one resident bottom segment.
// vmrun --vm-seg-size still passes 1 (the fixed-size sweeps predate D42).
#ifndef JS_VM_SEG_CACHE_MAX
#define JS_VM_SEG_CACHE_MAX UINT32_MAX
#endif

// Counters cost a few adds per call; the host harness reads them, the
// firmware only under the probe config.
#if !defined(ESP_PLATFORM) || defined(CONFIG_POCKET_VM_PROBE)
#define JS_VM_STACK_STATS 1
#endif

// Under ASan the free part of every segment is poisoned: a segment is
// poisoned whole when it comes from js_malloc_rt, a push unpoisons exactly
// the block it hands out, a pop re-poisons it. So a pointer into a popped
// frame -- a JSVarRef that close_var_refs missed, a frame walker reading
// past the live chain, a caller keeping the callee's argv -- is a
// use-after-poison report at the first touch, instead of a silent read of
// bytes that are still there. This is the check ledger 07 sec.6 said the
// allocator-level gate could not do: it works on the real frames, in the
// real VM, under every corpus and test262 run of the asan variant. Costs
// nothing outside ASan (the macros expand to nothing) and nothing on the
// target, which never builds with it.
#if defined(__SANITIZE_ADDRESS__)
#define JS_VM_STACK_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define JS_VM_STACK_ASAN 1
#endif
#endif
#ifdef JS_VM_STACK_ASAN
#include <sanitizer/asan_interface.h>
#define JS_VM_POISON(p, n)   __asan_poison_memory_region((p), (n))
#define JS_VM_UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
#define JS_VM_POISON(p, n)   ((void)0)
#define JS_VM_UNPOISON(p, n) ((void)0)
#endif

// The header sits at the front of its own js_malloc_rt block; the payload
// follows it, rounded up to JS_VM_SEG_ALIGN.
typedef struct JSVMSeg {
    struct JSVMSeg *prev;   // segment below this one (older frames); NULL at the bottom
    uint8_t *base;          // payload start, 16-aligned
    uint8_t *end;           // payload end
    uint8_t *top;           // first free byte; == base when empty
    // Set once at js_vm_seg_new and never recomputed: whether this segment
    // is a member of the size-by-position growth sequence (cacheable, and
    // the reason a bottom segment stays resident) or a one-off -- a frame
    // bigger than JS_VM_SEG_MAX, or the memory-limit fallback sized to a
    // frame alone after the sequence size failed to allocate. Recomputing
    // "was this the sequence size for its position" from payload alone at
    // pop time is not free: the position depends on chain depth AT
    // CREATION, which by pop time can only be had by walking prev links.
    // Cost: four pointers plus this int. On the target that is 16 -> 20 B
    // (checked with the xtensa compiler); on the host 32 -> 40 B (the int
    // is padded to pointer alignment), which is what frame_hdr= in
    // "#info vmstack" reports. Per segment, so hello's single resident
    // segment pays +4 B against the ~3.5 KiB the smaller JS_VM_SEG_FIRST
    // payload saves (docs/vm/vm-L2-design.md sec.7.2, calculated).
    int standard;
} JSVMSeg;

typedef struct JSVMStack {
    JSVMSeg *cur;           // topmost segment; NULL until the first push
    JSVMSeg *cache;         // empty standard segments kept for reuse, linked by prev
    uint32_t cache_n;
    uint32_t cache_max;
    size_t seg_first;       // payload of the bottom (n=1) standard segment
    size_t seg_max;         // ceiling every standard segment's payload is capped at
    // D10 (docs/vm/vm-L2-design.md sec.8): the recursion limit that survives
    // L2b. `used` is the sum of (top - base) over the live chain -- pushed
    // frame bytes, rounded, NOT counting segment headers or the unused tail
    // of each segment -- kept incrementally so the budget test is one add
    // and one compare per call. `budget` is JS_SetMaxStackSize's value (0 =
    // no limit), mirrored here by update_stack_limit; a push that would take
    // `used` past it is refused as RangeError BEFORE any segment is asked
    // for, which is what keeps "too deep" (RangeError) distinguishable from
    // "no heap for the frame" (InternalError) -- the two answers the corpus
    // fixes in expected/deep_recursion*.txt and expected/seg_oom_boundary.txt.
    // Bytes, not frames: a frame is 48 B + 8 B per arg/local/operand slot on
    // the target, so a frame count would bound memory by nothing.
    size_t used;
    size_t budget;
#ifdef CONFIG_POCKET_VM_RELOC
    // L3a (docs/vm/vm-L3-design.md D51). `generation` counts completed
    // moves; nothing in the VM reads it yet -- L3a keeps raw pointers and
    // fixes them up at move time, so there is no relative reference to
    // check it against. It is here because the test has to be able to tell
    // "the move happened" from "the move was a no-op", and because L3b
    // needs the field to exist in a layout it does not get to change.
    //
    // `pins` is 0 at every moment L3a can observe: a move is only legal
    // while the VM is parked (D45), and no native call can be on the C
    // stack at a park. The counter is the shape of the rule, not an
    // implementation of it -- js_vm_pin exists so the "a pin refuses a
    // move" completion condition (spec sec.8) has something to test, and
    // so that the day a real pin site appears it is a call, not a design.
    uint32_t generation;
    uint32_t pins;
#endif
#ifdef JS_VM_STACK_STATS
    size_t live_bytes_max;               // peak of `used`
    size_t frame_max;                    // largest single frame pushed
    uint32_t depth, depth_max;           // frames in use
    uint32_t seg_live, seg_live_max;     // segments on the chain
    uint64_t pushes, seg_mallocs, seg_frees, seg_reuses, dedicated, fallbacks;
    uint64_t budget_hits;                // pushes refused by the budget
    uint64_t seg_refused;                // pushes refused by the runtime (no segment)
    // Bytes the LIVE CHAIN's segments hold, header and alignment slack
    // included -- payload varies by position now, so this is a running sum
    // kept at every segment join/leave (js_vm_stack_push_slow / pop), not a
    // count times one fixed size the way the old resident_max~ was computed.
    // Excludes the cache, which D43 lets grow within a turn: `cached` is the
    // same kind of sum for the cached segments, and held_max the peak of
    // resident + cached -- what the heap actually gave up at the worst
    // moment. Reusing a cached segment moves bytes from one sum to the other
    // and cannot raise held_max; only a fresh malloc can. trims counts
    // JS_VMStackTrim calls that found something to free.
    size_t resident, resident_max;
    size_t cached, held_max;
    uint64_t trims;
#endif
} JSVMStack;

static inline void js_vm_stack_init(JSVMStack *st)
{
    memset(st, 0, sizeof(*st));
    st->seg_first = JS_VM_SEG_FIRST;
    st->seg_max = JS_VM_SEG_MAX;
    st->cache_max = JS_VM_SEG_CACHE_MAX;
}

static inline size_t js_vm_stack_round(size_t size)
{
    return (size + JS_VM_FRAME_ALIGN - 1) & ~(size_t)(JS_VM_FRAME_ALIGN - 1);
}

// The D10 test, asked by JS_CallInternal before js_vm_stack_push so that
// the budget is charged for exactly the bytes the push would add (the same
// rounding) and is consulted before the heap is. Placed here rather than
// inside push so that push keeps one failure meaning (NULL = the runtime
// refused memory) and the caller does not have to decode two.
static inline int js_vm_stack_over_budget(JSVMStack *st, size_t size)
{
    size = js_vm_stack_round(size);
    if (likely(!st->budget || st->used + size <= st->budget))
        return 0;
#ifdef JS_VM_STACK_STATS
    st->budget_hits++;
#endif
    return 1;
}

// Only before the first push: a live chain built on one sizing cannot be
// re-described with another (the cache test in pop compares against it).
// Fixed size, on purpose: FIRST == MAX == seg_size collapses
// js_vm_seg_want's min(seg_first * n, seg_max) to seg_size for every
// position n, so a configured size keeps meaning exactly what it meant
// before D42 -- every standard segment that size, position-independent --
// which is what the existing --vm-seg-size sweeps and seg_* corpus files
// rely on.
static inline int js_vm_stack_configure(JSVMStack *st, size_t seg_size, uint32_t cache_max)
{
    if (st->cur || st->cache || seg_size < JS_VM_SEG_ALIGN)
        return -1;
    seg_size = (seg_size + JS_VM_SEG_ALIGN - 1) & ~(size_t)(JS_VM_SEG_ALIGN - 1);
    st->seg_first = seg_size;
    st->seg_max = seg_size;
    st->cache_max = cache_max;
    return 0;
}

// Diagnostic D42 sweep: retain the production cache policy while varying
// FIRST/MAX in one binary. Refuse a live or cached chain without mutation.
static inline int js_vm_stack_configure_growth(JSVMStack *st, size_t first, size_t maximum)
{
    if (st->cur || st->cache || first < JS_VM_SEG_ALIGN || maximum < first ||
        maximum > SIZE_MAX / 2 || first % JS_VM_SEG_ALIGN ||
        maximum % first)
        return -1;
    st->seg_first = first;
    st->seg_max = maximum;
    return 0;
}

static inline size_t js_vm_seg_payload(const JSVMSeg *s)
{
    return (size_t)(s->end - s->base);
}

// The header cost this stack pays per live segment (report and resident
// accounting share this so the two cannot drift apart).
static inline size_t js_vm_seg_overhead(void)
{
    return sizeof(JSVMSeg) + (JS_VM_SEG_ALIGN - 1);
}

// The payload a standard segment at chain position `n` (n=1 for the bottom
// one) gets, before considering whether the frame that triggered it
// actually fits (design D42). Never 0: n is clamped to at least 1 and
// seg_first/seg_max are always >= JS_VM_SEG_ALIGN (js_vm_stack_init /
// _configure).
static inline size_t js_vm_seg_want(const JSVMStack *st, uint32_t n)
{
    if (n < 1)
        n = 1;
    // seg_first divides evenly into every payload this function or
    // js_vm_stack_configure ever produces, and seg_max/seg_first is a small
    // integer (8 on the shipped 512/4096 config) -- guard the multiply
    // against a pathological huge n (a very deep chain) overflowing size_t
    // by capping n first rather than checking the product after.
    size_t max_n = st->seg_max / st->seg_first;
    if (max_n == 0 || (size_t)n >= max_n)
        return st->seg_max;
    return (size_t)n * st->seg_first;
}

// The payload to give a new standard segment when a frame of `size` bytes
// does not fit in the current top segment and the chain is at position `n`
// (one past however many segments are already live). Returns 0 when even
// JS_VM_SEG_MAX cannot hold the frame -- the caller's cue to fall back to a
// dedicated segment sized to the frame alone -- and otherwise the smallest
// sequence size >= size, which may be js_vm_seg_want(st, n) itself (the
// frame fits its natural position) or a later position's size (the frame is
// bigger than position n wants but still <= seg_max): advancing the
// position this way, rather than dedicating, is what keeps an ordinary
// large function called at shallow depth from costing a malloc/free pair
// per call.
static inline size_t js_vm_seg_want_for_size(const JSVMStack *st, uint32_t n, size_t size)
{
    size_t want = js_vm_seg_want(st, n);
    if (size <= want)
        return want;
    if (size > st->seg_max)
        return 0;
    // Jump straight to the first position whose size covers `size` instead
    // of walking the sequence one step at a time -- a decision, not a loop
    // bounded by seg_max/seg_first.
    size_t n2 = (size + st->seg_first - 1) / st->seg_first;
    if (n2 < 1)
        n2 = 1;
    want = js_vm_seg_want(st, (uint32_t)n2);
    // want >= size is guaranteed here (n2*seg_first >= size by construction,
    // and js_vm_seg_want only ever returns less than that ceiling when it
    // clamps to seg_max, which the size <= st->seg_max check above already
    // established is >= size).
    return want;
}

// Host harness only (tools/vmtest/vmrun.c defines it; everything else, the
// firmware included, leaves the weak reference NULL): "the next allocation
// is a frame segment". vmrun marks it in its trace so tools/vmalloc/replay
// can serve segments from a region of their own (--seg-arena) and ask
// whether keeping them out of the objects' heap helps it (design D6,
// docs/vm/vm-L3-design.md sec.11).
//
// It carries the request size and is cleared (size 0) as soon as
// js_malloc_rt returns, because js_malloc_rt can refuse on the memory limit
// without calling the allocator at all: a bare "next one is a segment" flag
// would then be left set and label whatever unrelated allocation came next.
#if !defined(ESP_PLATFORM)
extern void vmtest_seg_alloc_hint(size_t size) __attribute__((weak));
#endif

// `standard`: whether this segment belongs to the size-by-position growth
// sequence (see JSVMSeg.standard) -- false for a frame bigger than
// JS_VM_SEG_MAX and for the memory-limit fallback sized to a frame alone.
static inline JSVMSeg *js_vm_seg_new(JSRuntime *rt, JSVMStack *st, size_t payload, int standard)
{
#if !defined(ESP_PLATFORM)
    if (vmtest_seg_alloc_hint)
        vmtest_seg_alloc_hint(sizeof(JSVMSeg) + (JS_VM_SEG_ALIGN - 1) + payload);
#endif
    JSVMSeg *s = js_malloc_rt(rt, sizeof(JSVMSeg) + (JS_VM_SEG_ALIGN - 1) + payload);
#if !defined(ESP_PLATFORM)
    if (vmtest_seg_alloc_hint)
        vmtest_seg_alloc_hint(0);
#endif
    if (!s)
        return NULL;
    uintptr_t b = ((uintptr_t)(s + 1) + JS_VM_SEG_ALIGN - 1) & ~(uintptr_t)(JS_VM_SEG_ALIGN - 1);
    s->base = s->top = (uint8_t *)b;
    s->end = s->base + payload;
    s->prev = NULL;
    s->standard = standard;
    JS_VM_POISON(s->base, payload);
#ifdef JS_VM_STACK_STATS
    st->seg_mallocs++;
    if (!standard)
        st->dedicated++;
#else
    (void)st;
#endif
    return s;
}

static inline void js_vm_seg_del(JSRuntime *rt, JSVMStack *st, JSVMSeg *s)
{
#ifdef JS_VM_STACK_STATS
    st->seg_frees++;
#else
    (void)st;
#endif
    // ASan wants a region unpoisoned by the time it is freed; free() then
    // poisons the whole chunk itself, so nothing is lost.
    JS_VM_UNPOISON(s->base, js_vm_seg_payload(s));
    js_free_rt(rt, s);
}

// The frame does not fit in the current segment (or there is none yet).
// NULL means the runtime refused the memory: the caller maps that to
// out-of-memory, not stack overflow -- see JS_CallInternal.
static inline void *js_vm_stack_push_slow(JSRuntime *rt, JSVMStack *st, size_t size)
{
    JSVMSeg *s;
    // Position of the segment about to be pushed: one past however many are
    // already live. Read from seg_live (stats builds only); a non-stats
    // (firmware) build carries no such counter, so it walks prev links here
    // instead -- acceptable because this is push_slow, already the slow
    // path taken once per segment rather than once per frame, and the walk
    // is bounded by the live segment count, which JS_VM_SEG_MAX keeps small
    // (at most a few dozen segments even at the device's 20 KiB budget).
#ifdef JS_VM_STACK_STATS
    uint32_t position = st->seg_live + 1;
#else
    uint32_t position = 1;
    for (JSVMSeg *w = st->cur; w; w = w->prev)
        position++;
#endif
    size_t want = js_vm_seg_want_for_size(st, position, size);
    if (want) {
        // Reused only when the most recently cached segment is exactly the
        // size THIS position wants (D42). The cache is LIFO and fills in the
        // order returning calls empty the chain, top down, so on the way back
        // down its head is the next position's size; a mismatch (a position
        // advanced for a large frame) is left in place for the position it
        // does match rather than evicted.
        if (st->cache && js_vm_seg_payload(st->cache) == want) {
            s = st->cache;
            st->cache = s->prev;
            st->cache_n--;
#ifdef JS_VM_STACK_STATS
            st->seg_reuses++;
            st->cached -= js_vm_seg_payload(s) + js_vm_seg_overhead();
#endif
        } else {
            s = js_vm_seg_new(rt, st, want, 1);
            if (unlikely(!s)) {
                // Memory is short: retry sized to exactly this frame, which
                // is <= want. Not a sequence size in general, so not marked
                // standard -- js_vm_seg_new will not offer it to the cache
                // on pop, and pop's "bottom stays resident" rule will not
                // pin an oversized block here either.
                s = js_vm_seg_new(rt, st, size, 0);
                if (!s)
                    return NULL;
#ifdef JS_VM_STACK_STATS
                st->fallbacks++;
#endif
            }
        }
    } else {
        // size > st->seg_max: a dedicated segment sized to the frame alone,
        // freed on pop, never cached.
        s = js_vm_seg_new(rt, st, size, 0);
        if (!s)
            return NULL;
    }
    s->prev = st->cur;
    st->cur = s;
#ifdef JS_VM_STACK_STATS
    st->seg_live++;
    if (st->seg_live > st->seg_live_max)
        st->seg_live_max = st->seg_live;
    st->resident += js_vm_seg_payload(s) + js_vm_seg_overhead();
    if (st->resident > st->resident_max)
        st->resident_max = st->resident;
    if (st->resident + st->cached > st->held_max)
        st->held_max = st->resident + st->cached;
#endif
    void *p = s->top;
    s->top += size;
    JS_VM_UNPOISON(p, size);
    return p;
}

// Returns a block of `size` bytes (rounded to JS_VM_FRAME_ALIGN) that stays
// at its address until the matching js_vm_stack_pop. Never returns memory
// the caller has to zero: JS_CallInternal initialises every slot it uses,
// exactly as it did with alloca.
static inline void *js_vm_stack_push(JSRuntime *rt, JSVMStack *st, size_t size)
{
    JSVMSeg *s = st->cur;
    size = js_vm_stack_round(size);
    // `used` is charged up front and refunded on a refused push; the fast
    // path is then one add, the same as the stats build already paid.
    st->used += size;
#ifdef JS_VM_STACK_STATS
    st->pushes++;
    st->depth++;
    if (st->depth > st->depth_max)
        st->depth_max = st->depth;
    if (st->used > st->live_bytes_max)
        st->live_bytes_max = st->used;
    if (size > st->frame_max)
        st->frame_max = size;
#endif
    if (likely(s && (size_t)(s->end - s->top) >= size)) {
        void *p = s->top;
        s->top += size;
        JS_VM_UNPOISON(p, size);
        return p;
    }
    void *p = js_vm_stack_push_slow(rt, st, size);
    if (!p) {
        st->used -= size;
#ifdef JS_VM_STACK_STATS
        st->depth--;
        st->seg_refused++;
#endif
    }
    return p;
}

// True when `p` lies in the live part of the top segment, i.e. it is a
// block this stack handed out and has not popped yet. Under strict LIFO
// the block being popped is always in the TOP segment (a segment is retired
// the moment its first block goes), so one segment is enough to ask. A
// generator frame (inside a JSAsyncFunctionState) can never satisfy this:
// that range is owned by pushed frames only.
static inline int js_vm_stack_holds(const JSVMStack *st, const void *p)
{
    const JSVMSeg *s = st->cur;
    return s && (const uint8_t *)p >= s->base && (const uint8_t *)p < s->top;
}

// `block` must be the most recent push still outstanding (LIFO). Retires the
// segment when the block was its first: a standard one goes to the cache
// while there is room, anything else (a dedicated one, or a standard one
// past the cache cap) is returned to the runtime.
static inline void js_vm_stack_pop(JSRuntime *rt, JSVMStack *st, void *block)
{
    JSVMSeg *s = st->cur;
    assert(s && (uint8_t *)block >= s->base && (uint8_t *)block < s->top);
    st->used -= (size_t)(s->top - (uint8_t *)block);
#ifdef JS_VM_STACK_STATS
    st->depth--;
#endif
    JS_VM_POISON(block, (size_t)(s->top - (uint8_t *)block));
    s->top = (uint8_t *)block;
    if (unlikely(s->top == s->base)) {
        // The bottom standard segment stays resident: it is the one every
        // later call at depth 1 would immediately re-create. s->standard,
        // not a payload comparison -- payload alone no longer identifies
        // "standard" now that the sequence has more than one legal value
        // (design D42). A dedicated bottom (only possible if the very first
        // frame ever pushed exceeded seg_max) has no depth-1 case to serve
        // by staying around, so it is freed like any other dedicated
        // segment below.
        if (!s->prev && s->standard)
            return;
        st->cur = s->prev;
#ifdef JS_VM_STACK_STATS
        st->seg_live--;
        st->resident -= js_vm_seg_payload(s) + js_vm_seg_overhead();
#endif
        if (s->standard && st->cache_n < st->cache_max) {
            s->prev = st->cache;
            st->cache = s;
            st->cache_n++;
#ifdef JS_VM_STACK_STATS
            st->cached += js_vm_seg_payload(s) + js_vm_seg_overhead();
#endif
        } else {
            js_vm_seg_del(rt, st, s);
        }
    }
}

// JS_FreeRuntime: no frame may be live (no JS runs during teardown).
static inline void js_vm_stack_free(JSRuntime *rt, JSVMStack *st)
{
    JSVMSeg *s, *p;
    for (s = st->cur; s; s = p) {
        assert(s->top == s->base);
        p = s->prev;
        js_vm_seg_del(rt, st, s);
    }
    for (s = st->cache; s; s = p) {
        p = s->prev;
        js_vm_seg_del(rt, st, s);
    }
    st->cur = st->cache = NULL;
    st->cache_n = 0;
#ifdef JS_VM_STACK_STATS
    st->cached = 0;
#endif
}

// D43: JS_VMStackTrim. Frees only the cache -- empty segments no frame is
// in -- so it is safe with a live chain; the resident bottom segment stays.
static inline void js_vm_stack_trim(JSRuntime *rt, JSVMStack *st)
{
    JSVMSeg *s, *p;
    if (!st->cache)
        return;
    for (s = st->cache; s; s = p) {
        p = s->prev;
        js_vm_seg_del(rt, st, s);
    }
    st->cache = NULL;
    st->cache_n = 0;
#ifdef JS_VM_STACK_STATS
    st->cached = 0;
    st->trims++;
#endif
}

// ---------------------------------------------------------------- L3a
//
// Moving the segments (docs/vm/vm-L3-design.md; the ledger of everything that
// points into them is docs/vm/vm-ledger/09-relocation-entries.md).
//
// This half does the memory: allocate the new blocks, copy the live bytes,
// hand back a table of "what moved where", and later poison and free the old
// ones. It deliberately does NOT know what a JSStackFrame is -- the typed
// fix-up walk over the ledger's 14 entries lives in quickjs.c, which does.
// Splitting it here is what keeps this header's promise (layout plus bump
// allocator, no VM semantics) intact for a stage whose whole subject is VM
// semantics.
#ifdef CONFIG_POCKET_VM_RELOC

// One moved segment. `delta` is what a pointer INTO this segment's payload
// gains; the pair of bounds is how a pointer is recognised as belonging here
// at all. Per segment, never one figure for the whole stack: the live chain
// is several separately malloc'd blocks whose new addresses have no reason to
// share an offset (spec sec.8, "do not express different displacements with a
// single addend at the root"). `old_seg` is the block the move has to give
// back once nothing names it any more -- stage 5, after the fix-up, not before.
typedef struct JSVMReloc {
    uint8_t *old_base;
    uint8_t *old_end;
    ptrdiff_t delta;
    JSVMSeg *old_seg;
} JSVMReloc;

// The lookup that stands in for "is this a pointer into a moved segment".
// Linear: the table has one row per LIVE segment, and JS_VM_SEG_MAX keeps
// that count to a few dozen even at the device's whole stack budget (the same
// bound js_vm_stack_push_slow relies on for its prev-walk). A pointer that
// matches no row is left alone -- that is not a failure, it is how a partial
// move (one segment, not the chain) stays legal (D53).
static inline void *js_vm_reloc_ptr(const JSVMReloc *tab, uint32_t n, void *p)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)p;
    if (!p)
        return p;
    for (i = 0; i < n; i++) {
        // The end is inclusive here, unlike js_vm_stack_holds: a frame's
        // cur_sp / caller_sp legitimately sits one past the last live slot,
        // and an exclusive test would silently leave such a pointer behind
        // whenever the operand stack happened to be full.
        if (b >= tab[i].old_base && b <= tab[i].old_end)
            return b + tab[i].delta;
    }
    return p;
}

// Stages 1-3 (design sec.4): allocate, copy, and relink onto the new blocks.
// The old blocks are still allocated and still readable when this returns 0 --
// only js_vm_stack_reloc_finish gives them up -- so a caller that fails
// between here and there can still read what it needs.
//
// Returns -1 with the VM byte-for-byte unchanged when any allocation fails:
// every new block taken so far is returned first. This is the "keep the old
// region on failure" the spec asks the VM to guarantee, and the reason the
// allocation is all done up front rather than segment by segment during the
// walk -- there is no way to half-rewrite a pointer graph and back out.
static inline int js_vm_stack_reloc_copy(JSRuntime *rt, JSVMStack *st,
                                         JSVMReloc **ptab, uint32_t *pn)
{
    JSVMSeg *s, *ns, *new_top = NULL, *new_prev = NULL;
    JSVMReloc *tab;
    uint32_t n = 0, i;

    *ptab = NULL;
    *pn = 0;
    for (s = st->cur; s; s = s->prev)
        n++;
    if (!n)
        return 0;
    tab = js_malloc_rt(rt, (size_t)n * sizeof(*tab));
    if (!tab)
        return -1;

    // Bottom-up, so the new chain's prev links can be written as they are
    // created: walking st->cur gives top-down, so fill the table from the
    // back and then build upward from row 0.
    i = n;
    for (s = st->cur; s; s = s->prev) {
        i--;
        tab[i].old_seg = s;
        tab[i].old_base = s->base;
        tab[i].old_end = s->end;
    }
    for (i = 0; i < n; i++) {
        size_t payload = js_vm_seg_payload(tab[i].old_seg);
        size_t live = (size_t)(tab[i].old_seg->top - tab[i].old_seg->base);
        ns = js_vm_seg_new(rt, st, payload, tab[i].old_seg->standard);
        if (!ns) {
            // Nothing has been rewritten yet; give back what this attempt
            // took and leave the caller exactly as it was found. The new
            // blocks are already chained through new_prev, so the partial
            // chain is its own undo list -- no second array to keep in step
            // with the table.
            while (new_prev) {
                JSVMSeg *dead = new_prev;
                new_prev = dead->prev;
                js_vm_seg_del(rt, st, dead);
            }
            js_free_rt(rt, tab);
            return -1;
        }
        // Copy only the live prefix: the tail is poisoned in the source
        // under ASan, and copying it would report on the memcpy itself
        // rather than on any real mistake. js_vm_seg_new already poisoned
        // the whole new payload, so unpoison exactly what is being filled.
        JS_VM_UNPOISON(ns->base, live);
        memcpy(ns->base, tab[i].old_seg->base, live);
        ns->top = ns->base + live;
        ns->prev = new_prev;
        new_prev = new_top = ns;
        tab[i].delta = (ptrdiff_t)(ns->base - tab[i].old_base);
    }
    st->cur = new_top;
    *ptab = tab;
    *pn = n;
    return 0;
}

// L4a (docs/vm/vm-L3-design.md sec.10): the same move, but GATHERING the
// chain into one block instead of copying each segment to a block of its
// own size. js_vm_stack_reloc_copy cannot reduce scatter -- it hands back
// exactly the sizes it takes, so the chain ends up as spread out as before,
// just elsewhere (measured on the host: span - resident of a 9-segment chain
// is 23-49 KB of other allocations sitting between the pieces). One block
// has no inside to put anything in.
//
// The fix-up does not change. Each old segment still gets one row in the
// table, and its delta now points at its slice of the new block rather than
// at a block of its own; D46's per-segment deltas were always the general
// case, and this is the first caller that makes them differ in kind.
//
// Only the live prefix of each segment is copied, bottom segment first, so
// the frames keep their LIFO order and every block stays at the
// JS_VM_FRAME_ALIGN it had: the new base is 16-aligned and each slice length
// is a sum of rounded frame sizes. A pointer to one past the end of a lower
// segment's live bytes (an operand stack filled to its last slot) lands on
// the first byte of the next slice -- the same "one past the end" in the new
// layout, which is all such a pointer is ever used for.
//
// The new block is not a member of the size-by-position sequence (standard
// = 0): it is exactly as large as what it holds, so it is never offered to
// the cache and is freed when the stack unwinds past it; the next push
// starts a fresh standard bottom segment as it would from empty. The block
// is exactly full, so the next deeper push opens a segment above it.
//
// Returns 0 with *pn == 0 when there is nothing to gather (fewer than two
// segments): a single segment is already contiguous, and moving it would
// only trade one address for another. -1 leaves the VM untouched.
static inline int js_vm_stack_reloc_coalesce(JSRuntime *rt, JSVMStack *st,
                                             JSVMReloc **ptab, uint32_t *pn)
{
    JSVMSeg *s, *ns;
    JSVMReloc *tab;
    uint32_t n = 0, i;
    size_t total = 0, off = 0, payload;

    *ptab = NULL;
    *pn = 0;
    for (s = st->cur; s; s = s->prev) {
        n++;
        total += (size_t)(s->top - s->base);
    }
    if (n < 2)
        return 0;
    tab = js_malloc_rt(rt, (size_t)n * sizeof(*tab));
    if (!tab)
        return -1;
    i = n;
    for (s = st->cur; s; s = s->prev) {
        i--;
        tab[i].old_seg = s;
        tab[i].old_base = s->base;
        tab[i].old_end = s->end;
    }
    // Never zero: with two or more segments on the chain the top one holds
    // at least one frame (a segment is retired the moment its first block
    // is popped; only a lone bottom segment stays resident empty).
    payload = (total + JS_VM_SEG_ALIGN - 1) & ~(size_t)(JS_VM_SEG_ALIGN - 1);
    ns = js_vm_seg_new(rt, st, payload, 0);
    if (!ns) {
        js_free_rt(rt, tab);
        return -1;
    }
    for (i = 0; i < n; i++) {
        size_t live = (size_t)(tab[i].old_seg->top - tab[i].old_seg->base);
        JS_VM_UNPOISON(ns->base + off, live);
        memcpy(ns->base + off, tab[i].old_seg->base, live);
        tab[i].delta = (ptrdiff_t)((ns->base + off) - tab[i].old_base);
        off += live;
    }
    ns->top = ns->base + off;
    ns->prev = NULL;
    st->cur = ns;
#ifdef JS_VM_STACK_STATS
    // The chain is one segment now; resident is what it holds. The old
    // segments' share leaves `resident` here and their memory leaves the
    // heap in js_vm_stack_reloc_finish. resident_max is a peak and keeps the
    // value it had, and seg_mallocs/seg_frees count the block and the frees.
    st->seg_live = 1;
    st->resident = js_vm_seg_payload(ns) + js_vm_seg_overhead();
#endif
    *ptab = tab;
    *pn = n;
    return 0;
}

// Stage 5: the old blocks stop existing, loudly. Filling them with 0xD3 first
// is what catches a fix-up this design MISSED on builds with no sanitizer --
// 0xD3D3D3D3 is not a valid JSValue tag and not a plausible pointer, so a
// read through a stale reference trips an existing assert or faults, instead
// of finding the bytes still sitting there and appearing to work. That is the
// check the ledger's completeness claim actually rests on (ledger sec.9).
//
// `keep_old` leaves the blocks allocated and poisoned rather than freeing
// them. Diagnostic only, and a deliberate leak: freeing hands the address
// back to tlsf, which can hand it straight out again and overwrite the
// poison with something that reads as valid.
static inline void js_vm_stack_reloc_finish(JSRuntime *rt, JSVMStack *st,
                                            JSVMReloc *tab, uint32_t n,
                                            int keep_old)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        JSVMSeg *s = tab[i].old_seg;
        size_t payload = js_vm_seg_payload(s);
        JS_VM_UNPOISON(s->base, payload);
        memset(s->base, 0xD3, payload);
        if (keep_old) {
            // Unlinked from every chain by the relink in _copy, so nothing
            // walks it and nothing frees it: it stays as a poisoned address
            // range for the rest of the process.
            JS_VM_POISON(s->base, payload);
        } else {
            js_vm_seg_del(rt, st, s);
        }
    }
    js_free_rt(rt, tab);
    st->generation++;
}

#endif /* CONFIG_POCKET_VM_RELOC */

// ---------------------------------------------------------------- L2b
//
// With CONFIG_POCKET_VM_FLATCALLS a JS-to-JS call does not recurse in C:
// JS_CallInternal pushes the callee's block and carries on in the same
// activation, and the callee's return pops it and resumes the caller from
// what the frame chain holds (docs/vm/vm-L2-design.md sec.9). Everything the
// dispatch loop kept in C locals for the caller must then be recoverable
// from the caller's frame. Most of it already is: pc is sf->cur_pc (D8),
// argc is sf->arg_count (D11), the buffers hang off sf, and the return
// fix-up is sf->ret_shape (D8). The one thing that is not is the caller's
// OPERAND STACK POINTER, which no field records and nothing can recompute:
// it goes in this link, in front of the JSStackFrame inside the same pushed
// block (D12). Not in JSStackFrame.cur_sp, which async_func_mark reads as
// "suspended" -- a generator frame calling a flat child is RUNNING, and a
// non-NULL cur_sp would have the GC walk its half-built operand stack.
//
// Every block JS_CallInternal pushes carries the link, floor frames too
// (the floor's is unused; a per-entry-path block layout would cost a branch
// on every pop for 8 bytes on the floor only). Generator frames live in a
// JSAsyncFunctionState and have no link; the JS_SF_SEG bit tells them apart.
#ifdef CONFIG_POCKET_VM_FLATCALLS
typedef struct JSVMLink {
    JSValue *caller_sp;     // the caller's sp at the call: func/this/args still on it
} JSVMLink;
#define JS_VM_FRAME_PREFIX sizeof(JSVMLink)

// JSStackFrame.l2_flags. Four states of two bits (design sec.12.16-3):
//   SEG        a floor entered from C, in a segment block; returns to C
//   SEG|FLAT   a flat SEG frame: pushed by flat_call:, returns to its caller
//              in the same C activation, its caller's sp in the JSVMLink
//   FLAT       a flat async frame (D33): an async function's first
//              synchronous stretch, running in the caller's activation but
//              living in its JSAsyncFunctionData, its caller's sp in
//              JSAsyncFunctionData.flat_caller_sp; returns to its caller
//              at its first await / return / throw and then drops FLAT
//   0          a generator/async floor resumed from C (js_mallocz'd frame,
//              or a flat async frame after its first stretch)
// A frame walker that reads these must still guard on class_id first
// (design D4-3): native frames are uninitialised C automatics.
#define JS_SF_SEG        1u   // pushed on the segment stack; local_buf == (JSValue *)(sf + 1)
#define JS_SF_FLAT       2u   // pushed by a flat call: its return resumes sf->prev_frame in the same C activation
#define JS_SF_MAY_YIELD  4u   // this activation has a C owner that can resume it (D17r)
#define JS_SF_SUSPENDED  8u   // cur_pc/cur_sp describe a parked frame (D18r)
#define JS_SF_OWNS_FUNC 16u   // a yieldable floor owns cur_func across host turns
#define JS_SF_TAIL      32u   // local slots own [func, this, args...] after tail replacement

// JSStackFrame.ret_shape: what the CALLER does with its operand stack when
// this frame returns (design D8 sec.7.3-2). argc << 2 | bits; neither the
// caller's cur_pc (already past a variable-length operand) nor the callee's
// arg_count (declared, not passed) can stand in for it.
#define JS_RET_METHOD 1u   // call_method: a `this` slot sits below the func slot (drop argc+2, not argc+1)
#define JS_RET_TAIL   2u   // tail_call: the caller returns the value itself (upstream's `goto done`)
#define JS_RET_SHAPE(argc, bits) (((uint32_t)(argc) << 2) | (bits))
#define JS_RET_ARGC(shape) ((int)((shape) >> 2))

_Static_assert(sizeof(JSVMLink) % JS_VM_FRAME_ALIGN == 0,
               "the link must keep the JSStackFrame behind it frame-aligned");
#else
#define JS_VM_FRAME_PREFIX 0
#endif

// Defined in quickjs.c: the runtime's stack, or NULL when the build keeps
// frames on the C stack (CONFIG_POCKET_VM_SEGFRAMES off).
JSVMStack *js_vm_stack_get(JSRuntime *rt);

#ifdef __cplusplus
}
#endif
