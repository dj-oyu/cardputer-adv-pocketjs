#include "ayatsuri_pose_dma_probe.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "ayatsuri_exact_damage.h"
#include "strip_copy_s3.h"
#include "pet/pet_hub.h"
#include "hal/sound.h"
static ay_damage_history displayed;
static ay_exact_history exact_pixels;
#define MODEL_SHA256 AY_LIVE_MODEL_SHA256
#define POSE_COUNT AY_LIVE_MODEL_POSE_COUNT
#define MODEL_RENDERER "compact_opaque_eye_masks"
#define LIVE_DEFORMATION "true"
#define MODEL_LABEL "AYATSURI  /  LIVE MODEL"
#ifdef AY_COMPANION_MOTION_POSES
#define MEASURE_FRAMES (AY_COMPANION_MOTION_POSES - AY_COMPANION_INSPECTION_POSES)
#define FIRST_MOTION_POSE AY_COMPANION_INSPECTION_POSES
#else
#define MEASURE_FRAMES 126u
#endif
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
#ifdef AY_COMPANION_MOTION_POSES
static const int64_t measure_limit_us = INT64_C(16000000);
static const int64_t demo_interval_us = INT64_C(50000);
#else
static const int64_t measure_limit_us = INT64_C(12000000);
static const int64_t demo_interval_us = INT64_C(120000);
#endif
#ifndef FIRST_MOTION_POSE
#define FIRST_MOTION_POSE 0u
#endif
#ifdef AY_COMPANION_COLORED_LIGHT
#define MODEL_LIGHTING "baked_rgb_diffuse"
#else
#define MODEL_LIGHTING "false"
#endif
_Static_assert(POSE_COUNT > FIRST_MOTION_POSE, "demo requires playable poses");
static const char *const tag = "ay_pose_dma";

static struct {
    uint16_t row[WIDTH];
    _Alignas(16) uint16_t band[WIDTH * BAND_ROWS];
#ifndef CONFIG_AYATSURI_LIVE_DMA_PROBE
    float depth[WIDTH];
    float scratch[AY_POSE_DMA_FIXTURE_SCRATCH_COUNT];
#endif
} storage;

#ifndef CONFIG_AYATSURI_LIVE_DMA_PROBE
typedef struct {
    hone_graph_band_payload payload;
    hone_graph_band_work work;
    hone_graph_band_index index;
    hone_flat_plan flat;
    hone_graph_bounds bounds[576];
    uint16_t band_entries[1024];
} baked_pose;

static baked_pose *baked_poses[POSE_COUNT];
#endif

typedef struct {
    unsigned packets;
    uint32_t geometry_us;
    bool fast;
#ifndef CONFIG_AYATSURI_LIVE_DMA_PROBE
    baked_pose *baked;
#endif
} pose_frame;

typedef struct {
    int64_t prepare_us, geometry_us, raster_us, submit_us, finish_us;
} frame_timing;

#ifdef AY_COMPANION_MOTION_POSES
typedef struct {
    unsigned from, to;
    int64_t interval_us, idle_us, late_us, work_us, previous_work_us;
    frame_timing timing;
} demo_gap;
#endif

typedef enum { PREFLIGHT, BENCHMARK, DEMO, FAILED } probe_phase;
static struct {
    uint32_t generation;
    probe_phase phase;
    unsigned checked, completed, flat_completed, demo_pose;
    int displayed_pose, held_pose;
    int64_t started, next_demo;
    frame_timing benchmark_timing;
#ifdef AY_COMPANION_MOTION_POSES
    uint16_t demo_cost_us[POSE_COUNT - FIRST_MOTION_POSE];
    int64_t demo_target_drain;
    int64_t demo_first_drain, demo_last_drain, demo_last_work;
    unsigned demo_intervals, demo_over_55ms;
    demo_gap demo_worst;
#endif
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
    unsigned drained_rects, drained_pixels, cache_overflows;
#endif
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

bool ayatsuri_pose_dma_probe_close_after_drain(void)
{
    if (!finish_pose(true) || !ay_live_model_close()) return false;
    probe.generation = 0;
    return true;
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

bool ayatsuri_pose_dma_probe_close_after_drain(void)
{
    for (unsigned pose = 0; pose < POSE_COUNT; ++pose) {
        free(baked_poses[pose]);
        baked_poses[pose] = NULL;
    }
    probe.generation = 0;
    return true;
}

static bool init_baked_pose(unsigned pose, baked_pose *baked)
{
    uint32_t generation = pose + 1;
    baked->work = (hone_graph_band_work){
        .binding = &storage,
        .generation = generation,
        .row = storage.row,
        .depth = storage.depth,
        .scratch = storage.scratch,
        .scratch_count = AY_POSE_DMA_FIXTURE_SCRATCH_COUNT,
    };
    if (!ay_pose_dma_fixture(pose, &storage, generation, &baked->payload) ||
        !hone_graph_index_build(&baked->index, &baked->payload.graph, generation,
                                baked->bounds, sizeof baked->bounds / sizeof baked->bounds[0],
                                baked->band_entries, sizeof baked->band_entries / sizeof baked->band_entries[0]) ||
        !hone_graph_index_band_valid(&opaque, &baked->payload, &baked->index, generation, &baked->work,
                                     storage.band, WIDTH * BAND_ROWS)) return false;
    baked->flat = hone_flat_prepare(&baked->payload, &baked->index, generation);
    return true;
}

static bool prepare_pose(unsigned pose, pose_frame *frame)
{
    if (pose >= POSE_COUNT) return false;
    baked_pose *baked = baked_poses[pose];
    if (!baked) {
        // Fix the descriptor's address before indexing. All packet data and all
        // mutable buffer identities stay unchanged until app_stop drains LCD.
        baked = calloc(1, sizeof *baked);
        if (!baked) return false;
        if (!init_baked_pose(pose, baked)) {
            free(baked);
            return false;
        }
        baked_poses[pose] = baked;
    }
    frame->baked = baked;
    frame->packets = (unsigned)baked->payload.graph.count;
    frame->fast = hone_flat_available(&opaque, &baked->flat);
    return true;
}

static bool render_pose_row(pose_frame *frame, unsigned y)
{
    baked_pose *baked = frame->baked;
    if (frame->fast) return hone_flat_row(&baked->payload, &baked->flat, &baked->work, y);
    return hone_graph_block_row(&opaque, &baked->payload, &baked->index, &baked->work, y);
}
#endif

static bool check_pose(unsigned pose)
{
    pose_frame frame = {0};
    if (!prepare_pose(pose, &frame)) {
        ESP_LOGE(tag, "FAIL phase=prepare pose=%u", pose);
        return false;
    }

    unsigned visible_pixels = 0;
    uint32_t hash = UINT32_C(2166136261);
    bool ok = frame.packets != 0;
    for (unsigned y = 0; ok && y < HEIGHT; ++y) {
        if (!render_pose_row(&frame, y)) {
            ESP_LOGE(tag, "FAIL phase=raster_preflight pose=%u row=%u", pose, y);
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
    if (!finish_pose(true)) {
        ESP_LOGE(tag, "FAIL phase=release_preflight pose=%u", pose);
        return false;
    }
    if (!ok) return false;
    ESP_LOGI(tag, "POSE pose=%u packets=%u visible_pixels=%u rgb565_hash=%08" PRIx32 " flat=%u",
             pose, frame.packets, visible_pixels, hash, (unsigned)frame.fast);
    if (!visible_pixels) ESP_LOGE(tag, "FAIL phase=empty_pose pose=%u", pose);
    return visible_pixels != 0;
}

#ifdef AY_COMPANION_MOTION_POSES
static const char *const inspection_labels[] = {"JOY", "SURPRISE", "CONCERN"};

/* Record the same RGB565 pixels that are sent to the LCD during preflight.
 * Keep this outside the measured section and exclude the right-hand HUD. */
static void report_face_band(int pose, unsigned y, unsigned rows)
{
    enum { FACE_LEFT = 88, FACE_WIDTH = 64, FACE_TOP = 34, FACE_BOTTOM = 86 };
#ifdef AY_COMPANION_COLORED_LIGHT
    enum { MODEL_LEFT = 48, MODEL_WIDTH = 104, MODEL_TOP = 20, MODEL_BOTTOM = 135 };
    const bool lighting_sample = pose == 0;
    const unsigned left = lighting_sample ? MODEL_LEFT : FACE_LEFT;
    const unsigned width = lighting_sample ? MODEL_WIDTH : FACE_WIDTH;
    const unsigned top = lighting_sample ? MODEL_TOP : FACE_TOP;
    const unsigned bottom = lighting_sample ? MODEL_BOTTOM : FACE_BOTTOM;
    char hex[MODEL_WIDTH * 4 + 1];
#else
    const unsigned left = FACE_LEFT, width = FACE_WIDTH, top = FACE_TOP, bottom = FACE_BOTTOM;
    char hex[FACE_WIDTH * 4 + 1];
#endif
    static const char symbols[] = "0123456789abcdef";
    for (unsigned row = 0; row < rows; ++row) {
        if (y + row < top || y + row >= bottom) continue;
        for (unsigned i = 0; i < width; ++i) {
            const uint16_t value = storage.band[row * WIDTH + left + i];
            for (unsigned nibble = 0; nibble < 4; ++nibble)
                hex[4 * i + nibble] = symbols[(value >> (12 - 4 * nibble)) & 15];
        }
        hex[width * 4] = '\0';
#ifdef AY_COMPANION_COLORED_LIGHT
        if (lighting_sample) {
            ESP_LOGI(tag, "LIGHT_ROW pose=0 y=%u rgb=%s", y + row, hex);
            continue;
        }
#endif
        ESP_LOGI(tag, "FACE_ROW pose=%d y=%u rgb=%s", pose, y + row, hex);
    }
}
#endif

static const char *inspection_label(int pose, char buffer[16])
{
#ifdef AY_COMPANION_MOTION_POSES
    if (pose >= 0 && pose < 3) return inspection_labels[pose];
#endif
    snprintf(buffer, 16, "POSE %02u", (unsigned)pose);
    return buffer;
}

static bool present_pose(int pose, const char *label, frame_timing *timing)
{
    pose_frame frame = {0};
    const int64_t prepared = esp_timer_get_time();
    if (pose >= 0 && !prepare_pose((unsigned)pose, &frame)) return false;
    if (timing) {
        timing->prepare_us += esp_timer_get_time() - prepared;
        timing->geometry_us += frame.geometry_us;
    }

    bool ok = true;
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
    unsigned frame_rects=0,frame_pixels=0;
    bool submitted=false;
    ay_exact_begin(&exact_pixels);
    bool overlay=pet_hub_overlay_visible() || sound_capture_active();
    bool force_full=board_capture_active() || pose<0;
#ifdef AY_COMPANION_MOTION_POSES
    /* Transmit the full inspection frame used by the RGB565 comparison. */
    force_full=force_full || (probe.phase==PREFLIGHT && pose>=0 && pose<3);
#endif
    ay_damage_frame damage=ay_damage_begin(&displayed,board_panel_write_epoch(),label,overlay,
                                          force_full);
#endif
    for (unsigned y = 0; y < HEIGHT; y += BAND_ROWS) {
        const unsigned rows = HEIGHT - y < BAND_ROWS ? HEIGHT - y : BAND_ROWS;
        int64_t started = esp_timer_get_time();
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
        unsigned left=0,right=0;
        if (pose>=0 && !ay_live_model_band(y,storage.band,&left,&right)) {ok=false; break;}
        if (pose<0) memset(storage.band,0,rows*sizeof storage.row);
#ifdef AY_COMPANION_MOTION_POSES
        if (probe.phase == PREFLIGHT && pose >= 0 && pose < 3) report_face_band(pose, y, rows);
#endif
        ay_damage_span window=ay_damage_plan(&displayed,&damage,y/BAND_ROWS,
                                             (ay_damage_span){(uint8_t)left,(uint8_t)right});
        ay_damage_span row_damage[BAND_ROWS];
        bool mandatory=damage.full || (y<32 && damage.header) ||
                       (y<48 && (damage.overlay || displayed.overlay));
        ay_damage_span changed={0,0};
        for(unsigned row=0;row<rows;row++) {
            row_damage[row]=ay_exact_row(&exact_pixels,y+row,storage.band+row*WIDTH,
                    (ay_damage_span){left,right},window,mandatory);
            changed=ay_exact_union(changed,row_damage[row]);
        }
        /* An identical frame still requires a successful LCD transaction. */
        if (!changed.right && !submitted && y+rows==HEIGHT) {
            row_damage[rows-1]=(ay_damage_span){0,16};
            changed=row_damage[rows-1];
        }
        unsigned cols=changed.right-changed.left;
#else
        unsigned x=0,cols=WIDTH;
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
#endif

        if (!cols) {
            if (timing) timing->raster_us+=esp_timer_get_time()-started;
            continue;
        }
        paint_begin(storage.band, (int)y, (int)rows);
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
        paint_fill(160, 0, 80, 30, 0x0841);
        paint_ascii(164, 3, "AYATSURI", 0xffff);
        paint_ascii(164, 13, label, 0xffff);
#ifdef AY_COMPANION_MOTION_POSES
        paint_ascii(164, 22, "L/U/R: FACE", 0x9d9f);
#else
        paint_ascii(164, 22, "L/R: POSE", 0x9d9f);
#endif
#else
        paint_fill(0, 0, WIDTH, 30, 0x0841);
        paint_ascii(6, 3, MODEL_LABEL, 0xffff);
        paint_ascii(6, 13, label, 0xffff);
        paint_ascii(6, 22, "ENTER: RUN AGAIN   BACK: EXIT", 0x9d9f);
#endif
        const int64_t rendered = esp_timer_get_time();
        if (timing) timing->raster_us += rendered - started;

#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
        /* On boards with two 4-row draw halves, use the lent DMA source.
         * A private narrow strip otherwise uses one synchronous SPI call per row. */
        esp_err_t result=ESP_OK;
        const unsigned step=(unsigned)board_strip_rows();
        for (unsigned off=0;off<rows && result==ESP_OK;off+=step) {
            unsigned n=rows-off<step ? rows-off : step;
            ay_damage_span strip_window={0,0};
            for(unsigned row=0;row<n;row++)strip_window=ay_exact_union(strip_window,row_damage[off+row]);
            unsigned x=strip_window.left,w=strip_window.right-x;
            if(!w)continue;
            uint16_t *strip=board_strip_next();
            hone_strip_copy(strip,storage.band+off*WIDTH,n*sizeof storage.row);
            result=board_present_submit((int)x,(int)(y+off),(int)w,(int)n,strip);
            if(result==ESP_OK){
                submitted=true;
                frame_rects++;frame_pixels+=n*w;
            }
        }
#else
        const esp_err_t result = board_present_submit((int)x, (int)y, (int)cols, (int)rows, storage.band);
#endif
        if (timing) timing->submit_us += esp_timer_get_time() - rendered;
        if (result != ESP_OK) {
            ESP_LOGE(tag, "FAIL phase=lcd_submit pose=%d row=%u error=%d", pose, y, (int)result);
            ok = false;
            break;
        }
    }

    const int64_t started = esp_timer_get_time();
    const esp_err_t drained = board_present_finish();
    if (drained != ESP_OK) ESP_LOGE(tag, "FAIL phase=lcd_drain pose=%d error=%d", pose, (int)drained);
    if (timing) timing->finish_us += esp_timer_get_time() - started;
    board_present_frame_end();
    const bool released = pose < 0 || finish_pose(drained == ESP_OK);
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
    if(timing==&probe.benchmark_timing && ok && drained==ESP_OK && released){
        probe.drained_rects+=frame_rects;probe.drained_pixels+=frame_pixels;
        probe.cache_overflows+=exact_pixels.overflow;
    }
    ay_exact_finish(&exact_pixels,ok && drained==ESP_OK && released && !board_capture_active());
    ay_damage_finish(&displayed,&damage,board_panel_write_epoch(),label,
                     ok && drained==ESP_OK && released && !board_capture_active());
#endif
    if (timing==&probe.benchmark_timing && ok && released && frame.fast) ++probe.flat_completed;
    return ok && drained == ESP_OK && released;
}

static void begin_benchmark(uint32_t generation)
{
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
    displayed.known=false;
#endif
    memset(&probe, 0, sizeof probe);
    probe.generation = generation;
    probe.displayed_pose = probe.held_pose = -1;
    probe.demo_pose = FIRST_MOTION_POSE;
#ifdef AY_COMPANION_MOTION_POSES
    ESP_LOGI(tag, "OVERLAY sha256=%s motion_first=%u", AY_COMPANION_OVERLAY_SHA256, FIRST_MOTION_POSE);
#endif
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(tag, "BEGIN model_sha256=%s poses=%u renderer=" MODEL_RENDERER " live_deformation=" LIVE_DEFORMATION " lighting=" MODEL_LIGHTING " "
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
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
    ESP_LOGI(tag,"DAMAGE drained_rects=%u drained_pixels=%u history_overflows=%u",
             probe.drained_rects,probe.drained_pixels,probe.cache_overflows);
    if (ok) snprintf(probe.result, sizeof probe.result, "%.2f FPS", fps);
    else snprintf(probe.result, sizeof probe.result, "FAILED");
#else
    if (ok) snprintf(probe.result, sizeof probe.result, "%.2f FPS  %u LCD FRAMES", fps, probe.completed);
    else snprintf(probe.result, sizeof probe.result, "BENCHMARK FAILED  %u FRAMES", probe.completed);
#endif
    ESP_LOGI(tag, "RESULT status=%s dma_completed_frames=%u elapsed_us=%" PRId64
             " model_fps=%.3f prepare_us=%" PRId64 " geometry_us=%" PRId64
             " raster_us=%" PRId64 " submit_us=%" PRId64 " final_drain_us=%" PRId64
             " flat_frames=%u stack_hwm=%u free_internal=%u largest_internal=%u",
             ok ? "PASS" : "FAIL", probe.completed, elapsed_us, fps,
             probe.benchmark_timing.prepare_us, probe.benchmark_timing.geometry_us,
             probe.benchmark_timing.raster_us, probe.benchmark_timing.submit_us,
             probe.benchmark_timing.finish_us, probe.flat_completed,
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

#ifdef AY_COMPANION_MOTION_POSES
static void demo_observe_cost(unsigned pose, int64_t elapsed)
{
    /* The table represents normal work on the way from the preceding pose.
     * Bound a single timer outlier and smooth later observations. */
    unsigned sample = elapsed > UINT16_MAX ? UINT16_MAX : (unsigned)elapsed;
    uint16_t *estimate = &probe.demo_cost_us[pose - FIRST_MOTION_POSE];
    *estimate = *estimate ? (3u * *estimate + sample + 2u) / 4u : sample;
}

static void demo_schedule(unsigned pose, int64_t started, int64_t drained)
{
    const int64_t work = drained - started;
    if (probe.next_demo) demo_observe_cost(pose, work);
    /* Start the next render early enough to drain on its clock edge. A
     * fresh sequence or a full-period stall anchors to the actual drain. */
    if (!probe.next_demo || drained - probe.demo_target_drain >= demo_interval_us)
        probe.demo_target_drain = drained;
    probe.demo_target_drain += demo_interval_us;
    unsigned estimate = probe.demo_cost_us[probe.demo_pose - FIRST_MOTION_POSE];
    if (!estimate) estimate = (unsigned)(work < demo_interval_us ? work : demo_interval_us);
    probe.next_demo = probe.demo_target_drain - estimate;
}

static void demo_completed(unsigned from, unsigned pose, int64_t due,
                           int64_t started, int64_t drained, const frame_timing *timing)
{
    const int64_t work = drained - started;
    if (!probe.demo_first_drain) {
        probe.demo_first_drain = drained;
    } else {
        const int64_t interval = drained - probe.demo_last_drain;
        if (interval > 55000) ++probe.demo_over_55ms;
        if (interval > probe.demo_worst.interval_us) {
            probe.demo_worst = (demo_gap){
                .from = from, .to = pose, .interval_us = interval,
                .idle_us = started - probe.demo_last_drain, .late_us = due ? started - due : 0,
                .work_us = work, .previous_work_us = probe.demo_last_work, .timing = *timing,
            };
        }
        if (++probe.demo_intervals == MEASURE_FRAMES) {
            const int64_t elapsed = drained - probe.demo_first_drain;
            ESP_LOGI(tag, "DEMO_RATE drain_intervals=%u elapsed_us=%" PRId64
                     " displayed_fps=%.3f longest_interval_us=%" PRId64 " over_55ms=%u",
                     probe.demo_intervals, elapsed, probe.demo_intervals * 1000000.0 / elapsed,
                     probe.demo_worst.interval_us, probe.demo_over_55ms);
            const demo_gap *gap = &probe.demo_worst;
            ESP_LOGI(tag, "DEMO_GAP from=%u to=%u interval_us=%" PRId64 " idle_us=%" PRId64
                     " late_us=%" PRId64 " previous_work_us=%" PRId64 " work_us=%" PRId64
                     " prepare_us=%" PRId64 " geometry_us=%" PRId64 " raster_us=%" PRId64
                     " submit_us=%" PRId64 " finish_us=%" PRId64,
                     gap->from, gap->to, gap->interval_us, gap->idle_us, gap->late_us,
                     gap->previous_work_us, gap->work_us, gap->timing.prepare_us, gap->timing.geometry_us,
                     gap->timing.raster_us, gap->timing.submit_us, gap->timing.finish_us);
            probe.demo_first_drain = drained;
            probe.demo_intervals = 0;
            probe.demo_over_55ms = 0;
            probe.demo_worst = (demo_gap){0};
        }
    }
    probe.demo_last_drain = drained;
    probe.demo_last_work = work;
}
#endif

int64_t ayatsuri_pose_dma_probe_step(uint32_t generation, bool restart, int inspect_pose, bool capture)
{
    if (generation != probe.generation || restart) begin_benchmark(generation);

    const int original_async = board_async_get();
    const bool is_capture = capture || board_capture_active();
    if (!is_capture) board_async_set(1);
#ifdef AY_COMPANION_MOTION_POSES
    if (inspect_pose == AY_COMPANION_RESUME_DEMO && probe.phase == DEMO && probe.held_pose >= 0) {
        probe.held_pose = -1;
        probe.next_demo = 0;
    }
#endif
    const bool select_pose = inspect_pose >= 0 && inspect_pose < (int)POSE_COUNT &&
                             inspect_pose != probe.held_pose;
    char pose_label[16];

#ifdef AY_COMPANION_MOTION_POSES
    if (select_pose || is_capture) {
        probe.demo_first_drain = 0;
        probe.demo_intervals = 0;
        probe.demo_over_55ms = 0;
        probe.demo_worst = (demo_gap){0};
        probe.next_demo = 0;
    }
#endif
    if (is_capture) {
        const int pose = probe.phase == DEMO ? probe.displayed_pose : -1;
        const char *label = probe.phase >= DEMO ? probe.result : "RUNNING";
        if (probe.phase == DEMO && probe.held_pose >= 0)
            label = inspection_label(probe.held_pose, pose_label);
        (void)present_pose(pose, label, NULL);
        if (probe.phase == BENCHMARK) begin_benchmark(generation);
    } else if (probe.phase == PREFLIGHT) {
        const bool banner_ready = probe.checked != 0 || present_pose(-1, "CHECKING", NULL);
        bool verified_pose = banner_ready && check_pose(probe.checked);
#ifdef AY_COMPANION_MOTION_POSES
        if (verified_pose && probe.checked < 3) {
            verified_pose = present_pose((int)probe.checked, inspection_labels[probe.checked], NULL);
            if (verified_pose) ESP_LOGI(tag, "FACE_DMA pose=%u status=PASS", probe.checked);
        }
#endif
        if (!verified_pose) {
            if (!banner_ready) ESP_LOGE(tag, "FAIL phase=preflight_banner");
            end_benchmark(false, 0);
            (void)present_pose(-1, probe.result, NULL);
        } else if (++probe.checked == CHECK_POSES) {
            probe.phase = BENCHMARK;
        }
    } else if (probe.phase == BENCHMARK) {
        if (!probe.completed) probe.started = esp_timer_get_time();
        const unsigned pose = FIRST_MOTION_POSE + probe.completed % (POSE_COUNT - FIRST_MOTION_POSE);
#ifdef AY_COMPANION_MOTION_POSES
        const int64_t frame_start = esp_timer_get_time();
#endif
        const bool ok = present_pose((int)pose, "RUNNING", &probe.benchmark_timing);
        if (ok) ++probe.completed;
        const int64_t finished = esp_timer_get_time();
#ifdef AY_COMPANION_MOTION_POSES
        /* Preflight does not end on the last motion pose. The first benchmark
         * sample may therefore have different geometry and cache reuse. */
        if (ok && probe.completed > 1) demo_observe_cost(pose, finished - frame_start);
#endif
        const int64_t elapsed = finished - probe.started;
        if (!ok || probe.completed == MEASURE_FRAMES || elapsed >= measure_limit_us) {
            end_benchmark(ok, elapsed);
            if (probe.phase == FAILED) (void)present_pose(-1, probe.result, NULL);
        }
    } else if (probe.phase == DEMO && (select_pose ||
               (probe.held_pose < 0 && esp_timer_get_time() >= probe.next_demo))) {
        const int pose = select_pose ? inspect_pose : (int)probe.demo_pose;
        const char *label = probe.result;
        if (select_pose) label = inspection_label(pose, pose_label);
        frame_timing *record = NULL;
#ifdef AY_COMPANION_MOTION_POSES
        frame_timing timing = {0};
        if (!select_pose) record = &timing;
        const int64_t demo_begin = esp_timer_get_time();
        const int64_t demo_due = probe.next_demo;
        const int previous_pose = probe.displayed_pose;
#endif
        if (!present_pose(pose, label, record)) {
            ESP_LOGE(tag, "FAIL demo_transfer");
            probe.phase = FAILED;
            (void)present_pose(-1, "LCD FAILED", NULL);
        } else {
            probe.displayed_pose = pose;
            if (select_pose) probe.held_pose = pose;
            else probe.demo_pose = FIRST_MOTION_POSE +
                    (probe.demo_pose - FIRST_MOTION_POSE + 1) % (POSE_COUNT - FIRST_MOTION_POSE);
#ifdef AY_COMPANION_MOTION_POSES
            if (!select_pose) {
                const int64_t drained = esp_timer_get_time();
                demo_schedule((unsigned)pose, demo_begin, drained);
                demo_completed((unsigned)previous_pose, (unsigned)pose, demo_due,
                               demo_begin, drained, &timing);
            }
#else
            probe.next_demo = esp_timer_get_time() + demo_interval_us;
#endif
        }
    }

    if (!is_capture) board_async_set(original_async);
#ifdef AY_COMPANION_MOTION_POSES
    if (probe.phase == DEMO && probe.held_pose < 0 && !is_capture) return probe.next_demo;
#endif
    return 0;
}
