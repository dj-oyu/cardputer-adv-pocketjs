#include "ksn_proc_plan.h"
#ifdef KASANE_MEGADEMO_DEVICE_PROBE
#include "proc_megademo.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "KSN_MEGA"
#define FRAME_BANDS ((KSN_PROC_H + STRIP_H - 1) / STRIP_H)

static size_t free_internal(void) {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static uint64_t hash_strip(uint64_t hash, const uint16_t *pixels, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        hash ^= pixels[i] & 255u;
        hash *= UINT64_C(1099511628211);
        hash ^= pixels[i] >> 8;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void ksn_megademo_device_probe_run(void) {
    const size_t start_free = free_internal();
    ksn_proc_plan *plans = heap_caps_calloc(PROC_MEGA_LAYERS, sizeof *plans,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ksn_proc_frame *frames[PROC_MEGA_LAYERS] = {0};
    ksn_proc_vm *vm = heap_caps_calloc(1, sizeof *vm,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    unsigned completed = 0, total_segments = 0;
    int64_t run_sum = 0, render_sum = 0, lcd_sum = 0;
    int64_t run_max = 0, frame_max = 0;
    uint64_t first_hash[3] = {0}, last_hash[3] = {0};
    const char *failure = NULL;
    bool capture_active = false;

    ESP_LOGI(TAG, "START frames=%u phases=3 layers=%u free=%u largest=%u stack_free=%u",
             PROC_MEGA_FRAMES, PROC_MEGA_LAYERS, (unsigned)start_free,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (!plans || !vm) { failure = "ALLOC"; goto done; }
    for (unsigned layer = 0; layer < PROC_MEGA_LAYERS; layer++) {
        frames[layer] = heap_caps_calloc(1, sizeof *frames[layer],
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!frames[layer]) { failure = "ALLOC_FRAME"; goto done; }
    }

    for (unsigned frame = 0; frame < PROC_MEGA_FRAMES; frame++) {
        const unsigned phase = proc_mega_phase(frame);
        const uint16_t backdrop = proc_mega_backdrop(frame);
        unsigned segments = 0;
        int64_t run_us = 0, render_us = 0, lcd_us = 0;
        uint64_t hash = UINT64_C(1469598103934665603);
        const int64_t frame_start = esp_timer_get_time();
        for (unsigned layer = 0; layer < PROC_MEGA_LAYERS; layer++) {
            float input[KSN_PROC_INPUTS];
            if ((frame % 16u) == 0) {
                ksn_proc_inst code[KSN_PROC_CODE];
                ksn_proc_program program;
                if (!proc_mega_build(frame, layer, code, &program, input) ||
                    !ksn_proc_plan_prepare(&plans[layer], &program)) {
                    failure = "REGISTER"; goto done;
                }
            } else if (!proc_mega_inputs(frame, layer, input)) {
                failure = "INPUT"; goto done;
            }
            const int64_t began = esp_timer_get_time();
            if (ksn_proc_plan_begin(vm, &plans[layer], input, frames[layer]) != KSN_PROC_RUNNING ||
                ksn_proc_plan_run(vm, &plans[layer], false) != KSN_PROC_DONE ||
                !frames[layer]->ready || !frames[layer]->count) {
                failure = "EXEC"; goto done;
            }
            run_us += esp_timer_get_time() - began;
            segments += frames[layer]->count;
        }
        if (segments < 110) { failure = "SEGMENTS"; goto done; }

        if (frame == 15u || frame == 31u || frame == 47u) {
            ESP_LOGI(TAG, "CAPTURE i=%u", frame);
            board_capture(true);
            capture_active = true;
        }
        for (int y = 0; y < KSN_PROC_H; y += STRIP_H) {
            const int rows = y + STRIP_H <= KSN_PROC_H ? STRIP_H : KSN_PROC_H - y;
            uint16_t *pixels = board_strip();
            const int64_t began = esp_timer_get_time();
            for (int i = 0; i < rows * KSN_PROC_W; i++) pixels[i] = backdrop;
            for (unsigned layer = 0; layer < PROC_MEGA_LAYERS; layer++) {
                if (!ksn_proc_render_band(frames[layer], pixels, y, rows)) {
                    failure = "RENDER"; goto done;
                }
            }
            hash = hash_strip(hash, pixels, (unsigned)(rows * KSN_PROC_W));
            render_us += esp_timer_get_time() - began;
            const int64_t sending = esp_timer_get_time();
            if (board_present_sync(y, rows, pixels) != ESP_OK) {
                failure = "LCD"; goto done;
            }
            lcd_us += esp_timer_get_time() - sending;
        }
        if (capture_active) {
            board_capture(false);
            capture_active = false;
        }
        if (frame % 16u == 0) first_hash[phase] = hash;
        else if (hash == last_hash[phase]) { failure = "STATIC_FRAME"; goto done; }
        last_hash[phase] = hash;
        completed++;
        total_segments += segments;
        run_sum += run_us; render_sum += render_us; lcd_sum += lcd_us;
        if (run_us > run_max) run_max = run_us;
        const int64_t frame_us = esp_timer_get_time() - frame_start;
        if (frame_us > frame_max) frame_max = frame_us;
        ESP_LOGI(TAG, "FRAME i=%u phase=%u segments=%u bands=%u hash=%016llx run_us=%lld render_us=%lld lcd_us=%lld frame_us=%lld free=%u stack_free=%u",
                 frame, phase, segments, FRAME_BANDS, (unsigned long long)hash,
                 (long long)run_us, (long long)render_us, (long long)lcd_us,
                 (long long)frame_us, (unsigned)free_internal(),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(pdMS_TO_TICKS(33));
    }
    for (unsigned phase = 0; phase < 3; phase++) {
        if (first_hash[phase] == last_hash[phase]) { failure = "PHASE_STATIC"; goto done; }
    }
    if (first_hash[0] == first_hash[1] || first_hash[1] == first_hash[2]) {
        failure = "PHASE_DUPLICATE"; goto done;
    }
    if (total_segments != 5680u) { failure = "SEGMENT_TOTAL"; goto done; }
done:
    if (capture_active) board_capture(false);
    if (failure) {
        ESP_LOGE(TAG, "FAIL reason=%s completed=%u segments=%u free=%u largest=%u stack_free=%u",
                 failure, completed, total_segments, (unsigned)free_internal(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    } else {
        ESP_LOGI(TAG, "PASS frames=%u segments=%u run_mean_us=%lld run_max_us=%lld render_mean_us=%lld lcd_mean_us=%lld frame_max_us=%lld phase_hashes=%016llx,%016llx,%016llx free=%u min_free=%u stack_free=%u",
                 completed, total_segments, (long long)(run_sum / completed),
                 (long long)run_max, (long long)(render_sum / completed),
                 (long long)(lcd_sum / completed), (long long)frame_max,
                 (unsigned long long)last_hash[0], (unsigned long long)last_hash[1],
                 (unsigned long long)last_hash[2], (unsigned)free_internal(),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    heap_caps_free(vm);
    for (unsigned layer = 0; layer < PROC_MEGA_LAYERS; layer++) heap_caps_free(frames[layer]);
    heap_caps_free(plans);
    ESP_LOGI(TAG, "END free=%u", (unsigned)free_internal());
}
#endif
