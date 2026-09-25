#include "pocket_av_playback_source.h"
#include "pocket_api.h"
#include "pocket_av.h"
#include "pocket_kasane.h"
#include "esp_log.h"
#include <stdlib.h>

typedef struct {
    ksn_source_registry registry;
    ksn_source_provider provider;
    ksn_source_handle handle;
    ksn_schema_value fields[6];
    pocket_av_ui_snapshot last;
    uint64_t revision;
    uint32_t pins,skipped;
    int32_t last_player_id;
    bool valid;
#ifdef KASANE_P0_PROBE
    uint32_t published,valid_published,max_position_ms;
#endif
} playback_service;

static playback_service *service;
/* A lease from the previous APP may outlive detach. Keep its provider until
 * the last pin returns, but do not hand that generation to a new session. */
static playback_service *retained;
static const ksn_slot_type field_types[]={
    KSN_SLOT_U16,KSN_SLOT_U32,KSN_SLOT_U32,KSN_SLOT_U32,KSN_SLOT_BOOL,
    KSN_SLOT_U32
};

static ksn_result acquire(void *context,uint64_t cursor,uint64_t now_us,
                          ksn_source_snapshot *out){
    (void)cursor;(void)now_us;
    playback_service *s=context;
    if(!s||!out||s->pins==UINT32_MAX)return KSN_BUSY;
    s->pins++;
    *out=(ksn_source_snapshot){.size=sizeof(*out),.version=KSN_SOURCE_ABI_VERSION,
        .field_count=6,.generation=s->handle.generation,.revision=s->revision,
        .valid_fields=s->valid?63u:0u,.changed_fields=63u,.fields=s->fields};
    return KSN_OK;
}
static void release(void *context,const ksn_source_snapshot *snapshot){
    (void)snapshot;
    playback_service *s=context;
    if(s&&s->pins)s->pins--;
}
static bool allow(void *policy,uint32_t consumer){
    (void)policy;return consumer!=0;
}
static ksn_result publish(playback_service *s,int32_t player_id,
                          const pocket_av_ui_snapshot *snapshot){
    /* Both producer and presenter run on the owner task. Never mutate the
     * borrowed fields under a reentrant lease; retry on the next UI turn. */
    if(s->pins){s->skipped++;return KSN_BUSY;}
    if(s->revision==UINT64_MAX)return KSN_LIMIT;
    if(snapshot){
        s->fields[0].data.number=(uint16_t)snapshot->state;
        s->fields[1].data.wide_number=snapshot->position_ms;
        s->fields[2].data.wide_number=snapshot->duration_ms;
        s->fields[3].data.wide_number=snapshot->underruns;
        s->fields[4].data.boolean=snapshot->state==POCKET_AV_UI_PLAYING;
        s->fields[5].data.wide_number=(uint32_t)player_id;
    }
    s->valid=snapshot!=NULL;
    s->revision++;
    s->last_player_id=player_id;
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

ksn_result pocket_av_playback_source_open(ksn_source_registry **registry,
                                           ksn_source_handle *handle){
    if(!registry||!handle)return KSN_INVALID;
    if(retained){
        if(ksn_source_unregister(&retained->registry,retained->handle)!=KSN_OK)
            return KSN_BUSY;
        free(retained);
        retained=NULL;
    }
    if(!service){
        playback_service *s=calloc(1,sizeof(*s));
        if(!s)return KSN_OOM;
        s->provider=(ksn_source_provider){.size=sizeof(s->provider),
            .version=KSN_SOURCE_ABI_VERSION,.field_count=6,
            .field_types=field_types,.context=s,.acquire=acquire,
            .release=release,.allow=allow};
        ksn_source_registry_init(&s->registry);
        ksn_result r=KSN_OK;
        if(r==KSN_OK)r=ksn_source_register(&s->registry,&s->provider,&s->handle);
        if(r==KSN_OK)r=publish(s,0,NULL);
        if(r!=KSN_OK){
            if(s->handle.generation)
                (void)ksn_source_unregister(&s->registry,s->handle);
            free(s);
            return r;
        }
        service=s;
        pocket_av_playback_source_service();
    }
    *registry=&service->registry;
    *handle=service->handle;
    return KSN_OK;
}

const uint64_t *pocket_av_playback_source_revision_ref(ksn_source_handle handle){
    playback_service *s=service;
    return s&&handle.index==s->handle.index&&
           handle.generation==s->handle.generation?&s->revision:NULL;
}

JSValue pocket_av_playback_source(JSContext *ctx,JSValueConst self,
                                  int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    ksn_source_registry *registry=NULL;
    ksn_source_handle handle={0};
    ksn_result r=pocket_av_playback_source_open(&registry,&handle);
    if(r!=KSN_OK)return pocket_api_throw(ctx,r==KSN_OOM?POCKET_ERR_OUT_OF_MEMORY:
        r==KSN_BUSY?POCKET_ERR_BUSY:POCKET_ERR_INVALID_ARGUMENT,
        "audio.playbackSource",
        r==KSN_OOM?"source allocation failed":r==KSN_BUSY?
        "a previous source still has readers":"source registration failed",
        r==KSN_OOM||r==KSN_BUSY,POCKET_OUTCOME_NOT_APPLIED);
    return pocket_kasane_source_capability(ctx,registry,handle);
}

void pocket_av_playback_source_service(void){
    playback_service *s=service;
    if(!s)return;
    int32_t id=pocket_av_ui_current_player();
    pocket_av_ui_snapshot snapshot={0};
    bool valid=id&&pocket_av_ui_read(id,&snapshot);
    if(!valid){
        if(s->valid)(void)publish(s,0,NULL);
        return;
    }
    /* Bound the title/progress projection's lag to roughly one LCD pixel
     * without requiring the producer to know the music viewport. Unknown
     * duration still needs a fresh status second, not a per-frame publish. */
    uint32_t quantum=snapshot.duration_ms?snapshot.duration_ms/240u:1000u;
    if(!quantum)quantum=1u;
    if(quantum>1000u)quantum=1000u;
    if(s->valid&&s->last_player_id==id&&
       s->last.state==snapshot.state&&
       s->last.position_ms/quantum==snapshot.position_ms/quantum&&
       s->last.position_ms/1000u==snapshot.position_ms/1000u&&
       s->last.duration_ms==snapshot.duration_ms&&
       s->last.underruns==snapshot.underruns)return;
    (void)publish(s,id,&snapshot);
}

void pocket_av_playback_source_reset(void){
    playback_service *s=service;
    if(!s){
        if(retained&&ksn_source_unregister(&retained->registry,retained->handle)==KSN_OK){
            free(retained);
            retained=NULL;
        }
        return;
    }
#ifdef KASANE_P0_PROBE
    ksn_source_borrow_probe_report();
    ESP_LOGI("KSN_PLAYBACK_SOURCE","STOP published=%lu valid=%lu skipped=%lu max_position_ms=%lu",
        (unsigned long)s->published,(unsigned long)s->valid_published,
        (unsigned long)s->skipped,
        (unsigned long)s->max_position_ms);
#endif
    service=NULL;
    if(ksn_source_unregister(&s->registry,s->handle)!=KSN_OK){
        ESP_LOGE("pocket.av","playbackSource unregister failed; storage retained");
        retained=s;
        return;
    }
    free(s);
}
