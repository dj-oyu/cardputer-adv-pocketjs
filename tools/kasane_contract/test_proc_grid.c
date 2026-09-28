#include "ksn_proc_grid.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static ksn_grid_index affine(int32_t base, int32_t x, int32_t y,
                             int32_t tx, int32_t ty)
{
    ksn_grid_index index = {0};
    const int32_t value[5] = {base, x, y, tx, ty};
    for (unsigned k = 0; k < 5; ++k) {
        index.term[k].constant = value[k];
        index.term[k].param = KSN_GRID_NO_PARAM;
    }
    return index;
}

static ksn_grid_program sum_program(void)
{
    ksn_grid_program p = {0};
    p.count = 4;
    p.result_reg = 0;
    p.body[0].op = KSN_GRID_LOAD;
    p.body[0].dst = 1;
    p.body[0].index = affine(0, 1, 0, 1, 0);
    p.body[1].op = KSN_GRID_CONST;
    p.body[1].dst = 2;
    p.body[1].immediate = 1;
    p.body[2].op = KSN_GRID_MUL;
    p.body[2].dst = 3;
    p.body[2].a = 1;
    p.body[2].b = 2;
    p.body[3].op = KSN_GRID_ADD;
    p.body[3].dst = 0;
    p.body[3].a = 0;
    p.body[3].b = 3;
    p.output = affine(0, 1, 0, 0, 0);
    return p;
}

static void compare_lanes(const ksn_grid_plan *plan,
                          ksn_grid_shape shape, ksn_grid_binding binding,
                          size_t output_count)
{
    int16_t expected[128] = {0}, actual[128] = {0};
    assert(output_count <= 128);
    binding.data[KSN_GRID_DEST] = expected;
    binding.count[KSN_GRID_DEST] = output_count;
    ksn_grid_execution scalar;
    assert(ksn_grid_begin(plan, &shape, &binding, &scalar) == KSN_GRID_OK);
    assert(scalar.safe && scalar.independent && scalar.reduction_shape);
    assert(scalar.pie_candidate && !scalar.pie_backend_selected);
    assert(ksn_grid_run_scalar(&scalar) == KSN_GRID_OK);
    binding.data[KSN_GRID_DEST] = actual;
    ksn_grid_execution lanes;
    assert(ksn_grid_begin(plan, &shape, &binding, &lanes) == KSN_GRID_OK);
    assert(ksn_grid_run_lanes_model(&lanes) == KSN_GRID_OK);
    assert(memcmp(expected, actual, output_count * sizeof(int16_t)) == 0);
}

static void test_downsample_and_bind(void)
{
    ksn_grid_program p = sum_program();
    /* Two runtime pitches are frozen at begin, not at registration. */
    p.body[0].index.term[2].constant = 0;
    p.body[0].index.term[2].scale = 2;
    p.body[0].index.term[2].param = 0;
    p.body[0].index.term[4].constant = 0;
    p.body[0].index.term[4].scale = 1;
    p.body[0].index.term[4].param = 0;
    p.body[0].index.term[1].constant = 2;
    p.output.term[2].constant = 0;
    p.output.term[2].scale = 1;
    p.output.term[2].param = 1;
    p.final_shift = 2;
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    memset(&p, 0, sizeof(p)); /* the prepared plan owns its instructions */
    int16_t source[12 * 8];
    for (unsigned i = 0; i < 96; ++i) source[i] = (int16_t)(i - 48);
    ksn_grid_binding bind = {0};
    bind.data[KSN_GRID_SOURCE] = source;
    bind.count[KSN_GRID_SOURCE] = 96;
    bind.param[0] = 12;
    bind.param[1] = 5;
    ksn_grid_shape shape = {5, 4, 2, 2};
    compare_lanes(&plan, shape, bind, 20);
    int16_t output[20] = {0};
    bind.data[KSN_GRID_DEST] = output;
    bind.count[KSN_GRID_DEST] = 20;
    ksn_grid_execution exec;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    assert(ksn_grid_run_scalar(&exec) == KSN_GRID_OK);
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 5; ++x) {
            int64_t total = 0;
            for (unsigned ty = 0; ty < 2; ++ty)
                for (unsigned tx = 0; tx < 2; ++tx)
                    total += source[(2*y+ty)*12 + 2*x+tx];
            int64_t expected = total / 4;
            if (total < 0 && total % 4) --expected;
            assert(output[y*5+x] == expected);
        }
    uint32_t work = exec.validation_work;
    assert(work <= 64);
    shape.width = 1;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    assert(exec.validation_work == work); /* no per-pixel proof */
    bind.param[0] = 2; /* inner source rows no longer fit */
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    bind.param[0] = -1;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_BAD_INDEX);
}

static void test_matrix_vector_and_tail(void)
{
    ksn_grid_program p = sum_program();
    p.count = 4;
    p.body[0].index = affine(0, 4, 0, 1, 0); /* matrix row */
    p.body[1].op = KSN_GRID_LOAD;
    p.body[1].dst = 2;
    p.body[1].buffer = 2;
    p.body[1].index = affine(0, 0, 0, 1, 0); /* separate vector buffer */
    p.final_shift = 0;
    int16_t src[48] = {0};
    for (unsigned row = 0; row < 12; ++row)
        for (unsigned col = 0; col < 4; ++col)
            src[row*4+col] = (int16_t)(row - col);
    int16_t vector[4] = {1, 2, -3, 4};
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    ksn_grid_binding bind = {0};
    bind.data[KSN_GRID_SOURCE] = src;
    bind.count[KSN_GRID_SOURCE] = 48;
    bind.data[2] = vector;
    bind.count[2] = 4;
    for (unsigned width = 1; width <= 9; ++width) {
        ksn_grid_shape shape = {(uint16_t)width, 1, 4, 1};
        compare_lanes(&plan, shape, bind, width);
    }
    int16_t output[9] = {0};
    bind.data[KSN_GRID_DEST] = output;
    bind.count[KSN_GRID_DEST] = 9;
    ksn_grid_shape shape = {9, 1, 4, 1};
    ksn_grid_execution exec;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    assert(ksn_grid_run_scalar(&exec) == KSN_GRID_OK);
    for (unsigned row = 0; row < 9; ++row) {
        int32_t expected = 0;
        for (unsigned col = 0; col < 4; ++col)
            expected += src[row*4+col] * vector[col];
        assert(output[row] == expected);
    }
}

static void test_negative_stride(void)
{
    ksn_grid_program p = sum_program();
    p.body[0].index = affine(8, -1, 0, 0, 0);
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    int16_t src[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    int16_t dst[9] = {0};
    ksn_grid_binding bind = {0};
    bind.data[KSN_GRID_SOURCE] = src;
    bind.count[KSN_GRID_SOURCE] = 9;
    bind.data[KSN_GRID_DEST] = dst;
    bind.count[KSN_GRID_DEST] = 9;
    ksn_grid_shape shape = {9, 1, 1, 1};
    ksn_grid_execution exec;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    assert(exec.pie_candidate);
    assert(ksn_grid_run_lanes_model(&exec) == KSN_GRID_OK);
    for (unsigned i = 0; i < 9; ++i) assert(dst[i] == 8 - (int)i);
    p.body[0].index.term[0].constant = 7;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_BAD_INDEX);
}

static void test_dependency_and_collision(void)
{
    ksn_grid_program p = sum_program();
    p.count = 3;
    p.body[0].buffer = KSN_GRID_DEST;
    p.body[0].index = affine(0, 1, 0, 0, 0);
    p.body[1].op = KSN_GRID_LOAD;
    p.body[1].dst = 2;
    p.body[1].index = affine(0, 1, 0, 0, 0);
    p.body[2].op = KSN_GRID_ADD;
    p.body[2].dst = 0;
    p.body[2].a = 1;
    p.body[2].b = 2;
    p.output = affine(1, 1, 0, 0, 0);
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    int16_t src[4] = {1, 2, 3, 4}, dst[5] = {10};
    ksn_grid_binding bind = {0};
    bind.data[KSN_GRID_SOURCE] = src;
    bind.count[KSN_GRID_SOURCE] = 4;
    bind.data[KSN_GRID_DEST] = dst;
    bind.count[KSN_GRID_DEST] = 5;
    ksn_grid_shape shape = {4, 1, 1, 1};
    ksn_grid_execution exec;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    assert(exec.safe && !exec.independent && !exec.pie_candidate);
    assert(ksn_grid_run_lanes_model(&exec) == KSN_GRID_BAD_IR);
    assert(ksn_grid_run_scalar(&exec) == KSN_GRID_OK);
    assert(dst[1] == 11 && dst[2] == 13 && dst[3] == 16 && dst[4] == 20);

    p = sum_program();
    p.output = affine(0, 0, 0, 0, 0);
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    bind.data[KSN_GRID_DEST] = dst;
    shape.tap_width = 1;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    assert(exec.safe && !exec.independent);
    assert(ksn_grid_run_scalar(&exec) == KSN_GRID_OK);
    assert(dst[0] == 4);

    p.output = affine(0, 1, 0, 0, 0);
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    bind.data[KSN_GRID_SOURCE] = dst;
    bind.count[KSN_GRID_SOURCE] = 5;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    assert(exec.safe && !exec.independent); /* alias despite distinct IDs */
}

static void test_rejections_and_limits(void)
{
    ksn_grid_program p = sum_program();
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    int16_t src[256] = {0}, dst[16] = {0};
    ksn_grid_binding bind = {0};
    bind.data[KSN_GRID_SOURCE] = src;
    bind.count[KSN_GRID_SOURCE] = 256;
    bind.data[KSN_GRID_DEST] = dst;
    bind.count[KSN_GRID_DEST] = 16;
    ksn_grid_shape shape = {9, 1, 1, 1};
    ksn_grid_execution exec;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    bind.count[KSN_GRID_SOURCE] = 8;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_BAD_INDEX);
    bind.count[KSN_GRID_SOURCE] = 256;
    shape.tap_width = 257;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_BAD_SHAPE);
    shape.tap_width = 1;
    shape.width = 0;
    bind.data[KSN_GRID_SOURCE] = NULL;
    bind.data[KSN_GRID_DEST] = NULL;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    assert(ksn_grid_run_scalar(&exec) == KSN_GRID_OK);
    p.count = KSN_GRID_CODE + 1;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_BAD_IR);
    p = sum_program();
    p.body[2].a = 7;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_BAD_IR);
    p = sum_program();
    p.output.term[3].constant = 1;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    shape = (ksn_grid_shape){1, 1, 1, 1};
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_BAD_INDEX);

    p = sum_program();
    p.count = 6;
    p.body[1].immediate = INT16_MAX;
    p.body[3].op = KSN_GRID_ADD;
    p.body[3].dst = 4;
    p.body[3].a = 3;
    p.body[3].b = 3;
    p.body[4] = p.body[3];
    p.body[4].dst = 5;
    p.body[4].a = 4;
    p.body[4].b = 4;
    p.body[5].op = KSN_GRID_ADD;
    p.body[5].dst = 0;
    p.body[5].a = 0;
    p.body[5].b = 5;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    bind.data[KSN_GRID_SOURCE] = src;
    bind.data[KSN_GRID_DEST] = dst;
    shape = (ksn_grid_shape){1, 1, 256, 1};
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    assert(exec.safe && exec.independent && exec.reduction_shape);
    assert(!exec.qacc_legal);
    assert(!exec.pie_candidate); /* exceeds signed 40-bit accumulator */
    for (unsigned i = 0; i < 256; ++i) src[i] = INT16_MAX;
    assert(ksn_grid_run_scalar(&exec) == KSN_GRID_OK);
    assert(dst[0] == INT16_MAX); /* int64 scalar remains defined */
}

static void test_procedural_without_source(void)
{
    ksn_grid_program p = {0};
    p.count = 2;
    p.body[0].op = KSN_GRID_CONST;
    p.body[0].dst = 1;
    p.body[0].immediate = 7;
    p.body[1].op = KSN_GRID_ADD;
    p.body[1].dst = 0;
    p.body[1].a = 0;
    p.body[1].b = 1;
    p.initial = 3;
    p.output = affine(0, 1, 3, 0, 0);
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    int16_t dst[6] = {0};
    ksn_grid_binding bind = {0};
    bind.data[KSN_GRID_DEST] = dst;
    bind.count[KSN_GRID_DEST] = 6;
    ksn_grid_shape shape = {3, 2, 4, 1};
    ksn_grid_execution exec;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    assert(exec.pie_candidate);
    assert(ksn_grid_run_lanes_model(&exec) == KSN_GRID_OK);
    for (unsigned i = 0; i < 6; ++i) assert(dst[i] == 31);
}

static void test_sequential_fold(void)
{
    ksn_grid_program p = {0};
    p.count = 4;
    p.body[0].op = KSN_GRID_CONST;
    p.body[0].dst = 1;
    p.body[0].immediate = 2;
    p.body[1].op = KSN_GRID_MUL;
    p.body[1].dst = 0;
    p.body[1].a = 0;
    p.body[1].b = 1;
    p.body[2].op = KSN_GRID_LOAD;
    p.body[2].dst = 2;
    p.body[2].index = affine(0, 0, 0, 1, 0);
    p.body[3].op = KSN_GRID_ADD;
    p.body[3].dst = 0;
    p.body[3].a = 0;
    p.body[3].b = 2;
    p.output = affine(0, 1, 0, 0, 0);
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    int16_t src[3] = {1, 2, 3}, dst[1] = {0};
    ksn_grid_binding bind = {0};
    bind.data[KSN_GRID_SOURCE] = src;
    bind.count[KSN_GRID_SOURCE] = 3;
    bind.data[KSN_GRID_DEST] = dst;
    bind.count[KSN_GRID_DEST] = 1;
    ksn_grid_shape shape = {1, 1, 3, 1};
    ksn_grid_execution exec;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    assert(exec.safe && exec.independent && !exec.reduction_shape);
    assert(!exec.pie_candidate);
    assert(ksn_grid_run_scalar(&exec) == KSN_GRID_OK);
    assert(dst[0] == 11);
}

static void test_versioned_overflow_bound(void)
{
    ksn_grid_program p = {0};
    p.count = 5;
    p.body[0].op = KSN_GRID_LOAD;
    p.body[0].dst = 1;
    p.body[0].index = affine(0, 0, 0, 0, 0);
    for (unsigned i = 1; i < 4; ++i) {
        p.body[i].op = KSN_GRID_MUL;
        p.body[i].dst = 1;
        p.body[i].a = p.body[i].b = 1;
    }
    p.body[4].op = KSN_GRID_ADD;
    p.body[4].dst = 0;
    p.body[4].a = 0;
    p.body[4].b = 1;
    p.output = affine(0, 1, 0, 0, 0);
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    assert(plan.analysis.value[1].a == 0 &&
           plan.analysis.value[2].a == 1 &&
           plan.analysis.value[3].a == 2);
    assert(plan.analysis.value[2].range_proven &&
           !plan.analysis.value[3].range_proven);
    int16_t src[1] = {INT16_MAX}, dst[1] = {0};
    ksn_grid_binding bind = {0};
    bind.data[KSN_GRID_SOURCE] = src;
    bind.count[KSN_GRID_SOURCE] = 1;
    bind.data[KSN_GRID_DEST] = dst;
    bind.count[KSN_GRID_DEST] = 1;
    ksn_grid_shape shape = {1, 1, 1, 1};
    ksn_grid_execution exec;
    assert(ksn_grid_begin(&plan, &shape, &bind, &exec) == KSN_GRID_OK);
    assert(!exec.reduction_shape && !exec.pie_candidate);
    assert(ksn_grid_run_scalar(&exec) == KSN_GRID_ARITH_OVERFLOW);
}

int main(void)
{
    test_downsample_and_bind();
    test_matrix_vector_and_tail();
    test_negative_stride();
    test_dependency_and_collision();
    test_rejections_and_limits();
    test_procedural_without_source();
    test_sequential_fold();
    test_versioned_overflow_bound();
    printf("proc-grid: host scalar/lane model and legality cases passed; "
           "plan=%zu execution=%zu bytes\n",
           sizeof(ksn_grid_plan), sizeof(ksn_grid_execution));
    return 0;
}
