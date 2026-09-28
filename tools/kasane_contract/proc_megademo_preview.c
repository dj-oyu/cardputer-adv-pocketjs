#include "proc_megademo.h"
#include "ksn_proc_plan.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define PHASE_FRAMES 16u
#define PHASE_COUNT (PROC_MEGA_FRAMES / PHASE_FRAMES)

static uint16_t pixels[KSN_PROC_W * KSN_PROC_H];
static ksn_proc_frame frame_buffer;
static ksn_proc_plan plans[PHASE_COUNT][PROC_MEGA_LAYERS];

static int write_ppm(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    if (fprintf(file, "P6\n%d %d\n255\n", KSN_PROC_W, KSN_PROC_H) < 0) {
        fclose(file);
        return 0;
    }
    for (unsigned i = 0; i < KSN_PROC_W * KSN_PROC_H; ++i) {
        uint16_t c = pixels[i];
        unsigned char rgb[3] = {
            (unsigned char)((((c >> 11) & 31u) * 255u + 15u) / 31u),
            (unsigned char)((((c >> 5) & 63u) * 255u + 31u) / 63u),
            (unsigned char)(((c & 31u) * 255u + 15u) / 31u)
        };
        if (fwrite(rgb, 1, sizeof rgb, file) != sizeof rgb) {
            fclose(file);
            return 0;
        }
    }
    return fclose(file) == 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTPUT_DIRECTORY\n", argv[0]);
        return 2;
    }
    for (unsigned phase = 0; phase < PHASE_COUNT; ++phase) {
        for (unsigned layer = 0; layer < PROC_MEGA_LAYERS; ++layer) {
            ksn_proc_inst code[KSN_PROC_CODE];
            ksn_proc_program program;
            float input[KSN_PROC_INPUTS];
            if (!proc_mega_build(phase * PHASE_FRAMES, layer, code, &program, input) ||
                !ksn_proc_plan_prepare(&plans[phase][layer], &program)) {
                fprintf(stderr, "phase %u layer %u: registration failed\n", phase, layer);
                return 1;
            }
        }
    }
    for (unsigned tick = 0; tick < PROC_MEGA_FRAMES; ++tick) {
        uint16_t backdrop = proc_mega_backdrop(tick);
        for (unsigned i = 0; i < KSN_PROC_W * KSN_PROC_H; ++i)
            pixels[i] = backdrop;
        unsigned segments = 0, raster_steps = 0, vm_steps = 0;
        for (unsigned layer = 0; layer < PROC_MEGA_LAYERS; ++layer) {
            float input[KSN_PROC_INPUTS];
            ksn_proc_vm vm;
            const unsigned phase = proc_mega_phase(tick);
            const ksn_proc_plan *plan = &plans[phase][layer];
            if (!proc_mega_inputs(tick, layer, input) ||
                ksn_proc_plan_begin(&vm, plan, input, &frame_buffer) != KSN_PROC_RUNNING ||
                ksn_proc_plan_run(&vm, plan, false) != KSN_PROC_DONE ||
                !ksn_proc_render_band(&frame_buffer, pixels, 0, KSN_PROC_H)) {
                fprintf(stderr, "frame %u layer %u: input, run, or render failed\n", tick, layer);
                return 1;
            }
            segments += frame_buffer.count;
            raster_steps += frame_buffer.raster_steps;
            vm_steps += vm.steps;
        }
        char path[1024];
        int n = snprintf(path, sizeof path, "%s/frame_%03u.ppm", argv[1], tick);
        if (n < 0 || (size_t)n >= sizeof path || !write_ppm(path)) {
            fprintf(stderr, "cannot write frame %u\n", tick);
            return 1;
        }
        printf("frame=%u phase=%u segments=%u raster=%u vm_steps=%u\n",
               tick, proc_mega_phase(tick), segments, raster_steps, vm_steps);
    }
    return 0;
}
