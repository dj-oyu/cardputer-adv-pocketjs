#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "ayatsuri_p384_profile.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/private/ecdsa.h"

struct measurement {
    uint32_t verify_us, point_us, verify_ticks, point_ticks;
    uint32_t field_us[AY_P384_FIELD_OPS], calls[AY_P384_FIELD_OPS];
    uint32_t digest_prefix;
    unsigned digest_bytes, point_calls, table_entries;
    bool detailed, static_table, table_used;
    int status;
#ifdef CONFIG_AYATSURI_P384_MODEXP
    uint32_t inv_us;
    uint8_t inv_calls, inv_hw, inv_fallback, inv_starts;
#endif
};

static struct measurement rows[8], *active;
static unsigned used, overflow;
static bool detail_setting, scope_detail, in_field;
#ifdef CONFIG_AYATSURI_P384_G_TABLE
static bool table_setting;
#endif
static _Atomic(TaskHandle_t) scope_owner, field_owner;
static int (*original_reduce)(mbedtls_mpi *);
#ifdef CONFIG_AYATSURI_P384_MODEXP
static const mbedtls_ecp_group *point_group;
static bool modexp_requested, modexp_tested;
static _Atomic(TaskHandle_t) modexp_owner;
static unsigned modexp_starts;
#endif

static bool on_task(_Atomic(TaskHandle_t) *owner)
{
    TaskHandle_t task = atomic_load_explicit(owner, memory_order_relaxed);
    return task && task == xTaskGetCurrentTaskHandle();
}

static uint32_t task_ticks(void)
{
#if configGENERATE_RUN_TIME_STATS
    TaskStatus_t status;
    vTaskGetInfo(NULL, &status, pdFALSE, eInvalid);
    return status.ulRunTimeCounter;
#else
    return 0;
#endif
}

void ay_p384_set_detail(bool detailed) { detail_setting = detailed; }
bool ay_p384_detail(void) { return detail_setting; }

void ay_p384_set_table(bool enabled)
{
#ifdef CONFIG_AYATSURI_P384_G_TABLE
    table_setting = enabled;
#else
    (void)enabled;
#endif
}

void ay_p384_scope_begin(bool detailed)
{
    scope_detail = detailed;
    atomic_store(&scope_owner, xTaskGetCurrentTaskHandle());
}

void ay_p384_scope_end(void)
{
    atomic_store(&scope_owner, NULL);
}

void ay_p384_report(const char *phase)
{
    static const char *const names[] = {"mul", "sqr", "quasi_reduce", "add", "sub", "invert"};
    for (unsigned i = 0; i < used; ++i) {
        const struct measurement *row = &rows[i];
        ESP_LOGI("P384", "phase=%s sig=%u hash_prefix=%08" PRIx32 " digest_bytes=%u rc=%d detail=%u"
                 " verify_us=%" PRIu32 " point_us=%" PRIu32 " point_calls=%u"
                 " verify_ticks=%" PRIu32 " point_ticks=%" PRIu32 " static_g_table=%u dynamic_g_entries=%u"
                 " g_table_used=%u",
                 phase, i + 1, row->digest_prefix, row->digest_bytes, row->status, (unsigned)row->detailed,
                 row->verify_us, row->point_us, row->point_calls, row->verify_ticks, row->point_ticks,
                 (unsigned)row->static_table, row->table_entries, (unsigned)row->table_used);
        if (row->detailed) {
            for (unsigned k = 0; k < AY_P384_FIELD_OPS; ++k)
                ESP_LOGI("P384", "phase=%s sig=%u field=%s calls=%" PRIu32 " us=%" PRIu32,
                         phase, i + 1, names[k], row->calls[k], row->field_us[k]);
        }
#ifdef CONFIG_AYATSURI_P384_MODEXP
        ESP_LOGI("P384_INV", "phase=%s sig=%u field_inv_calls=%u field_inv_hw=%u"
                 " field_inv_fallback=%u field_inv_starts=%u field_inv_us=%" PRIu32,
                 phase, i + 1, row->inv_calls, row->inv_hw, row->inv_fallback, row->inv_starts, row->inv_us);
#endif
    }
    if (overflow) ESP_LOGW("P384", "phase=%s dropped=%u", phase, overflow);
    used = overflow = 0;
}

extern __typeof__(mbedtls_ecdsa_verify) __real_mbedtls_ecdsa_verify;
int __wrap_mbedtls_ecdsa_verify(mbedtls_ecp_group *grp, const unsigned char *buf, size_t len,
                                const mbedtls_ecp_point *q, const mbedtls_mpi *r, const mbedtls_mpi *s)
{
    if (!on_task(&scope_owner) || grp->id != MBEDTLS_ECP_DP_SECP384R1 || active)
        return __real_mbedtls_ecdsa_verify(grp, buf, len, q, r, s);
    if (used >= sizeof rows / sizeof rows[0]) {
        ++overflow;
        return __real_mbedtls_ecdsa_verify(grp, buf, len, q, r, s);
    }
    struct measurement *row = &rows[used++];
    memset(row, 0, sizeof *row);
    row->detailed = scope_detail;
    row->digest_bytes = len;
    for (unsigned i = 0; i < 4 && i < len; ++i) row->digest_prefix = row->digest_prefix << 8 | buf[i];
    row->table_entries = grp->MBEDTLS_PRIVATE(T_size);
    row->static_table = grp->MBEDTLS_PRIVATE(T) != NULL && row->table_entries == 0;
    active = row;
    uint32_t ticks = task_ticks();
    int64_t start = esp_timer_get_time();
    row->status = __real_mbedtls_ecdsa_verify(grp, buf, len, q, r, s);
    row->verify_us = esp_timer_get_time() - start;
    row->verify_ticks = task_ticks() - ticks;
    active = NULL;
    return row->status;
}

int64_t ay_p384_field_begin(void)
{
    if (!on_task(&field_owner) || in_field) return -1;
    in_field = true;
    return esp_timer_get_time();
}

void ay_p384_field_end(enum ay_p384_field_op op, int64_t start)
{
    active->field_us[op] += esp_timer_get_time() - start;
    ++active->calls[op];
    in_field = false;
}

static int timed_reduce(mbedtls_mpi *value)
{
    int64_t start = ay_p384_field_begin();
    int result = original_reduce(value);
    if (start >= 0) ay_p384_field_end(AY_P384_REDUCE, start);
    return result;
}

extern __typeof__(mbedtls_ecp_muladd_restartable) __real_mbedtls_ecp_muladd_restartable;
int __wrap_mbedtls_ecp_muladd_restartable(mbedtls_ecp_group *grp, mbedtls_ecp_point *out,
    const mbedtls_mpi *m, const mbedtls_ecp_point *p, const mbedtls_mpi *n,
    const mbedtls_ecp_point *q, mbedtls_ecp_restart_ctx *rs)
{
    if (!on_task(&scope_owner) || !active || grp->id != MBEDTLS_ECP_DP_SECP384R1)
        return __real_mbedtls_ecp_muladd_restartable(grp, out, m, p, n, q, rs);
#ifdef CONFIG_AYATSURI_P384_G_TABLE
    /* TF-PSA marks t_data as unused. Its isolated per-signature group can
     * opt into the immutable table for the generator operand of u1*G+u2*Q. */
    bool select_table = table_setting && active->static_table && p == &grp->G &&
                        !rs && !grp->MBEDTLS_PRIVATE(t_data);
    if (select_table) grp->MBEDTLS_PRIVATE(t_data) = grp;
#ifdef CONFIG_AYATSURI_P384_MODEXP
    if (select_table) point_group = grp;
#endif
#endif
    /* The PSA builtin allocates this group for this verification operation.
     * Its fast reducer is a pointer to static code. Restore even after errors. */
    if (active->detailed) {
        original_reduce = grp->MBEDTLS_PRIVATE(modp);
        if (original_reduce) grp->MBEDTLS_PRIVATE(modp) = timed_reduce;
        atomic_store(&field_owner, xTaskGetCurrentTaskHandle());
    }
    uint32_t ticks = task_ticks();
    int64_t start = esp_timer_get_time();
    int result = __real_mbedtls_ecp_muladd_restartable(grp, out, m, p, n, q, rs);
    active->point_us += esp_timer_get_time() - start;
    active->point_ticks += task_ticks() - ticks;
    ++active->point_calls;
#ifdef CONFIG_AYATSURI_P384_MODEXP
    point_group = NULL;
#endif
#ifdef CONFIG_AYATSURI_P384_G_TABLE
    if (select_table) {
        /* The pinned ECP table selection branch acknowledges actual use. */
        active->table_used |= grp->MBEDTLS_PRIVATE(t_data) == &grp->G;
        grp->MBEDTLS_PRIVATE(t_data) = NULL;
    }
#endif
    if (active->detailed) {
        atomic_store(&field_owner, NULL);
        grp->MBEDTLS_PRIVATE(modp) = original_reduce;
        original_reduce = NULL;
    }
    return result;
}

#define TIME_BINARY_MPI(name, op)                                                        \
    extern __typeof__(mbedtls_mpi_##name) __real_mbedtls_mpi_##name;                      \
    int __wrap_mbedtls_mpi_##name(mbedtls_mpi *x, const mbedtls_mpi *a, const mbedtls_mpi *b) \
    {                                                                                    \
        int64_t start = ay_p384_field_begin();                                            \
        int result = __real_mbedtls_mpi_##name(x, a, b);                                  \
        if (start >= 0) ay_p384_field_end(op, start);                                     \
        return result;                                                                   \
    }
TIME_BINARY_MPI(add_mpi, AY_P384_ADD)
TIME_BINARY_MPI(sub_mpi, AY_P384_SUB)

extern int __real_mbedtls_mpi_gcd_modinv_odd(mbedtls_mpi *, mbedtls_mpi *, const mbedtls_mpi *, const mbedtls_mpi *);
#ifdef CONFIG_AYATSURI_CERT_INV_SCRATCH
extern int ay_cert_inv_dispatch(mbedtls_mpi *, mbedtls_mpi *, const mbedtls_mpi *, const mbedtls_mpi *);
#endif
static int stock_inverse(mbedtls_mpi *g, mbedtls_mpi *inv, const mbedtls_mpi *a, const mbedtls_mpi *n)
{
#ifdef CONFIG_AYATSURI_CERT_INV_SCRATCH
    return ay_cert_inv_dispatch(g, inv, a, n);
#else
    return __real_mbedtls_mpi_gcd_modinv_odd(g, inv, a, n);
#endif
}

#ifdef CONFIG_AYATSURI_P384_MODEXP
#if !defined(CONFIG_IDF_TARGET_ESP32S3) || !defined(CONFIG_MBEDTLS_HARDWARE_MPI) || !defined(CONFIG_AYATSURI_P384_G_TABLE)
#error "P-384 field MODEXP requires the S3 hardware MPI and scoped generator table"
#endif
enum { FIELD_WORDS = 12 };
_Static_assert(sizeof(mbedtls_mpi_uint) == 4, "S3 MPI uses 32-bit limbs");
/* Little-endian limbs for p = 2^384 - 2^128 - 2^96 + 2^32 - 1,
 * p-2, and (2^384)^2 mod p. ESP-IDF 6.0.1's S3 multiplier uses
 * exactly 12 words here (no rounding). The driver reads populated RR. */
static const mbedtls_mpi_uint prime_words[FIELD_WORDS] = {
    0xffffffff, 0, 0, 0xffffffff, 0xfffffffe, 0xffffffff, 0xffffffff, 0xffffffff,
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff
};
static const mbedtls_mpi_uint exponent_words[FIELD_WORDS] = {
    0xfffffffd, 0, 0, 0xffffffff, 0xfffffffe, 0xffffffff, 0xffffffff, 0xffffffff,
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff
};
static const mbedtls_mpi_uint rr_words[FIELD_WORDS] = {
    1, 0xfffffffe, 0, 2, 0, 0xfffffffe, 0, 2, 1, 0, 0, 0
};
#define FIELD_MPI(words) { .MBEDTLS_PRIVATE(p) = (mbedtls_mpi_uint *)(words), \
                           .MBEDTLS_PRIVATE(s) = 1, .MBEDTLS_PRIVATE(n) = FIELD_WORDS }
static const mbedtls_mpi prime = FIELD_MPI(prime_words);
static const mbedtls_mpi exponent = FIELD_MPI(exponent_words);

void ay_p384_set_modexp(bool enabled) { modexp_requested = enabled; }

void ay_p384_note_modexp_start(void)
{
    if (on_task(&modexp_owner)) ++modexp_starts;
}

/* Return false with the original input intact so a failed or unavailable
 * accelerator can fall back even for I == A. GCD outputs and noncanonical
 * inputs retain the full stock algebra and error semantics. The driver calls
 * grow(out, 12), already satisfied by this exact-size writable stack result. */
static bool try_field_inverse(mbedtls_mpi *g, mbedtls_mpi *inv, const mbedtls_mpi *a, const mbedtls_mpi *n,
                              unsigned *starts)
{
    *starts = 0;
    if (g || !inv || a == n || inv == n || n->MBEDTLS_PRIVATE(n) != FIELD_WORDS ||
        mbedtls_mpi_cmp_mpi(n, &prime) != 0 || mbedtls_mpi_cmp_int(a, 0) <= 0 ||
        mbedtls_mpi_cmp_mpi(a, n) >= 0) return false;
    mbedtls_mpi_uint words[FIELD_WORDS];
    mbedtls_mpi output = FIELD_MPI(words);
    mbedtls_mpi rr = FIELD_MPI(rr_words);
    modexp_starts = 0;
    atomic_store(&modexp_owner, xTaskGetCurrentTaskHandle());
    int ret = mbedtls_mpi_exp_mod(&output, a, &exponent, n, &rr);
    atomic_store(&modexp_owner, NULL);
    *starts = modexp_starts;
    if (ret || *starts != 1 || mbedtls_mpi_cmp_int(&output, 0) <= 0 ||
        mbedtls_mpi_cmp_mpi(&output, n) >= 0 || mbedtls_mpi_grow(inv, FIELD_WORDS) != 0)
        return false;
    memcpy(inv->MBEDTLS_PRIVATE(p), words, sizeof words);
    memset(inv->MBEDTLS_PRIVATE(p) + FIELD_WORDS, 0,
           (inv->MBEDTLS_PRIVATE(n) - FIELD_WORDS) * sizeof *words);
    inv->MBEDTLS_PRIVATE(s) = 1;
    return true;
}

bool ay_p384_modexp_selftest(void)
{
    mbedtls_mpi a, result, expected;
    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&result);
    mbedtls_mpi_init(&expected);
    unsigned starts = 0;
    /* 2^-1 = (p+1)/2, tested with fresh output and oversized in-place input. */
    bool ok = !mbedtls_mpi_add_int(&expected, &prime, 1) && !mbedtls_mpi_shift_r(&expected, 1) &&
              !mbedtls_mpi_lset(&a, 2) && try_field_inverse(NULL, &result, &a, &prime, &starts) &&
              result.MBEDTLS_PRIVATE(n) == FIELD_WORDS && !mbedtls_mpi_cmp_mpi(&result, &expected) &&
              !try_field_inverse(&result, &a, &a, &prime, &starts) && starts == 0 &&
              !mbedtls_mpi_grow(&a, FIELD_WORDS + 2) && try_field_inverse(NULL, &a, &a, &prime, &starts) &&
              a.MBEDTLS_PRIVATE(n) == FIELD_WORDS + 2 && !mbedtls_mpi_cmp_mpi(&a, &expected);
    /* The pre-existing output contains high limbs and a negative sign. */
    ok = ok && !mbedtls_mpi_set_bit(&result, FIELD_WORDS * 32, 1);
    result.MBEDTLS_PRIVATE(s) = -1;
    ok = ok && try_field_inverse(NULL, &result, &a, &prime, &starts) &&
         !mbedtls_mpi_cmp_int(&result, 2) && !mbedtls_mpi_lset(&a, 0) &&
         !try_field_inverse(NULL, &result, &a, &prime, &starts) && starts == 0 &&
         !try_field_inverse(NULL, &result, &prime, &prime, &starts) && starts == 0 &&
         !mbedtls_mpi_cmp_int(&result, 2);
    mbedtls_mpi_free(&expected);
    mbedtls_mpi_free(&result);
    mbedtls_mpi_free(&a);
    modexp_tested = ok;
    ESP_LOGI("P384_INV", "modexp_selftest=%s", ok ? "pass" : "fail");
    return ok;
}
#endif

int __wrap_mbedtls_mpi_gcd_modinv_odd(mbedtls_mpi *g, mbedtls_mpi *inv, const mbedtls_mpi *a,
                                      const mbedtls_mpi *n)
{
    int64_t start = ay_p384_field_begin();
    int result;
#ifdef CONFIG_AYATSURI_P384_MODEXP
    /* The P-384 ECDSA task owns this group. The internal table selection
     * acknowledges consumption before the first target normalization. */
    if (on_task(&scope_owner) && point_group && n == &point_group->P &&
        point_group->MBEDTLS_PRIVATE(t_data) == &point_group->G) {
        int64_t inv_start = esp_timer_get_time();
        unsigned starts = 0;
        bool done = modexp_requested && modexp_tested && try_field_inverse(g, inv, a, n, &starts);
        result = done ? 0 : stock_inverse(g, inv, a, n);
        active->inv_us += esp_timer_get_time() - inv_start;
        ++active->inv_calls;
        active->inv_hw += done;
        active->inv_fallback += modexp_requested && !done;
        active->inv_starts += starts;
    } else
#endif
    {
        result = stock_inverse(g, inv, a, n);
    }
    if (start >= 0) ay_p384_field_end(AY_P384_INVERT, start);
    return result;
}
