#include "flower_d1_pressure_probe.h"
#include "flower_parts.h"
#include "garden.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdint.h>

enum {
    CAP = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
    FREE_FLOOR = 65536,
    CHUNK = 4096,
    MAX_BLOCKS = 128,
};
static void *blocks[MAX_BLOCKS];
static unsigned held_count;
static bool active;
static uint16_t hash_strip[240 * 8] __attribute__((aligned(16)));

static size_t free_bytes(void) { return heap_caps_get_free_size(CAP); }
static size_t largest_bytes(void) { return heap_caps_get_largest_free_block(CAP); }

size_t flower_d1_candidate_allocation_bytes(void)
{
    return (size_t)count * sizeof(Petal) + 32u * 32u +
           sizeof(GardenFrame) + 15u;
}

uint32_t flower_d1_committed_hash(const flower_frame *frame,
                                  const glass_rain_frame *rain)
{
    if (!frame || !rain) return 0;
    uint32_t hash = UINT32_C(2166136261);
    for (int y = 0; y < 135; y += 8) {
        int h = 135 - y < 8 ? 135 - y : 8;
        flower_frame_draw(frame, hash_strip, y, h);
        glass_rain_draw_frame(rain, hash_strip, y, h);
        const uint8_t *bytes = (const uint8_t *)hash_strip;
        for (size_t i = 0; i < (size_t)240 * h * sizeof(uint16_t); ++i)
            hash = (hash ^ bytes[i]) * UINT32_C(16777619);
    }
    return hash;
}

void flower_d1_pressure_end(void)
{
    if (!active && !held_count) return;
    for (unsigned i = 0; i < held_count; ++i) {
        if (blocks[i]) heap_caps_free(blocks[i]);
        blocks[i] = NULL;
    }
    held_count = 0;
    active = false;
    ESP_LOGI("FLOWER_D1_HEAP", "RELEASE free=%lu largest=%lu",
             (unsigned long)free_bytes(), (unsigned long)largest_bytes());
}

bool flower_d1_pressure_begin(size_t candidate_block_bytes)
{
    flower_d1_pressure_end();
    size_t before_free = free_bytes(), before_largest = largest_bytes();
    /* Room for shell, logging, repair and audio remains available at every
     * step. Refuse to run when the ordinary workload is already near it. */
    if (candidate_block_bytes < 1024 || before_free < FREE_FLOOR + 2 * CHUNK) {
        ESP_LOGW("FLOWER_D1_HEAP",
                 "SKIP required=%lu free=%lu largest=%lu floor=%u",
                 (unsigned long)candidate_block_bytes,
                 (unsigned long)before_free, (unsigned long)before_largest,
                 (unsigned)FREE_FLOOR);
        return false;
    }
    active = true;

    /* Lay small occupied blocks through the large region, then release every
     * other one. This recovers usable total free space as separated holes. */
    while (held_count < MAX_BLOCKS / 2 && free_bytes() >= FREE_FLOOR + CHUNK + 1024) {
        void *p = heap_caps_malloc(CHUNK, CAP);
        if (!p) break;
        blocks[held_count++] = p;
    }
    for (unsigned i = 0; i < held_count; i += 2) {
        heap_caps_free(blocks[i]);
        blocks[i] = NULL;
    }

    /* Trim each still-large contiguous tail. The guard leaves at least 64 KiB
     * free even at the worst point; a device with too little spare heap simply
     * reports SKIP and returns to the ordinary frame path. */
    for (unsigned tries = 0; tries < 8 && largest_bytes() >= candidate_block_bytes;
         ++tries) {
        size_t largest = largest_bytes();
        size_t free_now = free_bytes();
        if (free_now <= FREE_FLOOR) break;
        size_t room = free_now - FREE_FLOOR;
        size_t amount = largest - candidate_block_bytes + 1024;
        if (amount < CHUNK) amount = CHUNK;
        if (amount > room || held_count >= MAX_BLOCKS) break;
        void *p = heap_caps_malloc(amount, CAP);
        if (!p) break;
        blocks[held_count++] = p;
    }
    bool ready = largest_bytes() < candidate_block_bytes;
    ESP_LOGI("FLOWER_D1_HEAP",
             "PRESSURE ready=%u required=%lu held=%u free_before=%lu "
             "largest_before=%lu free=%lu largest=%lu floor=%u",
             (unsigned)ready, (unsigned long)candidate_block_bytes, held_count,
             (unsigned long)before_free, (unsigned long)before_largest,
             (unsigned long)free_bytes(), (unsigned long)largest_bytes(),
             (unsigned)FREE_FLOOR);
    if (!ready) flower_d1_pressure_end();
    return ready;
}
