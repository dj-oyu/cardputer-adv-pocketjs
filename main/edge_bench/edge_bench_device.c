#include "edge_bench_device.h"
#include "edge_bench_owner.h"
#include "bench_core.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "esp_app_desc.h"
#include "esp_log.h"

#ifndef EDGE_BENCH_SOURCE_ID
#error missing bench source identity
#endif
#define BENCH_ID "local.edgebench"
#define WINDOW_LIMIT_US INT64_C(250000)
static edge_owner owner; /* UI owner only */
static uint64_t boot_id;
static atomic_bool armed,cancelled,done;
static TaskHandle_t worker_task; /* UI-owned; worker and UI are pinned to the same core. */
/* Single worker; no new START until old result was consumed by the UI owner. */
static edge_bench_result result;
static uint32_t wall_us[31][3],calls,cp_at_pie_end,stack_free;
static int guard_error;
static uint32_t completed_generation;
static int64_t window_start;
static int start_core,start_hz;
static bool stop_requested(void *unused){(void)unused;return atomic_load_explicit(&cancelled,memory_order_acquire)||guard_error;}
static void warm_yield(void *unused){(void)unused;vTaskDelay(1);}
static void timed_yield(void){vTaskDelay(1);}
static uint32_t read_cpenable(void){
#ifdef __XTENSA__
 uint32_t value;__asm__ volatile("rsr.cpenable %0":"=a"(value));return value;
#else
 return 0; /* Host port fixture only; never evidence of CP3 activation. */
#endif
}
static void prime_cp3(void){
#ifdef __XTENSA__
 /* IDF's voluntary yield clears CPENABLE. Reacquire through the standard
  * lazy exception before EACH window, equally for scalar/PIE/overhead. */
 __asm__ volatile("ee.zero.q q7":::"memory");
#endif
}
static uint32_t read_ccount(void){
 uint32_t cy;
 if(!(calls&1u)){
  prime_cp3();
  start_core=xPortGetCoreID();start_hz=esp_clk_cpu_freq();window_start=esp_timer_get_time();
  __asm__ volatile("":::"memory");cy=esp_cpu_get_cycle_count();__asm__ volatile("":::"memory");
 }else{
  __asm__ volatile("":::"memory");cy=esp_cpu_get_cycle_count();__asm__ volatile("":::"memory");
  int64_t elapsed=esp_timer_get_time()-window_start;
  if(elapsed<0||elapsed>=WINDOW_LIMIT_US||start_core!=CONFIG_POCKET_EDGE_BENCH_CORE||xPortGetCoreID()!=start_core||start_hz<=0||start_hz>1000000000||esp_clk_cpu_freq()!=start_hz)guard_error=5;
  unsigned window=calls/2;
  if(window<93)wall_us[window/3][window%3]=(uint32_t)(elapsed<0?0:elapsed);
  else guard_error=5;
  /* Read-only snapshot, outside the counter interval. Never write CPENABLE. */
  if(window%3&&((window/3+(window%3-1))&1u))cp_at_pie_end=read_cpenable();
 }
 calls++;return cy;
}
static void worker(void *arg){
 uint32_t generation=(uint32_t)(uintptr_t)arg;
 int64_t bind_start=esp_timer_get_time();
 while(!atomic_load_explicit(&armed,memory_order_acquire)&&!atomic_load_explicit(&cancelled,memory_order_acquire)){
  if(esp_timer_get_time()-bind_start>=1000000){guard_error=6;break;}vTaskDelay(1);
 }
 if(stop_requested(NULL)){memset(&result,0,sizeof result);result.error=guard_error?guard_error:4;}
 else{
  edge_bench_control c={stop_requested,warm_yield,NULL};
  (void)edge_bench_run_controlled(read_ccount,timed_yield,&result,&c);
  if(guard_error)result.error=guard_error;
  if(!result.error&&(calls!=186||result.completed!=31||result.checksum!=UINT64_C(0x2ea0f4944e5cf580)))result.error=7;
 }
 stack_free=uxTaskGetStackHighWaterMark(NULL);
 completed_generation=generation;
 atomic_store_explicit(&done,true,memory_order_release);
 /* No shared run/result access after publication. The higher-priority UI
  * owner on this same core deletes this noncurrent task and joins its IDF
  * cleanup before releasing the measurement owner. Never resume/reuse it. */
 vTaskSuspend(NULL);
 for(;;)vTaskDelay(1);
}
void edge_device_init(uint64_t boot){boot_id=boot;}
bool edge_device_owned(void){return owner.owned;}
uint32_t edge_device_instance(void){return owner.generation;}
bool edge_device_start(void){
 if(xPortInIsrContext()||xPortGetCoreID()!=CONFIG_POCKET_EDGE_BENCH_CORE||!edge_owner_begin(&owner))return false;
 atomic_store(&armed,false);atomic_store(&cancelled,false);atomic_store(&done,false);
 memset(&result,0,sizeof result);memset(wall_us,0,sizeof wall_us);
 worker_task=NULL;calls=cp_at_pie_end=stack_free=completed_generation=0;guard_error=0;start_hz=0;start_core=-1;window_start=0;
 if(xTaskCreatePinnedToCore(worker,"edgebench",CONFIG_POCKET_EDGE_BENCH_STACK,(void*)(uintptr_t)owner.generation,4,&worker_task,CONFIG_POCKET_EDGE_BENCH_CORE)!=pdPASS){owner.owned=false;return false;}
 return true;
}
void edge_device_bind(const char *id,uint32_t run,uint32_t instance){
 if(id&&strcmp(id,BENCH_ID)==0&&edge_owner_bind(&owner,run,instance))atomic_store_explicit(&armed,true,memory_order_release);
}
void edge_device_poll(void){
 if(!owner.owned||owner.logged||!atomic_load_explicit(&done,memory_order_acquire))return;
 if(completed_generation!=owner.generation){owner.quarantine=true;return;}
 if(worker_task){vTaskDelete(worker_task);worker_task=NULL;}
 owner.logged=true;
 const esp_app_desc_t *d=esp_app_get_description();
 char elf[65];for(unsigned i=0;i<32;i++)snprintf(elf+2*i,3,"%02x",d->app_elf_sha256[i]);
 ESP_LOGI("EDGE_BENCH","boot=%" PRIu64 " run=%" PRIu32 " generation=%" PRIu32 " base=47e1a4c642fc0807759e44a09ee07898f1f0ae3e source=%s app=%.32s elf=%s clock=CCOUNT core=%d hz=%d flags=O2,noLTO,FNV-consumer stack_config=%d stack_free_bytes=%" PRIu32 " cp_pie_end=%" PRIx32,
  boot_id,owner.run,owner.generation,EDGE_BENCH_SOURCE_ID,d->version,elf,CONFIG_POCKET_EDGE_BENCH_CORE,start_hz,CONFIG_POCKET_EDGE_BENCH_STACK,stack_free,cp_at_pie_end);
 for(unsigned r=0;r<result.completed&&r<31;r++){
  unsigned scalar_slot=(r&1u)?2:1,pie_slot=(r&1u)?1:2;
  ESP_LOGI("EDGE_BENCH","run=%" PRIu32 " generation=%" PRIu32 " sample=%u scalar=%" PRIu32 " pie=%" PRIu32 " overhead=%" PRIu32 " scalar_us=%" PRIu32 " pie_us=%" PRIu32,
   owner.run,owner.generation,r,result.scalar[r],result.pie[r],result.clock_overhead[r],wall_us[r][scalar_slot],wall_us[r][pie_slot]);
 }
 bool stop=atomic_load_explicit(&cancelled,memory_order_acquire);
 ESP_LOGI("EDGE_BENCH","run=%" PRIu32 " generation=%" PRIu32 " completed=%u error=%d status=%s checksum=%016" PRIx64 " worker_user_work_done=1 worker_deleted=1 owner_release=%d",
  owner.run,owner.generation,result.completed,result.error,owner.quarantine?"QUARANTINED":stop?"CANCELLED":result.error?"ERROR":"COMPLETED",result.checksum,!owner.quarantine);
 if(!owner.quarantine)(void)edge_owner_release(&owner,completed_generation);
}
bool edge_device_stop(void){
 if(!owner.owned)return true;
 if(owner.quarantine)return false;
 atomic_store_explicit(&cancelled,true,memory_order_release);
 int64_t until=esp_timer_get_time()+100000;
 while(!atomic_load_explicit(&done,memory_order_acquire)&&esp_timer_get_time()<until)vTaskDelay(1);
 if(!atomic_load_explicit(&done,memory_order_acquire)){owner.quarantine=true;return false;}
 edge_device_poll();return !owner.owned;
}
