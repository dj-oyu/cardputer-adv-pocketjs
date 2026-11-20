/* ESP32-S3 DMA-SHA diagnostic. IDF owns the descriptor, GDMA channel and the
 * shared SHA/AES lock. Only the completion wait of our one transfer is wrapped. */
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sha/sha_core.h"
#include "soc/hwcrypto_reg.h"
#include "soc/interrupts.h"
#include "soc/soc.h"

enum { PADDED_BYTES = 3968, MESSAGE_BYTES = PADDED_BYTES - 64,
       BATCHES = 500, PER_BATCH = 8, PERIOD_MS = 5 };
static const uint8_t expected[32] = {
    0xe4,0x73,0x0b,0x22,0xfd,0xae,0xe1,0x8f,0xd7,0x0b,0x12,0xb9,0x38,0x05,0x61,0x86,
    0x14,0x28,0x04,0x2a,0xaa,0xaa,0xf7,0x93,0x50,0x88,0x61,0x15,0xe5,0xb5,0xc9,0x31
};

static _Atomic(TaskHandle_t) transfer_owner;
static StaticSemaphore_t done_storage;
static SemaphoreHandle_t done;
static bool use_irq;
static unsigned waits, timeouts;
static int64_t wait_us;

static void sha_done(void *arg)
{
    REG_WRITE(SHA_CLEAR_IRQ_REG, 1);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &wake);
    if (wake) portYIELD_FROM_ISR();
}

void __real_sha_hal_wait_idle(void);
void __wrap_sha_hal_wait_idle(void)
{
    TaskHandle_t owner = atomic_load(&transfer_owner);
    if (!owner || xTaskGetCurrentTaskHandle() != owner) {
        __real_sha_hal_wait_idle();
        return;
    }
    int64_t start = esp_timer_get_time();
    ++waits;
    if (use_irq && !xSemaphoreTake(done, pdMS_TO_TICKS(20))) ++timeouts;
    /* Finish stock busy check even on a missed IRQ, before releasing the
     * input loan, digest registers, or shared accelerator lock. */
    __real_sha_hal_wait_idle();
    wait_us += esp_timer_get_time() - start;
}

static uint32_t runtime(TaskHandle_t task)
{
    TaskStatus_t status;
    vTaskGetInfo(task, &status, pdFALSE, eInvalid);
    return status.ulRunTimeCounter;
}

static esp_err_t hash_once(uint8_t *input, TaskHandle_t self)
{
    uint32_t digest[8];
    esp_sha_acquire_hardware();
    esp_sha_set_mode(SHA2_256);
    if (use_irq) {
        REG_WRITE(SHA_CLEAR_IRQ_REG, 1);
        REG_WRITE(SHA_INT_ENA_REG, 1);
    }
    atomic_store(&transfer_owner, self);
    int result = esp_sha_dma(SHA2_256, input, PADDED_BYTES, NULL, 0, true);
    atomic_store(&transfer_owner, NULL);
    if (use_irq) {
        REG_WRITE(SHA_INT_ENA_REG, 0);
        REG_WRITE(SHA_CLEAR_IRQ_REG, 1);
    }
    if (result == 0) esp_sha_read_digest_state(SHA2_256, digest);
    esp_sha_release_hardware();
    return result == 0 && !timeouts && !memcmp(digest, expected, sizeof digest) ? ESP_OK : ESP_FAIL;
}

esp_err_t ayatsuri_sha_dma_window(bool irq, const atomic_bool *cancel)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCore(0), idle1 = xTaskGetIdleTaskHandleForCore(1);
    uint8_t *input = heap_caps_calloc(1, PADDED_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!input) return ESP_ERR_NO_MEM;
    /* A pre-padded SHA-256 known-answer vector for 3904 zero bytes. */
    input[MESSAGE_BYTES] = 0x80;
    input[PADDED_BYTES - 2] = (MESSAGE_BYTES * 8u) >> 8;
    input[PADDED_BYTES - 1] = (MESSAGE_BYTES * 8u) & 255;
    use_irq = irq;
    waits = timeouts = 0;
    wait_us = 0;
    intr_handle_t handle = NULL;
    esp_err_t err = ESP_OK;
    if (irq) {
        done = xSemaphoreCreateBinaryStatic(&done_storage);
        err = esp_intr_alloc(ETS_SHA_INTR_SOURCE, ESP_INTR_FLAG_LOWMED, sha_done, done, &handle);
        if (err == ESP_OK) ESP_LOGI("SHA_IRQ", "core=%d vector=%d",
                                   esp_intr_get_cpu(handle), esp_intr_get_intno(handle));
    }
    uint32_t worker = runtime(self), core0 = runtime(idle0), core1 = runtime(idle1);
    int64_t start = esp_timer_get_time();
    unsigned hashes = 0;
    TickType_t release = xTaskGetTickCount();
    for (unsigned i = 0; err == ESP_OK && i < BATCHES; ++i) {
        if (atomic_load(cancel)) { err = ESP_ERR_INVALID_STATE; break; }
        for (unsigned j = 0; j < PER_BATCH; ++j) {
            err = hash_once(input, self);
            if (err != ESP_OK) break;
            ++hashes;
        }
        vTaskDelayUntil(&release, pdMS_TO_TICKS(PERIOD_MS));
    }
    /* Switch out once to charge the final CPU slice to this worker. */
    vTaskDelay(1);
    int64_t elapsed = esp_timer_get_time() - start;
    uint32_t worker_delta = runtime(self) - worker;
    uint32_t idle0_delta = runtime(idle0) - core0, idle1_delta = runtime(idle1) - core1;
    if (waits != hashes && err == ESP_OK) err = ESP_FAIL;
    if (handle) esp_intr_free(handle);
    if (irq) { vSemaphoreDelete(done); done = NULL; }
    free(input);
    ESP_LOGI("SHA_DMA","mode=%s status=%s hashes=%u message_bytes=%u dma_bytes=%u elapsed_us=%" PRId64
             " worker_ticks=%" PRIu32 " idle0_ticks=%" PRIu32 " idle1_ticks=%" PRIu32
             " waits=%u wait_wall_us=%" PRId64 " irq_timeouts=%u",
             irq ? "irq":"stock", esp_err_to_name(err), hashes, hashes*MESSAGE_BYTES,
             hashes*PADDED_BYTES, elapsed, worker_delta,idle0_delta,idle1_delta,waits,wait_us,timeouts);
    return err;
}
