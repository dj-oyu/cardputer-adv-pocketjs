#include "sdkconfig.h"
#ifdef CONFIG_AYATSURI_STREAM_DEVICE_PROBE
#include "ayatsuri_stream_device_probe.h"
#include "ayatsuri_stream_probe_core.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdlib.h>
static const char*tag="ay_stream_probe";
static uint64_t now(void*context){(void)context;return (uint64_t)esp_timer_get_time();}
static void pause_frame(void*context,unsigned frame){
 (void)context;
 if((frame+1)%8==0)ESP_LOGI(tag,"PROGRESS synthetic=true frame=%u heap_internal=%u largest_internal=%u stack_hwm=%u",frame+1,
  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)uxTaskGetStackHighWaterMark(NULL));
 vTaskDelay(1);
}
void ayatsuri_stream_device_probe_run(void){
 static bool ran;if(ran)return;ran=true;
 const esp_app_desc_t*app=esp_app_get_description();
 ESP_LOGI(tag,"BEGIN id=AY_STREAM_SYNTH_V1 synthetic=true runtime_ready=false lcd_dma=not_exercised elf_sha_prefix=%02x%02x%02x%02x%02x%02x%02x%02x",app->app_elf_sha256[0],app->app_elf_sha256[1],app->app_elf_sha256[2],app->app_elf_sha256[3],app->app_elf_sha256[4],app->app_elf_sha256[5],app->app_elf_sha256[6],app->app_elf_sha256[7]);
 ESP_LOGI(tag,"CONFIG depth_rows=%u depth_bytes=%u",(unsigned)AY_STREAM_PROBE_DEPTH_ROWS,(unsigned)(240*AY_STREAM_PROBE_DEPTH_ROWS*sizeof(float)));
 size_t free_before=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),largest_before=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
 unsigned stack_before=(unsigned)uxTaskGetStackHighWaterMark(NULL);size_t need=ayatsuri_stream_probe_arena_bytes();
 void*arena=need?heap_caps_malloc(need,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT):NULL;
 ayatsuri_stream_probe_work*work=heap_caps_malloc(sizeof *work,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
 ayatsuri_stream_probe_stats stats={0};
 bool ok=arena&&work&&ayatsuri_stream_probe_run(arena,need,work,&stats,now,pause_frame,NULL);
 ESP_LOGI(tag,"PIXELS synthetic=true depth_rows=%u final_hash=%08" PRIx32,(unsigned)AY_STREAM_PROBE_DEPTH_ROWS,stats.final_pixel_hash);
 ESP_LOGI(tag,"RESULT synthetic=true status=%s frames=%u providers=%u rollback=%u arena=%u workspace=%u snapshot_peak=%u geometry_us=%" PRIu64 " geometry_min_us=%" PRIu64 " geometry_max_us=%" PRIu64 " raster_us=%" PRIu64 " ready_after_arena_wipe=%u stack_hwm_before=%u stack_hwm_after=%u",ok?"PASS":"FAIL",stats.frames,stats.provider_calls,stats.rollbacks,(unsigned)need,(unsigned)sizeof *work,(unsigned)stats.snapshot_peak,stats.geometry_us,stats.geometry_min_us,stats.geometry_max_us,stats.raster_us,(unsigned)stats.ready_after_arena_wipe,stack_before,(unsigned)uxTaskGetStackHighWaterMark(NULL));
 free(work);free(arena);
 ESP_LOGI(tag,"END synthetic=true heap_internal_before=%u largest_internal_before=%u heap_internal_after=%u largest_internal_after=%u runtime_ready=false model_fps=unmeasured lcd_dma=not_exercised",(unsigned)free_before,(unsigned)largest_before,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
}
#endif
