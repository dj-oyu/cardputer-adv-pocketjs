#include "pocket_kasane.h"
#include "pocket_api.h"
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
static JSClassID tx_class, modal_class, ref_class, template_class;
static JSClassID instance_class, ticket_class, image_class, animation_class;
static JSClassID schema_class;
static JSClassID source_cap_class;
static JSRuntime *tx_rt, *modal_rt, *ref_rt, *template_rt;
static JSRuntime *instance_rt, *ticket_rt, *image_rt, *animation_rt;
static JSRuntime *schema_rt;
static JSRuntime *source_cap_rt;
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
static const JSClassDef ticket_def={.class_name=KASANE_CLASS};
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
            s->values[i].data.number=slot->initial_number;
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

ksn_result pocket_kasane_presenter_step(bool *blocked){
    if(blocked)*blocked=false;
    if(state&&state->schema){
        ksn_result r=schema_refresh(blocked);
        return r==KSN_BUSY?KSN_OK:r;
    }
    if(!state||!state->provider)return KSN_OK;
    return state->provider->step(state->provider_state,blocked);
}

static bool number_in(JSContext *ctx, JSValueConst value, double lo, double hi,
                      double *out) {
    double n;
    if(!JS_IsNumber(value)||JS_ToFloat64(ctx,&n,value)<0||!isfinite(n)||
       floor(n)!=n||n<lo||n>hi) return false;
    *out=n;
    return true;
}

static bool parse_i16(JSContext *ctx, JSValueConst value, int16_t *out) {
    double n;
    if(!JS_IsNumber(value)||JS_ToFloat64(ctx,&n,value)<0||!isfinite(n)) return false;
    n=round(n);
    if(n<INT16_MIN||n>INT16_MAX)return false;
    *out=(int16_t)n;
    return true;
}

static bool parse_u8(JSContext *ctx, JSValueConst value, uint8_t *out) {
    double n;
    if(!number_in(ctx,value,0,255,&n)) return false;
    *out=(uint8_t)n;
    return true;
}

static bool parse_u32(JSContext *ctx, JSValueConst value, uint32_t *out) {
    double n;
    if(!number_in(ctx,value,0,4294967295.0,&n)) return false;
    *out=(uint32_t)n;
    return true;
}

static bool parse_rect_mode(JSContext *ctx, JSValueConst value, ksn_rect *out,
                            const char *op,bool translate) {
    int64_t length=0;
    if(JS_IsArray(value)&&JS_GetLength(ctx,value,&length)<0) return false;
    if(!JS_IsArray(value)||length!=4) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "rectangle must be [x0, y0, x1, y1]",false,NULL);
        return false;
    }
    int16_t coords[4];
    for(unsigned i=0;i<4;i++) {
        JSValue item=JS_GetPropertyUint32(ctx,value,i);
        if(JS_IsException(item)) return false;
        bool ok=parse_i16(ctx,item,&coords[i]);
        JS_FreeValue(ctx,item);
        if(!ok) {
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "rounded coordinates must fit int16",false,NULL);
            return false;
        }
    }
    *out=(ksn_rect){coords[0],coords[1],coords[2],coords[3]};
    if(out->x0>out->x1||out->y0>out->y1) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "rectangle edges are reversed",false,NULL);
        return false;
    }
    if(translate)viewport_rect(out);
    return true;
}

static bool parse_rect(JSContext *ctx, JSValueConst value, ksn_rect *out,
                       const char *op) {
    return parse_rect_mode(ctx,value,out,op,true);
}

static bool property_rect(JSContext *ctx, JSValueConst object, const char *name,
                          ksn_rect fallback, ksn_rect *out, const char *op) {
    JSValue value=JS_GetPropertyStr(ctx,object,name);
    if(JS_IsException(value)) return false;
    if(JS_IsUndefined(value)) { *out=fallback; JS_FreeValue(ctx,value); return true; }
    bool ok=parse_rect(ctx,value,out,op);
    JS_FreeValue(ctx,value);
    return ok;
}

static bool property_local_rect(JSContext *ctx,JSValueConst object,const char *name,
                                ksn_rect fallback,ksn_rect *out,const char *op) {
    JSValue value=JS_GetPropertyStr(ctx,object,name);
    if(JS_IsException(value))return false;
    if(JS_IsUndefined(value)){*out=fallback;JS_FreeValue(ctx,value);return true;}
    bool ok=parse_rect_mode(ctx,value,out,op,false);
    JS_FreeValue(ctx,value);return ok;
}

static bool property_u8(JSContext *ctx, JSValueConst object, const char *name,
                        uint8_t fallback, uint8_t *out, const char *op) {
    JSValue value=JS_GetPropertyStr(ctx,object,name);
    if(JS_IsException(value)) return false;
    if(JS_IsUndefined(value)) { *out=fallback; JS_FreeValue(ctx,value); return true; }
    bool ok=parse_u8(ctx,value,out);
    JS_FreeValue(ctx,value);
    if(!ok) pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "value must be an integer from 0 to 255",false,NULL);
    return ok;
}

static bool parse_draw_base(JSContext *ctx, JSValueConst value, ksn_draw *out,
                            const char *op) {
    if(!JS_IsObject(value)) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "rectangle must be an object",false,NULL);
        return false;
    }
    JSValue bounds=JS_GetPropertyStr(ctx,value,"bounds");
    bool ok=!JS_IsException(bounds)&&parse_rect(ctx,bounds,&out->bounds,op);
    JS_FreeValue(ctx,bounds);
    if(!ok) return false;
    out->kind=KSN_RECT;
    out->clip=out->bounds;
    if(!property_rect(ctx,value,"clip",out->bounds,&out->clip,op)) return false;
    viewport_clip(&out->clip);
    if(!property_u8(ctx,value,"opacity",255,&out->opacity,op)) return false;
    return true;
}

static bool property_color(JSContext *ctx,JSValueConst value,const char *name,
                           ksn_rgba *out,const char *op) {
    JSValue color=JS_GetPropertyStr(ctx,value,name);
    if(JS_IsException(color)) return false;
    bool ok=parse_u32(ctx,color,out);
    JS_FreeValue(ctx,color);
    if(!ok) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "color must be an RRGGBBAA uint32",false,NULL);
        return false;
    }
    return true;
}

static bool parse_draw(JSContext *ctx, JSValueConst value, ksn_draw *out,
                       const char *op) {
    return parse_draw_base(ctx,value,out,op)&&
           property_color(ctx,value,"color",&out->data.shape.color,op);
}

/* Cache templates live in their own local coordinate space. Translating or
 * clipping them here destroys pixels that a later placement moves into the
 * viewport. The placement supplies the viewport origin and final clip. */
static bool parse_cache_draw(JSContext *ctx,JSValueConst value,ksn_draw *out,
                             const char *op) {
    if(!JS_IsObject(value)) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "rectangle must be an object",false,NULL);
        return false;
    }
    JSValue bounds=JS_GetPropertyStr(ctx,value,"bounds");
    bool ok=!JS_IsException(bounds)&&
        parse_rect_mode(ctx,bounds,&out->bounds,op,false);
    JS_FreeValue(ctx,bounds);
    if(!ok)return false;
    out->kind=KSN_RECT;out->clip=out->bounds;
    if(!property_local_rect(ctx,value,"clip",out->bounds,&out->clip,op)||
       !property_u8(ctx,value,"opacity",255,&out->opacity,op))return false;
    return property_color(ctx,value,"color",&out->data.shape.color,op);
}

static bool parse_primitive(JSContext *ctx,JSValueConst value,ksn_kind kind,
                            ksn_draw *out,const char *op) {
    if(kind!=KSN_GRADIENT) {
        if(!parse_draw(ctx,value,out,op))return false;
        out->kind=kind;
        if(kind==KSN_ROUND_RECT)
            return property_u8(ctx,value,"radius",0,&out->data.shape.radius,op);
        if(kind==KSN_STROKE)
            return property_u8(ctx,value,"width",1,&out->data.shape.width,op);
        return true;
    }
    if(!parse_draw_base(ctx,value,out,op))return false;
    out->kind=kind;
    if(!property_color(ctx,value,"from",&out->data.gradient.from,op)||
       !property_color(ctx,value,"to",&out->data.gradient.to,op)||
       !property_u8(ctx,value,"radius",0,&out->data.gradient.radius,op))return false;
    JSValue axis=JS_GetPropertyStr(ctx,value,"axis");
    if(JS_IsException(axis))return false;
    bool valid=JS_IsUndefined(axis);
    out->data.gradient.axis=1;
    if(JS_IsString(axis)) {
        const char *name=JS_ToCString(ctx,axis);
        if(!name){JS_FreeValue(ctx,axis);return false;}
        valid=!strcmp(name,"x")||!strcmp(name,"y");
        out->data.gradient.axis=!strcmp(name,"y");
        JS_FreeCString(ctx,name);
    }
    JS_FreeValue(ctx,axis);
    if(!valid){throw_result(ctx,KSN_INVALID,op);return false;}
    JSValue dither=JS_GetPropertyStr(ctx,value,"dither");
    if(JS_IsException(dither))return false;
    valid=JS_IsUndefined(dither)||JS_IsBool(dither);
    out->data.gradient.dither=JS_IsBool(dither)&&JS_ToBool(ctx,dither);
    JS_FreeValue(ctx,dither);
    if(!valid)throw_result(ctx,KSN_INVALID,op);
    return valid;
}

static bool parse_placement(JSContext *ctx, JSValueConst value, ksn_placement *out,
                            const char *op) {
    *out=(ksn_placement){.x=viewport.x0,.y=viewport.y0,.clip=viewport,
                         .opacity=255,.visible=true};
    if(JS_IsUndefined(value)) return true;
    if(!JS_IsObject(value)) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "placement must be an object",false,NULL);
        return false;
    }
    JSValue offset=JS_GetPropertyStr(ctx,value,"offset");
    if(JS_IsException(offset)) return false;
    if(!JS_IsUndefined(offset)) {
        int64_t length=0;
        if(JS_IsArray(offset)&&JS_GetLength(ctx,offset,&length)<0) {
            JS_FreeValue(ctx,offset);return false;
        }
        if(!JS_IsArray(offset)||length!=2) {
            JS_FreeValue(ctx,offset);
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "offset must be [x, y]",false,NULL);
            return false;
        }
        JSValue x=JS_GetPropertyUint32(ctx,offset,0);
        if(JS_IsException(x)) { JS_FreeValue(ctx,offset);return false; }
        JSValue y=JS_GetPropertyUint32(ctx,offset,1);
        if(JS_IsException(y)) {
            JS_FreeValue(ctx,x);JS_FreeValue(ctx,offset);return false;
        }
        int16_t local_x=0,local_y=0;
        bool ok=parse_i16(ctx,x,&local_x)&&parse_i16(ctx,y,&local_y);
        JS_FreeValue(ctx,x);JS_FreeValue(ctx,y);JS_FreeValue(ctx,offset);
        int32_t absolute_x=(int32_t)local_x+viewport.x0;
        int32_t absolute_y=(int32_t)local_y+viewport.y0;
        if(!ok||absolute_x<INT16_MIN||absolute_x>INT16_MAX||
           absolute_y<INT16_MIN||absolute_y>INT16_MAX) {
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "offset coordinates must be int16 values",false,NULL);
            return false;
        }
        out->x=(int16_t)absolute_x;out->y=(int16_t)absolute_y;
    } else JS_FreeValue(ctx,offset);
    if(!property_rect(ctx,value,"clip",viewport,&out->clip,op)) return false;
    viewport_clip(&out->clip);
    if(!property_u8(ctx,value,"opacity",255,&out->opacity,op)) return false;
    JSValue visible=JS_GetPropertyStr(ctx,value,"visible");
    if(JS_IsException(visible)) return false;
    if(!JS_IsUndefined(visible)) {
        if(!JS_IsBool(visible)) {
            JS_FreeValue(ctx,visible);
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "visible must be boolean",false,NULL);
            return false;
        }
        out->visible=JS_ToBool(ctx,visible);
    }
    JS_FreeValue(ctx,visible);
    return true;
}

static ksn_tx tx_from(JSContext *ctx, JSValueConst value, const char *op) {
    uint32_t raw=opaque_value(value,tx_class);
    if(!raw) pocket_api_throw(ctx,POCKET_ERR_CLOSED,op,
                              "transaction is outside its build callback",false,NULL);
    return (ksn_tx){raw};
}

/* Built on the first wrapper that needs them rather than with the namespace:
 * an app that never instantiates or animates keeps neither prototype, its
 * shape, nor the method-name atoms. Defined after the method tables. */
static bool instance_proto(JSContext *ctx);
static bool animation_proto(JSContext *ctx);

static JSValue wrap_direct(JSContext *ctx, JSClassID class_id, uint32_t handle) {
    JSValue object=JS_NewObjectClass(ctx,class_id);
    if(JS_IsException(object)) return object;
    JS_SetOpaque(object,(void *)(uintptr_t)handle);
    return object;
}

static JSValue js_tx_background(JSContext *ctx, JSValueConst self, int argc,
                                JSValueConst *argv) {
    ksn_tx tx=tx_from(ctx,self,"kasane.background");
    if(!tx.value) return JS_EXCEPTION;
    uint32_t color;
    if(argc<1||!parse_u32(ctx,argv[0],&color))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.background",
                                "color must be an RRGGBBAA uint32",false,NULL);
    ksn_result result=ksn_view_background(view(),tx,color);
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,"kasane.background");
}

static JSValue expose_ref(JSContext *ctx,ksn_tx tx,ksn_ref ref) {
    ref_slot *slot=claim_ref(ctx,ref,tx);
    if(!slot) { ksn_view_cancel(view(),tx); discard_candidates(tx); return JS_EXCEPTION; }
    JSValue object=wrap_direct(ctx,ref_class,slot->handle);
    if(JS_IsException(object)) {
        *slot=(ref_slot){0};ksn_view_cancel(view(),tx);discard_candidates(tx);
    }
    return object;
}

static JSValue js_tx_primitive(JSContext *ctx, JSValueConst self, int argc,
                               JSValueConst *argv,ksn_kind kind,const char *op) {
    ksn_tx tx=tx_from(ctx,self,op);
    if(!tx.value) return JS_EXCEPTION;
    ksn_draw draw={0};
    if(!parse_primitive(ctx,argc?argv[0]:JS_UNDEFINED,kind,&draw,op)) return JS_EXCEPTION;
    ksn_ref ref;ksn_result result=ksn_view_add(view(),tx,&draw,&ref);
    return result==KSN_OK?expose_ref(ctx,tx,ref):throw_result(ctx,result,op);
}

/* Bound conversion before QuickJS allocates UTF-8 storage (at most 384 bytes
 * plus its string header). Core rejects lone surrogates and control scalars. */
static const char *text_string(JSContext *ctx,JSValueConst value,uint16_t *bytes,const char *op){
    if(!JS_IsString(value)){throw_result(ctx,KSN_INVALID,op);return NULL;}
    int64_t units;
    if(JS_GetLength(ctx,value,&units)<0)return NULL;
    if(units>128){throw_result(ctx,KSN_LIMIT,op);return NULL;}
    size_t length;const char *text=JS_ToCStringLen(ctx,&length,value);
    if(!text)return NULL;
    if(length>128){JS_FreeCString(ctx,text);throw_result(ctx,KSN_LIMIT,op);return NULL;}
    *bytes=(uint16_t)length;return text;
}

static bool property_u16(JSContext *ctx,JSValueConst spec,const char *key,uint16_t fallback,
                         uint16_t *out,const char *op){
    JSValue value=JS_GetPropertyStr(ctx,spec,key);
    if(JS_IsException(value))return false;
    double n=fallback;bool ok=JS_IsUndefined(value)||number_in(ctx,value,0,65535,&n);
    JS_FreeValue(ctx,value);
    if(!ok){throw_result(ctx,KSN_INVALID,op);return false;}
    *out=(uint16_t)n;return true;
}

static bool parse_angle(JSContext *ctx,JSValueConst value,int32_t *out,const char *op){
    double degrees;
    if(!JS_IsNumber(value)||JS_ToFloat64(ctx,&degrees,value)<0||!isfinite(degrees)||degrees<-32768||degrees>32767){
        throw_result(ctx,KSN_INVALID,op);return false;
    }
    *out=(int32_t)lround(degrees*(1024.0/360.0));return true;
}
static bool parse_rotation(JSContext *ctx,JSValueConst value,uint16_t *out,const char *op){
    int32_t turns;if(!parse_angle(ctx,value,&turns,op))return false;
    *out=(uint16_t)((turns%1024+1024)%1024);return true;
}
static JSValue js_resource(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *op="kasane.resource";
    const char *name=argc?bounded_cstring(ctx,argv[0],31):NULL;
    if(!name)return throw_result(ctx,KSN_INVALID,op);
    const pocket_app_image_asset *asset=pocket_app_image_lookup(name);
    JS_FreeCString(ctx,name);
    if(!asset)return throw_result(ctx,KSN_UNSUPPORTED,op);
    if(state&&state->building.value)return throw_result(ctx,KSN_BUSY,op);
    /* Finish every fallible JS allocation before reserving a native resource. */
    JSValue object=JS_NewObjectClass(ctx,image_class);
    if(JS_IsException(object))return object;
    const struct {const char *name;int value;} metadata[]={
        {"width",asset->width},{"height",asset->height},
        {"variants",asset->variants},{"frames",asset->frames}};
    for(unsigned i=0;i<sizeof(metadata)/sizeof(metadata[0]);i++)
        if(JS_DefinePropertyValueStr(ctx,object,metadata[i].name,
                JS_NewInt32(ctx,metadata[i].value),JS_PROP_ENUMERABLE)<0){
            JS_FreeValue(ctx,object);return JS_EXCEPTION;
    }
    if(!ensure_state(ctx,op)){JS_FreeValue(ctx,object);return JS_EXCEPTION;}
    unsigned index=0;
    for(;index<4;index++)if(state->images[index].asset==asset)break;
    if(index==4){
        for(index=0;index<4&&state->images[index].asset;index++);
        if(index==4){JS_FreeValue(ctx,object);return throw_result(ctx,KSN_LIMIT,op);}
        ksn_image_port port;ksn_result opened=asset->open(&port);
        if(opened!=KSN_OK){JS_FreeValue(ctx,object);return throw_result(ctx,opened,op);}
        if(port.width!=asset->width||port.height!=asset->height||
           port.variants!=asset->variants||port.frames!=asset->frames){
            JS_FreeValue(ctx,object);return throw_result(ctx,KSN_INVALID,op);
        }
        ksn_resource resource={0};
        ksn_result result=ksn_view_host_register_image(view(),&port,&resource);
        if(result!=KSN_OK){JS_FreeValue(ctx,object);return throw_result(ctx,result,op);}
        state->images[index].asset=asset;state->images[index].resource=resource;
    }
    JS_SetOpaque(object,(void *)(uintptr_t)state->images[index].resource.value);return object;
}

static JSValue js_tx_image(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="kasane.image";ksn_tx tx=tx_from(ctx,self,op);
    if(!tx.value)return JS_EXCEPTION;
    JSValueConst spec=argc?argv[0]:JS_UNDEFINED;ksn_draw draw={0};
    if(!parse_draw_base(ctx,spec,&draw,op))return JS_EXCEPTION;
    draw.kind=KSN_IMAGE;
    JSValue resource=JS_GetPropertyStr(ctx,spec,"resource");
    if(JS_IsException(resource))return resource;
    draw.data.image.resource.value=opaque_value(resource,image_class);JS_FreeValue(ctx,resource);
    if(!draw.data.image.resource.value)return throw_result(ctx,KSN_INVALID,op);
    if(!property_u16(ctx,spec,"variant",0,&draw.data.image.variant,op)||
       !property_u16(ctx,spec,"frame",0,&draw.data.image.frame,op)||
       !property_u16(ctx,spec,"sourceX",0,&draw.data.image.source_x,op)||
       !property_u16(ctx,spec,"sourceY",0,&draw.data.image.source_y,op))return JS_EXCEPTION;
    JSValue scale=JS_GetPropertyStr(ctx,spec,"scale");
    if(JS_IsException(scale))return scale;
    bool stretch=JS_IsUndefined(scale);
    double n=1;bool ok=stretch||(JS_IsNumber(scale)&&JS_ToFloat64(ctx,&n,scale)==0);
    JS_FreeValue(ctx,scale);
    if(!ok||(n!=0.5&&n!=1&&n!=2))return throw_result(ctx,KSN_INVALID,op);
    draw.data.image.scale=stretch?KSN_IMAGE_STRETCH:n==0.5?KSN_IMAGE_HALF:n==2?KSN_IMAGE_2X:KSN_IMAGE_1X;
    /* The currently exposed resource class contains only the 64x64 pet atlas.
     * Source extents stay fixed when setRect changes the destination. */
    uint16_t width=draw.data.image.source_x<64?64-draw.data.image.source_x:0;
    uint16_t height=draw.data.image.source_y<64?64-draw.data.image.source_y:0;
    if(!property_u16(ctx,spec,"sourceWidth",width,&draw.data.image.source_width,op)||
       !property_u16(ctx,spec,"sourceHeight",height,&draw.data.image.source_height,op))return JS_EXCEPTION;
    JSValue rotation=JS_GetPropertyStr(ctx,spec,"rotation");
    if(JS_IsException(rotation))return rotation;
    ok=JS_IsUndefined(rotation)||parse_rotation(ctx,rotation,&draw.data.image.rotation,op);
    JS_FreeValue(ctx,rotation);if(!ok)return JS_EXCEPTION;
    ksn_ref ref;ksn_result result=ksn_view_add(view(),tx,&draw,&ref);
    return result==KSN_OK?expose_ref(ctx,tx,ref):throw_result(ctx,result,op);
}

static JSValue js_tx_text(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="kasane.text";ksn_tx tx=tx_from(ctx,self,op);
    if(!tx.value)return JS_EXCEPTION;
    JSValueConst spec=argc?argv[0]:JS_UNDEFINED;ksn_draw draw={0};
    if(!parse_draw_base(ctx,spec,&draw,op)||
       !property_color(ctx,spec,"color",&draw.data.text.color,op))return JS_EXCEPTION;
    draw.kind=KSN_TEXT;draw.data.text.font=KSN_BODY;
    JSValue font=JS_GetPropertyStr(ctx,spec,"font");
    if(JS_IsException(font))return font;
    if(!JS_IsUndefined(font)){
        uint16_t bytes=0;const char *name=text_string(ctx,font,&bytes,op);
        if(!name){JS_FreeValue(ctx,font);return JS_EXCEPTION;}
        bool valid=true;
        if(bytes==7&&!memcmp(name,"caption",7))draw.data.text.font=KSN_CAPTION;
        else if(bytes==4&&!memcmp(name,"body",4))draw.data.text.font=KSN_BODY;
        else if(bytes==7&&!memcmp(name,"display",7))draw.data.text.font=KSN_DISPLAY;
        else valid=false;
        JS_FreeCString(ctx,name);JS_FreeValue(ctx,font);
        if(!valid)return throw_result(ctx,KSN_INVALID,op);
    }else JS_FreeValue(ctx,font);
    JSValue value=JS_GetPropertyStr(ctx,spec,"text");
    if(JS_IsException(value))return value;
    const char *text=text_string(ctx,value,&draw.data.text.bytes,op);JS_FreeValue(ctx,value);
    if(!text)return JS_EXCEPTION;
    draw.data.text.utf8=text;
    uint8_t capacity=draw.data.text.bytes?draw.data.text.bytes:1;
    if(!property_u8(ctx,spec,"capacity",capacity,&capacity,op)){JS_FreeCString(ctx,text);return JS_EXCEPTION;}
    draw.data.text.capacity=capacity;
    ksn_ref ref;ksn_result result=ksn_view_add(view(),tx,&draw,&ref);
    JS_FreeCString(ctx,text);
    return result==KSN_OK?expose_ref(ctx,tx,ref):throw_result(ctx,result,op);
}

#define PRIMITIVE(name,kind,op) \
static JSValue name(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){ \
    return js_tx_primitive(ctx,self,argc,argv,kind,op); \
}
PRIMITIVE(js_tx_rect,KSN_RECT,"kasane.rect")
PRIMITIVE(js_tx_round_rect,KSN_ROUND_RECT,"kasane.roundRect")
PRIMITIVE(js_tx_stroke_rect,KSN_STROKE,"kasane.strokeRect")
PRIMITIVE(js_tx_gradient,KSN_GRADIENT,"kasane.gradient")
#undef PRIMITIVE

static JSValue js_tx_group(JSContext *ctx, JSValueConst self, int argc,
                           JSValueConst *argv) {
    ksn_tx tx=tx_from(ctx,self,"kasane.group");
    if(!tx.value) return JS_EXCEPTION;
    if(argc<3)
        return throw_result(ctx,KSN_INVALID,"kasane.group");
    ref_slot *first=ref_from(ctx,argv[0],"kasane.group");
    double count_number;uint8_t opacity;
    if(!first) return JS_EXCEPTION;
    if(!number_in(ctx,argv[1],1,UINT16_MAX,&count_number)||
       !parse_u8(ctx,argv[2],&opacity))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.group",
                                "group(firstRef, count, opacity) requires integer values",
                                false,NULL);
    ksn_result result=ksn_view_group(view(),tx,first->ref,(uint16_t)count_number,opacity);
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,"kasane.group");
}

static JSValue js_tx_instantiate(JSContext *ctx, JSValueConst self, int argc,
                                 JSValueConst *argv) {
    ksn_tx tx=tx_from(ctx,self,"kasane.instantiate");
    if(!tx.value) return JS_EXCEPTION;
    uint32_t raw=argc?opaque_value(argv[0],template_class):0;
    ksn_placement placement;
    if(!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"kasane.instantiate",
                                "template is closed",false,NULL);
    if(!parse_placement(ctx,argc>1?argv[1]:JS_UNDEFINED,&placement,
                        "kasane.instantiate")) return JS_EXCEPTION;
    if(!instance_proto(ctx)) return JS_EXCEPTION;
    JSValue object=wrap_direct(ctx,instance_class,0);
    if(JS_IsException(object)) return object;
    ksn_instance instance;ksn_result result=ksn_view_instantiate(
        view(),tx,(ksn_template){raw},&placement,&instance);
    if(result!=KSN_OK) {
        JS_FreeValue(ctx,object);return throw_result(ctx,result,"kasane.instantiate");
    }
    JS_SetOpaque(object,(void *)(uintptr_t)instance.value);return object;
}

static JSValue change_ref(JSContext *ctx, JSValueConst self, int argc,
                          JSValueConst *argv, ksn_property property,
                          const char *op) {
    ref_slot *slot=ref_from(ctx,self,op);
    if(!slot) return JS_EXCEPTION;
    ksn_tx tx=argc?tx_from(ctx,argv[0],op):(ksn_tx){0};
    if(!tx.value) return JS_EXCEPTION;
    if(slot->status==REF_CANDIDATE&&slot->ticket.value!=tx.value)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,op,
                                "candidate reference belongs to another transaction",
                                false,NULL);
    ksn_change change={.property=property};
    if(property==KSN_SET_RECT||property==KSN_SET_CLIP) {
        if(!parse_rect(ctx,argc>1?argv[1]:JS_UNDEFINED,&change.value.rect,op)) return JS_EXCEPTION;
        if(property==KSN_SET_CLIP)viewport_clip(&change.value.rect);
    } else if(property==KSN_SET_COLOR) {
        if(argc<2||!parse_u32(ctx,argv[1],&change.value.color))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "color must be an RRGGBBAA uint32",false,NULL);
    } else if(property==KSN_SET_TEXT) {
        const char *text=text_string(ctx,argc>1?argv[1]:JS_UNDEFINED,&change.value.text.bytes,op);
        if(!text)return JS_EXCEPTION;
        change.value.text.utf8=text;
        ksn_result result=ksn_view_change(view(),tx,slot->ref,&change);
        JS_FreeCString(ctx,text);
        return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,op);
    } else if(property==KSN_SET_ROTATION) {
        if(argc<2)return throw_result(ctx,KSN_INVALID,op);
        if(!parse_rotation(ctx,argv[1],&change.value.rotation,op))return JS_EXCEPTION;
    } else if(property==KSN_SET_IMAGE_FRAME) {
        double variant,frame;
        if(argc<3||!number_in(ctx,argv[1],0,65535,&variant)||!number_in(ctx,argv[2],0,65535,&frame))
            return throw_result(ctx,KSN_INVALID,op);
        change.value.image.variant=(uint16_t)variant;change.value.image.frame=(uint16_t)frame;
    } else if(property==KSN_SET_REVEAL) {
        double n;
        if(argc<2||!number_in(ctx,argv[1],0,128,&n))return throw_result(ctx,KSN_INVALID,op);
        change.value.reveal=(uint16_t)n;
    } else {
        if(argc<2||!JS_IsBool(argv[1]))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "visible must be boolean",false,NULL);
        change.value.visible=JS_ToBool(ctx,argv[1]);
    }
    ksn_result result=ksn_view_change(view(),tx,slot->ref,&change);
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,op);
}

#define REF_SETTER(name,property,op) \
static JSValue name(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){ \
    return change_ref(ctx,self,argc,argv,property,op); \
}
REF_SETTER(js_ref_rect,KSN_SET_RECT,"kasane.ref.setRect")
REF_SETTER(js_ref_clip,KSN_SET_CLIP,"kasane.ref.setClip")
REF_SETTER(js_ref_color,KSN_SET_COLOR,"kasane.ref.setColor")
REF_SETTER(js_ref_visible,KSN_SET_VISIBLE,"kasane.ref.setVisible")
REF_SETTER(js_ref_text,KSN_SET_TEXT,"kasane.ref.setText")
REF_SETTER(js_ref_reveal,KSN_SET_REVEAL,"kasane.ref.setReveal")
REF_SETTER(js_ref_image,KSN_SET_IMAGE_FRAME,"kasane.ref.setImageFrame")
REF_SETTER(js_ref_rotation,KSN_SET_ROTATION,"kasane.ref.setRotation")

static bool parse_pose(JSContext *ctx,JSValueConst spec,const char *key,ksn_pose *out,const char *op){
    JSValue value=JS_GetPropertyStr(ctx,spec,key);
    if(JS_IsException(value))return false;
    if(!JS_IsObject(value)){JS_FreeValue(ctx,value);throw_result(ctx,KSN_INVALID,op);return false;}
    JSValue bounds=JS_GetPropertyStr(ctx,value,"bounds");
    bool ok=!JS_IsException(bounds)&&parse_rect(ctx,bounds,&out->bounds,op);JS_FreeValue(ctx,bounds);
    if(ok){JSValue angle=JS_GetPropertyStr(ctx,value,"rotation");
        ok=!JS_IsException(angle)&&(JS_IsUndefined(angle)||parse_angle(ctx,angle,&out->rotation,op));
        JS_FreeValue(ctx,angle);}
    JS_FreeValue(ctx,value);return ok;
}
static bool parse_choice(JSContext *ctx,JSValueConst spec,const char *key,const char *const *names,unsigned count,
                          unsigned *out,const char *op){
    JSValue value=JS_GetPropertyStr(ctx,spec,key);if(JS_IsException(value))return false;
    if(JS_IsUndefined(value)){*out=0;JS_FreeValue(ctx,value);return true;}
    uint16_t bytes;const char *s=text_string(ctx,value,&bytes,op);JS_FreeValue(ctx,value);if(!s)return false;
    unsigned i=0;while(i<count&&(strlen(names[i])!=bytes||memcmp(names[i],s,bytes)))i++;
    JS_FreeCString(ctx,s);if(i==count){throw_result(ctx,KSN_INVALID,op);return false;}*out=i;return true;
}
static JSValue js_ref_animate(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    const char *op="kasane.ref.animate";ref_slot *slot=ref_from(ctx,self,op);if(!slot)return JS_EXCEPTION;
    ksn_tx tx=argc?tx_from(ctx,argv[0],op):(ksn_tx){0};if(!tx.value)return JS_EXCEPTION;
    if(argc<2||!JS_IsObject(argv[1]))return throw_result(ctx,KSN_INVALID,op);
    if(slot->status==REF_CANDIDATE&&slot->ticket.value!=tx.value)return throw_result(ctx,KSN_STALE,op);
    ksn_motion motion={.first=slot->ref,.count=1,.property=KSN_TRANSFORM};
    if(!parse_pose(ctx,argv[1],"from",&motion.from.pose,op)||!parse_pose(ctx,argv[1],"to",&motion.to.pose,op))return JS_EXCEPTION;
    JSValue duration=JS_GetPropertyStr(ctx,argv[1],"durationMs");if(JS_IsException(duration))return duration;
    double n;bool ok=number_in(ctx,duration,1,86400000,&n);JS_FreeValue(ctx,duration);
    if(!ok)return throw_result(ctx,KSN_INVALID,op);
    motion.duration_ms=(uint32_t)n;
    static const char *const easings[]={"linear","ease-out","ease-in-out","step"};
    static const char *const repeats[]={"once","loop","ping-pong"};unsigned easing,repeat;
    if(!parse_choice(ctx,argv[1],"easing",easings,4,&easing,op)||!parse_choice(ctx,argv[1],"repeat",repeats,3,&repeat,op))return JS_EXCEPTION;
    motion.easing=(ksn_easing)easing;motion.repeat=(ksn_repeat)repeat;
    if(!animation_proto(ctx))return JS_EXCEPTION;
    JSValue object=JS_NewObjectClass(ctx,animation_class);if(JS_IsException(object))return object;
    ksn_animation id;ksn_result result=ksn_runtime_animate(view(),tx,&motion,&id);
    if(result!=KSN_OK){JS_FreeValue(ctx,object);return throw_result(ctx,result,op);}
    JS_SetOpaque(object,(void *)(uintptr_t)id.value);return object;
}
static JSValue animation_stop(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,bool finish){
    const char *op=finish?"kasane.animation.finish":"kasane.animation.stop";
    ksn_animation id={opaque_value(self,animation_class)};if(!id.value)return throw_result(ctx,KSN_STALE,op);
    ksn_tx tx=argc?tx_from(ctx,argv[0],op):(ksn_tx){0};if(!tx.value)return JS_EXCEPTION;
    ksn_result result=ksn_view_stop_animation(view(),tx,id,finish);
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,op);
}
static JSValue js_animation_stop(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){return animation_stop(ctx,self,argc,argv,false);}
static JSValue js_animation_finish(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){return animation_stop(ctx,self,argc,argv,true);}
static JSValue js_animation_poll(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argc;(void)argv;static const char *const names[]={"discarded","pending","running","finished","stopped"};
    ksn_animation_status status=ksn_view_poll_animation(view(),(ksn_animation){opaque_value(self,animation_class)});
    return JS_NewString(ctx,names[status]);
}
static JSValue js_instance_place(JSContext *ctx, JSValueConst self, int argc,
                                 JSValueConst *argv) {
    uint32_t raw=opaque_value(self,instance_class);
    if(!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"kasane.instance.place",
                                "instance is closed",false,NULL);
    ksn_tx tx=argc?tx_from(ctx,argv[0],"kasane.instance.place"):(ksn_tx){0};
    if(!tx.value) return JS_EXCEPTION;
    ksn_placement placement;
    if(!parse_placement(ctx,argc>1?argv[1]:JS_UNDEFINED,&placement,
                        "kasane.instance.place")) return JS_EXCEPTION;
    ksn_result result=ksn_view_place(view(),tx,(ksn_instance){raw},&placement);
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,"kasane.instance.place");
}

static JSValue js_instance_visible(JSContext *ctx, JSValueConst self, int argc,
                                   JSValueConst *argv) {
    uint32_t raw=opaque_value(self,instance_class);
    if(!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,
                                "kasane.instance.setVisible","instance is closed",
                                false,NULL);
    ksn_tx tx=argc?tx_from(ctx,argv[0],"kasane.instance.setVisible"):(ksn_tx){0};
    if(!tx.value) return JS_EXCEPTION;
    if(argc<2||!JS_IsBool(argv[1]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                                "kasane.instance.setVisible","visible must be boolean",
                                false,NULL);
    ksn_result result=ksn_view_visible(view(),tx,(ksn_instance){raw},JS_ToBool(ctx,argv[1]));
    return result==KSN_OK?JS_UNDEFINED
                        :throw_result(ctx,result,"kasane.instance.setVisible");
}

static JSValue js_modal_open(JSContext *ctx, JSValueConst self, int argc,
                             JSValueConst *argv) {
    uint32_t raw=opaque_value(self,modal_class);
    if(!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"kasane.modal.open",
                                "transaction is outside its build callback",false,NULL);
    if(overlay_profile)return throw_result(ctx,KSN_UNSUPPORTED,"kasane.modal.open");
    if(argc<1||!JS_IsObject(argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.modal.open",
                                "modal spec must be an object",false,NULL);
    ksn_modal_backdrop backdrop=KSN_MODAL_SOLID;
    JSValue mode=JS_GetPropertyStr(ctx,argv[0],"backdrop");
    if(JS_IsException(mode)) return mode;
    if(!JS_IsUndefined(mode)) {
        const char *text=JS_IsString(mode)?JS_ToCString(ctx,mode):NULL;
        if(JS_IsString(mode)&&!text) { JS_FreeValue(ctx,mode);return JS_EXCEPTION; }
        if(text&&strcmp(text,"dim-live")==0) backdrop=KSN_MODAL_DIM_LIVE;
        else if(!text||strcmp(text,"solid")!=0) {
            if(text)JS_FreeCString(ctx,text);
            JS_FreeValue(ctx,mode);
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                                    "kasane.modal.open",
                                    "backdrop must be 'solid' or 'dim-live'",false,NULL);
        }
        if(text)JS_FreeCString(ctx,text);
    }
    JS_FreeValue(ctx,mode);
    uint32_t color=0x00000080u,focus=0;
    JSValue color_value=JS_GetPropertyStr(ctx,argv[0],"color");
    if(JS_IsException(color_value)) return color_value;
    if(!JS_IsUndefined(color_value)&&!parse_u32(ctx,color_value,&color)) {
        JS_FreeValue(ctx,color_value);
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.modal.open",
                                "color must be an RRGGBBAA uint32",false,NULL);
    }
    JS_FreeValue(ctx,color_value);
    JSValue focus_value=JS_GetPropertyStr(ctx,argv[0],"focus");
    if(JS_IsException(focus_value)) return focus_value;
    if(!JS_IsUndefined(focus_value)&&!parse_u32(ctx,focus_value,&focus)) {
        JS_FreeValue(ctx,focus_value);
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.modal.open",
                                "focus must be a uint32",false,NULL);
    }
    JS_FreeValue(ctx,focus_value);
    ksn_result result=ksn_view_modal_open(view(),(ksn_tx){raw},backdrop,color,focus);
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,"kasane.modal.open");
}

static JSValue js_modal_close(JSContext *ctx, JSValueConst self, int argc,
                              JSValueConst *argv) {
    (void)argc;(void)argv;
    uint32_t raw=opaque_value(self,modal_class);
    if(!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"kasane.modal.close",
                                "transaction is outside its build callback",false,NULL);
    ksn_result result=ksn_view_modal_close(view(),(ksn_tx){raw});
    return result==KSN_OK?JS_UNDEFINED:throw_result(ctx,result,"kasane.modal.close");
}

/* Validate ownership before entering any parser (which can invoke JS getters).
 * Every exception in an owning mutation aborts even when the callback catches
 * it. A foreign/expired transaction must never cancel the current builder. */
static JSValue mutate(JSContext *ctx, JSValueConst self, int argc,
                       JSValueConst *argv, JSClassID class_id, bool tx_argument,
                       JSCFunction *function, const char *op) {
    JSValueConst token=tx_argument?(argc?argv[0]:JS_UNDEFINED):self;
    ksn_tx tx={opaque_value(token,class_id)};
    if(!state||!tx.value||state->building.value!=tx.value||
       !view()||view()->host->builder.value!=tx.value)
        return throw_result(ctx,KSN_STALE,op);
    JSValue result=function(ctx,self,argc,argv);
    if(JS_IsException(result)) {
        ksn_view_cancel(view(),tx);discard_candidates(tx);
    }
    return result;
}

#define MUTATOR(name,class_id,tx_argument,op) \
static JSValue name##_checked(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){ \
    return mutate(ctx,self,argc,argv,class_id,tx_argument,name,op); \
}
MUTATOR(js_tx_background,tx_class,false,"kasane.background")
MUTATOR(js_tx_rect,tx_class,false,"kasane.rect")
MUTATOR(js_tx_round_rect,tx_class,false,"kasane.roundRect")
MUTATOR(js_tx_stroke_rect,tx_class,false,"kasane.strokeRect")
MUTATOR(js_tx_gradient,tx_class,false,"kasane.gradient")
MUTATOR(js_tx_text,tx_class,false,"kasane.text")
MUTATOR(js_tx_image,tx_class,false,"kasane.image")
MUTATOR(js_tx_group,tx_class,false,"kasane.group")
MUTATOR(js_tx_instantiate,tx_class,false,"kasane.instantiate")
MUTATOR(js_ref_rect,tx_class,true,"kasane.ref.setRect")
MUTATOR(js_ref_clip,tx_class,true,"kasane.ref.setClip")
MUTATOR(js_ref_color,tx_class,true,"kasane.ref.setColor")
MUTATOR(js_ref_visible,tx_class,true,"kasane.ref.setVisible")
MUTATOR(js_ref_text,tx_class,true,"kasane.ref.setText")
MUTATOR(js_ref_reveal,tx_class,true,"kasane.ref.setReveal")
MUTATOR(js_ref_image,tx_class,true,"kasane.ref.setImageFrame")
MUTATOR(js_ref_rotation,tx_class,true,"kasane.ref.setRotation")
MUTATOR(js_ref_animate,tx_class,true,"kasane.ref.animate")
MUTATOR(js_animation_stop,tx_class,true,"kasane.animation.stop")
MUTATOR(js_animation_finish,tx_class,true,"kasane.animation.finish")
MUTATOR(js_instance_place,tx_class,true,"kasane.instance.place")
MUTATOR(js_instance_visible,tx_class,true,"kasane.instance.setVisible")
MUTATOR(js_modal_open,modal_class,false,"kasane.modal.open")
MUTATOR(js_modal_close,modal_class,false,"kasane.modal.close")
#undef MUTATOR

/* The scene controller of createScene(), native since the guest-memory work
 * (docs/kasane/kasane-guest-memory-reduce.md). As JS it cost each Kasane app
 * four closures, their bytecode and source copies, a dozen var_refs and the
 * atoms of every local name; here it is one opaque holder and two bound
 * functions. apps/kasane/create_scene.js remains as the reference model the
 * node tests drive, and the flush below is that file transcribed statement by
 * statement: keep the two in step. */
typedef struct {
    JSValue build,patch,refs,candidate,model;
    bool pending,pending_replace,dirty,rebuild,running;
} kasane_scene;
static JSClassID scene_class;
static JSRuntime *scene_rt;

static void scene_set(JSContext *ctx,JSValue *slot,JSValue value) {
    JSValue old=*slot;*slot=value;JS_FreeValue(ctx,old);
}

/* build(tx, model) must yield a non-null, non-callable object (the candidate
 * refs); patch(tx, refs, model) promotes the displayed refs unchanged. */
static JSValue scene_callback(JSContext *ctx,kasane_scene *scene,JSValueConst tx,
                              ksn_update_mode mode) {
    if(mode==KSN_PATCH) {
        JSValueConst args[]={tx,scene->refs,scene->model};
        JSValue returned=JS_Call(ctx,scene->patch,JS_UNDEFINED,3,args);
        if(!JS_IsException(returned))
            scene_set(ctx,&scene->candidate,JS_DupValue(ctx,scene->refs));
        return returned;
    }
    JSValueConst args[]={tx,scene->model};
    JSValue returned=JS_Call(ctx,scene->build,JS_UNDEFINED,2,args);
    if(JS_IsException(returned)) return returned;
    scene_set(ctx,&scene->candidate,JS_DupValue(ctx,returned));
    if(JS_IsObject(returned)&&!JS_IsFunction(ctx,returned)) return returned;
    JS_FreeValue(ctx,returned);
    return JS_ThrowTypeError(ctx,"scene build must return an object containing candidate refs");
}

/* scene is NULL for view.replace/patch(build): the ticket is created and the
 * JS function called. A scene discards the ticket, so none is allocated. */
static JSValue run_build(JSContext *ctx, JSValueConst build, kasane_scene *scene,
                         ksn_update_mode mode, const char *op) {
    if(!scene&&!JS_IsFunction(ctx,build))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "build must be a synchronous function",false,NULL);
    if(!ensure_state(ctx,op)) return JS_EXCEPTION;
    if(state->provider||state->schema)return throw_result(ctx,KSN_BUSY,op);
    /* Keep the callback closed to reentrant builds after an inner abort. */
    if(state->building.value) return throw_result(ctx,KSN_BUSY,op);
    apply_outcome();
    ksn_tx tx;ksn_result result=ksn_view_begin(view(),mode,&tx);
    if(result!=KSN_OK) return throw_result(ctx,result,op);
    state->building=tx;
    JSValue ticket=JS_UNDEFINED,tx_object=JS_UNDEFINED,modal_object=JS_UNDEFINED;
    /* No fallible allocation may follow successful native submission. */
    if(!scene) {
        ticket=wrap_direct(ctx,ticket_class,tx.value);
        if(JS_IsException(ticket)) goto fail;
    }
    tx_object=wrap_direct(ctx,tx_class,tx.value);
    if(JS_IsException(tx_object)) goto fail;
    modal_object=wrap_direct(ctx,modal_class,tx.value);
    if(JS_IsException(modal_object)) goto fail;
    if(JS_DefinePropertyValueStr(ctx,tx_object,"modal",JS_DupValue(ctx,modal_object),
                                 JS_PROP_C_W_E)<0) goto fail;
    JSValue arg=JS_DupValue(ctx,tx_object);
    JSValue returned=scene?scene_callback(ctx,scene,arg,mode)
                          :JS_Call(ctx,build,JS_UNDEFINED,1,&arg);
    JS_FreeValue(ctx,arg);
    JS_SetOpaque(tx_object,NULL);JS_SetOpaque(modal_object,NULL);
    if(JS_IsException(returned)) goto fail;
    bool thenable=false;
    if(JS_IsObject(returned)) {
        JSValue then=JS_GetPropertyStr(ctx,returned,"then");
        if(JS_IsException(then)) {
            JS_FreeValue(ctx,returned);goto fail;
        }
        thenable=JS_IsFunction(ctx,then);JS_FreeValue(ctx,then);
    }
    JS_FreeValue(ctx,returned);
    if(thenable) {
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "build must not return a Promise or thenable",false,
                                POCKET_OUTCOME_NOT_APPLIED);
        goto fail;
    }
    result=ksn_view_submit(view(),tx);
    if(result!=KSN_OK) { throw_result(ctx,result,op);goto fail; }
    state->building=(ksn_tx){0};
    state->submitted=tx;state->submitted_mode=mode;state->active=true;
    ksn_runtime_app_activate(state->lease);
    JS_FreeValue(ctx,tx_object);JS_FreeValue(ctx,modal_object);
    return ticket;
fail:
    if(JS_IsObject(tx_object)) JS_SetOpaque(tx_object,NULL);
    if(JS_IsObject(modal_object)) JS_SetOpaque(modal_object,NULL);
    JS_FreeValue(ctx,tx_object);JS_FreeValue(ctx,modal_object);JS_FreeValue(ctx,ticket);
    ksn_view_cancel(view(),tx);discard_candidates(tx);state->building=(ksn_tx){0};
    return JS_EXCEPTION;
}

static JSValue js_replace(JSContext *ctx, JSValueConst self, int argc,
                          JSValueConst *argv) {
    (void)self;
    return run_build(ctx,argc?argv[0]:JS_UNDEFINED,NULL,KSN_REPLACE,"kasane.replace");
}
static JSValue js_patch(JSContext *ctx, JSValueConst self, int argc,
                        JSValueConst *argv) {
    (void)self;
    return run_build(ctx,argc?argv[0]:JS_UNDEFINED,NULL,KSN_PATCH,"kasane.patch");
}

static void scene_finalizer(JSRuntime *rt, JSValue value) {
    kasane_scene *scene=JS_GetOpaque(value,scene_class);
    if(!scene) return;
    JS_FreeValueRT(rt,scene->build);JS_FreeValueRT(rt,scene->patch);
    JS_FreeValueRT(rt,scene->refs);JS_FreeValueRT(rt,scene->candidate);
    JS_FreeValueRT(rt,scene->model);js_free_rt(rt,scene);
}
static void scene_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark) {
    kasane_scene *scene=JS_GetOpaque(value,scene_class);
    if(!scene) return;
    JS_MarkValue(rt,scene->build,mark);JS_MarkValue(rt,scene->patch,mark);
    JS_MarkValue(rt,scene->refs,mark);JS_MarkValue(rt,scene->candidate,mark);
    JS_MarkValue(rt,scene->model,mark);
}
static const JSClassDef scene_def={.class_name=KASANE_CLASS,.finalizer=scene_finalizer,
                                   .gc_mark=scene_mark};

static JSValue scene_invalidate(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,
                                int magic,JSValueConst *data) {
    (void)self;(void)magic;
    kasane_scene *scene=JS_GetOpaque(data[0],scene_class);
    scene->dirty=true;
    if(argc&&JS_ToBool(ctx,argv[0])) scene->rebuild=true;
    return JS_UNDEFINED;
}

static JSValue scene_flush_turn(JSContext *ctx,kasane_scene *scene,JSValueConst model) {
    if(scene->pending) {
        apply_outcome();
        ksn_submission outcome=state?ksn_view_poll(view()):(ksn_submission){0};
        if(outcome.status==KSN_SUBMITTED) return JS_FALSE;
        if(outcome.status==KSN_PRESENTED)
            scene_set(ctx,&scene->refs,JS_DupValue(ctx,scene->candidate));
        else if(outcome.status==KSN_DISCARDED) {
            scene->dirty=true;
            if(scene->pending_replace) scene->rebuild=true;
        } else return JS_ThrowPlainError(ctx,"scene lost its submission");
        scene_set(ctx,&scene->candidate,JS_NULL);
        scene->pending=false;
    }
    if(!scene->dirty) return JS_TRUE;
    bool replacing=scene->rebuild||JS_IsNull(scene->refs)||JS_IsUndefined(scene->patch);
    scene->dirty=false;
    scene->rebuild=false;
    scene_set(ctx,&scene->model,JS_DupValue(ctx,model));
    JSValue submitted=replacing?run_build(ctx,JS_UNDEFINED,scene,KSN_REPLACE,"kasane.replace")
                               :run_build(ctx,JS_UNDEFINED,scene,KSN_PATCH,"kasane.patch");
    JSValue out=JS_FALSE;
    if(!JS_IsException(submitted)) {
        scene->pending_replace=replacing;
        scene->pending=true;
        goto done;
    }
    JSValue error=JS_GetException(ctx);
    /* An interrupt skips catch and finally in JS too: pass it straight on. */
    if(JS_IsUncatchableError(error)) { JS_Throw(ctx,error);return JS_EXCEPTION; }
    scene_set(ctx,&scene->candidate,JS_NULL);
    scene->dirty=true;
    if(replacing) scene->rebuild=true;
    bool busy=false;
    if(JS_ToBool(ctx,error)) {
        JSValue code=JS_GetPropertyStr(ctx,error,"code");
        if(JS_IsException(code)) { JS_FreeValue(ctx,error);out=JS_EXCEPTION;goto done; }
        if(JS_IsString(code)) {
            size_t length;const char *text=JS_ToCStringLen(ctx,&length,code);
            if(!text) { JS_FreeValue(ctx,code);JS_FreeValue(ctx,error);out=JS_EXCEPTION;goto done; }
            busy=length==4&&!memcmp(text,"BUSY",4);
            JS_FreeCString(ctx,text);
        }
        JS_FreeValue(ctx,code);
    }
    if(busy) JS_FreeValue(ctx,error);
    else { JS_Throw(ctx,error);out=JS_EXCEPTION; }
done:
    scene_set(ctx,&scene->model,JS_UNDEFINED);
    return out;
}

static JSValue scene_flush(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,
                           int magic,JSValueConst *data) {
    (void)self;(void)magic;
    kasane_scene *scene=JS_GetOpaque(data[0],scene_class);
    if(scene->running) return JS_ThrowPlainError(ctx,"scene flush is not reentrant");
    scene->running=true;
    JSValue out=scene_flush_turn(ctx,scene,argc?argv[0]:JS_UNDEFINED);
    scene->running=false;
    return out;
}

static JSValue js_cache_create(JSContext *ctx, JSValueConst self, int argc,
                               JSValueConst *argv) {
    (void)self;
    if(argc<1||!JS_IsArray(argv[0]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.cache.create",
                                "definition must be an array of rectangles",false,NULL);
    if(!ensure_state(ctx,"kasane.cache.create")) return JS_EXCEPTION;
    if(state->building.value) return throw_result(ctx,KSN_BUSY,"kasane.cache.create");
    apply_outcome();
    int64_t length=0;
    if(JS_GetLength(ctx,argv[0],&length)<0) return JS_EXCEPTION;
    if(length<1||length>KSN_CACHE_COMMANDS)
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,"kasane.cache.create",
                                "cache definition has an invalid command count",false,NULL);
    ksn_draw draws[KSN_CACHE_COMMANDS];
    memset(draws,0,sizeof(draws));
    for(int64_t i=0;i<length;i++) {
        JSValue item=JS_GetPropertyUint32(ctx,argv[0],(uint32_t)i);
        bool ok=!JS_IsException(item)&&parse_cache_draw(ctx,item,&draws[i],"kasane.cache.create");
        JS_FreeValue(ctx,item);
        if(!ok) return JS_EXCEPTION;
    }
    JSValue object=wrap_direct(ctx,template_class,0);
    if(JS_IsException(object)) return object;
    ksn_template result_handle;
    ksn_result result=create_template(draws,(uint16_t)length,&result_handle);
    if(result!=KSN_OK) {
        JS_FreeValue(ctx,object);return throw_result(ctx,result,"kasane.cache.create");
    }
    JS_SetOpaque(object,(void *)(uintptr_t)result_handle.value);return object;
}

static JSValue js_cache_release(JSContext *ctx, JSValueConst self, int argc,
                                JSValueConst *argv) {
    (void)self;
    uint32_t raw=argc?opaque_value(argv[0],template_class):0;
    if(!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"kasane.cache.release",
                                "template is closed",false,NULL);
    if(state&&state->building.value) return throw_result(ctx,KSN_BUSY,"kasane.cache.release");
    apply_outcome();
    ksn_result result=ksn_view_cache_release(view(),(ksn_template){raw});
    if(result!=KSN_OK) return throw_result(ctx,result,"kasane.cache.release");
    JS_SetOpaque(argv[0],NULL);return JS_UNDEFINED;
}

static const char *status_name(ksn_submission_status status) {
    switch(status) {
    case KSN_SUBMITTED:return "SUBMITTED";
    case KSN_PRESENTED:return "PRESENTED";
    case KSN_DISCARDED:return "DISCARDED";
    default:return "NONE";
    }
}

/* Defining own properties avoids invoking user-installed prototype setters.
 * JS_DefinePropertyValueStr consumes value on both success and failure. */
static bool put(JSContext *ctx, JSValueConst object, const char *name, JSValue value) {
    if(JS_IsException(value)) return false;
    return JS_DefinePropertyValueStr(ctx,object,name,value,JS_PROP_C_W_E)>=0;
}
#define PUT(object,name,value) do { if(!put(ctx,object,name,value)) goto fail; } while(0)

static JSValue js_poll(JSContext *ctx, JSValueConst self, int argc,
                       JSValueConst *argv) {
    (void)self;(void)argc;(void)argv;
    apply_outcome();
    ksn_submission submission=state?ksn_view_poll(view()):(ksn_submission){0};
    JSValue out=JS_NewObject(ctx);
    if(JS_IsException(out)) return out;
    PUT(out,"ticket",submission.ticket.value
        ?wrap_direct(ctx,ticket_class,submission.ticket.value):JS_NULL);
    PUT(out,"status",JS_NewString(ctx,status_name(submission.status)));
    PUT(out,"reason",JS_NewString(ctx,result_code(submission.reason)));
    PUT(out,"layer",JS_NewString(ctx,"app"));
    return out;
fail:
    JS_FreeValue(ctx,out);return JS_EXCEPTION;
}

static JSValue js_cancel(JSContext *ctx, JSValueConst self, int argc,
                         JSValueConst *argv) {
    (void)self;
    uint32_t raw=argc?opaque_value(argv[0],ticket_class):0;
    if(!state||!raw)
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"kasane.cancel",
                                "ticket is closed",false,NULL);
    ksn_result result=ksn_view_cancel(view(),(ksn_tx){raw});
    if(result!=KSN_OK) return throw_result(ctx,result,"kasane.cancel");
    apply_outcome();return JS_UNDEFINED;
}

static JSValue js_features(JSContext *ctx, JSValueConst self, int argc,
                           JSValueConst *argv) {
    (void)self;(void)argc;(void)argv;
    JSValue out=JS_UNDEFINED,capacity=JS_UNDEFINED,cache=JS_UNDEFINED;
    out=JS_NewObject(ctx);if(JS_IsException(out)) goto fail;
    capacity=JS_NewObject(ctx);if(JS_IsException(capacity)) goto fail;
    cache=JS_NewObject(ctx);if(JS_IsException(cache)) goto fail;
    PUT(out,"rect",JS_NewBool(ctx,true));
    PUT(out,"roundRect",JS_NewBool(ctx,true));
    PUT(out,"strokeRect",JS_NewBool(ctx,true));
    PUT(out,"gradient",JS_NewBool(ctx,true));
    PUT(out,"text",JS_NewBool(ctx,true));
    PUT(out,"image",JS_NewBool(ctx,true));
    PUT(out,"imageStretch",JS_NewBool(ctx,true));
    PUT(out,"imageRotation",JS_NewBool(ctx,true));
    PUT(out,"groupOpacity",JS_NewBool(ctx,true));
    PUT(out,"modal",JS_NewBool(ctx,!overlay_profile));
    PUT(out,"animation",JS_NewBool(ctx,true));
    PUT(out,"frosted",JS_NewBool(ctx,false));
    PUT(capacity,"commands",JS_NewInt32(ctx,KSN_APP_COMMANDS));
    PUT(capacity,"textBytes",JS_NewInt32(ctx,KSN_APP_TEXT_BYTES));
    PUT(capacity,"refs",JS_NewInt32(ctx,KASANE_REF_LIMIT));
    PUT(capacity,"animations",JS_NewInt32(ctx,KSN_APP_TRACKS));
    PUT(cache,"commands",JS_NewInt32(ctx,KSN_CACHE_COMMANDS));
    PUT(cache,"templates",JS_NewInt32(ctx,KSN_CACHE_TEMPLATES));
    PUT(cache,"instances",JS_NewInt32(ctx,KSN_CACHE_INSTANCES));
    PUT(out,"capacity",JS_DupValue(ctx,capacity));
    PUT(out,"cache",JS_DupValue(ctx,cache));
    JS_FreeValue(ctx,capacity);JS_FreeValue(ctx,cache);return out;
fail:
    JS_FreeValue(ctx,out);JS_FreeValue(ctx,capacity);JS_FreeValue(ctx,cache);
    return JS_EXCEPTION;
}

static JSValue js_stats(JSContext *ctx, JSValueConst self, int argc,
                        JSValueConst *argv) {
    (void)self;(void)argc;(void)argv;
    apply_outcome();
    ksn_view_stats stats=ksn_runtime_stats(KSN_APP);
    JSValue out=JS_UNDEFINED,displayed=JS_UNDEFINED,cache=JS_UNDEFINED;
    out=JS_NewObject(ctx);if(JS_IsException(out)) goto fail;
    displayed=JS_NewObject(ctx);if(JS_IsException(displayed)) goto fail;
    cache=JS_NewObject(ctx);if(JS_IsException(cache)) goto fail;
    PUT(out,"active",JS_NewBool(ctx,state&&state->active));
    PUT(out,"nativeBytes",JS_NewUint32(ctx,ksn_runtime_reserved_bytes()+
        (state?sizeof(*state)+(source_caps?sizeof(*source_caps):0)+
          (state->provider?
          state->provider->native_bytes(state->provider_state):0)+
          (state->schema?state->schema->allocation_bytes+
                         state->schema->owned_asset_bytes+
                         (schema_external_state(state->schema)?
                          sizeof(schema_externals):0):0):0)));
    PUT(displayed,"commands",JS_NewInt32(ctx,stats.displayed.commands));
    PUT(displayed,"textBytes",JS_NewInt32(ctx,stats.displayed.text_bytes));
    PUT(cache,"commands",JS_NewInt32(ctx,stats.shared_cache.commands));
    PUT(cache,"templates",JS_NewInt32(ctx,stats.shared_cache.templates));
    PUT(cache,"instances",JS_NewInt32(ctx,stats.shared_cache.instances));
    PUT(cache,"reservedBytes",JS_NewUint32(ctx,ksn_runtime_cache_bytes()));
    PUT(out,"displayed",JS_DupValue(ctx,displayed));PUT(out,"cache",JS_DupValue(ctx,cache));
    JS_FreeValue(ctx,displayed);JS_FreeValue(ctx,cache);return out;
fail:
    JS_FreeValue(ctx,out);JS_FreeValue(ctx,displayed);JS_FreeValue(ctx,cache);
    return JS_EXCEPTION;
}
#undef PUT

static JSValue js_input_scope(JSContext *ctx, JSValueConst self, int argc,
                              JSValueConst *argv) {
    (void)self;(void)argc;(void)argv;
    ksn_input_scope scope=pocket_kasane_input_scope(false);
    const char *name=scope==KSN_INPUT_MODAL?"modal":scope==KSN_INPUT_BLOCKED?"blocked":"app";
    return JS_NewString(ctx,name);
}

static const JSCFunctionListEntry tx_methods[]={
    JS_CFUNC_DEF("background",1,js_tx_background_checked),
    JS_CFUNC_DEF("rect",1,js_tx_rect_checked),
    JS_CFUNC_DEF("roundRect",1,js_tx_round_rect_checked),
    JS_CFUNC_DEF("strokeRect",1,js_tx_stroke_rect_checked),
    JS_CFUNC_DEF("gradient",1,js_tx_gradient_checked),
    JS_CFUNC_DEF("text",1,js_tx_text_checked),
    JS_CFUNC_DEF("image",1,js_tx_image_checked),
    JS_CFUNC_DEF("group",3,js_tx_group_checked),
    JS_CFUNC_DEF("instantiate",2,js_tx_instantiate_checked),
};
static const JSCFunctionListEntry modal_methods[]={
    JS_CFUNC_DEF("open",1,js_modal_open_checked),JS_CFUNC_DEF("close",0,js_modal_close_checked),
};
static const JSCFunctionListEntry ref_methods[]={
    JS_CFUNC_DEF("setRect",2,js_ref_rect_checked),JS_CFUNC_DEF("setClip",2,js_ref_clip_checked),
    JS_CFUNC_DEF("setColor",2,js_ref_color_checked),JS_CFUNC_DEF("setVisible",2,js_ref_visible_checked),
    JS_CFUNC_DEF("setText",2,js_ref_text_checked),JS_CFUNC_DEF("setReveal",2,js_ref_reveal_checked),
    JS_CFUNC_DEF("setImageFrame",3,js_ref_image_checked),
    JS_CFUNC_DEF("setRotation",2,js_ref_rotation_checked),
    JS_CFUNC_DEF("animate",2,js_ref_animate_checked),
};
static const JSCFunctionListEntry animation_methods[]={
    JS_CFUNC_DEF("stop",1,js_animation_stop_checked),JS_CFUNC_DEF("finish",1,js_animation_finish_checked),
    JS_CFUNC_DEF("poll",0,js_animation_poll),
};
static const JSCFunctionListEntry instance_methods[]={
    JS_CFUNC_DEF("place",2,js_instance_place_checked),
    JS_CFUNC_DEF("setVisible",2,js_instance_visible_checked),
};
/* Returns {invalidate, flush} as own data properties holding bound functions,
 * the shape the JS factory's object literal had, so detached calls still work.
 * The returned object is also the state's holder (its class inherits
 * Object.prototype): each bound function keeps it alive through its data slot,
 * a cycle the collector breaks through scene_mark. One object fewer than a
 * separate hidden holder. */
static JSValue js_create_scene(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    JSValueConst options=argc?argv[0]:JS_UNDEFINED;
    JSValue build=JS_UNDEFINED,patch=JS_UNDEFINED;
    bool valid=false;
    if(JS_ToBool(ctx,options)) {
        build=JS_GetPropertyStr(ctx,options,"build");
        if(JS_IsException(build)) return build;
        if(JS_IsFunction(ctx,build)) {
            patch=JS_GetPropertyStr(ctx,options,"patch");
            if(JS_IsException(patch)) { JS_FreeValue(ctx,build);return patch; }
            valid=JS_IsUndefined(patch)||JS_IsFunction(ctx,patch);
        }
    }
    if(!valid) {
        JS_FreeValue(ctx,build);JS_FreeValue(ctx,patch);
        return JS_ThrowTypeError(ctx,"createScene requires build and optional patch callbacks");
    }
    JSValue out=JS_NewObjectClass(ctx,scene_class);
    kasane_scene *scene=JS_IsException(out)?NULL:js_mallocz(ctx,sizeof(*scene));
    if(!scene) {
        JS_FreeValue(ctx,out);JS_FreeValue(ctx,build);JS_FreeValue(ctx,patch);
        return JS_EXCEPTION;
    }
    *scene=(kasane_scene){.build=build,.patch=patch,.refs=JS_NULL,.candidate=JS_NULL,
                          .model=JS_UNDEFINED,.dirty=true,.rebuild=true};
    JS_SetOpaque(out,scene);
    JSValueConst data[]={out};
    JSValue invalidate=JS_NewCFunctionData2(ctx,scene_invalidate,"invalidate",1,0,1,data);
    if(JS_IsException(invalidate)||
       JS_DefinePropertyValueStr(ctx,out,"invalidate",invalidate,JS_PROP_C_W_E)<0) goto fail;
    JSValue flush=JS_NewCFunctionData2(ctx,scene_flush,"flush",1,0,1,data);
    if(JS_IsException(flush)||
       JS_DefinePropertyValueStr(ctx,out,"flush",flush,JS_PROP_C_W_E)<0) goto fail;
    return out;
fail:
    JS_FreeValue(ctx,out);
    return JS_EXCEPTION;
}

static schema_state *schema_owner(JSValueConst self){
    uint32_t handle=(uint32_t)(uintptr_t)JS_GetOpaque(self,schema_class);
    return handle&&state&&state->schema&&state->schema->handle==handle?
           state->schema:NULL;
}
static JSValue js_schema_set(JSContext *ctx,schema_state *s,JSValueConst model){
    const char *op="kasane.view.set";
    if(!JS_IsObject(model)||JS_IsArray(model))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "slots must be an object",false,NULL);
    ksn_schema_value candidate[KSN_SCHEMA_MAX_SLOTS];
    char pending_text[KSN_SCHEMA_MAX_SLOTS][KSN_SCHEMA_TEXT_MAX+1u];
    bool text_dirty[KSN_SCHEMA_MAX_SLOTS]={0};
    memcpy(candidate,s->values,s->definition->slot_count*sizeof(*candidate));
    ksn_p0_probe_copy(KSN_P0_ADAPTER_TEMP,s->definition->slot_count*sizeof(*candidate));
    JSPropertyEnum *props=NULL;uint32_t count=0;
    if(JS_GetOwnPropertyNames(ctx,&props,&count,model,
                              JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY))return JS_EXCEPTION;
    bool ok=true;
    for(uint32_t p=0;p<count&&ok;p++){
        const char *key=JS_AtomToCString(ctx,props[p].atom);
        if(!key){ok=false;break;}
        unsigned i=0;
        for(;i<s->definition->slot_count;i++)
            if(strcmp(key,s->definition->slots[i].name)==0)break;
        JS_FreeCString(ctx,key);
        if(i==s->definition->slot_count){
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "unknown display slot",false,NULL);ok=false;break;
        }
        JSValue value=JS_GetProperty(ctx,model,props[p].atom);
        if(JS_IsException(value)){ok=false;break;}
        const ksn_schema_slot *slot=&s->definition->slots[i];
        if(slot->type==KSN_SLOT_TEXT){
            if(!JS_IsString(value))ok=false;
            else{
                int64_t units=0;
                if(JS_GetLength(ctx,value,&units)<0||units>slot->capacity)ok=false;
                else{
                    size_t length=0;const char *text=JS_ToCStringLen(ctx,&length,value);
                    if(text)ksn_p0_probe_copy(KSN_P0_UTF8_MATERIALIZED,length);
                    if(!text)ok=false;
                    else if(length>slot->capacity)ok=false;
                    else{
                        memcpy(pending_text[i],text,length);
                        ksn_p0_probe_copy(KSN_P0_ADAPTER_TEMP,length);
                        pending_text[i][length]=0;
                        candidate[i].data.text=(ksn_schema_text){pending_text[i],(uint16_t)length};
                        text_dirty[i]=true;
                    }
                    if(text)JS_FreeCString(ctx,text);
                }
            }
        }else if(slot->type==KSN_SLOT_RECT){
            ok=parse_rect_mode(ctx,value,&candidate[i].data.rect,op,false);
        }else if(slot->type==KSN_SLOT_BOOL){
            ok=JS_IsBool(value);
            if(ok)candidate[i].data.boolean=JS_ToBool(ctx,value)!=0;
        }else if(slot->type==KSN_SLOT_COLOR){
            uint32_t color;ok=parse_u32(ctx,value,&color);
            if(ok)candidate[i].data.color=color;
        }else if(slot->type==KSN_SLOT_U16){
            double number;ok=number_in(ctx,value,0,
                           slot->maximum?slot->maximum:UINT16_MAX,&number);
            if(ok)candidate[i].data.number=(uint16_t)number;
        }else if(slot->type==KSN_SLOT_RESOURCE){
            candidate[i].data.resource.value=opaque_value(value,image_class);
            ok=candidate[i].data.resource.value!=0;
        }
        JS_FreeValue(ctx,value);
        if(!ok&&!JS_HasException(ctx))
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                             "display slot type or value is invalid",false,NULL);
    }
    JS_FreePropertyEnum(ctx,props,count);
    if(!ok)return JS_EXCEPTION;
    uint32_t changed_slots=0;
    for(unsigned i=0;i<s->definition->slot_count;i++){
        bool differs;
        if(text_dirty[i]){
            ksn_schema_text a=candidate[i].data.text,b=s->values[i].data.text;
            differs=a.bytes!=b.bytes||memcmp(a.utf8,b.utf8,a.bytes)!=0;
        }else differs=memcmp(&candidate[i].data,&s->values[i].data,
                            sizeof(candidate[i].data))!=0;
        if(differs)changed_slots|=(uint32_t)1u<<i;
    }
    if(!changed_slots)return JS_UNDEFINED;
    if(s->revision==UINT64_MAX)return throw_result(ctx,KSN_LIMIT,op);
    ksn_result check=ksn_schema_preflight_view(view(),s->definition,candidate,viewport);
    if(check!=KSN_OK)return throw_result(ctx,check,op);
    for(unsigned i=0;i<s->definition->slot_count;i++){
        if(!(changed_slots&((uint32_t)1u<<i)))continue;
        if(text_dirty[i]){
            ksn_schema_text text=candidate[i].data.text;
            char *dest=(char *)s->values[i].data.text.utf8;
            memcpy(dest,text.utf8,text.bytes+1u);
            ksn_p0_probe_copy(KSN_P0_ADAPTER_OWNED,text.bytes+1u);
            s->values[i].data.text=(ksn_schema_text){dest,text.bytes};
            ksn_p0_probe_copy(KSN_P0_ADAPTER_SLOT_COMMIT,sizeof(ksn_schema_text));
        }else{
            s->values[i]=candidate[i];
            ksn_p0_probe_copy(KSN_P0_ADAPTER_SLOT_COMMIT,sizeof(candidate[i]));
        }
    }
    s->revision++;
    s->pending_base_slots|=changed_slots;
    ksn_result submitted=schema_refresh(NULL);
    return submitted==KSN_OK||submitted==KSN_BUSY?JS_UNDEFINED:
           throw_result(ctx,submitted,op);
}
static JSValue js_schema_set_method(JSContext *ctx,JSValueConst self,
                                    int argc,JSValueConst *argv){
    schema_state *generic=schema_owner(self);
    if(!generic)return throw_result(ctx,KSN_STALE,"kasane.view.set");
    return js_schema_set(ctx,generic,argc?argv[0]:JS_UNDEFINED);
}
/* A numeric source index is local to the mount; a C-issued capability can
 * refer to a separate service registry. Neither path searches source names. */
static JSValue js_schema_bind_method(JSContext *ctx,JSValueConst self,
                                     int argc,JSValueConst *argv){
    const char *op="kasane.view.bind";
    schema_state *s=schema_owner(self);
    if(!s)return throw_result(ctx,KSN_STALE,op);
    if(s->session.ticket.value)return throw_result(ctx,KSN_BUSY,op);
    if(argc<2||!JS_IsObject(argv[1])||JS_IsArray(argv[1]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "source and slot bindings are required",false,NULL);
    bool local=JS_IsNumber(argv[0]);
    unsigned source_index=0;
    source_cap_record *offer=NULL;
    if(local){
        double number;
        if(!s->asset->source_count||!number_in(ctx,argv[0],0,
                  s->asset->source_count-1u,&number))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "source index is out of range",false,NULL);
        source_index=(unsigned)number;
    }else{
        uint32_t serial=(uint32_t)(uintptr_t)JS_GetOpaque(argv[0],source_cap_class);
        if(!serial||!source_caps)return throw_result(ctx,KSN_STALE,op);
        for(unsigned i=0;i<source_caps->count;i++)
            if(source_caps->records[i].serial==serial){
                offer=&source_caps->records[i];break;
            }
        if(!offer)return throw_result(ctx,KSN_STALE,op);
    }
    JSPropertyEnum *props=NULL;uint32_t count=0;
    if(JS_GetOwnPropertyNames(ctx,&props,&count,argv[1],
                              JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY))return JS_EXCEPTION;
    if(!count||count>KSN_SOURCE_MAX_FIELDS){
        JS_FreePropertyEnum(ctx,props,count);
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                "binding count is out of range",false,NULL);
    }
    ksn_source_binding bindings[KSN_SOURCE_MAX_FIELDS];
    bool valid=true;
    for(uint32_t p=0;p<count&&valid;p++){
        const char *name=JS_AtomToCString(ctx,props[p].atom);
        if(!name){valid=false;break;}
        unsigned slot=0;
        for(;slot<s->definition->slot_count;slot++)
            if(strcmp(name,s->definition->slots[slot].name)==0)break;
        JS_FreeCString(ctx,name);
        if(slot==s->definition->slot_count){valid=false;break;}
        JSValue value=JS_GetProperty(ctx,argv[1],props[p].atom);
        if(JS_IsException(value)){valid=false;break;}
        double field;
        valid=number_in(ctx,value,0,KSN_SOURCE_MAX_FIELDS-1u,&field);
        JS_FreeValue(ctx,value);
        if(valid)bindings[p]=(ksn_source_binding){(uint8_t)slot,(uint8_t)field};
    }
    JS_FreePropertyEnum(ctx,props,count);
    if(!valid)return JS_HasException(ctx)?JS_EXCEPTION:
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                         "slot name or field index is invalid",false,NULL);
    /* A binding getter can call view.set() and submit while we parse it. */
    if(s->session.ticket.value)return throw_result(ctx,KSN_BUSY,op);
    schema_sources *sources=schema_native(s);
    schema_externals *external=schema_external_state(s);
    schema_source *entry=local?&sources->entries[source_index]:NULL;
    int external_index=-1;
    if(!local&&external)for(unsigned i=0;i<external->count;i++)
        if(external->entries[i].registry==offer->registry&&
           external->entries[i].handle.index==offer->handle.index&&
           external->entries[i].handle.generation==offer->handle.generation){
            external_index=(int)i;break;
        }
    ksn_source_subscription *old=local?&entry->subscription:
        external_index>=0?&external->entries[external_index].subscription:NULL;
    ksn_source_subscription candidate;
    ksn_result r=ksn_source_subscribe(local?&sources->registry:offer->registry,
        local?entry->handle:offer->handle,s->handle,
        s->definition,bindings,(uint8_t)count,&candidate);
    if(r!=KSN_OK)return throw_result(ctx,r,op);
    if(old&&old->binding_count==count&&
       memcmp(old->bindings,bindings,count*sizeof(*bindings))==0)return JS_UNDEFINED;
    for(unsigned i=0;i<s->asset->source_count;i++)
        if((!local||i!=source_index)&&
           (sources->entries[i].subscription.bound_slots&candidate.bound_slots))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "another source owns a bound slot",false,NULL);
    if(external)for(unsigned i=0;i<external->count;i++)
        if((local||(int)i!=external_index)&&
           (external->entries[i].subscription.bound_slots&candidate.bound_slots))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "another source owns a bound slot",false,NULL);
    unsigned active_sources=external?external->count:0u;
    if(sources)for(unsigned i=0;i<s->asset->source_count;i++)
        if(sources->entries[i].subscription.binding_count)active_sources++;
    if((!old||!old->binding_count)&&
       active_sources>=KSN_SOURCE_MAX_REGISTERED)
        return throw_result(ctx,KSN_LIMIT,op);
    if(!local&&!external){
        external=calloc(1,sizeof(*external));
        if(!external)return throw_result(ctx,KSN_OOM,op);
        if(sources)sources->external=external;
        else s->source_state=external;
    }
    s->pending_base_slots|=(old?old->bound_slots:0)|candidate.bound_slots;
    if(local){
        entry->subscription=candidate;
        entry->pending_revision=0;
    }else{
        schema_external *target=&external->entries[external_index>=0?
                                                   (unsigned)external_index:
                                                   external->count++];
        *target=(schema_external){.registry=offer->registry,.handle=offer->handle,
                                  .subscription=candidate};
    }
    /* The next owner step reacquires the complete current snapshot. Only the
     * first external bind allocates its fixed-capacity subscription block;
     * a temporary producer failure leaves the current frame intact. */
    return JS_UNDEFINED;
}
static JSValue js_schema_unbind_method(JSContext *ctx,JSValueConst self,
                                       int argc,JSValueConst *argv){
    const char *op="kasane.view.unbind";
    schema_state *s=schema_owner(self);
    if(!s)return throw_result(ctx,KSN_STALE,op);
    if(s->session.ticket.value)return throw_result(ctx,KSN_BUSY,op);
    if(!argc)return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                     "source is required",false,NULL);
    if(JS_IsNumber(argv[0])){
        double number;
        if(!s->asset->source_count||!number_in(ctx,argv[0],0,
                  s->asset->source_count-1u,&number))
            return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                    "source index is out of range",false,NULL);
        schema_sources *sources=schema_native(s);
        schema_source *entry=&sources->entries[(unsigned)number];
        s->pending_base_slots|=entry->subscription.bound_slots;
        entry->subscription=(ksn_source_subscription){0};
        entry->pending_revision=0;
        sources->expiry_us=0;
        return JS_UNDEFINED;
    }
    uint32_t serial=(uint32_t)(uintptr_t)JS_GetOpaque(argv[0],source_cap_class);
    if(!serial||!source_caps)return throw_result(ctx,KSN_STALE,op);
    source_cap_record *offer=NULL;
    for(unsigned i=0;i<source_caps->count;i++)
        if(source_caps->records[i].serial==serial){
            offer=&source_caps->records[i];break;
        }
    if(!offer)return throw_result(ctx,KSN_STALE,op);
    schema_externals *external=schema_external_state(s);
    if(external)for(unsigned i=0;i<external->count;i++)
        if(external->entries[i].registry==offer->registry&&
           external->entries[i].handle.index==offer->handle.index&&
           external->entries[i].handle.generation==offer->handle.generation){
            schema_external_remove(s,i);
            break;
        }
    return JS_UNDEFINED;
}
static const JSCFunctionListEntry schema_methods[]={
    JS_CFUNC_DEF("set",1,js_schema_set_method),
    JS_CFUNC_DEF("bind",2,js_schema_bind_method),
    JS_CFUNC_DEF("unbind",1,js_schema_unbind_method),
};
static bool lazy_proto(JSContext *ctx,JSClassID id,const JSCFunctionListEntry *methods,int count);
static JSValue js_schema_mount(JSContext *ctx,const pocket_app_view_asset *asset,
                               void *owned_asset,size_t owned_asset_bytes){
    if(!ensure_state(ctx,"kasane.mount"))return JS_EXCEPTION;
    if(state->schema||state->provider||state->building.value||
       state->submitted.value||state->active)
        return throw_result(ctx,KSN_BUSY,"kasane.mount");
    if(schema_serial==UINT32_MAX)return throw_result(ctx,KSN_LIMIT,"kasane.mount");
    if(!lazy_proto(ctx,schema_class,schema_methods,
                   (int)(sizeof(schema_methods)/sizeof(schema_methods[0]))))
        return JS_EXCEPTION;
    JSValue object=JS_NewObjectClass(ctx,schema_class);
    if(JS_IsException(object))return object;
    schema_state *s=schema_alloc(asset,schema_serial+1u);
    if(!s){JS_FreeValue(ctx,object);return throw_result(ctx,KSN_OOM,"kasane.mount");}
    s->owned_asset=owned_asset;
    s->owned_asset_bytes=owned_asset_bytes;
    schema_serial=s->handle;
    JS_SetOpaque(object,(void *)(uintptr_t)s->handle);
    state->schema=s;
    return object;
}
/* Runtime declarations compile once into the same immutable descriptor that
 * flash assets use. The pools are one mount-time allocation, never visited by
 * the per-frame source/key fast path. */
#define RUNTIME_NAME_BYTES 32u
typedef struct {
    pocket_app_view_asset asset;
    ksn_schema schema;
    size_t bytes;
} runtime_descriptor;
static bool runtime_u16(JSContext *ctx,JSValueConst object,const char *key,
                        uint16_t maximum,uint16_t *out,bool required){
    JSValue value=JS_GetPropertyStr(ctx,object,key);
    if(JS_IsException(value))return false;
    if(JS_IsUndefined(value)&&!required){JS_FreeValue(ctx,value);return true;}
    double number;bool ok=number_in(ctx,value,0,maximum,&number);
    JS_FreeValue(ctx,value);
    if(ok)*out=(uint16_t)number;
    return ok;
}
static bool runtime_binding(JSContext *ctx,JSValueConst object,const char *key,
                            ksn_slot_type type,const ksn_schema *schema,
                            char literal[KSN_SCHEMA_TEXT_MAX+1u],
                            ksn_schema_binding *out){
    JSValue value=JS_GetPropertyStr(ctx,object,key);
    if(JS_IsException(value))return false;
    JSValue slot=JS_IsObject(value)&&!JS_IsArray(value)?
                 JS_GetPropertyStr(ctx,value,"slot"):JS_UNDEFINED;
    if(JS_IsException(slot)){JS_FreeValue(ctx,value);return false;}
    bool ok=false;
    if(JS_IsString(slot)){
        const char *name=bounded_cstring(ctx,slot,RUNTIME_NAME_BYTES-1u);
        if(name){
            for(unsigned i=0;i<schema->slot_count;i++)
                if(strcmp(name,schema->slots[i].name)==0&&schema->slots[i].type==type){
                    *out=(ksn_schema_binding){.slot=(uint8_t)i};ok=true;break;
                }
            JS_FreeCString(ctx,name);
        }
    }else if(JS_IsUndefined(slot)){
        *out=(ksn_schema_binding){.slot=KSN_SCHEMA_LITERAL};
        if(type==KSN_SLOT_RECT)ok=parse_rect_mode(ctx,value,&out->literal.rect,
                                                  "kasane.mount",false);
        else if(type==KSN_SLOT_COLOR){
            uint32_t color;ok=parse_u32(ctx,value,&color);
            if(ok)out->literal.color=color;
        }else if(type==KSN_SLOT_BOOL){
            ok=JS_IsBool(value);if(ok)out->literal.boolean=JS_ToBool(ctx,value)!=0;
        }else if(type==KSN_SLOT_U16){
            double n;ok=number_in(ctx,value,0,UINT16_MAX,&n);
            if(ok)out->literal.number=(uint16_t)n;
        }else if(type==KSN_SLOT_RESOURCE){
            out->literal.resource.value=opaque_value(value,image_class);
            ok=out->literal.resource.value!=0;
        }else if(type==KSN_SLOT_TEXT&&JS_IsString(value)){
            int64_t units=0;
            if(JS_GetLength(ctx,value,&units)==0&&units<=KSN_SCHEMA_TEXT_MAX){
                size_t length=0;const char *src=JS_ToCStringLen(ctx,&length,value);
                if(src){
                    ksn_p0_probe_copy(KSN_P0_UTF8_MATERIALIZED,length);
                    if(length<=KSN_SCHEMA_TEXT_MAX){
                        memcpy(literal,src,length);literal[length]=0;
                        ksn_p0_probe_copy(KSN_P0_ADAPTER_TEMP,length);
                        out->literal.text=(ksn_schema_text){literal,(uint16_t)length};ok=true;
                    }
                    JS_FreeCString(ctx,src);
                }
            }
        }
    }
    JS_FreeValue(ctx,slot);JS_FreeValue(ctx,value);
    return ok;
}
static bool runtime_optional_binding(JSContext *ctx,JSValueConst object,const char *key,
                                     ksn_slot_type type,const ksn_schema *schema,
                                     char literal[KSN_SCHEMA_TEXT_MAX+1u],
                                     ksn_schema_binding *out,bool *present){
    JSValue value=JS_GetPropertyStr(ctx,object,key);
    if(JS_IsException(value))return false;
    *present=!JS_IsUndefined(value);JS_FreeValue(ctx,value);
    return !*present||runtime_binding(ctx,object,key,type,schema,literal,out);
}
static runtime_descriptor *runtime_compile(JSContext *ctx,JSValueConst definition){
    JSValue slots=JS_GetPropertyStr(ctx,definition,"slots");
    JSValue nodes=JS_GetPropertyStr(ctx,definition,"nodes");
    JSPropertyEnum *properties=NULL;uint32_t slot_count=0;
    int64_t node_count=0;
    bool valid=JS_IsObject(slots)&&!JS_IsArray(slots)&&JS_IsArray(nodes)&&
        JS_GetOwnPropertyNames(ctx,&properties,&slot_count,slots,
                               JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY)==0&&
        JS_GetLength(ctx,nodes,&node_count)==0&&
        slot_count<=KSN_SCHEMA_MAX_SLOTS&&node_count>=0&&
        node_count<=KSN_SCHEMA_MAX_NODES;
    runtime_descriptor *r=NULL;
    if(valid){
        size_t bytes=sizeof(*r)+(size_t)slot_count*sizeof(ksn_schema_slot)+
          (size_t)node_count*sizeof(ksn_schema_node)+
          (size_t)slot_count*RUNTIME_NAME_BYTES+
          (size_t)node_count*(KSN_SCHEMA_TEXT_MAX+1u);
        r=calloc(1,bytes);valid=r!=NULL;
        if(valid){
            r->bytes=bytes;
            char *pool=(char *)(r+1);
            ksn_schema_slot *slot_defs=(ksn_schema_slot *)pool;
            pool+=(size_t)slot_count*sizeof(*slot_defs);
            ksn_schema_node *node_defs=(ksn_schema_node *)pool;
            pool+=(size_t)node_count*sizeof(*node_defs);
            char *names=pool;pool+=(size_t)slot_count*RUNTIME_NAME_BYTES;
            char *literal_text=pool;
            r->asset.schema=&r->schema;
            r->schema=(ksn_schema){.version=1,.slot_count=(uint8_t)slot_count,
                .node_count=(uint8_t)node_count,.background=0x000000ffu,
                .slots=slot_defs,.nodes=node_defs};
            uint16_t version=0;
            valid=runtime_u16(ctx,definition,"version",1,&version,true)&&version==1;
            JSValue bg=JS_GetPropertyStr(ctx,definition,"background");
            if(JS_IsException(bg))valid=false;
            else if(!JS_IsUndefined(bg)){
                uint32_t color=0;
                if(!parse_u32(ctx,bg,&color))valid=false;
                else r->schema.background=color;
            }
            JS_FreeValue(ctx,bg);
            for(unsigned i=0;valid&&i<slot_count;i++){
                JSValue name_value=JS_AtomToValue(ctx,properties[i].atom);
                const char *name=bounded_cstring(ctx,name_value,RUNTIME_NAME_BYTES-1u);
                JS_FreeValue(ctx,name_value);
                if(!name){valid=false;break;}
                size_t len=strlen(name);
                if(!len||len>=RUNTIME_NAME_BYTES)valid=false;
                else{memcpy(names+i*RUNTIME_NAME_BYTES,name,len+1u);
                     slot_defs[i].name=names+i*RUNTIME_NAME_BYTES;}
                JS_FreeCString(ctx,name);
                if(!valid)break;
                JSValue spec=JS_GetProperty(ctx,slots,properties[i].atom);
                JSValue type=JS_IsObject(spec)?JS_GetPropertyStr(ctx,spec,"type"):JS_UNDEFINED;
                const char *label=bounded_cstring(ctx,type,16);
                if(!label)valid=false;
                else{
                    static const char *const types[]={"text","rect","color","bool","u16","resource"};
                    unsigned t=0;for(;t<6;t++)if(strcmp(label,types[t])==0)break;
                    if(t==6)valid=false;else slot_defs[i].type=(ksn_slot_type)t;
                    JS_FreeCString(ctx,label);
                }
                if(valid&&slot_defs[i].type==KSN_SLOT_TEXT){
                    uint16_t cap=0;valid=runtime_u16(ctx,spec,"capacity",KSN_SCHEMA_TEXT_MAX,&cap,true)&&cap>0;
                    slot_defs[i].capacity=(uint8_t)cap;
                }
                if(valid&&slot_defs[i].type==KSN_SLOT_U16){
                    valid=runtime_u16(ctx,spec,"initial",UINT16_MAX,&slot_defs[i].initial_number,false)&&
                          runtime_u16(ctx,spec,"maximum",UINT16_MAX,&slot_defs[i].maximum,false);
                }
                JS_FreeValue(ctx,type);JS_FreeValue(ctx,spec);
            }
            JSValue background_slot=JS_GetPropertyStr(ctx,definition,"backgroundSlot");
            if(JS_IsException(background_slot))valid=false;
            else if(valid&&!JS_IsUndefined(background_slot)){
                const char *name=bounded_cstring(ctx,background_slot,RUNTIME_NAME_BYTES-1u);
                if(!name)valid=false;
                else{
                    bool found=false;
                    for(unsigned i=0;i<slot_count;i++)
                        if(strcmp(name,slot_defs[i].name)==0&&slot_defs[i].type==KSN_SLOT_COLOR){
                            r->schema.dynamic_background=true;
                            r->schema.background_slot=(uint8_t)i;found=true;break;
                        }
                    if(!found)valid=false;
                    JS_FreeCString(ctx,name);
                }
            }
            JS_FreeValue(ctx,background_slot);
            for(unsigned i=0;valid&&i<(unsigned)node_count;i++){
                JSValue spec=JS_GetPropertyUint32(ctx,nodes,i);
                JSValue type=JS_IsObject(spec)?JS_GetPropertyStr(ctx,spec,"type"):JS_UNDEFINED;
                const char *label=bounded_cstring(ctx,type,16);
                if(!label)valid=false;
                else{
                    static const char *const types[]={"rect","roundRect","text","image","plateText"};
                    unsigned t=0;for(;t<5;t++)if(strcmp(label,types[t])==0)break;
                    if(t==5)valid=false;else node_defs[i].kind=(ksn_schema_node_kind)t;
                    JS_FreeCString(ctx,label);
                }
                ksn_schema_node *n=&node_defs[i];
                char *text=literal_text+i*(KSN_SCHEMA_TEXT_MAX+1u);
                if(valid)valid=runtime_binding(ctx,spec,"bounds",KSN_SLOT_RECT,&r->schema,text,&n->bounds);
                JSValue add=JS_GetPropertyStr(ctx,spec,"rectAdd");
                if(JS_IsException(add))valid=false;
                else if(!JS_IsUndefined(add)){
                    int64_t length=0;
                    if(!JS_IsArray(add)||JS_GetLength(ctx,add,&length)<0||length!=4)valid=false;
                    for(unsigned edge=0;valid&&edge<4;edge++){
                        JSValue name_value=JS_GetPropertyUint32(ctx,add,edge);
                        if(JS_IsException(name_value)){valid=false;break;}
                        if(!JS_IsNull(name_value)&&!JS_IsUndefined(name_value)){
                            const char *name=bounded_cstring(ctx,name_value,RUNTIME_NAME_BYTES-1u);
                            if(!name)valid=false;
                            else{
                                bool found=false;
                                for(unsigned slot=0;slot<slot_count;slot++)
                                    if(strcmp(name,slot_defs[slot].name)==0&&
                                       slot_defs[slot].type==KSN_SLOT_U16){
                                        n->rect_add_mask|=(uint8_t)(1u<<edge);
                                        n->rect_add_slot[edge]=(uint8_t)slot;found=true;break;
                                    }
                                if(!found)valid=false;
                                JS_FreeCString(ctx,name);
                            }
                        }
                        JS_FreeValue(ctx,name_value);
                    }
                }
                JS_FreeValue(ctx,add);
                if(valid&&n->kind!=KSN_NODE_IMAGE)
                    valid=runtime_binding(ctx,spec,"color",KSN_SLOT_COLOR,&r->schema,text,&n->color);
                if(valid&&(n->kind==KSN_NODE_TEXT||n->kind==KSN_NODE_PLATE_TEXT))
                    valid=runtime_binding(ctx,spec,"text",KSN_SLOT_TEXT,&r->schema,text,&n->text);
                if(valid&&n->kind==KSN_NODE_PLATE_TEXT)
                    valid=runtime_binding(ctx,spec,"plateColor",KSN_SLOT_COLOR,&r->schema,text,&n->plate_color);
                if(valid&&n->kind==KSN_NODE_IMAGE){
                    valid=runtime_binding(ctx,spec,"resource",KSN_SLOT_RESOURCE,&r->schema,text,&n->resource)&&
                          runtime_binding(ctx,spec,"variant",KSN_SLOT_U16,&r->schema,text,&n->variant)&&
                          runtime_binding(ctx,spec,"frame",KSN_SLOT_U16,&r->schema,text,&n->frame)&&
                          runtime_u16(ctx,spec,"sourceWidth",256,&n->source_width,true)&&
                          runtime_u16(ctx,spec,"sourceHeight",256,&n->source_height,true);
                }
                bool present=false;
                if(valid)valid=runtime_optional_binding(ctx,spec,"visible",KSN_SLOT_BOOL,&r->schema,
                                                         text,&n->visible,&present);
                if(present)n->flags|=KSN_SCHEMA_HAS_VISIBLE;
                if(valid)valid=runtime_optional_binding(ctx,spec,"page",KSN_SLOT_U16,&r->schema,
                                                         text,&n->page,&present);
                if(present){n->flags|=KSN_SCHEMA_HAS_PAGE;
                    valid=valid&&runtime_u16(ctx,spec,"pageEquals",UINT16_MAX,&n->page_equals,true);}
                if(valid)valid=runtime_optional_binding(ctx,spec,"reveal",KSN_SLOT_U16,&r->schema,
                                                         text,&n->reveal,&present);
                if(present)n->flags|=KSN_SCHEMA_HAS_REVEAL;
                uint16_t radius=0,font=0;
                if(valid)valid=runtime_u16(ctx,spec,"radius",8,&radius,false)&&
                               runtime_u16(ctx,spec,"font",KSN_DISPLAY,&font,false);
                n->radius=(uint8_t)radius;n->font=(ksn_font)font;
                JS_FreeValue(ctx,type);JS_FreeValue(ctx,spec);
            }
            if(valid)valid=ksn_schema_validate(&r->schema)==KSN_OK;
        }
    }
    JS_FreePropertyEnum(ctx,properties,slot_count);
    JS_FreeValue(ctx,slots);JS_FreeValue(ctx,nodes);
    if(!valid){free(r);return NULL;}
    return r;
}
static JSValue js_presenter_mount(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    if(argc&&JS_IsObject(argv[0])&&!JS_IsArray(argv[0])){
        runtime_descriptor *compiled=runtime_compile(ctx,argv[0]);
        if(!compiled)return JS_HasException(ctx)?JS_EXCEPTION:
            pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.mount",
                             "invalid view definition",false,NULL);
        JSValue object=js_schema_mount(ctx,&compiled->asset,compiled,compiled->bytes);
        if(JS_IsException(object)){free(compiled);return object;}
        if(argc>1&&!JS_IsUndefined(argv[1])){
            JSValue result=js_schema_set(ctx,state->schema,argv[1]);
            if(JS_IsException(result)){
                if(!state->submitted.value){
                    schema_state *failed=state->schema;state->schema=NULL;
                    free(failed);free(compiled);
                }
                JS_FreeValue(ctx,object);return result;
            }
            JS_FreeValue(ctx,result);
        }
        return object;
    }
    const char *id=argc?bounded_cstring(ctx,argv[0],31):NULL;
    if(!id)return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.mount",
                                  "view id must be a registered presenter",false,NULL);
    const pocket_app_view_asset *asset=pocket_app_view_lookup(id);
    if(asset){JS_FreeCString(ctx,id);return js_schema_mount(ctx,asset,NULL,0);}
    const pocket_app_view_provider *provider=pocket_app_view_provider_lookup(id);
    JS_FreeCString(ctx,id);
    if(!provider)return throw_result(ctx,KSN_UNSUPPORTED,"kasane.mount");
    if(!ensure_state(ctx,"kasane.mount"))return JS_EXCEPTION;
    if(state->schema||state->provider||state->building.value||state->submitted.value||state->active)
        return throw_result(ctx,KSN_BUSY,"kasane.mount");
    pocket_app_view_host host={.owner=state,.view=view(),.viewport=viewport,
        .now_us=&owner_now_us,.busy=provider_busy,.submitted=provider_submitted};
    void *instance=NULL;
    JSValue object=provider->mount(ctx,&host,&instance);
    if(JS_IsException(object))return object;
    if(!instance){JS_FreeValue(ctx,object);return throw_result(ctx,KSN_INVALID,"kasane.mount");}
    state->provider=provider;state->provider_state=instance;
    return object;
}
static const JSCFunctionListEntry functions[]={
    JS_CFUNC_DEF("resource",1,js_resource),
    JS_CFUNC_DEF("mount",1,js_presenter_mount),
    JS_CFUNC_DEF("createScene",1,js_create_scene),
    JS_CFUNC_DEF("replace",1,js_replace),JS_CFUNC_DEF("patch",1,js_patch),
    JS_CFUNC_DEF("poll",0,js_poll),JS_CFUNC_DEF("cancel",1,js_cancel),
    JS_CFUNC_DEF("features",0,js_features),JS_CFUNC_DEF("stats",0,js_stats),
    JS_CFUNC_DEF("inputScope",0,js_input_scope),
};

static bool register_class(JSContext *ctx, JSClassID *id, JSRuntime **owner,
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
static bool instance_proto(JSContext *ctx) {
    return lazy_proto(ctx,instance_class,instance_methods,COUNT(instance_methods));
}
static bool animation_proto(JSContext *ctx) {
    return lazy_proto(ctx,animation_class,animation_methods,COUNT(animation_methods));
}

static esp_err_t build_kasane(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    /* Instance and animation prototypes are built on first use (above). */
    if(!register_class(ctx,&tx_class,&tx_rt,&tx_def)||
       !register_class(ctx,&modal_class,&modal_rt,&modal_def)||
       !register_class(ctx,&ref_class,&ref_rt,&ref_def)||
       !register_class(ctx,&instance_class,&instance_rt,&instance_def)||
       !register_class(ctx,&animation_class,&animation_rt,&animation_def)||
       !register_class(ctx,&template_class,&template_rt,&template_def)||
       !register_class(ctx,&image_class,&image_rt,&image_def)||
       !register_class(ctx,&ticket_class,&ticket_rt,&ticket_def)||
       !register_class(ctx,&scene_class,&scene_rt,&scene_def)||
       !register_class(ctx,&schema_class,&schema_rt,&schema_def)||
       !set_proto(ctx,tx_class,tx_methods,COUNT(tx_methods))||
       !set_proto(ctx,modal_class,modal_methods,COUNT(modal_methods))||
       !set_proto(ctx,ref_class,ref_methods,COUNT(ref_methods))) return ESP_ERR_NO_MEM;
    /* Handles with no methods, and scenes (whose methods are own properties),
     * inherit Object.prototype directly instead of each owning an empty object. */
    JSValue plain=JS_NewObject(ctx);
    if(JS_IsException(plain)) return ESP_ERR_NO_MEM;
    JSValue object_proto=JS_GetPrototype(ctx,plain);
    JS_FreeValue(ctx,plain);
    JS_SetClassProto(ctx,template_class,JS_DupValue(ctx,object_proto));
    JS_SetClassProto(ctx,image_class,JS_DupValue(ctx,object_proto));
    JS_SetClassProto(ctx,ticket_class,JS_DupValue(ctx,object_proto));
    JS_SetClassProto(ctx,scene_class,object_proto);
    if(JS_SetPropertyFunctionList(ctx,ns,functions,
       (int)(sizeof(functions)/sizeof(functions[0])))<0) return ESP_ERR_NO_MEM;
    JSValue cache=JS_NewObject(ctx);
    if(JS_IsException(cache)) return ESP_ERR_NO_MEM;
    /* QuickJS keeps these entries for lazy function materialization. */
    static const JSCFunctionListEntry cache_functions[]={
        JS_CFUNC_DEF("create",1,js_cache_create),JS_CFUNC_DEF("release",1,js_cache_release),
    };
    if(JS_SetPropertyFunctionList(ctx,cache,cache_functions,2)<0) {
        JS_FreeValue(ctx,cache);return ESP_ERR_NO_MEM;
    }
    return JS_SetPropertyStr(ctx,ns,"cache",cache)<0?ESP_ERR_NO_MEM:ESP_OK;
}

static const pocket_limit_t kasane_limits[]={
    {.name="commands",.kind=POCKET_LIMIT_INT,.number=KSN_APP_COMMANDS},
    {.name="references",.kind=POCKET_LIMIT_INT,.number=KASANE_REF_LIMIT},
    {.name="cacheTemplates",.kind=POCKET_LIMIT_INT,.number=KSN_CACHE_TEMPLATES},
    {.name="cacheInstances",.kind=POCKET_LIMIT_INT,.number=KSN_CACHE_INSTANCES},
    {0},
};
static const pocket_capability_t capability={
    .name="display.kasane",.supported=true,.available=true,.limits=kasane_limits,
};

esp_err_t pocket_kasane_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    esp_err_t result=pocket_api_register(&capability);
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
    if(!register_class(ctx,&source_cap_class,&source_cap_rt,&source_cap_def))
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

void pocket_kasane_reset(void) {
    const pocket_app_view_provider *provider=state?state->provider:NULL;
    void *provider_state=state?state->provider_state:NULL;
    schema_state *schema=state?state->schema:NULL;
    if(state&&ksn_runtime_app_detach(state->lease)==KSN_BUSY)return;
    if(provider)provider->destroy(provider_state);
    if(schema)free(schema_external_state(schema));
    if(schema)free(schema->owned_asset);
    free(schema);
    free(source_caps);source_caps=NULL;
    state=NULL;   /* the runtime released it with the lease */
    viewport=KASANE_SCREEN;
    overlay_profile=false;
    owner_now_us=0;
}
bool pocket_kasane_active(void) { return state&&state->active; }
ksn_result pocket_kasane_update_notice(const sys_notice *notice,uint16_t variant){
    if(!state||!state->active)return KSN_OK;
    ksn_view *system=ksn_runtime_app_system_view(state->lease);
    if(!system)return KSN_BUSY;
    if(state->notice_tx.value){
        ksn_submission outcome=ksn_view_poll(system);
        if(outcome.ticket.value!=state->notice_tx.value)return KSN_STALE;
        if(outcome.status==KSN_SUBMITTED)return KSN_BUSY;
        if(outcome.status==KSN_PRESENTED){
            state->notice_displayed=state->notice_pending;
            state->notice_variant=state->notice_pending_variant;
        }
        state->notice_tx=(ksn_tx){0};
    }
    uint32_t id=notice?notice->id:0;
    if(id==state->notice_displayed&&(!id||variant==state->notice_variant))return KSN_OK;
    if(notice&&!state->notice_resource.value){
        ksn_image_port image;ksn_result result=ksn_pet_builtin_image(&image);
        if(result==KSN_OK)result=ksn_view_host_register_image(system,&image,&state->notice_resource);
        if(result!=KSN_OK)return result;
    }
    ksn_tx tx;ksn_result result=ksn_view_begin(system,KSN_REPLACE,&tx);
    if(result!=KSN_OK)return result;
    result=ksn_notice_emit(system,tx,notice,state->notice_resource,variant);
    if(result==KSN_OK)result=ksn_view_submit(system,tx);
    if(result!=KSN_OK){ksn_view_cancel(system,tx);return result;}
    state->notice_tx=tx;state->notice_pending=id;state->notice_pending_variant=variant;
    return KSN_OK;
}
bool pocket_kasane_notice_composited(void){
    return state&&ksn_runtime_app_system_view(state->lease)&&
        (state->notice_displayed||(state->notice_tx.value&&state->notice_pending));
}
bool pocket_kasane_system_pending(void){return state&&ksn_runtime_app_system_view(state->lease)&&state->notice_tx.value;}
bool pocket_kasane_has_submission(void) {
    return ksn_runtime_has_submission();
}
uint32_t pocket_kasane_source_wait_ticks(uint64_t now_us,uint32_t cap,uint32_t hz){
    if(!state||!state->schema||!hz)return cap;
    schema_sources *native=schema_native(state->schema);
    schema_externals *external=schema_external_state(state->schema);
    uint64_t next=native&&native->expiry_us?native->expiry_us:UINT64_MAX;
    if(external&&external->expiry_us&&external->expiry_us<next)
        next=external->expiry_us;
    /* A due deadline is retried by the regular owner frame. Returning zero
     * here could busy-spin after a failed or pending submission. */
    if(next==UINT64_MAX||next<=now_us)return cap;
    uint64_t delta=next-now_us,seconds=delta/1000000u;
    if(seconds>cap/hz)return cap;
    uint64_t ticks=seconds*hz+((delta%1000000u)*hz+999999u)/1000000u;
    return ticks<cap?(uint32_t)ticks:cap;
}
ksn_result pocket_kasane_advance(uint64_t now_us){
    apply_outcome();return ksn_runtime_advance_animations(now_us);
}
bool pocket_kasane_animation_pending(void){return ksn_runtime_animation_pending();}
void pocket_kasane_animations_presented(uint64_t now_us){ksn_runtime_animations_presented(now_us);}
void pocket_kasane_set_animation_time(uint64_t now_us){
    owner_now_us=now_us;
    ksn_runtime_set_animation_time(now_us);
}
bool pocket_kasane_needs_present(void) {
    return ksn_runtime_needs_present();
}
void pocket_kasane_invalidate_bands(uint32_t bands) {
    ksn_runtime_invalidate_bands(bands);
}
void pocket_kasane_invalidate(void) {
    ksn_runtime_invalidate();
}
ksn_result pocket_kasane_present(const ksn_display_port *display,ksn_render_stats *stats) {
    if(!stats) return KSN_INVALID;
    *stats=(ksn_render_stats){0};
    if(!pocket_kasane_needs_present()) return KSN_OK;
    ksn_result result=ksn_runtime_present(display,stats);
    apply_outcome();return result;
}
ksn_result pocket_kasane_present_backdrop(const ksn_display_port *display,
                                          ksn_backdrop_loader load,ksn_render_stats *stats) {
    if(!stats||!load)return KSN_INVALID;
    *stats=(ksn_render_stats){0};
    if(!pocket_kasane_needs_present())return KSN_OK;
    ksn_result result=ksn_runtime_present_backdrop(display,load,stats);
    apply_outcome();return result;
}
void pocket_kasane_end_turn(void) {
    if(state) { ksn_runtime_app_end_turn(state->lease); apply_outcome(); }
}
ksn_input_scope pocket_kasane_input_scope(bool host_priority) {
    return ksn_runtime_input_scope(host_priority);
}
