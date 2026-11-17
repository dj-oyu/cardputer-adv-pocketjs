#include "ayatsuri_pose_dma_probe.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/board.h"
#include "ui/paint.h"
#include "../../tools/hone/native_geometry/graph_flat_band_candidate.h"
#include "private/assets/ay_pose_dma_fixture.h"

enum {
    WIDTH = 240, HEIGHT = 135, BAND_ROWS = 8, MEASURE_FRAMES = 60,
    MAX_POSE_PACKETS = 576, MAX_POSE_BAND_ENTRIES = 1024,
};
static const int64_t measure_limit_us = INT64_C(12000000);
static const int64_t demo_interval_us = INT64_C(120000);
static const char *const tag = "ay_pose_dma";
static const ksn_mesh_snapshot opaque = {.enabled = true};

static struct {
    uint16_t row[WIDTH];
    uint16_t band[WIDTH * BAND_ROWS];
    float depth[WIDTH];
    float scratch[AY_POSE_DMA_FIXTURE_SCRATCH_COUNT];
    hone_graph_bounds bounds[MAX_POSE_PACKETS];
    uint16_t band_entries[MAX_POSE_BAND_ENTRIES];
} storage;

typedef enum { PREFLIGHT, BENCHMARK, DEMO, FAILED } probe_phase;
static struct {
    uint32_t generation;
    probe_phase phase;
    unsigned checked, completed, flat_completed, demo_pose;
    int64_t started, last_demo, index_us, raster_us, submit_us, finish_us;
    char result[40];
} probe;

static bool prepare_pose(unsigned pose, hone_graph_band_payload *payload,
                         hone_graph_band_work *work, hone_graph_band_index *index,
                         hone_flat_plan *flat)
{
    uint32_t generation = pose + 1;
    *work = (hone_graph_band_work){
        .binding = &storage,
        .generation = generation,
        .row = storage.row,
        .depth = storage.depth,
        .scratch = storage.scratch,
        .scratch_count = AY_POSE_DMA_FIXTURE_SCRATCH_COUNT,
    };
    // Every pose uses the same const packets on every replay. The index borrows
    // this exact payload descriptor for one pose and owns no packet data.
    if (!ay_pose_dma_fixture(pose, &storage, generation, payload) ||
        !hone_graph_index_build(index, &payload->graph, generation,
                                storage.bounds, MAX_POSE_PACKETS,
                                storage.band_entries, MAX_POSE_BAND_ENTRIES) ||
        !hone_graph_index_band_valid(&opaque, payload, index, generation, work,
                                     storage.band, WIDTH * BAND_ROWS)) return false;
    *flat = hone_flat_prepare(payload, index, generation);
    return true;
}

static bool render_pose_row(const hone_graph_band_payload *payload,
                            const hone_flat_plan *flat, hone_graph_band_work *work,
                            unsigned y)
{
    if (hone_flat_available(&opaque, flat)) return hone_flat_row(payload, flat, work, y);
    return hone_graph_block_row(&opaque, payload, flat->index, work, y);
}

static bool check_pose(unsigned pose)
{
    hone_graph_band_payload payload;
    hone_graph_band_work work;
    hone_graph_band_index index;
    hone_flat_plan flat;
    if (!prepare_pose(pose, &payload, &work, &index, &flat) || !payload.graph.count) return false;

    unsigned visible_pixels = 0;
    uint32_t hash = UINT32_C(2166136261);
    for (unsigned y = 0; y < HEIGHT; ++y) {
        if (!render_pose_row(&payload, &flat, &work, y)) return false;
        for (unsigned x = 0; x < WIDTH; ++x) {
            const uint16_t pixel = storage.row[x];
            visible_pixels += pixel != 0;
            hash = (hash ^ pixel) * UINT32_C(16777619);
        }
        if ((y + 1) % BAND_ROWS == 0) vTaskDelay(1);
    }
    ESP_LOGI(tag, "POSE pose=%u packets=%u visible_pixels=%u rgb565_hash=%08" PRIx32 " flat=%u",
             pose, (unsigned)payload.graph.count, visible_pixels, hash,
             (unsigned)hone_flat_available(&opaque, &flat));
    return visible_pixels != 0;
}

static bool present_pose(int pose, const char *label, bool measure)
{
    hone_graph_band_payload payload;
    hone_graph_band_work work;
    hone_graph_band_index index;
    hone_flat_plan flat;
    const int64_t prepared = esp_timer_get_time();
    if (pose >= 0 && !prepare_pose((unsigned)pose, &payload, &work, &index, &flat)) return false;
    if (measure) probe.index_us += esp_timer_get_time() - prepared;

    bool ok = true;
    for (unsigned y = 0; y < HEIGHT; y += BAND_ROWS) {
        const unsigned rows = HEIGHT - y < BAND_ROWS ? HEIGHT - y : BAND_ROWS;
        int64_t started = esp_timer_get_time();
        if (pose < 0) {
            memset(storage.band, 0, rows * sizeof storage.row);
        } else {
            for (unsigned row = 0; row < rows; ++row) {
                if (!render_pose_row(&payload, &flat, &work, y + row)) {
                    ok = false;
                    break;
                }
                memcpy(storage.band + row * WIDTH, storage.row, sizeof storage.row);
            }
        }
        if (!ok) break;

        paint_begin(storage.band, (int)y, (int)rows);
        paint_fill(0, 0, WIDTH, 12, 0x0841);
        paint_ascii(6, 3, "AYATSURI  /  BAKED MODEL", 0xffff);
        paint_fill(0, 113, WIDTH, HEIGHT - 113, 0x0841);
        paint_ascii(6, 115, label, 0xffff);
        paint_ascii(6, 125, "ENTER: RUN AGAIN   BACK: EXIT", 0x9d9f);
        const int64_t rendered = esp_timer_get_time();
        if (measure) probe.raster_us += rendered - started;

        const esp_err_t result = board_present_submit(0, (int)y, WIDTH, (int)rows, storage.band);
        if (measure) probe.submit_us += esp_timer_get_time() - rendered;
        if (result != ESP_OK) {
            ok = false;
            break;
        }
    }

    const int64_t started = esp_timer_get_time();
    const esp_err_t drained = board_present_finish();
    if (measure) probe.finish_us += esp_timer_get_time() - started;
    board_present_frame_end();
    if (measure && ok && drained == ESP_OK && pose >= 0 && hone_flat_available(&opaque, &flat)) {
        ++probe.flat_completed;
    }
    return ok && drained == ESP_OK;
}

static void begin_benchmark(uint32_t generation)
{
    memset(&probe, 0, sizeof probe);
    probe.generation = generation;
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(tag, "BEGIN model_sha256=%s baked_poses=%u renderer=flat_or_block16 live_deformation=false lighting=false "
             "elf_sha_prefix=%02x%02x%02x%02x%02x%02x%02x%02x",
             AY_POSE_DMA_FIXTURE_MODEL_SHA256, (unsigned)AY_POSE_DMA_FIXTURE_COUNT,
             app->app_elf_sha256[0], app->app_elf_sha256[1], app->app_elf_sha256[2],
             app->app_elf_sha256[3], app->app_elf_sha256[4], app->app_elf_sha256[5],
             app->app_elf_sha256[6], app->app_elf_sha256[7]);

}

static void end_benchmark(bool ok, int64_t elapsed_us)
{
    const double fps = elapsed_us > 0 ? probe.completed * 1000000.0 / elapsed_us : 0;
    ok = ok && probe.completed >= AY_POSE_DMA_FIXTURE_COUNT;
    probe.phase = ok ? DEMO : FAILED;
    if (ok) snprintf(probe.result, sizeof probe.result, "%.2f FPS  %u LCD FRAMES", fps, probe.completed);
    else snprintf(probe.result, sizeof probe.result, "BENCHMARK FAILED  %u FRAMES", probe.completed);
    ESP_LOGI(tag, "RESULT status=%s dma_completed_frames=%u elapsed_us=%" PRId64
             " baked_pose_fps=%.3f index_us=%" PRId64 " raster_us=%" PRId64 " submit_us=%" PRId64
             " final_drain_us=%" PRId64 " flat_frames=%u stack_hwm=%u free_internal=%u",
             ok ? "PASS" : "FAIL", probe.completed, elapsed_us, fps,
             probe.index_us, probe.raster_us, probe.submit_us, probe.finish_us, probe.flat_completed,
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void ayatsuri_pose_dma_probe_step(uint32_t generation, bool restart, bool capture)
{
    if (generation != probe.generation || restart) begin_benchmark(generation);

    const int original_async = board_async_get();
    const bool is_capture = capture || board_capture_active();
    if (!is_capture) board_async_set(1);

    if (is_capture) {
        const int pose = probe.phase == DEMO ? (int)probe.demo_pose : -1;
        const char *label = probe.phase >= DEMO ? probe.result : "BENCHMARK RUNNING";
        (void)present_pose(pose, label, false);
        if (probe.phase == BENCHMARK) begin_benchmark(generation);
    } else if (probe.phase == PREFLIGHT) {
        if ((probe.checked == 0 && !present_pose(-1, "CHECKING SIX POSES", false)) ||
            !check_pose(probe.checked)) {
            ESP_LOGE(tag, "FAIL preflight pose=%u", probe.checked);
            end_benchmark(false, 0);
            (void)present_pose(-1, probe.result, false);
        } else if (++probe.checked == AY_POSE_DMA_FIXTURE_COUNT) {
            probe.phase = BENCHMARK;
        }
    } else if (probe.phase == BENCHMARK) {
        if (!probe.completed) probe.started = esp_timer_get_time();
        const unsigned pose = probe.completed % AY_POSE_DMA_FIXTURE_COUNT;
        const bool ok = present_pose((int)pose, "BENCHMARK RUNNING", true);
        if (ok) ++probe.completed;
        const int64_t elapsed = esp_timer_get_time() - probe.started;
        if (!ok || probe.completed == MEASURE_FRAMES || elapsed >= measure_limit_us) {
            end_benchmark(ok, elapsed);
            if (probe.phase == FAILED) (void)present_pose(-1, probe.result, false);
        }
    } else if (probe.phase == DEMO && esp_timer_get_time() - probe.last_demo >= demo_interval_us) {
        if (!present_pose((int)probe.demo_pose, probe.result, false)) {
            ESP_LOGE(tag, "FAIL demo_transfer");
            probe.phase = FAILED;
            (void)present_pose(-1, "DISPLAY TRANSFER FAILED", false);
        } else {
            probe.demo_pose = (probe.demo_pose + 1) % AY_POSE_DMA_FIXTURE_COUNT;
            probe.last_demo = esp_timer_get_time();
        }
    }

    if (!is_capture) board_async_set(original_async);
}
