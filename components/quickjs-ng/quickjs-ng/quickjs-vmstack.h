// L2a: bytecode-function frames in runtime-owned, non-moving segments
// (docs/quickjs-freertos-vm-spec.md sec.7 "L2a", docs/vm-L2-design.md sec.3).
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
// early -- spec sec.14.5: "L2a moves the JSStackFrame itself, not only the
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
// Segment size: JS_VM_SEG_SIZE bytes of payload (see the note at its
// definition -- ledger 07's 4096 was chosen on OBJECT allocation histories,
// not frame lifetimes). A frame larger than that gets a segment of exactly its
// own size (a "dedicated" segment), freed on pop and never cached, the same
// rule tools/vmalloc/adapter_segment.c follows. When the standard segment
// cannot be obtained (memory limit) the push retries with a dedicated segment
// sized to the frame alone, so that a call fails only when the frame ITSELF
// does not fit -- the boundary the alloca path had -- rather than whenever
// 4 KiB happens not to be available.
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

#define JS_VM_SEG_ALIGN 16

#if defined(ESP_PLATFORM)
#define JS_VM_FRAME_ALIGN 4
#else
#define JS_VM_FRAME_ALIGN 8
#endif

// Standard segment payload. 4096 is ledger 07's figure for the general
// allocator; for frames the number that matters is "frames per segment":
// on the target a small function's frame is 48 + 8 * (args + vars + stack)
// bytes, ~136 B for a typical one (計算値), so 4096 holds ~30 nesting levels
// before a second segment is needed, and one segment is the resident cost
// paid from the first call to JS_FreeRuntime. "#info vmstack" from vmrun
// (--stats) reports what the corpus actually needed; see the L2a report.
#ifndef JS_VM_SEG_SIZE
#define JS_VM_SEG_SIZE 4096
#endif
// Empty standard segments kept for reuse instead of being returned. One is
// enough to absorb a call depth oscillating across a segment boundary
// without a malloc/free pair per crossing; more only pins memory.
#ifndef JS_VM_SEG_CACHE_MAX
#define JS_VM_SEG_CACHE_MAX 1
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
} JSVMSeg;

typedef struct JSVMStack {
    JSVMSeg *cur;           // topmost segment; NULL until the first push
    JSVMSeg *cache;         // empty standard segments kept for reuse, linked by prev
    uint32_t cache_n;
    uint32_t cache_max;
    size_t seg_size;        // standard payload size
#ifdef JS_VM_STACK_STATS
    size_t live_bytes, live_bytes_max;   // frame bytes in use (rounded sizes)
    size_t frame_max;                    // largest single frame pushed
    uint32_t depth, depth_max;           // frames in use
    uint32_t seg_live, seg_live_max;     // segments on the chain
    uint64_t pushes, seg_mallocs, seg_frees, seg_reuses, dedicated, fallbacks;
#endif
} JSVMStack;

static inline void js_vm_stack_init(JSVMStack *st)
{
    memset(st, 0, sizeof(*st));
    st->seg_size = JS_VM_SEG_SIZE;
    st->cache_max = JS_VM_SEG_CACHE_MAX;
}

// Only before the first push: a live chain built on one size cannot be
// re-described with another (the cache test in pop compares against it).
static inline int js_vm_stack_configure(JSVMStack *st, size_t seg_size, uint32_t cache_max)
{
    if (st->cur || st->cache || seg_size < JS_VM_SEG_ALIGN)
        return -1;
    st->seg_size = (seg_size + JS_VM_SEG_ALIGN - 1) & ~(size_t)(JS_VM_SEG_ALIGN - 1);
    st->cache_max = cache_max;
    return 0;
}

static inline size_t js_vm_seg_payload(const JSVMSeg *s)
{
    return (size_t)(s->end - s->base);
}

static inline JSVMSeg *js_vm_seg_new(JSRuntime *rt, JSVMStack *st, size_t payload)
{
    JSVMSeg *s = js_malloc_rt(rt, sizeof(JSVMSeg) + (JS_VM_SEG_ALIGN - 1) + payload);
    if (!s)
        return NULL;
    uintptr_t b = ((uintptr_t)(s + 1) + JS_VM_SEG_ALIGN - 1) & ~(uintptr_t)(JS_VM_SEG_ALIGN - 1);
    s->base = s->top = (uint8_t *)b;
    s->end = s->base + payload;
    s->prev = NULL;
    JS_VM_POISON(s->base, payload);
#ifdef JS_VM_STACK_STATS
    st->seg_mallocs++;
    if (payload != st->seg_size)
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
    if (size <= st->seg_size) {
        if (st->cache) {
            s = st->cache;
            st->cache = s->prev;
            st->cache_n--;
#ifdef JS_VM_STACK_STATS
            st->seg_reuses++;
#endif
        } else {
            s = js_vm_seg_new(rt, st, st->seg_size);
            if (unlikely(!s)) {
                s = js_vm_seg_new(rt, st, size);
                if (!s)
                    return NULL;
#ifdef JS_VM_STACK_STATS
                st->fallbacks++;
#endif
            }
        }
    } else {
        s = js_vm_seg_new(rt, st, size);
        if (!s)
            return NULL;
    }
    s->prev = st->cur;
    st->cur = s;
#ifdef JS_VM_STACK_STATS
    st->seg_live++;
    if (st->seg_live > st->seg_live_max)
        st->seg_live_max = st->seg_live;
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
    size = (size + JS_VM_FRAME_ALIGN - 1) & ~(size_t)(JS_VM_FRAME_ALIGN - 1);
#ifdef JS_VM_STACK_STATS
    st->pushes++;
    st->depth++;
    if (st->depth > st->depth_max)
        st->depth_max = st->depth;
    st->live_bytes += size;
    if (st->live_bytes > st->live_bytes_max)
        st->live_bytes_max = st->live_bytes;
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
#ifdef JS_VM_STACK_STATS
    if (!p) {
        st->depth--;
        st->live_bytes -= size;
    }
#endif
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
#ifdef JS_VM_STACK_STATS
    st->depth--;
    st->live_bytes -= (size_t)(s->top - (uint8_t *)block);
#endif
    JS_VM_POISON(block, (size_t)(s->top - (uint8_t *)block));
    s->top = (uint8_t *)block;
    if (unlikely(s->top == s->base)) {
        int standard = js_vm_seg_payload(s) == st->seg_size;
        // The bottom standard segment stays resident: it is the one every
        // later call would immediately re-create.
        if (!s->prev && standard)
            return;
        st->cur = s->prev;
#ifdef JS_VM_STACK_STATS
        st->seg_live--;
#endif
        if (standard && st->cache_n < st->cache_max) {
            s->prev = st->cache;
            st->cache = s;
            st->cache_n++;
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
}

// Defined in quickjs.c: the runtime's stack, or NULL when the build keeps
// frames on the C stack (CONFIG_POCKET_VM_SEGFRAMES off).
JSVMStack *js_vm_stack_get(JSRuntime *rt);

#ifdef __cplusplus
}
#endif
