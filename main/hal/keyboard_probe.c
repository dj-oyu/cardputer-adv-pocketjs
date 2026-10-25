#include "keyboard_probe.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

// IDF probe uses 100 kHz independently of the later device's 400 kHz config.
// These are bounded boot policy limits, not a claimed TCA8418 POR requirement.
enum { ATTEMPTS = 3, PROBE_MS = 50, GAP_MS = 10, RETRY_WINDOW_US = 250000 };

esp_err_t keyboard_probe_startup(i2c_master_bus_handle_t bus) {
    const int64_t began = esp_timer_get_time();
    esp_err_t error = ESP_FAIL;
    unsigned attempts = 0;
    for (unsigned i = 0; i < ATTEMPTS; ++i) {
        if (i) {
            // Suppress retry when a clock check observes the window expired.
            // Preemption after the check or inside IDF is not a hard deadline.
            if (esp_timer_get_time() - began >= RETRY_WINDOW_US) break;
            TickType_t ticks = pdMS_TO_TICKS(GAP_MS);
            vTaskDelay(ticks ? ticks : 1);
            if (esp_timer_get_time() - began >= RETRY_WINDOW_US) break;
        }
        ++attempts;
        error = i2c_master_probe(bus, 0x34, PROBE_MS);
        const int64_t elapsed = esp_timer_get_time() - began;
        if (error == ESP_OK) {
            ESP_LOGI("board", "KBD_PROBE ready attempts=%u elapsed_us=%" PRId64,
                     attempts, elapsed);
            return ESP_OK;
        }
        ESP_LOGW("board", "KBD_PROBE addr=0x34 attempt=%u/%u error=%s(0x%x) "
                 "elapsed_us=%" PRId64 " sda=%d scl=%d",
                 attempts, (unsigned)ATTEMPTS, esp_err_to_name(error),
                 (unsigned)error, elapsed, gpio_get_level(8), gpio_get_level(9));
        if (error != ESP_ERR_NOT_FOUND && error != ESP_ERR_TIMEOUT) break;
    }
    ESP_LOGE("board", "KBD_PROBE failed attempts=%u error=%s(0x%x) elapsed_us=%" PRId64,
             attempts, esp_err_to_name(error), (unsigned)error,
             esp_timer_get_time() - began);
    return error;
}
