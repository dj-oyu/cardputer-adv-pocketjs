#include "derby_fixed_workload.h"
#include "esp_timer.h"
#include "usb_console.h"
#include "app_session.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
static TaskHandle_t ui_owner;
bool derby_runtime_resources_deadline_ok(uint32_t reserve_us){
    return app_derby_fixed_deadline_ok(reserve_us);
}
void derby_runtime_resources_bind_ui_task(void){
    /* Bind at task entry, never infer ownership from the sampling caller. */
    configASSERT(!ui_owner);
    ui_owner=xTaskGetCurrentTaskHandle();
}
bool derby_runtime_resources_read(df_resources *out){
    if(!out||!ui_owner||xTaskGetCurrentTaskHandle()!=ui_owner||
       strcmp(pcTaskGetName(ui_owner),"ui")||df_status()!=DF_TAIL)return false;
    multi_heap_info_t info={0};
    size_t used=0,cap=0;
    if(!app_derby_fixed_guest_memory(&used,&cap)||used>UINT32_MAX||cap>UINT32_MAX)return false;
    /* One allocator query. No global minimum monitor start/reset/stop. */
    heap_caps_get_info(&info,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    *out=(df_resources){
        /* ESP-IDF 6.0.1 returns BYTES, unlike upstream FreeRTOS words. */
        .ui_stack_free=uxTaskGetStackHighWaterMark(ui_owner),
        .internal_free=info.total_free_bytes,.internal_largest=info.largest_free_block,
        .guest_used=(uint32_t)used,.guest_cap=(uint32_t)cap,.synthetic=false};
    return true;
}
uint64_t df_now_us(void){return (uint64_t)esp_timer_get_time();}
int df_write(const char *data,size_t size){return usb_console_frame(data,size)==0;}
