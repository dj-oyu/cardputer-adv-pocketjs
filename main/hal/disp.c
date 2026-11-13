// The display task: disp.h.
//
// One job at a time. While a job is in flight the UI task must not touch what
// the job reads (docs/perf/two-core-pipeline.md sec.1.2, invariant I1); every
// way back to that state goes through disp_join(). The notification wakes the
// task, a binary semaphore wakes the joiner: the UI task's own notification
// slot belongs to vm_wake (main/vm/vm_wake.c). The semaphore also orders the
// job's writes before the joiner's reads.
//
// The in-flight state is the UI task's alone (only it starts and joins), so
// it needs no lock: the display task sees only `job` and writes `job_result`.
#include "disp.h"
#include <stddef.h>
#ifdef CONFIG_POCKET_DISP_TASK
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#endif
#ifdef CONFIG_POCKET_DISP_ASSERT
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// The first task to start a job is the UI task; nobody else may start or join.
static TaskHandle_t owner;
static void check_owner(void) {
    TaskHandle_t self=xTaskGetCurrentTaskHandle();
    if(!owner) owner=self;
    assert(self==owner);
}
#else
static inline void check_owner(void) {}
#endif

static disp_finish_fn pending_finish;
static bool in_flight;      // handed to the task, not yet waited for
static bool pending;        // finish step not run yet
static esp_err_t job_result;

#ifdef CONFIG_POCKET_DISP_TASK
#define DISP_STACK CONFIG_POCKET_DISP_STACK
static StaticTask_t task_storage;
static StackType_t task_stack[DISP_STACK / sizeof(StackType_t)];
static StaticSemaphore_t done_storage;
static SemaphoreHandle_t done;
static TaskHandle_t task;
static disp_job_fn job;

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
     * stays below sfx (7). */
    task=xTaskCreateStaticPinnedToCore(disp_task,"disp",DISP_STACK/sizeof(StackType_t),NULL,5,
                                       task_stack,&task_storage,CONFIG_POCKET_DISP_CORE);
    return task ? ESP_OK : ESP_FAIL;
}

esp_err_t disp_run(disp_job_fn fn, bool on_task) {
    disp_join();
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

void disp_start(disp_job_fn fn, disp_finish_fn finish, bool on_task) {
    check_owner();
    disp_join();
    pending_finish=finish;
    pending=true;
#ifdef CONFIG_POCKET_DISP_TASK
    if(task && on_task) {
        job=fn;
        in_flight=true;
        xTaskNotifyGive(task);
        return;
    }
#else
    (void)on_task;
#endif
    job_result=fn();
}

void disp_join(void) {
    if(!pending) return;
    check_owner();
#ifdef CONFIG_POCKET_DISP_TASK
    if(in_flight) {
        // A present takes milliseconds; a second means the task is stuck
        // (an SPI send that never completes), which should say so (F14).
        while(xSemaphoreTake(done,pdMS_TO_TICKS(1000))!=pdTRUE)
            ESP_LOGW("disp","join waiting > 1 s");
        in_flight=false;
    }
#endif
    // Cleared before the finish step, which may itself reach a join point.
    pending=false;
    disp_finish_fn finish=pending_finish;
    pending_finish=NULL;
    if(finish) finish(job_result);
}

bool disp_busy(void) {
    return pending;
}
