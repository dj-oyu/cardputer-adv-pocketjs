#include "ksn_proc_grid_pie.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static ksn_grid_index index5(int32_t base, int32_t x, int32_t y,
                             int32_t tx, int32_t ty)
{
    ksn_grid_index index = {0};
    int32_t value[5] = {base, x, y, tx, ty};
    for (unsigned k = 0; k < 5; ++k) {
        index.term[k].constant = value[k];
        index.term[k].param = KSN_GRID_NO_PARAM;
    }
    return index;
}

static ksn_grid_program program(int16_t weight, uint8_t shift)
{
    ksn_grid_program p = {0};
    p.count = 4;
    p.final_shift = shift;
    p.body[0].op = KSN_GRID_LOAD;
    p.body[0].dst = 1;
    p.body[0].index = index5(0, 2, 128, 1, 64);
    p.body[1].op = KSN_GRID_CONST;
    p.body[1].dst = 2;
    p.body[1].immediate = weight;
    p.body[2].op = KSN_GRID_MUL;
    p.body[2].dst = 3;
    p.body[2].a = 1;
    p.body[2].b = 2;
    p.body[3].op = KSN_GRID_ADD;
    p.body[3].dst = 0;
    p.body[3].a = 0;
    p.body[3].b = 3;
    p.output = index5(0, 1, 24, 0, 0);
    return p;
}

/* The device calibration fixture, with the same resolved indexing and buffer
 * sizes. A profile/key mismatch must fail rather than quietly gather. */
static void profile_selection_case(void)
{
#ifdef KSN_GRID_PIE_MODEL
    uint8_t source_storage[32 * 24 * sizeof(int16_t) + 16]
        __attribute__((aligned(16))) = {0};
    int16_t *source = (int16_t *)(void *)(source_storage + 8);
    int16_t dest[16 * 12] __attribute__((aligned(16))) = {0};
    ksn_grid_program p = program(1, 2);
    p.body[0].index = index5(0, 2, 0, 1, 0);
    p.body[0].index.term[2] = (ksn_grid_coeff){0, 2, 0};
    p.body[0].index.term[4] = (ksn_grid_coeff){0, 1, 0};
    p.output = index5(0, 1, 0, 0, 0);
    p.output.term[2] = (ksn_grid_coeff){0, 1, 1};
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    ksn_grid_shape shape = {16, 12, 2, 2};
    ksn_grid_binding binding = {0};
    binding.data[KSN_GRID_SOURCE] = source;
    binding.count[KSN_GRID_SOURCE] = 32 * 24;
    binding.data[KSN_GRID_DEST] = dest;
    binding.count[KSN_GRID_DEST] = 16 * 12;
    binding.param[0] = 32;
    binding.param[1] = 16;
    ksn_grid_execution e;
    assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
    ksn_grid_pie_access_info access;
    assert(ksn_grid_pie_describe_access(&e, &access));
    assert(access.reason == KSN_GRID_SELECTION_PROFILE);
    assert(access.selected == KSN_GRID_PIE_LOAD_FUSED);
    assert(!e.selection_valid); /* Read-only inspection does not mutate bind. */
    assert(ksn_grid_pie_select(&e));
    assert(e.profile_key == UINT64_C(0x0a123287ac2f3390));
    assert(e.selection_reason == KSN_GRID_SELECTION_PROFILE);
    assert(e.selected_strategy == KSN_GRID_PIE_LOAD_FUSED);
    e.requested_strategy = KSN_GRID_PIE_LOAD_AFFINE;
    assert(ksn_grid_pie_select(&e));
    assert(e.selection_reason == KSN_GRID_SELECTION_FORCED);
    assert(e.selected_strategy == KSN_GRID_PIE_LOAD_AFFINE);
    e.requested_strategy = KSN_GRID_PIE_LOAD_AUTO;
    assert(ksn_grid_pie_select(&e));
    assert(e.selection_reason == KSN_GRID_SELECTION_PROFILE);
    shape.width = 15;
    assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
    assert(ksn_grid_pie_select(&e));
    assert(e.selection_reason == KSN_GRID_SELECTION_FALLBACK);
    assert(e.selected_strategy == KSN_GRID_PIE_LOAD_GATHER);
    shape.width = 16;
    assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
    assert(ksn_grid_pie_select(&e));
    assert(e.selection_reason == KSN_GRID_SELECTION_PROFILE);
#endif
}

static void run_case(unsigned width, unsigned height, unsigned tw, unsigned th,
                     int16_t weight, uint8_t shift, unsigned variant)
{
    int16_t source[64 * 8] __attribute__((aligned(16)));
    int16_t weights[64 * 8] __attribute__((aligned(16)));
    int16_t scalar[24 * 3] __attribute__((aligned(16))) = {0};
    int16_t optimized[24 * 3] __attribute__((aligned(16))) = {0};
    for (unsigned i = 0; i < 64u * 8u; ++i) {
        source[i] = (int16_t)((int32_t)((i * 437u + 23u) & 65535u) - 32768);
        weights[i] = (int16_t)((int32_t)((i * 157u + 91u) & 65535u) - 32768);
    }
    source[0] = INT16_MIN;
    source[1] = INT16_MAX;
    ksn_grid_program p = program(weight, shift);
    if (variant == 1) { /* independent definitions and both commutative ops */
        ksn_grid_instruction load = p.body[0], constant = p.body[1];
        constant.dst = 5;
        load.dst = 2;
        p.body[0] = constant;
        p.body[1] = load;
        p.body[2].dst = 1;
        p.body[2].a = 2;
        p.body[2].b = 5;
        p.body[3].a = 1;
        p.body[3].b = 0;
    } else if (variant == 2 || variant == 3 || variant == 12) {
        ksn_grid_instruction weighted = p.body[1];
        weighted.op = KSN_GRID_LOAD;
        weighted.buffer = 2;
        weighted.index = index5(3, variant == 12 ? 0 : 1, 100, 2, 32);
        if (variant == 2 || variant == 12) p.body[1] = weighted;
        else {
            ksn_grid_instruction load = p.body[0];
            p.body[0] = weighted;
            p.body[1] = load;
            p.body[2].a = 2;
            p.body[2].b = 1;
            p.body[3].a = 3;
            p.body[3].b = 0;
        }
    } else if (variant == 4 || variant == 8) {
        p.count = 2;
        if (variant == 8) {
            p.body[0] = p.body[1];
            p.body[0].dst = 1;
        }
        p.body[1].op = KSN_GRID_ADD;
        p.body[1].dst = 0;
        p.body[1].a = 1;
        p.body[1].b = 0;
    } else if (variant == 5) {
        p.initial = INT64_C(1) << 35; /* legal QACC, unsupported PIE bias */
    } else if (variant == 6) {
        p.count = 5;
        p.body[4] = p.body[3];
        p.body[3].op = KSN_GRID_CONST;
        p.body[3].dst = 4;
        p.body[3].immediate = 7;
    } else if (variant == 7) {
        p.body[2].dst = 1; /* overwrite a consumed source register */
        p.body[3].b = 1;
    } else if (variant == 9) {
        p.body[0] = p.body[1];
        p.body[0].dst = 1;
        p.body[1].immediate = -1;
    } else if (variant == 10) {
        p.initial = INT32_MIN;
    } else if (variant == 11) {
        p.initial = INT32_MAX;
    }
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    assert(plan.mac.valid == (variant != 6));
    ksn_grid_shape shape = {(uint16_t)width, (uint16_t)height,
                            (uint16_t)tw, (uint16_t)th};
    ksn_grid_binding binding = {0};
    binding.data[KSN_GRID_SOURCE] = source;
    binding.count[KSN_GRID_SOURCE] = 64u * 8u;
    binding.data[2] = weights;
    binding.count[2] = 64u * 8u;
    binding.data[KSN_GRID_DEST] = scalar;
    binding.count[KSN_GRID_DEST] = 24u * 3u;
    ksn_grid_execution e;
    assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
    assert(e.pie_candidate);
    if (variant == 0 && tw == 2) {
        ksn_grid_pie_access_info access;
        assert(ksn_grid_pie_describe_access(&e, &access));
        assert(access.input == KSN_GRID_ACCESS_GATHER);
        assert(access.pair_candidate && !access.pair_selected &&
               !access.fused_selected);
#ifdef KSN_GRID_PIE_MODEL
        assert((access.candidate_mask & (1u << KSN_GRID_PIE_LOAD_FUSED)) ==
               (width >= 8 ? (1u << KSN_GRID_PIE_LOAD_FUSED) : 0u));
        if (width >= 8) {
        e.requested_strategy = KSN_GRID_PIE_LOAD_AFFINE;
        assert(ksn_grid_pie_select(&e));
        assert(ksn_grid_pie_describe_access(&e, &access));
        assert(access.input == KSN_GRID_ACCESS_INTERLEAVED2);
        assert(access.pair_candidate && access.pair_selected &&
               !access.fused_selected);
        assert(access.tap_issues == 4 && access.tap_stalls == 1);
        assert(access.pair_issues + access.pair_stalls <
               access.split_issues + access.split_stalls);
        }
#endif
    }
    if (variant == 12) {
        ksn_grid_pie_access_info access;
        assert(ksn_grid_pie_describe_access(&e, &access));
        assert(e.access[plan.mac.right.instruction] ==
               KSN_GRID_ACCESS_BROADCAST);
    }
    assert(ksn_grid_run_scalar(&e) == KSN_GRID_OK);
    binding.data[KSN_GRID_DEST] = optimized;
    assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
    bool expect_pie = width >= 8 && variant != 5 && variant != 6;
    assert(ksn_grid_pie_eligible(&e) == expect_pie);
    ksn_grid_pie_policy policy = {true, 8, 8, 0};
    assert(ksn_grid_run_auto(&e, &policy) == KSN_GRID_OK);
#ifdef KSN_GRID_PIE_MODEL
    assert(e.pie_backend_selected == expect_pie);
#else
    assert(!e.pie_backend_selected);
#endif
    assert(memcmp(scalar, optimized, sizeof scalar) == 0);
#ifdef KSN_GRID_PIE_MODEL
    if (expect_pie) {
        int16_t gathered[24 * 3] __attribute__((aligned(16))) = {0};
        binding.data[KSN_GRID_DEST] = gathered;
        assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
        e.requested_strategy = KSN_GRID_PIE_LOAD_GATHER;
        assert(ksn_grid_run_auto(&e, &policy) == KSN_GRID_OK);
        assert(e.pie_backend_selected);
        assert(memcmp(gathered, optimized, sizeof gathered) == 0);
        if (variant == 0 && tw == 2) {
            memset(gathered, 0, sizeof gathered);
            assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
            e.requested_strategy = KSN_GRID_PIE_LOAD_AFFINE;
            assert(ksn_grid_run_auto(&e, &policy) == KSN_GRID_OK);
            assert(memcmp(gathered, optimized, sizeof gathered) == 0);
            memset(gathered, 0, sizeof gathered);
            assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
            e.requested_strategy = KSN_GRID_PIE_LOAD_FUSED;
            assert(ksn_grid_run_auto(&e, &policy) == KSN_GRID_OK);
            assert(memcmp(gathered, optimized, sizeof gathered) == 0);
        }
        binding.data[KSN_GRID_DEST] = optimized;
    }
#endif
    if (variant == 2 || variant == 3 || variant == 12) {
        policy.min_loaded_weight_outputs = 0;
        assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
        assert(ksn_grid_run_auto(&e, &policy) == KSN_GRID_OK);
        assert(!e.pie_backend_selected);
    }
    policy.min_outputs = 0;
    assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
    assert(ksn_grid_run_auto(&e, &policy) == KSN_GRID_OK);
    assert(!e.pie_backend_selected);
}

static void test_alignment(void)
{
    ksn_grid_program p = program(1, 2);
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    int16_t source[64 * 8] __attribute__((aligned(16))) = {0};
    int16_t dest[24 * 3 + 1] __attribute__((aligned(16))) = {0};
    ksn_grid_binding binding = {0};
    binding.data[KSN_GRID_SOURCE] = source;
    binding.count[KSN_GRID_SOURCE] = 64u * 8u;
    binding.data[KSN_GRID_DEST] = dest + 1;
    binding.count[KSN_GRID_DEST] = 24u * 3u;
    ksn_grid_shape shape = {16, 2, 2, 2};
    ksn_grid_execution e;
    assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
    assert(e.pie_candidate && !ksn_grid_pie_eligible(&e));
    ksn_grid_pie_policy policy = {true, 8, 0, 0};
    assert(ksn_grid_run_auto(&e, &policy) == KSN_GRID_OK);
    assert(!e.pie_backend_selected);
}

static void test_registration_normalization(void)
{
    int16_t source[64 * 8] __attribute__((aligned(16)));
    int16_t scalar[24 * 3] __attribute__((aligned(16)));
    int16_t optimized[24 * 3] __attribute__((aligned(16)));
    for (unsigned i = 0; i < 64u * 8u; ++i)
        source[i] = (int16_t)((int)(i % 43u) - 21);
    ksn_grid_shape shape = {16, 2, 2, 2};
    ksn_grid_binding binding = {0};
    binding.data[KSN_GRID_SOURCE] = source;
    binding.count[KSN_GRID_SOURCE] = 64u * 8u;
    binding.count[KSN_GRID_DEST] = 24u * 3u;
    ksn_grid_pie_policy policy = {true, 8, 8, 8};

    for (unsigned variant = 0; variant < 10; ++variant) {
        ksn_grid_program p = program(3, 2);
        if (variant == 0) { /* register reuse, reordered definitions, identities */
            p.count = 8;
            p.body[0].op = KSN_GRID_CONST;
            p.body[0].dst = 4;
            p.body[0].immediate = 0;
            p.body[1].op = KSN_GRID_LOAD;
            p.body[1].dst = 1;
            p.body[1].index = index5(0, 2, 128, 1, 64);
            p.body[2].op = KSN_GRID_ADD;
            p.body[2].dst = 1;
            p.body[2].a = 1;
            p.body[2].b = 4;
            p.body[3].op = KSN_GRID_CONST;
            p.body[3].dst = 5;
            p.body[3].immediate = 1;
            p.body[4].op = KSN_GRID_MUL;
            p.body[4].dst = 1;
            p.body[4].a = 1;
            p.body[4].b = 5;
            p.body[5].op = KSN_GRID_CONST;
            p.body[5].dst = 2;
            p.body[5].immediate = 3;
            p.body[6].op = KSN_GRID_MUL;
            p.body[6].dst = 3;
            p.body[6].a = 2;
            p.body[6].b = 1;
            p.body[7].op = KSN_GRID_ADD;
            p.body[7].dst = 0;
            p.body[7].a = 3;
            p.body[7].b = 0;
        } else if (variant == 1) { /* constant DAG folded to coefficient 3 */
            p.count = 6;
            p.body[1].immediate = 1;
            p.body[2].op = KSN_GRID_CONST;
            p.body[2].dst = 3;
            p.body[2].immediate = 2;
            p.body[3].op = KSN_GRID_ADD;
            p.body[3].dst = 2;
            p.body[3].a = 2;
            p.body[3].b = 3;
            p.body[4].op = KSN_GRID_MUL;
            p.body[4].dst = 4;
            p.body[4].a = 1;
            p.body[4].b = 2;
            p.body[5].op = KSN_GRID_ADD;
            p.body[5].dst = 0;
            p.body[5].a = 0;
            p.body[5].b = 4;
        } else if (variant == 2) { /* one load used as both PIE operands */
            p.count = 3;
            p.body[1].op = KSN_GRID_MUL;
            p.body[1].dst = 2;
            p.body[1].a = p.body[1].b = 1;
            p.body[2].op = KSN_GRID_ADD;
            p.body[2].dst = 0;
            p.body[2].a = 0;
            p.body[2].b = 2;
        } else if (variant == 3) { /* nonzero add cannot become one MAC */
            p.body[2].op = KSN_GRID_ADD;
            p.body[2].a = 1;
            p.body[2].b = 2;
        } else if (variant == 4) { /* dead checked operation cannot be dropped */
            p.count = 5;
            p.body[4] = p.body[3];
            p.body[3].op = KSN_GRID_CONST;
            p.body[3].dst = 4;
            p.body[3].immediate = 7;
        } else if (variant == 5) { /* constant product exceeds int16 but fits one MAC */
            p.body[0].op = KSN_GRID_CONST;
            p.body[0].immediate = INT16_MAX;
            p.body[1].immediate = INT16_MAX;
            p.final_shift = 30;
        } else if (variant == 6) { /* constant MIN folds without changing the tap value */
            p.count = 6;
            p.body[1].immediate = 3;
            p.body[2].op = KSN_GRID_CONST;
            p.body[2].dst = 4;
            p.body[2].immediate = 4;
            p.body[3].op = KSN_GRID_MIN;
            p.body[3].dst = 2;
            p.body[3].a = 2;
            p.body[3].b = 4;
            p.body[4].op = KSN_GRID_MUL;
            p.body[4].dst = 3;
            p.body[4].a = 1;
            p.body[4].b = 2;
            p.body[5].op = KSN_GRID_ADD;
            p.body[5].dst = 0;
            p.body[5].a = 0;
            p.body[5].b = 3;
        } else if (variant == 7 || variant == 8) {
            /* Shared load factors when possible; otherwise uses two MACs. */
            p.count = 7;
            p.body[1].immediate = variant == 7 ? 7 : 30000;
            p.body[3].op = KSN_GRID_CONST;
            p.body[3].dst = 4;
            p.body[3].immediate = variant == 7 ? -4 : 30000;
            p.body[4].op = KSN_GRID_MUL;
            p.body[4].dst = 5;
            p.body[4].a = 1;
            p.body[4].b = 4;
            p.body[5].op = KSN_GRID_ADD;
            p.body[5].dst = 6;
            p.body[5].a = 3;
            p.body[5].b = 5;
            p.body[6].op = KSN_GRID_ADD;
            p.body[6].dst = 0;
            p.body[6].a = 0;
            p.body[6].b = 6;
        } else { /* two different loads use consecutive QACC products */
            p.count = 8;
            p.body[3].op = KSN_GRID_LOAD;
            p.body[3].dst = 4;
            p.body[3].index = index5(15, -1, 128, 1, 64);
            p.body[4].op = KSN_GRID_CONST;
            p.body[4].dst = 5;
            p.body[4].immediate = 2;
            p.body[5].op = KSN_GRID_MUL;
            p.body[5].dst = 6;
            p.body[5].a = 4;
            p.body[5].b = 5;
            p.body[6].op = KSN_GRID_ADD;
            p.body[6].dst = 7;
            p.body[6].a = 3;
            p.body[6].b = 6;
            p.body[7].op = KSN_GRID_ADD;
            p.body[7].dst = 0;
            p.body[7].a = 0;
            p.body[7].b = 7;
        }
        ksn_grid_plan plan;
        assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
        assert(plan.mac.valid == (variant != 4));
        if (variant != 4)
            assert(plan.mac.terms ==
                   ((variant == 3 || variant >= 8) ? 2 : 1));
        memset(scalar, 0, sizeof scalar);
        memset(optimized, 0, sizeof optimized);
        binding.data[KSN_GRID_DEST] = scalar;
        ksn_grid_execution e;
        assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
        if (variant >= 8) {
            ksn_grid_pie_access_info access;
            assert(ksn_grid_pie_describe_access(&e, &access));
#ifdef KSN_GRID_PIE_MODEL
            assert(access.candidate_mask ==
                   (1u << KSN_GRID_PIE_LOAD_GATHER));
            assert(access.selected == KSN_GRID_PIE_LOAD_GATHER);
            assert(access.reason == KSN_GRID_SELECTION_FALLBACK);
#else
            assert(access.candidate_mask == 0);
#endif
        }
        assert(ksn_grid_run_scalar(&e) == KSN_GRID_OK);
        binding.data[KSN_GRID_DEST] = optimized;
        assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
        assert(ksn_grid_run_auto(&e, &policy) == KSN_GRID_OK);
#ifdef KSN_GRID_PIE_MODEL
        assert(e.pie_backend_selected == (variant != 4));
#else
        assert(!e.pie_backend_selected);
#endif
        assert(memcmp(scalar, optimized, sizeof scalar) == 0);
    }
}

#ifdef KSN_GRID_PIE_MODEL
static void test_affine_load_bounds(void)
{
    ksn_grid_program p = program(1, 0);
    p.body[0].index = index5(0, 1, 0, 0, 0);
    p.output = index5(0, 1, 0, 0, 0);
    ksn_grid_plan plan;
    int16_t source[16] __attribute__((aligned(16)));
    int16_t scalar[8] __attribute__((aligned(16)));
    int16_t optimized[8] __attribute__((aligned(16)));
    for (unsigned i = 0; i < 16; ++i) source[i] = (int16_t)(i * 193 - 1200);
    ksn_grid_binding binding = {0};
    binding.data[KSN_GRID_SOURCE] = source;
    binding.data[KSN_GRID_DEST] = scalar;
    binding.count[KSN_GRID_DEST] = 8;
    ksn_grid_shape shape = {8, 1, 1, 1};
    const struct { int32_t base, stride; unsigned count; } cases[] = {
        {0, 0, 1}, {0, 1, 8}, {0, 2, 15}, {0, 2, 16}, {14, -2, 15}
    };
    for (unsigned test = 0; test < sizeof cases / sizeof cases[0]; ++test) {
            int32_t stride = cases[test].stride;
            unsigned count = cases[test].count;
            p.body[0].index.term[0].constant = cases[test].base;
            p.body[0].index.term[1].constant = stride;
            assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
            binding.count[KSN_GRID_SOURCE] = count;
            ksn_grid_execution e;
            assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
            assert(ksn_grid_run_scalar(&e) == KSN_GRID_OK);
            binding.data[KSN_GRID_DEST] = optimized;
            memset(optimized, 0, sizeof optimized);
            assert(ksn_grid_begin(&plan, &shape, &binding, &e) == KSN_GRID_OK);
            e.requested_strategy = KSN_GRID_PIE_LOAD_AFFINE;
#ifdef KSN_GRID_PIE_MODEL
            uint32_t direct = ksn_grid_pie_direct_taps;
            uint32_t stride2 = ksn_grid_pie_stride2_taps;
            uint32_t gather = ksn_grid_pie_gather_taps;
            uint32_t broadcast = ksn_grid_pie_broadcast_taps;
#endif
            assert(ksn_grid_run_pie(&e) == KSN_GRID_OK);
            assert(memcmp(scalar, optimized, sizeof scalar) == 0);
#ifdef KSN_GRID_PIE_MODEL
            if (stride == 0) assert(ksn_grid_pie_broadcast_taps == broadcast + 1);
            else if (stride == 1) assert(ksn_grid_pie_direct_taps == direct + 1);
            else if (count == 15) assert(ksn_grid_pie_gather_taps == gather + 1);
            else assert(ksn_grid_pie_stride2_taps == stride2 + 1);
#endif
            binding.data[KSN_GRID_DEST] = scalar;
        }
}
#endif

static void test_dependent_rows(void)
{
    enum { WIDTH = 16, HEIGHT = 17, PITCH = WIDTH + 1 };
    ksn_grid_program p = {0};
    p.count = 4;
    p.body[0].op = KSN_GRID_LOAD;
    p.body[0].dst = 1;
    p.body[0].buffer = KSN_GRID_DEST;
    p.body[0].index = index5(0, 1, PITCH, 0, 0);
    p.body[1].op = KSN_GRID_LOAD;
    p.body[1].dst = 2;
    p.body[1].buffer = KSN_GRID_SOURCE;
    p.body[1].index = index5(0, 1, WIDTH, 0, 0);
    p.body[2].op = KSN_GRID_ADD;
    p.body[2].dst = 3;
    p.body[2].a = 1;
    p.body[2].b = 2;
    p.body[3].op = KSN_GRID_ADD;
    p.body[3].dst = 0;
    p.body[3].a = 0;
    p.body[3].b = 3;
    p.output = index5(1, 1, PITCH, 0, 0);
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    int16_t source[WIDTH * HEIGHT] __attribute__((aligned(16)));
    int16_t scalar[PITCH * HEIGHT] __attribute__((aligned(16))) = {0};
    int16_t optimized[PITCH * HEIGHT] __attribute__((aligned(16))) = {0};
    for (unsigned y = 0; y < HEIGHT; ++y) {
        scalar[y * PITCH] = optimized[y * PITCH] = y & 1 ? INT16_MIN : INT16_MAX;
        for (unsigned x = 0; x < WIDTH; ++x)
            source[y * WIDTH + x] = (x + y) & 1 ? -30000 : 30000;
    }
    ksn_grid_binding bind = {0};
    bind.data[KSN_GRID_SOURCE] = source;
    bind.count[KSN_GRID_SOURCE] = WIDTH * HEIGHT;
    bind.data[KSN_GRID_DEST] = scalar;
    bind.count[KSN_GRID_DEST] = PITCH * HEIGHT;
    ksn_grid_shape shape = {WIDTH, HEIGHT, 1, 1};
    ksn_grid_execution e;
    assert(ksn_grid_begin(&plan, &shape, &bind, &e) == KSN_GRID_OK);
    assert(e.scan_rows_candidate && !e.independent && !e.pie_candidate);
    assert(ksn_grid_run_scalar(&e) == KSN_GRID_OK);
    bind.data[KSN_GRID_DEST] = optimized;
    assert(ksn_grid_begin(&plan, &shape, &bind, &e) == KSN_GRID_OK);
    assert(ksn_grid_scan_pie_eligible(&e));
    ksn_grid_pie_policy policy = {true, 8, 8, 8};
    assert(ksn_grid_run_auto(&e, &policy) == KSN_GRID_OK);
#ifdef KSN_GRID_PIE_MODEL
    assert(e.pie_backend_selected);
#else
    assert(!e.pie_backend_selected);
#endif
    assert(memcmp(scalar, optimized, sizeof scalar) == 0);
    policy.min_scan_rows = 0;
    assert(ksn_grid_begin(&plan, &shape, &bind, &e) == KSN_GRID_OK);
    assert(ksn_grid_run_auto(&e, &policy) == KSN_GRID_OK);
    assert(!e.pie_backend_selected);
    shape.height = 7;
    assert(ksn_grid_begin(&plan, &shape, &bind, &e) == KSN_GRID_OK);
    assert(e.scan_rows_candidate && !ksn_grid_scan_pie_eligible(&e));
    shape.height = HEIGHT;
    p.body[0].index = p.output; /* reads this output, not its predecessor */
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    assert(ksn_grid_begin(&plan, &shape, &bind, &e) == KSN_GRID_OK);
    assert(!e.scan_rows_candidate);
    p.body[0].index = index5(0, 1, PITCH, 0, 0);
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    bind.data[KSN_GRID_SOURCE] = optimized; /* hidden alias via another ID */
    bind.count[KSN_GRID_SOURCE] = PITCH * HEIGHT;
    assert(ksn_grid_begin(&plan, &shape, &bind, &e) == KSN_GRID_OK);
    assert(!e.scan_rows_candidate);

    p = (ksn_grid_program){0};
    p.count = 6;
    p.final_shift = 1;
    p.body[0].op = KSN_GRID_LOAD;
    p.body[0].dst = 1;
    p.body[0].buffer = KSN_GRID_DEST;
    p.body[0].index = index5(0, 1, PITCH, 0, 0);
    p.body[1].op = KSN_GRID_CONST;
    p.body[1].dst = 2;
    p.body[1].immediate = 2;
    p.body[2].op = KSN_GRID_MUL;
    p.body[2].dst = 3;
    p.body[2].a = 1;
    p.body[2].b = 2;
    p.body[3].op = KSN_GRID_LOAD;
    p.body[3].dst = 4;
    p.body[3].buffer = KSN_GRID_SOURCE;
    p.body[3].index = index5(0, 1, WIDTH, 0, 0);
    p.body[4].op = KSN_GRID_ADD;
    p.body[4].dst = 5;
    p.body[4].a = 3;
    p.body[4].b = 4;
    p.body[5].op = KSN_GRID_ADD;
    p.body[5].dst = 0;
    p.body[5].a = 0;
    p.body[5].b = 5;
    p.output = index5(1, 1, PITCH, 0, 0);
    assert(ksn_grid_prepare(&p, &plan) == KSN_GRID_OK);
    memset(scalar, 0, sizeof scalar);
    memset(optimized, 0, sizeof optimized);
    for (unsigned row = 0; row < HEIGHT; ++row)
        scalar[row * PITCH] = optimized[row * PITCH] =
            row & 1 ? INT16_MIN : INT16_MAX;
    bind.data[KSN_GRID_SOURCE] = source;
    bind.count[KSN_GRID_SOURCE] = WIDTH * HEIGHT;
    bind.data[KSN_GRID_DEST] = scalar;
    assert(ksn_grid_begin(&plan, &shape, &bind, &e) == KSN_GRID_OK);
    assert(e.scan_rows_candidate && e.scan_prev_coefficient == 2);
    assert(ksn_grid_run_scalar(&e) == KSN_GRID_OK);
    bind.data[KSN_GRID_DEST] = optimized;
    assert(ksn_grid_begin(&plan, &shape, &bind, &e) == KSN_GRID_OK);
    policy.min_scan_rows = 8;
    assert(ksn_grid_run_auto(&e, &policy) == KSN_GRID_OK);
#ifdef KSN_GRID_PIE_MODEL
    assert(e.pie_backend_selected);
#else
    assert(!e.pie_backend_selected);
#endif
    assert(memcmp(scalar, optimized, sizeof scalar) == 0);
}

int main(void)
{
    profile_selection_case();
    test_registration_normalization();
    static const unsigned width[] = {1, 7, 8, 9, 15, 16, 17};
    static const int16_t weight[] = {1, -1, 16384, INT16_MIN};
    static const uint8_t shift[] = {0, 2, 14, 30};
    unsigned cases = 0;
    for (unsigned w = 0; w < sizeof width / sizeof width[0]; ++w)
        for (unsigned taps = 1; taps <= 4; ++taps)
            for (unsigned c = 0; c < sizeof weight / sizeof weight[0]; ++c) {
                run_case(width[w], 2, taps, 2, weight[c], shift[c], 0);
                ++cases;
            }
    for (unsigned variant = 1; variant <= 12; ++variant)
        for (unsigned w = 2; w < 7; w += 2)
            for (unsigned taps = 1; taps <= 4; taps += 3)
                for (unsigned c = 0; c < 4; ++c) {
                    run_case(width[w], 2, taps, 2, weight[c], shift[c], variant);
                    ++cases;
                }
    test_alignment();
#ifdef KSN_GRID_PIE_MODEL
    test_affine_load_bounds();
#endif
    test_dependent_rows();
#ifdef KSN_GRID_PIE_MODEL
    assert(ksn_grid_pie_direct_taps > 0);
    assert(ksn_grid_pie_stride2_taps > 0);
    assert(ksn_grid_pie_unzip_pairs > 0);
    assert(ksn_grid_pie_fused_pairs > 0);
    assert(ksn_grid_pie_broadcast_taps > 0);
    assert(ksn_grid_pie_gather_taps > 0);
    printf("grid loads: direct=%u stride2=%u paired=%u fused=%u broadcast=%u gather=%u\n",
           ksn_grid_pie_direct_taps, ksn_grid_pie_stride2_taps,
           ksn_grid_pie_unzip_pairs, ksn_grid_pie_fused_pairs,
           ksn_grid_pie_broadcast_taps,
           ksn_grid_pie_gather_taps);
#endif
    printf("proc-grid-pie: %u scalar/dispatch cases passed backend=%u\n",
           cases, (unsigned)ksn_grid_pie_backend_available());
    return 0;
}
