#include "ayatsuri_pose_dma_probe.h"

#include <inttypes.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/board.h"
#include "../../tools/hone/native_geometry/alpha_graph_occlusion_band_candidate.h"
#include "private/assets/ay_pose_dma_fixture.h"

enum { WIDTH = 240, HEIGHT = 135, BAND_ROWS = 8, MEASURE_FRAMES = 60 };
static const int64_t measure_limit_us = INT64_C(12000000);
static const char *const tag = "ay_pose_dma";
static const ksn_mesh_snapshot opaque = {.enabled = true};

static struct {
    uint16_t row[WIDTH];
    uint16_t band[WIDTH * BAND_ROWS];
    float depth[WIDTH];
    float scratch[AY_POSE_DMA_FIXTURE_SCRATCH_COUNT];
} storage;

static bool prepare_pose(unsigned pose, hone_graph_band_payload *payload,
                         hone_graph_band_work *work)
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
    return ay_pose_dma_fixture(pose, &storage, generation, payload) &&
           hone_graph_band_valid(&opaque, payload, work, storage.band, WIDTH * BAND_ROWS);
}

static bool check_pose(unsigned pose)
{
    hone_graph_band_payload payload;
    hone_graph_band_work work;
    if (!prepare_pose(pose, &payload, &work) || !payload.graph.count) return false;

    unsigned visible_pixels = 0;
    uint32_t hash = UINT32_C(2166136261);
    for (unsigned y = 0; y < HEIGHT; ++y) {
        if (!hone_graph_occlusion_band_row(&opaque, &payload, &work, y)) return false;
        for (unsigned x = 0; x < WIDTH; ++x) {
            const uint16_t pixel = storage.row[x];
            visible_pixels += pixel != 0;
            hash = (hash ^ pixel) * UINT32_C(16777619);
        }
        if ((y + 1) % BAND_ROWS == 0) vTaskDelay(1);
    }
    ESP_LOGI(tag, "POSE pose=%u packets=%u visible_pixels=%u rgb565_hash=%08" PRIx32,
             pose, (unsigned)payload.graph.count, visible_pixels, hash);
    return visible_pixels != 0;
}

static bool present_pose(unsigned pose, int64_t *raster_us, int64_t *submit_us,
                         int64_t *finish_us)
{
    hone_graph_band_payload payload;
    hone_graph_band_work work;
    if (!prepare_pose(pose, &payload, &work)) return false;

    bool ok = true;
    for (unsigned y = 0; y < HEIGHT; y += BAND_ROWS) {
        const unsigned rows = HEIGHT - y < BAND_ROWS ? HEIGHT - y : BAND_ROWS;
        int64_t started = esp_timer_get_time();
        for (unsigned row = 0; row < rows; ++row) {
            if (!hone_graph_occlusion_band_row(&opaque, &payload, &work, y + row)) {
                ok = false;
                break;
            }
            memcpy(storage.band + row * WIDTH, storage.row, sizeof storage.row);
        }
        const int64_t rendered = esp_timer_get_time();
        *raster_us += rendered - started;
        if (!ok) break;

        const esp_err_t result = board_present_submit(0, (int)y, WIDTH, (int)rows, storage.band);
        *submit_us += esp_timer_get_time() - rendered;
        if (result != ESP_OK) {
            ok = false;
            break;
        }
    }

    const int64_t started = esp_timer_get_time();
    const esp_err_t drained = board_present_finish();
    *finish_us += esp_timer_get_time() - started;
    board_present_frame_end();
    return ok && drained == ESP_OK;
}

void ayatsuri_pose_dma_probe_run(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(tag, "BEGIN model_sha256=%s baked_poses=%u live_deformation=false lighting=false "
             "elf_sha_prefix=%02x%02x%02x%02x%02x%02x%02x%02x",
             AY_POSE_DMA_FIXTURE_MODEL_SHA256, (unsigned)AY_POSE_DMA_FIXTURE_COUNT,
             app->app_elf_sha256[0], app->app_elf_sha256[1], app->app_elf_sha256[2],
             app->app_elf_sha256[3], app->app_elf_sha256[4], app->app_elf_sha256[5],
             app->app_elf_sha256[6], app->app_elf_sha256[7]);

    if (board_present_finish() != ESP_OK || board_capture_active()) {
        ESP_LOGE(tag, "FAIL display_busy");
        return;
    }
    for (unsigned pose = 0; pose < AY_POSE_DMA_FIXTURE_COUNT; ++pose) {
        if (!check_pose(pose)) {
            ESP_LOGE(tag, "FAIL preflight pose=%u", pose);
            return;
        }
    }

    const int original_async = board_async_get();
    board_async_set(1);
    unsigned completed = 0;
    bool ok = true;
    int64_t raster_us = 0, submit_us = 0, finish_us = 0;
    const int64_t started = esp_timer_get_time();
    while (completed < MEASURE_FRAMES && esp_timer_get_time() - started < measure_limit_us) {
        const unsigned pose = completed % AY_POSE_DMA_FIXTURE_COUNT;
        if (!present_pose(pose, &raster_us, &submit_us, &finish_us)) {
            ok = false;
            break;
        }
        ++completed;
        vTaskDelay(1);
    }
    const int64_t elapsed_us = esp_timer_get_time() - started;
    board_async_set(original_async);

    const double fps = elapsed_us > 0 ? completed * 1000000.0 / elapsed_us : 0;
    ESP_LOGI(tag, "RESULT status=%s dma_completed_frames=%u elapsed_us=%" PRId64
             " baked_pose_fps=%.3f raster_us=%" PRId64 " submit_us=%" PRId64
             " final_drain_us=%" PRId64 " stack_hwm=%u free_internal=%u",
             ok && completed >= AY_POSE_DMA_FIXTURE_COUNT ? "PASS" : "FAIL",
             completed, elapsed_us, fps, raster_us, submit_us, finish_us,
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
