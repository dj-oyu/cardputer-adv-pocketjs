#include "flower_d1_species_probe.h"
#include "flower.h"
#include "flower_parts.h"
#include "scene_mem.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdint.h>

/* The corresponding host frames are printed by tools/test_flower_frame.c.
 * This table deliberately freezes the 2026-09-28 O2 reference: a changed
 * botanical or math kernel must be reviewed on both host and device. */
static const uint32_t host_hash[FLOWER_SPECIES_COUNT] = {
    0x1069ac08u, 0x36baae55u, 0xe23e8db2u, 0x52d3a32du,
    0x693c4f95u, 0xf9ee9882u, 0xf4467ec8u, 0xb586b2a8u,
    0x659da3b5u, 0xf7d7eefcu, 0xdb653294u, 0xb76c93c9u,
    0xb15809f9u, 0x3a3c41b4u,
};
static const char *const names[FLOWER_SPECIES_COUNT] = {
    "valley", "sunflower", "snowdrop", "tulip", "daffodil", "crocus",
    "calla", "platycodon", "echinacea", "anemone", "nigella", "aquilegia",
    "fritillaria", "iris",
};

enum { HOLD_FRAMES = 30, BAND_ROWS = 8, WIDTH = 240, HEIGHT = 135 };
/* The ESP32-S3 PIE store silently rounds an unaligned address down to 16 B.
 * A 240-pixel row preserves alignment once this base is aligned. */
static uint16_t pixels[WIDTH * BAND_ROWS] __attribute__((aligned(16)));
static uint32_t expected_rows[HEIGHT];
static flower_frame *lease;
static unsigned frame_index, passes, failures;
static bool reported;
static unsigned failure_stage;
static int failure_row;
static uint32_t failure_expected, failure_actual;

static uint32_t hash_bytes(uint32_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * UINT32_C(16777619);
    return hash;
}

static void fixed_prepare(unsigned species)
{
    /* Exact fixture inputs from tools/test_flower_frame.c: odd species use
     * the second frame of each two-species pair. */
    elapsed = 3.5f + (float)(species & ~1u);
    flower_grain_frame = (species & ~1u) * 13u + (species & 1u);
    flower_prepare(.033f, 0, 0, (flower_species_t)species);
}

static bool probe_one(unsigned species, uint32_t *legacy_hash)
{
    failure_stage = 0;
    failure_row = -1;
    failure_expected = failure_actual = 0;
    scene_mem_release();
    fixed_prepare(species);
    if (!lease) lease = flower_frame_create();
    if (!lease) { failure_stage = 1; return false; }

    uint32_t hash = UINT32_C(2166136261);
    for (int y = 0; y < HEIGHT; y += BAND_ROWS) {
        int h = HEIGHT - y < BAND_ROWS ? HEIGHT - y : BAND_ROWS;
        flower_draw(pixels, y, h);
        for (int row = 0; row < h; ++row) {
            const uint16_t *p = pixels + row * WIDTH;
            expected_rows[y + row] = hash_bytes(UINT32_C(2166136261),
                                                 p, WIDTH * sizeof(*p));
        }
        hash = hash_bytes(hash, pixels, (size_t)WIDTH * h * sizeof(*pixels));
    }
    *legacy_hash = hash;
    if (!flower_frame_capture(lease)) { failure_stage = 2; return false; }

    /* Check the lease before changing the live scene, then after replacement
     * and finally after eviction. This localizes any device-only mismatch. */
    for (unsigned stage = 3; stage <= 5; ++stage) {
        if (stage == 4)
            flower_prepare(0, 0, 0,
                           (flower_species_t)((species + 1u) % FLOWER_SPECIES_COUNT));
        if (stage == 5) scene_mem_release();
        for (int bottom = HEIGHT; bottom > 0;) {
            int h = bottom < BAND_ROWS ? bottom : BAND_ROWS;
            bottom -= h;
            flower_frame_draw(lease, pixels, bottom, h);
            for (int row = 0; row < h; ++row) {
                uint32_t got = hash_bytes(UINT32_C(2166136261), pixels + row * WIDTH,
                                          WIDTH * sizeof(*pixels));
                uint32_t want = expected_rows[bottom + row];
                if (got != want) {
                    failure_stage = stage;
                    failure_row = bottom + row;
                    failure_expected = want;
                    failure_actual = got;
                    return false;
                }
            }
        }
    }
    /* A second draw detects a lease whose first draw modified retained data. */
    for (int y = 0; y < HEIGHT; y += BAND_ROWS) {
        int h = HEIGHT - y < BAND_ROWS ? HEIGHT - y : BAND_ROWS;
        flower_frame_draw(lease, pixels, y, h);
        for (int row = 0; row < h; ++row)
            if (hash_bytes(UINT32_C(2166136261), pixels + row * WIDTH,
                           WIDTH * sizeof(*pixels)) != expected_rows[y + row]) {
                failure_stage = 6;
                failure_row = y + row;
                return false;
            }
    }
    return true;
}

bool flower_d1_species_prepare(void)
{
    unsigned species = frame_index / HOLD_FRAMES;
    if (species >= FLOWER_SPECIES_COUNT) {
        if (!reported) {
            ESP_LOGI("FLOWER_D1", "SUMMARY passes=%u failures=%u count=%u",
                     passes, failures, (unsigned)FLOWER_SPECIES_COUNT);
            flower_frame_destroy(lease);
            lease = NULL;
            reported = true;
        }
        return false;
    }
    if (frame_index % HOLD_FRAMES == 0) {
        uint32_t hash = 0;
        int64_t started = esp_timer_get_time();
        bool lease_ok = probe_one(species, &hash);
        bool host_ok = hash == host_hash[species];
        if (lease_ok && host_ok) ++passes;
        else ++failures;
        ESP_LOGI("FLOWER_D1",
                 "SPECIES id=%u name=%s hash=%08lx host=%08lx lease=%u host_match=%u "
                 "stage=%u row=%d want=%08lx got=%08lx "
                 "us=%lu bytes=%lu free=%lu largest=%lu",
                 species, names[species], (unsigned long)hash,
                 (unsigned long)host_hash[species], (unsigned)lease_ok,
                 (unsigned)host_ok, failure_stage, failure_row,
                 (unsigned long)failure_expected, (unsigned long)failure_actual,
                 (unsigned long)(esp_timer_get_time() - started),
                 (unsigned long)flower_frame_bytes(lease),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    /* Prepare the same fixed shot once after the destructive lease check.
     * The shell keeps capturing/presenting it for the remaining hold frames;
     * repeated prepare would advance GardenFrame's mote state. */
    if (frame_index % HOLD_FRAMES == 0) fixed_prepare(species);
    ++frame_index;
    return true;
}
