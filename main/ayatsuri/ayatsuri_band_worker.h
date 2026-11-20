#ifndef AYATSURI_BAND_WORKER_H
#define AYATSURI_BAND_WORKER_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
#include "ayatsuri_companion_net_probe.h"
#include "esp_timer.h"
#endif

/* A single presentation task owns this zero-initialized object. Submitted
 * work borrows the callback context until join or close. The callback has
 * exclusive ownership of its output and must not call presentation APIs. */
typedef bool (*ay_band_job)(void *context, unsigned band);
typedef struct {
    StaticSemaphore_t ready_storage;
    SemaphoreHandle_t ready;
    TaskHandle_t task;
    ay_band_job render;
    void *context;
    unsigned band;
    bool attempted, pending, result, stopping;
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
    bool measure;
    uint32_t submitted_us, notified_us, started_us, finished_us;
    uintptr_t core0_at_queue;
#endif
} ay_band_worker;

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
static void ay_band_worker_main(void *opaque) {
    ay_band_worker *w = opaque;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (w->stopping) {
            /* Distinguish explicit parking from the indefinite notification
             * wait, which some FreeRTOS configurations also mark suspended. */
            xSemaphoreGive(w->ready);
            vTaskSuspend(NULL);
            continue;
        }
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
        if (w->measure) w->started_us = (uint32_t)esp_timer_get_time();
#endif
        w->result = w->render(w->context, w->band);
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
        if (w->measure) w->finished_us = (uint32_t)esp_timer_get_time();
#endif
        xSemaphoreGive(w->ready);
    }
}
#endif

static inline bool ay_band_worker_start(ay_band_worker *w, ay_band_job render,
                                        void *context, unsigned band) {
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    if (!render || w->pending) return false;
    if (!w->attempted) {
        w->attempted = true;
        w->ready = xSemaphoreCreateBinaryStatic(&w->ready_storage);
        if (!w->ready || xTaskCreatePinnedToCore(ay_band_worker_main, "ay_band",
                    4096, w, uxTaskPriorityGet(NULL), &w->task, 1-xPortGetCoreID()) != pdPASS)
            w->task = NULL;
    }
    if (!w->task) return false;
    w->render = render;
    w->context = context;
    w->band = band;
    w->pending = true;
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
    w->measure = ay_net_measure_frame;
    if (w->measure) {
        /* Take an opaque task identity without dereferencing the other TCB. */
        w->core0_at_queue = (uintptr_t)xTaskGetCurrentTaskHandleForCore(0);
        w->submitted_us = (uint32_t)esp_timer_get_time();
    }
#endif
    xTaskNotifyGive(w->task);
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
    if (w->measure) w->notified_us = (uint32_t)esp_timer_get_time();
#endif
    return true;
#else
    (void)w; (void)render; (void)context; (void)band;
    return false;
#endif
}

static inline bool ay_band_worker_join(ay_band_worker *w) {
    if (!w->pending) return true;
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
    const uint32_t wait_us = w->measure ? (uint32_t)esp_timer_get_time() - w->submitted_us : 0;
    const uintptr_t task_at_join = w->measure ? (uintptr_t)xTaskGetCurrentTaskHandleForCore(0) : 0;
#endif
    xSemaphoreTake(w->ready, portMAX_DELAY);
#ifdef CONFIG_AYATSURI_COMPANION_NET_PROBE
    if (w->measure) {
        const uint32_t resumed_us = (uint32_t)esp_timer_get_time() - w->submitted_us;
        const uint32_t start_us = w->started_us - w->submitted_us;
        const uint32_t end_us = w->finished_us - w->submitted_us;
        /* Count only the part of each stage that overlaps the caller's join. */
        const uint32_t raster_us = start_us > wait_us ? start_us : wait_us;
        const uint32_t wake_us = end_us > wait_us ? end_us : wait_us;
        ay_net_frame_worker_wait.dispatch += raster_us - wait_us;
        ay_net_frame_worker_wait.raster += wake_us - raster_us;
        ay_net_frame_worker_wait.wake += resumed_us - wake_us;
        if (start_us > ay_net_frame_worker_wait.peak.dispatch) {
            ay_net_frame_worker_wait.peak = (ay_net_dispatch){
                .queued = w->submitted_us,
                .notified = w->notified_us,
                .joined = w->submitted_us + wait_us,
                .started = w->started_us,
                .dispatch = start_us,
                .core0_at_queue = w->core0_at_queue,
                .core0_at_join = task_at_join,
            };
        }
    }
#endif
    w->pending = false;
    return w->result;
}

static inline void ay_band_worker_close(ay_band_worker *w) {
    (void)ay_band_worker_join(w);
    if (w->task) {
        w->stopping = true;
        xTaskNotifyGive(w->task);
        xSemaphoreTake(w->ready, portMAX_DELAY);
        while (eTaskGetState(w->task) != eSuspended) vTaskDelay(1);
        vTaskDelete(w->task);
    }
    if (w->ready) vSemaphoreDelete(w->ready);
    *w = (ay_band_worker){0};
}
#endif
