#include "pocket_av_output_source.h"
#include "pocket_api.h"
#include "pocket_kasane.h"
#include "sound.h"
#include "ui/kasane/ksn_source_pool_adapter.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>

typedef struct {
    ksn_schema_value fields[1];
    char clock[9];
    bool valid;
} output_payload;
typedef struct {
    ksn_source_registry registry;
    ksn_source_provider provider;
    ksn_source_pool_adapter adapter;
    ksn_source_pool pool;
    ksn_source_handle handle;
    output_payload payloads[KSN_SOURCE_POOL_SLOTS];
    atomic_int pending_end_id;
    uint32_t published;
    int32_t last_id;
    uint32_t last_second;
    bool last_valid;
} output_service;

/* The callback is read by the audio task only after sound_stream_set_observer
 * publishes it. This pointer stays live until the output task has stopped;
 * reset retains the allocation on a failed stop instead of risking UAF. */
static _Atomic(output_service *) live;
static output_service *service;

static ksn_result describe(const void *data,ksn_source_pool_view *out){
    const output_payload *payload=data;
    if(!payload||!out)return KSN_INVALID;
    *out=(ksn_source_pool_view){.fields=payload->fields,
        .valid_fields=payload->valid?1u:0u,.changed_fields=1u};
    return KSN_OK;
}
static bool allow(void *policy,uint32_t consumer){
    (void)policy;return consumer!=0;
}
static ksn_result publish(output_service *s,int32_t id,uint32_t frames,bool valid){
    ksn_source_write write={0};
    ksn_result r=ksn_source_pool_begin(&s->pool,&write);
    if(r!=KSN_OK)return r;
    output_payload *payload=write.data;
    if(valid){
        uint32_t seconds=frames/SOUND_SAMPLE_RATE;
        unsigned hour=seconds/3600u,minute=seconds/60u%60u,second=seconds%60u;
        payload->clock[0]=(char)('0'+hour/10u);
        payload->clock[1]=(char)('0'+hour%10u);
        payload->clock[2]=':';
        payload->clock[3]=(char)('0'+minute/10u);
        payload->clock[4]=(char)('0'+minute%10u);
        payload->clock[5]=':';
        payload->clock[6]=(char)('0'+second/10u);
        payload->clock[7]=(char)('0'+second%10u);
        payload->clock[8]=0;
        payload->fields[0].data.text=(ksn_schema_text){payload->clock,8};
    }else payload->fields[0].data.text=(ksn_schema_text){NULL,0};
    payload->valid=valid;
    r=ksn_source_pool_publish(&write,NULL);
    if(r==KSN_OK){
        s->published++;
        s->last_id=id;
        s->last_second=frames/SOUND_SAMPLE_RATE;
        s->last_valid=valid;
    }else (void)ksn_source_pool_cancel(&write);
    return r;
}
static void observe(int32_t id,uint32_t frames,bool active){
    output_service *s=atomic_load_explicit(&live,memory_order_acquire);
    if(!s)return;
    uint32_t second=frames/SOUND_SAMPLE_RATE;
    if(active&&s->last_valid&&s->last_id==id&&s->last_second==second)return;
    ksn_result r=publish(s,id,frames,active);
    if(!active&&r!=KSN_OK)
        atomic_store_explicit(&s->pending_end_id,id,memory_order_release);
}

JSValue pocket_av_output_source(JSContext *ctx,JSValueConst self,
                                int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    if(!service){
        static const ksn_slot_type types[]={KSN_SLOT_TEXT};
        output_service *s=calloc(1,sizeof(*s));
        if(!s)return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,
            "audio.outputSource","source allocation failed",true,
            POCKET_OUTCOME_NOT_APPLIED);
        atomic_init(&s->pending_end_id,0);
        ksn_result r=ksn_source_pool_init(&s->pool,s->payloads,sizeof(s->payloads),
                                           sizeof(s->payloads[0]));
        if(r==KSN_OK)r=ksn_source_pool_adapter_open(&s->adapter,&s->pool,types,1,
            offsetof(output_payload,fields),describe,allow,NULL,&s->provider);
        if(r==KSN_OK)ksn_source_registry_init(&s->registry);
        if(r==KSN_OK)r=ksn_source_register(&s->registry,&s->provider,&s->handle);
        if(r==KSN_OK)r=ksn_source_pool_adapter_registered(&s->adapter,s->handle);
        /* A complete invalid snapshot lets views bind before play(). The
         * audio task is not observing yet, so there is exactly one writer. */
        if(r==KSN_OK)r=publish(s,0,0,false);
        if(r!=KSN_OK){
            if(s->handle.generation)(void)ksn_source_unregister(&s->registry,s->handle);
            free(s);
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                "audio.outputSource","source registration failed",false,
                POCKET_OUTCOME_NOT_APPLIED);
        }
        service=s;
        atomic_store_explicit(&live,s,memory_order_release);
        sound_stream_set_observer(observe);
    }
    return pocket_kasane_source_capability(ctx,&service->registry,service->handle);
}

void pocket_av_output_source_service(int32_t current_stream_id){
    output_service *s=service;
    if(!s)return;
    int pending=atomic_load_explicit(&s->pending_end_id,memory_order_acquire);
    if(!pending)return;
    /* The output task may still be publishing this stream. Once the owner
     * has seen its completion, no producer writes this pool concurrently. */
    if(current_stream_id==pending)return;
    if(current_stream_id==0&&publish(s,pending,0,false)==KSN_OK)
        atomic_compare_exchange_strong_explicit(&s->pending_end_id,&pending,0,
            memory_order_acq_rel,memory_order_acquire);
    else if(current_stream_id!=0)
        atomic_compare_exchange_strong_explicit(&s->pending_end_id,&pending,0,
            memory_order_acq_rel,memory_order_acquire);
}

void pocket_av_output_source_suspend(void){
    sound_stream_set_observer(NULL);
}
void pocket_av_output_source_reset(bool audio_stopped){
    output_service *s=service;
    if(!s)return;
#ifdef KASANE_P0_PROBE
    if(audio_stopped)ESP_LOGI("KSN_OUTPUT_SOURCE","STOP published=%lu skipped=%lu audio_stopped=1",
                             (unsigned long)s->published,
                             (unsigned long)ksn_source_pool_skipped(&s->pool));
    else ESP_LOGE("KSN_OUTPUT_SOURCE","STOP audio task still active");
#endif
    service=NULL;
    if(ksn_source_unregister(&s->registry,s->handle)!=KSN_OK){
        ESP_LOGE("pocket.av","outputSource unregister failed; storage retained");
        return;
    }
    if(!audio_stopped){
        ESP_LOGE("pocket.av","outputSource audio task not stopped; storage retained");
        return;
    }
    atomic_store_explicit(&live,NULL,memory_order_release);
    free(s);
}
