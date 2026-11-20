#include "ayatsuri_cert_inv_scratch.h"

#if CONFIG_AYATSURI_CERT_INV_SCRATCH
#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/bignum.h"
#include "mbedtls/platform_util.h"

/* IDF 6.0.1 TF-PSA Crypto private interfaces used by ecp.c and bignum.c. */
extern int __real_mbedtls_mpi_gcd_modinv_odd(mbedtls_mpi *g, mbedtls_mpi *inv,
                                             const mbedtls_mpi *a, const mbedtls_mpi *n);
extern void mbedtls_mpi_core_gcd_modinv_odd(mbedtls_mpi_uint *g, mbedtls_mpi_uint *inv,
                                            const mbedtls_mpi_uint *a, size_t a_limbs,
                                            const mbedtls_mpi_uint *n, size_t n_limbs,
                                            mbedtls_mpi_uint *scratch);

enum { FIELD_LIMBS = 12, SCRATCH_LIMBS = 5 * FIELD_LIMBS };
_Static_assert(SCRATCH_LIMBS * sizeof(mbedtls_mpi_uint) == 240,
               "The measured certificate inversion used 240 bytes of scratch");
static _Atomic(TaskHandle_t) owner;
static unsigned used;
static bool tested;

/* ecp_normalize_jac_many calls gcd_modinv_odd(NULL, &c[i], &c[i], &grp->P).
 * The original allocates a local GCD result and grows the inverse before it
 * allocates T[5*n]. Preserve both of those allocations and the original core.
 * Keeping this frame out of the wrapper avoids adding scratch to other calls. */
static __attribute__((noinline)) int invert_in_place(mbedtls_mpi *inv, const mbedtls_mpi *n)
{
    mbedtls_mpi local_g;
    mbedtls_mpi_uint scratch[SCRATCH_LIMBS] = {0};
    mbedtls_mpi_init(&local_g);
    int ret = mbedtls_mpi_grow(&local_g, FIELD_LIMBS);
    if (!ret) ret = mbedtls_mpi_grow(inv, FIELD_LIMBS);
    if (!ret) {
        /* At this point inv has at least 12 limbs. Any limbs beyond the field
         * are zero because the input is positive and strictly below n. The
         * original passes min(A->n,N->n) after growing this same aliased MPI. */
        mbedtls_mpi_uint *out = inv->MBEDTLS_PRIVATE(p);
        mbedtls_mpi_core_gcd_modinv_odd(local_g.MBEDTLS_PRIVATE(p), out, out, FIELD_LIMBS,
                                        n->MBEDTLS_PRIVATE(p), FIELD_LIMBS, scratch);
        inv->MBEDTLS_PRIVATE(s) = 1;
        if (inv->MBEDTLS_PRIVATE(n) > FIELD_LIMBS) {
            memset(out + FIELD_LIMBS, 0,
                   (inv->MBEDTLS_PRIVATE(n) - FIELD_LIMBS) * sizeof(*out));
        }
        mbedtls_platform_zeroize(scratch, sizeof(scratch));
    }
    mbedtls_mpi_free(&local_g);
    return ret;
}

static int cert_inverse(mbedtls_mpi *g, mbedtls_mpi *inv, const mbedtls_mpi *a,
                        const mbedtls_mpi *n, unsigned *count)
{
    if (g || !inv || inv != a || inv == n || n->MBEDTLS_PRIVATE(n) != FIELD_LIMBS ||
        n->MBEDTLS_PRIVATE(s) != 1 || mbedtls_mpi_bitlen(n) != 384 || !mbedtls_mpi_get_bit(n, 0) ||
        mbedtls_mpi_cmp_int(a, 0) <= 0 || mbedtls_mpi_cmp_mpi(a, n) >= 0)
        return __real_mbedtls_mpi_gcd_modinv_odd(g, inv, a, n);
    ++*count;
    return invert_in_place(inv, n);
}

#ifdef CONFIG_AYATSURI_P384_PROFILE
#define CERT_INV_DISPATCH ay_cert_inv_dispatch
#else
#define CERT_INV_DISPATCH __wrap_mbedtls_mpi_gcd_modinv_odd
#endif
int CERT_INV_DISPATCH(mbedtls_mpi *g, mbedtls_mpi *inv,
                                       const mbedtls_mpi *a, const mbedtls_mpi *n)
{
    TaskHandle_t task = atomic_load_explicit(&owner, memory_order_acquire);
    if (task && !xPortInIsrContext() && task == xTaskGetCurrentTaskHandle())
        return cert_inverse(g, inv, a, n, &used);
    return __real_mbedtls_mpi_gcd_modinv_odd(g, inv, a, n);
}

void ay_cert_inv_scratch_begin(bool enabled)
{
    used = 0;
    atomic_store_explicit(&owner, enabled && tested ? xTaskGetCurrentTaskHandle() : NULL,
                          memory_order_release);
}

unsigned ay_cert_inv_scratch_end(void)
{
    atomic_store_explicit(&owner, NULL, memory_order_release);
    return used;
}

/* Expected inverses are independently computed integer test vectors over the
 * standard P-384 field. Also compare status, value, and capacity with the stock
 * wrapper, including an input with deliberately oversized backing storage. */
static bool inverse_vector(const mbedtls_mpi *n, const char *input, const char *expected,
                           size_t capacity, unsigned expected_calls)
{
    mbedtls_mpi a, b, answer;
    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&b);
    mbedtls_mpi_init(&answer);
    unsigned calls = 0;
    bool ok = !mbedtls_mpi_read_string(&a, 16, input) &&
              !mbedtls_mpi_copy(&b, &a) && !mbedtls_mpi_grow(&a, capacity) &&
              !mbedtls_mpi_grow(&b, capacity) && !mbedtls_mpi_read_string(&answer, 16, expected);
    if (ok) {
        int ref = __real_mbedtls_mpi_gcd_modinv_odd(NULL, &b, &b, n);
        int result = cert_inverse(NULL, &a, &a, n, &calls);
        ok = !ref && !result && calls == expected_calls && !mbedtls_mpi_cmp_mpi(&a, &answer) &&
             !mbedtls_mpi_cmp_mpi(&a, &b) && a.MBEDTLS_PRIVATE(n) == b.MBEDTLS_PRIVATE(n);
    }
    mbedtls_mpi_free(&answer);
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&a);
    return ok;
}

bool ay_cert_inv_scratch_selftest(void)
{
    static const char prime[] =
        "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff";
    static const char half[] =
        "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7fffffff800000000000000080000000";
    mbedtls_mpi n;
    mbedtls_mpi_init(&n);
    tested = !mbedtls_mpi_read_string(&n, 16, prime) && inverse_vector(&n, "2", half, 1, 1) &&
             inverse_vector(&n, "de4dbeefdf213e5576be76412d174834ac56fbe0105cc509",
                            "927ffa0fd8352950644234364bea93ed5c68e48de9d2df2c4711b2e9575407e9"
                            "6f5255c5d4d9a1ad58b625fe7570dbf6", FIELD_LIMBS + 4, 1) &&
             !mbedtls_mpi_grow(&n, FIELD_LIMBS + 1) && inverse_vector(&n, "2", half, 1, 0);
    mbedtls_mpi_free(&n);
    ESP_LOGI("HTTPS_HW", "cert_inv_selftest=%s scratch_bytes=240", tested ? "pass" : "fail");
    return tested;
}
#endif
