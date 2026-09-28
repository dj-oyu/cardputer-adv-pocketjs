#include "ui/kasane/ksn_proc_grid_resize.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SW 240u
#define SH 135u
#define DW 112u
#define DH 63u

static uint16_t source[SW * SH];
static uint16_t bilinear[DW * DH], nearest[DW * DH];
static uint16_t rows[2][SW];

static unsigned component(uint16_t color, unsigned c)
{
    return c == 0 ? (color >> 11) & 31u :
           c == 1 ? (color >> 5) & 63u : color & 31u;
}

static uint16_t pack(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void fixture(unsigned kind)
{
    for (unsigned y = 0; y < SH; ++y)
        for (unsigned x = 0; x < SW; ++x) {
            uint16_t color;
            if (kind == 0) color = pack(12, 30, 21); /* flat UI fill */
            else if (kind == 1) { /* sparse one-pixel strokes */
                unsigned on = (x == 37 || x == 99 || y == 24 || y == 84);
                color = on ? pack(31, 63, 31) : 0;
            } else if (kind == 2) { /* smooth image-like ramp */
                color = pack(x * 31 / (SW - 1), y * 63 / (SH - 1),
                             (x + y) * 31 / (SW + SH - 2));
            } else { /* high-frequency alternating pixels */
                color = (x + y) & 1u ? pack(31, 63, 31) : 0;
            }
            source[y * SW + x] = color;
        }
}

static unsigned overlap(unsigned a0, unsigned a1,
                        unsigned b0, unsigned b1)
{
    unsigned lo = a0 > b0 ? a0 : b0;
    unsigned hi = a1 < b1 ? a1 : b1;
    return hi > lo ? hi - lo : 0;
}

/* Exact box-area reference in integer overlap coordinates. Each source pixel
 * contributes its RGB565 components in proportion to covered area. */
static double area_channel(unsigned x, unsigned y, unsigned c)
{
    unsigned sx0 = x * SW / DW, sx1 = ((x + 1) * SW + DW - 1) / DW;
    unsigned sy0 = y * SH / DH, sy1 = ((y + 1) * SH + DH - 1) / DH;
    uint64_t sum = 0;
    for (unsigned sy = sy0; sy < sy1; ++sy) {
        unsigned wy = overlap(sy * DH, (sy + 1) * DH, y * SH, (y + 1) * SH);
        for (unsigned sx = sx0; sx < sx1; ++sx) {
            unsigned wx = overlap(sx * DW, (sx + 1) * DW,
                                  x * SW, (x + 1) * SW);
            sum += (uint64_t)component(source[sy * SW + sx], c) * wx * wy;
        }
    }
    return (double)sum / ((double)SW * SH);
}

static unsigned nearest_index(unsigned i, unsigned source_size,
                              unsigned output_size)
{
    unsigned index = (i * source_size + source_size / 2u) / output_size;
    return index < source_size ? index : source_size - 1u;
}

static void run_case(unsigned kind, const char *name)
{
    ksn_grid_resize_plan plan;
    if (!ksn_grid_resize_prepare_stream(&plan, SW, SH, DW, DH)) abort();
    fixture(kind);
    ksn_grid_resize_routes discard, routes;
    ksn_grid_resize_routes_read(&discard);
    unsigned bilinear_reads = 0, nearest_rows = 0, area_reads = 0;
    unsigned stretch_calls = 0, stretch_pixels = 0;
    unsigned cached_y = SH, previous_nearest = SH;
    for (unsigned y = 0; y < DH; ++y) {
        unsigned sy = plan.y0[y];
        unsigned sy1 = sy + (sy + 1u < SH);
        if (cached_y != sy) {
            for (unsigned x = 0; x < SW; ++x) rows[0][x] = source[sy * SW + x];
            ++bilinear_reads;
            if (sy1 != sy) {
                for (unsigned x = 0; x < SW; ++x)
                    rows[1][x] = source[sy1 * SW + x];
                ++bilinear_reads;
            } else {
                for (unsigned x = 0; x < SW; ++x) rows[1][x] = rows[0][x];
            }
            cached_y = sy;
        }
        for (unsigned x = 0; x < DW; x += 16) {
            unsigned count = DW - x < 16 ? DW - x : 16;
            if (!ksn_grid_resize_span(&plan, y, x, count, rows[0], rows[1],
                    &bilinear[y * DW + x], true, NULL)) abort();
        }
        unsigned ny = nearest_index(y, SH, DH);
        if (ny != previous_nearest) { ++nearest_rows; previous_nearest = ny; }
        for (unsigned x = 0; x < DW; ++x)
            nearest[y * DW + x] = source[ny * SW + nearest_index(x, SW, DW)];
        for (unsigned x = 0; x < DW;) {
            unsigned count = DW - x < 16 ? DW - x : 16;
            unsigned start = nearest_index(x, SW, DW);
            unsigned last = nearest_index(x + count - 1u, SW, DW);
            while (count > 1 && last - start >= 32)
                last = nearest_index(x + --count - 1u, SW, DW);
            ++stretch_calls;
            stretch_pixels += last - start + 1u;
            x += count;
        }
        /* A one-row cache still needs every source row intersecting this box.
         * The current bilinear stream instead stores a fixed two-row pair. */
        area_reads += ((y + 1u) * SH + DH - 1u) / DH - y * SH / DH;
    }
    ksn_grid_resize_routes_read(&routes);
    double bilinear_error = 0, nearest_error = 0;
    unsigned worse_bilinear = 0, worse_nearest = 0;
    for (unsigned y = 0; y < DH; ++y)
        for (unsigned x = 0; x < DW; ++x) {
            double be = 0, ne = 0;
            for (unsigned c = 0; c < 3; ++c) {
                double reference = area_channel(x, y, c);
                double scale = c == 1 ? 255.0 / 63.0 : 255.0 / 31.0;
                be += fabs(component(bilinear[y * DW + x], c) - reference) * scale;
                ne += fabs(component(nearest[y * DW + x], c) - reference) * scale;
            }
            bilinear_error += be / 3.0;
            nearest_error += ne / 3.0;
            if (be > ne + 0.001) ++worse_bilinear;
            if (ne > be + 0.001) ++worse_nearest;
        }
    double n = (double)DW * DH;
    printf("%-8s rows nearest=%u bilinear=%u area=%u | provider nearest=%u calls/%u pixels bilinear=%u calls/%u pixels | MAE8 nearest=%.3f bilinear=%.3f | wins nearest=%u bilinear=%u | blocks copy=%u sparse=%u dense_pie_eligible=%u dense_scalar=%u\n",
           name, nearest_rows, bilinear_reads, area_reads,
           stretch_calls, stretch_pixels, bilinear_reads, bilinear_reads * SW,
           nearest_error / n, bilinear_error / n,
           worse_bilinear, worse_nearest,
           routes.flat, routes.sparse, routes.dense_pie, routes.dense_scalar);
    if (routes.flat + routes.sparse + routes.dense != DW * DH / 8u ||
        routes.dense != routes.dense_pie + routes.dense_scalar ||
        bilinear_reads > 2 * DH || nearest_rows > DH ||
        (kind == 0 && (bilinear_error != 0 || nearest_error != 0))) abort();
}

int main(void)
{
    run_case(0, "flat");
    run_case(1, "strokes");
    run_case(2, "gradient");
    run_case(3, "checker");
    return 0;
}
