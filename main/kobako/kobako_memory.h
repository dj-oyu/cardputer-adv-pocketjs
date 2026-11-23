#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"

enum {
    KOBAKO_MEMORY_GUEST = 1u,
    KOBAKO_MEMORY_FREE = 2u,
    KOBAKO_MEMORY_LARGEST = 4u,
    KOBAKO_MEMORY_FAILURE = 8u,
};

/* One listener belongs to one JS guest. Install after kobako_api_install();
 * reset before freeing its context. All functions except the native failure
 * hook run on the JS owner task. */
esp_err_t kobako_memory_install(JSContext *ctx, void *user);
void kobako_memory_reset(void);

/* A native allocator's failure hook may call this from any task. It only
 * touches bounded atomics; it never allocates, formats, or enters JS. */
void kobako_memory_native_failure(size_t requested, uint32_t caps);

/* The app-session owner calls these after draining the QuickJS canary once
 * and at turn boundaries, including turns used only for display. */
void kobako_memory_oom(const JSOOMCanary *canary, uint64_t now_us);
bool kobako_memory_native_sample_due(uint64_t now_us);
void kobako_memory_sample(uint64_t now_us, size_t guest_used, size_t guest_limit,
                          bool native_valid, size_t internal_free,
                          size_t internal_largest);
void kobako_memory_pump(bool leaving);
/* The pressure bits (KOBAKO_MEMORY_*) in force now. */
uint8_t kobako_memory_mask(void);
