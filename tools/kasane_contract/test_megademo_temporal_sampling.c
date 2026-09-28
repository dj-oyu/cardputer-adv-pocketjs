/* Actual 48-frame MEGADEMO source through the generic 240x135 -> 112x63
 * sampling rules. Compare spatial and temporal error to a box-area reference.
 * This is host quality evidence, not an LCD capture or device timing. */
#include "ksn_proc_plan.h"
#include "proc_megademo.h"
#include "ui/kasane/ksn_proc_grid_resize.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW 240u
#define SH 135u
#define DW 112u
#define DH 63u
#define PIXELS (DW * DH)

static uint16_t source[SW * SH];
static uint16_t nearest[PIXELS], bilinear[PIXELS];
static uint16_t rows[2][SW];
static float previous_area[PIXELS][3];
static uint16_t previous_nearest[PIXELS], previous_bilinear[PIXELS];
static ksn_proc_plan plans[3][PROC_MEGA_LAYERS];

static void append_point_batch(ksn_proc_frame *output, unsigned phase)
{
    static const int coeff[3][6] = {
        {16384, 0, 0, 16384, 0, 0},
        {16000, 1600, -1000, 16384, -6 * 16384, 6 * 16384},
        {15360, -2304, 1300, 15500, 18 * 16384, -10 * 16384}
    };
    static const uint16_t color[3] = {0x07ff, 0xfde0, 0xf81f};
    int old_x = 0, old_y = 0;
    for (int i = 0; i < 40; ++i) {
        int sx = 18 + i * 5;
        int sy = 92 + ((i * 13) % 23) - 11;
        int x = (int)floor((double)(coeff[phase][0] * sx +
                    coeff[phase][1] * sy + coeff[phase][4]) / 16384);
        int y = (int)floor((double)(coeff[phase][2] * sx +
                    coeff[phase][3] * sy + coeff[phase][5]) / 16384);
        if (x < 0 || x >= (int)SW || y < 0 || y >= (int)SH) abort();
        if (i) {
            if (output->count >= KSN_PROC_SEGMENTS) abort();
            output->segments[output->count++] =
                (ksn_proc_segment){old_x, old_y, x, y, color[phase]};
        }
        old_x = x;
        old_y = y;
    }
}

static unsigned component(uint16_t color, unsigned channel)
{
    return channel == 0 ? (color >> 11) & 31u :
           channel == 1 ? (color >> 5) & 63u : color & 31u;
}

static float scaled(float value, unsigned channel)
{
    return value * (channel == 1 ? 255.0f / 63.0f : 255.0f / 31.0f);
}

static unsigned overlap(unsigned a0, unsigned a1, unsigned b0, unsigned b1)
{
    unsigned lo = a0 > b0 ? a0 : b0;
    unsigned hi = a1 < b1 ? a1 : b1;
    return hi > lo ? hi - lo : 0;
}

static float area(unsigned x, unsigned y, unsigned channel)
{
    unsigned sx0 = x * SW / DW, sx1 = ((x + 1) * SW + DW - 1) / DW;
    unsigned sy0 = y * SH / DH, sy1 = ((y + 1) * SH + DH - 1) / DH;
    uint64_t sum = 0;
    for (unsigned sy = sy0; sy < sy1; ++sy) {
        unsigned wy = overlap(sy * DH, (sy + 1) * DH, y * SH, (y + 1) * SH);
        for (unsigned sx = sx0; sx < sx1; ++sx) {
            unsigned wx = overlap(sx * DW, (sx + 1) * DW,
                                  x * SW, (x + 1) * SW);
            sum += (uint64_t)component(source[sy * SW + sx], channel) * wx * wy;
        }
    }
    return scaled((float)sum / (SW * SH), channel);
}

static unsigned nearest_index(unsigned index, unsigned source_size,
                              unsigned output_size)
{
    unsigned mapped = (index * source_size + source_size / 2u) / output_size;
    return mapped < source_size ? mapped : source_size - 1u;
}

static void render_source(unsigned frame)
{
    unsigned phase = proc_mega_phase(frame);
    for (unsigned i = 0; i < SW * SH; ++i)
        source[i] = proc_mega_backdrop(frame);
    for (unsigned layer = 0; layer < PROC_MEGA_LAYERS; ++layer) {
        ksn_proc_inst code[KSN_PROC_CODE];
        ksn_proc_program program;
        float input[KSN_PROC_INPUTS];
        ksn_proc_vm vm;
        ksn_proc_frame output;
        if (!proc_mega_build(frame, layer, code, &program, input)) abort();
        if (frame % 16u == 0 &&
            !ksn_proc_plan_prepare(&plans[phase][layer], &program)) abort();
        if (ksn_proc_plan_begin(&vm, &plans[phase][layer], input, &output) !=
            KSN_PROC_RUNNING) abort();
        if (ksn_proc_plan_run(&vm, &plans[phase][layer], false) != KSN_PROC_DONE)
            abort();
        if (layer == 3) append_point_batch(&output, phase);
        if (!ksn_proc_render_band(&output, source, 0, SH)) abort();
    }
}

int main(void)
{
    ksn_grid_resize_plan plan;
    if (!ksn_grid_resize_prepare_stream(&plan, SW, SH, DW, DH)) abort();
    double spatial[2] = {0}, temporal[2] = {0};
    unsigned stable_spikes[2] = {0}, comparisons = 0, stable = 0;
    for (unsigned frame = 0; frame < PROC_MEGA_FRAMES; ++frame) {
        render_source(frame);
        unsigned cached_y = SH;
        for (unsigned y = 0; y < DH; ++y) {
            unsigned sy = plan.y0[y];
            if (cached_y != sy) {
                unsigned sy1 = sy + (sy + 1u < SH);
                memcpy(rows[0], source + sy * SW, sizeof rows[0]);
                memcpy(rows[1], source + sy1 * SW, sizeof rows[1]);
                cached_y = sy;
            }
            if (!ksn_grid_resize_span(&plan, y, 0, DW, rows[0], rows[1],
                                      bilinear + y * DW, false, NULL)) abort();
            unsigned ny = nearest_index(y, SH, DH);
            for (unsigned x = 0; x < DW; ++x)
                nearest[y * DW + x] = source[ny * SW + nearest_index(x, SW, DW)];
        }
        for (unsigned y = 0; y < DH; ++y)
            for (unsigned x = 0; x < DW; ++x) {
                unsigned i = y * DW + x;
                float ideal[3], mean_area_motion = 0;
                for (unsigned c = 0; c < 3; ++c) {
                    ideal[c] = area(x, y, c);
                    float outputs[2] = {
                        scaled(component(nearest[i], c), c),
                        scaled(component(bilinear[i], c), c)
                    };
                    for (unsigned mode = 0; mode < 2; ++mode)
                        spatial[mode] += fabsf(outputs[mode] - ideal[c]);
                    if (frame % 16u) {
                        mean_area_motion += fabsf(ideal[c] - previous_area[i][c]) / 3;
                        for (unsigned mode = 0; mode < 2; ++mode) {
                            uint16_t before = mode ? previous_bilinear[i] :
                                                     previous_nearest[i];
                            float delta = outputs[mode] -
                                scaled(component(before, c), c);
                            temporal[mode] += fabsf(delta -
                                (ideal[c] - previous_area[i][c]));
                        }
                    }
                    previous_area[i][c] = ideal[c];
                }
                if (frame % 16u) {
                    ++comparisons;
                    if (mean_area_motion < 5.0f) {
                        ++stable;
                        for (unsigned mode = 0; mode < 2; ++mode) {
                            uint16_t before = mode ? previous_bilinear[i] :
                                                     previous_nearest[i];
                            uint16_t after = mode ? bilinear[i] : nearest[i];
                            float motion = 0;
                            for (unsigned c = 0; c < 3; ++c)
                                motion += fabsf(scaled(component(after,c),c) -
                                                scaled(component(before,c),c)) / 3;
                            if (motion > 30.0f) ++stable_spikes[mode];
                        }
                    }
                }
            }
        memcpy(previous_nearest, nearest, sizeof nearest);
        memcpy(previous_bilinear, bilinear, sizeof bilinear);
    }
    for (unsigned mode = 0; mode < 2; ++mode)
        printf("%s spatial_mae8=%.4f temporal_delta_mae8=%.4f stable_spikes=%u/%u\n",
               mode ? "bilinear" : "nearest",
               spatial[mode] / (PROC_MEGA_FRAMES * PIXELS * 3),
               temporal[mode] / (comparisons * 3),
               stable_spikes[mode], stable);
    return 0;
}
