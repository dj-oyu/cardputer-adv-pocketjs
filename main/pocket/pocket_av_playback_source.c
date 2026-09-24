#include "pocket_av_playback_source.h"
#include "pocket_api.h"
#include "pocket_av.h"
#include "pocket_kasane.h"
#include "ui/kasane/ksn_source_pool_adapter.h"
#include "esp_log.h"
#include <stddef.h>
#include <stdlib.h>

typedef struct {
    ksn_schema_value fields[5];
    bool valid;
} playback_payload;
typedef struct {
    ksn_source_registry registry;
    ksn_source_provider provider;
    ksn_source_pool_adapter adapter;
    ksn_source_pool pool;
    ksn_source_handle handle;
    playback_payload payloads[KSN_SOURCE_POOL_SLOTS];
    pocket_av_ui_snapshot last;
    int32_t last_player_id;
    bool last_valid;
#ifdef KASANE_P0_PROBE
    uint32_t published,valid_published,max_position_ms;
#endif
} playback_service;

static playback_service *service;
static const ksn_slot_type field_types[]={
    KSN_SLOT_U16,KSN_SLOT_U32,KSN_SLOT_U32,KSN_SLOT_U32,KSN_SLOT_BOOL
};

static ksn_result describe(const void *data,ksn_source_pool_view *out){
    const playback_payload *payload=data;
    if(!payload||!out)return KSN_INVALID;
    *out=(ksn_source_pool_view){.fields=payload->fields,
        .valid_fields=payload->valid?31u:0u,.changed_fields=31u};
    return KSN_OK;
}
static bool allow(void *policy,uint32_t consumer){
    (void)policy;return consumer!=0;
}
static ksn_result publish(playback_service *s,int32_t player_id,
                          const pocket_av_ui_snapshot *snapshot){
    ksn_source_write write={0};
    ksn_result r=ksn_source_pool_begin(&s->pool,&write);
    if(r!=KSN_OK)return r;
    playback_payload *payload=write.data;
    payload->valid=snapshot!=NULL;
    if(snapshot){
        payload->fields[0].data.number=(uint16_t)snapshot->state;
        payload->fields[1].data.wide_number=snapshot->position_ms;
        payload->fields[2].data.wide_number=snapshot->duration_ms;
        payload->fields[3].data.wide_number=snapshot->underruns;
        payload->fields[4].data.boolean=snapshot->state==POCKET_AV_UI_PLAYING;
    }
    r=ksn_source_pool_publish(&write,NULL);
    if(r!=KSN_OK){(void)ksn_source_pool_cancel(&write);return r;}
    s->last_player_id=player_id;
    s->last_valid=snapshot!=NULL;
    if(snapshot)s->last=*snapshot;
#ifdef KASANE_P0_PROBE
    s->published++;
    if(snapshot){
        s->valid_published++;
        if(snapshot->position_ms>s->max_position_ms)
            s->max_position_ms=snapshot->position_ms;
    }
#endif
    return KSN_OK;
}

JSValue pocket_av_playback_source(JSContext *ctx,JSValueConst self,
                                  int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    if(!service){
        playback_service *s=calloc(1,sizeof(*s));
        if(!s)return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,
            "audio.playbackSource","source allocation failed",true,
            POCKET_OUTCOME_NOT_APPLIED);
        ksn_result r=ksn_source_pool_init(&s->pool,s->payloads,sizeof(s->payloads),
                                           sizeof(s->payloads[0]));
        if(r==KSN_OK)r=ksn_source_pool_adapter_open(&s->adapter,&s->pool,
            field_types,5,offsetof(playback_payload,fields),describe,allow,NULL,
            &s->provider);
        if(r==KSN_OK)ksn_source_registry_init(&s->registry);
        if(r==KSN_OK)r=ksn_source_register(&s->registry,&s->provider,&s->handle);
        if(r==KSN_OK)r=ksn_source_pool_adapter_registered(&s->adapter,s->handle);
        if(r==KSN_OK)r=publish(s,0,NULL);
        if(r!=KSN_OK){
            if(s->handle.generation)
                (void)ksn_source_unregister(&s->registry,s->handle);
            free(s);
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                "audio.playbackSource","source registration failed",false,
                POCKET_OUTCOME_NOT_APPLIED);
        }
        service=s;
    }
    return pocket_kasane_source_capability(ctx,&service->registry,service->handle);
}

void pocket_av_playback_source_service(void){
    playback_service *s=service;
    if(!s)return;
    int32_t id=pocket_av_ui_current_player();
    pocket_av_ui_snapshot snapshot={0};
    bool valid=id&&pocket_av_ui_read(id,&snapshot);
    if(!valid){
        if(s->last_valid)(void)publish(s,0,NULL);
        return;
    }
    if(s->last_valid&&s->last_player_id==id&&
       s->last.state==snapshot.state&&
       s->last.position_ms/1000u==snapshot.position_ms/1000u&&
       s->last.duration_ms==snapshot.duration_ms&&
       s->last.underruns==snapshot.underruns)return;
    (void)publish(s,id,&snapshot);
}

void pocket_av_playback_source_reset(void){
    playback_service *s=service;
    if(!s)return;
#ifdef KASANE_P0_PROBE
    ESP_LOGI("KSN_PLAYBACK_SOURCE","STOP published=%lu valid=%lu skipped=%lu max_position_ms=%lu",
        (unsigned long)s->published,(unsigned long)s->valid_published,
        (unsigned long)ksn_source_pool_skipped(&s->pool),
        (unsigned long)s->max_position_ms);
#endif
    service=NULL;
    if(ksn_source_unregister(&s->registry,s->handle)!=KSN_OK){
        ESP_LOGE("pocket.av","playbackSource unregister failed; storage retained");
        return;
    }
    free(s);
}
