#include "ksn_proc_grid_resize.h"
#include "ksn_proc_grid_pie.h"

#include <string.h>

#ifdef KASANE_PROC_DEVICE_PROBE
bool ksn_grid_resize_fast_weights = false;
bool ksn_grid_resize_shared_qr = false;
static ksn_grid_resize_routes resize_routes;
void ksn_grid_resize_routes_read(ksn_grid_resize_routes *out)
{
    *out = resize_routes;
    resize_routes = (ksn_grid_resize_routes){0};
}
#define RESIZE_ROUTE(kind) (++resize_routes.kind)
#else
#define RESIZE_ROUTE(kind) ((void)0)
#endif

static void axis(unsigned source, unsigned output, uint16_t *base,
                 uint8_t *fraction)
{
    for (unsigned i = 0; i < output; ++i) {
        int32_t numerator = (int32_t)(2u * i + 1u) * (int32_t)source -
                            (int32_t)output;
        int32_t denominator = (int32_t)(2u * output);
        if (numerator <= 0 || source == 1) {
            base[i] = 0; fraction[i] = 0;
        } else if (numerator >= (int32_t)(source - 1u) * denominator) {
            base[i] = (uint16_t)(source - 1u); fraction[i] = 0;
        } else {
            unsigned cell = (unsigned)(numerator / denominator);
            unsigned remainder = (unsigned)(numerator % denominator);
            unsigned phase = (remainder * 128u + (unsigned)denominator / 2u) /
                             (unsigned)denominator;
            if (phase == 128u) { ++cell; phase = 0; }
            base[i] = (uint16_t)cell;
            fraction[i] = (uint8_t)phase;
        }
    }
}

bool ksn_grid_resize_prepare_stream(ksn_grid_resize_plan *plan,
                                    unsigned source_width, unsigned source_height,
                                    unsigned width, unsigned height)
{
    if (!plan || !source_width || !source_height || !width || !height ||
        source_width > 256 || source_height > 256 ||
        width > 256 || height > 256)
        return false;
    memset(plan, 0, sizeof *plan);
    plan->source_width = (uint16_t)source_width;
    plan->source_height = (uint16_t)source_height;
    plan->width = (uint16_t)width;
    plan->height = (uint16_t)height;
    axis(source_width, width, plan->x0, plan->fx);
    axis(source_height, height, plan->y0, plan->fy);
    return true;
}

bool ksn_grid_resize_prepare(ksn_grid_resize_plan *plan,
                             unsigned source_width, unsigned source_height,
                             unsigned width, unsigned height)
{
    return source_width <= 256 && source_height <= 256 &&
           width <= 256 && height <= 256 &&
           source_width * source_height <= 8192u &&
           width * height <= 4096u &&
           ksn_grid_resize_prepare_stream(plan, source_width, source_height,
                                          width, height);
}

bool ksn_grid_resize_pie_available(void)
{
#if KSN_GRID_HAS_PIE || defined(KSN_GRID_PIE_MODEL)
    return true;
#else
    return false;
#endif
}

static unsigned component(uint16_t color, unsigned channel)
{
    return channel == 0 ? (color >> 11) & 31u :
           channel == 1 ? (color >> 5) & 63u : color & 31u;
}

static uint16_t combine(const int16_t channel[3][8], unsigned lane)
{
    unsigned red = (unsigned)channel[0][lane];
    unsigned green = (unsigned)channel[1][lane];
    unsigned blue = (unsigned)channel[2][lane];
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

#if KSN_GRID_HAS_PIE
static void pie_channel(const int16_t values[4][8],
                        const int16_t weights[4][8], int16_t result[8])
{
    __asm__ volatile("ee.zero.qacc\n" : : : "memory");
    for (unsigned tap = 0; tap < 4; ++tap) {
        const int16_t *v = values[tap], *w = weights[tap];
        __asm__ volatile(
            "ee.vld.128.ip q0, %[values], 0\n"
            "ee.vld.128.ip q1, %[weights], 0\n"
            "ee.vmulas.s16.qacc q0, q1\n"
            : : [values] "a"(v), [weights] "a"(w) : "memory");
    }
    int shift = 14;
    __asm__ volatile(
        "ee.srcmb.s16.qacc q2, %[shift], 0\n"
        "ee.vst.128.ip q2, %[result], 0\n"
        : : [shift] "a"(shift), [result] "a"(result) : "memory");
}

/* Only the coefficients stay in QR; one channel of scratch is reused so
 * sparse/dense mixing never expands all three channels ahead of time. */
static void pie_weights_load(const int16_t weights[4][8])
{
    const int16_t *w0 = weights[0], *w1 = weights[1];
    const int16_t *w2 = weights[2], *w3 = weights[3];
    __asm__ volatile(
        "ee.vld.128.ip q3, %[w0], 0\n"
        "ee.vld.128.ip q4, %[w1], 0\n"
        "ee.vld.128.ip q5, %[w2], 0\n"
        "ee.vld.128.ip q6, %[w3], 0\n"
        : : [w0] "a"(w0), [w1] "a"(w1),
            [w2] "a"(w2), [w3] "a"(w3) : "memory");
}

static void pie_channel_loaded(const int16_t values[4][8], int16_t result[8])
{
    const int16_t *v0 = values[0], *v1 = values[1];
    const int16_t *v2 = values[2], *v3 = values[3];
    int shift = 14;
    __asm__ volatile(
        "ee.zero.qacc\n"
        "ee.vld.128.ip q0, %[v0], 0\n"
        "ee.vmulas.s16.qacc q0, q3\n"
        "ee.vld.128.ip q0, %[v1], 0\n"
        "ee.vmulas.s16.qacc q0, q4\n"
        "ee.vld.128.ip q0, %[v2], 0\n"
        "ee.vmulas.s16.qacc q0, q5\n"
        "ee.vld.128.ip q0, %[v3], 0\n"
        "ee.vmulas.s16.qacc q0, q6\n"
        "ee.srcmb.s16.qacc q2, %[shift], 0\n"
        "ee.vst.128.ip q2, %[result], 0\n"
        : : [v0] "a"(v0), [v1] "a"(v1), [v2] "a"(v2),
            [v3] "a"(v3), [shift] "a"(shift), [result] "a"(result)
        : "memory");
}
#endif

/* The diagnostic keeps dense scratch separate for controlled A/B timing;
 * the shipping build inlines the original single-block layout. */
#ifdef KASANE_PROC_DEVICE_PROBE
#define RESIZE_DENSE_LINK __attribute__((noinline))
#else
#define RESIZE_DENSE_LINK inline __attribute__((always_inline))
#endif
static RESIZE_DENSE_LINK void dense_block(
    const ksn_grid_resize_plan *plan, const uint16_t pixel[4][8],
    uint16_t *dest, unsigned x, unsigned y, unsigned lanes, bool use_pie)
{
#if !KSN_GRID_HAS_PIE
    (void)use_pie;
#endif
    int16_t values[4][8] __attribute__((aligned(16)));
    int16_t weights[4][8] __attribute__((aligned(16)));
    int16_t channel[3][8] __attribute__((aligned(16)));
    for (unsigned lane = 0; lane < lanes; ++lane) {
        unsigned xx = x + lane;
        unsigned fx = plan->fx[xx], fy = plan->fy[y];
#ifdef KASANE_PROC_DEVICE_PROBE
        if (ksn_grid_resize_fast_weights) {
            unsigned cross = fx * fy;
            unsigned right = (fx << 7) - cross;
            unsigned lower = (fy << 7) - cross;
            weights[0][lane] = (int16_t)(16384u - right - lower - cross);
            weights[1][lane] = (int16_t)right;
            weights[2][lane] = (int16_t)lower;
            weights[3][lane] = (int16_t)cross;
        } else {
#endif
            weights[0][lane] = (int16_t)((128u - fx) * (128u - fy));
            weights[1][lane] = (int16_t)(fx * (128u - fy));
            weights[2][lane] = (int16_t)((128u - fx) * fy);
            weights[3][lane] = (int16_t)(fx * fy);
#ifdef KASANE_PROC_DEVICE_PROBE
        }
#endif
    }
#if KSN_GRID_HAS_PIE
    bool shared = false;
#ifdef KASANE_PROC_DEVICE_PROBE
    shared = use_pie && ksn_grid_resize_shared_qr;
#endif
    if (shared) pie_weights_load(weights);
#endif
    for (unsigned c = 0; c < 3; ++c) {
        for (unsigned tap = 0; tap < 4; ++tap)
            for (unsigned lane = 0; lane < lanes; ++lane)
                values[tap][lane] = (int16_t)component(pixel[tap][lane], c);
#if KSN_GRID_HAS_PIE
        if (use_pie) {
            if (shared) pie_channel_loaded(values, channel[c]);
            else pie_channel(values, weights, channel[c]);
            continue;
        }
#endif
        for (unsigned lane = 0; lane < lanes; ++lane) {
            unsigned sum = 0;
            for (unsigned tap = 0; tap < 4; ++tap)
                sum += (unsigned)values[tap][lane] *
                       (unsigned)weights[tap][lane];
            channel[c][lane] = (int16_t)(sum >> 14);
        }
    }
    for (unsigned lane = 0; lane < lanes; ++lane)
        dest[lane] = combine(channel, lane);
}

static void block(const ksn_grid_resize_plan *plan,
                  const uint16_t *row0, const uint16_t *row1,
                  uint16_t *dest, unsigned x, unsigned y, unsigned lanes,
                  bool use_pie)
{
    uint16_t pixel[4][8];
    uint16_t difference[8], any_difference = 0;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        unsigned xx = x + lane, sx = plan->x0[xx];
        unsigned sx1 = sx + (sx + 1u < plan->source_width);
        pixel[0][lane] = row0[sx];
        pixel[1][lane] = row0[sx1];
        pixel[2][lane] = row1[sx];
        pixel[3][lane] = row1[sx1];
        difference[lane] = (pixel[0][lane] ^ pixel[1][lane]) |
                           (pixel[0][lane] ^ pixel[2][lane]) |
                           (pixel[0][lane] ^ pixel[3][lane]);
        any_difference |= difference[lane];
    }
    if (!any_difference) {
        RESIZE_ROUTE(flat);
        for (unsigned lane = 0; lane < lanes; ++lane)
            dest[lane] = pixel[0][lane];
        return;
    }
    unsigned changed = 0;
    for (unsigned lane = 0; lane < lanes && changed <= 2; ++lane)
        changed += difference[lane] != 0;
    if (changed <= 2) {
        RESIZE_ROUTE(sparse);
        for (unsigned lane = 0; lane < lanes; ++lane) {
            uint16_t p = pixel[0][lane];
            if (!difference[lane]) { dest[lane] = p; continue; }
            unsigned fx = plan->fx[x + lane], fy = plan->fy[y];
            unsigned weight[4] = {(128u - fx) * (128u - fy),
                                  fx * (128u - fy),
                                  (128u - fx) * fy, fx * fy};
            unsigned channel[3];
            for (unsigned c = 0; c < 3; ++c) {
                unsigned sum = 0;
                for (unsigned tap = 0; tap < 4; ++tap)
                    sum += component(pixel[tap][lane], c) * weight[tap];
                channel[c] = sum >> 14;
            }
            dest[lane] = (uint16_t)((channel[0] << 11) |
                                    (channel[1] << 5) | channel[2]);
        }
        return;
    }
    RESIZE_ROUTE(dense);
#ifdef KASANE_PROC_DEVICE_PROBE
    if (use_pie) RESIZE_ROUTE(dense_pie);
    else RESIZE_ROUTE(dense_scalar);
#endif
    dense_block(plan, pixel, dest, x, y, lanes, use_pie);
}

bool ksn_grid_resize_span(const ksn_grid_resize_plan *plan,
                          unsigned y, unsigned x, unsigned count,
                          const uint16_t *row0, const uint16_t *row1,
                          uint16_t *dest, bool enable_pie, bool *used_pie)
{
    if (!plan || !plan->source_width || !plan->source_height ||
        y >= plan->height || x > plan->width || count > plan->width - x ||
        (count && (!row0 || !row1 || !dest))) return false;
    bool pie = enable_pie && ksn_grid_resize_pie_available() && count >= 8 &&
               (plan->source_width != plan->width ||
                plan->source_height != plan->height);
    if (used_pie) *used_pie = pie;
    for (unsigned offset = 0; offset < count; offset += 8) {
        unsigned lanes = count - offset;
        if (lanes > 8) lanes = 8;
        if (plan->source_width == plan->width &&
            plan->source_height == plan->height) {
            RESIZE_ROUTE(identity);
            memcpy(dest + offset, row0 + x + offset,
                   lanes * sizeof *dest);
        } else
            block(plan, row0, row1, dest + offset, x + offset, y, lanes,
                  pie && lanes == 8);
    }
    return true;
}

bool ksn_grid_resize_run(const ksn_grid_resize_plan *plan,
                         const int16_t *source, size_t source_count,
                         int16_t *dest, size_t dest_count,
                         bool enable_pie, bool *used_pie)
{
    if (!plan || !source || !dest ||
        source_count != (size_t)plan->source_width * plan->source_height ||
        dest_count != (size_t)plan->width * plan->height) return false;
    uintptr_t src_begin = (uintptr_t)source, dst_begin = (uintptr_t)dest;
    size_t src_bytes = source_count * sizeof *source;
    size_t dst_bytes = dest_count * sizeof *dest;
    if (src_begin <= dst_begin ? dst_begin - src_begin < src_bytes :
                                src_begin - dst_begin < dst_bytes) return false;
    if (plan->source_width == plan->width &&
        plan->source_height == plan->height) {
        memcpy(dest, source, dst_bytes);
        if (used_pie) *used_pie = false;
        return true;
    }
    bool pie = enable_pie && ksn_grid_resize_pie_available() &&
               plan->width >= 8;
    if (used_pie) *used_pie = pie;
    for (unsigned y = 0; y < plan->height; ++y) {
        unsigned sy = plan->y0[y];
        unsigned sy1 = sy + (sy + 1u < plan->source_height);
        if (!ksn_grid_resize_span(plan, y, 0, plan->width,
                (const uint16_t *)(source + (size_t)sy * plan->source_width),
                (const uint16_t *)(source + (size_t)sy1 * plan->source_width),
                (uint16_t *)(dest + (size_t)y * plan->width), pie, NULL))
            return false;
    }
    return true;
}
