// MEASUREMENT HARNESS -- worktree only, never committed, never part of pocket.audio.
//
// Answers the one number the host could not: what an Opus frame costs to decode
// on this part. Everything else about Opus was measurable without a board (state
// size from the target compiler, bytes/second from the encoder); CPU was not.
//
// Runs on a task pinned to core 0 at priority 7 -- the same core and priority as
// sound.c's "sfx" task -- rather than inside sound.c itself, because sound.c is
// held by another agent and because the harness has to CHOOSE the stack size in
// order to report what the decoder actually needs.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "opus.h"
#include "opus_test_data.h"

// The host measured 15,968 bytes of decode-only peak stack by painting. The real
// audio task has 4,096. Giving the decoder 24 KiB here is deliberate: a harness
// that crashed at 4,096 would report nothing, and the high-water mark below is
// what says whether 4,096 could ever have worked.
#define PROBE_STACK 24576
#define PROBE_PASSES 20          // 20 x 50 frames = 1000 decodes = 20 s of audio

static void opus_probe_task(void *arg) {
    (void)arg;
    int sz = opus_decoder_get_size(1);
    unsigned before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int err = 0;
    OpusDecoder *dec = opus_decoder_create(24000, 1, &err);
    unsigned after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!dec || err != OPUS_OK) {
        ESP_LOGE("opusprobe", "OPUSPROBE create failed err=%d size=%d", err, sz);
        vTaskDelete(NULL);
        return;
    }
    // get_size is the number the host derived as 18,340 from the target structs.
    // Printing it next to the real heap delta is how that derivation gets checked
    // against the part instead of against my arithmetic.
    ESP_LOGI("opusprobe", "OPUSPROBE get_size=%d heap_used=%u", sz, before - after);

    static int16_t pcm[OPUS_FRAME_SAMPLES];
    int64_t total_us = 0, worst_us = 0;
    long frames = 0, bad = 0;

    for (int pass = 0; pass < PROBE_PASSES; pass++) {
        opus_decoder_ctl(dec, OPUS_RESET_STATE);
        for (int f = 0; f < OPUS_NF; f++) {
            const uint8_t *pkt = opus_data + opus_off[f];
            int len = opus_off[f + 1] - opus_off[f];
            int64_t t0 = esp_timer_get_time();
            int n = opus_decode(dec, pkt, len, pcm, OPUS_FRAME_SAMPLES, 0);
            int64_t dt = esp_timer_get_time() - t0;
            if (n != OPUS_FRAME_SAMPLES) bad++;
            total_us += dt;
            if (dt > worst_us) worst_us = dt;
            frames++;
        }
    }
    unsigned hw = uxTaskGetStackHighWaterMark(NULL);   // words remaining, not bytes
    unsigned used = PROBE_STACK - hw * sizeof(StackType_t);

    // mean is what decides the frame budget; worst is what decides whether a
    // single frame can blow the 33.3 ms deadline on its own.
    ESP_LOGI("opusprobe",
             "OPUSPROBE frames=%ld bad=%ld mean_us=%lld worst_us=%lld stack_used=%u of %d",
             frames, bad, total_us / (frames ? frames : 1), worst_us, used, PROBE_STACK);
    ESP_LOGI("opusprobe", "OPUSPROBE realtime_pct=%lld",
             (total_us * 100) / (frames * 20000LL));   // 20 ms of audio per frame
    opus_decoder_destroy(dec);
    vTaskDelete(NULL);
}

void opus_probe(void) {
    xTaskCreatePinnedToCore(opus_probe_task, "opusprobe", PROBE_STACK / sizeof(StackType_t),
                            NULL, 7, NULL, 0);
}
