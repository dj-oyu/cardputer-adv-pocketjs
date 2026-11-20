#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "aes/esp_aes.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sha/sha_core.h"

enum { SHA_BYTES = 3968, SHA_MESSAGE = SHA_BYTES - 64, AES_BYTES = 2048,
       BATCHES = 320, TRANSFERS = 12, PERIOD_MS = 11 };

static const uint8_t aes_expected[] = {
    0x66,0xe9,0x4b,0xd4,0xef,0x8a,0x2c,0x3b,0x88,0x4c,0xfa,0x59,0xca,0x34,0x2b,0x2e,
};
static const uint8_t sha_expected[] = {
    0xe4,0x73,0x0b,0x22,0xfd,0xae,0xe1,0x8f,0xd7,0x0b,0x12,0xb9,0x38,0x05,0x61,0x86,
    0x14,0x28,0x04,0x2a,0xaa,0xaa,0xf7,0x93,0x50,0x88,0x61,0x15,0xe5,0xb5,0xc9,0x31,
};

static bool aes_once(esp_aes_context *ctx, uint8_t *input)
{
    uint8_t iv[16] = {0};
    uint8_t *output = input + AES_BYTES;
    return !esp_aes_crypt_cbc(ctx, ESP_AES_ENCRYPT, AES_BYTES, iv, input, output) &&
           !memcmp(output, aes_expected, sizeof aes_expected);
}

static bool sha_once(uint8_t *input)
{
    uint32_t digest[8];
    esp_sha_acquire_hardware();
    esp_sha_set_mode(SHA2_256);
    int ret = esp_sha_dma(SHA2_256, input, SHA_BYTES, NULL, 0, true);
    if (ret == 0) esp_sha_read_digest_state(SHA2_256, digest);
    esp_sha_release_hardware();
    return ret == 0 && !memcmp(digest, sha_expected, sizeof sha_expected);
}

esp_err_t ayatsuri_lcd_gdma_load(bool aes, const atomic_bool *cancel)
{
    const unsigned bytes = aes ? AES_BYTES : SHA_BYTES;
    uint8_t *input = heap_caps_calloc(1, aes ? 2 * AES_BYTES : SHA_BYTES,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!input) return ESP_ERR_NO_MEM;
    esp_aes_context ctx;
    esp_err_t err = ESP_OK;
    if (aes) {
        const uint8_t key[16] = {0};
        esp_aes_init(&ctx);
        if (esp_aes_setkey(&ctx, key, 128)) err = ESP_FAIL;
    } else {
        /* SHA-256 padding for a public vector of 3904 zero bytes. */
        input[SHA_MESSAGE] = 0x80;
        input[SHA_BYTES - 2] = (SHA_MESSAGE * 8u) >> 8;
        input[SHA_BYTES - 1] = (SHA_MESSAGE * 8u) & 255;
    }
    unsigned transfers = 0;
    TickType_t release = xTaskGetTickCount();
    for (unsigned i = 0; err == ESP_OK && i < BATCHES; ++i) {
        if (atomic_load(cancel)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        for (unsigned j = 0; j < TRANSFERS; ++j) {
            if (!(aes ? aes_once(&ctx, input) : sha_once(input))) {
                err = ESP_FAIL;
                break;
            }
            ++transfers;
        }
        vTaskDelayUntil(&release, pdMS_TO_TICKS(PERIOD_MS));
    }
    if (aes) esp_aes_free(&ctx);
    free(input);
    ESP_LOGI("GDMA_LOAD", "engine=%s status=%s transfers=%u bytes=%u",
             aes ? "aes" : "sha", esp_err_to_name(err), transfers, transfers * bytes);
    return err;
}
