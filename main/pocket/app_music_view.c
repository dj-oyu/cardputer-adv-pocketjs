#include "app_music_view.h"
#include "app_legacy_presenter.h"
#include "pocket_api.h"
#include "pocket_av.h"
#include "pocket_av_playback_source.h"
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
#include "pocket_av_output_source.h"
#include "sound.h"
#endif
#include "ui/kasane/ksn_p0_probe.h"
#ifdef KASANE_P0_PROBE
#include "esp_log.h"
#include "esp_timer.h"
#endif
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
#include "esp_heap_caps.h"
#ifdef KASANE_P1_OVERLAY_STAGE_PROBE
#define P1_STAGE_START(name) int64_t name=esp_timer_get_time()
#define P1_STAGE_END(kind,name) ksn_p0_probe_sample(kind,(uint32_t)(esp_timer_get_time()-name))
#else
#define P1_STAGE_START(name) ((void)0)
#define P1_STAGE_END(kind,name) ((void)0)
#endif
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUSIC_INPUT_UNITS 256u
typedef struct {
    uint64_t key[12];
    ksn_presenter_values owned;
    ksn_ref refs[KSN_PRESENTER_ITEMS];
    uint32_t topology[KSN_PRESENTER_ITEMS];
    uint8_t light_ref_index[3];
    char system_status[KSN_PRESENTER_TEXT_MAX+1u];
    ksn_tx ticket;
    ksn_rgba topology_background;
    uint64_t system_until_us,title_revision,message_revision;
    uint64_t system_revision,manual_revision;
    uint32_t handle;
    int32_t playback_id;
    ksn_source_registry *playback_registry;
    const uint64_t *playback_revision;
    ksn_source_subscription playback_subscription;
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    ksn_source_registry *output_registry;
    ksn_source_subscription *output_subscription;
    uint32_t output_probe_patch_calls,output_probe_replace_calls;
    uint32_t output_probe_light_calls,output_probe_fast_calls;
#endif
    pocket_av_ui_snapshot audio_cache;
    uint64_t audio_revision;
#ifdef KASANE_P0_PROBE
    uint64_t read_total_us,read_cache_us,read_borrow_us;
    uint32_t read_calls,read_max_us;
    uint32_t read_cache_calls,read_cache_max_us,read_borrow_calls,read_borrow_max_us;
#endif
    uint8_t system_bytes,topology_count;
    bool have_key,have_refs,manual,reactive,help,audio_live;
} music_state;
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
_Static_assert(sizeof(music_state)<=1536,"Diagnostic music view allocation budget");
#else
_Static_assert(sizeof(music_state)<=1280,"Music view allocation budget");
#endif

static music_state *active;
static pocket_app_view_host current_host;
static uint32_t music_serial;
static JSClassID music_class;
static JSRuntime *music_rt;
static const JSClassDef music_def={.class_name="Kasane"};
#if defined(KASANE_P0_PROBE) && !defined(KASANE_P1_OUTPUT_OVERLAY_PROBE)
static uint32_t p0_time_light_patch,p0_time_light_same,p0_time_light_fallback;
#endif
#ifdef KASANE_P0_PROBE
void pocket_app_music_view_probe_reset_counters(void){
    music_state *p=active;
    if(!p)return;
    p->read_total_us=p->read_cache_us=p->read_borrow_us=0;
    p->read_calls=p->read_max_us=0;
    p->read_cache_calls=p->read_cache_max_us=0;
    p->read_borrow_calls=p->read_borrow_max_us=0;
}
#endif

static ksn_view *music_view(void){
    return current_host.view;
}
static ksn_rect music_viewport(void){
    return current_host.viewport;
}
static uint64_t music_now_us(void){
    return *current_host.now_us;
}
static bool music_busy(void){
    return current_host.busy(current_host.owner);
}
static void music_submitted(ksn_tx ticket,ksn_update_mode mode){
    current_host.submitted(current_host.owner,ticket,mode);
}

static const char *music_result_code(ksn_result result) {
    switch(result) {
    case KSN_OK: return "OK";
    case KSN_INVALID: return POCKET_ERR_INVALID_ARGUMENT;
    case KSN_LIMIT: return POCKET_ERR_LIMIT_EXCEEDED;
    case KSN_OOM: return POCKET_ERR_OUT_OF_MEMORY;
    case KSN_STALE: return POCKET_ERR_CLOSED;
    case KSN_BUSY: return POCKET_ERR_BUSY;
    case KSN_UNSUPPORTED: return POCKET_ERR_UNSUPPORTED;
    case KSN_IO: return POCKET_ERR_IO_ERROR;
    case KSN_CANCELLED: return POCKET_ERR_CANCELLED;
    }
    return POCKET_ERR_CONFLICT;
}

static JSValue music_throw_result(JSContext *ctx, ksn_result result, const char *op) {
    const char *code=music_result_code(result);
    return pocket_api_throw(ctx,code,op,code,
                            result==KSN_BUSY||result==KSN_OOM||result==KSN_IO,
                            POCKET_OUTCOME_NOT_APPLIED);
}

static bool music_number_in(JSContext *ctx, JSValueConst value, double lo, double hi,
                      double *out) {
    double n;
    if(!JS_IsNumber(value)||JS_ToFloat64(ctx,&n,value)<0||!isfinite(n)||
       floor(n)!=n||n<lo||n>hi) return false;
    *out=n;
    return true;
}


static bool presenter_text_equal(const char *a,uint8_t a_bytes,
                                 const char *b,uint8_t b_bytes){
    return a_bytes==b_bytes&&memcmp(a,b,a_bytes)==0;
}
static bool presenter_values_equal(const ksn_presenter_values *a,
                                   const ksn_presenter_values *b){
    for(unsigned i=0;i<KSN_PRESENTER_SLOTS;i++)
        if(!presenter_text_equal(a->text[i],a->bytes[i],b->text[i],b->bytes[i]))return false;
    return a->position_ms==b->position_ms&&a->duration_ms==b->duration_ms&&
           a->phase==b->phase&&a->playing==b->playing&&a->help==b->help;
}
static uint64_t presenter_viewport_key(void){
    return (uint16_t)music_viewport().x0|((uint64_t)(uint16_t)music_viewport().y0<<16)|
           ((uint64_t)(uint16_t)music_viewport().x1<<32)|((uint64_t)(uint16_t)music_viewport().y1<<48);
}
static uint64_t presenter_light_key(uint32_t phase,int track){
    return ksn_presenter_music_light_key(phase,(uint16_t)track);
}
static uint32_t presenter_shape(const ksn_presenter_item *item){
    return item->kind|((uint32_t)item->text_id<<8)|
           ((uint32_t)item->font<<16)|((uint32_t)item->radius<<24);
}
static bool presenter_can_patch(const music_state *p,const ksn_presenter_plan *plan){
    if(!plan->patchable||!p->have_refs||p->topology_count!=plan->count||
       p->topology_background!=plan->background)return false;
    for(unsigned i=0;i<plan->count;i++)
        if(p->topology[i]!=presenter_shape(&plan->items[i]))return false;
    return true;
}
static bool music_fast_light_eligible(const music_state *p){
    return p->have_key&&p->have_refs&&!p->manual&&p->reactive&&!p->help&&
           p->audio_live&&p->audio_cache.state==POCKET_AV_UI_PLAYING&&
           !p->audio_cache.duration_ms&&p->playback_revision&&
           *p->playback_revision&&*p->playback_revision==p->audio_revision&&
           p->playback_subscription.has_validated&&
           p->playback_subscription.validated_revision==p->audio_revision&&
           !p->owned.bytes[1]&&
           !(p->system_bytes&&music_now_us()<p->system_until_us)&&
           p->key[0]==(uint64_t)(1u<<9)&&
           p->key[1]==presenter_viewport_key()&&p->key[2]==0&&
           p->key[3]==p->title_revision&&p->key[4]==3&&
           p->key[5]==POCKET_AV_UI_PLAYING&&
           p->key[6]==p->audio_cache.position_ms/1000u&&p->key[7]==0&&
           p->key[9]==2&&p->key[11]==0;
}
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
static bool music_light_only_change(const music_state *p,const uint64_t key[12]){
    if(!p->have_key||p->key[9]!=2u||key[9]!=2u||p->key[10]==key[10])return false;
    for(unsigned i=0;i<12;i++)if(i!=10&&p->key[i]!=key[i])return false;
    return true;
}
static ksn_result music_light_patch(const ksn_presenter_plan *plan,
                                     const ksn_ref refs[KSN_PRESENTER_ITEMS],
                                     uint64_t old_lights,uint64_t new_lights,
                                     ksn_tx *out){
    static const ksn_rgba colors[]={0x78c8ffffu,0x3c78aaffu,0x1e3c5affu};
    if(!plan||!refs||!out)return KSN_INVALID;
    ksn_tx tx;
    ksn_result r=ksn_view_begin(music_view(),KSN_PATCH,&tx);
    if(r!=KSN_OK)return r;
    unsigned changed=0;
    for(unsigned i=0;i<plan->count&&r==KSN_OK;i++){
        const ksn_presenter_item *it=&plan->items[i];
        if(it->kind!=KSN_RECT)continue;
        for(unsigned light=0;light<3;light++){
            if(it->color!=colors[light])continue;
            if(((old_lights>>(light*16u))&0xffffu)==
               ((new_lights>>(light*16u))&0xffffu))break;
            ksn_rect bounds={it->bounds.x0+music_viewport().x0,
                             it->bounds.y0+music_viewport().y0,
                             it->bounds.x1+music_viewport().x0,
                             it->bounds.y1+music_viewport().y0};
            ksn_change rect={.property=KSN_SET_RECT,.value.rect=bounds};
            r=ksn_view_change(music_view(),tx,refs[i],&rect);
            changed++;
            break;
        }
    }
    if(r==KSN_OK&&changed)r=ksn_view_submit(music_view(),tx);
    if(r!=KSN_OK||!changed){(void)ksn_view_cancel(music_view(),tx);return r==KSN_OK?KSN_INVALID:r;}
    *out=tx;return KSN_OK;
}
#endif

/* The music-specific projection is a consumer of the same typed capability
 * available to an arbitrary mount. No AV-owned pointer crosses the source
 * lease, and a replaced player cannot satisfy an older binding. */
static const ksn_schema_slot playback_slots[]={
    {.name="state",.type=KSN_SLOT_U16},
    {.name="positionMs",.type=KSN_SLOT_U32},
    {.name="durationMs",.type=KSN_SLOT_U32},
    {.name="underruns",.type=KSN_SLOT_U32},
    {.name="playing",.type=KSN_SLOT_BOOL},
    {.name="playerId",.type=KSN_SLOT_U32}
};
static const ksn_schema playback_schema={.version=KSN_SCHEMA_ABI_VERSION,
    .slot_count=6,.background=0x000000ffu,.slots=playback_slots};
static const ksn_source_binding playback_bindings[]={
    {0,0},{1,1},{2,2},{3,3},{4,4},{5,5}
};
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
static const ksn_schema_slot output_slots[]={
    {.name="elapsed",.type=KSN_SLOT_TEXT,.capacity=8},
    {.name="frames",.type=KSN_SLOT_U32}
};
static const ksn_schema output_schema={.version=KSN_SCHEMA_ABI_VERSION,
    .slot_count=2,.background=0x000000ffu,.slots=output_slots};
static const ksn_source_binding output_bindings[]={{0,0},{1,1}};
static ksn_result music_output_borrow(music_state *p,ksn_source_lease *lease,
                                       bool *valid,uint32_t *frames){
    *valid=false;
    *frames=0;
    if(!p->output_registry||!p->output_subscription)return KSN_OK;
    ksn_result r=ksn_source_borrow_identity(p->output_registry,
        p->output_subscription,music_now_us(),lease);
    if(r!=KSN_OK)return r;
    if((lease->valid_slots&3u)==3u){
        *frames=lease->snapshot.fields[1].data.wide_number;
        *valid=true;
    }
    return KSN_OK;
}
static void music_output_finish(ksn_source_lease *lease,bool accepted){
    if(lease->active&&accepted)(void)ksn_source_commit(lease);
    ksn_source_release(lease);
}
/* Diagnostic fast path: source updates only the existing light rectangles.
 * It is eligible solely while all other inputs and the visible topology are
 * unchanged. A topology edge falls back to the full presenter below. */
static ksn_result music_output_fast_refresh(music_state *p,bool *handled){
    *handled=false;
    if(!p->output_subscription||!music_fast_light_eligible(p))return KSN_OK;
    P1_STAGE_START(started);
    ksn_source_lease lease={0};
    bool valid=false;
    uint32_t frames=0;
    ksn_result r=music_output_borrow(p,&lease,&valid,&frames);
    if(r!=KSN_OK){*handled=true;return r;}
    if(!valid){music_output_finish(&lease,false);return KSN_OK;}
    int width=music_viewport().x1-music_viewport().x0;
    uint64_t lights=presenter_light_key(frames/(SOUND_SAMPLE_RATE/30u),width-24);
    if(!lights){music_output_finish(&lease,false);return KSN_OK;}
    *handled=true;
    if(lights==p->key[10]){
        music_output_finish(&lease,true);
        P1_STAGE_END(KSN_P1_MUSIC_FAST,started);
        return KSN_OK;
    }
    if(music_busy()){music_output_finish(&lease,false);return KSN_BUSY;}
    ksn_tx ticket;
    r=ksn_presenter_music_light_patch(music_view(),music_viewport(),p->refs,
        p->light_ref_index,p->topology_count,p->key[10],lights,&ticket);
    if(r==KSN_UNSUPPORTED){
        *handled=false;
        music_output_finish(&lease,false);
        return KSN_OK;
    }
    if(r!=KSN_OK){
        music_output_finish(&lease,false);
        return r;
    }
    music_output_finish(&lease,true);
    p->key[10]=lights;
    p->have_refs=false;
    p->ticket=ticket;
    p->output_probe_fast_calls++;
    music_submitted(ticket,KSN_PATCH);
    P1_STAGE_END(KSN_P1_MUSIC_FAST,started);
    return KSN_OK;
}
#else
/* The production player keeps its event-driven JS contract. When only the
 * unknown-duration light advances, use the same three-rectangle PATCH as the
 * source-driven diagnostic without rebuilding the entire music plan. */
static ksn_result music_time_fast_refresh(music_state *p,bool *handled){
    *handled=false;
    if(!music_fast_light_eligible(p))return KSN_OK;
    int width=music_viewport().x1-music_viewport().x0;
    uint64_t lights=presenter_light_key((uint32_t)(music_now_us()/66667u),width-24);
    if(!lights)return KSN_OK;
    *handled=true;
    if(lights==p->key[10]){
#ifdef KASANE_P0_PROBE
        p0_time_light_same++;
#endif
        return KSN_OK;
    }
    if(music_busy())return KSN_BUSY;
    ksn_tx ticket;
    ksn_result r=ksn_presenter_music_light_patch(music_view(),music_viewport(),p->refs,
        p->light_ref_index,p->topology_count,p->key[10],lights,&ticket);
    if(r==KSN_UNSUPPORTED){
#ifdef KASANE_P0_PROBE
        p0_time_light_fallback++;
#endif
        *handled=false;return KSN_OK;
    }
    if(r!=KSN_OK)return r;
#ifdef KASANE_P0_PROBE
    p0_time_light_patch++;
#endif
    p->key[10]=lights;
    p->have_refs=false;
    p->ticket=ticket;
    music_submitted(ticket,KSN_PATCH);
    return KSN_OK;
}
#endif
static ksn_result music_audio_read(music_state *p,
                                   const pocket_av_ui_snapshot **audio,
                                   bool *live,ksn_source_lease *lease){
    *live=false;
    *audio=NULL;
    if(!p->playback_registry||!p->playback_subscription.binding_count)return KSN_OK;
    ksn_result r=ksn_source_borrow_identity(p->playback_registry,
        &p->playback_subscription,music_now_us(),lease);
    if(r!=KSN_OK)return r;
    const ksn_schema_value *fields=lease->snapshot.fields;
    if((lease->valid_slots&63u)==63u&&
       fields[5].data.wide_number==(uint32_t)p->playback_id){
        if(fields[0].data.number>POCKET_AV_UI_ERROR){
            ksn_source_release(lease);return KSN_INVALID;
        }
        p->audio_cache=(pocket_av_ui_snapshot){
            .state=(pocket_av_ui_state)fields[0].data.number,
            .position_ms=fields[1].data.wide_number,
            .duration_ms=fields[2].data.wide_number,
            .underruns=fields[3].data.wide_number
        };
        ksn_p0_probe_copy(KSN_P0_PRODUCER_MATERIALIZED,sizeof(p->audio_cache));
        *live=true;
        *audio=&p->audio_cache;
    }
    p->audio_live=*live;
    p->audio_revision=lease->snapshot.revision;
    return KSN_OK;
}

/* Borrow source facts only within this owner turn. Compare the visible
 * projection before constructing a plan. The command bank owns submitted text;
 * pending work never points into a mutable source. */
static ksn_result music_refresh(void) {
    if(!active)return KSN_OK;
    music_state *p=active;
    if((!p->reactive&&!p->manual)||p->ticket.value)return KSN_OK;
    bool fast_handled=false;
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    ksn_result fast_result=music_output_fast_refresh(p,&fast_handled);
#else
    ksn_result fast_result=music_time_fast_refresh(p,&fast_handled);
#endif
    if(fast_handled||fast_result!=KSN_OK)return fast_result;
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    P1_STAGE_START(refresh_started);
#endif
    int width=music_viewport().x1-music_viewport().x0,height=music_viewport().y1-music_viewport().y0;
    if((width<25||height<28))return KSN_INVALID;
    uint64_t key[12]={0};
    key[0]=((uint64_t)p->manual<<8)|((uint64_t)p->reactive<<9);
    key[1]=presenter_viewport_key();
    const pocket_av_ui_snapshot *audio=NULL;
    ksn_source_lease source_lease={0};
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    ksn_source_lease output_lease={0};
    uint32_t output_frames=0;
    bool output_live=false;
#endif
    bool live=false,system_status=false;
    if(p->manual)key[2]=p->manual_revision;
    else{
        key[2]=p->help?1u:0u;
        if(!p->help){
#ifdef KASANE_P0_PROBE
            int64_t read_started=esp_timer_get_time();
            bool read_cache=false,read_borrow=false;
#endif
            uint64_t revision=p->playback_revision?*p->playback_revision:0;
            if(revision&&revision==p->audio_revision&&
               p->playback_subscription.has_validated&&
               revision==p->playback_subscription.validated_revision){
#ifdef KASANE_P0_PROBE
                read_cache=true;
#endif
                live=p->audio_live;
                if(live)audio=&p->audio_cache;
            }else{
#ifdef KASANE_P0_PROBE
                read_borrow=true;
#endif
                ksn_result source_result=music_audio_read(p,&audio,&live,&source_lease);
                if(source_result!=KSN_OK)return source_result;
            }
#ifdef KASANE_P0_PROBE
            uint32_t read_us=(uint32_t)(esp_timer_get_time()-read_started);
            p->read_total_us+=read_us;p->read_calls++;
            if(read_us>p->read_max_us)p->read_max_us=read_us;
            if(read_cache){
                p->read_cache_us+=read_us;p->read_cache_calls++;
                if(read_us>p->read_cache_max_us)p->read_cache_max_us=read_us;
            }
            if(read_borrow){
                p->read_borrow_us+=read_us;p->read_borrow_calls++;
                if(read_us>p->read_borrow_max_us)p->read_borrow_max_us=read_us;
            }
#endif
            system_status=p->system_bytes&&music_now_us()<p->system_until_us;
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
            P1_STAGE_START(output_borrow_started);
            ksn_result output_result=music_output_borrow(p,&output_lease,
                                                          &output_live,&output_frames);
            P1_STAGE_END(KSN_P1_MUSIC_OUTPUT_BORROW,output_borrow_started);
            if(output_result!=KSN_OK){
                ksn_source_release(&source_lease);
                return output_result;
            }
#endif
            key[3]=p->title_revision;
            if(system_status){key[4]=1;key[5]=p->system_revision;}
            else if(p->owned.bytes[1]){key[4]=2;key[5]=p->message_revision;}
            else if(live){
                key[4]=3;key[5]=(uint64_t)audio->state;
                key[6]=audio->position_ms/1000u;
                key[7]=audio->duration_ms/1000u;
                key[11]=audio->duration_ms?1u:0u;
            }
            if(live&&audio->duration_ms){
                uint64_t fill=(uint64_t)audio->position_ms*(unsigned)(width-24)/audio->duration_ms;
                key[10]=fill>(uint64_t)(width-24)?(uint64_t)(width-24):fill;
                if(key[10])key[9]=1;
            }else if(live&&audio->state==POCKET_AV_UI_PLAYING){
                uint32_t phase=(uint32_t)(music_now_us()/66667u);
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
                if(output_live)phase=output_frames/(SOUND_SAMPLE_RATE/30u);
#endif
                key[10]=presenter_light_key(phase,width-24);
                if(key[10])key[9]=2;
            }
        }
    }
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    P1_STAGE_END(KSN_P1_MUSIC_KEY,refresh_started);
#endif
    if(p->have_key&&memcmp(key,p->key,sizeof(key))==0){
        if(source_lease.active)(void)ksn_source_commit(&source_lease);
        ksn_source_release(&source_lease);
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
        music_output_finish(&output_lease,true);
#endif
        return KSN_OK;
    }
    ksn_presenter_values values=p->owned;
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    P1_STAGE_START(make_started);
#endif
    ksn_p0_probe_copy(KSN_P0_MUSIC_MODEL_COPY,sizeof(values));
    if(p->reactive){
        if(system_status){
            memcpy(values.text[1],p->system_status,p->system_bytes+1u);
            ksn_p0_probe_copy(KSN_P0_MUSIC_STATUS,p->system_bytes+1u);
            values.bytes[1]=p->system_bytes;
        }
        if(live){
            values.position_ms=audio->position_ms;
            values.duration_ms=audio->duration_ms;
            values.playing=audio->state==POCKET_AV_UI_PLAYING;
            if(!values.bytes[1]){
                static const char *const names[]={"READY","PLAYING","PAUSED","ENDED","ERROR"};
                if((unsigned)audio->state>=sizeof(names)/sizeof(names[0])){
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
                    music_output_finish(&output_lease,false);
#endif
                    ksn_source_release(&source_lease);return KSN_INVALID;
                }
                char line[KSN_PRESENTER_TEXT_MAX+1u];
                int n=snprintf(line,sizeof(line),"%s  %us",names[audio->state],
                               (unsigned)(audio->position_ms/1000u));
                if(audio->duration_ms&&n>0&&(size_t)n<sizeof(line))
                    n+=snprintf(line+n,sizeof(line)-(size_t)n," / %us",
                                (unsigned)(audio->duration_ms/1000u));
                ksn_p0_probe_copy(KSN_P0_MUSIC_MATERIALIZED,strlen(line)+1u);
                ksn_result r=ksn_presenter_copy_text(values.text[1],&values.bytes[1],
                                                       line,strlen(line));
                if(r!=KSN_OK){
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
                    music_output_finish(&output_lease,false);
#endif
                    ksn_source_release(&source_lease);return r;
                }
            }
        }
        values.phase=(uint32_t)(music_now_us()/66667u);
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
        if(output_live&&live&&audio->state==POCKET_AV_UI_PLAYING&&
           !audio->duration_ms)
            values.phase=output_frames/(SOUND_SAMPLE_RATE/30u);
#endif
        values.help=p->help;
    }
    ksn_presenter_plan plan;
    ksn_result r=ksn_presenter_make(KSN_PRESENTER_MUSIC,&values,
        (uint16_t)(music_viewport().x1-music_viewport().x0),(uint16_t)(music_viewport().y1-music_viewport().y0),&plan);
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    plan.patchable=true;
#endif
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    P1_STAGE_END(KSN_P1_MUSIC_MAKE,make_started);
#endif
    if(r!=KSN_OK){
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
        music_output_finish(&output_lease,false);
#endif
        ksn_source_release(&source_lease);return r;
    }
    if(music_busy()){
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
        music_output_finish(&output_lease,false);
#endif
        ksn_source_release(&source_lease);return KSN_BUSY;
    }
    bool patch=presenter_can_patch(p,&plan);
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    bool light_only=patch&&music_light_only_change(p,key);
    if(patch)p->output_probe_patch_calls++;
    else p->output_probe_replace_calls++;
    if(light_only)p->output_probe_light_calls++;
#endif
    ksn_tx ticket;
    ksn_ref candidate_refs[KSN_PRESENTER_ITEMS];
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    P1_STAGE_START(submit_started);
#endif
    if(patch){
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
        if(light_only)r=music_light_patch(&plan,p->refs,p->key[10],key[10],&ticket);
        else
#endif
            r=ksn_presenter_patch(music_view(),music_viewport(),&plan,p->refs,&ticket);
    }
    else r=ksn_presenter_submit(music_view(),music_viewport(),(ksn_resource){0},&plan,candidate_refs,&ticket);
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    P1_STAGE_END(KSN_P1_MUSIC_SUBMIT,submit_started);
#endif
    if(r!=KSN_OK){
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
        music_output_finish(&output_lease,false);
#endif
        ksn_source_release(&source_lease);return r;
    }
    if(source_lease.active)(void)ksn_source_commit(&source_lease);
    ksn_source_release(&source_lease);
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    music_output_finish(&output_lease,true);
#endif
    if(!patch)memcpy(p->refs,candidate_refs,sizeof(candidate_refs));
    for(unsigned light=0;light<3;light++)p->light_ref_index[light]=UINT8_MAX;
    static const ksn_rgba light_colors[]={0x78c8ffffu,0x3c78aaffu,0x1e3c5affu};
    for(unsigned i=0;i<plan.count;i++){
        if(plan.items[i].kind!=KSN_RECT)continue;
        for(unsigned light=0;light<3;light++)
            if(plan.items[i].color==light_colors[light])
                p->light_ref_index[light]=(uint8_t)i;
    }
    p->topology_count=plan.count;p->topology_background=plan.background;
    for(unsigned i=0;i<plan.count;i++)p->topology[i]=presenter_shape(&plan.items[i]);
    p->have_refs=false;
    memcpy(p->key,key,sizeof(key));p->have_key=true;p->ticket=ticket;
    music_submitted(ticket,patch?KSN_PATCH:KSN_REPLACE);
    return KSN_OK;
}


static ksn_result music_host_status(void *instance,const char *text,size_t bytes,
                                    uint64_t until_us){
    if(!instance||instance!=active)return KSN_OK;
    music_state *p=instance;
    if(p->manual)return KSN_UNSUPPORTED;
    char candidate[KSN_PRESENTER_TEXT_MAX+1u]={0};
    uint8_t count=0;
    ksn_result result=ksn_presenter_copy_text(candidate,&count,text,bytes);
    if(result!=KSN_OK)return result;
    bool changed=!presenter_text_equal(p->system_status,p->system_bytes,candidate,count);
    if(changed&&p->system_revision==UINT64_MAX)return KSN_LIMIT;
    memcpy(p->system_status,candidate,count+1u);
    ksn_p0_probe_copy(KSN_P0_MUSIC_STATUS,count+1u);
    if(changed)p->system_revision++;
    p->system_bytes=count;p->system_until_us=until_us;
    p->reactive=true;
    return music_refresh();
}

static ksn_result music_settle(void *instance,bool *blocked){
    if(blocked)*blocked=false;
    if(!instance||instance!=active)return KSN_OK;
    music_state *p=instance;
    if(p->ticket.value){
        ksn_submission outcome=ksn_view_poll(music_view());
        if(outcome.ticket.value!=p->ticket.value)return KSN_STALE;
        if(outcome.status==KSN_SUBMITTED){if(blocked)*blocked=true;return KSN_OK;}
        if(outcome.status==KSN_DISCARDED){p->have_key=false;p->have_refs=false;}
        else if(outcome.status!=KSN_PRESENTED)return KSN_STALE;
        else p->have_refs=true;
        p->ticket=(ksn_tx){0};
    }
    return KSN_OK;
}

static ksn_result music_step(void *instance,bool *blocked){
    bool pending=false;
    ksn_result settled=music_settle(instance,&pending);
    if(blocked)*blocked=pending;
    if(settled!=KSN_OK||pending||!instance||instance!=active)return settled;
    music_state *p=instance;
    ksn_result result=music_refresh();
    if(result==KSN_BUSY)result=KSN_OK;
    if(blocked)*blocked=p->ticket.value!=0;
    return result;
}

static bool presenter_text(JSContext *ctx,JSValueConst model,const char *name,
                           char out[KSN_PRESENTER_TEXT_MAX+1u],uint8_t *bytes){
    JSValue value=JS_GetPropertyStr(ctx,model,name);
    if(JS_IsException(value))return false;
    if(!JS_IsString(value)){
        JS_FreeValue(ctx,value);
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.view.update",
                         "presenter text must be a string",false,NULL);
        return false;
    }
    int64_t units;
    if(JS_GetLength(ctx,value,&units)<0){JS_FreeValue(ctx,value);return false;}
    if(units>MUSIC_INPUT_UNITS){
        JS_FreeValue(ctx,value);
        music_throw_result(ctx,KSN_LIMIT,"kasane.view.update");return false;
    }
    size_t length;const char *s=JS_ToCStringLen(ctx,&length,value);
    JS_FreeValue(ctx,value);
    if(!s)return false;
    ksn_p0_probe_copy(KSN_P0_UTF8_MATERIALIZED,length);
    ksn_result result=ksn_presenter_copy_text(out,bytes,s,length);
    JS_FreeCString(ctx,s);
    if(result==KSN_OK)return true;
    music_throw_result(ctx,result,"kasane.view.update");return false;
}
static bool presenter_number(JSContext *ctx,JSValueConst model,const char *name,
                             bool nullable,uint32_t *out){
    JSValue value=JS_GetPropertyStr(ctx,model,name);
    if(JS_IsException(value))return false;
    if(nullable&&(JS_IsNull(value)||JS_IsUndefined(value))){*out=0;JS_FreeValue(ctx,value);return true;}
    double n;bool ok=music_number_in(ctx,value,0,UINT32_MAX,&n);
    JS_FreeValue(ctx,value);
    if(ok){*out=(uint32_t)n;return true;}
    pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.view.update",
                     "presenter number must be a nonnegative integer",false,NULL);
    return false;
}
static bool presenter_bool(JSContext *ctx,JSValueConst model,const char *name,bool *out){
    JSValue value=JS_GetPropertyStr(ctx,model,name);
    if(JS_IsException(value))return false;
    if(!JS_IsBool(value)){
        JS_FreeValue(ctx,value);
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.view.update",
                         "presenter flag must be boolean",false,NULL);
        return false;
    }
    *out=JS_ToBool(ctx,value)!=0;JS_FreeValue(ctx,value);return true;
}
static JSValue js_presenter_update(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    uint32_t handle=(uint32_t)(uintptr_t)JS_GetOpaque(self,music_class);
    if(!handle||!active||active->handle!=handle)
        return music_throw_result(ctx,KSN_STALE,"kasane.view.update");
    if(active->reactive)return music_throw_result(ctx,KSN_BUSY,"kasane.view.update");
    JSValueConst model=argc?argv[0]:JS_UNDEFINED;
    if(!JS_IsObject(model))return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                         "kasane.view.update","model must be an object",false,NULL);
    ksn_presenter_values values={0};
    ksn_presenter_kind kind=KSN_PRESENTER_MUSIC;
    if(kind==KSN_PRESENTER_MUSIC){
        if(!presenter_text(ctx,model,"title",values.text[0],&values.bytes[0])||
           !presenter_text(ctx,model,"status",values.text[1],&values.bytes[1])||
           !presenter_number(ctx,model,"positionMs",false,&values.position_ms)||
           !presenter_number(ctx,model,"durationMs",true,&values.duration_ms)||
           !presenter_number(ctx,model,"phase",false,&values.phase)||
           !presenter_bool(ctx,model,"playing",&values.playing)||
           !presenter_bool(ctx,model,"help",&values.help))return JS_EXCEPTION;
    }else{
        return music_throw_result(ctx,KSN_UNSUPPORTED,"kasane.view.update");
    }
    uint16_t width=(uint16_t)(music_viewport().x1-music_viewport().x0);
    uint16_t height=(uint16_t)(music_viewport().y1-music_viewport().y0);
    ksn_presenter_plan plan;
    ksn_result result=ksn_presenter_make(kind,&values,width,height,&plan);
    if(result!=KSN_OK)return music_throw_result(ctx,result,"kasane.view.update");
    bool changed=!active->manual||
                 !presenter_values_equal(&active->owned,&values);
    if(changed&&active->manual_revision==UINT64_MAX)
        return music_throw_result(ctx,KSN_LIMIT,"kasane.view.update");
    active->owned=values;active->manual=true;
    ksn_p0_probe_copy(KSN_P0_MUSIC_MODEL_COPY,sizeof(values));
    if(changed)active->manual_revision++;
    result=music_refresh();
    return result==KSN_OK||result==KSN_BUSY?JS_UNDEFINED:
           music_throw_result(ctx,result,"kasane.view.update");
}
static music_state *presenter_owner(JSValueConst self){
    uint32_t handle=(uint32_t)(uintptr_t)JS_GetOpaque(self,music_class);
    return handle&&active&&active->handle==handle?
           active:NULL;
}
static bool presenter_optional_text(JSContext *ctx,JSValueConst model,const char *key,
                                    char out[KSN_PRESENTER_TEXT_MAX+1u],uint8_t *bytes){
    JSValue value=JS_GetPropertyStr(ctx,model,key);
    if(JS_IsException(value))return false;
    if(JS_IsUndefined(value)){JS_FreeValue(ctx,value);return true;}
    if(!JS_IsString(value)){
        JS_FreeValue(ctx,value);
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.view.set",
                         "slot value must be a string",false,NULL);
        return false;
    }
    int64_t units;
    if(JS_GetLength(ctx,value,&units)<0){JS_FreeValue(ctx,value);return false;}
    if(units>MUSIC_INPUT_UNITS){
        JS_FreeValue(ctx,value);
        music_throw_result(ctx,KSN_LIMIT,"kasane.view.set");return false;
    }
    size_t length;const char *text=JS_ToCStringLen(ctx,&length,value);
    JS_FreeValue(ctx,value);
    if(!text)return false;
    ksn_p0_probe_copy(KSN_P0_UTF8_MATERIALIZED,length);
    ksn_result result=ksn_presenter_copy_text(out,bytes,text,length);
    JS_FreeCString(ctx,text);
    if(result==KSN_OK)return true;
    music_throw_result(ctx,result,"kasane.view.set");return false;
}

static JSValue js_presenter_set(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    music_state *p=presenter_owner(self);
    if(!p)return music_throw_result(ctx,KSN_STALE,"kasane.view.set");
    if(p->manual)return music_throw_result(ctx,KSN_BUSY,"kasane.view.set");
        JSValueConst model=argc?argv[0]:JS_UNDEFINED;
    if(!JS_IsObject(model))return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                         "kasane.view.set","slots must be an object",false,NULL);
    ksn_presenter_values candidate=p->owned;
    ksn_p0_probe_copy(KSN_P0_MUSIC_MODEL_COPY,sizeof(candidate));
    if(!presenter_optional_text(ctx,model,"title",candidate.text[0],&candidate.bytes[0])||
       !presenter_optional_text(ctx,model,"message",candidate.text[1],&candidate.bytes[1]))
        return JS_EXCEPTION;
    bool title_changed=!presenter_text_equal(p->owned.text[0],p->owned.bytes[0],
                                             candidate.text[0],candidate.bytes[0]);
    bool message_changed=!presenter_text_equal(p->owned.text[1],p->owned.bytes[1],
                                               candidate.text[1],candidate.bytes[1]);
    if((title_changed&&p->title_revision==UINT64_MAX)||
       (message_changed&&p->message_revision==UINT64_MAX))
        return music_throw_result(ctx,KSN_LIMIT,"kasane.view.set");
    p->owned=candidate;
    ksn_p0_probe_copy(KSN_P0_MUSIC_MODEL_COPY,sizeof(candidate));
    p->reactive=true;
    if(title_changed)p->title_revision++;
    if(message_changed)p->message_revision++;
    ksn_result result=music_refresh();
    return result==KSN_OK||result==KSN_BUSY?JS_UNDEFINED:
           music_throw_result(ctx,result,"kasane.view.set");
}
static JSValue js_presenter_bind(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    music_state *p=presenter_owner(self);
    if(!p)return music_throw_result(ctx,KSN_STALE,"kasane.view.bind");
    if(p->manual)return music_throw_result(ctx,KSN_BUSY,"kasane.view.bind");
    const char *source=argc&&JS_IsString(argv[0])?JS_ToCString(ctx,argv[0]):NULL;
    if(!source)return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.view.bind",
                                     "source must be playback",false,NULL);
    bool playback=strcmp(source,"playback")==0;
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    bool output=strcmp(source,"output")==0;
#endif
    JS_FreeCString(ctx,source);
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    if(output){
        if(p->output_subscription)return JS_UNDEFINED;
        ksn_source_registry *registry=NULL;
        ksn_source_handle handle={0};
        ksn_result result=pocket_av_output_source_existing(&registry,&handle);
        if(result!=KSN_OK)return music_throw_result(ctx,result,"kasane.view.bind");
        ksn_source_subscription *subscription=calloc(1,sizeof(*subscription));
        if(!subscription)return music_throw_result(ctx,KSN_OOM,"kasane.view.bind");
        result=ksn_source_subscribe(registry,handle,p->handle,
            &output_schema,output_bindings,2,subscription);
        if(result!=KSN_OK){
            free(subscription);
            return music_throw_result(ctx,result,"kasane.view.bind");
        }
        p->output_registry=registry;
        p->output_subscription=subscription;
#ifdef KASANE_P0_PROBE
        ESP_LOGI("KSN_P1_MUSIC","bound heap_free=%lu largest=%lu min=%lu subscription_bytes=%lu",
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned long)sizeof(*subscription));
#endif
        p->have_key=false;
        p->reactive=true;
        result=music_refresh();
        return result==KSN_OK||result==KSN_BUSY?JS_UNDEFINED:
               music_throw_result(ctx,result,"kasane.view.bind");
    }
#endif
    if(!playback)
        return music_throw_result(ctx,KSN_UNSUPPORTED,"kasane.view.bind");
    int32_t id=pocket_av_ui_current_player();
    if(!id)return pocket_api_throw(ctx,POCKET_ERR_NOT_AVAILABLE,"kasane.view.bind",
                                  "no open player",false,NULL);
    p->playback_id=id;
    ksn_source_registry *registry=NULL;
    ksn_source_handle source_handle={0};
    ksn_result source_result=pocket_av_playback_source_open(&registry,&source_handle);
    if(source_result!=KSN_OK)return music_throw_result(ctx,source_result,"kasane.view.bind");
    ksn_source_subscription subscription={0};
    source_result=ksn_source_subscribe(registry,source_handle,p->handle,
        &playback_schema,playback_bindings,6,&subscription);
    if(source_result!=KSN_OK)return music_throw_result(ctx,source_result,"kasane.view.bind");
    p->playback_registry=registry;
    p->playback_revision=pocket_av_playback_source_revision_ref(source_handle);
    p->playback_subscription=subscription;
    p->audio_revision=0;
    p->audio_live=false;
    p->reactive=true;
    ksn_result result=music_refresh();
    return result==KSN_OK||result==KSN_BUSY?JS_UNDEFINED:
           music_throw_result(ctx,result,"kasane.view.bind");
}
static JSValue js_presenter_toggle_help(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argc;(void)argv;
    music_state *p=presenter_owner(self);
    if(!p)return music_throw_result(ctx,KSN_STALE,"kasane.view.toggleHelp");
    if(p->manual)return music_throw_result(ctx,KSN_BUSY,"kasane.view.toggleHelp");
        p->help=!p->help;p->reactive=true;
    ksn_result result=music_refresh();
    return result==KSN_OK||result==KSN_BUSY?JS_NewBool(ctx,p->help):
           music_throw_result(ctx,result,"kasane.view.toggleHelp");
}
static JSValue js_presenter_dismiss_help(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argc;(void)argv;
    music_state *p=presenter_owner(self);
    if(!p)return music_throw_result(ctx,KSN_STALE,"kasane.view.dismissHelp");
    if(p->manual)return music_throw_result(ctx,KSN_BUSY,"kasane.view.dismissHelp");
        if(!p->help)return JS_FALSE;
    p->help=false;p->reactive=true;
    ksn_result result=music_refresh();
    return result==KSN_OK||result==KSN_BUSY?JS_TRUE:
           music_throw_result(ctx,result,"kasane.view.dismissHelp");
}
static const JSCFunctionListEntry presenter_methods[]={
    JS_CFUNC_DEF("update",1,js_presenter_update),
    JS_CFUNC_DEF("set",1,js_presenter_set),
    JS_CFUNC_DEF("bind",1,js_presenter_bind),
    JS_CFUNC_DEF("toggleHelp",0,js_presenter_toggle_help),
    JS_CFUNC_DEF("dismissHelp",0,js_presenter_dismiss_help),
};

static bool music_class_ready(JSContext *ctx){
    JSRuntime *rt=JS_GetRuntime(ctx);
    if(!pocket_api_class_ready(rt,&music_rt,&music_class)){
        JS_NewClassID(rt,&music_class);
        if(JS_NewClass(rt,music_class,&music_def)<0)return false;
    }
    JSValue proto=JS_GetClassProto(ctx,music_class);
    bool ready=!JS_IsNull(proto);
    JS_FreeValue(ctx,proto);
    if(ready)return true;
    proto=JS_NewObject(ctx);
    if(JS_IsException(proto))return false;
    if(JS_SetPropertyFunctionList(ctx,proto,presenter_methods,
       (int)(sizeof(presenter_methods)/sizeof(presenter_methods[0])))<0||
       JS_HasException(ctx)){
        JS_FreeValue(ctx,proto);return false;
    }
    JS_SetClassProto(ctx,music_class,proto);
    return true;
}

static JSValue music_mount(JSContext *ctx,const pocket_app_view_host *host,void **out){
    if(!host||!out||!host->view||!host->now_us||
       !host->busy||!host->submitted||active)
        return music_throw_result(ctx,KSN_BUSY,"kasane.mount");
    if(music_serial==UINT32_MAX)return music_throw_result(ctx,KSN_LIMIT,"kasane.mount");
    if(!music_class_ready(ctx))return JS_EXCEPTION;
    JSValue object=JS_NewObjectClass(ctx,music_class);
    if(JS_IsException(object))return object;
    music_state *p=calloc(1,sizeof(*p));
    if(!p){JS_FreeValue(ctx,object);return music_throw_result(ctx,KSN_OOM,"kasane.mount");}
    p->handle=++music_serial;
#ifdef KASANE_P0_PROBE
    ESP_LOGI("KSN_MUSIC_SOURCE","start");
#ifndef KASANE_P1_OUTPUT_OVERLAY_PROBE
    p0_time_light_patch=p0_time_light_same=p0_time_light_fallback=0;
#endif
#endif
    JS_SetOpaque(object,(void *)(uintptr_t)p->handle);
    current_host=*host;active=p;*out=p;
    return object;
}

static size_t music_native_bytes(const void *instance){
    return instance?sizeof(music_state):0u;
}
static void music_destroy(void *instance){
#ifdef KASANE_P0_PROBE
    music_state *p=instance;
#ifndef KASANE_P1_OUTPUT_OVERLAY_PROBE
    if(p)ESP_LOGI("KSN_P5_MUSIC","time_light patch=%lu same=%lu fallback=%lu",
        (unsigned long)p0_time_light_patch,(unsigned long)p0_time_light_same,
        (unsigned long)p0_time_light_fallback);
#endif
    if(p)ESP_LOGI("KSN_MUSIC_SOURCE","read calls=%lu mean_us=%lu max_us=%lu",
        (unsigned long)p->read_calls,
        (unsigned long)(p->read_calls?p->read_total_us/p->read_calls:0),
        (unsigned long)p->read_max_us);
    if(p)ESP_LOGI("KSN_MUSIC_SOURCE",
        "read source cache_calls=%lu cache_mean_us=%lu cache_max_us=%lu borrow_calls=%lu borrow_mean_us=%lu borrow_max_us=%lu",
        (unsigned long)p->read_cache_calls,
        (unsigned long)(p->read_cache_calls?p->read_cache_us/p->read_cache_calls:0),
        (unsigned long)p->read_cache_max_us,(unsigned long)p->read_borrow_calls,
        (unsigned long)(p->read_borrow_calls?p->read_borrow_us/p->read_borrow_calls:0),
        (unsigned long)p->read_borrow_max_us);
#endif
    if(instance==active){active=NULL;current_host=(pocket_app_view_host){0};}
#ifdef KASANE_P1_OUTPUT_OVERLAY_PROBE
    if(instance){
        music_state *p=instance;
        ESP_LOGI("KSN_P1_MUSIC","submit patch=%lu light=%lu fast=%lu replace=%lu",
            (unsigned long)p->output_probe_patch_calls,
            (unsigned long)p->output_probe_light_calls,
            (unsigned long)p->output_probe_fast_calls,
            (unsigned long)p->output_probe_replace_calls);
    }
    if(instance)free(((music_state *)instance)->output_subscription);
#endif
    free(instance);
}
const pocket_app_view_provider pocket_app_music_view={
    .name="music",.mount=music_mount,.settle=music_settle,.step=music_step,
    .host_status=music_host_status,.native_bytes=music_native_bytes,
    .destroy=music_destroy
};
