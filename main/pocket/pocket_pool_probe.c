#include "pocket_pool_probe.h"
#ifdef KASANE_P0_PROBE
#include "pocket_api.h"
#include "pocket_kasane.h"
#include "ui/kasane/ksn_source_pool_adapter.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ksn_schema_value fields[2];
    char face[6],tag[5];
} pool_probe_payload;
typedef struct {
    ksn_source_registry registry;
    ksn_source_provider provider;
    ksn_source_pool_adapter adapter;
    ksn_source_pool pool;
    ksn_source_handle handle;
    pool_probe_payload payloads[KSN_SOURCE_POOL_SLOTS];
    TaskHandle_t worker;
    atomic_bool stop,exited;
    atomic_uint published;
} pool_probe_service;
static pool_probe_service *service;

static ksn_result describe(const void *data,ksn_source_pool_view *out){
    const pool_probe_payload *payload=data;
    if(!payload||!out)return KSN_INVALID;
    *out=(ksn_source_pool_view){.fields=payload->fields,
        .valid_fields=3u,.changed_fields=3u};
    return KSN_OK;
}
static bool allow(void *policy,uint32_t consumer){
    (void)policy;return consumer!=0;
}
static void publish(pool_probe_service *s,unsigned count){
    ksn_source_write write={0};
    if(ksn_source_pool_begin(&s->pool,&write)!=KSN_OK)return;
    pool_probe_payload *payload=write.data;
    /* Construct each complete generation in its final slot; no payload copy. */
    unsigned shown=count%10000u;
    payload->face[0]='P';
    for(unsigned place=4;place;place--){
        payload->face[place]=(char)('0'+shown%10u);shown/=10u;
    }
    payload->face[5]=0;
    memcpy(payload->tag,"TASK",5);
    payload->fields[0].data.text=(ksn_schema_text){payload->face,5};
    payload->fields[1].data.text=(ksn_schema_text){payload->tag,4};
    if(ksn_source_pool_publish(&write,NULL)==KSN_OK)
        atomic_fetch_add_explicit(&s->published,1,memory_order_relaxed);
}
static void worker(void *arg){
    pool_probe_service *s=arg;
    unsigned count=0;
    while(!atomic_load_explicit(&s->stop,memory_order_acquire)){
        publish(s,count++);
#ifdef KASANE_P5_FAIRNESS_PROBE
        /* Faster than the UI frame, so every eligible presenter step finds a
         * newer immutable generation. Diagnostic builds only. */
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(10));
#else
        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));
#endif
    }
    atomic_store_explicit(&s->exited,true,memory_order_release);
    vTaskDelete(NULL);
}

JSValue pocket_pool_probe_source(JSContext *ctx,JSValueConst self,
                                 int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    if(!service){
        static const ksn_slot_type types[]={KSN_SLOT_TEXT,KSN_SLOT_TEXT};
        pool_probe_service *s=calloc(1,sizeof(*s));
        if(!s)return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,
            "time.poolProbeSource","source allocation failed",true,
            POCKET_OUTCOME_NOT_APPLIED);
        atomic_init(&s->stop,false);
        atomic_init(&s->exited,false);
        atomic_init(&s->published,0);
        ksn_result r=ksn_source_pool_init(&s->pool,s->payloads,sizeof(s->payloads),
                                           sizeof(s->payloads[0]));
        if(r==KSN_OK)r=ksn_source_pool_adapter_open(&s->adapter,&s->pool,types,2,
            offsetof(pool_probe_payload,fields),describe,allow,NULL,&s->provider);
        if(r==KSN_OK)ksn_source_registry_init(&s->registry);
        if(r==KSN_OK)r=ksn_source_register(&s->registry,&s->provider,&s->handle);
        if(r==KSN_OK)r=ksn_source_pool_adapter_registered(&s->adapter,s->handle);
        if(r!=KSN_OK){free(s);return pocket_api_throw(ctx,
            POCKET_ERR_INVALID_ARGUMENT,"time.poolProbeSource",
            "source registration failed",false,POCKET_OUTCOME_NOT_APPLIED);}
        if(xTaskCreate(worker,"ksn_pool_probe",3072,s,4,&s->worker)!=pdPASS){
            (void)ksn_source_unregister(&s->registry,s->handle);
            free(s);
            return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,
                "time.poolProbeSource","producer task allocation failed",true,
                POCKET_OUTCOME_NOT_APPLIED);
        }
        service=s;
        ESP_LOGI("KSN_POOL","START core=task heap=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    }
    return pocket_kasane_source_capability(ctx,&service->registry,service->handle);
}

void pocket_pool_probe_reset(void){
    pool_probe_service *s=service;
    if(!s)return;
    atomic_store_explicit(&s->stop,true,memory_order_release);
    for(unsigned wait=0;wait<200&&
        !atomic_load_explicit(&s->exited,memory_order_acquire);wait++)
        vTaskDelay(1);
    if(!atomic_load_explicit(&s->exited,memory_order_acquire)){
        /* A stuck worker may still dereference s. Preserve storage over UAF. */
        ESP_LOGE("KSN_POOL","STOP_TIMEOUT storage retained");
        service=NULL;
        return;
    }
    ESP_LOGI("KSN_POOL","STOP published=%u skipped=%u heap=%u",
             (unsigned)atomic_load_explicit(&s->published,memory_order_relaxed),
             (unsigned)ksn_source_pool_skipped(&s->pool),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if(ksn_source_unregister(&s->registry,s->handle)==KSN_OK)free(s);
    else ESP_LOGE("KSN_POOL","UNREGISTER_FAILED storage retained");
    service=NULL;
}
#endif
