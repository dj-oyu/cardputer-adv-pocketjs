#include "ksn_proc_grid_pie.h"

#include <limits.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "esp_memory_utils.h"
#include "esp_idf_version.h"
#include "esp_rom_sys.h"
#endif

typedef struct {
    uint64_t key;
    ksn_grid_pie_load_strategy strategy;
} ksn_grid_profile_entry;
#include "ksn_proc_grid_profile.h"

_Static_assert(KSN_GRID_PROFILE_BACKEND_REV == KSN_GRID_PIE_BACKEND_REV,
               "regenerate grid profile after backend changes");
#ifdef KSN_GRID_PIE_MODEL
uint32_t ksn_grid_pie_direct_taps, ksn_grid_pie_stride2_taps;
uint32_t ksn_grid_pie_unzip_pairs;
uint32_t ksn_grid_pie_fused_pairs;
uint32_t ksn_grid_pie_gather_taps;
uint32_t ksn_grid_pie_broadcast_taps;
ksn_grid_trace_fn g_ksn_grid_pie_trace;
#define LOAD_COUNT(name) (++ksn_grid_pie_##name)
#define TRACE(kind, a, b, arg) do { if (g_ksn_grid_pie_trace) \
    g_ksn_grid_pie_trace((kind), (a), (b), (arg)); } while (0)
#else
#define LOAD_COUNT(name) ((void)0)
#define TRACE(kind, a, b, arg) ((void)0)
#endif

bool ksn_grid_pie_backend_available(void)
{
#if KSN_GRID_HAS_PIE || defined(KSN_GRID_PIE_MODEL)
    return true;
#else
    return false;
#endif
}

bool ksn_grid_pie_eligible(const ksn_grid_execution *e)
{
    if (!e || !e->safe || !e->pie_candidate || !e->plan ||
        !e->plan->mac.valid || e->shape.width < 8 || !e->shape.height)
        return false;
    const ksn_grid_program *p = &e->plan->program;
    if (p->initial < INT32_MIN || p->initial > INT32_MAX ||
        e->output[1] != 1 ||
        (e->output[0] & 7) || (e->output[2] & 7) ||
        ((uintptr_t)e->binding.data[KSN_GRID_DEST] & 15)) return false;
    return true;
}

bool ksn_grid_scan_pie_eligible(const ksn_grid_execution *e)
{
    return e && e->safe && e->scan_rows_candidate && e->shape.height >= 8;
}

#if KSN_GRID_HAS_PIE
static inline void pie_start(const int16_t *coefficient)
{
    __asm__ volatile(
        "ee.vld.128.ip q1, %[coefficient], 0\n"
        "ee.zero.qacc\n"
        : : [coefficient] "a"(coefficient) : "memory");
}

static inline void pie_zero(void)
{
    __asm__ volatile("ee.zero.qacc\n" : : : "memory");
}

static inline void pie_reload_coefficient(const int16_t *coefficient)
{
    __asm__ volatile("ee.vld.128.ip q1, %[coefficient], 0\n"
                     : : [coefficient] "a"(coefficient) : "memory");
}

static inline void pie_tap(const int16_t *lanes)
{
    __asm__ volatile(
        "ee.vld.128.ip q0, %[lanes], 0\n"
        "ee.vmulas.s16.qacc q0, q1\n"
        : : [lanes] "a"(lanes) : "memory");
}

/* Accumulate two gathered taps in order. The fused instruction consumes the
 * old q0 before replacing it with the next tap. Both vectors must stay live
 * until the assembly finishes. */
static inline void pie_tap_fused_pair(const int16_t *first,
                                      const int16_t *next)
{
    __asm__ volatile(
        "ee.vld.128.ip q0, %[first], 0\n"
        "ee.vmulas.s16.qacc.ld.ip q0, %[next], 0, q0, q1\n"
        "ee.vmulas.s16.qacc q0, q1\n"
        : : [first] "a"(first), [next] "a"(next) : "memory");
}

/* Two adjacent taps from sixteen contiguous source cells: q2 receives even
 * cells and q3 odd cells. Accumulate in the original tx=0,tx=1 order. */
static inline void pie_tap_unzip_pair(const int16_t *source)
{
    __asm__ volatile(
        "ee.vld.128.ip q2, %[source], 16\n"
        "ee.vld.128.ip q3, %[source], 0\n"
        "ee.vunzip.16 q2, q3\n"
        "ee.vmulas.s16.qacc q2, q1\n"
        "ee.vmulas.s16.qacc q3, q1\n"
        : [source] "+&a"(source) : : "memory");
}

static inline void pie_tap_stride2(const int16_t *source)
{
    __asm__ volatile(
        "ee.vld.128.ip q2, %[source], 16\n"
        "ee.vld.128.ip q3, %[source], 0\n"
        "ee.vunzip.16 q2, q3\n"
        "ee.vmulas.s16.qacc q2, q1\n"
        : [source] "+&a"(source) : : "memory");
}

static inline void pie_tap_dynamic(const int16_t *lanes,
                                   const int16_t *coefficient)
{
    __asm__ volatile(
        "ee.vld.128.ip q0, %[lanes], 0\n"
        "ee.vld.128.ip q1, %[coefficient], 0\n"
        "ee.vmulas.s16.qacc q0, q1\n"
        : : [lanes] "a"(lanes), [coefficient] "a"(coefficient) : "memory");
}

static inline void pie_tap_broadcast_input(const int16_t *value)
{
    __asm__ volatile(
        "ee.vldbc.16 q0, %[value]\n"
        "ee.vmulas.s16.qacc q0, q1\n"
        : : [value] "a"(value) : "memory");
}

static inline void pie_tap_broadcast_coefficient(const int16_t *lanes,
                                                 const int16_t *coefficient)
{
    __asm__ volatile(
        "ee.vld.128.ip q0, %[lanes], 0\n"
        "ee.vldbc.16 q1, %[coefficient]\n"
        "ee.vmulas.s16.qacc q0, q1\n"
        : : [lanes] "a"(lanes), [coefficient] "a"(coefficient) : "memory");
}

static inline void pie_finish(int16_t *dest, int shift)
{
    __asm__ volatile(
        "ee.srcmb.s16.qacc q2, %[shift], 0\n"
        "ee.vst.128.ip q2, %[dest], 0\n"
        : : [shift] "a"(shift), [dest] "a"(dest) : "memory");
}

/* The scalar store saturates after every column. VADDS has exactly that
 * signed-int16 behavior, so there is no QACC state to carry between columns. */
static inline void pie_scan_step(const int16_t *previous,
                                 const int16_t *samples, int16_t *result)
{
    __asm__ volatile(
        "ee.vld.128.ip q0, %[previous], 0\n"
        "ee.vld.128.ip q1, %[samples], 0\n"
        "ee.vadds.s16 q2, q0, q1\n"
        "ee.vst.128.ip q2, %[result], 0\n"
        : : [previous] "a"(previous), [samples] "a"(samples),
            [result] "a"(result) : "memory");
}

static inline void pie_scan_affine_step(const int16_t *previous,
                                        const int16_t *samples,
                                        const int16_t *coefficient,
                                        const int16_t *ones,
                                        int shift, int16_t *result)
{
    pie_start(coefficient);
    pie_tap(previous);
    pie_reload_coefficient(ones);
    pie_tap(samples);
    pie_finish(result, shift);
}
#endif

static int16_t extract(int64_t value, unsigned shift)
{
    int64_t divisor = INT64_C(1) << shift;
    int64_t q = value / divisor;
    if (value < 0 && value % divisor) --q;
    if (q > INT16_MAX) q = INT16_MAX;
    if (q < INT16_MIN) q = INT16_MIN;
    return (int16_t)q;
}

#if KSN_GRID_HAS_PIE
static void split_bias(int32_t bias, int16_t out[4])
{
    int64_t q = -(int64_t)(bias / INT32_C(32768));
    out[0] = (int16_t)(bias % INT32_C(32768));
    for (unsigned j = 1; j < 4; ++j) {
        int64_t part = q;
        if (part > INT16_MAX) part = INT16_MAX;
        if (part < INT16_MIN) part = INT16_MIN;
        out[j] = (int16_t)part;
        q -= part;
    }
}

/* Same exact signed-int32 decomposition as the existing Q14 point kernel.
 * q0/q1 are overwritten, so a fixed coefficient is reloaded afterwards. */
static void pie_add_bias(int32_t bias)
{
    int16_t parts[4];
    int16_t values[8] __attribute__((aligned(16)));
    int16_t coefficient[8] __attribute__((aligned(16)));
    split_bias(bias, parts);
    for (unsigned term = 0; term < 4; ++term) {
        for (unsigned lane = 0; lane < 8; ++lane) {
            values[lane] = parts[term];
            coefficient[lane] = term == 0 ? 1 : INT16_MIN;
        }
        pie_tap_dynamic(values, coefficient);
    }
}
#endif

static size_t input_address(const int64_t index[5], unsigned x, unsigned y,
                            unsigned tx, unsigned ty)
{
    return (size_t)(index[0] + index[1] * x + index[2] * y +
                    index[3] * tx + index[4] * ty);
}

static bool aligned_load(const int16_t *base, size_t count, int64_t at,
                         size_t cells)
{
    return at >= 0 && (uint64_t)at <= count && cells <= count - (size_t)at &&
           (((uintptr_t)(base + (size_t)at) & 15u) == 0u);
}

typedef ksn_grid_access_kind load_mode;
#define LOAD_GATHER KSN_GRID_ACCESS_GATHER
#define LOAD_CONTIGUOUS KSN_GRID_ACCESS_CONTIGUOUS
#define LOAD_STRIDE2 KSN_GRID_ACCESS_INTERLEAVED2
#define LOAD_BROADCAST KSN_GRID_ACCESS_BROADCAST
#define LOAD_REVERSE KSN_GRID_ACCESS_REVERSE

static uint64_t hash_word(uint64_t hash, uint64_t value)
{
    for (unsigned byte = 0; byte < 8; ++byte) {
        hash ^= (uint8_t)(value >> (byte * 8u));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool profile_environment_matches(void)
{
#ifdef ESP_PLATFORM
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
    return false; /* Fixed-frequency calibration cannot cover DFS. */
#else
    return KSN_GRID_PROFILE_OPT_SIZE == 1u &&
#ifdef CONFIG_COMPILER_OPTIMIZATION_SIZE
           CONFIG_COMPILER_OPTIMIZATION_SIZE == 1 &&
#else
           false &&
#endif
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ == KSN_GRID_PROFILE_CPU_MHZ &&
           ESP_IDF_VERSION_MAJOR == KSN_GRID_PROFILE_IDF_MAJOR &&
           ESP_IDF_VERSION_MINOR == KSN_GRID_PROFILE_IDF_MINOR &&
           ESP_IDF_VERSION_PATCH == KSN_GRID_PROFILE_IDF_PATCH &&
           esp_rom_get_cpu_ticks_per_us() == KSN_GRID_PROFILE_CPU_MHZ;
#endif
#else
    return true; /* Host PIE model checks functional profile dispatch. */
#endif
}

static unsigned memory_class(const void *pointer)
{
#ifdef ESP_PLATFORM
    return esp_ptr_internal(pointer) ? 1u :
           esp_ptr_external_ram(pointer) ? 2u : 0u;
#else
    (void)pointer;
    return 1u; /* host PIE model uses the internal-memory profile class */
#endif
}

/* Resolved bind facts, not raw pointers: a profile may rank legal candidates
 * only when its measured shape, access pattern and memory placement match. */
uint64_t ksn_grid_pie_profile_key(const ksn_grid_execution *e)
{
    if (!e || !e->safe || !e->plan || !e->plan->mac.valid) return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
#define HASH(value) (hash = hash_word(hash, (uint64_t)(value)))
    HASH(KSN_GRID_PIE_BACKEND_REV);
    HASH(e->shape.width); HASH(e->shape.height);
    HASH(e->shape.tap_width); HASH(e->shape.tap_height);
    HASH(e->plan->program.initial != 0);
    HASH(e->plan->program.final_shift);
    for (unsigned k = 0; k < 3; ++k) HASH(e->output[k]);
    const ksn_grid_mac_operand *operand[2] = {
        &e->plan->mac.left, &e->plan->mac.right
    };
    for (unsigned side = 0; side < 2; ++side) {
        const ksn_grid_mac_operand *op = operand[side];
        HASH(op->is_load);
        if (!op->is_load) continue;
        unsigned slot = op->instruction;
        unsigned buffer = e->plan->program.body[slot].buffer;
        for (unsigned k = 0; k < 5; ++k) HASH(e->index[slot][k]);
        HASH(e->binding.count[buffer]);
        HASH((uintptr_t)e->binding.data[buffer] & 15u);
        HASH(memory_class(e->binding.data[buffer]));
    }
    if (e->plan->mac.terms == 2) {
        HASH(2);
        const ksn_grid_mac_operand *extra[2] = {
            &e->plan->mac.extra_left, &e->plan->mac.extra_right
        };
        for (unsigned side = 0; side < 2; ++side) {
            const ksn_grid_mac_operand *op = extra[side];
            HASH(op->is_load);
            if (!op->is_load) continue;
            unsigned slot = op->instruction;
            unsigned buffer = e->plan->program.body[slot].buffer;
            for (unsigned k = 0; k < 5; ++k) HASH(e->index[slot][k]);
            HASH(e->binding.count[buffer]);
            HASH((uintptr_t)e->binding.data[buffer] & 15u);
            HASH(memory_class(e->binding.data[buffer]));
        }
    }
    HASH(e->binding.count[KSN_GRID_DEST]);
    HASH((uintptr_t)e->binding.data[KSN_GRID_DEST] & 15u);
    HASH(memory_class(e->binding.data[KSN_GRID_DEST]));
#undef HASH
    return hash ? hash : 1u;
}

uint8_t ksn_grid_pie_candidates(const ksn_grid_execution *e)
{
    if (!ksn_grid_pie_backend_available() || !ksn_grid_pie_eligible(e))
        return 0;
    if (e->plan->mac.terms == 2)
        return (1u << KSN_GRID_PIE_LOAD_GATHER) |
               (1u << KSN_GRID_PIE_LOAD_AFFINE);
    uint8_t mask = (1u << KSN_GRID_PIE_LOAD_GATHER) |
                   (1u << KSN_GRID_PIE_LOAD_AFFINE);
    ksn_grid_mac mac = e->plan->mac;
    if (!mac.left.is_load && mac.right.is_load) {
        ksn_grid_mac_operand swap = mac.left;
        mac.left = mac.right;
        mac.right = swap;
    }
    if (mac.left.is_load && !mac.right.is_load &&
        e->access[mac.left.instruction] == LOAD_STRIDE2 &&
        e->shape.tap_width == 2 && e->shape.tap_height == 2 &&
        e->index[mac.left.instruction][3] == 1)
        mask |= 1u << KSN_GRID_PIE_LOAD_FUSED;
    return mask;
}

static bool choose_route(const ksn_grid_execution *e, uint8_t mask,
                         uint64_t key, ksn_grid_pie_load_strategy *selected,
                         ksn_grid_selection_reason *reason)
{
    if (!mask) return false;
    *selected = KSN_GRID_PIE_LOAD_GATHER;
    *reason = KSN_GRID_SELECTION_FALLBACK;
    if (e->requested_strategy != KSN_GRID_PIE_LOAD_AUTO) {
        *selected = e->requested_strategy;
        if (*selected >= KSN_GRID_PIE_LOAD_FUSED + 1u ||
            !(mask & (1u << *selected))) return false;
        *reason = KSN_GRID_SELECTION_FORCED;
    } else if (profile_environment_matches()) {
        for (unsigned i = 1; i < sizeof ksn_grid_profile /
                                  sizeof ksn_grid_profile[0]; ++i) {
            const ksn_grid_profile_entry *entry = &ksn_grid_profile[i];
            if (entry->key == key &&
                entry->strategy < KSN_GRID_PIE_LOAD_FUSED + 1u &&
                (mask & (1u << entry->strategy))) {
                *selected = entry->strategy;
                *reason = KSN_GRID_SELECTION_PROFILE;
                break;
            }
        }
    }
    return true;
}

bool ksn_grid_pie_select(ksn_grid_execution *e)
{
    if (!e) return false;
    bool environment = profile_environment_matches();
    if (e->selection_valid &&
        e->selected_request_snapshot == e->requested_strategy &&
        e->profile_environment_snapshot == environment) return true;
    e->candidate_mask = ksn_grid_pie_candidates(e);
    e->selection_valid = false;
    if (!e->candidate_mask) return false;
    e->profile_key = ksn_grid_pie_profile_key(e);
    if (!choose_route(e, e->candidate_mask, e->profile_key,
                      &e->selected_strategy, &e->selection_reason)) return false;
    e->selected_request_snapshot = e->requested_strategy;
    e->profile_environment_snapshot = environment;
    e->selection_valid = true;
    return true;
}

/* Selection is complete before execution. Alignment and extra vector cells
 * are still checked per block; a failed fast load safely gathers instead. */
static load_mode classify_load(const ksn_grid_execution *e,
                               const ksn_grid_mac_operand *operand,
                               bool is_input,
                               ksn_grid_pie_load_strategy selected)
{
    if (selected == KSN_GRID_PIE_LOAD_GATHER ||
        !operand->is_load) return LOAD_GATHER;
    load_mode mode = e->access[operand->instruction];
    if (selected == KSN_GRID_PIE_LOAD_FUSED && is_input &&
        mode == LOAD_STRIDE2 && e->shape.tap_width == 2 &&
        e->shape.tap_height == 2) return LOAD_GATHER;
    return mode;
}

static void dual_operands(const ksn_grid_mac_operand *a,
                          const ksn_grid_mac_operand *b,
                          const ksn_grid_mac_operand **input,
                          const ksn_grid_mac_operand **weight)
{
    *input = a;
    *weight = b;
    if (!a->is_load && b->is_load) {
        *input = b;
        *weight = a;
    }
}

/* Verified against tools/pie/stalls.py on the emitted assembly templates.
 * Compare issue + QR interlock cycles, never instruction count alone. */
#define KSN_GRID_PAIR_ISSUES 5u
#define KSN_GRID_PAIR_STALLS 1u

bool ksn_grid_pie_describe_access(const ksn_grid_execution *e,
                                  ksn_grid_pie_access_info *info)
{
    if (!e || !info || !e->safe || !e->plan || !e->plan->mac.valid)
        return false;
    if (e->plan->mac.terms == 2) {
        *info = (ksn_grid_pie_access_info){0};
        info->candidate_mask = ksn_grid_pie_candidates(e);
        info->selected = KSN_GRID_PIE_LOAD_AUTO;
        info->reason = KSN_GRID_SELECTION_NONE;
        if (info->candidate_mask && !choose_route(e, info->candidate_mask,
                ksn_grid_pie_profile_key(e), &info->selected, &info->reason))
            return false;
        ksn_grid_pie_load_strategy path = info->candidate_mask ?
            info->selected : KSN_GRID_PIE_LOAD_GATHER;
        const ksn_grid_mac *mac = &e->plan->mac;
        const ksn_grid_mac_operand *input, *weight;
        dual_operands(&mac->left, &mac->right, &input, &weight);
        info->input = classify_load(e, input, true, path);
        info->coefficient = classify_load(e, weight, false, path);
        dual_operands(&mac->extra_left, &mac->extra_right, &input, &weight);
        info->extra_input = classify_load(e, input, true, path);
        info->extra_coefficient = classify_load(e, weight, false,
                                                path);
        info->tap_issues = 6;
        info->tap_stalls = 2;
        return true;
    }
    ksn_grid_mac mac = e->plan->mac;
    if (!mac.left.is_load && mac.right.is_load) {
        ksn_grid_mac_operand tmp = mac.left;
        mac.left = mac.right;
        mac.right = tmp;
    }
    *info = (ksn_grid_pie_access_info){0};
    info->candidate_mask = ksn_grid_pie_candidates(e);
    info->selected = KSN_GRID_PIE_LOAD_AUTO;
    info->reason = KSN_GRID_SELECTION_NONE;
    if (info->candidate_mask) {
        if (e->selection_valid &&
            e->selected_request_snapshot == e->requested_strategy &&
            e->profile_environment_snapshot == profile_environment_matches()) {
            info->selected = e->selected_strategy;
            info->reason = e->selection_reason;
        } else if (!choose_route(e, info->candidate_mask,
                                  ksn_grid_pie_profile_key(e),
                                  &info->selected, &info->reason)) return false;
    }
    ksn_grid_pie_load_strategy path = info->candidate_mask ?
        info->selected : KSN_GRID_PIE_LOAD_GATHER;
    info->input = classify_load(e, &mac.left, true, path);
    info->coefficient = classify_load(e, &mac.right, false, path);
    info->tap_issues = mac.right.is_load ? 3u :
                       info->input == LOAD_STRIDE2 ? 4u : 2u;
    info->tap_stalls = 1u;
    info->pair_candidate = mac.left.is_load && !mac.right.is_load &&
        e->access[mac.left.instruction] == LOAD_STRIDE2 &&
        e->shape.tap_width == 2 &&
        e->index[mac.left.instruction][3] == 1;
    info->fused_selected = path == KSN_GRID_PIE_LOAD_FUSED &&
        (info->candidate_mask & (1u << KSN_GRID_PIE_LOAD_FUSED));
    if (info->pair_candidate) {
        info->pair_issues = KSN_GRID_PAIR_ISSUES;
        info->pair_stalls = KSN_GRID_PAIR_STALLS;
        info->split_issues = (uint8_t)(2u * info->tap_issues);
        info->split_stalls = (uint8_t)(2u * info->tap_stalls);
        info->pair_selected = path == KSN_GRID_PIE_LOAD_AFFINE &&
            info->pair_issues + info->pair_stalls <
            info->split_issues + info->split_stalls;
    }
    return true;
}

/* Bound/alias legality was checked at bind. This helper additionally checks
 * any cells a vector load would touch beyond its eight logical samples. */
static const int16_t *operand_lanes(const int16_t *source, size_t source_count,
                                    int64_t at, int64_t stride, load_mode mode,
                                    int16_t scratch[8])
{
    if (mode == LOAD_CONTIGUOUS &&
        aligned_load(source, source_count, at, 8)) {
        LOAD_COUNT(direct_taps);
        return source + (size_t)at;
    }
    if (mode == LOAD_BROADCAST) {
        for (unsigned lane = 0; lane < 8; ++lane)
            scratch[lane] = source[(size_t)at];
        LOAD_COUNT(broadcast_taps);
        return scratch;
    }
    int64_t pos = at;
    for (unsigned lane = 0; lane < 8; ++lane) {
        scratch[lane] = source[(size_t)pos];
        if (lane != 7) pos += stride;
    }
    LOAD_COUNT(gather_taps);
    return scratch;
}

static int16_t operand_value(const ksn_grid_execution *e,
                             const ksn_grid_mac_operand *operand,
                             unsigned x, unsigned y, unsigned tx, unsigned ty)
{
    if (!operand->is_load) return operand->constant;
    unsigned slot = operand->instruction;
    const ksn_grid_instruction *load = &e->plan->program.body[slot];
    return e->binding.data[load->buffer][input_address(e->index[slot],
                                                       x, y, tx, ty)];
}

__attribute__((noinline))
static void block8_fixed(const ksn_grid_execution *e, unsigned x, unsigned y,
                         const ksn_grid_mac_operand *input,
                         const int16_t coefficient[8], load_mode mode,
                         bool pair_candidate, bool fused_pair)
{
    const ksn_grid_program *p = &e->plan->program;
    const int16_t *source = input->is_load ?
        e->binding.data[p->body[input->instruction].buffer] : NULL;
    size_t source_count = input->is_load ?
        e->binding.count[p->body[input->instruction].buffer] : 0;
    const int64_t *index = input->is_load ? e->index[input->instruction] : NULL;
    int16_t *dest = e->binding.data[KSN_GRID_DEST] +
        (size_t)(e->output[0] + e->output[2] * y + x);
    int16_t lanes[8] __attribute__((aligned(16)));
    int16_t fused_lanes[16] __attribute__((aligned(16)));
    TRACE(KSN_GRID_TRACE_START_FIXED, coefficient, NULL, (int32_t)p->initial);
#if KSN_GRID_HAS_PIE
    pie_start(coefficient);
    if (p->initial) {
        pie_add_bias((int32_t)p->initial);
        pie_reload_coefficient(coefficient);
    }
#else
    int64_t accum[8];
    for (unsigned lane = 0; lane < 8; ++lane) accum[lane] = p->initial;
#endif
    int64_t row = source ? index[0] + index[1] * x + index[2] * y : 0;
    for (unsigned ty = 0; ty < e->shape.tap_height; ++ty) {
        int64_t tap_row = source ? row + index[4] * ty : 0;
        if (fused_pair) {
            const int16_t *first = operand_lanes(source, source_count,
                tap_row, index[1], LOAD_GATHER, fused_lanes);
            const int16_t *next = operand_lanes(source, source_count,
                tap_row + index[3], index[1], LOAD_GATHER, fused_lanes + 8);
#if KSN_GRID_HAS_PIE
            pie_tap_fused_pair(first, next);
#else
            for (unsigned lane = 0; lane < 8; ++lane) {
                accum[lane] += (int32_t)first[lane] * coefficient[lane];
                accum[lane] += (int32_t)next[lane] * coefficient[lane];
            }
#endif
            LOAD_COUNT(fused_pairs);
            TRACE(KSN_GRID_TRACE_FUSED_PAIR, fused_lanes, NULL, 0);
            continue;
        }
        /* For a 2x2 window, both horizontal taps are already in the same
         * aligned 16-cell source window. One VUNZIP supplies both QACC taps. */
        if (source && pair_candidate &&
            aligned_load(source, source_count, tap_row, 16)) {
            const int16_t *pair = source + (size_t)tap_row;
#if KSN_GRID_HAS_PIE
            pie_tap_unzip_pair(pair);
#else
            for (unsigned lane = 0; lane < 8; ++lane) {
                accum[lane] += (int32_t)pair[2 * lane] * coefficient[lane];
                accum[lane] += (int32_t)pair[2 * lane + 1] * coefficient[lane];
            }
#endif
            LOAD_COUNT(unzip_pairs);
            TRACE(KSN_GRID_TRACE_PAIR, pair, NULL, 0);
            continue;
        }
        for (unsigned tx = 0; tx < e->shape.tap_width; ++tx) {
            const int16_t *values = lanes;
            if (source) {
                int64_t at = tap_row + index[3] * tx;
                if (mode == LOAD_BROADCAST) {
#if KSN_GRID_HAS_PIE
                    pie_tap_broadcast_input(source + (size_t)at);
#else
                    for (unsigned lane = 0; lane < 8; ++lane)
                        accum[lane] += (int32_t)source[(size_t)at] * coefficient[lane];
#endif
                    LOAD_COUNT(broadcast_taps);
                    TRACE(KSN_GRID_TRACE_BROADCAST_INPUT,
                          source + (size_t)at, NULL, 0);
                    continue;
                }
                if (mode == LOAD_STRIDE2 &&
                    aligned_load(source, source_count, at, 16)) {
                    const int16_t *stride2 = source + (size_t)at;
#if KSN_GRID_HAS_PIE
                    pie_tap_stride2(stride2);
#else
                    for (unsigned lane = 0; lane < 8; ++lane)
                        accum[lane] += (int32_t)stride2[2 * lane] * coefficient[lane];
#endif
                    LOAD_COUNT(stride2_taps);
                    TRACE(KSN_GRID_TRACE_STRIDE2, stride2, NULL, 0);
                    continue;
                }
                values = operand_lanes(source, source_count, at, index[1], mode,
                                        lanes);
            } else {
                for (unsigned lane = 0; lane < 8; ++lane)
                    lanes[lane] = input->constant;
                LOAD_COUNT(gather_taps);
            }
#if KSN_GRID_HAS_PIE
            pie_tap(values);
#else
            for (unsigned lane = 0; lane < 8; ++lane)
                accum[lane] += (int32_t)values[lane] * coefficient[lane];
#endif
            TRACE(KSN_GRID_TRACE_TAP, values, NULL, 0);
        }
    }
#if KSN_GRID_HAS_PIE
    pie_finish(dest, p->final_shift);
#else
    for (unsigned lane = 0; lane < 8; ++lane)
        dest[lane] = extract(accum[lane], p->final_shift);
#endif
    TRACE(KSN_GRID_TRACE_FINISH, dest, NULL, p->final_shift);
}

__attribute__((noinline))
static void block8_dynamic(const ksn_grid_execution *e, unsigned x, unsigned y,
                            const ksn_grid_mac *mac, load_mode source_mode,
                            load_mode weight_mode)
{
    const ksn_grid_program *p = &e->plan->program;
    unsigned a = mac->left.instruction, b = mac->right.instruction;
    const int16_t *source = e->binding.data[p->body[a].buffer];
    const int16_t *weights = e->binding.data[p->body[b].buffer];
    const int64_t *source_index = e->index[a], *weight_index = e->index[b];
    size_t source_count = e->binding.count[p->body[a].buffer];
    size_t weight_count = e->binding.count[p->body[b].buffer];
    int16_t *dest = e->binding.data[KSN_GRID_DEST] +
        (size_t)(e->output[0] + e->output[2] * y + x);
    int16_t lanes[8] __attribute__((aligned(16)));
    int16_t coeff[8] __attribute__((aligned(16)));
    TRACE(KSN_GRID_TRACE_START_DYNAMIC, NULL, NULL, (int32_t)p->initial);
#if KSN_GRID_HAS_PIE
    pie_zero();
    if (p->initial) pie_add_bias((int32_t)p->initial);
#else
    int64_t accum[8];
    for (unsigned lane = 0; lane < 8; ++lane) accum[lane] = p->initial;
#endif
    int64_t source_row = source_index[0] + source_index[1] * x + source_index[2] * y;
    int64_t weight_row = weight_index[0] + weight_index[1] * x + weight_index[2] * y;
    for (unsigned ty = 0; ty < e->shape.tap_height; ++ty)
        for (unsigned tx = 0; tx < e->shape.tap_width; ++tx) {
            int64_t source_at = source_row + source_index[4] * ty + source_index[3] * tx;
            int64_t weight_at = weight_row + weight_index[4] * ty + weight_index[3] * tx;
            const int16_t *values = operand_lanes(source, source_count, source_at,
                                                   source_index[1], source_mode, lanes);
            if (weight_mode == LOAD_BROADCAST) {
#if KSN_GRID_HAS_PIE
                pie_tap_broadcast_coefficient(values, weights + (size_t)weight_at);
#else
                for (unsigned lane = 0; lane < 8; ++lane)
                    accum[lane] += (int32_t)values[lane] * weights[(size_t)weight_at];
#endif
                LOAD_COUNT(broadcast_taps);
                TRACE(KSN_GRID_TRACE_BROADCAST_COEFFICIENT,
                      values, weights + (size_t)weight_at, 0);
                continue;
            }
            const int16_t *factors = operand_lanes(weights, weight_count, weight_at,
                                                    weight_index[1], weight_mode, coeff);
#if KSN_GRID_HAS_PIE
            pie_tap_dynamic(values, factors);
#else
            for (unsigned lane = 0; lane < 8; ++lane)
                accum[lane] += (int32_t)values[lane] * factors[lane];
#endif
            TRACE(KSN_GRID_TRACE_DYNAMIC, values, factors, 0);
        }
#if KSN_GRID_HAS_PIE
    pie_finish(dest, p->final_shift);
#else
    for (unsigned lane = 0; lane < 8; ++lane)
        dest[lane] = extract(accum[lane], p->final_shift);
#endif
    TRACE(KSN_GRID_TRACE_FINISH, dest, NULL, p->final_shift);
}

/* A two-term expression uses the same QACC and preserves tap/term order.
 * AFFINE takes only aligned contiguous loads directly. Reverse and failed
 * per-block guards gather into scratch without changing the selected route. */
__attribute__((noinline))
static void block8_dual(const ksn_grid_execution *e, unsigned x, unsigned y,
                        ksn_grid_pie_load_strategy selected)
{
    const ksn_grid_program *p = &e->plan->program;
    const ksn_grid_mac *mac = &e->plan->mac;
    const ksn_grid_mac_operand *left[2] = {&mac->left, &mac->extra_left};
    const ksn_grid_mac_operand *right[2] = {&mac->right, &mac->extra_right};
    int16_t *dest = e->binding.data[KSN_GRID_DEST] +
        (size_t)(e->output[0] + e->output[2] * y + x);
    int16_t a[8] __attribute__((aligned(16)));
    int16_t b[8] __attribute__((aligned(16)));
    TRACE(KSN_GRID_TRACE_START_DYNAMIC, NULL, NULL, (int32_t)p->initial);
#if KSN_GRID_HAS_PIE
    pie_zero();
    if (p->initial) pie_add_bias((int32_t)p->initial);
#else
    int64_t accum[8];
    for (unsigned lane = 0; lane < 8; ++lane) accum[lane] = p->initial;
#endif
    for (unsigned ty = 0; ty < e->shape.tap_height; ++ty)
        for (unsigned tx = 0; tx < e->shape.tap_width; ++tx)
            for (unsigned term = 0; term < 2; ++term) {
                const ksn_grid_mac_operand *input, *weight;
                dual_operands(left[term], right[term], &input, &weight);
                const int16_t *values = a;
                if (input->is_load) {
                    unsigned slot = input->instruction;
                    unsigned buffer = p->body[slot].buffer;
                    const int64_t *index = e->index[slot];
                    int64_t at = index[0] + index[1] * x + index[2] * y +
                                 index[3] * tx + index[4] * ty;
                    values = operand_lanes(e->binding.data[buffer],
                        e->binding.count[buffer], at, index[1],
                        classify_load(e, input, true, selected), a);
                } else {
                    for (unsigned lane = 0; lane < 8; ++lane)
                        a[lane] = input->constant;
                }
                if (!weight->is_load) {
#if KSN_GRID_HAS_PIE
                    pie_tap_broadcast_coefficient(values, &weight->constant);
#else
                    for (unsigned lane = 0; lane < 8; ++lane)
                        accum[lane] += (int32_t)values[lane] * weight->constant;
#endif
                    LOAD_COUNT(broadcast_taps);
                    TRACE(KSN_GRID_TRACE_BROADCAST_COEFFICIENT,
                          values, &weight->constant, 0);
                    continue;
                }
                unsigned slot = weight->instruction;
                unsigned buffer = p->body[slot].buffer;
                const int64_t *index = e->index[slot];
                int64_t at = index[0] + index[1] * x + index[2] * y +
                             index[3] * tx + index[4] * ty;
                load_mode weight_mode = classify_load(e, weight, false,
                                                      selected);
                if (weight_mode == LOAD_BROADCAST) {
#if KSN_GRID_HAS_PIE
                    pie_tap_broadcast_coefficient(values,
                        e->binding.data[buffer] + (size_t)at);
#else
                    for (unsigned lane = 0; lane < 8; ++lane)
                        accum[lane] += (int32_t)values[lane] *
                            e->binding.data[buffer][(size_t)at];
#endif
                    LOAD_COUNT(broadcast_taps);
                    TRACE(KSN_GRID_TRACE_BROADCAST_COEFFICIENT, values,
                          e->binding.data[buffer] + (size_t)at, 0);
                    continue;
                }
                const int16_t *factors = operand_lanes(
                    e->binding.data[buffer], e->binding.count[buffer],
                    at, index[1], weight_mode, b);
#if KSN_GRID_HAS_PIE
                pie_tap_dynamic(values, factors);
#else
                for (unsigned lane = 0; lane < 8; ++lane)
                    accum[lane] += (int32_t)values[lane] * factors[lane];
#endif
                TRACE(KSN_GRID_TRACE_DYNAMIC, values, factors, 0);
            }
#if KSN_GRID_HAS_PIE
    pie_finish(dest, p->final_shift);
#else
    for (unsigned lane = 0; lane < 8; ++lane)
        dest[lane] = extract(accum[lane], p->final_shift);
#endif
    TRACE(KSN_GRID_TRACE_FINISH, dest, NULL, p->final_shift);
}

ksn_grid_status ksn_grid_run_pie(ksn_grid_execution *e)
{
    if (!e || !ksn_grid_pie_backend_available() || !ksn_grid_pie_eligible(e))
        return KSN_GRID_BAD_IR;
    if (!ksn_grid_pie_select(e)) return KSN_GRID_BAD_IR;
    const ksn_grid_program *p = &e->plan->program;
    ksn_grid_mac mac = e->plan->mac;
    if (mac.terms == 2) {
        for (unsigned y = 0; y < e->shape.height; ++y) {
            unsigned x = 0;
            for (; x + 8 <= e->shape.width; x += 8)
                block8_dual(e, x, y, e->selected_strategy);
            for (; x < e->shape.width; ++x) {
                int64_t sum = p->initial;
                for (unsigned ty = 0; ty < e->shape.tap_height; ++ty)
                    for (unsigned tx = 0; tx < e->shape.tap_width; ++tx) {
                        sum += (int32_t)operand_value(e, &mac.left, x, y, tx, ty) *
                               operand_value(e, &mac.right, x, y, tx, ty);
                        sum += (int32_t)operand_value(e, &mac.extra_left,
                                                      x, y, tx, ty) *
                               operand_value(e, &mac.extra_right, x, y, tx, ty);
                    }
                size_t at = (size_t)(e->output[0] + e->output[2] * y + x);
                e->binding.data[KSN_GRID_DEST][at] = extract(sum, p->final_shift);
            }
        }
        e->pie_backend_selected = true;
        return KSN_GRID_OK;
    }
    /* A constant operand occupies q1 for the whole block. When both operands
     * are loads, q1 is gathered and reloaded for every tap. */
    if (!mac.left.is_load && mac.right.is_load) {
        ksn_grid_mac_operand tmp = mac.left;
        mac.left = mac.right;
        mac.right = tmp;
    }
    ksn_grid_pie_access_info access;
    if (!ksn_grid_pie_describe_access(e, &access)) return KSN_GRID_BAD_IR;
    load_mode left_mode = access.input;
    load_mode right_mode = access.coefficient;
    bool pair_candidate = access.pair_selected;
    bool fused_pair = access.fused_selected;
    int16_t coefficient[8] __attribute__((aligned(16)));
    for (unsigned lane = 0; lane < 8; ++lane)
        coefficient[lane] = mac.right.constant;
    for (unsigned y = 0; y < e->shape.height; ++y) {
        unsigned x = 0;
        for (; x + 8 <= e->shape.width; x += 8)
            if (mac.right.is_load)
                block8_dynamic(e, x, y, &mac, left_mode, right_mode);
            else block8_fixed(e, x, y, &mac.left, coefficient, left_mode,
                              pair_candidate, fused_pair);
        for (; x < e->shape.width; ++x) {
            int64_t sum = p->initial;
            for (unsigned ty = 0; ty < e->shape.tap_height; ++ty)
                for (unsigned tx = 0; tx < e->shape.tap_width; ++tx)
                    sum += (int32_t)operand_value(e, &mac.left, x, y, tx, ty) *
                           operand_value(e, &mac.right, x, y, tx, ty);
            size_t at = (size_t)(e->output[0] + e->output[2] * y + x);
            e->binding.data[KSN_GRID_DEST][at] = extract(sum, p->final_shift);
        }
    }
    e->pie_backend_selected = true;
    return KSN_GRID_OK;
}

/* A lane is one row, not one adjacent column. Each row advances in its
 * original order; the eight rows share a PIE operation for each column. */
ksn_grid_status ksn_grid_run_scan_pie(ksn_grid_execution *e)
{
    if (!ksn_grid_pie_backend_available() || !ksn_grid_scan_pie_eligible(e))
        return KSN_GRID_BAD_IR;
    const ksn_grid_instruction *src =
        &e->plan->program.body[e->scan_source_slot];
    const int16_t *input = e->binding.data[src->buffer];
    int16_t *dest = e->binding.data[KSN_GRID_DEST];
    const int64_t *source_index = e->index[e->scan_source_slot];
    const int64_t *output_index = e->output;
#if KSN_GRID_HAS_PIE
    int16_t coefficient[8] __attribute__((aligned(16)));
    int16_t ones[8] __attribute__((aligned(16)));
    for (unsigned lane = 0; lane < 8; ++lane) {
        coefficient[lane] = e->scan_prev_coefficient;
        ones[lane] = 1;
    }
#endif
    int16_t previous[8] __attribute__((aligned(16)));
    int16_t samples[8] __attribute__((aligned(16)));
    int16_t result[8] __attribute__((aligned(16)));
    unsigned y = 0;
    for (; y + 8 <= e->shape.height; y += 8) {
        for (unsigned x = 0; x < e->shape.width; ++x) {
            for (unsigned lane = 0; lane < 8; ++lane) {
                size_t at = (size_t)(output_index[0] + output_index[1] * x +
                                     output_index[2] * (y + lane));
                previous[lane] = dest[at - 1];
                samples[lane] = input[input_address(source_index, x,
                                                     y + lane, 0, 0)];
            }
#if KSN_GRID_HAS_PIE
            if (e->scan_prev_coefficient == 1 &&
                !e->plan->program.final_shift)
                pie_scan_step(previous, samples, result);
            else
                pie_scan_affine_step(previous, samples, coefficient, ones,
                                     e->plan->program.final_shift, result);
#else
            for (unsigned lane = 0; lane < 8; ++lane)
                result[lane] = extract((int32_t)previous[lane] *
                                       e->scan_prev_coefficient + samples[lane],
                                       e->plan->program.final_shift);
#endif
            TRACE(KSN_GRID_TRACE_SCAN, previous, samples,
                  e->scan_prev_coefficient);
            TRACE(KSN_GRID_TRACE_FINISH, result, NULL,
                  e->plan->program.final_shift);
            for (unsigned lane = 0; lane < 8; ++lane) {
                size_t at = (size_t)(output_index[0] + output_index[1] * x +
                                     output_index[2] * (y + lane));
                dest[at] = result[lane];
            }
        }
    }
    for (; y < e->shape.height; ++y)
        for (unsigned x = 0; x < e->shape.width; ++x) {
            size_t at = (size_t)(output_index[0] + output_index[1] * x +
                                 output_index[2] * y);
            int16_t sample = input[input_address(source_index, x, y, 0, 0)];
            dest[at] = extract((int32_t)dest[at - 1] *
                               e->scan_prev_coefficient + sample,
                               e->plan->program.final_shift);
        }
    e->pie_backend_selected = true;
    return KSN_GRID_OK;
}

ksn_grid_status ksn_grid_run_auto(ksn_grid_execution *e,
                                  const ksn_grid_pie_policy *policy)
{
    if (!e || !e->safe) return KSN_GRID_BAD_IR;
    e->pie_backend_selected = false;
    uint32_t outputs = (uint32_t)e->shape.width * e->shape.height;
    if (policy && policy->enable_pie && policy->min_scan_rows >= 8 &&
        e->shape.height >= policy->min_scan_rows &&
        ksn_grid_pie_backend_available() && ksn_grid_scan_pie_eligible(e))
        return ksn_grid_run_scan_pie(e);
    if (policy && policy->enable_pie && ksn_grid_pie_backend_available() &&
        ksn_grid_pie_eligible(e)) {
        const ksn_grid_mac *mac = &e->plan->mac;
        bool loaded_weight = (mac->left.is_load && mac->right.is_load) ||
            (mac->terms == 2 && mac->extra_left.is_load &&
             mac->extra_right.is_load);
        uint16_t minimum = loaded_weight ? policy->min_loaded_weight_outputs :
                                           policy->min_outputs;
        if (minimum >= 8 && outputs >= minimum) return ksn_grid_run_pie(e);
    }
    return ksn_grid_run_scalar(e);
}
