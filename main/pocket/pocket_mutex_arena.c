#include "pocket_mutex_arena.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#ifdef KASANE_P0_PROBE
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif
#include <stdatomic.h>
#include <limits.h>
#include <stdint.h>

/* A lazy libc lock owns its queue for the lifetime of the process. Allocating
 * those 84-byte queues between short-lived app allocations fragments the
 * largest internal heap block. Group only mutexes created while an opted-in
 * producer is active. Queue creation/deletion is not on the audio or draw
 * hot path. Allocate before later system services start their own lifetime
 * locks; placing this block at the first mutex creation is too late to avoid
 * fragmenting the largest free heap region. */
#define POCKET_MUTEX_ARENA_SLOTS 16u
static _Atomic(StaticQueue_t *) arena;
static atomic_bool active;
static portMUX_TYPE arena_lock=portMUX_INITIALIZER_UNLOCKED;
static uint8_t taken[POCKET_MUTEX_ARENA_SLOTS];
static unsigned active_users;
#ifdef KASANE_P0_PROBE
static unsigned peak,fallback;
static atomic_bool device_race_armed,device_probe_running;
static atomic_uint device_race_allocated,device_race_duplicates;
static atomic_bool device_fail_next_allocation;
#endif

/* Caller holds arena_lock. A reserved slot also counts as live until static
 * mutex creation finishes, so teardown cannot free its backing mid-create. */
static StaticQueue_t *detach_if_unused_locked(void) {
    if(atomic_load_explicit(&active,memory_order_relaxed))return NULL;
    for(unsigned i=0;i<POCKET_MUTEX_ARENA_SLOTS;i++)
        if(taken[i])return NULL;
    return atomic_exchange_explicit(&arena,NULL,memory_order_acq_rel);
}

bool pocket_mutex_arena_activate(void) {
    portENTER_CRITICAL(&arena_lock);
    if(active_users==UINT_MAX){portEXIT_CRITICAL(&arena_lock);return false;}
    if(atomic_load_explicit(&arena,memory_order_relaxed)) {
        active_users++;
        atomic_store_explicit(&active,true,memory_order_release);
        portEXIT_CRITICAL(&arena_lock);
        return true;
    }
    portEXIT_CRITICAL(&arena_lock);
#ifdef KASANE_P0_PROBE
    if(atomic_exchange_explicit(&device_fail_next_allocation,false,memory_order_acq_rel))
        return false;
#endif
    StaticQueue_t *storage=heap_caps_calloc(POCKET_MUTEX_ARENA_SLOTS,
        sizeof(*storage),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!storage)return false;
#ifdef KASANE_P0_PROBE
    if(atomic_load_explicit(&device_race_armed,memory_order_acquire)) {
        atomic_fetch_add_explicit(&device_race_allocated,1,memory_order_acq_rel);
        int64_t deadline=esp_timer_get_time()+200000;
        while(atomic_load_explicit(&device_race_allocated,memory_order_acquire)<2u&&
              esp_timer_get_time()<deadline)taskYIELD();
    }
#endif
    bool duplicate=false;
    portENTER_CRITICAL(&arena_lock);
    if(active_users==UINT_MAX){
        portEXIT_CRITICAL(&arena_lock);
        heap_caps_free(storage);
        return false;
    }
    if(!atomic_load_explicit(&arena,memory_order_relaxed))
        atomic_store_explicit(&arena,storage,memory_order_release);
    else duplicate=true;
    active_users++;
    atomic_store_explicit(&active,true,memory_order_release);
    portEXIT_CRITICAL(&arena_lock);
    if(duplicate) {
#ifdef KASANE_P0_PROBE
        atomic_fetch_add_explicit(&device_race_duplicates,1,memory_order_relaxed);
#endif
        heap_caps_free(storage);
    }
    return true;
}

void pocket_mutex_arena_deactivate(void) {
    StaticQueue_t *unused=NULL;
    portENTER_CRITICAL(&arena_lock);
    if(active_users)active_users--;
    atomic_store_explicit(&active,active_users!=0,memory_order_release);
    unused=detach_if_unused_locked();
    portEXIT_CRITICAL(&arena_lock);
    if(unused)heap_caps_free(unused);
}

extern QueueHandle_t __real_xQueueCreateMutex(const uint8_t type);
QueueHandle_t __wrap_xQueueCreateMutex(const uint8_t type) {
    if(!atomic_load_explicit(&active,memory_order_acquire))
        return __real_xQueueCreateMutex(type);
    int slot=-1;
    StaticQueue_t *storage;
    portENTER_CRITICAL(&arena_lock);
    storage=atomic_load_explicit(&arena,memory_order_relaxed);
    if(atomic_load_explicit(&active,memory_order_relaxed)&&storage) {
        for(unsigned i=0;i<POCKET_MUTEX_ARENA_SLOTS;i++)if(!taken[i]) {
            taken[i]=1;
            slot=(int)i;
            break;
        }
#ifdef KASANE_P0_PROBE
        if(slot<0)fallback++;
        else {
            unsigned live=0;
            for(unsigned i=0;i<POCKET_MUTEX_ARENA_SLOTS;i++)live+=taken[i]!=0;
            if(live>peak)peak=live;
        }
#endif
    }
    portEXIT_CRITICAL(&arena_lock);
    if(slot<0)return __real_xQueueCreateMutex(type);
    QueueHandle_t queue=xQueueCreateMutexStatic(type,&storage[slot]);
    if(queue)return queue;
    StaticQueue_t *unused=NULL;
    portENTER_CRITICAL(&arena_lock);
    taken[slot]=0;
    unused=detach_if_unused_locked();
    portEXIT_CRITICAL(&arena_lock);
    if(unused)heap_caps_free(unused);
    return __real_xQueueCreateMutex(type);
}

extern void __real_vQueueDelete(QueueHandle_t queue);
void __wrap_vQueueDelete(QueueHandle_t queue) {
    int slot=-1;
    if(atomic_load_explicit(&arena,memory_order_acquire)&&queue) {
        portENTER_CRITICAL(&arena_lock);
        StaticQueue_t *storage=atomic_load_explicit(&arena,memory_order_relaxed);
        if(storage) {
            uintptr_t first=(uintptr_t)storage,value=(uintptr_t)queue;
            uintptr_t span=POCKET_MUTEX_ARENA_SLOTS*sizeof(*storage);
            if(value>=first&&value-first<span&&
               (value-first)%sizeof(*storage)==0)
                slot=(int)((value-first)/sizeof(*storage));
        }
        portEXIT_CRITICAL(&arena_lock);
    }
    __real_vQueueDelete(queue);
    if(slot>=0) {
        StaticQueue_t *unused=NULL;
        portENTER_CRITICAL(&arena_lock);
        taken[slot]=0;
        unused=detach_if_unused_locked();
        portEXIT_CRITICAL(&arena_lock);
        if(unused)heap_caps_free(unused);
    }
}

#ifdef KASANE_P0_PROBE
typedef struct {
    atomic_bool go,abort;
    atomic_uint ready,done,activated,created;
} pocket_mutex_race_probe;
static pocket_mutex_race_probe device_probe;

static void pocket_mutex_race_worker(void *arg) {
    pocket_mutex_race_probe *probe=(pocket_mutex_race_probe *)arg;
    while(!atomic_load_explicit(&probe->go,memory_order_acquire))vTaskDelay(1);
    if(!atomic_load_explicit(&probe->abort,memory_order_acquire)) {
        bool activated=pocket_mutex_arena_activate();
        if(activated) {
            atomic_fetch_add_explicit(&probe->activated,1,memory_order_relaxed);
            SemaphoreHandle_t mutex=xSemaphoreCreateMutex();
            if(mutex)atomic_fetch_add_explicit(&probe->created,1,memory_order_relaxed);
            atomic_fetch_add_explicit(&probe->ready,1,memory_order_acq_rel);
            int64_t deadline=esp_timer_get_time()+200000;
            while(atomic_load_explicit(&probe->ready,memory_order_acquire)<2u&&
                  esp_timer_get_time()<deadline)taskYIELD();
            pocket_mutex_arena_deactivate();
            if(mutex)vSemaphoreDelete(mutex);
        } else atomic_fetch_add_explicit(&probe->ready,1,memory_order_acq_rel);
    }
    atomic_fetch_add_explicit(&probe->done,1,memory_order_release);
    vTaskDelete(NULL);
}

bool pocket_mutex_arena_device_race_probe(void) {
    bool expected=false;
    if(!atomic_compare_exchange_strong(&device_probe_running,&expected,true))return false;
    portENTER_CRITICAL(&arena_lock);
    bool empty=!atomic_load_explicit(&arena,memory_order_relaxed)&&active_users==0;
    portEXIT_CRITICAL(&arena_lock);
    if(!empty) {
        ESP_LOGW("KSN_MUTEX_ARENA","RACE_SKIP arena already owned; run immediately after boot");
        atomic_store(&device_probe_running,false);
        return false;
    }
    atomic_store(&device_probe.go,false);
    atomic_store(&device_probe.abort,false);
    atomic_store(&device_probe.ready,0);
    atomic_store(&device_probe.done,0);
    atomic_store(&device_probe.activated,0);
    atomic_store(&device_probe.created,0);
    atomic_store(&device_race_allocated,0);
    atomic_store(&device_race_duplicates,0);
    BaseType_t a=xTaskCreatePinnedToCore(pocket_mutex_race_worker,"ksn_arena0",2048,
                                         &device_probe,4,NULL,0);
    BaseType_t b=xTaskCreatePinnedToCore(pocket_mutex_race_worker,"ksn_arena1",2048,
                                         &device_probe,4,NULL,1);
    if(a!=pdPASS||b!=pdPASS)atomic_store(&device_probe.abort,true);
    atomic_store_explicit(&device_race_armed,true,memory_order_release);
    atomic_store_explicit(&device_probe.go,true,memory_order_release);
    unsigned expected_done=(unsigned)(a==pdPASS)+(unsigned)(b==pdPASS);
    int64_t deadline=esp_timer_get_time()+1000000;
    while(atomic_load_explicit(&device_probe.done,memory_order_acquire)<expected_done&&
          esp_timer_get_time()<deadline)vTaskDelay(1);
    atomic_store_explicit(&device_race_armed,false,memory_order_release);
    unsigned done=atomic_load(&device_probe.done);
    unsigned activated=atomic_load(&device_probe.activated);
    unsigned created=atomic_load(&device_probe.created);
    unsigned allocated=atomic_load(&device_race_allocated);
    unsigned duplicates=atomic_load(&device_race_duplicates);
    portENTER_CRITICAL(&arena_lock);
    bool released=!atomic_load_explicit(&arena,memory_order_relaxed)&&active_users==0;
    for(unsigned i=0;i<POCKET_MUTEX_ARENA_SLOTS;i++)released&=taken[i]==0;
    portEXIT_CRITICAL(&arena_lock);
    bool pass=expected_done==2&&done==2&&activated==2&&created==2&&
              allocated==2&&duplicates==1&&released;
    ESP_LOGI("KSN_MUTEX_ARENA","RACE_%s tasks=%u/%u activated=%u mutexes=%u allocations=%u duplicates=%u released=%u",
             pass?"PASS":"FAIL",done,expected_done,activated,created,allocated,
             duplicates,(unsigned)released);
    /* A timed-out worker may still reference the static probe state. Keep
     * the diagnostic locked out rather than resetting it under that worker. */
    if(done==expected_done)atomic_store(&device_probe_running,false);
    return pass;
}

bool pocket_mutex_arena_device_oom_probe(void) {
    bool expected=false;
    if(!atomic_compare_exchange_strong(&device_probe_running,&expected,true))return false;
    portENTER_CRITICAL(&arena_lock);
    bool empty=!atomic_load_explicit(&arena,memory_order_relaxed)&&active_users==0;
    portEXIT_CRITICAL(&arena_lock);
    if(!empty) {
        ESP_LOGW("KSN_MUTEX_ARENA","OOM_SKIP arena already owned; run immediately after boot");
        atomic_store(&device_probe_running,false);
        return false;
    }
    atomic_store(&device_fail_next_allocation,true);
    bool rejected=!pocket_mutex_arena_activate();
    SemaphoreHandle_t fallback_mutex=xSemaphoreCreateMutex();
    if(fallback_mutex)vSemaphoreDelete(fallback_mutex);
    bool recovered=pocket_mutex_arena_activate();
    SemaphoreHandle_t arena_mutex=recovered?xSemaphoreCreateMutex():NULL;
    if(recovered)pocket_mutex_arena_deactivate();
    if(arena_mutex)vSemaphoreDelete(arena_mutex);
    portENTER_CRITICAL(&arena_lock);
    bool released=!atomic_load_explicit(&arena,memory_order_relaxed)&&active_users==0;
    for(unsigned i=0;i<POCKET_MUTEX_ARENA_SLOTS;i++)released&=taken[i]==0;
    portEXIT_CRITICAL(&arena_lock);
    bool pass=rejected&&fallback_mutex&&recovered&&arena_mutex&&released;
    ESP_LOGI("KSN_MUTEX_ARENA","OOM_INJECT_%s rejected=%u fallback_mutex=%u recovered=%u arena_mutex=%u released=%u",
             pass?"PASS":"FAIL",(unsigned)rejected,(unsigned)(fallback_mutex!=NULL),
             (unsigned)recovered,(unsigned)(arena_mutex!=NULL),(unsigned)released);
    atomic_store(&device_probe_running,false);
    return pass;
}

void pocket_mutex_arena_report(void) {
    unsigned live=0,p,f;
    portENTER_CRITICAL(&arena_lock);
    for(unsigned i=0;i<POCKET_MUTEX_ARENA_SLOTS;i++)live+=taken[i]!=0;
    p=peak;f=fallback;
    portEXIT_CRITICAL(&arena_lock);
    ESP_LOGI("KSN_MUTEX_ARENA","slots=%u live=%u peak=%u fallback=%u bytes=%u",
        atomic_load_explicit(&arena,memory_order_acquire)?POCKET_MUTEX_ARENA_SLOTS:0u,
        live,p,f,atomic_load_explicit(&arena,memory_order_acquire)?
        (unsigned)(POCKET_MUTEX_ARENA_SLOTS*sizeof(StaticQueue_t)):0u);
}
#endif
