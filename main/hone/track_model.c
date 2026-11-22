#include "track_model.h"
#include "track_character.h"
#include "ayatsuri/ayatsuri_live_model_fixture_api.h"
#ifdef AYATSURI_TRACK_MODEL
#include "hone_track_asset.h"
#endif

#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "ayatsuri/ayatsuri_band_worker.h"
#endif

enum { WIDTH = 240, HEIGHT = 135, ROWS = 8, BANDS = (HEIGHT + ROWS - 1) / ROWS,
       MAX_GROUPS = 256, HCA_HEADER = 144, HCA_COUNTS = 11, HCA_OFFSETS = 10, LABEL = 12, MAX_CHARACTERS = 16 };
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* One character, either parsed from an HCA image or described by the
 * compile-time fox tables. The raster path only ever sees this view. */
typedef struct { uint16_t first_pose, poses, period_ms; char name[17]; } track_clip;
typedef struct {
    uint16_t vertices, triangles, keys, channels, groups, colors, poses, faces, emotes, clips, preset_colors;
    const hone_track_vertex *vertex;
    const hone_track_triangle *triangle;
    const int8_t *delta;          /* [channels][keys] */
    const uint16_t *scale;        /* [channels] */
    const uint16_t *color;        /* [colors][keys] */
    const uint16_t *preset_color; /* HCA: [preset_colors][faces], chosen by the face */
    const uint8_t *visible;       /* fox: [keys][(groups+7)/8] */
    const uint16_t *group_mask;   /* HCA: {preset mask, emote requirement} per group */
    const hone_track_pose *pose;  /* fox: every pose; HCA: clip frames */
    const char *labels;           /* HCA: faces then emotes, LABEL bytes each */
    track_clip clip[16];
    char name[17];
    char sha[65];
} track_asset;

typedef struct { int16_t x, y, z; } position;
typedef struct { unsigned first, end; } span;

static track_asset asset;
static bool loaded, validated[MAX_CHARACTERS];
static unsigned selected, face, emote, clip_index;
static uint8_t visible_groups[MAX_GROUPS / 8];

static position *vertices;
static uint16_t *colors;
static uint32_t *masks;        /* [BANDS][words] */
static unsigned words;
static span extents[BANDS];
static bool active;
static unsigned cached_y = HEIGHT;
static uint16_t cached_pixels[ROWS * WIDTH];
static float local_depth[ROWS * WIDTH];
static uint32_t wait_us;
#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_NUMBER_OF_CORES > 1
static ay_band_worker worker;
static float worker_depth[ROWS * WIDTH];
static bool serial_raster;
#endif

static void *grab(size_t bytes)
{
#ifdef ESP_PLATFORM
    return heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return calloc(1, bytes);
#endif
}

static void drop(void *p)
{
#ifdef ESP_PLATFORM
    heap_caps_free(p);
#else
    free(p);
#endif
}

/* ---- asset views ------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static const char *hca_parse(const uint8_t *data, size_t size, track_asset *out)
{
    if (!data || size < HCA_HEADER) return "short header";
    if (memcmp(data, "HCA1", 4) || rd16(data + 4) != 2) return "bad magic or version";
    if ((uintptr_t)data & 3u) return "image is not word aligned";
    uint32_t n[HCA_COUNTS], off[HCA_OFFSETS];
    for (unsigned i = 0; i < HCA_COUNTS; ++i) n[i] = rd32(data + 8 + 4 * i);
    for (unsigned i = 0; i < HCA_OFFSETS; ++i) off[i] = rd32(data + 52 + 4 * i);
    /* n: vertices triangles keys channels groups colors poses clips faces emotes preset_colors */
    if (!n[0] || n[0] > UINT16_MAX || !n[1] || n[1] > UINT16_MAX || !n[2] || n[2] > UINT16_MAX ||
        !n[3] || n[3] > UINT16_MAX || !n[4] || n[4] > MAX_GROUPS || !n[5] || n[5] > 0x7fff ||
        !n[6] || n[6] > UINT16_MAX || !n[7] || n[7] > COUNT(out->clip) || !n[8] || n[8] > 32 || n[9] > 32 ||
        n[10] > 0x7fff)
        return "count out of range";
    uint64_t need[HCA_OFFSETS] = {(uint64_t)n[0] * sizeof(hone_track_vertex), (uint64_t)n[1] * sizeof(hone_track_triangle),
                        (uint64_t)n[3] * n[2], (uint64_t)n[3] * 2, (uint64_t)n[5] * n[2] * 2,
                        (uint64_t)n[4] * 4, (uint64_t)n[6] * sizeof(hone_track_pose), (uint64_t)n[7] * 24,
                        (uint64_t)(n[8] + n[9]) * LABEL, (uint64_t)n[10] * n[8] * 2};
    for (unsigned i = 0; i < HCA_OFFSETS; ++i) {
        if (off[i] & 3u || off[i] < HCA_HEADER || (uint64_t)off[i] + need[i] > size) return "table outside image";
    }
    memset(out, 0, sizeof *out);
    out->vertices = (uint16_t)n[0]; out->triangles = (uint16_t)n[1]; out->keys = (uint16_t)n[2];
    out->channels = (uint16_t)n[3]; out->groups = (uint16_t)n[4]; out->colors = (uint16_t)n[5];
    out->poses = (uint16_t)n[6]; out->clips = (uint16_t)n[7]; out->faces = (uint16_t)n[8]; out->emotes = (uint16_t)n[9];
    out->preset_colors = (uint16_t)n[10];
    out->vertex = (const hone_track_vertex *)(data + off[0]);
    out->triangle = (const hone_track_triangle *)(data + off[1]);
    out->delta = (const int8_t *)(data + off[2]);
    out->scale = (const uint16_t *)(data + off[3]);
    out->color = (const uint16_t *)(data + off[4]);
    out->group_mask = (const uint16_t *)(data + off[5]);
    out->pose = (const hone_track_pose *)(data + off[6]);
    out->labels = (const char *)(data + off[8]);
    out->preset_color = (const uint16_t *)(data + off[9]);
    for (unsigned i = 0; i < n[7]; ++i) {
        const uint8_t *c = data + off[7] + 24 * i;
        track_clip *clip = &out->clip[i];
        memcpy(clip->name, c, 16);
        clip->name[16] = 0;
        clip->first_pose = rd16(c + 16); clip->poses = rd16(c + 18); clip->period_ms = rd16(c + 20);
        if (!clip->poses || clip->poses > 320 || (unsigned)clip->first_pose + clip->poses > n[6]) return "clip outside poses";
    }
    memcpy(out->name, data + 124, 16);
    for (unsigned i = 0; i < 32; ++i) {
        static const char hex[] = "0123456789abcdef";
        out->sha[2 * i] = hex[data[92 + i] >> 4];
        out->sha[2 * i + 1] = hex[data[92 + i] & 15];
    }
    return NULL;
}

#ifdef AYATSURI_TRACK_MODEL
static void fox_view(track_asset *out)
{
    memset(out, 0, sizeof *out);
    out->vertices = HONE_TRACK_VERTICES; out->triangles = HONE_TRACK_TRIANGLES; out->keys = HONE_TRACK_KEYS;
    out->channels = HONE_TRACK_CHANNELS; out->groups = HONE_TRACK_GROUPS; out->colors = HONE_TRACK_COLORS;
    out->poses = AY_LIVE_MODEL_POSE_COUNT; out->faces = AY_COMPANION_INSPECTION_POSES;
    out->vertex = track_vertices; out->triangle = track_triangles;
    out->delta = &track_delta[0][0]; out->scale = track_scale; out->color = &track_color[0][0];
    out->visible = &track_visible[0][0]; out->pose = track_poses;
    out->clips = 1;
    out->clip[0] = (track_clip){AY_COMPANION_INSPECTION_POSES, AY_COMPANION_MOTION_POSES - AY_COMPANION_INSPECTION_POSES, 48, "motion"};
    memcpy(out->name, "fox", 4);
    memcpy(out->sha, AY_LIVE_MODEL_SHA256, strlen(AY_LIVE_MODEL_SHA256) < 64 ? strlen(AY_LIVE_MODEL_SHA256) + 1 : 64);
}
#endif

static bool fox_present(void)
{
#ifdef AYATSURI_TRACK_MODEL
    return true;
#else
    return false;
#endif
}

static int endpoint_of(const track_asset *a, const hone_track_vertex *v, unsigned axis, unsigned key)
{
    unsigned channel = v->channel[axis];
    return v->base[axis] + (channel ? a->scale[channel] * (int)a->delta[(size_t)channel * a->keys + key] : 0);
}

static const char *valid_asset(const track_asset *a)
{
    for (unsigned i = 0; i < a->poses; ++i) {
        if (a->pose[i].first >= a->keys || a->pose[i].second >= a->keys) return "pose key out of range";
    }
    for (unsigned i = 0; i < a->triangles; ++i) {
        const hone_track_triangle *t = &a->triangle[i];
        if (t->vertex[0] >= a->vertices || t->vertex[1] >= a->vertices || t->vertex[2] >= a->vertices ||
            t->group >= a->groups ||
            ((t->color_channel & 0x8000) ? (t->color_channel & 0x7fff) >= a->preset_colors : t->color_channel >= a->colors))
            return "triangle reference out of range";
    }
    for (unsigned i = 0; i < a->vertices; ++i) {
        const hone_track_vertex *v = &a->vertex[i];
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (v->channel[axis] >= a->channels) return "channel out of range";
            for (unsigned key = 0; key < (v->channel[axis] ? a->keys : 1u); ++key) {
                int value = endpoint_of(a, v, axis, key);
                if (axis < 2 ? (value < -4096 || value > 8191) : (value < INT16_MIN || value > INT16_MAX))
                    return "coordinate outside the admitted range";
            }
        }
    }
    return NULL;
}

/* ---- selection -------------------------------------------------------- */

static bool pinned(void) { return active; }

static void release_buffers(void)
{
    drop(vertices); drop(colors); drop(masks);
    vertices = NULL; colors = NULL; masks = NULL;
    loaded = false;
}

static const char *character_view(unsigned index, track_asset *out)
{
    unsigned blob = index;
    if (fox_present()) {
        if (index == 0) {
#ifdef AYATSURI_TRACK_MODEL
            fox_view(out);
#endif
            return NULL;
        }
        blob = index - 1;
    }
    if (blob >= ay_character_blob_count) return "no such character";
    size_t size = 0;
    const uint8_t *image = ay_character_image(blob, &size);
    if (!image) return "character image unavailable";
    return hca_parse(image, size, out);
}

unsigned ay_character_count(void) { return ay_character_blob_count + (fox_present() ? 1u : 0u); }

const char *ay_character_name(unsigned index)
{
    if (index >= ay_character_count()) return "";
    if (fox_present()) {
        if (index == 0) return "fox";
        index -= 1;
    }
    return ay_character_blobs[index].name;
}

unsigned ay_character_current(void) { return selected; }

const char *ay_character_check(const uint8_t *data, size_t size)
{
    static track_asset probe;
    const char *why = hca_parse(data, size, &probe);
    return why ? why : valid_asset(&probe);
}

/* Mapping a different image replaces the mapping the loaded asset points into.
 * After a failed switch, point the loaded asset at the mapping again. */
static void reattach(void)
{
    track_asset again;
    if (loaded && !character_view(selected, &again)) {
        asset = again;
    }
}

static bool open_character(unsigned index)
{
    if (index >= ay_character_count() || index >= MAX_CHARACTERS) return false;
    track_asset next;
    if (character_view(index, &next)) {
        reattach();
        return false;
    }
    if (!validated[index]) {
        if (valid_asset(&next)) {
            reattach();
            return false;
        }
        validated[index] = true;
    }
    unsigned word_count = (next.triangles + 31u) / 32u;
    position *v = grab((size_t)next.vertices * sizeof *v);
    uint16_t *c = grab(((size_t)next.colors + next.preset_colors) * sizeof *c);
    uint32_t *m = grab((size_t)BANDS * word_count * sizeof *m);
    if (!v || !c || !m) {
        drop(v); drop(c); drop(m);
        reattach();
        return false;
    }
    release_buffers();
    asset = next;
    vertices = v; colors = c; masks = m; words = word_count;
    selected = index;
    loaded = true;
    face = 0; emote = 0; clip_index = 0;
    cached_y = HEIGHT;
    return true;
}

bool ay_character_select(unsigned index)
{
    if (pinned()) return false;
    if (loaded && index == selected) return true;
    return open_character(index);
}

static bool ensure(void) { return loaded || open_character(selected); }

/* ---- character state -------------------------------------------------- */

static bool is_fox(void) { return asset.visible != NULL; }

unsigned ay_character_pose_count(void)
{
    if (!ensure()) return 0;
    return is_fox() ? asset.poses : asset.faces + asset.clip[clip_index].poses;
}

unsigned ay_character_inspection_count(void) { return ensure() ? asset.faces : 0; }
unsigned ay_character_face(void) { return face; }

bool ay_character_set_face(unsigned preset)
{
    if (!ensure() || preset >= asset.faces) return false;
    face = preset;
    return true;
}

const char *ay_character_face_label(unsigned preset)
{
    if (!ensure() || preset >= asset.faces) return "";
    return is_fox() ? "" : asset.labels + (size_t)preset * LABEL;
}

unsigned ay_character_emote_count(void) { return ensure() ? asset.emotes : 0; }
unsigned ay_character_emote(void) { return emote; }

bool ay_character_set_emote(unsigned value)
{
    if (!ensure() || value > asset.emotes) return false;
    emote = value;
    return true;
}

const char *ay_character_emote_label(unsigned value)
{
    if (!ensure() || !value || value > asset.emotes) return "";
    return asset.labels + (size_t)(asset.faces + value - 1) * LABEL;
}

unsigned ay_character_clip_count(void) { return ensure() ? asset.clips : 0; }
unsigned ay_character_clip(void) { return clip_index; }

bool ay_character_set_clip(unsigned value)
{
    if (pinned() || !ensure() || value >= asset.clips) return false;
    clip_index = value;
    return true;
}

const char *ay_character_clip_label(unsigned value)
{
    return ensure() && value < asset.clips ? asset.clip[value].name : "";
}

unsigned ay_character_period_ms(void) { return ensure() ? asset.clip[clip_index].period_ms : 48u; }
const char *ay_character_sha256(void) { return ensure() ? asset.sha : ""; }

/* ---- rasterizer ------------------------------------------------------- */

/* Round the offset to the nearest integer. Division by 255 has no half tie. */
static int mix(int a, int b, unsigned blend)
{
    int delta = (b - a) * (int)blend;
    return a + (delta + (delta >= 0 ? 127 : -127)) / 255;
}

static int coordinate(const hone_track_vertex *v, unsigned axis, hone_track_pose pose)
{
    int a = endpoint_of(&asset, v, axis, pose.first);
    if (!v->channel[axis] || !pose.blend || pose.first == pose.second) return a;
    return mix(a, endpoint_of(&asset, v, axis, pose.second), pose.blend);
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

/* Which source pose a still or clip frame names, and which face it shows. */
static hone_track_pose resolve(unsigned pose)
{
    if (is_fox()) return asset.pose[pose];
    if (pose < asset.faces) return (hone_track_pose){0, 0, 0};
    return asset.pose[asset.clip[clip_index].first_pose + pose - asset.faces];
}

static void select_groups(hone_track_pose pose)
{
    memset(visible_groups, 0, sizeof visible_groups);
    if (is_fox()) {
        size_t row = (size_t)(pose.blend < 128 ? pose.first : pose.second) * ((asset.groups + 7u) / 8u);
        memcpy(visible_groups, asset.visible + row, (asset.groups + 7u) / 8u);
        return;
    }
    unsigned overlay = emote ? 1u << (emote - 1) : 0;
    for (unsigned g = 0; g < asset.groups; ++g) {
        unsigned mask = asset.group_mask[2 * g], need = asset.group_mask[2 * g + 1];
        if ((mask >> face & 1u) && (!need || (need & overlay))) visible_groups[g / 8] |= (uint8_t)(1u << (g % 8));
    }
}

static unsigned index_frame(hone_track_pose pose)
{
    for (unsigned i = 0; i < asset.vertices; ++i) {
        const hone_track_vertex *v = &asset.vertex[i];
        vertices[i] = (position){coordinate(v, 0, pose), coordinate(v, 1, pose), coordinate(v, 2, pose)};
    }
    for (unsigned i = 1; i < asset.colors; ++i)
        colors[i] = mix_color(asset.color[(size_t)i * asset.keys + pose.first],
                              asset.color[(size_t)i * asset.keys + pose.second], pose.blend);
    for (unsigned i = 0; i < asset.preset_colors; ++i)
        colors[asset.colors + i] = asset.preset_color[(size_t)i * asset.faces + face];
    memset(masks, 0, (size_t)BANDS * words * sizeof *masks);
    memset(extents, 0, sizeof extents);
    select_groups(pose);
    unsigned count = 0;
    for (unsigned i = 0; i < asset.triangles; ++i) {
        const hone_track_triangle *t = &asset.triangle[i];
        if (!(visible_groups[t->group / 8] & (1u << (t->group % 8)))) continue;
        position a = vertices[t->vertex[0]], b = vertices[t->vertex[1]], c = vertices[t->vertex[2]];
        if (!edge(a,b,c.x,c.y)) continue;
        span xs = bounds(a.x,b.x,c.x,WIDTH), ys = bounds(a.y,b.y,c.y,HEIGHT);
        if (xs.first >= xs.end || ys.first >= ys.end) continue;
        ++count;
        for (unsigned band = ys.first / ROWS; band <= (ys.end - 1) / ROWS; ++band) {
            masks[(size_t)band * words + i / 32] |= UINT32_C(1) << (i % 32);
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
    const unsigned cc = t->color_channel;
    uint16_t rgb = !cc ? t->rgb565 : colors[(cc & 0x8000) ? asset.colors + (cc & 0x7fff) : cc];
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
    for (unsigned w = 0; w < words; ++w) {
        uint32_t bits = masks[(size_t)band * words + w];
        while (bits) {
            unsigned face_index = 32 * w + (unsigned)__builtin_ctz(bits);
            raster_triangle(&asset.triangle[face_index], y, end_y, depth, pixels);
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
    if (active || !packets || !fast || !geometry_us || !ensure() || pose >= ay_character_pose_count()) return false;
    if (!is_fox() && pose < asset.faces) face = pose;
#ifdef ESP_PLATFORM
    int64_t start = esp_timer_get_time();
#endif
    unsigned count = index_frame(resolve(pose));
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
    release_buffers();
    ay_character_image_release();
    return true;
}
