#ifndef AYATSURI_BAND_WORKER_H
#define AYATSURI_BAND_WORKER_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

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
        w->result = w->render(w->context, w->band);
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
    xTaskNotifyGive(w->task);
    return true;
#else
    (void)w; (void)render; (void)context; (void)band;
    return false;
#endif
}

static inline bool ay_band_worker_join(ay_band_worker *w) {
    if (!w->pending) return true;
    xSemaphoreTake(w->ready, portMAX_DELAY);
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
