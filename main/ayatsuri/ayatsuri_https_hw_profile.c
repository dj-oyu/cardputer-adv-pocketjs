#include "ayatsuri_https_hw_profile.h"
#include "sdkconfig.h"
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
#include "hal/lcd_spi.h"
#endif
#ifdef CONFIG_AYATSURI_P384_PROFILE
#include "ayatsuri_p384_profile.h"
#endif
#ifdef CONFIG_AYATSURI_CERT_HEAP_TRACE
#include "ayatsuri_cert_heap_trace.h"
#endif
#ifdef CONFIG_AYATSURI_CERT_INV_SCRATCH
#include "ayatsuri_cert_inv_scratch.h"
static bool cert_inv_scratch_enabled;

void ayatsuri_https_hw_use_cert_inv_scratch(bool enabled)
{
    cert_inv_scratch_enabled = enabled;
}
#endif

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_crypto_shared_gdma.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/mpi_types.h"
#include "hal/sha_types.h"
#include "mbedtls/bignum.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

enum tls_stage { CERT_VERIFY, SERVER_SIG, KEYGEN, EXPORT, AGREEMENT, STAGE_COUNT };
static const char *const stage_names[] = {"cert_verify", "server_sig", "keygen", "export", "agreement"};
static const uint32_t heap_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
typedef struct {
    int64_t wall;
    uint32_t cpu;
    unsigned mpi[3];
    unsigned heap_before, heap_after;
} stage_sample;

/* Counters have a single writer. Other tasks take only the owner check.
 * Profile the S3 hardware start points rather than byte streams or keys. */
static _Atomic(TaskHandle_t) owner;
static _Atomic(TaskHandle_t) short_mul_owner;
static bool short_mul_tested, short_mul_enabled;
static struct {
    unsigned block[SHA_TYPE_MAX], dma[SHA_TYPE_MAX], hist[SHA_TYPE_MAX][5], sha_bytes[SHA_TYPE_MAX];
    unsigned aes_block, aes_dma, aes_dma_blocks, mpi[3], hmac, ds;
    unsigned gdma_sha, gdma_aes, gdma_error, locks, handshake_calls;
    esp_sha_type mode;
    const char *version, *suite;
    int64_t handshake_us, acquired_at, lock_wait_us, lock_hold_us;
    unsigned matched_releases;
    unsigned handshake_heap_before, handshake_heap_after;
    struct {
        stage_sample sum;
        unsigned calls;
    } stage[STAGE_COUNT];
    bool in_handshake, in_stage, have_ecdh_key;
    mbedtls_svc_key_id_t ecdh_key;
    unsigned ecc_family, ecc_bits;
    unsigned software_mul;
#ifdef CONFIG_AYATSURI_CERT_INV_SCRATCH
    unsigned cert_inv_scratch_calls;
#endif
} counts;

static inline bool collecting(void)
{
    TaskHandle_t task = atomic_load_explicit(&owner, memory_order_relaxed);
    return task && task == xTaskGetCurrentTaskHandle();
}

static uint32_t current_cpu(void)
{
#if configGENERATE_RUN_TIME_STATS
    TaskStatus_t status;
    vTaskGetInfo(NULL, &status, pdFALSE, eInvalid);
    return status.ulRunTimeCounter;
#else
    return 0;
#endif
}

/* Attribute nested calls to their outer stage (including X.509's own PK
 * verification). Nothing is added to the per-multiplication hot path. */
static bool stage_begin(stage_sample *before)
{
    if (!collecting() || !counts.in_handshake || counts.in_stage) return false;
    counts.in_stage = true;
    memcpy(before->mpi, counts.mpi, sizeof before->mpi);
    before->heap_before = heap_caps_get_minimum_free_size(heap_caps);
    before->cpu = current_cpu();
    before->wall = esp_timer_get_time();
    return true;
}

static void stage_end(enum tls_stage stage, const stage_sample *before)
{
    stage_sample *sum = &counts.stage[stage].sum;
    sum->wall += esp_timer_get_time() - before->wall;
    sum->cpu += current_cpu() - before->cpu;
    for (unsigned i = 0; i < 3; ++i) sum->mpi[i] += counts.mpi[i] - before->mpi[i];
    if (!counts.stage[stage].calls) sum->heap_before = before->heap_before;
    sum->heap_after = heap_caps_get_minimum_free_size(heap_caps);
    ++counts.stage[stage].calls;
    counts.in_stage = false;
}

void ayatsuri_https_hw_begin(void)
{
    memset(&counts, 0, sizeof counts);
    counts.mode = SHA_TYPE_MAX;
    atomic_store(&owner, xTaskGetCurrentTaskHandle());
}

void ayatsuri_https_hw_end(const char *phase)
{
    atomic_store(&owner, NULL);
#ifdef CONFIG_AYATSURI_P384_PROFILE
    ay_p384_report(phase);
#endif
#ifdef CONFIG_AYATSURI_CERT_HEAP_TRACE
    if (counts.stage[CERT_VERIFY].calls) ay_cert_heap_trace_report(phase);
#endif
    ESP_LOGI("HTTPS_HW", "phase=%s cert_short_mul_enabled=%u software_mul=%u",
             phase, (unsigned)short_mul_enabled, counts.software_mul);
    ESP_LOGI("HTTPS_HW", "phase=%s tls=%s suite=%s handshake_calls=%u handshake_wall_us=%" PRId64
             " aes_block=%u aes_dma=%u aes_dma_blocks=%u mpi_mul=%u mpi_modmul=%u mpi_modexp=%u"
             " hmac_start=%u ds_sign=%u gdma_sha=%u gdma_aes=%u gdma_err=%u",
             phase, counts.version ? counts.version : "unknown", counts.suite ? counts.suite : "unknown",
             counts.handshake_calls, counts.handshake_us, counts.aes_block, counts.aes_dma,
             counts.aes_dma_blocks, counts.mpi[MPI_MULT], counts.mpi[MPI_MODMULT], counts.mpi[MPI_MODEXP],
             counts.hmac, counts.ds, counts.gdma_sha, counts.gdma_aes, counts.gdma_error);
    ESP_LOGI("HTTPS_HW", "phase=%s sha_aes_locks=%u matched_releases=%u lock_wait_wall_us=%" PRId64
             " lock_hold_wall_us=%" PRId64, phase, counts.locks, counts.matched_releases,
             counts.lock_wait_us, counts.lock_hold_us);
    for (unsigned i = 0; i < SHA_TYPE_MAX; ++i) {
        if (counts.block[i] || counts.dma[i])
            ESP_LOGI("HTTPS_HW", "phase=%s sha_mode=%u block_calls=%u dma_chunks=%u dma_bytes=%u"
                     " le512=%u b513_1024=%u b1025_3967=%u eq3968=%u gt3968=%u",
                     phase, i, counts.block[i], counts.dma[i], counts.sha_bytes[i],
                     counts.hist[i][0], counts.hist[i][1], counts.hist[i][2], counts.hist[i][3], counts.hist[i][4]);
    }
    if (counts.handshake_calls) {
#ifdef CONFIG_AYATSURI_CERT_INV_SCRATCH
        ESP_LOGI("HTTPS_HW", "phase=%s cert_inv_scratch=%u scratch_calls=%u",
                 phase, (unsigned)cert_inv_scratch_enabled, counts.cert_inv_scratch_calls);
#endif
        ESP_LOGI("HTTPS_HW", "phase=%s tls_heap=handshake sdk_in=%u sdk_out=%u",
                 phase, counts.handshake_heap_before, counts.handshake_heap_after);
        ESP_LOGI("HTTPS_HW", "phase=%s ecdh_psa_family=%u ecdh_bits=%u stage_cpu_enabled=%u",
                 phase, counts.ecc_family, counts.ecc_bits, (unsigned)configGENERATE_RUN_TIME_STATS);
        for (unsigned i = 0; i < STAGE_COUNT; ++i) {
            const stage_sample *s = &counts.stage[i].sum;
            ESP_LOGI("HTTPS_HW", "phase=%s tls_stage=%s calls=%u wall_us=%" PRId64
                     " cpu_ticks=%" PRIu32 " mul=%u modmul=%u modexp=%u",
                     phase, stage_names[i], counts.stage[i].calls, s->wall, s->cpu,
                     s->mpi[MPI_MULT], s->mpi[MPI_MODMULT], s->mpi[MPI_MODEXP]);
            if (counts.stage[i].calls)
                ESP_LOGI("HTTPS_HW", "phase=%s tls_heap=%s sdk_in=%u sdk_out=%u",
                         phase, stage_names[i], s->heap_before, s->heap_after);
        }
    }
}

void __real_sha_hal_set_mode(esp_sha_type mode);
void __wrap_sha_hal_set_mode(esp_sha_type mode)
{
    if (collecting()) counts.mode = mode;
    __real_sha_hal_set_mode(mode);
}

void __real_sha_hal_hash_block(esp_sha_type mode, const void *data, size_t words, bool first);
void __wrap_sha_hal_hash_block(esp_sha_type mode, const void *data, size_t words, bool first)
{
    if (collecting() && (unsigned)mode < SHA_TYPE_MAX) ++counts.block[mode];
    __real_sha_hal_hash_block(mode, data, words, first);
}

void __real_sha_hal_hash_dma(size_t blocks, bool first);
void __wrap_sha_hal_hash_dma(size_t blocks, bool first)
{
    if (collecting() && (unsigned)counts.mode < SHA_TYPE_MAX) {
        unsigned bytes = blocks * (counts.mode >= SHA2_384 ? 128u : 64u);
        ++counts.dma[counts.mode];
        counts.sha_bytes[counts.mode] += bytes;
        unsigned bucket = bytes <= 512 ? 0 : bytes <= 1024 ? 1 : bytes < 3968 ? 2 : bytes == 3968 ? 3 : 4;
        ++counts.hist[counts.mode][bucket];
    }
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
    const bool lcd_busy = lcd_spi_gdma_probe_lcd_busy();
#endif
    __real_sha_hal_hash_dma(blocks, first);
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
    if (lcd_busy) lcd_spi_gdma_probe_sample(LCD_GDMA_SHA);
#endif
}

void __real_aes_hal_transform_block(const void *input, void *output);
void __wrap_aes_hal_transform_block(const void *input, void *output)
{
    if (collecting()) ++counts.aes_block;
    __real_aes_hal_transform_block(input, output);
}

void __real_aes_hal_transform_dma_start(size_t blocks);
void __wrap_aes_hal_transform_dma_start(size_t blocks)
{
    if (collecting()) {
        ++counts.aes_dma;
        counts.aes_dma_blocks += blocks;
    }
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
    const bool lcd_busy = lcd_spi_gdma_probe_lcd_busy();
#endif
    __real_aes_hal_transform_dma_start(blocks);
#ifdef CONFIG_AYATSURI_LCD_GDMA_PRIORITY
    if (lcd_busy) lcd_spi_gdma_probe_sample(LCD_GDMA_AES);
#endif
}

void __real_mpi_hal_start_op(mpi_op_t op);
void __wrap_mpi_hal_start_op(mpi_op_t op)
{
    if (collecting() && (unsigned)op <= MPI_MODEXP) ++counts.mpi[op];
    __real_mpi_hal_start_op(op);
#ifdef CONFIG_AYATSURI_P384_MODEXP
    if (op == MPI_MODEXP) ay_p384_note_modexp_start();
#endif
}

void __real_hmac_hal_start(void);
void __wrap_hmac_hal_start(void)
{
    if (collecting()) ++counts.hmac;
    __real_hmac_hal_start();
}

void __real_ds_hal_start_sign(void);
void __wrap_ds_hal_start_sign(void)
{
    if (collecting()) ++counts.ds;
    __real_ds_hal_start_sign();
}

esp_err_t __real_esp_crypto_shared_gdma_start_axi_ahb(const crypto_dma_desc_t *input,
                                                    const crypto_dma_desc_t *output, crypto_dma_user_t user);
esp_err_t __wrap_esp_crypto_shared_gdma_start_axi_ahb(const crypto_dma_desc_t *input,
                                                    const crypto_dma_desc_t *output, crypto_dma_user_t user)
{
    esp_err_t result = __real_esp_crypto_shared_gdma_start_axi_ahb(input, output, user);
    if (collecting()) {
        if (result != ESP_OK) ++counts.gdma_error;
        else if (user == CRYPTO_DMA_USER_SHA) ++counts.gdma_sha;
        else if (user == CRYPTO_DMA_USER_AES) ++counts.gdma_aes;
    }
    return result;
}

void __real_esp_crypto_sha_aes_lock_acquire(void);
void __wrap_esp_crypto_sha_aes_lock_acquire(void)
{
    if (!collecting()) {
        __real_esp_crypto_sha_aes_lock_acquire();
        return;
    }
    int64_t before = esp_timer_get_time();
    __real_esp_crypto_sha_aes_lock_acquire();
    counts.acquired_at = esp_timer_get_time();
    counts.lock_wait_us += counts.acquired_at - before;
    ++counts.locks;
}

void __real_esp_crypto_sha_aes_lock_release(void);
void __wrap_esp_crypto_sha_aes_lock_release(void)
{
    if (collecting() && counts.acquired_at) {
        counts.lock_hold_us += esp_timer_get_time() - counts.acquired_at;
        ++counts.matched_releases;
        counts.acquired_at = 0;
    }
    __real_esp_crypto_sha_aes_lock_release();
}

int __real_mbedtls_ssl_handshake(mbedtls_ssl_context *ssl);
int __wrap_mbedtls_ssl_handshake(mbedtls_ssl_context *ssl)
{
    if (!collecting()) return __real_mbedtls_ssl_handshake(ssl);
    if (!counts.handshake_calls)
        counts.handshake_heap_before = heap_caps_get_minimum_free_size(heap_caps);
    int64_t start = esp_timer_get_time();
    counts.in_handshake = true;
    int result = __real_mbedtls_ssl_handshake(ssl);
    counts.in_handshake = false;
    counts.handshake_us += esp_timer_get_time() - start;
    counts.handshake_heap_after = heap_caps_get_minimum_free_size(heap_caps);
    ++counts.handshake_calls;
    if (!result) {
        /* Both getters return library constants that survive client cleanup. */
        counts.version = mbedtls_ssl_get_version(ssl);
        counts.suite = mbedtls_ssl_get_ciphersuite(ssl);
    }
    return result;
}

extern __typeof__(mbedtls_x509_crt_verify_restartable) __real_mbedtls_x509_crt_verify_restartable;
int __wrap_mbedtls_x509_crt_verify_restartable(mbedtls_x509_crt *crt, mbedtls_x509_crt *trust,
    mbedtls_x509_crl *crl, const mbedtls_x509_crt_profile *profile, const char *cn, uint32_t *flags,
    int (*verify)(void *, mbedtls_x509_crt *, int, uint32_t *), void *context, mbedtls_x509_crt_restart_ctx *rs)
{
    stage_sample before;
    bool active = stage_begin(&before);
#ifdef CONFIG_AYATSURI_P384_PROFILE
    if (active) ay_p384_scope_begin(ay_p384_detail());
#endif
    if (active && short_mul_enabled) atomic_store(&short_mul_owner, xTaskGetCurrentTaskHandle());
#ifdef CONFIG_AYATSURI_CERT_HEAP_TRACE
    if (active) ay_cert_heap_trace_begin();
#endif
#ifdef CONFIG_AYATSURI_CERT_INV_SCRATCH
    if (active) ay_cert_inv_scratch_begin(cert_inv_scratch_enabled);
#endif
    int status = __real_mbedtls_x509_crt_verify_restartable(crt, trust, crl, profile, cn, flags, verify, context, rs);
#ifdef CONFIG_AYATSURI_P384_PROFILE
    if (active) ay_p384_scope_end();
#endif
#ifdef CONFIG_AYATSURI_CERT_INV_SCRATCH
    if (active) counts.cert_inv_scratch_calls += ay_cert_inv_scratch_end();
#endif
#ifdef CONFIG_AYATSURI_CERT_HEAP_TRACE
    if (active) ay_cert_heap_trace_end();
#endif
    if (active && short_mul_enabled) atomic_store(&short_mul_owner, NULL);
    if (active) stage_end(CERT_VERIFY, &before);
    return status;
}

/* TF-PSA builtin bignum_core.h. No private operands are inspected outside
 * this optional public-certificate verification path. The S3 IDF multiply
 * grows to ceil((bitlen(X)+bitlen(Y))/32), sometimes one word shorter than
 * the upstream core kernel's mandatory output. A stack product handles
 * that case and every combination of aliased inputs/output. */
enum { SHORT_WORDS = 12 };
_Static_assert(sizeof(mbedtls_mpi_uint) == 4, "requires ESP32-S3 32-bit limbs");
extern void mbedtls_mpi_core_mul(mbedtls_mpi_uint *, const mbedtls_mpi_uint *, size_t,
                                 const mbedtls_mpi_uint *, size_t);
extern __typeof__(mbedtls_mpi_mul_mpi) __real_mbedtls_mpi_mul_mpi;

static int short_mul(mbedtls_mpi *z, const mbedtls_mpi *x, const mbedtls_mpi *y, unsigned *used)
{
    size_t xb = mbedtls_mpi_bitlen(x), yb = mbedtls_mpi_bitlen(y);
    if (xb < 2 || yb < 2 || xb > SHORT_WORDS * 32 || yb > SHORT_WORDS * 32)
        return __real_mbedtls_mpi_mul_mpi(z, x, y);
    size_t words = (xb + yb + 31) / 32;
    int ret = mbedtls_mpi_grow(z, words);
    if (ret) return ret;
    mbedtls_mpi_uint product[2 * SHORT_WORDS];
    int sign = x->MBEDTLS_PRIVATE(s) * y->MBEDTLS_PRIVATE(s);
    size_t xn = (xb + 31) / 32, yn = (yb + 31) / 32;
    mbedtls_mpi_core_mul(product, x->MBEDTLS_PRIVATE(p), xn, y->MBEDTLS_PRIVATE(p), yn);
    memcpy(z->MBEDTLS_PRIVATE(p), product, words * sizeof *product);
    memset(z->MBEDTLS_PRIVATE(p) + words, 0, (z->MBEDTLS_PRIVATE(n) - words) * sizeof *product);
    z->MBEDTLS_PRIVATE(s) = sign;
    mbedtls_platform_zeroize(product, (xn + yn) * sizeof *product);
    ++*used;
    return 0;
}

int __wrap_mbedtls_mpi_mul_mpi(mbedtls_mpi *z, const mbedtls_mpi *x, const mbedtls_mpi *y)
{
#ifdef CONFIG_AYATSURI_P384_PROFILE
    int64_t start = ay_p384_field_begin();
#endif
    TaskHandle_t task = atomic_load_explicit(&short_mul_owner, memory_order_relaxed);
    int result = task && task == xTaskGetCurrentTaskHandle() ? short_mul(z, x, y, &counts.software_mul) :
                 __real_mbedtls_mpi_mul_mpi(z, x, y);
#ifdef CONFIG_AYATSURI_P384_PROFILE
    if (start >= 0) ay_p384_field_end(x == y ? AY_P384_SQR : AY_P384_MUL, start);
#endif
    return result;
}

void ayatsuri_https_hw_set_short_mul(bool enabled)
{
    short_mul_enabled = enabled && short_mul_tested;
}

/* Compare independently constructed outputs including spare limbs and
 * resulting allocation size. Inputs are synthetic, never TLS objects. */
static bool short_mul_case(unsigned bits, int small, unsigned alias)
{
    mbedtls_mpi expected;
    mbedtls_mpi_init(&expected);
    bool ok = true;
    size_t capacity = 0;
    for (unsigned variant = 0; ok && variant < 2; ++variant) {
        mbedtls_mpi x, y, result;
        mbedtls_mpi_init(&x);
        mbedtls_mpi_init(&y);
        mbedtls_mpi_init(&result);
        mbedtls_mpi *out = alias == 1 || alias == 3 ? &x : alias == 2 ? &y : &result;
        const mbedtls_mpi *second = alias == 3 ? &x : &y;
        unsigned used = 0;
        ok = !mbedtls_mpi_lset(&x, 1) && !mbedtls_mpi_shift_l(&x, bits) &&
             !mbedtls_mpi_sub_int(&x, &x, 1) && !mbedtls_mpi_lset(&y, small);
        if (ok && alias == 0) ok = !mbedtls_mpi_set_bit(out, 800, 1);
        if (ok) {
            int ret = variant ? short_mul(out, &x, second, &used) : __real_mbedtls_mpi_mul_mpi(out, &x, second);
            ok = !ret;
            /* -(2^33 - 1)*3 = -0x5fffffffd, independent of the other backend. */
            if (ok && bits == 33 && small == -3 && alias == 0)
                ok = out->MBEDTLS_PRIVATE(s) == -1 && out->MBEDTLS_PRIVATE(n) >= 2 &&
                     out->MBEDTLS_PRIVATE(p)[0] == UINT32_C(0xfffffffd) && out->MBEDTLS_PRIVATE(p)[1] == 5;
        }
        if (ok && !variant) {
            capacity = out->MBEDTLS_PRIVATE(n);
            ok = !mbedtls_mpi_copy(&expected, out);
        } else if (ok) {
            ok = capacity == out->MBEDTLS_PRIVATE(n) && !mbedtls_mpi_cmp_mpi(&expected, out) &&
                 used == (unsigned)(bits <= 32 * SHORT_WORDS && (alias == 3 || small < -1 || small > 1));
        }
        mbedtls_mpi_free(&result);
        mbedtls_mpi_free(&y);
        mbedtls_mpi_free(&x);
    }
    mbedtls_mpi_free(&expected);
    return ok;
}

bool ayatsuri_https_hw_short_mul_selftest(void)
{
    short_mul_tested = short_mul_case(33, -3, 0) && short_mul_case(32, -3, 1) &&
                       short_mul_case(256, -7, 2) && short_mul_case(384, 0, 3) &&
                       short_mul_case(385, 3, 0) && short_mul_case(31, 0, 0) &&
                       short_mul_case(31, -1, 0);
    if (!short_mul_tested) short_mul_enabled = false;
    ESP_LOGI("HTTPS_HW", "cert_short_mul_selftest=%s max_operand_bits=%u",
             short_mul_tested ? "pass" : "fail", 32u * SHORT_WORDS);
    return short_mul_tested;
}

extern __typeof__(mbedtls_pk_verify_restartable) __real_mbedtls_pk_verify_restartable;
int __wrap_mbedtls_pk_verify_restartable(mbedtls_pk_context *ctx, mbedtls_md_type_t md,
    const unsigned char *hash, size_t hashlen, const unsigned char *sig, size_t siglen, mbedtls_pk_restart_ctx *rs)
{
    stage_sample before;
    bool active = stage_begin(&before);
    int status = __real_mbedtls_pk_verify_restartable(ctx, md, hash, hashlen, sig, siglen, rs);
    if (active) stage_end(SERVER_SIG, &before);
    return status;
}

extern __typeof__(psa_generate_key) __real_psa_generate_key;
psa_status_t __wrap_psa_generate_key(const psa_key_attributes_t *attributes, mbedtls_svc_key_id_t *key)
{
    stage_sample before;
    bool active = collecting() && attributes && psa_get_key_algorithm(attributes) == PSA_ALG_ECDH &&
                  stage_begin(&before);
    psa_status_t status = __real_psa_generate_key(attributes, key);
    if (active) {
        stage_end(KEYGEN, &before);
        counts.have_ecdh_key = status == PSA_SUCCESS;
        if (counts.have_ecdh_key) {
            counts.ecdh_key = *key;
            counts.ecc_family = PSA_KEY_TYPE_ECC_GET_FAMILY(psa_get_key_type(attributes));
            counts.ecc_bits = psa_get_key_bits(attributes);
        }
    }
    return status;
}

static bool handshake_key(mbedtls_svc_key_id_t key)
{
    return collecting() && counts.have_ecdh_key && mbedtls_svc_key_id_equal(key, counts.ecdh_key);
}

extern __typeof__(psa_export_public_key) __real_psa_export_public_key;
psa_status_t __wrap_psa_export_public_key(mbedtls_svc_key_id_t key, uint8_t *out, size_t capacity, size_t *len)
{
    stage_sample before;
    bool active = handshake_key(key) && stage_begin(&before);
    psa_status_t status = __real_psa_export_public_key(key, out, capacity, len);
    if (active) stage_end(EXPORT, &before);
    return status;
}

extern __typeof__(psa_raw_key_agreement) __real_psa_raw_key_agreement;
psa_status_t __wrap_psa_raw_key_agreement(psa_algorithm_t alg, mbedtls_svc_key_id_t key,
    const uint8_t *peer, size_t peer_len, uint8_t *out, size_t capacity, size_t *len)
{
    stage_sample before;
    bool active = alg == PSA_ALG_ECDH && handshake_key(key) && stage_begin(&before);
    psa_status_t status = __real_psa_raw_key_agreement(alg, key, peer, peer_len, out, capacity, len);
    if (active) stage_end(AGREEMENT, &before);
    return status;
}
