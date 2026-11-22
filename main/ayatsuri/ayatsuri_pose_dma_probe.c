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
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
#include "ayatsuri_companion_net_probe.h"
#include "hal/lcd_spi.h"
#endif

#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
#include "hone/hone_frame.h"
#include "ayatsuri_live_model_fixture_api.h"
#include "pocket/wifi_time.h"
#include "ayatsuri_exact_damage.h"
#include "strip_copy_s3.h"
#include "pet/pet_hub.h"
#include "hal/sound.h"
#ifdef AYATSURI_CHARACTERS
#include "hone/track_character.h"
#endif
#ifdef AYATSURI_RASTER_TEST
#include "ayatsuri_raster_test.h"
#include "companion_raster_test_pixels.h"
#endif
#ifdef AYATSURI_CHARACTERS
/* Runtime-sized pose space: the active character decides every count below. */
#define MODEL_SHA256 ay_character_sha256()
#define POSE_COUNT ay_character_pose_count()
#define POSE_CAP 320u
#else
#define MODEL_SHA256 AY_LIVE_MODEL_SHA256
#define POSE_COUNT AY_LIVE_MODEL_POSE_COUNT
#endif
#if defined(AYATSURI_CHARACTERS)
#define MODEL_RENDERER "indexed_rgb565_character_track"
#define LIVE_DEFORMATION "false"
#elif defined(AYATSURI_TRACK_MODEL)
#define MODEL_RENDERER "indexed_rgb565_motion_track"
#define LIVE_DEFORMATION "false"
#else
#define MODEL_RENDERER "compact_opaque_eye_masks"
#define LIVE_DEFORMATION "true"
#endif
#define MODEL_LABEL "AYATSURI  /  LIVE MODEL"
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
/* Older private model overlays can still build the stock diagnostic. */
extern void ay_live_model_projection_reuse(bool enabled) __attribute__((weak));
#ifdef CONFIG_AYATSURI_RENDER_TAIL_AB
void ay_live_model_phase_us(uint32_t out[7]);
#endif
#endif
#if defined(AYATSURI_CHARACTERS)
#define COMPANION_MOTION 1
#define OVERLAY_SHA ay_character_sha256()
#define FIRST_MOTION_POSE ay_character_inspection_count()
#define MEASURE_FRAMES (POSE_COUNT - FIRST_MOTION_POSE)
#ifndef AY_COMPANION_COLORED_LIGHT
#define AY_COMPANION_COLORED_LIGHT 1
#endif
#elif defined(AY_COMPANION_MOTION_POSES)
#define COMPANION_MOTION 1
#define OVERLAY_SHA AY_COMPANION_OVERLAY_SHA256
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
#ifdef AYATSURI_RASTER_TEST
_Static_assert(AY_RASTER_WIDTH == WIDTH && AY_RASTER_HEIGHT == HEIGHT, "LCD chart dimensions");
#endif
#ifdef COMPANION_MOTION
static const int64_t measure_limit_us = INT64_C(16000000);
#if defined(AYATSURI_CHARACTERS)
/* Each clip states its own frame period. */
#define demo_interval_us ((int64_t)ay_character_period_ms() * INT64_C(1000))
#elif defined(AY_COMPANION_FPS_TRIAL) || defined(AYATSURI_TRACK_MODEL)
/* The acceptance deadline remains 50 ms. Give the UI tick and DMA timing
 * variance room on the render clock while preserving the ordered poses. */
static const int64_t demo_interval_us = INT64_C(48000);
#else
static const int64_t demo_interval_us = INT64_C(50000);
#endif
#else
static const int64_t measure_limit_us = INT64_C(12000000);
static const int64_t demo_interval_us = INT64_C(120000);
#endif
#ifndef FIRST_MOTION_POSE
#define FIRST_MOTION_POSE 0u
#endif
#ifdef AYATSURI_TRACK_MODEL
#define MODEL_LIGHTING "baked_face_rgb565"
#elif defined(AY_COMPANION_COLORED_LIGHT)
#define MODEL_LIGHTING "baked_rgb_diffuse"
#else
#define MODEL_LIGHTING "false"
#endif
#ifndef AYATSURI_CHARACTERS
_Static_assert(POSE_COUNT > FIRST_MOTION_POSE, "demo requires playable poses");
#endif
static const char *const tag = "ay_pose_dma";

typedef struct {
    uint16_t row[WIDTH];
    _Alignas(16) uint16_t band[WIDTH * BAND_ROWS];
#ifndef CONFIG_AYATSURI_LIVE_DMA_PROBE
    float depth[WIDTH];
    float scratch[AY_POSE_DMA_FIXTURE_SCRATCH_COUNT];
#endif
} storage_t;

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
    bool serial;
} frame_timing;

#ifdef COMPANION_MOTION
typedef struct {
    unsigned from, to;
    int64_t interval_us, idle_us, late_us, work_us, previous_work_us;
    frame_timing timing;
} demo_gap;
#endif

typedef enum { PREFLIGHT, BENCHMARK, DEMO, FAILED } probe_phase;
#ifdef AYATSURI_CHARACTERS
#define MOTION_POSES (POSE_COUNT - FIRST_MOTION_POSE)
#define POSE_STOPS (MOTION_POSES < 8u ? MOTION_POSES : 8u)
#define DEMO_COST_SLOTS POSE_CAP
#else
enum { MOTION_POSES = POSE_COUNT - FIRST_MOTION_POSE, POSE_STOPS = MOTION_POSES < 8 ? MOTION_POSES : 8 };
#define DEMO_COST_SLOTS (POSE_COUNT - FIRST_MOTION_POSE)
#endif
typedef struct {
    uint32_t generation;
    probe_phase phase;
    unsigned checked, completed, flat_completed, demo_pose;
    unsigned pose_stop, expression;
    int displayed_pose, held_pose;
    int64_t started, next_demo;
    frame_timing benchmark_timing;
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
    ay_net_request network_requested;
    unsigned serial_penalty_us;
#endif
#ifdef COMPANION_MOTION
    uint16_t demo_cost_us[DEMO_COST_SLOTS];
    int64_t demo_target_drain;
    int64_t demo_first_drain, demo_last_drain, demo_last_work;
    unsigned demo_intervals, demo_over_55ms;
    demo_gap demo_worst;
#endif
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
    unsigned drained_rects, drained_pixels, cache_overflows;
#endif
#ifdef AYATSURI_RASTER_TEST
    int raster_marker;
    bool raster_scan, raster_motion_logged;
#endif
    char result[40];
} probe_t;

/* Everything the Companion keeps between frames, about 14 KB. It lives on the heap while the
 * Companion runs and is released when it closes: as static DRAM it was carried by every
 * firmware with this feature compiled in, including while other apps run. The accessors below
 * keep the code that uses these names unchanged. */
typedef struct {
    ay_damage_history displayed;
    ay_exact_history exact_pixels;
    storage_t storage;
    probe_t probe;
} workspace_t;
static workspace_t *ws;
#define displayed (ws->displayed)
#define exact_pixels (ws->exact_pixels)
#define storage (ws->storage)
#define probe (ws->probe)

static bool workspace_open(void)
{
    if (!ws) ws = heap_caps_aligned_calloc(16, 1, sizeof *ws, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return ws != NULL;
}

static void workspace_close(void)
{
    heap_caps_free(ws);
    ws = NULL;
}

#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
static hone_frame snapshot;
static hone_entry live_entry;
static hone_presentation presentation;

static bool release_live(void *context)
{
    (void)context;
    return ay_live_model_finish(true);
}

/* The legacy evaluator has already frozen the projected packets and their eye
 * masks. Large aligned requests retain its dual-core raster fast path. */
static bool render_live(void *context, hone_rect region, const hone_target *target, hone_span *span)
{
    (void)context;
    if (region.width == WIDTH && region.y % BAND_ROWS == 0 &&
        region.height == (HEIGHT - region.y < BAND_ROWS ? HEIGHT - region.y : BAND_ROWS) &&
        target->stride_bytes == sizeof storage.row)
        return ay_live_model_band(region.y, target->pixels, &span->left, &span->right);
    for (unsigned row = 0; row < region.height; ++row) {
        uint16_t *dest = (uint16_t *)((uint8_t *)target->pixels + row * target->stride_bytes);
        if (region.width == WIDTH) {
            if (!ay_live_model_row(region.y + row, dest)) return false;
        } else {
            if (!ay_live_model_row(region.y + row, storage.row)) return false;
            memmove(dest, storage.row + region.x, region.width * sizeof *dest);
        }
    }
    return true;
}

static bool finish_pose(bool drained, bool accepted)
{
    if (presentation.snapshot) {
        /* An undrained transfer also has to join any speculative CPU raster
         * worker before the caller returns its core to Wi-Fi. Keep both pins. */
        if (!drained && snapshot.count) (void)ay_live_model_finish(false);
        if (hone_presentation_complete(&presentation, drained, accepted ? HONE_OK : HONE_IO_ERROR)
            != HONE_OK) return false;
    }
    if (snapshot.phase == HONE_IDLE) return true;
    return hone_snapshot_dispose(&snapshot) == HONE_OK;
}

bool ayatsuri_pose_dma_probe_close_after_drain(void)
{
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
    if (!ayatsuri_companion_net_reset()) return false;
#endif
    if (!finish_pose(true, false) || !ay_live_model_close()) return false;
    workspace_close();
    return true;
}

static bool prepare_pose(int pose, pose_frame *frame)
{
    /* Previous DMA may still own the strips, including after a banner frame. */
    if (snapshot.phase != HONE_IDLE && !finish_pose(board_present_finish() == ESP_OK, false))
        return false;
    const hone_frame_options options = {.profile = HONE_OPAQUE_RGB565, .width = WIDTH, .height = HEIGHT,
        .max_bytes = sizeof snapshot + sizeof live_entry, .max_drawables = 1,
        .max_work = 2u * WIDTH * HEIGHT};
    if (hone_frame_begin(&snapshot, &options, &live_entry, 1) != HONE_OK) return false;
    if (pose >= 0) {
        if (!ay_live_model_prepare((unsigned)pose, &frame->packets, &frame->fast, &frame->geometry_us)) {
            hone_frame_cancel(&snapshot);
            return false;
        }
        hone_drawable drawable = {.render = render_live, .release = release_live};
        if (hone_frame_submit(&snapshot, &drawable, NULL) != HONE_OK) {
            release_live(NULL);
            hone_frame_cancel(&snapshot);
            return false;
        }
    }
    return hone_frame_commit(&snapshot) == HONE_OK;
}

static bool render_live_region(unsigned y, unsigned rows, uint16_t *pixels, hone_span *span)
{
    hone_target target = {.pixels = pixels, .width = WIDTH, .height = rows,
        .stride_bytes = sizeof storage.row, .capacity_bytes = rows * sizeof storage.row,
        .format = HONE_RGB565_NATIVE};
    return hone_snapshot_render(&snapshot, (hone_rect){0, y, WIDTH, rows}, &target, span) == HONE_OK;
}

static bool render_pose_row(pose_frame *frame, unsigned y)
{
    (void)frame;
    return render_live_region(y, 1, storage.row, NULL);
}
#else
static const ksn_mesh_snapshot opaque = {.enabled = true};

static bool finish_pose(bool drained, bool accepted)
{
    (void)accepted;
    return drained;
}

bool ayatsuri_pose_dma_probe_close_after_drain(void)
{
    for (unsigned pose = 0; pose < POSE_COUNT; ++pose) {
        free(baked_poses[pose]);
        baked_poses[pose] = NULL;
    }
    workspace_close();
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

static bool prepare_pose(int pose, pose_frame *frame)
{
    if (pose < 0) return true;
    if ((unsigned)pose >= POSE_COUNT) return false;
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
    if (!finish_pose(true, ok)) {
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
static const char *const inspection_labels[] = {"JOY", "SURPRISE", "CONCERN"
#ifdef AYATSURI_TRACK_MODEL
    , "BLINK", "INHALE", "GLANCE"
#endif
};
#endif

#ifdef COMPANION_MOTION
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
#if defined(AYATSURI_CHARACTERS)
    if (pose >= 0 && (unsigned)pose < ay_character_inspection_count())
        return ay_character_face_label((unsigned)pose);
#elif defined(COMPANION_MOTION)
    if (pose >= 0 && (unsigned)pose < sizeof inspection_labels / sizeof *inspection_labels)
        return inspection_labels[pose];
#endif
    if (pose >= (int)FIRST_MOTION_POSE) {
        snprintf(buffer, 16, "POSE %u/%u", probe.pose_stop + 1, POSE_STOPS);
        return buffer;
    }
    snprintf(buffer, 16, "POSE %02u", (unsigned)pose);
    return buffer;
}

#ifdef AYATSURI_CHARACTERS
/* 0 is no backdrop; 1..n pick a background from the store. */
static unsigned background;

static const uint16_t *background_pixels(void)
{
    return background ? ay_background_pixels(background - 1) : NULL;
}

#define face_text(index) ay_character_face_label(index)
#define PREFLIGHT_POSES (ay_character_inspection_count() < CHECK_POSES ? ay_character_inspection_count() : (unsigned)CHECK_POSES)

static void upper_copy(char *out, size_t size, const char *text)
{
    size_t i = 0;
    for (; text[i] && i + 1 < size; ++i) out[i] = text[i] >= 'a' && text[i] <= 'z' ? (char)(text[i] - 32) : text[i];
    out[i] = '\0';
}

/* Append with truncation; the HUD lines are short and clipped on purpose. */
static void append(char *out, size_t size, const char *text)
{
    size_t used = strlen(out);
    for (size_t i = 0; text[i] && used + 1 < size; ++i) out[used++] = text[i];
    out[used] = '\0';
}

/* Four status lines for the HUD, plus the string that decides whether the
 * header bands need repainting. Every dynamic line sits in rows 0..31. */
static void hud_compose(const char *label, char hud[4][13], char key[48])
{
    char name[13], clip[13], face[13], emote[13];
    /* Asking a character anything unpacks it, which takes seconds on the first run:
     * the "loading" banner has to be drawn without touching the store. */
    const bool known = ay_character_store_ready();
    upper_copy(name, sizeof name, known ? ay_character_name(ay_character_current()) : "");
    upper_copy(clip, sizeof clip, known && ay_character_clip_count() > 1 ? ay_character_clip_label(ay_character_clip()) : "");
    upper_copy(face, sizeof face, known ? ay_character_face_label(ay_character_face()) : "");
    upper_copy(emote, sizeof emote, known ? ay_character_emote_label(ay_character_emote()) : "");
    hud[0][0] = '\0';
    append(hud[0], sizeof hud[0], name);
    if (clip[0]) {
        append(hud[0], sizeof hud[0], " ");
        append(hud[0], sizeof hud[0], clip);
    }
    upper_copy(hud[1], sizeof hud[1], face);
    hud[2][0] = '\0';
    append(hud[2], sizeof hud[2], emote[0] ? "+" : "NO EMOTE");
    append(hud[2], sizeof hud[2], emote);
    upper_copy(hud[3], sizeof hud[3], label);
    key[0] = '\0';
    for (unsigned i = 0; i < 4; ++i) {
        if (i) append(key, 48, "|");
        append(key, 48, hud[i]);
    }
}
#else
#define face_text(index) inspection_labels[index]
#define PREFLIGHT_POSES CHECK_POSES
#endif

static bool present_pose_impl(int pose, const char *label, frame_timing *timing)
{
    pose_frame frame = {0};
    const int64_t prepared = esp_timer_get_time();
    if (!prepare_pose(pose, &frame)) return false;
    if (timing) {
        timing->prepare_us += esp_timer_get_time() - prepared;
        timing->geometry_us += frame.geometry_us;
    }

    bool ok = true;
#ifdef AYATSURI_CHARACTERS
    char hud[4][13], damage_key[48];
    hud_compose(label, hud, damage_key);
#define DAMAGE_LABEL damage_key
#else
#define DAMAGE_LABEL label
#endif
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
    if (hone_present(&snapshot, &presentation) != HONE_OK) {
        finish_pose(true, false);
        return false;
    }
    unsigned frame_rects=0,frame_pixels=0;
    bool submitted=false;
    ay_exact_begin(&exact_pixels);
    bool overlay=pet_hub_alert_shown() || sound_capture_active();
    bool force_full=board_capture_active() || pose<0;
#ifdef COMPANION_MOTION
    /* Transmit the full inspection frame used by the RGB565 comparison. */
    force_full=force_full || (probe.phase==PREFLIGHT && pose>=0 && pose<3);
#endif
    ay_damage_frame damage=ay_damage_begin(&displayed,board_panel_write_epoch(),DAMAGE_LABEL,overlay,
                                          force_full);
#ifdef AYATSURI_RASTER_TEST
    bool scan = probe.raster_scan && probe.phase == DEMO && pose >= (int)FIRST_MOTION_POSE;
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
    scan = scan && !probe.network_requested && !ayatsuri_companion_net_busy();
#endif
    const int marker = scan ? ay_raster_marker((unsigned)pose - FIRST_MOTION_POSE) : -1;
    ay_damage_span raster_damage = {0, 0};
    if (marker != probe.raster_marker)
        raster_damage = ay_exact_union(ay_raster_marker_span(marker),
                                       ay_raster_marker_span(probe.raster_marker));
#endif
#endif
    for (unsigned y = 0; y < HEIGHT; y += BAND_ROWS) {
        const unsigned rows = HEIGHT - y < BAND_ROWS ? HEIGHT - y : BAND_ROWS;
        int64_t started = esp_timer_get_time();
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
        hone_span span;
        if (!render_live_region(y, rows, storage.band, &span)) {ok=false; break;}
        unsigned left = span.left, right = span.right;
#ifdef COMPANION_MOTION
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
#ifdef AYATSURI_RASTER_TEST
            row_damage[row] = ay_exact_union(row_damage[row], raster_damage);
#endif
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
#ifdef AYATSURI_RASTER_TEST
        for (unsigned row = 0; row < rows; ++row) {
            ay_raster_composite(storage.band + row * WIDTH,
                                ay_raster_pixels + (y + row) * WIDTH, changed, marker);
        }
#endif
#ifdef AYATSURI_CHARACTERS
        if (background_pixels()) {
            /* A model pixel of zero is empty: the backdrop shows through, as for the test chart. */
            for (unsigned row = 0; row < rows; ++row) {
                const uint16_t *source = background_pixels() + (y + row) * WIDTH;
                uint16_t *model = storage.band + row * WIDTH;
                for (unsigned x = changed.left; x < changed.right; ++x) {
                    if (!model[x]) model[x] = source[x];
                }
            }
        }
#endif
        paint_begin(storage.band, (int)y, (int)rows);
#if defined(AYATSURI_CHARACTERS)
        /* Left-hand panel: the right side belongs to the emote marks. */
        paint_fill(0, 0, 72, 88, 0x0841);
        paint_ascii(3, 1, hud[0], 0xffff);
        paint_ascii(3, 9, hud[1], 0xffff);
        paint_ascii(3, 17, hud[2], 0x9d9f);
        paint_ascii(3, 25, hud[3], 0x9d9f);
        paint_ascii(3, 33, "L/R POSE", 0x9d9f);
        paint_ascii(3, 41, "U/D FACE", 0x9d9f);
        paint_ascii(3, 49, "E EMOTE", 0x9d9f);
        paint_ascii(3, 57, "C CHAR", 0x9d9f);
        paint_ascii(3, 65, "M MOTION", 0x9d9f);
        paint_ascii(3, 73, "B BACKDROP", 0x9d9f);
        paint_ascii(3, 81, "SPC ENT", 0x9d9f);
#elif defined(CONFIG_AYATSURI_LIVE_DMA_PROBE)
        paint_fill(176, 0, 64, 40, 0x0841);
        paint_ascii(180, 1, label, 0xffff);
        paint_ascii(180, 9, "L/R POSE", 0x9d9f);
#ifdef COMPANION_MOTION
        paint_ascii(180, 17, "U/D FACE", 0x9d9f);
#endif
        paint_ascii(180, 25, "SPC PLAY", 0x9d9f);
        paint_ascii(180, 33, "ENT BENCH", 0x9d9f);
#ifdef AYATSURI_RASTER_TEST
        paint_fill(176, 40, 64, 8, 0x0841);
        paint_ascii(180, 41, "T SCAN", 0x9d9f);
#endif
#else
        paint_fill(0, 0, WIDTH, 30, 0x0841);
        paint_ascii(6, 3, MODEL_LABEL, 0xffff);
        paint_ascii(6, 13, label, 0xffff);
        paint_ascii(6, 22, "L/R POSE   SPACE PLAY   ENTER BENCH", 0x9d9f);
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
            if (w==WIDTH || board_capture_active() || pet_hub_alert_shown() ||
                sound_capture_active()) {
                uint16_t *strip=board_strip_next();
                hone_strip_copy(strip,storage.band+off*WIDTH,n*sizeof storage.row);
                result=board_present_submit((int)x,(int)(y+off),(int)w,(int)n,strip);
            } else {
                result=board_present_submit_source((int)x,(int)(y+off),(int)w,(int)n,
                                                   storage.band+off*WIDTH);
            }
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
    const bool released = finish_pose(drained == ESP_OK, ok);
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
    if(timing==&probe.benchmark_timing && ok && drained==ESP_OK && released){
        probe.drained_rects+=frame_rects;probe.drained_pixels+=frame_pixels;
        probe.cache_overflows+=exact_pixels.overflow;
    }
    const bool completed = ok && drained == ESP_OK && released && !board_capture_active();
#ifdef AYATSURI_RASTER_TEST
    if (completed) {
        if (damage.full && probe.displayed_pose < 0 && probe.checked == 0)
            ESP_LOGI(tag, "RASTER chart_bytes=%u drained_pixels=%u marker_x=%d",
                     (unsigned)sizeof ay_raster_pixels, frame_pixels, marker);
        if (!probe.raster_motion_logged && probe.raster_marker >= 0 && marker >= 0 &&
            marker != probe.raster_marker) {
            ESP_LOGI(tag, "RASTER scan_drained previous_x=%d x=%d", probe.raster_marker, marker);
            probe.raster_motion_logged = true;
        }
        probe.raster_marker = marker;
    }
#endif
    ay_exact_finish(&exact_pixels,completed);
    ay_damage_finish(&displayed,&damage,board_panel_write_epoch(),DAMAGE_LABEL,completed);
#endif
    if (timing==&probe.benchmark_timing && ok && released && frame.fast) ++probe.flat_completed;
    return ok && drained == ESP_OK && released;
}

static bool present_pose(int pose, const char *label, frame_timing *timing)
{
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
    if (pose >= 0) {
        const bool paired = wifi_time_try_render_core0();
        ay_live_model_serial_raster(!paired);
        if (timing) timing->serial = !paired;
        const bool ok = present_pose_impl(pose, label, timing);
        if (paired) wifi_time_release_render_core0();
        return ok;
    }
#endif
    return present_pose_impl(pose, label, timing);
}

#ifdef AYATSURI_CHARACTERS
static unsigned benchmark_face, benchmark_emote;
#endif

static void begin_companion(uint32_t generation, probe_phase phase)
{
#ifdef AYATSURI_RASTER_TEST
    const bool raster_scan = generation != probe.generation || probe.raster_scan;
#endif
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
    displayed.known=false;
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
#ifdef AY_COMPANION_FPS_EYE32
    if (ay_live_model_projection_reuse) ay_live_model_projection_reuse(true);
#else
    if (ay_live_model_projection_reuse) ay_live_model_projection_reuse(false);
#endif
#endif
#endif
#ifdef AYATSURI_CHARACTERS
    /* The benchmark walks every face; the person's choice comes back after. */
    if (phase == PREFLIGHT) {
        benchmark_face = ay_character_face();
        benchmark_emote = ay_character_emote();
        (void)ay_character_set_emote(0);
    }
#endif
    memset(&probe, 0, sizeof probe);
    probe.generation = generation;
    probe.phase = phase;
    probe.displayed_pose = probe.held_pose = -1;
    probe.demo_pose = FIRST_MOTION_POSE;
#ifdef AYATSURI_RASTER_TEST
    probe.raster_marker = -1;
    probe.raster_scan = raster_scan;
#endif
#ifdef COMPANION_MOTION
    ESP_LOGI(tag, "OVERLAY sha256=%s motion_first=%u", OVERLAY_SHA, (unsigned)FIRST_MOTION_POSE);
#endif
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(tag, "BEGIN model_sha256=%s poses=%u renderer=" MODEL_RENDERER " live_deformation=" LIVE_DEFORMATION " lighting=" MODEL_LIGHTING " "
             "elf_sha_prefix=%02x%02x%02x%02x%02x%02x%02x%02x",
             MODEL_SHA256, (unsigned)POSE_COUNT,
             app->app_elf_sha256[0], app->app_elf_sha256[1], app->app_elf_sha256[2],
             app->app_elf_sha256[3], app->app_elf_sha256[4], app->app_elf_sha256[5],
             app->app_elf_sha256[6], app->app_elf_sha256[7]);
}

#if defined(COMPANION_MOTION) && defined(CONFIG_AYATSURI_COMPANION_NET_PROBE)
static ay_net_request network_benchmark(void)
{
#if defined(CONFIG_AYATSURI_CHAT_STREAM_PROBE)
    return AY_NET_RESERVE_WIFI_INIT_CHAT_BITAP;
#elif defined(CONFIG_AYATSURI_RENDER_TAIL_AB)
    return AY_NET_RESERVE_WIFI_INIT_RENDER_TAIL;
#elif defined(CONFIG_AYATSURI_LCD_GDMA_PRIORITY)
    return AY_NET_RESERVE_WIFI_INIT_LCD_GDMA;
#elif defined(CONFIG_AYATSURI_P384_MODEXP)
    return AY_NET_RESERVE_WIFI_INIT_P384_MODEXP;
#elif defined(CONFIG_AYATSURI_P384_G_TABLE)
    return AY_NET_RESERVE_WIFI_INIT_P384_TABLE;
#elif defined(CONFIG_AYATSURI_P384_PROFILE)
    return AY_NET_RESERVE_WIFI_INIT_P384_DETAIL;
#elif defined(CONFIG_AYATSURI_CERT_INV_SCRATCH)
    return AY_NET_RESERVE_WIFI_INIT_CERT_SCRATCH;
#else
    return AY_NET_RESERVE_WIFI_INIT_SMALL_STACK;
#endif
}
#endif

static void end_benchmark(bool ok, int64_t elapsed_us)
{
#ifdef AYATSURI_CHARACTERS
    (void)ay_character_set_face(benchmark_face);
    (void)ay_character_set_emote(benchmark_emote);
#endif
    const double fps = elapsed_us > 0 ? probe.completed * 1000000.0 / elapsed_us : 0;
    ok = ok && probe.completed >= CHECK_POSES;
    probe.phase = ok ? DEMO : FAILED;
#if defined(COMPANION_MOTION) && defined(CONFIG_AYATSURI_COMPANION_NET_PROBE)
    if (ok) probe.network_requested = network_benchmark();
#endif
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

#ifdef COMPANION_MOTION
static void demo_observe_cost(unsigned pose, int64_t elapsed)
{
    /* The table represents normal work on the way from the preceding pose.
     * Bound a single timer outlier and smooth later observations. */
    unsigned sample = elapsed > UINT16_MAX ? UINT16_MAX : (unsigned)elapsed;
    uint16_t *estimate = &probe.demo_cost_us[pose - FIRST_MOTION_POSE];
    *estimate = *estimate ? (3u * *estimate + sample + 2u) / 4u : sample;
}

static void demo_schedule(unsigned pose, int64_t started, int64_t drained, bool serial)
{
    const int64_t work = drained - started;
    if (probe.next_demo && !serial) demo_observe_cost(pose, work);
    /* Start the next render early enough to drain on its clock edge. A
     * fresh sequence or a full-period stall anchors to the actual drain. */
    if (!probe.next_demo || drained - probe.demo_target_drain >= demo_interval_us)
        probe.demo_target_drain = drained;
    /* Keep the absolute animation clock. Correct a quarter of the last
     * drain's phase error in this interval. Repaying it all makes an early
     * frame followed by an on-time frame show as a long LCD interval. */
    const int64_t phase_error = probe.demo_target_drain - drained;
    const int64_t next_drain = drained + demo_interval_us + phase_error / 4;
    probe.demo_target_drain += demo_interval_us;
    unsigned estimate = probe.demo_cost_us[probe.demo_pose - FIRST_MOTION_POSE];
    if (!estimate) estimate = (unsigned)(work < demo_interval_us ? work : demo_interval_us);
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
    if (serial) {
        const unsigned paired = probe.demo_cost_us[pose - FIRST_MOTION_POSE];
        const unsigned extra = work > paired ? (unsigned)(work - paired) : 0;
        const unsigned bounded = extra > 10000u ? 10000u : extra;
        probe.serial_penalty_us = (3u * probe.serial_penalty_us + bounded + 2u) / 4u;
        estimate += probe.serial_penalty_us;
    }
#endif
    probe.next_demo = next_drain - estimate;
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
                     " displayed_fps=%.3f longest_interval_us=%" PRId64 " over_55ms=%u period_us=%" PRId64,
                     probe.demo_intervals, elapsed, probe.demo_intervals * 1000000.0 / elapsed,
                     probe.demo_worst.interval_us, probe.demo_over_55ms, demo_interval_us);
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

#ifdef AYATSURI_CHARACTERS
/* Any change of character or clip waits for the previous frame to drain, as
 * prepare_pose does, because the player refuses to switch while a frame is pinned. */
static bool quiesce(void)
{
    return snapshot.phase == HONE_IDLE || finish_pose(board_present_finish() == ESP_OK, false);
}

static void restart_playback(void)
{
    probe.pose_stop = 0;
    probe.expression = 0;
    probe.demo_pose = FIRST_MOTION_POSE;
    probe.held_pose = -1;
    probe.next_demo = 0;
    probe.result[0] = '\0';
    memset(probe.demo_cost_us, 0, sizeof probe.demo_cost_us);
    displayed.known = false;
}

static void change_character(ay_companion_command command)
{
    if (!quiesce()) return;
    if (command == AY_COMPANION_NEXT_CHARACTER) {
        const unsigned count = ay_character_count();
        if (count < 2 || !ay_character_select((ay_character_current() + 1) % count)) return;
    } else {
        const unsigned count = ay_character_clip_count();
        if (count < 2 || !ay_character_set_clip((ay_character_clip() + 1) % count)) return;
    }
    restart_playback();
    ESP_LOGI(tag, "CHARACTER name=%s clip=%s poses=%u faces=%u emotes=%u period_ms=%u",
             ay_character_name(ay_character_current()), ay_character_clip_label(ay_character_clip()),
             ay_character_pose_count(), ay_character_inspection_count(), ay_character_emote_count(),
             ay_character_period_ms());
}
#endif

static int navigate(ay_companion_command command)
{
    switch (command) {
#ifdef AYATSURI_CHARACTERS
    case AY_COMPANION_NEXT_BACKGROUND: {
        const unsigned count = ay_background_count();
        if (!count) return -1;
        background = (background + 1) % (count + 1);
        displayed.known = false;  /* the whole screen changes */
        probe.next_demo = 0;
        ESP_LOGI(tag, "BACKGROUND %u %s", background, background ? ay_background_name(background - 1) : "NONE");
        return -1;
    }
    case AY_COMPANION_NEXT_EMOTE:
        (void)ay_character_set_emote((ay_character_emote() + 1) % (ay_character_emote_count() + 1));
        ESP_LOGI(tag, "EMOTE %u %s", ay_character_emote(), ay_character_emote_label(ay_character_emote()));
        return -1;
    case AY_COMPANION_NEXT_CHARACTER:
    case AY_COMPANION_NEXT_CLIP:
        change_character(command);
        return -1;
#endif
    case AY_COMPANION_PREVIOUS_POSE:
    case AY_COMPANION_NEXT_POSE:
        probe.pose_stop = (probe.pose_stop + (command == AY_COMPANION_NEXT_POSE ? 1 : POSE_STOPS - 1))
                          % POSE_STOPS;
        return FIRST_MOTION_POSE + MOTION_POSES * probe.pose_stop / POSE_STOPS;
#ifdef COMPANION_MOTION
    case AY_COMPANION_PREVIOUS_EXPRESSION:
    case AY_COMPANION_NEXT_EXPRESSION:
        probe.expression = (probe.expression + (command == AY_COMPANION_NEXT_EXPRESSION ? 1 : FIRST_MOTION_POSE - 1))
                           % FIRST_MOTION_POSE;
        return probe.expression;
#endif
    case AY_COMPANION_PLAY:
        if (probe.held_pose >= 0) {
            if (probe.held_pose >= (int)FIRST_MOTION_POSE) probe.demo_pose = (unsigned)probe.held_pose;
            probe.held_pose = -1;
            probe.next_demo = 0;
            probe.result[0] = '\0';
        }
        break;
    default:
        break;
    }
    return -1;
}

static const char *playback_label(void)
{
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
    if (probe.network_requested || ayatsuri_companion_net_busy()) return "NET BENCH";
#endif
    return probe.result[0] ? probe.result : "PLAY";
}

int64_t ayatsuri_pose_dma_probe_step(uint32_t generation, ay_companion_command command, bool capture)
{
    if (!workspace_open()) return 0;
    if (generation != probe.generation) begin_companion(generation, DEMO);
#ifdef AYATSURI_CHARACTERS
    if (!ay_character_store_ready()) {
        /* The first boot of a firmware with new characters unpacks them into flash. */
        (void)present_pose(-1, "LOADING", NULL);
        ESP_LOGI(tag, "CHARACTER_STORE installing");
        const bool installed = ay_character_store_install();
        ESP_LOGI(tag, "CHARACTER_STORE %s", installed ? "ready" : "failed");
        if (!installed) (void)present_pose(-1, "NO CHARACTERS", NULL);
    }
#endif
    const int original_async = board_async_get();
    const bool is_capture = capture || board_capture_active();
    bool testing = probe.phase == PREFLIGHT || probe.phase == BENCHMARK;
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
    testing = testing || probe.network_requested || ayatsuri_companion_net_busy();
#endif
    int inspect_pose = -1;
    if (!is_capture && !testing) {
        if (command == AY_COMPANION_BENCHMARK) {
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
            if (ayatsuri_companion_net_reset())
#endif
            {
                ESP_LOGI(tag, "BENCH_START");
                begin_companion(generation, PREFLIGHT);
            }
        } else if (probe.phase == DEMO) {
#ifdef AYATSURI_RASTER_TEST
            if (command == AY_COMPANION_RASTER_TOGGLE) {
                probe.raster_scan = !probe.raster_scan;
                probe.raster_motion_logged = false;
                probe.next_demo = 0;
                ESP_LOGI(tag, "RASTER scan=%s", probe.raster_scan ? "on" : "off");
            }
#endif
            inspect_pose = navigate(command);
        }
    }
    if (!is_capture) board_async_set(1);
    bool select_pose = inspect_pose >= 0 && inspect_pose != probe.held_pose;
    /* A held still has to be drawn again when a toggle changes how it looks. */
    bool redraw_held = false;
#ifdef AYATSURI_RASTER_TEST
    redraw_held = redraw_held || command == AY_COMPANION_RASTER_TOGGLE;
#endif
#ifdef AYATSURI_CHARACTERS
    redraw_held = redraw_held || command == AY_COMPANION_NEXT_EMOTE || command == AY_COMPANION_NEXT_BACKGROUND;
#endif
    if (!is_capture && !testing && redraw_held && probe.held_pose >= 0) {
        inspect_pose = probe.held_pose;
        select_pose = true;
    }
    char pose_label[16];

#ifdef COMPANION_MOTION
    if (select_pose || is_capture) {
        probe.demo_first_drain = 0;
        probe.demo_intervals = 0;
        probe.demo_over_55ms = 0;
        probe.demo_worst = (demo_gap){0};
        probe.next_demo = 0;
    }
#endif
    if (is_capture) {
        int pose = -1;
        if (probe.phase == DEMO) pose = probe.displayed_pose >= 0 ? probe.displayed_pose : (int)probe.demo_pose;
        const char *label = probe.phase >= DEMO ? playback_label() : "RUNNING";
        if (probe.phase == DEMO && probe.held_pose >= 0)
            label = inspection_label(probe.held_pose, pose_label);
        (void)present_pose(pose, label, NULL);
        if (probe.phase == BENCHMARK) begin_companion(generation, PREFLIGHT);
    } else if (probe.phase == PREFLIGHT) {
        const bool banner_ready = probe.checked != 0 || present_pose(-1, "CHECKING", NULL);
        bool verified_pose = banner_ready && check_pose(probe.checked);
#ifdef COMPANION_MOTION
        if (verified_pose && probe.checked < 3) {
            verified_pose = present_pose((int)probe.checked, face_text(probe.checked), NULL);
            if (verified_pose) ESP_LOGI(tag, "FACE_DMA pose=%u status=PASS", probe.checked);
        }
#endif
        if (!verified_pose) {
            if (!banner_ready) ESP_LOGE(tag, "FAIL phase=preflight_banner");
            end_benchmark(false, 0);
            (void)present_pose(-1, probe.result, NULL);
        } else if (++probe.checked == PREFLIGHT_POSES) {
            probe.phase = BENCHMARK;
        }
    } else if (probe.phase == BENCHMARK) {
        if (!probe.completed) probe.started = esp_timer_get_time();
        const unsigned pose = FIRST_MOTION_POSE + probe.completed % (POSE_COUNT - FIRST_MOTION_POSE);
#ifdef COMPANION_MOTION
        const int64_t frame_start = esp_timer_get_time();
#endif
        const bool ok = present_pose((int)pose, "RUNNING", &probe.benchmark_timing);
        if (ok) ++probe.completed;
        const int64_t finished = esp_timer_get_time();
#ifdef COMPANION_MOTION
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
        const char *label = playback_label();
        if (select_pose) label = inspection_label(pose, pose_label);
        frame_timing *record = NULL;
#ifdef COMPANION_MOTION
        frame_timing timing = {0};
        if (!select_pose) record = &timing;
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
        const bool measure = !select_pose && (ayatsuri_companion_net_busy() || probe.network_requested);
        if (measure) {
            ay_net_measure_frame = true;
            ay_net_frame_worker_wait = (ay_net_worker_wait){0};
            lcd_spi_probe_begin();
        }
#endif
        const int64_t demo_begin = esp_timer_get_time();
        const int64_t demo_due = probe.next_demo;
        const int previous_pose = probe.displayed_pose;
#endif
        const bool presented = present_pose(pose, label, record);
#if defined(COMPANION_MOTION) && defined(CONFIG_AYATSURI_COMPANION_NET_PROBE)
        const int64_t drained = esp_timer_get_time();
        const uint32_t lcd_wait = measure ? lcd_spi_probe_end() : 0;
        ay_net_measure_frame = false;
#endif
        if (!presented) {
            ESP_LOGE(tag, "FAIL demo_transfer");
            probe.phase = FAILED;
            (void)present_pose(-1, "LCD FAILED", NULL);
        } else {
            if (probe.displayed_pose < 0 && !probe.result[0]) ESP_LOGI(tag, "COMPANION_READY");
            probe.displayed_pose = pose;
            if (select_pose) {
                probe.held_pose = pose;
                ESP_LOGI(tag, "NAV kind=%s pose=%d label=%s", pose < (int)FIRST_MOTION_POSE ? "expression" : "pose", pose, label);
            } else {
                probe.demo_pose = FIRST_MOTION_POSE + (probe.demo_pose - FIRST_MOTION_POSE + 1) % MOTION_POSES;
            }
#ifdef COMPANION_MOTION
            if (!select_pose) {
#ifndef CONFIG_AYATSURI_COMPANION_NET_PROBE
                const int64_t drained = esp_timer_get_time();
#endif
                demo_schedule((unsigned)pose, demo_begin, drained, timing.serial);
                demo_completed((unsigned)previous_pose, (unsigned)pose, demo_due,
                               demo_begin, drained, &timing);
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
                ay_net_frame frame = {
                    .started=demo_begin, .drained=drained, .prepare=timing.prepare_us,
                    .raster=timing.raster_us, .band_wait=ay_live_model_band_wait_us(), .serial=timing.serial,
                    .worker_wait=ay_net_frame_worker_wait, .lcd_wait=lcd_wait,
#ifdef CONFIG_AYATSURI_RENDER_TAIL_AB
                    .geometry=(uint32_t)timing.geometry_us, .submit=(uint32_t)timing.submit_us,
                    .final_drain=(uint32_t)timing.finish_us, .pose=(uint32_t)pose,
#endif
                };
#ifdef CONFIG_AYATSURI_RENDER_TAIL_AB
                if (measure) {
                    uint32_t phase_us[7];
                    ay_live_model_phase_us(phase_us);
                    frame.deform=phase_us[2];
                    frame.wrap=phase_us[4];
                    frame.provider=phase_us[5];
                }
#endif
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
                if (measure) lcd_spi_gdma_probe_frame(frame.crypto_hits);
#endif
#ifdef CONFIG_AYATSURI_LIVE_DMA_PROBE
                if (probe.network_requested == AY_NET_PROJECTION_REUSE && ay_live_model_projection_reuse)
                    ay_live_model_projection_reuse(true);
#endif
                ayatsuri_companion_net_frame(&frame, probe.network_requested);
                probe.network_requested = AY_NET_NO_REQUEST;
#endif
            }
#else
            probe.next_demo = esp_timer_get_time() + demo_interval_us;
#endif
        }
    }

    if (!is_capture) board_async_set(original_async);
#ifdef COMPANION_MOTION
    if (probe.phase == DEMO && probe.held_pose < 0 && !is_capture) return probe.next_demo;
#endif
    return 0;
}
