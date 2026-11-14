#include "pocket_kasane.h"
#ifdef ESP_PLATFORM
#include "disp.h"
#else
/* Host tests: nothing is ever in flight. */
static inline bool disp_busy(void) { return false; }
#endif
#include "pocket_api.h"
#include "pocket_shiki_frame.h"
#include "pocket_grid.h"
#include "pocket_video.h"
#include "pocket_pixel.h"
#include "ui/kasane/ksn_shiki_frame.h"
#ifdef KASANE_PROC_JS_DIAGNOSTIC
#include "esp_log.h"
#endif
#include "ui/kasane/ksn_runtime.h"
#include "app_notice.h"
#include "app_view_provider.h"
#include "ui/kasane/ksn_schema_session.h"
#include "ui/kasane/ksn_p0_probe.h"
#include "app_view_assets.h"
#include "pet/ksn_pet.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Two-core P0 probe (docs/perf/two-core-pipeline.md sec.5): where the JS
 * turn first touches state the display would read. Device only. */
#if defined(ESP_PLATFORM)
#include "frame_bd.h"
#else
#define frame_bd_join_point(site) ((void)0)
#endif

#define KASANE_REF_LIMIT 32u
#define KASANE_REF_STORAGE 64u
#define KASANE_SCREEN ((ksn_rect){0,0,240,135})
static ksn_rect viewport={0,0,240,135};
static bool overlay_profile;

void pocket_kasane_set_viewport(int16_t x,int16_t y,int16_t width,int16_t height) {
    int32_t x1=(int32_t)x+width,y1=(int32_t)y+height;
    overlay_profile=true;
    if(width<=0||height<=0||x<0||y<0||x1>240||y1>135) viewport=KASANE_SCREEN;
    else viewport=(ksn_rect){x,y,(int16_t)x1,(int16_t)y1};
}

static void viewport_rect(ksn_rect *r) {
    int32_t x0=(int32_t)r->x0+viewport.x0,x1=(int32_t)r->x1+viewport.x0;
    int32_t y0=(int32_t)r->y0+viewport.y0,y1=(int32_t)r->y1+viewport.y0;
    r->x0=(int16_t)(x0<INT16_MIN?INT16_MIN:x0>INT16_MAX?INT16_MAX:x0);
    r->x1=(int16_t)(x1<INT16_MIN?INT16_MIN:x1>INT16_MAX?INT16_MAX:x1);
    r->y0=(int16_t)(y0<INT16_MIN?INT16_MIN:y0>INT16_MAX?INT16_MAX:y0);
    r->y1=(int16_t)(y1<INT16_MIN?INT16_MIN:y1>INT16_MAX?INT16_MAX:y1);
}
static void viewport_clip(ksn_rect *r) {
    if(r->x0<viewport.x0)r->x0=viewport.x0;
    if(r->y0<viewport.y0)r->y0=viewport.y0;
    if(r->x1>viewport.x1)r->x1=viewport.x1;
    if(r->y1>viewport.y1)r->y1=viewport.y1;
    /* A command wholly outside the viewport has an empty, not reversed,
     * clip. Off-screen animation poses remain valid and simply draw nothing. */
    if(r->x1<r->x0)r->x1=r->x0;
    if(r->y1<r->y0)r->y1=r->y0;
}

typedef enum { REF_FREE, REF_CANDIDATE, REF_ACTIVE } ref_status;
typedef struct {
    uint32_t handle;
    ksn_ref ref;
    ksn_tx ticket;
    ref_status status;
} ref_slot;
typedef struct {
    ksn_source_provider provider;
    ksn_source_subscription subscription;
    ksn_source_handle handle;
    uint64_t pending_revision;
} schema_source;
typedef struct {
    ksn_source_registry *registry;
    ksn_source_handle handle;
    ksn_source_subscription subscription;
    uint64_t pending_revision;
} schema_external;
typedef struct {
    uint64_t expiry_us;
    uint8_t count;
    schema_external entries[KSN_SOURCE_MAX_REGISTERED];
} schema_externals;
typedef struct {
    ksn_source_registry registry;
    uint64_t expiry_us;
    schema_externals *external;
    schema_source entries[];
} schema_sources;
typedef struct {
    ksn_schema_session session;
    const ksn_schema *definition;
    const pocket_app_view_asset *asset;
    void *owned_asset;
    size_t allocation_bytes,owned_asset_bytes;
    void *source_state;
    uint64_t revision;
    uint32_t pending_base_slots;
    uint32_t handle;
    ksn_schema_value values[];
} schema_state;
typedef struct {
    ksn_app_lease lease;
    /* Two generations let a 32-reference REPLACE be built while the displayed
     * generation remains valid. Retiring identities frees slots immediately;
     * old wrappers/finalizers cannot affect subsequently reused slots. */
    ref_slot refs[KASANE_REF_STORAGE];
    ksn_tx building;
    ksn_tx submitted;
    ksn_update_mode submitted_mode;
    struct {const pocket_app_image_asset *asset;ksn_resource resource;} images[4];
    ksn_resource proc_resource[2];
    ksn_resource video_resource;
    ksn_resource pixel_resource;
    ksn_resource grid_resources[POCKET_GRID_MAX_SLOTS];
    uint16_t grid_width[POCKET_GRID_MAX_SLOTS],grid_height[POCKET_GRID_MAX_SLOTS];
    ksn_resource notice_resource;
    ksn_tx notice_tx;
    uint32_t notice_displayed,notice_pending;
    uint16_t notice_variant,notice_pending_variant;
    bool active;
    const pocket_app_view_provider *provider;
    void *provider_state;
    schema_state *schema;
} kasane_state;

_Static_assert(sizeof(kasane_state)<=KSN_RUNTIME_TAIL_BUDGET,"Kasane control allocation budget");

static kasane_state *state;
/* Never recycle identities across host reset while old JS wrappers can live. */
static uint32_t ref_serial;
static uint32_t schema_serial;
static uint64_t owner_now_us;
#ifdef KASANE_PROC_JS_DIAGNOSTIC
static uint32_t proc_log_frame;
#endif
static JSClassID tx_class, modal_class, ref_class, template_class;
static JSClassID instance_class, image_class, animation_class;
static JSClassID schema_class;
static JSClassID source_cap_class;
static pocket_class_owner_t tx_owner, modal_owner, ref_owner, template_owner;
static pocket_class_owner_t instance_owner, image_owner, animation_owner;
static pocket_class_owner_t schema_class_owner;
static pocket_class_owner_t source_cap_owner;
typedef struct {
    ksn_source_registry *registry;
    ksn_source_handle handle;
    uint32_t serial;
} source_cap_record;
typedef struct {
    uint8_t count;
    source_cap_record records[KSN_SOURCE_MAX_REGISTERED];
} source_cap_table;
static source_cap_table *source_caps;
static uint32_t source_cap_serial;

static const char *result_code(ksn_result result) {
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

static JSValue throw_result(JSContext *ctx, ksn_result result, const char *op) {
    const char *code=result_code(result);
    return pocket_api_throw(ctx,code,op,code,
                            result==KSN_BUSY||result==KSN_OOM||result==KSN_IO,
                            POCKET_OUTCOME_NOT_APPLIED);
}
static const char *bounded_cstring(JSContext *ctx,JSValueConst value,int64_t max_units){
    int64_t units=0;
    return JS_IsString(value)&&JS_GetLength(ctx,value,&units)==0&&units<=max_units?
           JS_ToCString(ctx,value):NULL;
}

/* The state lives in the runtime's single block (attach_tail), so the whole
 * native arena is one allocation and the runtime frees it on detach. */
static ksn_result attach_state(const char *op) {
    kasane_state *candidate=NULL;ksn_app_lease lease;
    ksn_result result=ksn_runtime_app_attach_tail(&lease,sizeof(*candidate),(void **)&candidate);
    (void)op;
    if(result!=KSN_OK) return result;
    candidate->lease=lease;state=candidate;
    return KSN_OK;
}

static bool ensure_state(JSContext *ctx, const char *op) {
    if(state) {
        if(ksn_runtime_app_view(state->lease))return true;
        throw_result(ctx,KSN_STALE,op);return false;
    }
    ksn_result result=attach_state(op);
    if(result!=KSN_OK) { throw_result(ctx,result,op);return false; }
    return true;
}

void pocket_kasane_prepare(void) {
    if(!state) (void)attach_state("prepare");
}

/* No JS calls after allocation: attach only a complete first definition, so
 * failures cannot alter the displayed bank or reserve an unused cache. */
static ksn_result create_template(const ksn_draw *draws,uint16_t count,ksn_template *out) {
    if(state->building.value) return KSN_BUSY;
    return ksn_runtime_cache_create(ksn_runtime_app_view(state->lease),draws,count,out);
}

static ksn_view *view(void) {
    return state?ksn_runtime_app_view(state->lease):NULL;
}
static bool provider_busy(void *owner){
    kasane_state *s=owner;
    return s->building.value||s->submitted.value;
}
static void provider_submitted(void *owner,ksn_tx ticket,ksn_update_mode mode){
    kasane_state *s=owner;
    s->submitted=ticket;s->submitted_mode=mode;s->active=true;
    ksn_runtime_app_activate(s->lease);
}

static uint32_t opaque_value(JSValueConst value, JSClassID class_id) {
    return (uint32_t)(uintptr_t)JS_GetOpaque(value,class_id);
}

static void release_ref_handle(uint32_t handle) {
    if(!state||!handle) return;
    unsigned index=handle&63u;
    if(state->refs[index].handle==handle)
        state->refs[index]=(ref_slot){0};
}

static void ref_finalizer(JSRuntime *rt, JSValue value) {
    (void)rt;
    release_ref_handle(opaque_value(value,ref_class));
}

/* One shared class name: QuickJS interns each distinct name as an atom in the
 * guest, and the name is only ever printed by its debug dumps. */
#define KASANE_CLASS "Kasane"
static const JSClassDef tx_def={.class_name=KASANE_CLASS};
static const JSClassDef modal_def={.class_name=KASANE_CLASS};
static const JSClassDef ref_def={.class_name=KASANE_CLASS,.finalizer=ref_finalizer};
static const JSClassDef template_def={.class_name=KASANE_CLASS};
static const JSClassDef instance_def={.class_name=KASANE_CLASS};
static const JSClassDef image_def={.class_name=KASANE_CLASS};
static const JSClassDef animation_def={.class_name=KASANE_CLASS};
static const JSClassDef schema_def={.class_name=KASANE_CLASS};
static const JSClassDef source_cap_def={.class_name=KASANE_CLASS};

static ref_slot *ref_from(JSContext *ctx, JSValueConst self, const char *op) {
    uint32_t handle=opaque_value(self,ref_class);
    if(!state||!handle) {
        pocket_api_throw(ctx,POCKET_ERR_CLOSED,op,"draw reference is closed",false,NULL);
        return NULL;
    }
    ref_slot *slot=&state->refs[handle&63u];
    if(slot->handle!=handle||slot->status==REF_FREE) {
        pocket_api_throw(ctx,POCKET_ERR_CLOSED,op,"draw reference is stale",false,NULL);
        return NULL;
    }
    return slot;
}

static ref_slot *claim_ref(JSContext *ctx, ksn_ref ref, ksn_tx ticket) {
    unsigned candidates=0;
    for(unsigned i=0;i<KASANE_REF_STORAGE;i++)
        if(state->refs[i].status==REF_CANDIDATE&&
           state->refs[i].ticket.value==ticket.value) candidates++;
    if(candidates==KASANE_REF_LIMIT) {
        pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,"kasane.rect",
                         "the update exposes more than 32 draw references",false,
                         POCKET_OUTCOME_NOT_APPLIED);
        return NULL;
    }
    for(unsigned i=0;i<KASANE_REF_STORAGE;i++) {
        if(state->refs[i].status!=REF_FREE) continue;
        if(ref_serial==0x03ffffffu) break;
        uint32_t handle=(++ref_serial<<6)|i;
        state->refs[i]=(ref_slot){handle,ref,ticket,REF_CANDIDATE};
        return &state->refs[i];
    }
    pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,"kasane.rect",
                     "draw-reference wrapper storage is in use",false,
                     POCKET_OUTCOME_NOT_APPLIED);
    return NULL;
}

static void discard_candidates(ksn_tx ticket) {
    if(!state||!ticket.value) return;
    for(unsigned i=0;i<KASANE_REF_STORAGE;i++)
        if(state->refs[i].status==REF_CANDIDATE &&
           state->refs[i].ticket.value==ticket.value)
            state->refs[i]=(ref_slot){0};
}

static void apply_outcome(void) {
    if(!state||!state->submitted.value) return;
    ksn_submission result=ksn_view_poll(view());
    if(result.ticket.value!=state->submitted.value||
       (result.status!=KSN_PRESENTED&&result.status!=KSN_DISCARDED)) return;
    if(result.status==KSN_PRESENTED) {
        if(state->submitted_mode==KSN_REPLACE)
            for(unsigned i=0;i<KASANE_REF_STORAGE;i++)
                if(state->refs[i].status==REF_ACTIVE)
                    state->refs[i]=(ref_slot){0};
        for(unsigned i=0;i<KASANE_REF_STORAGE;i++)
            if(state->refs[i].status==REF_CANDIDATE&&
               state->refs[i].ticket.value==result.ticket.value)
                state->refs[i].status=REF_ACTIVE;
    } else discard_candidates(result.ticket);
    state->submitted=(ksn_tx){0};
}

static char *schema_text_base(schema_state *s){
    return (char *)&s->values[s->definition->slot_count];
}
static schema_sources *schema_native(schema_state *s){
    return s->asset->source_count?(schema_sources *)s->source_state:NULL;
}
static schema_externals *schema_external_state(schema_state *s){
    if(!s->source_state)return NULL;
    return s->asset->source_count?schema_native(s)->external:
           (schema_externals *)s->source_state;
}
static void schema_external_remove(schema_state *s,unsigned index){
    schema_externals *external=schema_external_state(s);
    s->pending_base_slots|=external->entries[index].subscription.bound_slots;
    external->count--;
    if(index<external->count)
        memmove(&external->entries[index],&external->entries[index+1],
                (external->count-index)*sizeof(external->entries[0]));
    external->expiry_us=0;
    if(!external->count){
        if(s->asset->source_count)schema_native(s)->external=NULL;
        else s->source_state=NULL;
        free(external);
    }
}
static bool schema_handle_stale(ksn_source_registry *registry,
                                ksn_source_handle handle){
    return !registry||handle.index>=KSN_SOURCE_MAX_REGISTERED||
           !handle.generation||!registry->entries[handle.index].provider||
           registry->entries[handle.index].generation!=handle.generation;
}
static bool schema_prune_stale(schema_state *s){
    bool pruned=false;
    schema_sources *sources=schema_native(s);
    if(sources)for(unsigned i=0;i<s->asset->source_count;i++){
        schema_source *entry=&sources->entries[i];
        if(entry->subscription.binding_count&&
           schema_handle_stale(&sources->registry,entry->handle)){
            s->pending_base_slots|=entry->subscription.bound_slots;
            entry->subscription=(ksn_source_subscription){0};
            entry->pending_revision=0;
            sources->expiry_us=0;
            pruned=true;
        }
    }
    schema_externals *external=schema_external_state(s);
    if(external)for(unsigned i=external->count;i>0;i--)
        if(schema_handle_stale(external->entries[i-1].registry,
                               external->entries[i-1].handle)){
            schema_external_remove(s,i-1);
            pruned=true;
        }
    return pruned;
}
static schema_state *schema_alloc(const pocket_app_view_asset *asset,uint32_t consumer){
    const ksn_schema *definition=asset?asset->schema:NULL;
    if(!definition||ksn_schema_validate(definition)!=KSN_OK)return NULL;
    if(asset->source_count>KSN_SOURCE_MAX_REGISTERED||
       (asset->source_count&&!asset->sources))return NULL;
    size_t text_bytes=0;
    for(unsigned i=0;i<definition->slot_count;i++)
        if(definition->slots[i].type==KSN_SLOT_TEXT)
            text_bytes+=(size_t)definition->slots[i].capacity+1u;
    size_t source_offset=(text_bytes+7u)&~(size_t)7u;
    size_t source_bytes=asset->source_count?
        sizeof(schema_sources)+asset->source_count*sizeof(schema_source):0u;
    for(unsigned i=0;i<asset->source_count;i++){
        if(!asset->sources[i].source_open||!asset->sources[i].source_bindings||
           !asset->sources[i].source_binding_count||
           asset->sources[i].source_bytes>SIZE_MAX-7u)return NULL;
        size_t padded=(asset->sources[i].source_bytes+7u)&~(size_t)7u;
        if(source_bytes>SIZE_MAX-padded)return NULL;
        source_bytes+=padded;
    }
    size_t fixed_bytes=sizeof(schema_state)+
        definition->slot_count*sizeof(ksn_schema_value)+source_offset;
    if(fixed_bytes>SIZE_MAX-source_bytes)return NULL;
    size_t bytes=fixed_bytes+source_bytes;
    schema_state *s=calloc(1,bytes);
    if(!s)return NULL;
    s->definition=definition;s->asset=asset;s->revision=1;s->handle=consumer;
    s->allocation_bytes=bytes;
    if(asset->source_count){
        s->source_state=schema_text_base(s)+source_offset;
    }
    size_t offset=0;
    for(unsigned i=0;i<definition->slot_count;i++){
        const ksn_schema_slot *slot=&definition->slots[i];
        if(slot->type==KSN_SLOT_TEXT){
            s->values[i].data.text.utf8=schema_text_base(s)+offset;
            offset+=(size_t)slot->capacity+1u;
        }else if(slot->type==KSN_SLOT_COLOR)
            s->values[i].data.color=definition->dynamic_background&&
                definition->background_slot==i?definition->background:0x000000ffu;
        else if(slot->type==KSN_SLOT_U16)
            s->values[i].data.number=(uint16_t)slot->initial_number;
        else if(slot->type==KSN_SLOT_U32)
            s->values[i].data.wide_number=slot->initial_number;
    }
    if(ksn_schema_session_init(&s->session,definition)!=KSN_OK){free(s);return NULL;}
    schema_sources *native=schema_native(s);
    if(native){
        char *storage=(char *)(native->entries+asset->source_count);
        uint32_t source_slots=0;
        ksn_source_registry_init(&native->registry);
        for(unsigned i=0;i<asset->source_count;i++){
            const pocket_app_view_source *source=&asset->sources[i];
            schema_source *entry=&native->entries[i];
            ksn_result r=source->source_open(storage,&entry->provider);
            if(r==KSN_OK)r=ksn_source_register(&native->registry,
                                                &entry->provider,&entry->handle);
            if(r==KSN_OK&&source->source_registered)
                source->source_registered(storage,entry->handle);
            if(r==KSN_OK)r=ksn_source_subscribe(&native->registry,entry->handle,
                consumer,definition,source->source_bindings,
                source->source_binding_count,&entry->subscription);
            if(r==KSN_OK&&(source_slots&entry->subscription.bound_slots))
                r=KSN_INVALID;
            if(r!=KSN_OK){free(s);return NULL;}
            source_slots|=entry->subscription.bound_slots;
            storage+=(source->source_bytes+7u)&~(size_t)7u;
        }
    }
    return s;
}
static void schema_note_submitted(schema_state *s,ksn_tx before){
    if(s->session.ticket.value&&s->session.ticket.value!=before.value){
        state->submitted=s->session.ticket;
        state->submitted_mode=s->session.pending_delta==KSN_SCHEMA_PATCHED?
                              KSN_PATCH:KSN_REPLACE;
        state->active=true;
        ksn_runtime_app_activate(state->lease);
    }
}
/* Keep the borrowed-value scratch off the ordinary mount hot path. */
static __attribute__((noinline)) ksn_result schema_refresh_native(
    schema_state *s,schema_sources *sources,bool *blocked){
    schema_source *native=&sources->entries[0];
    if(s->session.ticket.value){
        ksn_submission outcome=ksn_view_poll(view());
        /* A complete current snapshot can be reacquired after this ticket
         * settles. Do not read the producer or advance its cursor for values
         * that the session cannot preflight while a submission is pending. */
        if(outcome.ticket.value==s->session.ticket.value&&
           outcome.status==KSN_SUBMITTED){
            if(blocked)*blocked=true;
            return KSN_OK;
        }
        if(outcome.ticket.value==s->session.ticket.value&&
           outcome.status==KSN_PRESENTED&&native->pending_revision){
            ksn_result ack=ksn_source_presented(&native->subscription,
                                                  native->pending_revision);
            if(ack!=KSN_OK)return ack;
        }
    }
    ksn_schema_value effective[KSN_SCHEMA_MAX_SLOTS];
    ksn_source_lease lease={0};
    uint64_t revision=s->revision;
    ksn_result source=ksn_source_acquire(&sources->registry,&native->subscription,
        s->definition,s->values,owner_now_us,effective,&lease);
    if(source!=KSN_OK)return source;
    if(lease.dirty_slots){
        if(revision==UINT64_MAX){ksn_source_release(&lease);return KSN_LIMIT;}
        revision++;
    }
    ksn_tx before=s->session.ticket;
    ksn_result r=ksn_schema_session_step_dirty(&s->session,view(),viewport,effective,
        revision,s->pending_base_slots|lease.dirty_slots,blocked);
    if(r==KSN_OK){
        s->revision=revision;
        r=ksn_source_commit(&lease);
        if(r==KSN_OK){
            s->pending_base_slots=0;
            sources->expiry_us=lease.snapshot.expires_at_us>owner_now_us?
                               lease.snapshot.expires_at_us:0;
        }
    }
    ksn_source_release(&lease);
    if(r==KSN_OK&&s->session.ticket.value&&
       s->session.ticket.value!=before.value){
        native->pending_revision=native->subscription.validated_revision;
        schema_note_submitted(s,before);
    }
    return r;
}
/* Multiple producers are composed only on assets that declare them. The
 * single-source clock keeps its smaller lease and stack frame above. */
static __attribute__((noinline)) ksn_result schema_refresh_native_many(
    schema_state *s,schema_sources *sources,schema_externals *external,bool *blocked){
    schema_source *active_static[KSN_SOURCE_MAX_REGISTERED];
    uint8_t static_count=0;
    if(sources)for(unsigned i=0;i<s->asset->source_count;i++)
        if(sources->entries[i].subscription.binding_count)
            active_static[static_count++]=&sources->entries[i];
    uint8_t external_count=external?external->count:0;
    uint8_t count=static_count+external_count;
    if(s->session.ticket.value){
        ksn_submission outcome=ksn_view_poll(view());
        if(outcome.ticket.value==s->session.ticket.value&&
           outcome.status==KSN_SUBMITTED){
            if(blocked)*blocked=true;
            return KSN_OK;
        }
        if(outcome.ticket.value==s->session.ticket.value&&
           outcome.status==KSN_PRESENTED){
            for(unsigned i=0;i<static_count;i++)if(active_static[i]->pending_revision){
                ksn_result ack=ksn_source_presented(&active_static[i]->subscription,
                                                      active_static[i]->pending_revision);
                if(ack!=KSN_OK)return ack;
            }
            for(unsigned i=0;i<external_count;i++)
                if(external->entries[i].pending_revision){
                    ksn_result ack=ksn_source_presented(
                        &external->entries[i].subscription,
                        external->entries[i].pending_revision);
                    if(ack!=KSN_OK)return ack;
                }
        }
    }
    ksn_source_member members[KSN_SOURCE_MAX_REGISTERED];
    for(unsigned i=0;i<static_count;i++)members[i]=(ksn_source_member){
        &sources->registry,&active_static[i]->subscription};
    for(unsigned i=0;i<external_count;i++)members[static_count+i]=
        (ksn_source_member){external->entries[i].registry,
                            &external->entries[i].subscription};
    ksn_schema_value effective[KSN_SCHEMA_MAX_SLOTS];
    ksn_source_bundle bundle={0};
    ksn_result source=ksn_source_bundle_acquire(members,count,s->definition,
        s->values,owner_now_us,effective,&bundle);
    if(source!=KSN_OK)return source;
    uint64_t revision=s->revision;
    if(bundle.dirty_slots){
        if(revision==UINT64_MAX){ksn_source_bundle_release(&bundle);return KSN_LIMIT;}
        revision++;
    }
    ksn_tx before=s->session.ticket;
    ksn_result r=ksn_schema_session_step_dirty(&s->session,view(),viewport,effective,
        revision,s->pending_base_slots|bundle.dirty_slots,blocked);
    if(r==KSN_OK){
        s->revision=revision;
        r=ksn_source_bundle_commit(&bundle);
        if(r==KSN_OK){
            s->pending_base_slots=0;
            uint64_t next=UINT64_MAX;
            for(unsigned i=0;i<static_count;i++){
                uint64_t expiry=bundle.leases[i].snapshot.expires_at_us;
                if(expiry>owner_now_us&&expiry<next)next=expiry;
            }
            if(sources)sources->expiry_us=next==UINT64_MAX?0:next;
            next=UINT64_MAX;
            for(unsigned i=0;i<external_count;i++){
                uint64_t expiry=bundle.leases[static_count+i].snapshot.expires_at_us;
                if(expiry>owner_now_us&&expiry<next)next=expiry;
            }
            if(external)external->expiry_us=next==UINT64_MAX?0:next;
        }
    }
    ksn_source_bundle_release(&bundle);
    if(r==KSN_OK&&s->session.ticket.value&&
        s->session.ticket.value!=before.value){
        for(unsigned i=0;i<static_count;i++)
            active_static[i]->pending_revision=
                active_static[i]->subscription.validated_revision;
        for(unsigned i=0;i<external_count;i++)
            external->entries[i].pending_revision=
                external->entries[i].subscription.validated_revision;
        schema_note_submitted(s,before);
    }
    return r;
}
static ksn_result schema_refresh(bool *blocked){
    if(blocked)*blocked=false;
    if(!state||!state->schema)return KSN_OK;
    schema_state *s=state->schema;
    schema_sources *native=schema_native(s);
    schema_externals *external=schema_external_state(s);
    if(native&&s->asset->source_count==1&&(!external||!external->count)){
        if(native->entries[0].subscription.binding_count){
            ksn_result r=schema_refresh_native(s,native,blocked);
            if(r==KSN_STALE&&schema_prune_stale(s))return schema_refresh(blocked);
            return r;
        }
    }else{
        unsigned active_static=0;
        if(native)for(unsigned i=0;i<s->asset->source_count;i++)
            if(native->entries[i].subscription.binding_count)active_static++;
        if(active_static||(external&&external->count)){
            ksn_result r=schema_refresh_native_many(s,native,external,blocked);
            if(r==KSN_STALE&&schema_prune_stale(s))return schema_refresh(blocked);
            return r;
        }
    }
    ksn_tx before=s->session.ticket;
    ksn_result r=ksn_schema_session_step_dirty(&s->session,view(),viewport,s->values,
        s->revision,s->pending_base_slots,blocked);
    if(r==KSN_OK)s->pending_base_slots=0;
    if(r==KSN_OK)schema_note_submitted(s,before);
    return r;
}

ksn_result pocket_kasane_presenter_host_status(const char *text,size_t bytes,uint64_t until_us){
    if(!state||!state->provider||!state->provider->host_status)return KSN_OK;
    return state->provider->host_status(state->provider_state,text,bytes,until_us);
}

ksn_result pocket_kasane_presenter_settle(bool *blocked){
    if(blocked)*blocked=false;
    if(state&&state->schema){
        schema_state *s=state->schema;
        if(s->session.ticket.value){
            ksn_submission outcome=ksn_view_poll(view());
            if(outcome.ticket.value!=s->session.ticket.value)return KSN_STALE;
            if(outcome.status==KSN_SUBMITTED){
                if(blocked)*blocked=true;
                return KSN_OK;
            }
            if(outcome.status==KSN_PRESENTED){
                schema_sources *native=schema_native(s);
                if(native)for(unsigned i=0;i<s->asset->source_count;i++){
                    schema_source *entry=&native->entries[i];
                    if(!entry->pending_revision)continue;
                    ksn_result r=ksn_source_presented(&entry->subscription,
                                                       entry->pending_revision);
                    if(r!=KSN_OK)return r;
                    entry->pending_revision=0;
                }
                schema_externals *external=schema_external_state(s);
                if(external)for(unsigned i=0;i<external->count;i++){
                    schema_external *entry=&external->entries[i];
                    if(!entry->pending_revision)continue;
                    ksn_result r=ksn_source_presented(&entry->subscription,
                                                       entry->pending_revision);
                    if(r!=KSN_OK)return r;
                    entry->pending_revision=0;
                }
            }
        }
        return ksn_schema_session_settle(&s->session,view(),blocked);
    }
    if(!state||!state->provider||!state->provider->settle)return KSN_OK;
    return state->provider->settle(state->provider_state,blocked);
}

ksn_result pocket_kasane_presenter_step(bool *blocked){
    if(blocked)*blocked=false;
    if(state&&state->schema){
        ksn_result r=schema_refresh(blocked);
        return r==KSN_BUSY?KSN_OK:r;
    }
    if(!state||!state->provider)return KSN_OK;
    return state->provider->step(state->provider_state,blocked);
}

// An int-tagged value needs no rounding, finiteness or integrality test, only
// the range: answered without the soft-double helpers (__floatsidf, __ltdf2,
// __gtdf2, __fixdfsi) that the double path calls -- ~300 calls a STRESS turn
// for coordinates that are almost always ints (docs/vm/qjs-observe.md sec.3.1).
static bool number_in(JSContext *ctx, JSValueConst value, double lo, double hi,
                      double *out) {
    double n;
    if(JS_VALUE_GET_TAG(value)==JS_TAG_INT){
        const int32_t i=JS_VALUE_GET_INT(value);
        if(i<lo||i>hi) return false;
        *out=i;
        return true;
    }
    if(!JS_IsNumber(value)||JS_ToFloat64(ctx,&n,value)<0||!isfinite(n)||
       floor(n)!=n||n<lo||n>hi) return false;
    *out=n;
    return true;
}
#include "pocket_kasane_parse.inc"
#include "pocket_kasane_tx.inc"
#include "pocket_kasane_scene.inc"
#include "pocket_kasane_schema.inc"
/* QuickJS keeps these entries for lazy function materialization. */
static const JSCFunctionListEntry cache_functions[]={
    JS_CFUNC_DEF("create",1,js_cache_create),JS_CFUNC_DEF("release",1,js_cache_release),
};
/* cache is an object entry of the list: the fork's lazy lists build it on the
 * first touch of the name (api-reorg.md sec.3.1), so HELLO's mount never
 * pays for it. grid, video and pixel are added the same way by their own
 * files (pocket_grid_install & co.). */
static const JSCFunctionListEntry functions[]={
    JS_OBJECT_DEF("cache",cache_functions,2,JS_PROP_C_W_E),
    JS_CFUNC_DEF("resource",1,js_resource),
    JS_CFUNC_DEF("mount",1,js_presenter_mount),
    JS_CFUNC_DEF("replace",1,js_replace),JS_CFUNC_DEF("patch",1,js_patch),
    JS_CFUNC_DEF("stats",0,js_stats),
};

static bool register_class(JSContext *ctx, JSClassID *id, pocket_class_owner_t *owner,
                           const JSClassDef *def) {
    JSRuntime *rt=JS_GetRuntime(ctx);
    if(pocket_api_class_ready(rt,owner,id)) return true;
    JS_NewClassID(rt,id);
    return JS_NewClass(rt,*id,def)>=0;
}
static bool set_proto(JSContext *ctx, JSClassID id, const JSCFunctionListEntry *methods,
                      int count) {
    JSValue proto=JS_NewObject(ctx);
    if(JS_IsException(proto)) return false;
    /* JS_InstantiateFunctionListItem drops the result of adding an autoinit
     * property, so an out-of-memory shows only as a pending exception. */
    if(JS_SetPropertyFunctionList(ctx,proto,methods,count)<0||JS_HasException(ctx)) {
        JS_FreeValue(ctx,proto);return false;
    }
    JS_SetClassProto(ctx,id,proto);return true;
}
static bool lazy_proto(JSContext *ctx, JSClassID id, const JSCFunctionListEntry *methods,
                       int count) {
    JSValue proto=JS_GetClassProto(ctx,id);
    bool ready=!JS_IsNull(proto);
    JS_FreeValue(ctx,proto);
    return ready||set_proto(ctx,id,methods,count);
}
#define COUNT(methods) ((int)(sizeof(methods)/sizeof(methods[0])))
static bool tx_proto(JSContext *ctx) {
    return lazy_proto(ctx,tx_class,tx_methods,COUNT(tx_methods));
}
static bool modal_proto(JSContext *ctx) {
    return lazy_proto(ctx,modal_class,modal_methods,COUNT(modal_methods));
}
static bool ref_proto(JSContext *ctx) {
    return lazy_proto(ctx,ref_class,ref_methods,COUNT(ref_methods));
}
static bool instance_proto(JSContext *ctx) {
    return lazy_proto(ctx,instance_class,instance_methods,COUNT(instance_methods));
}
static bool animation_proto(JSContext *ctx) {
    return lazy_proto(ctx,animation_class,animation_methods,COUNT(animation_methods));
}

// Weak: the host tests that build this file without the Shiki API link
// without it (the namespace is then never installed and the capability not
// published).
__attribute__((weak)) esp_err_t pocket_shiki_api_install(JSContext *ctx,JSValueConst kasane);
__attribute__((weak)) esp_err_t pocket_shiki_api_register(void);
__attribute__((weak)) bool shiki_heap_active(void);
static esp_err_t build_kasane(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    /* A new session's namespace: wrappers still remembered belong to a guest
     * that is gone without a reset (only host fixtures do that). Forget them;
     * freeing into a dead runtime is not possible. */
    build_ctx=NULL;
    /* Instance and animation prototypes are built on first use (above). */
    if(!register_class(ctx,&tx_class,&tx_owner,&tx_def)||
       !register_class(ctx,&modal_class,&modal_owner,&modal_def)||
       !register_class(ctx,&ref_class,&ref_owner,&ref_def)||
       !register_class(ctx,&instance_class,&instance_owner,&instance_def)||
       !register_class(ctx,&animation_class,&animation_owner,&animation_def)||
       !register_class(ctx,&template_class,&template_owner,&template_def)||
       !register_class(ctx,&image_class,&image_owner,&image_def)||
       !register_class(ctx,&schema_class,&schema_class_owner,&schema_def)) return ESP_ERR_NO_MEM;
    /* Handles with no methods,
     * inherit Object.prototype directly instead of each owning an empty object. */
    JSValue plain=JS_NewObject(ctx);
    if(JS_IsException(plain)) return ESP_ERR_NO_MEM;
    JSValue object_proto=JS_GetPrototype(ctx,plain);
    JS_FreeValue(ctx,plain);
    JS_SetClassProto(ctx,template_class,JS_DupValue(ctx,object_proto));
    JS_SetClassProto(ctx,image_class,JS_DupValue(ctx,object_proto));
    JS_FreeValue(ctx,object_proto);
    if(JS_SetPropertyFunctionList(ctx,ns,functions,
       (int)(sizeof(functions)/sizeof(functions[0])))<0) return ESP_ERR_NO_MEM;
    if(pocket_grid_install(ctx,ns)!=ESP_OK)return ESP_ERR_NO_MEM;
    if(pocket_video_install(ctx,ns)!=ESP_OK)return ESP_ERR_NO_MEM;
    if(pocket_pixel_install(ctx,ns)!=ESP_OK)return ESP_ERR_NO_MEM;
    // kasane.shiki last: it shares the grid member above.
    // Only a session that has a Shiki heap gets it -- the one whose manifest
    // names kasane.shiki (app_session.c takes the heap before the guest).
    if(pocket_shiki_api_install&&shiki_heap_active&&shiki_heap_active())
        return pocket_shiki_api_install(ctx,ns);
    return ESP_OK;
}

static const pocket_limit_t kasane_limits[]={
    {.name="commands",.kind=POCKET_LIMIT_INT,.number=KSN_APP_COMMANDS},
    {.name="references",.kind=POCKET_LIMIT_INT,.number=KASANE_REF_LIMIT},
    {.name="cacheTemplates",.kind=POCKET_LIMIT_INT,.number=KSN_CACHE_TEMPLATES},
    {.name="cacheInstances",.kind=POCKET_LIMIT_INT,.number=KSN_CACHE_INSTANCES},
    /* api-reorg.md sec.3.2: what the removed features() reported, each enforced --
     * text bytes of a scene (ksn_core.h), animation tracks, the commands of
     * one cache template (ksn_cache.h). */
    {.name="textBytes",.kind=POCKET_LIMIT_INT,.number=KSN_APP_TEXT_BYTES},
    {.name="animations",.kind=POCKET_LIMIT_INT,.number=KSN_APP_TRACKS},
    {.name="cacheCommands",.kind=POCKET_LIMIT_INT,.number=KSN_CACHE_COMMANDS},
    {0},
};
static const pocket_capability_t capability={
    .name="display.kasane",.supported=true,.available=true,.limits=kasane_limits,
};

esp_err_t pocket_kasane_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    esp_err_t result=pocket_api_register(&capability);
    if(result==ESP_OK&&pocket_shiki_api_register)result=pocket_shiki_api_register();
    return result==ESP_OK?pocket_api_lazy(ctx,"kasane",build_kasane,NULL):result;
}
JSValue pocket_kasane_source_capability(JSContext *ctx,ksn_source_registry *registry,
                                         ksn_source_handle handle){
    const char *op="kasane.sourceCapability";
    if(!registry||handle.index>=KSN_SOURCE_MAX_REGISTERED||!handle.generation||
       !registry->entries[handle.index].provider||
       registry->entries[handle.index].generation!=handle.generation)
        return throw_result(ctx,KSN_STALE,op);
    if(!ensure_state(ctx,op))return JS_EXCEPTION;
    if(!register_class(ctx,&source_cap_class,&source_cap_owner,&source_cap_def))
        return throw_result(ctx,KSN_OOM,op);
    JSValue proto=JS_GetClassProto(ctx,source_cap_class);
    if(JS_IsNull(proto)){
        JS_FreeValue(ctx,proto);
        proto=JS_NewObject(ctx);
        if(JS_IsException(proto))return proto;
        JS_SetClassProto(ctx,source_cap_class,proto);
    }else JS_FreeValue(ctx,proto);
    if(!source_caps){
        source_caps=calloc(1,sizeof(*source_caps));
        if(!source_caps)return throw_result(ctx,KSN_OOM,op);
    }
    source_cap_record *record=NULL;
    for(unsigned i=0;i<source_caps->count;i++)
        if(source_caps->records[i].registry==registry&&
           source_caps->records[i].handle.index==handle.index&&
           source_caps->records[i].handle.generation==handle.generation){
            record=&source_caps->records[i];break;
        }
    unsigned slot=source_caps->count;
    if(!record){
        if(source_cap_serial==UINT32_MAX)return throw_result(ctx,KSN_LIMIT,op);
        if(slot==KSN_SOURCE_MAX_REGISTERED){
            for(unsigned i=0;i<source_caps->count;i++)
                if(schema_handle_stale(source_caps->records[i].registry,
                                       source_caps->records[i].handle)){
                    slot=i;break;
                }
            if(slot==KSN_SOURCE_MAX_REGISTERED)return throw_result(ctx,KSN_LIMIT,op);
        }
    }
    JSValue object=JS_NewObjectClass(ctx,source_cap_class);
    if(JS_IsException(object))return object;
    if(!record){
        record=&source_caps->records[slot];
        if(slot==source_caps->count)source_caps->count++;
        *record=(source_cap_record){registry,handle,++source_cap_serial};
    }
    JS_SetOpaque(object,(void *)(uintptr_t)record->serial);
    return object;
}

// Resident suspension; see pocket_kasane_set_dormant().
static bool dormant;

bool pocket_kasane_reset(void) {
    /* First, and whatever the detach below says: they are guest values. */
    release_build_wrappers();
    dormant=false;
    const pocket_app_view_provider *provider=state?state->provider:NULL;
    void *provider_state=state?state->provider_state:NULL;
    schema_state *schema=state?state->schema:NULL;
    if(state&&ksn_runtime_app_detach(state->lease)==KSN_BUSY)return false;
    pocket_proc_reset();
    pocket_grid_reset();
    pocket_video_reset();
    pocket_pixel_reset();
    if(provider)provider->destroy(provider_state);
    if(schema)free(schema_external_state(schema));
    if(schema)free(schema->owned_asset);
    free(schema);
    free(source_caps);source_caps=NULL;
    state=NULL;   /* the runtime released it with the lease */
    viewport=KASANE_SCREEN;
    overlay_profile=false;
    owner_now_us=0;
#ifdef KASANE_PROC_JS_DIAGNOSTIC
    proc_log_frame=0;
#endif
    return true;
}
// Resident suspension (docs/vm/app-suspend-design.md sec.4). The APP lease is
// kept -- the provider, schema and refs live in its tail, and detaching would
// free them under the sleeping app -- but for as long as the app sleeps the
// host screens must see no APP at all: no notice composited into it, no
// source deadline waking the loop, no system bands claimed. Home then draws
// notices natively, as it does with no app. The wake repaints everything.
void pocket_kasane_set_dormant(bool on) {
    dormant=on;
    if(!on) pocket_kasane_invalidate();
}

ksn_result pocket_kasane_proc_publish(void){
    if(overlay_profile&&!pocket_proc_is_image_mode())return KSN_INVALID;
    if(!state){
        ksn_result result=attach_state("procedural.commit");
        if(result!=KSN_OK)return result;
    }
    if(!ksn_runtime_app_view(state->lease))return KSN_STALE;
    state->active=true;
    ksn_runtime_app_activate(state->lease);
    if(pocket_proc_is_image_mode()){
        if(state->proc_resource[0].value){
            ksn_runtime_invalidate_image(state->proc_resource[0]);
            pocket_grid_source_invalidated(state->proc_resource[0].value);
        }
    }else ksn_runtime_invalidate();
    return KSN_OK;
}
ksn_result pocket_kasane_proc_publish_at(unsigned surface,ksn_rect source_damage){
    if(surface>=2)return KSN_INVALID;
    if(!state){
        ksn_result result=attach_state("procedural.commit");
        if(result!=KSN_OK)return result;
    }
    if(!ksn_runtime_app_view(state->lease)||!state->proc_resource[surface].value)
        return KSN_STALE;
    state->active=true;
    ksn_runtime_app_activate(state->lease);
    if(source_damage.x0<source_damage.x1&&source_damage.y0<source_damage.y1){
        /* The bands whose drawing changed, where the frame says (a 1:1 node
         * then sends only those), else the one bounding rectangle. */
        ksn_damage bands={0};
        if(pocket_proc_band_damage(surface,&bands.bands,bands.x0,bands.x1)){
            if(bands.bands)ksn_runtime_invalidate_image_source_damage(state->proc_resource[surface],&bands);
        }else ksn_runtime_invalidate_image_source_rect(state->proc_resource[surface],source_damage);
        pocket_grid_source_invalidated(state->proc_resource[surface].value);
    }
    return KSN_OK;
}
bool pocket_kasane_active(void) { return !dormant&&state&&state->active; }
#include "pocket_kasane_present.inc"
