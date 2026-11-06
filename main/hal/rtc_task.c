#include "rtc_task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

TaskHandle_t rtc_task_create(TaskFunction_t fn, const char *name,
                             uint32_t stack_bytes, UBaseType_t prio) {
    StaticTask_t *tcb=heap_caps_malloc(sizeof *tcb,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    StackType_t *stack=heap_caps_malloc(stack_bytes,MALLOC_CAP_RTCRAM);
    bool rtc=stack!=NULL;
    if(!stack) stack=heap_caps_malloc(stack_bytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    TaskHandle_t h=NULL;
    if(tcb&&stack)
        h=xTaskCreateStaticPinnedToCore(fn,name,stack_bytes/sizeof(StackType_t),NULL,
                                        prio,stack,tcb,0);
    if(!h) { heap_caps_free(tcb); heap_caps_free(stack); }
    ESP_LOGI("boot","TASK_STACK %s %s bytes=%u rtc_free=%u rtc_largest=%u",name,
             !h?"FAILED":rtc?"rtc":"dram",(unsigned)stack_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_RTCRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM));
    return h;
}
