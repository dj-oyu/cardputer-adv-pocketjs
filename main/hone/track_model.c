#include "track_model.h"
#include "ayatsuri/ayatsuri_live_model_fixture_api.h"
#include "hone_track_asset.h"

#include <float.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "ayatsuri/ayatsuri_band_worker.h"
#endif

enum { WIDTH = 240, HEIGHT = 135, ROWS = 8, BANDS = (HEIGHT + ROWS - 1) / ROWS,
       WORDS = (HONE_TRACK_TRIANGLES + 31) / 32 };
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
_Static_assert(HONE_TRACK_VERTICES > 0 && HONE_TRACK_VERTICES <= UINT16_MAX &&
               HONE_TRACK_TRIANGLES > 0 && HONE_TRACK_TRIANGLES <= UINT16_MAX &&
               HONE_TRACK_CHANNELS > 0 && HONE_TRACK_CHANNELS <= UINT16_MAX &&
               HONE_TRACK_COLORS > 0 && HONE_TRACK_COLORS <= UINT16_MAX &&
               HONE_TRACK_KEYS > 0 && HONE_TRACK_KEYS <= UINT16_MAX &&
               HONE_TRACK_GROUPS > 0 && HONE_TRACK_GROUPS <= 256, "invalid track dimensions");
_Static_assert(COUNT(track_vertices) == HONE_TRACK_VERTICES && COUNT(track_triangles) == HONE_TRACK_TRIANGLES &&
               COUNT(track_delta) == HONE_TRACK_CHANNELS && COUNT(track_scale) == HONE_TRACK_CHANNELS &&
               COUNT(track_delta[0]) == HONE_TRACK_KEYS && COUNT(track_color) == HONE_TRACK_COLORS &&
               COUNT(track_color[0]) == HONE_TRACK_KEYS && COUNT(track_visible) == HONE_TRACK_KEYS &&
               COUNT(track_visible[0]) == (HONE_TRACK_GROUPS + 7) / 8 &&
               COUNT(track_poses) == AY_LIVE_MODEL_POSE_COUNT, "track table dimensions disagree");

typedef struct { int16_t x, y, z; } position;
typedef struct { unsigned first, end; } span;
static position vertices[HONE_TRACK_VERTICES];
static uint16_t colors[HONE_TRACK_COLORS];
/* Bitsets keep RAM bounded even when a triangle moves across many bands.
 * Each band replays its visible faces in original submission order. */
static uint32_t masks[BANDS][WORDS];
static span extents[BANDS];
static bool active, checked;
static unsigned cached_y = HEIGHT;
static uint16_t cached_pixels[ROWS * WIDTH];
static float local_depth[ROWS * WIDTH];
static uint32_t wait_us;
#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_NUMBER_OF_CORES > 1
static ay_band_worker worker;
static float worker_depth[ROWS * WIDTH];
static bool serial_raster;
#endif

static int endpoint(const hone_track_vertex *v, unsigned axis, unsigned key)
{
    unsigned channel = v->channel[axis];
    return v->base[axis] + (channel ? track_scale[channel] * (int)track_delta[channel][key] : 0);
}

static bool valid_asset(void)
{
    for (unsigned i = 0; i < COUNT(track_poses); ++i) {
        if (track_poses[i].first >= HONE_TRACK_KEYS || track_poses[i].second >= HONE_TRACK_KEYS) return false;
    }
    for (unsigned i = 0; i < HONE_TRACK_TRIANGLES; ++i) {
        const hone_track_triangle *t = &track_triangles[i];
        if (t->vertex[0] >= HONE_TRACK_VERTICES || t->vertex[1] >= HONE_TRACK_VERTICES ||
            t->vertex[2] >= HONE_TRACK_VERTICES || t->color_channel >= HONE_TRACK_COLORS ||
            t->group >= HONE_TRACK_GROUPS) return false;
    }
    for (unsigned i = 0; i < HONE_TRACK_VERTICES; ++i) {
        const hone_track_vertex *v = &track_vertices[i];
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (v->channel[axis] >= HONE_TRACK_CHANNELS) return false;
            for (unsigned key = 0; key < (v->channel[axis] ? HONE_TRACK_KEYS : 1u); ++key) {
                int value = endpoint(v, axis, key);
                if (axis < 2 ? (value < -4096 || value > 8191) : (value < INT16_MIN || value > INT16_MAX))
                    return false;
            }
        }
    }
    return true;
}

/* Round the offset to the nearest integer. Division by 255 has no half tie. */
static int mix(int a, int b, unsigned blend)
{
    int delta = (b - a) * (int)blend;
    return a + (delta + (delta >= 0 ? 127 : -127)) / 255;
}

static int coordinate(const hone_track_vertex *v, unsigned axis, hone_track_pose pose)
{
    int a = endpoint(v, axis, pose.first);
    if (!v->channel[axis] || !pose.blend || pose.first == pose.second) return a;
    return mix(a, endpoint(v, axis, pose.second), pose.blend);
}

static uint16_t mix_color(uint16_t a, uint16_t b, unsigned blend)
{
    if (a == b || !blend) return a;
    return (uint16_t)((mix(a >> 11, b >> 11, blend) << 11) |
                      (mix((a >> 5) & 63, (b >> 5) & 63, blend) << 5) |
                      mix(a & 31, b & 31, blend));
}

static int minimum(int a, int b) { return a < b ? a : b; }
static int maximum(int a, int b) { return a > b ? a : b; }

static int edge(position a, position b, int x, int y)
{
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}

/* Half-open range of integer pixel centres. Coordinates are Q4 and the
 * admitted range bounds every 32-bit edge determinant to less than 2^29. */
static span bounds(int a, int b, int c, int extent)
{
    int lo = minimum(minimum(a, b), c), hi = maximum(maximum(a, b), c);
    if (hi < 0 || lo > (extent - 1) * 16) return (span){0};
    return (span){(unsigned)((maximum(lo, 0) + 15) / 16), (unsigned)minimum(hi / 16 + 1, extent)};
}

static unsigned index_frame(hone_track_pose pose)
{
    for (unsigned i = 0; i < HONE_TRACK_VERTICES; ++i) {
        const hone_track_vertex *v = &track_vertices[i];
        vertices[i] = (position){coordinate(v, 0, pose), coordinate(v, 1, pose), coordinate(v, 2, pose)};
    }
    for (unsigned i = 1; i < HONE_TRACK_COLORS; ++i)
        colors[i] = mix_color(track_color[i][pose.first], track_color[i][pose.second], pose.blend);
    memset(masks, 0, sizeof masks);
    memset(extents, 0, sizeof extents);
    const uint8_t *visible = track_visible[pose.blend < 128 ? pose.first : pose.second];
    unsigned count = 0;
    for (unsigned i = 0; i < HONE_TRACK_TRIANGLES; ++i) {
        const hone_track_triangle *t = &track_triangles[i];
        if (!(visible[t->group / 8] & (1u << (t->group % 8)))) continue;
        position a = vertices[t->vertex[0]], b = vertices[t->vertex[1]], c = vertices[t->vertex[2]];
        if (!edge(a,b,c.x,c.y)) continue;
        span xs = bounds(a.x,b.x,c.x,WIDTH), ys = bounds(a.y,b.y,c.y,HEIGHT);
        if (xs.first >= xs.end || ys.first >= ys.end) continue;
        ++count;
        for (unsigned band = ys.first / ROWS; band <= (ys.end - 1) / ROWS; ++band) {
            masks[band][i / 32] |= UINT32_C(1) << (i % 32);
            span *range = &extents[band];
            if (!range->end || xs.first < range->first) range->first = xs.first;
            if (xs.end > range->end) range->end = xs.end;
        }
    }
    return count;
}

static int edge_min(position a, position b)
{
    return !(b.y < a.y || (b.y == a.y && b.x > a.x));
}

static void raster_triangle(const hone_track_triangle *t, unsigned first_y, unsigned end_y,
                            float *depth, uint16_t *pixels)
{
    position a = vertices[t->vertex[0]], b = vertices[t->vertex[1]], c = vertices[t->vertex[2]];
    int area = edge(a,b,c.x,c.y);
    if (area < 0) {
        position swap = b; b = c; c = swap;
        area = -area;
    }
    span xs = bounds(a.x,b.x,c.x,WIDTH), ys = bounds(a.y,b.y,c.y,HEIGHT);
    unsigned top = ys.first > first_y ? ys.first : first_y;
    unsigned bottom = ys.end < end_y ? ys.end : end_y;
    int x = (int)xs.first * 16, y = (int)top * 16;
    int e0 = edge(b,c,x,y), e1 = edge(c,a,x,y), e2 = edge(a,b,x,y);
    int dx0 = (b.y - c.y) * 16, dx1 = (c.y - a.y) * 16, dx2 = (a.y - b.y) * 16;
    int dy0 = (c.x - b.x) * 16, dy1 = (a.x - c.x) * 16, dy2 = (b.x - a.x) * 16;
    float inv = 1.0f / (float)area;
    float z1 = (float)(b.z - a.z) * inv, z2 = (float)(c.z - a.z) * inv;
    int limit0 = edge_min(b,c), limit1 = edge_min(c,a), limit2 = edge_min(a,b);
    uint16_t rgb = t->color_channel ? colors[t->color_channel] : t->rgb565;
    for (unsigned row = top; row < bottom; ++row) {
        int w0 = e0, w1 = e1, w2 = e2;
        unsigned i = (row - first_y) * WIDTH + xs.first;
        for (unsigned col = xs.first; col < xs.end; ++col, ++i) {
            if (w0 >= limit0 && w1 >= limit1 && w2 >= limit2) {
                float z = (float)a.z + (float)w1 * z1 + (float)w2 * z2;
                if (z > depth[i]) { depth[i] = z; pixels[i] = rgb; }
            }
            w0 += dx0; w1 += dx1; w2 += dx2;
        }
        e0 += dy0; e1 += dy1; e2 += dy2;
    }
}

static void raster_band(unsigned y, float *depth, uint16_t *pixels)
{
    unsigned end_y = (unsigned)minimum(y + ROWS, HEIGHT), band = y / ROWS;
    memset(pixels, 0, (end_y - y) * WIDTH * sizeof *pixels);
    span xs = extents[band];
    for (unsigned row = 0; row < end_y - y; ++row)
        for (unsigned x = xs.first; x < xs.end; ++x) depth[row * WIDTH + x] = -FLT_MAX;
    for (unsigned w = 0; w < WORDS; ++w) {
        uint32_t bits = masks[band][w];
        while (bits) {
            unsigned face = 32 * w + (unsigned)__builtin_ctz(bits);
            raster_triangle(&track_triangles[face], y, end_y, depth, pixels);
            bits &= bits - 1;
        }
    }
}

#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_NUMBER_OF_CORES > 1
static bool render_worker(void *unused, unsigned y)
{
    (void)unused;
    raster_band(y, worker_depth, cached_pixels);
    return true;
}

static bool join_worker(void)
{
    if (!worker.pending) return true;
    int64_t start = esp_timer_get_time();
    bool ok = ay_band_worker_join(&worker);
    wait_us += (uint32_t)(esp_timer_get_time() - start);
    cached_y = ok ? worker.band : HEIGHT;
    return ok;
}
#else
static bool join_worker(void) { return true; }
#endif

bool ay_live_model_prepare(unsigned pose, unsigned *packets, bool *fast, uint32_t *geometry_us)
{
    if (active || pose >= COUNT(track_poses) || !packets || !fast || !geometry_us) return false;
    if (!checked && !valid_asset()) return false;
    checked = true;
#ifdef ESP_PLATFORM
    int64_t start = esp_timer_get_time();
#endif
    unsigned count = index_frame(track_poses[pose]);
    *packets = count;
    *fast = true;
    *geometry_us = 0;
#ifdef ESP_PLATFORM
    *geometry_us = (uint32_t)(esp_timer_get_time() - start);
#endif
    active = true;
    wait_us = 0;
    cached_y = HEIGHT;
    return true;
}

bool ay_live_model_band(unsigned y, uint16_t *pixels, unsigned *left, unsigned *right)
{
    if (!active || y >= HEIGHT || y % ROWS || !pixels || !left || !right || left == right ||
        !join_worker()) return false;
    if (cached_y == y) {
        size_t rows = (size_t)minimum(y + ROWS, HEIGHT) - y;
        memcpy(pixels, cached_pixels, rows * WIDTH * sizeof *pixels);
    } else {
#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_NUMBER_OF_CORES > 1
        if (!serial_raster && y % (2 * ROWS) == 0 && y + ROWS < HEIGHT &&
            ay_band_worker_start(&worker, render_worker, NULL, y + ROWS)) cached_y = HEIGHT;
#endif
        raster_band(y, local_depth, pixels);
    }
    *left = extents[y / ROWS].first;
    *right = extents[y / ROWS].end;
    return true;
}

bool ay_live_model_row(unsigned y, uint16_t pixels[240])
{
    if (!active || y >= HEIGHT || !pixels || !join_worker()) return false;
    unsigned top = y - y % ROWS;
    if (cached_y != top) {
        raster_band(top, local_depth, cached_pixels);
        cached_y = top;
    }
    memcpy(pixels, &cached_pixels[(y - top) * WIDTH], WIDTH * sizeof *pixels);
    return true;
}

void ay_live_model_serial_raster(bool serial)
{
#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    serial_raster = serial;
#else
    (void)serial;
#endif
}

uint32_t ay_live_model_band_wait_us(void) { return wait_us; }

bool ay_live_model_finish(bool drained)
{
    if (!join_worker() || !drained) return false;
    active = false;
    return true;
}

bool ay_live_model_close(void)
{
    if (active) return false;
#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    ay_band_worker_close(&worker);
#endif
    return true;
}
