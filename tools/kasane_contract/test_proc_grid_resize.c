#include "ui/kasane/ksn_proc_grid_resize.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "resize test %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

static unsigned channel(uint16_t color, unsigned component)
{
    return component == 0 ? (color >> 11) & 31u :
           component == 1 ? (color >> 5) & 63u : color & 31u;
}

static void check_case(unsigned sw, unsigned sh, unsigned dw, unsigned dh)
{
    ksn_grid_resize_plan plan;
    CHECK(ksn_grid_resize_prepare(&plan, sw, sh, dw, dh));
    size_t sc = (size_t)sw * sh, dc = (size_t)dw * dh;
    int16_t *source = malloc(sc * sizeof *source);
    int16_t *scalar = malloc(dc * sizeof *scalar);
    int16_t *pie = malloc(dc * sizeof *pie);
    CHECK(source && scalar && pie);
    for (unsigned y = 0; y < sh; ++y)
        for (unsigned x = 0; x < sw; ++x) {
            unsigned r = (x * 7u + y * 11u) & 31u;
            unsigned g = (x * 9u + y * 13u) & 63u;
            unsigned b = (x * 17u + y * 3u) & 31u;
            source[y * sw + x] = (int16_t)((r << 11) | (g << 5) | b);
        }
    bool used = true;
    CHECK(ksn_grid_resize_run(&plan, source, sc, scalar, dc, false, &used));
    CHECK(!used);
    CHECK(ksn_grid_resize_run(&plan, source, sc, pie, dc, true, &used));
    CHECK(used == (dw >= 8 && ksn_grid_resize_pie_available() &&
                   (sw != dw || sh != dh)));
    for (unsigned y = 0; y < dh; ++y)
        for (unsigned x = 0; x < dw; ++x) {
            size_t index = (size_t)y * dw + x;
            CHECK(scalar[index] == pie[index]);
            if (sw == dw && sh == dh) CHECK(scalar[index] == source[index]);
            double px = ((double)x + 0.5) * sw / dw - 0.5;
            double py = ((double)y + 0.5) * sh / dh - 0.5;
            if (px < 0) px = 0;
            if (py < 0) py = 0;
            if (px > sw - 1) px = sw - 1;
            if (py > sh - 1) py = sh - 1;
            unsigned x0 = (unsigned)px, y0 = (unsigned)py;
            unsigned x1 = x0 + (x0 + 1 < sw), y1 = y0 + (y0 + 1 < sh);
            double fx = px - x0, fy = py - y0;
            uint16_t colors[4] = {
                (uint16_t)source[y0 * sw + x0],
                (uint16_t)source[y0 * sw + x1],
                (uint16_t)source[y1 * sw + x0],
                (uint16_t)source[y1 * sw + x1]
            };
            double weights[4] = {
                (1 - fx) * (1 - fy), fx * (1 - fy),
                (1 - fx) * fy, fx * fy
            };
            for (unsigned c = 0; c < 3; ++c) {
                double expected = 0;
                for (unsigned t = 0; t < 4; ++t)
                    expected += channel(colors[t], c) * weights[t];
                double actual = channel((uint16_t)scalar[index], c);
                if (fabs(actual - expected) > 2.01)
                    fprintf(stderr, "%ux%u -> %ux%u at %u,%u c%u actual %.1f expected %.3f\n",
                            sw, sh, dw, dh, x, y, c, actual, expected);
                CHECK(fabs(actual - expected) <= 2.01);
            }
        }
    CHECK(!ksn_grid_resize_run(&plan, source, sc - 1, scalar, dc, true, NULL));
    CHECK(!ksn_grid_resize_run(&plan, source, sc, source, dc, true, NULL));
    free(source); free(scalar); free(pie);
}

static void check_sparse(void)
{
    ksn_grid_resize_plan plan;
    CHECK(ksn_grid_resize_prepare_stream(&plan, 16, 2, 8, 1));
    uint16_t source[32], actual[8];
    for (unsigned i = 0; i < 32; ++i) source[i] = 0x1234;
    source[5] = source[16 + 5] = 0xf800;
    source[6] = source[16 + 6] = 0xf800;
    CHECK(ksn_grid_resize_span(&plan, 0, 0, 8, source, source + 16,
                               actual, true, NULL));
    for (unsigned x = 0; x < 8; ++x) {
        unsigned sx = plan.x0[x], sx1 = sx + (sx + 1 < 16);
        unsigned fx = plan.fx[x], fy = plan.fy[0];
        unsigned weights[4] = {(128 - fx) * (128 - fy), fx * (128 - fy),
                               (128 - fx) * fy, fx * fy};
        uint16_t taps[4] = {source[sx], source[sx1],
                            source[16 + sx], source[16 + sx1]};
        unsigned channels[3];
        for (unsigned c = 0; c < 3; ++c) {
            unsigned sum = 0;
            for (unsigned tap = 0; tap < 4; ++tap)
                sum += channel(taps[tap], c) * weights[tap];
            channels[c] = sum >> 14;
        }
        CHECK(actual[x] == (uint16_t)((channels[0] << 11) |
                                       (channels[1] << 5) | channels[2]));
    }
}

int main(void)
{
    check_sparse();
    check_case(90, 45, 42, 21);  /* 15:7 on both axes */
    check_case(90, 45, 53, 31);  /* unrelated horizontal/vertical ratios */
    check_case(90, 45, 90, 45);  /* exact identity */
    check_case(60, 30, 28, 14);  /* GRID LAB 15:7 */
    check_case(60, 30, 37, 23);  /* GRID LAB asymmetric ratio */
    check_case(60, 30, 30, 15);  /* GRID LAB 2:1 */
    check_case(1, 1, 17, 13);
    check_case(11, 7, 3, 2);
    check_case(8, 8, 16, 16);
    ksn_grid_resize_plan plan;
    CHECK(!ksn_grid_resize_prepare(&plan, 240, 135, 112, 63));
    CHECK(!ksn_grid_resize_prepare(&plan, 0, 8, 8, 8));
    puts("grid arbitrary-ratio RGB565 scalar/PIE model passed");
    return 0;
}
