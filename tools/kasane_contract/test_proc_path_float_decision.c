#include "ksn_proc_plan.h"
#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <string.h>

#define I(op,dst,a,b,value,color) {op,dst,a,b,value,color}

static ksn_proc_vm reference_vm, planned_vm;
static ksn_proc_frame reference_frame, planned_frame;
static uint16_t first_pixels[KSN_PROC_W * KSN_PROC_H];
static uint16_t second_pixels[KSN_PROC_W * KSN_PROC_H];

static void same_result(ksn_proc_status expected)
{
    assert(reference_vm.status == expected && planned_vm.status == expected);
    assert(reference_vm.pc == planned_vm.pc);
    assert(reference_vm.last_pc == planned_vm.last_pc);
    assert(reference_vm.steps == planned_vm.steps);
    assert(!memcmp(reference_vm.reg, planned_vm.reg, sizeof reference_vm.reg));
    assert(reference_vm.pen_valid == planned_vm.pen_valid);
    assert(reference_vm.pen_x == planned_vm.pen_x);
    assert(reference_vm.pen_y == planned_vm.pen_y);
    assert(reference_frame.ready == planned_frame.ready);
    assert(reference_frame.count == planned_frame.count);
    assert(reference_frame.raster_steps == planned_frame.raster_steps);
    assert(!memcmp(reference_frame.segments, planned_frame.segments,
                   reference_frame.count * sizeof reference_frame.segments[0]));
}

static void run_both(const ksn_proc_program *program, const ksn_proc_plan *plan,
                     const float input[KSN_PROC_INPUTS],
                     const ksn_proc_state *state, ksn_proc_status expected)
{
    assert(ksn_proc_begin_state(&reference_vm, program, input, state,
                                &reference_frame) == KSN_PROC_RUNNING);
    assert(ksn_proc_plan_begin_state(&planned_vm, plan, input, state,
                                     &planned_frame) == KSN_PROC_RUNNING);
    assert(ksn_proc_run(&reference_vm) == expected);
    assert(ksn_proc_plan_run(&planned_vm, plan, false) == expected);
    same_result(expected);
}

static void ordered_dynamic_path(void)
{
    const ksn_proc_inst code[] = {
        I(KSN_PROC_CUBIC,0,16,0,0,0x07e0),
        I(KSN_PROC_SET,0,0,0,80,0), I(KSN_PROC_SET,1,0,0,15,0),
        I(KSN_PROC_INPUT,2,0,0,0,0), I(KSN_PROC_INPUT,3,1,0,0,0),
        I(KSN_PROC_SET,4,0,0,125,0), I(KSN_PROC_SET,5,0,0,55,0),
        I(KSN_PROC_SET,6,0,0,150,0), I(KSN_PROC_SET,7,0,0,15,0),
        I(KSN_PROC_CUBIC,0,16,0,0,0xf800),
        I(KSN_PROC_SET,0,0,0,170,0), I(KSN_PROC_SET,1,0,0,30,0),
        I(KSN_PROC_LINE,0,0,1,0,0x001f)
    };
    const ksn_proc_program program = {code, sizeof code / sizeof code[0]};
    ksn_proc_plan plan;
    ksn_proc_analysis analysis;
    ksn_proc_state state;
    ksn_proc_state_reset(&state);
    const float first[8] = {10,15,25,55,55,55,80,15};
    memcpy(state.reg, first, sizeof first);
    assert(ksn_proc_analyze(&program, &analysis));
    assert(ksn_proc_plan_prepare(&plan, &program));
    assert(plan.fused_count == 0);
    assert(analysis.inst[0].reads == 0xff &&
           analysis.inst[0].effects == (KSN_PA_EFFECT_PEN | KSN_PA_EFFECT_DRAW));
    assert(analysis.inst[9].reads == 0xff &&
           analysis.inst[9].failure & KSN_PA_FAIL_RASTER);
    assert(analysis.inst[12].effects & KSN_PA_EFFECT_DRAW);
    assert(!analysis.inst[0].relocatable && !analysis.inst[9].relocatable);

    const float input_a[KSN_PROC_INPUTS] = {95,75,0,0};
    run_both(&program, &plan, input_a, &state, KSN_PROC_DONE);
    assert(reference_frame.count == 33 && reference_vm.steps == program.count);
    assert(reference_frame.segments[0].x0 == 10 &&
           reference_frame.segments[15].x1 == 80 &&
           reference_frame.segments[16].x0 == 80 &&
           reference_frame.segments[31].x1 == 150 &&
           reference_frame.segments[32].x0 == 150 &&
           reference_frame.segments[32].x1 == 170);
    assert(reference_frame.segments[15].color == 0x07e0 &&
           reference_frame.segments[16].color == 0xf800 &&
           reference_frame.segments[32].color == 0x001f);
    assert(ksn_proc_render_band(&reference_frame, first_pixels, 0, KSN_PROC_H));
    ksn_proc_segment first_half[16];
    memcpy(first_half, reference_frame.segments, sizeof first_half);
    const ksn_proc_segment control_a = reference_frame.segments[23];

    const float input_b[KSN_PROC_INPUTS] = {110,35,0,0};
    run_both(&program, &plan, input_b, &state, KSN_PROC_DONE);
    assert(!memcmp(first_half, reference_frame.segments, sizeof first_half));
    assert(memcmp(&control_a, &reference_frame.segments[23], sizeof control_a));
    assert(ksn_proc_render_band(&reference_frame, second_pixels, 0, KSN_PROC_H));
    assert(memcmp(first_pixels, second_pixels, sizeof first_pixels));
    memset(first_pixels, 0, sizeof first_pixels);
    for(int bottom = KSN_PROC_H; bottom > 0; ) {
        int rows = bottom < 8 ? bottom : 8;
        bottom -= rows;
        assert(ksn_proc_render_band(&reference_frame,
               first_pixels + bottom * KSN_PROC_W, bottom, rows));
    }
    assert(!memcmp(first_pixels, second_pixels, sizeof first_pixels));

    const float invalid[KSN_PROC_INPUTS] = {721,35,0,0};
    run_both(&program, &plan, invalid, &state, KSN_PROC_INVALID);
    assert(!reference_frame.ready && reference_frame.count == 16 &&
           reference_vm.last_pc == 9);
    puts("D3 path: ordered two-cubic composition, parameter update, band replay and failure match scalar VM");
}

static void float_boundary_is_not_q14_points(void)
{
    const ksn_proc_inst code[] = {
        I(KSN_PROC_INPUT,0,0,0,0,0),
        I(KSN_PROC_INPUT,1,1,0,0,0),
        I(KSN_PROC_ADD,0,0,1,0,0),
        I(KSN_PROC_SET,2,0,0,20,0),
        I(KSN_PROC_PLOT,0,0,2,0,0xffff)
    };
    const ksn_proc_program program = {code, sizeof code / sizeof code[0]};
    ksn_proc_plan plan;
    ksn_proc_analysis analysis;
    assert(ksn_proc_analyze(&program, &analysis));
    assert(ksn_proc_plan_prepare(&plan, &program));
    assert(!plan.points_registered);
    assert(analysis.inst[2].failure & KSN_PA_FAIL_FINITE);
    assert(analysis.inst[4].failure & KSN_PA_FAIL_COORD);
    const float input[KSN_PROC_INPUTS] = {0.4999f,0.0002f,0,0};
    run_both(&program, &plan, input, NULL, KSN_PROC_DONE);
    assert(reference_frame.count == 1 && reference_frame.segments[0].x0 == 1);

    /* The separately typed Q14 point batch floors the same half-pixel
     * translation. Implicitly treating float draw IR as that batch changes
     * the pixel. This is a semantic rejection, not a PIE speed benchmark. */
    int16_t sx = 0, sy = 20, dx = -1, dy = -1;
    const KsnProcAffineQ14 coeff = {16384,0,0,16384,8192,0};
    ksn_proc_points_affine_scalar((KsnProcPointDst){&dx,&dy},
                                  (KsnProcPointSrc){&sx,&sy}, 1, &coeff);
    assert(dx == 0 && dy == 20 && dx != reference_frame.segments[0].x0);
    puts("D3a float: scalar plan matches float VM; implicit Q14 point lowering changes half-pixel rounding");
}

int main(void)
{
    ordered_dynamic_path();
    float_boundary_is_not_q14_points();
    return 0;
}
