/* Character store: unpacks the embedded HCZ containers into the ay_chars
 * partition once, then maps one character at a time.
 *
 * The characters are tens of times larger than anything else the Companion
 * keeps in RAM, and they compress about 3:1, so the app image carries them
 * deflated and this file turns them into plain HCA images that the cache can
 * read directly. The partition is the last 1.4 MiB of flash, which the
 * source store never used.
 *
 * Layout: the first 64 KiB page is a directory; each character then starts on
 * a 64 KiB boundary (the flash MMU page), so it can be mapped on its own.
 * The directory is written last, so a power cut leaves the partition
 * unusable (and reinstalled next time) rather than half-valid. */

#include "track_character.h"

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"

#define TAG "ay_store"
#define PAGE 0x10000u
#define SECTOR 4096u
#define DIR_MAGIC 0x48435941u /* "AYCH" */
#define MAX_ENTRIES 16

typedef struct { uint32_t raw_size, crc, offset, spare; } entry_t;
typedef struct {
    uint32_t magic, version, count, spare;
    entry_t entry[MAX_ENTRIES];
    uint32_t crc; /* of everything above */
} directory_t;
_Static_assert(sizeof(directory_t) <= SECTOR, "directory must fit one sector");

typedef struct { uint32_t raw_size, crc; const uint8_t *deflate; size_t deflate_size; } container_t;

static const esp_partition_t *partition;
static bool checked, ready;
static const void *mapped;
static esp_partition_mmap_handle_t handle;
static unsigned mapped_blob = UINT_MAX;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t pages(uint32_t bytes) { return (bytes + PAGE - 1) / PAGE * PAGE; }

static bool container(unsigned blob, container_t *out)
{
    if (blob >= ay_character_blob_count) return false;
    const uint8_t *p = ay_character_blobs[blob].begin;
    size_t size = (size_t)(ay_character_blobs[blob].end - p);
    if (size < 12 || memcmp(p, "HCZ1", 4)) return false;
    out->raw_size = rd32(p + 4);
    out->crc = rd32(p + 8);
    out->deflate = p + 12;
    out->deflate_size = size - 12;
    return out->raw_size >= 144;
}

/* Where each blob goes, in order. False when the set cannot fit. */
static bool plan(entry_t *entries, unsigned *count)
{
    if (!partition || ay_character_blob_count > MAX_ENTRIES) return false;
    uint32_t cursor = PAGE;
    for (unsigned i = 0; i < ay_character_blob_count; ++i) {
        container_t c;
        if (!container(i, &c)) return false;
        entries[i] = (entry_t){c.raw_size, c.crc, cursor, 0};
        cursor += pages(c.raw_size);
        if (cursor > partition->size) return false;
    }
    *count = ay_character_blob_count;
    return true;
}

static bool directory_matches(const entry_t *want, unsigned count)
{
    directory_t dir;
    if (esp_partition_read(partition, 0, &dir, sizeof dir) != ESP_OK) return false;
    if (dir.magic != DIR_MAGIC || dir.version != 1 || dir.count != count) return false;
    if (mz_crc32(0, (const mz_uint8 *)&dir, offsetof(directory_t, crc)) != dir.crc) return false;
    return memcmp(dir.entry, want, count * sizeof *want) == 0;
}

typedef struct {
    uint8_t *stage;
    uint32_t written, fill, crc;
    bool failed;
} sink_t;

static int sink(const void *data, int length, void *user)
{
    sink_t *s = user;
    const uint8_t *p = data;
    while (length > 0 && !s->failed) {
        uint32_t take = SECTOR - s->fill;
        if (take > (uint32_t)length) take = (uint32_t)length;
        memcpy(s->stage + s->fill, p, take);
        s->crc = (uint32_t)mz_crc32(s->crc, p, take);
        s->fill += take;
        p += take;
        length -= (int)take;
        if (s->fill == SECTOR) {
            s->failed = esp_partition_write(partition, s->written, s->stage, SECTOR) != ESP_OK;
            s->written += SECTOR;
            s->fill = 0;
            vTaskDelay(1); /* the watchdog and the idle task still have to run */
        }
    }
    return !s->failed;
}

/* Streams the deflate data through a 32 KiB window, the documented non-allocating
 * way to use tinfl: the window is also the output buffer, flushed as it fills. */
static bool unpack(const container_t *c, uint32_t offset)
{
    sink_t s = {.stage = malloc(SECTOR), .written = offset};
    tinfl_decompressor *state = malloc(sizeof *state);
    uint8_t *window = malloc(TINFL_LZ_DICT_SIZE);
    bool good = false;
    int status = TINFL_STATUS_FAILED;
    if (s.stage && state && window) {
        tinfl_init(state);
        size_t in_used = 0, out_used = 0;
        do {
            size_t in_bytes = c->deflate_size - in_used;
            size_t out_bytes = TINFL_LZ_DICT_SIZE - out_used;
            status = tinfl_decompress(state, c->deflate + in_used, &in_bytes, window, window + out_used, &out_bytes, 0);
            in_used += in_bytes;
            if (out_bytes && !sink(window + out_used, (int)out_bytes, &s)) break;
            out_used = (out_used + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
        } while (status > TINFL_STATUS_DONE);
        good = status == TINFL_STATUS_DONE && !s.failed;
        if (good && s.fill) {
            memset(s.stage + s.fill, 0xff, SECTOR - s.fill);
            good = esp_partition_write(partition, s.written, s.stage, SECTOR) == ESP_OK;
            s.written += SECTOR;
        }
        good = good && s.written - offset >= c->raw_size && s.crc == c->crc;
    }
    if (!good) ESP_LOGE(TAG, "unpack failed: status=%d io=%d crc=%08" PRIx32 " want=%08" PRIx32,
                        status, !s.failed, s.crc, c->crc);
    free(s.stage);
    free(state);
    free(window);
    return good;
}

static void open_partition(void)
{
    if (checked) return;
    checked = true;
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x43, "ay_chars");
    if (!partition) {
        ESP_LOGW(TAG, "no ay_chars partition");
        return;
    }
    entry_t want[MAX_ENTRIES];
    unsigned count;
    ready = plan(want, &count) && directory_matches(want, count);
    ESP_LOGI(TAG, "%u characters, %s", ay_character_blob_count, ready ? "already unpacked" : "need unpacking");
}

bool ay_character_store_ready(void)
{
    open_partition();
    return ready;
}

bool ay_character_store_install(void)
{
    open_partition();
    if (ready) return true;
    entry_t want[MAX_ENTRIES];
    unsigned count;
    if (!plan(want, &count)) {
        ESP_LOGE(TAG, "characters do not fit the partition");
        return false;
    }
    ay_character_image_release();
    /* Invalidate first: the directory is the last thing written. */
    if (esp_partition_erase_range(partition, 0, SECTOR) != ESP_OK) return false;
    for (unsigned i = 0; i < count; ++i) {
        container_t c;
        if (!container(i, &c)) return false;
        const uint32_t span = (c.raw_size + SECTOR - 1) / SECTOR * SECTOR;
        if (esp_partition_erase_range(partition, want[i].offset, span) != ESP_OK || !unpack(&c, want[i].offset))
            return false;
        ESP_LOGI(TAG, "%s: %" PRIu32 " bytes at 0x%" PRIx32, ay_character_blobs[i].name, c.raw_size, want[i].offset);
    }
    directory_t dir = {.magic = DIR_MAGIC, .version = 1, .count = count};
    memcpy(dir.entry, want, count * sizeof *want);
    dir.crc = (uint32_t)mz_crc32(0, (const mz_uint8 *)&dir, offsetof(directory_t, crc));
    uint8_t *sector = malloc(SECTOR);
    if (!sector) return false;
    memset(sector, 0xff, SECTOR);
    memcpy(sector, &dir, sizeof dir);
    ready = esp_partition_write(partition, 0, sector, SECTOR) == ESP_OK;
    free(sector);
    return ready;
}

void ay_character_image_release(void)
{
    if (mapped) esp_partition_munmap(handle);
    mapped = NULL;
    mapped_blob = UINT_MAX;
}

const uint8_t *ay_character_image(unsigned blob, size_t *size)
{
    if (!ay_character_store_ready() && !ay_character_store_install()) return NULL;
    entry_t want[MAX_ENTRIES];
    unsigned count;
    if (!plan(want, &count) || blob >= count) return NULL;
    if (mapped && mapped_blob == blob) {
        *size = want[blob].raw_size;
        return mapped;
    }
    ay_character_image_release();
    if (esp_partition_mmap(partition, want[blob].offset, pages(want[blob].raw_size), ESP_PARTITION_MMAP_DATA,
                           &mapped, &handle) != ESP_OK) {
        mapped = NULL;
        return NULL;
    }
    mapped_blob = blob;
    *size = want[blob].raw_size;
    return mapped;
}
