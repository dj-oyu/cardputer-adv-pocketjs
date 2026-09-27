#include "pocket_av_output_source.h"
#include "pocket_api.h"
#include "pocket_kasane.h"
#include "sound.h"
#include "ui/kasane/ksn_source_pool_adapter.h"
#include "ui/kasane/ksn_p0_probe.h"
#include "pocket_mutex_arena.h"
#include "esp_log.h"
#ifdef KASANE_P0_PROBE
#include "esp_heap_caps.h"
#endif
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>

typedef struct {
    ksn_schema_value fields[4];
    char clock[9];
    uint32_t changed_fields;
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
#ifdef KASANE_P0_PROBE
    uint32_t max_published_frames,max_starved_blocks,valid_published;
#endif
#if defined(KASANE_P0_PROBE) && defined(KASANE_P0_COPY_PROBE)
    ksn_source_read probe_pins[2];
    uint32_t probe_pinned,probe_released;
    bool probe_hold;
#endif
    int32_t last_id;
    uint32_t last_second;
    bool last_valid,arena_active;
    uint32_t last_frames,last_starved,sample_ms;
} output_service;

/* The callback is read by the audio task only after sound_stream_set_observer
 * publishes it. This pointer stays live until the output task has stopped;
 * reset retains the allocation on a failed stop instead of risking UAF. */
static _Atomic(output_service *) live;
static output_service *service;
/* A stop/unregister failure cannot prove that every reader and callback has
 * returned. Keep that allocation for the rest of this boot, and reject a new
 * service instead of overwriting live while the old callback may still run. */
static output_service *retained;

#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
ksn_result pocket_av_output_source_existing(ksn_source_registry **registry,
                                             ksn_source_handle *handle){
    if(!registry||!handle)return KSN_INVALID;
    output_service *s=service;
    if(!s)return KSN_STALE;
    *registry=&s->registry;
    *handle=s->handle;
    return KSN_OK;
}
#endif

#ifdef KASANE_P0_PROBE
static void output_heap_probe(const char *phase){
    const uint32_t caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    ESP_LOGI("KSN_OUTPUT_SOURCE","HEAP phase=%s service_bytes=%u free=%u largest=%u min=%u",
        phase,(unsigned)sizeof(output_service),
        (unsigned)heap_caps_get_free_size(caps),
        (unsigned)heap_caps_get_largest_free_block(caps),
        (unsigned)heap_caps_get_minimum_free_size(caps));
}
#endif

static ksn_result describe(const void *data,ksn_source_pool_view *out){
    const output_payload *payload=data;
    if(!payload||!out)return KSN_INVALID;
    *out=(ksn_source_pool_view){.fields=payload->fields,
        .valid_fields=payload->valid?15u:0u,
        .changed_fields=payload->changed_fields};
    return KSN_OK;
}
static bool allow(void *policy,uint32_t consumer){
    (void)policy;return consumer!=0;
}
static ksn_result publish(output_service *s,int32_t id,uint32_t frames,
                          uint32_t starved_blocks,bool valid){
    ksn_source_write write={0};
    ksn_result r=ksn_source_pool_begin(&s->pool,&write);
    if(r!=KSN_OK)return r;
    output_payload *payload=write.data;
    if(valid){
        uint32_t seconds=frames/SOUND_SAMPLE_RATE;
        bool stream_changed=!s->last_valid||s->last_id!=id;
        bool text_changed=stream_changed||seconds!=s->last_second;
#ifdef KASANE_P1_VISIBLE_33MS_PROBE
        /* Diagnostic-only stress: force the subscribed text node to change
         * with the 33 ms producer instead of once per second. */
        if(s->sample_ms==33u&&frames!=s->last_frames)text_changed=true;
#endif
        payload->changed_fields=
            (text_changed?1u:0u)|
            (stream_changed||frames!=s->last_frames?2u:0u)|
            (stream_changed||starved_blocks!=s->last_starved?4u:0u)|
            (stream_changed?8u:0u);
#ifdef KASANE_P1_VISIBLE_33MS_PROBE
        if(s->sample_ms==33u){
            unsigned minute=seconds/60u%100u,second=seconds%60u;
            unsigned centisecond=(unsigned)(((uint64_t)(frames%SOUND_SAMPLE_RATE)*100u)/
                                          SOUND_SAMPLE_RATE);
            payload->clock[0]=(char)('0'+minute/10u);
            payload->clock[1]=(char)('0'+minute%10u);
            payload->clock[2]=':';
            payload->clock[3]=(char)('0'+second/10u);
            payload->clock[4]=(char)('0'+second%10u);
            payload->clock[5]=':';
            payload->clock[6]=(char)('0'+centisecond/10u);
            payload->clock[7]=(char)('0'+centisecond%10u);
        }else
#endif
        {
        unsigned hour=seconds/3600u,minute=seconds/60u%60u,second=seconds%60u;
        payload->clock[0]=(char)('0'+hour/10u);
        payload->clock[1]=(char)('0'+hour%10u);
        payload->clock[2]=':';
        payload->clock[3]=(char)('0'+minute/10u);
        payload->clock[4]=(char)('0'+minute%10u);
        payload->clock[5]=':';
        payload->clock[6]=(char)('0'+second/10u);
        payload->clock[7]=(char)('0'+second%10u);
        }
        payload->clock[8]=0;
        payload->fields[0].data.text=(ksn_schema_text){payload->clock,8};
        payload->fields[1].data.wide_number=frames;
        payload->fields[2].data.wide_number=starved_blocks;
        payload->fields[3].data.wide_number=(uint32_t)id;
    }else{
        payload->fields[0].data.text=(ksn_schema_text){NULL,0};
        payload->changed_fields=15u;
    }
    payload->valid=valid;
    r=ksn_source_pool_publish(&write,NULL);
    if(r==KSN_OK){
        s->published++;
#ifdef KASANE_P0_PROBE
        if(valid){
            s->valid_published++;
            if(frames>s->max_published_frames)s->max_published_frames=frames;
            if(starved_blocks>s->max_starved_blocks)s->max_starved_blocks=starved_blocks;
        }
#endif
        s->last_id=id;
        s->last_second=frames/SOUND_SAMPLE_RATE;
        s->last_frames=frames;
        s->last_starved=starved_blocks;
        s->last_valid=valid;
    }else (void)ksn_source_pool_cancel(&write);
    return r;
}
static void observe(int32_t id,uint32_t frames,uint32_t starved_blocks,bool active){
    output_service *s=atomic_load_explicit(&live,memory_order_acquire);
    if(!s)return;
#if defined(KASANE_P0_PROBE) && defined(KASANE_P0_COPY_PROBE)
    if(s->probe_hold&&frames/SOUND_SAMPLE_RATE>=8u){
        for(unsigned i=0;i<s->probe_pinned;i++)
            if(ksn_source_pool_release(&s->probe_pins[i])==KSN_OK)s->probe_released++;
        s->probe_hold=false;
    }
#endif
    if(active&&s->last_valid&&s->last_id==id&&s->last_frames==frames&&
       s->last_starved==starved_blocks)return;
    ksn_result r=publish(s,id,frames,starved_blocks,active);
#if defined(KASANE_P0_PROBE) && defined(KASANE_P0_COPY_PROBE)
    if(active&&r==KSN_OK&&s->probe_hold&&s->probe_pinned<2u&&
       ksn_source_pool_acquire(&s->pool,&s->probe_pins[s->probe_pinned])==KSN_OK)
        s->probe_pinned++;
#endif
    if(!active&&r!=KSN_OK)
        atomic_store_explicit(&s->pending_end_id,id,memory_order_release);
}

JSValue pocket_av_output_source(JSContext *ctx,JSValueConst self,
                                int argc,JSValueConst *argv){
    (void)self;
    if(retained)return pocket_api_throw(ctx,POCKET_ERR_BUSY,
        "audio.outputSource","a previous source is still retained",true,
        POCKET_OUTCOME_NOT_APPLIED);
    uint32_t sample_ms=1000;
    bool sample_specified=false;
    if(argc>0&&!JS_IsUndefined(argv[0])&&JS_IsObject(argv[0])&&
       !JS_IsArray(argv[0])){
        JSValue sample=JS_GetPropertyStr(ctx,argv[0],"sampleMs");
        if(JS_IsException(sample))return JS_EXCEPTION;
        if(!JS_IsUndefined(sample)){
            double number=0;
            bool valid=JS_IsNumber(sample)&&JS_ToFloat64(ctx,&number,sample)==0&&
                number>=32&&number<=1000&&(double)(uint32_t)number==number;
            JS_FreeValue(ctx,sample);
            if(!valid)return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                "audio.outputSource","sampleMs must be an integer from 32 to 1000",
                false,POCKET_OUTCOME_NOT_APPLIED);
            sample_ms=(uint32_t)number;
            sample_specified=true;
        }else JS_FreeValue(ctx,sample);
    }else if(argc>0&&!JS_IsUndefined(argv[0])
#if defined(KASANE_P0_PROBE) && defined(KASANE_P0_COPY_PROBE)
             &&!JS_IsBool(argv[0])
#endif
             )return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                "audio.outputSource","options must be an object",
                false,POCKET_OUTCOME_NOT_APPLIED);
#if defined(KASANE_P0_PROBE) && defined(KASANE_P0_COPY_PROBE)
    bool probe_hold=argc>0&&JS_IsBool(argv[0])&&JS_ToBool(ctx,argv[0]);
#else
    (void)argc;(void)argv;
#endif
    if(service&&sample_specified&&service->sample_ms!=sample_ms)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
            "audio.outputSource","sampleMs cannot change while the source is active",
            false,POCKET_OUTCOME_NOT_APPLIED);
    if(!service){
        static const ksn_slot_type types[]={KSN_SLOT_TEXT,KSN_SLOT_U32,
                                            KSN_SLOT_U32,KSN_SLOT_U32};
        /* Allocate early enough to keep later system-lifetime locks together.
         * If no lock uses the arena, reset returns its backing. */
        bool arena_active=pocket_mutex_arena_activate();
#ifdef KASANE_P0_PROBE
        output_heap_probe("before_alloc");
#endif
        output_service *s=calloc(1,sizeof(*s));
        if(!s){
            if(arena_active)pocket_mutex_arena_deactivate();
            return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,
                "audio.outputSource","source allocation failed",true,
                POCKET_OUTCOME_NOT_APPLIED);
        }
#ifdef KASANE_P0_PROBE
        output_heap_probe("after_alloc");
#endif
        atomic_init(&s->pending_end_id,0);
        s->arena_active=arena_active;
        s->sample_ms=sample_ms;
#if defined(KASANE_P0_PROBE) && defined(KASANE_P0_COPY_PROBE)
        s->probe_hold=probe_hold;
#endif
        ksn_result r=ksn_source_pool_init(&s->pool,s->payloads,sizeof(s->payloads),
                                           sizeof(s->payloads[0]));
        if(r==KSN_OK)r=ksn_source_pool_adapter_open(&s->adapter,&s->pool,types,4,
            offsetof(output_payload,fields),describe,allow,NULL,&s->provider);
        if(r==KSN_OK)ksn_source_registry_init(&s->registry);
        if(r==KSN_OK)r=ksn_source_register(&s->registry,&s->provider,&s->handle);
        if(r==KSN_OK)r=ksn_source_pool_adapter_registered(&s->adapter,s->handle);
        /* A complete invalid snapshot lets views bind before play(). The
         * audio task is not observing yet, so there is exactly one writer. */
        if(r==KSN_OK)r=publish(s,0,0,0,false);
        if(r!=KSN_OK){
            if(s->handle.generation)(void)ksn_source_unregister(&s->registry,s->handle);
            free(s);
            if(arena_active)pocket_mutex_arena_deactivate();
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                "audio.outputSource","source registration failed",false,
                POCKET_OUTCOME_NOT_APPLIED);
        }
        service=s;
#if defined(KASANE_P0_PROBE) && defined(KASANE_P0_COPY_PROBE)
        for(unsigned i=0;i<KSN_SOURCE_POOL_SLOTS;i++)
            if(!ksn_p0_probe_watch_source_text(s->payloads[i].clock,8u))
                ESP_LOGE("KSN_OUTPUT_SOURCE","source text watch registration failed slot=%u",i);
#endif
        atomic_store_explicit(&live,s,memory_order_release);
        uint32_t interval_frames=(uint32_t)(((uint64_t)sample_ms*
            SOUND_SAMPLE_RATE+500u)/1000u);
        sound_stream_set_observer_interval(observe,interval_frames);
#ifdef KASANE_P0_PROBE
        output_heap_probe("after_register");
#endif
    }
    JSValue capability=pocket_kasane_source_capability(ctx,&service->registry,
                                                       service->handle);
#ifdef KASANE_P0_PROBE
    output_heap_probe("after_capability");
#endif
    return capability;
}

void pocket_av_output_source_service(int32_t current_stream_id){
    output_service *s=service;
    if(!s)return;
    int pending=atomic_load_explicit(&s->pending_end_id,memory_order_acquire);
    if(!pending)return;
    /* The output task may still be publishing this stream. Once the owner
     * has seen its completion, no producer writes this pool concurrently. */
    if(current_stream_id==pending)return;
    if(current_stream_id==0&&publish(s,pending,0,0,false)==KSN_OK)
        atomic_compare_exchange_strong_explicit(&s->pending_end_id,&pending,0,
            memory_order_acq_rel,memory_order_acquire);
    else if(current_stream_id!=0)
        atomic_compare_exchange_strong_explicit(&s->pending_end_id,&pending,0,
            memory_order_acq_rel,memory_order_acquire);
}

void pocket_av_output_source_suspend(void){
    sound_stream_set_observer(NULL);
}
bool pocket_av_output_source_reset(bool audio_stopped){
    output_service *s=service;
    if(!s)return retained==NULL;
#ifdef KASANE_P0_PROBE
    output_heap_probe("before_reset");
#endif
#ifdef KASANE_P0_PROBE
    if(audio_stopped)ESP_LOGI("KSN_OUTPUT_SOURCE",
                             "STOP published=%lu valid_published=%lu skipped=%lu audio_stopped=1 max_frames=%lu max_starved=%lu",
                             (unsigned long)s->published,
                             (unsigned long)s->valid_published,
                             (unsigned long)ksn_source_pool_skipped(&s->pool),
                             (unsigned long)s->max_published_frames,
                             (unsigned long)s->max_starved_blocks);
    else ESP_LOGE("KSN_OUTPUT_SOURCE","STOP audio task still active");
#endif
#if defined(KASANE_P0_PROBE) && defined(KASANE_P0_COPY_PROBE)
    if(audio_stopped){
        for(unsigned i=0;i<s->probe_pinned;i++)
            if(s->probe_pins[i].active&&
               ksn_source_pool_release(&s->probe_pins[i])==KSN_OK)s->probe_released++;
        ESP_LOGI("KSN_OUTPUT_SOURCE","PIN_PROBE pinned=%lu released=%lu",
            (unsigned long)s->probe_pinned,(unsigned long)s->probe_released);
    }
#endif
    service=NULL;
    if(ksn_source_unregister(&s->registry,s->handle)!=KSN_OK){
        ESP_LOGE("pocket.av","outputSource unregister failed; storage retained");
        retained=s;
        return false;
    }
    if(!audio_stopped){
        ESP_LOGE("pocket.av","outputSource audio task not stopped; storage retained");
        retained=s;
        return false;
    }
    atomic_store_explicit(&live,NULL,memory_order_release);
#if defined(KASANE_P0_PROBE) && defined(KASANE_P0_COPY_PROBE)
    for(unsigned i=0;i<KSN_SOURCE_POOL_SLOTS;i++)
        ksn_p0_probe_unwatch_source_text(s->payloads[i].clock);
#endif
    bool arena_active=s->arena_active;
    free(s);
    if(arena_active)pocket_mutex_arena_deactivate();
#ifdef KASANE_P0_PROBE
    output_heap_probe("after_free");
#endif
    return true;
}
