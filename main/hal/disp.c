// The display task: disp.h.
//
// One job at a time and the caller always waits, so the job may touch what
// the UI task owns (the Kasane and Shiki state, the strip, the SPI device):
// the UI task is blocked in disp_run() for exactly as long. The notification
// wakes the task, a binary semaphore wakes the caller back: the UI task's own
// notification slot belongs to vm_wake (main/vm/vm_wake.c).
#include "disp.h"
#ifdef CONFIG_POCKET_DISP_TASK
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define DISP_STACK CONFIG_POCKET_DISP_STACK
static StaticTask_t task_storage;
static StackType_t task_stack[DISP_STACK / sizeof(StackType_t)];
static StaticSemaphore_t done_storage;
static SemaphoreHandle_t done;
static TaskHandle_t task;
static disp_job_fn job;
static esp_err_t job_result;

static void disp_task(void *arg) {
    (void)arg;
    for(;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        job_result=job();
        xSemaphoreGive(done);
    }
}

esp_err_t disp_init(void) {
    if(task) return ESP_OK;
    done=xSemaphoreCreateBinaryStatic(&done_storage);
    if(!done) return ESP_ERR_NO_MEM;
    /* Pinned: the render path uses PIE (coprocessor 3), whose state is kept
     * per task on the core the task runs on. Priority 5, the UI task's: it
     * runs only while the UI task waits, and stays below sfx (7). */
    task=xTaskCreateStaticPinnedToCore(disp_task,"disp",DISP_STACK/sizeof(StackType_t),NULL,5,
                                       task_stack,&task_storage,CONFIG_POCKET_DISP_CORE);
    return task ? ESP_OK : ESP_FAIL;
}

esp_err_t disp_run(disp_job_fn fn, bool on_task) {
    if(!task || !on_task) return fn();
    job=fn;
    xTaskNotifyGive(task);
    xSemaphoreTake(done,portMAX_DELAY);
    return job_result;
}

void disp_set_priority(unsigned prio) {
    if(task) vTaskPrioritySet(task,prio);
}

uint32_t disp_stack_free(void) {
    return task ? (uint32_t)uxTaskGetStackHighWaterMark(task) : 0;
}
#endif
