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
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
#include "ayatsuri_live_model_fixture_api.h"
#define MODEL_SHA256 AY_LIVE_MODEL_SHA256
#define POSE_COUNT AY_LIVE_MODEL_POSE_COUNT
#define MODEL_RENDERER "compact_opaque"
#define LIVE_DEFORMATION "true"
#define MODEL_LABEL "AYATSURI  /  LIVE MODEL"
#define MEASURE_FRAMES 126u
#else
#include "../../tools/hone/native_geometry/graph_flat_band_candidate.h"
#include "private/assets/ay_pose_dma_fixture.h"
#define MODEL_SHA256 AY_POSE_DMA_FIXTURE_MODEL_SHA256
#define POSE_COUNT AY_POSE_DMA_FIXTURE_COUNT
#define MODEL_RENDERER "flat_or_block16"
#define LIVE_DEFORMATION "false"
#define MODEL_LABEL "AYATSURI  /  BAKED MODEL"
#define MEASURE_FRAMES 60u
#endif

enum { WIDTH = 240, HEIGHT = 135, BAND_ROWS = 8, CHECK_POSES = 6 };
static const int64_t measure_limit_us = INT64_C(12000000);
static const int64_t demo_interval_us = INT64_C(120000);
static const char *const tag = "ay_pose_dma";

static struct {
    uint16_t row[WIDTH];
    uint16_t band[WIDTH * BAND_ROWS];
#ifndef CONFIG_AYATSURI_LIVE_DMA_PROBE
    float depth[WIDTH];
    float scratch[AY_POSE_DMA_FIXTURE_SCRATCH_COUNT];
    hone_graph_bounds bounds[576];
    uint16_t band_entries[1024];
#endif
} storage;

typedef struct {
    unsigned packets;
    uint32_t geometry_us;
    bool fast;
#ifndef CONFIG_AYATSURI_LIVE_DMA_PROBE
    hone_graph_band_payload payload;
    hone_graph_band_work work;
    hone_graph_band_index index;
    hone_flat_plan flat;
#endif
} pose_frame;

typedef enum { PREFLIGHT, BENCHMARK, DEMO, FAILED } probe_phase;
static struct {
    uint32_t generation;
    probe_phase phase;
    unsigned checked, completed, flat_completed, demo_pose;
    int64_t started, last_demo, prepare_us, geometry_us, raster_us, submit_us, finish_us;
    char result[40];
} probe;

#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
static bool waiting_for_drain;

static bool finish_pose(bool drained)
{
    const bool released = ay_live_model_finish(drained);
    waiting_for_drain = !released;
    return released;
}

static bool prepare_pose(unsigned pose, pose_frame *frame)
{
    if (waiting_for_drain && !finish_pose(board_present_finish() == ESP_OK)) return false;
    return ay_live_model_prepare(pose, &frame->packets, &frame->fast, &frame->geometry_us);
}

static bool render_pose_row(pose_frame *frame, unsigned y)
{
    (void)frame;
    return ay_live_model_row(y, storage.row);
}
#else
static const ksn_mesh_snapshot opaque = {.enabled = true};

static bool finish_pose(bool drained)
{
    return drained;
}

static bool prepare_pose(unsigned pose, pose_frame *frame)
{
    uint32_t generation = pose + 1;
    frame->work = (hone_graph_band_work){
        .binding = &storage,
        .generation = generation,
        .row = storage.row,
        .depth = storage.depth,
        .scratch = storage.scratch,
        .scratch_count = AY_POSE_DMA_FIXTURE_SCRATCH_COUNT,
    };
    // Every pose uses the same const packets on every replay. The index borrows
    // this exact payload descriptor for one pose and owns no packet data.
    if (!ay_pose_dma_fixture(pose, &storage, generation, &frame->payload) ||
        !hone_graph_index_build(&frame->index, &frame->payload.graph, generation,
                                storage.bounds, sizeof storage.bounds / sizeof storage.bounds[0],
                                storage.band_entries, sizeof storage.band_entries / sizeof storage.band_entries[0]) ||
        !hone_graph_index_band_valid(&opaque, &frame->payload, &frame->index, generation, &frame->work,
                                     storage.band, WIDTH * BAND_ROWS)) return false;
    frame->flat = hone_flat_prepare(&frame->payload, &frame->index, generation);
    frame->packets = (unsigned)frame->payload.graph.count;
    frame->fast = hone_flat_available(&opaque, &frame->flat);
    return true;
}

static bool render_pose_row(pose_frame *frame, unsigned y)
{
    if (frame->fast) return hone_flat_row(&frame->payload, &frame->flat, &frame->work, y);
    return hone_graph_block_row(&opaque, &frame->payload, &frame->index, &frame->work, y);
}
#endif

static bool check_pose(unsigned pose)
{
    pose_frame frame = {0};
    if (!prepare_pose(pose, &frame)) return false;

    unsigned visible_pixels = 0;
    uint32_t hash = UINT32_C(2166136261);
    bool ok = frame.packets != 0;
    for (unsigned y = 0; ok && y < HEIGHT; ++y) {
        if (!render_pose_row(&frame, y)) {
            ok = false;
            break;
        }
        for (unsigned x = 0; x < WIDTH; ++x) {
            const uint16_t pixel = storage.row[x];
            visible_pixels += pixel != 0;
            hash = (hash ^ pixel) * UINT32_C(16777619);
        }
        if ((y + 1) % BAND_ROWS == 0) vTaskDelay(1);
    }
    if (!finish_pose(true) || !ok) return false;
    ESP_LOGI(tag, "POSE pose=%u packets=%u visible_pixels=%u rgb565_hash=%08" PRIx32 " flat=%u",
             pose, frame.packets, visible_pixels, hash, (unsigned)frame.fast);
    return visible_pixels != 0;
}

static bool present_pose(int pose, const char *label, bool measure)
{
    pose_frame frame = {0};
    const int64_t prepared = esp_timer_get_time();
    if (pose >= 0 && !prepare_pose((unsigned)pose, &frame)) return false;
    if (measure) {
        probe.prepare_us += esp_timer_get_time() - prepared;
        probe.geometry_us += frame.geometry_us;
    }

    bool ok = true;
    for (unsigned y = 0; y < HEIGHT; y += BAND_ROWS) {
        const unsigned rows = HEIGHT - y < BAND_ROWS ? HEIGHT - y : BAND_ROWS;
        int64_t started = esp_timer_get_time();
        if (pose < 0) {
            memset(storage.band, 0, rows * sizeof storage.row);
        } else {
            for (unsigned row = 0; row < rows; ++row) {
                if (!render_pose_row(&frame, y + row)) {
                    ok = false;
                    break;
                }
                memcpy(storage.band + row * WIDTH, storage.row, sizeof storage.row);
            }
        }
        if (!ok) break;

        paint_begin(storage.band, (int)y, (int)rows);
        paint_fill(0, 0, WIDTH, 12, 0x0841);
        paint_ascii(6, 3, MODEL_LABEL, 0xffff);
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
    const bool released = pose < 0 || finish_pose(drained == ESP_OK);
    if (measure && ok && released && frame.fast) ++probe.flat_completed;
    return ok && drained == ESP_OK && released;
}

static void begin_benchmark(uint32_t generation)
{
    memset(&probe, 0, sizeof probe);
    probe.generation = generation;
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(tag, "BEGIN model_sha256=%s poses=%u renderer=" MODEL_RENDERER " live_deformation=" LIVE_DEFORMATION " lighting=false "
             "elf_sha_prefix=%02x%02x%02x%02x%02x%02x%02x%02x",
             MODEL_SHA256, (unsigned)POSE_COUNT,
             app->app_elf_sha256[0], app->app_elf_sha256[1], app->app_elf_sha256[2],
             app->app_elf_sha256[3], app->app_elf_sha256[4], app->app_elf_sha256[5],
             app->app_elf_sha256[6], app->app_elf_sha256[7]);

}

static void end_benchmark(bool ok, int64_t elapsed_us)
{
    const double fps = elapsed_us > 0 ? probe.completed * 1000000.0 / elapsed_us : 0;
    ok = ok && probe.completed >= CHECK_POSES;
    probe.phase = ok ? DEMO : FAILED;
    if (ok) snprintf(probe.result, sizeof probe.result, "%.2f FPS  %u LCD FRAMES", fps, probe.completed);
    else snprintf(probe.result, sizeof probe.result, "BENCHMARK FAILED  %u FRAMES", probe.completed);
    ESP_LOGI(tag, "RESULT status=%s dma_completed_frames=%u elapsed_us=%" PRId64
             " model_fps=%.3f prepare_us=%" PRId64 " geometry_us=%" PRId64
             " raster_us=%" PRId64 " submit_us=%" PRId64 " final_drain_us=%" PRId64
             " flat_frames=%u stack_hwm=%u free_internal=%u largest_internal=%u",
             ok ? "PASS" : "FAIL", probe.completed, elapsed_us, fps,
             probe.prepare_us, probe.geometry_us, probe.raster_us, probe.submit_us, probe.finish_us, probe.flat_completed,
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
        } else if (++probe.checked == CHECK_POSES) {
            probe.phase = BENCHMARK;
        }
    } else if (probe.phase == BENCHMARK) {
        if (!probe.completed) probe.started = esp_timer_get_time();
        const unsigned pose = probe.completed % POSE_COUNT;
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
            probe.demo_pose = (probe.demo_pose + 1) % POSE_COUNT;
            probe.last_demo = esp_timer_get_time();
        }
    }

    if (!is_capture) board_async_set(original_async);
}
