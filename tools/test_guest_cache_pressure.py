#!/usr/bin/env python3
"""Host-check actual pressure/owner wiring with scripted native heap samples.

Compiles selected production functions, the real pocket_memory state machine,
QuickJS counters, and block_cache. The RTOS task identity and heap samples are
stubs: this does not test the device allocator, scheduling, or memory gains.
Build the QuickJS host objects first with tools/build_memory_test.sh.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path(os.environ.get("CACHE", "/tmp/qjs-kasane-host"))


def function(path, name):
    text = (ROOT / path).read_text()
    match = re.search(r"(?ms)^(?:static )?(?:inline )?(?:void|bool) "
                      + re.escape(name) + r"\([^;{]*\{.*?^}", text)
    if match is None:
        raise RuntimeError(f"function body not found: {path}: {name}")
    return match.group(0)


C = r'''
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pocketjs/guest.h"
#include "pocketjs/block_cache.h"
#include "pocket_memory.h"

static size_t native_free = 50000, native_largest = 30000;
static int task = 1;
static uint64_t clock_us = 1000000;
static unsigned backend_live;
typedef struct { size_t size; size_t pad; } header_t;
static void *allocate(void *ctx, size_t size) {
    (void)ctx;
    header_t *h = malloc(sizeof(*h) + size);
    assert(h);
    h->size = size;
    backend_live++;
    return h + 1;
}
static void release(void *ctx, void *p) {
    (void)ctx;
    backend_live--;
    free((header_t *)p - 1);
}
static size_t length(void *ctx, const void *p) {
    (void)ctx;
    return ((const header_t *)p - 1)->size;
}
static const block_cache_backend_t backend = {allocate, release, length, NULL};
struct pocketjs_guest {
    block_cache_t block_cache;
    int owner_task;
    bool block_cache_on;
    JSContext *ctx;
};
static pocketjs_guest_t *guest;
#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_8BIT 2u
static int xTaskGetCurrentTaskHandle(void) { return task; }
static int64_t esp_timer_get_time(void) { return (int64_t)clock_us; }
static size_t heap_caps_get_free_size(unsigned caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return native_free;
}
static size_t heap_caps_get_largest_free_block(unsigned caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return native_largest;
}
static JSContext *pocketjs_guest_quickjs_context(pocketjs_guest_t *g) { return g->ctx; }
#define ESP_PLATFORM 1
/* Selected production bodies are inserted here, never copied by hand. */
@FUNCTIONS@

static void reuse_once(void) {
    void *p = block_cache_malloc(&guest->block_cache, 48, 48);
    assert(p);
    memset(p, 0x5a, 48);
    block_cache_free(&guest->block_cache, p);
}
int main(void) {
    const uint16_t sizes[] = {48};
    pocketjs_guest_t g = {.owner_task = 1, .block_cache_on = true};
    guest = &g;
    JSRuntime *rt = JS_NewRuntime();
    assert(rt);
    JS_SetMemoryLimit(rt, 163840);
    g.ctx = JS_NewContext(rt);
    assert(g.ctx && pocket_memory_install(g.ctx, NULL) == ESP_OK);
    block_cache_init(&g.block_cache, &backend, sizes, 1, true);
    reuse_once();
    task = 2;
    pocketjs_guest_block_cache_flush(&g);
    pocketjs_guest_block_cache_set_pressure(&g, true);
    assert(g.block_cache.enabled && block_cache_held(&g.block_cache) == 48);
    task = 1;
    sample_memory_pressure();
    assert(pocket_memory_mask() == 0 && g.block_cache.enabled);
    size_t used, limit;
    JS_GetMemoryCounters(rt, &used, &limit);
    JS_SetMemoryLimit(rt, used + 4096);
    clock_us += 100000;
    sample_memory_pressure();
    assert(pocket_memory_mask() == POCKET_MEMORY_GUEST);
    assert(g.block_cache.enabled && block_cache_held(&g.block_cache) == 48);

    native_free = 20948; native_largest = 9216;
    clock_us += 100000;
    sample_memory_pressure();
    assert((pocket_memory_mask() & 6) == 6);
    assert(!g.block_cache.enabled && backend_live == 0);
    for (unsigned i = 0; i < 20; i++) {
        reuse_once();
        clock_us += 1000; // No new native sample yet: pressure must persist.
        sample_memory_pressure();
        assert(!g.block_cache.enabled && backend_live == 0);
    }
    task = 2;
    pocketjs_guest_block_cache_set_pressure(&g, false);
    assert(!g.block_cache.enabled);
    task = 1;
    native_free = 50000; // Largest still pressured, so neither partial nor
    clock_us += 100000; // complete FREE recovery may re-enable retention.
    sample_memory_pressure();
    clock_us += 500000;
    sample_memory_pressure();
    assert((pocket_memory_mask() & 6) == POCKET_MEMORY_LARGEST);
    assert(!g.block_cache.enabled);
    native_largest = 30000;
    clock_us += 100000;
    sample_memory_pressure();
    assert(!g.block_cache.enabled);
    clock_us += 499999;
    sample_memory_pressure();
    assert(!g.block_cache.enabled);
    clock_us += 100000;
    sample_memory_pressure();
    assert(pocket_memory_mask() == POCKET_MEMORY_GUEST && g.block_cache.enabled);
    size_t after_used, after_limit;
    JS_GetMemoryCounters(rt, &after_used, &after_limit);
    assert(after_used == used && after_limit == used + 4096);
    reuse_once();
    assert(block_cache_held(&g.block_cache) == 48);
    pocketjs_guest_block_cache_flush(&g);
    assert(g.block_cache.enabled && backend_live == 0);
    // The wrapper never activates a cache rejected at creation (PSRAM or
    // failed TLSF eligibility). Eligibility is independent of pressure.
    g.block_cache_on = false;
    block_cache_init(&g.block_cache, &backend, sizes, 1, false);
    pocketjs_guest_block_cache_set_pressure(&g, false);
    pocketjs_guest_block_cache_set_pressure(&g, true);
    assert(!g.block_cache.enabled);
    pocketjs_guest_block_cache_set_pressure(NULL, true);
    guest = NULL;
    sample_memory_pressure();
    pocket_memory_reset();
    JS_FreeContext(g.ctx);
    JS_FreeRuntime(rt);
    assert(backend_live == 0);
    puts("GUEST_CACHE_PRESSURE PASS: owner/eligibility/guest-only/sustained/partial-recovery/hysteresis/accounting");
    return 0;
}
'''


def main():
    bodies = "\n\n".join([
        function("components/pocketjs_guest/src/guest.c", "guest_uses_block_cache"),
        function("components/pocketjs_guest/src/guest.c", "pocketjs_guest_block_cache_flush"),
        function("components/pocketjs_guest/src/guest.c", "pocketjs_guest_block_cache_set_pressure"),
        function("main/app_session.c", "sample_memory_pressure"),
    ])
    with tempfile.TemporaryDirectory(prefix="guest-cache-pressure-") as tmp:
        source = Path(tmp) / "binding.c"
        source.write_text(C.replace("@FUNCTIONS@", bodies))
        binary = Path(tmp) / "test"
        args = ["gcc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        for directory in ["components/quickjs-ng/quickjs-ng", "tools/hostshim",
                          "main/pocket", "components/pocketjs_guest/include"]:
            args += ["-I", str(ROOT / directory)]
        args += [str(source), str(ROOT / "main/pocket/pocket_memory.c"),
                 str(ROOT / "components/pocketjs_guest/src/block_cache.c"),
                 str(ROOT / "tools/hostshim/pocket_api_stub.c")]
        args += [str(CACHE / (name + ".o")) for name in
                 ["dtoa", "libregexp", "libunicode", "quickjs", "quickjs-vm"]]
        subprocess.run(args + ["-lm", "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
