#ifndef AYATSURI_STREAM_FRAME_OWNER_CANDIDATE_H
#define AYATSURI_STREAM_FRAME_OWNER_CANDIDATE_H
#include "ayatsuri_model_stream_candidate.h"
#include <string.h>

/* Synchronous opaque-only integration candidate. No allocation/display calls.
 * All members are private by convention; trusted native code, not a security
 * boundary. Never copy/reinitialize a live owner or mutate borrowed snapshots.
 * Generation must not be reused across owner lifetimes. */
typedef struct {
    ksn_mesh_snapshot snapshots[2];
    /* Immutable frame-associated material/program/driver package, if admitted.
     * Borrowed until this slot is replaced or its unpinned owner is released. */
    const void *payload[2];
    ayatsuri_stream_candidate *stream;
    const void *identity;
    uint32_t generation;
    uint64_t revision;
    unsigned committed;
    bool initialized, ready, produced, pinned, busy;
} ayatsuri_stream_frame_owner;
typedef struct {
    const ayatsuri_stream_frame_owner *owner;
    uint32_t generation;
    uint64_t revision;
} ayatsuri_stream_frame_token;
/* This callback must preflight the WHOLE composed frame, including materials,
 * alpha/layer limits and every band. It may borrow but not mutate/retain snapshot
 * or owner; it must not submit display work. No script/reentry is permitted. */
typedef bool (*ayatsuri_stream_frame_preflight)(void*,const ksn_mesh_snapshot*);

static inline bool ay_stream_owner_disjoint(const void*a,size_t n,const void*b,size_t m){
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return n<=UINTPTR_MAX-x&&m<=UINTPTR_MAX-y&&(x+n<=y||y+m<=x);
}
static inline bool ay_stream_owner_valid(const ayatsuri_stream_frame_owner*f,const void*id,uint32_t gen){
    return f&&id&&gen&&f->initialized&&f->identity==id&&f->generation==gen&&!f->busy;
}
/* f starts zeroed. Buffers are exclusive full-capacity allocations, not slices
 * overlapping other live display/geometry storage. Stream remains borrowed. */
static inline bool ayatsuri_stream_frame_owner_init(ayatsuri_stream_frame_owner*f,
    ayatsuri_stream_candidate*s,const void*id,uint32_t gen,void*a,size_t an,
    void*b,size_t bn,uint32_t work_limit){
    if(!f||f->initialized||!s||!id||!gen||!a||!b||!an||!bn||!work_limit||
       !s->core.alive||!s->core.model||s->core.owner!=id||s->core.generation!=gen||
       !ay_stream_owner_disjoint(a,an,b,bn))return false;
    const void*ptrs[]={f,s,s->core.arena,s->core.model,s->core.model->data};
    size_t sizes[]={sizeof *f,sizeof *s,s->core.arena_bytes,sizeof *s->core.model,s->core.model->bytes};
    for(unsigned j=0;j<5;j++){
        if(!ay_stream_owner_disjoint(a,an,ptrs[j],sizes[j])||
           !ay_stream_owner_disjoint(b,bn,ptrs[j],sizes[j]))return false;
        if(j&&!ay_stream_owner_disjoint(f,sizeof *f,ptrs[j],sizes[j]))return false;
    }
    ayatsuri_stream_frame_owner next={0};
    next.snapshots[0]=(ksn_mesh_snapshot){.packets={a,an,0,0},.work_limit=work_limit,.enabled=true};
    next.snapshots[1]=(ksn_mesh_snapshot){.packets={b,bn,0,0},.work_limit=work_limit,.enabled=true};
    next.stream=s;next.identity=id;next.generation=gen;next.initialized=true;
    *f=next;return true;
}
static inline ayatsuri_model_status ay_stream_frame_produce_common(ayatsuri_stream_frame_owner*f,
    const void*id,uint32_t gen,const float camera[9],const float light[6],
    ayatsuri_model_geometry_provider provider,void*context,ayatsuri_skin_result slot[32],
    const ayatsuri_stream_routes*routes,ayatsuri_stream_attributed_sink sink,void*sink_context,const void*payload){
    if(!ay_stream_owner_valid(f,id,gen)||f->pinned||f->revision==UINT64_MAX)return AYATSURI_MODEL_STATE;
    if(!slot||!camera||!light||!ay_stream_owner_disjoint(f,sizeof *f,slot,32*sizeof *slot)||
       !ay_stream_owner_disjoint(f,sizeof *f,camera,9*sizeof *camera)||
       !ay_stream_owner_disjoint(f,sizeof *f,light,6*sizeof *light))return AYATSURI_MODEL_INVALID;
    for(unsigned j=0;j<2;j++)if(!ay_stream_owner_disjoint(f->snapshots[j].packets.data,
        f->snapshots[j].packets.capacity,slot,32*sizeof *slot))return AYATSURI_MODEL_INVALID;
    f->busy=true;f->produced=false;f->payload[1-f->committed]=NULL;
    ayatsuri_model_status status=routes?ayatsuri_stream_candidate_frame_attributed(f->stream,id,gen,camera,light,
        provider,context,slot,&f->snapshots[1-f->committed],routes,sink,sink_context):
        ayatsuri_stream_candidate_frame(f->stream,id,gen,camera,light,provider,context,slot,&f->snapshots[1-f->committed]);
    f->busy=false;
    if(status==AYATSURI_MODEL_OK){f->revision++;f->produced=true;f->payload[1-f->committed]=payload;}
    return status;
}
/* Sink writes only exclusive pending payload storage. Its dependencies stay
 * immutable through display ACK and subsequent READY redraw. No implicit copy. */
static inline ayatsuri_model_status ayatsuri_stream_frame_produce_attributed(ayatsuri_stream_frame_owner*f,
    const void*id,uint32_t gen,const float camera[9],const float light[6],
    ayatsuri_model_geometry_provider provider,void*context,ayatsuri_skin_result slot[32],
    const ayatsuri_stream_routes*routes,ayatsuri_stream_attributed_sink sink,void*sink_context,const void*payload){
    if(!routes||!sink||!payload)return AYATSURI_MODEL_INVALID;
    return ay_stream_frame_produce_common(f,id,gen,camera,light,provider,context,slot,routes,sink,sink_context,payload);
}
static inline ayatsuri_model_status ayatsuri_stream_frame_produce(ayatsuri_stream_frame_owner*f,
    const void*id,uint32_t gen,const float camera[9],const float light[6],
    ayatsuri_model_geometry_provider provider,void*context,ayatsuri_skin_result slot[32]){
    return ay_stream_frame_produce_common(f,id,gen,camera,light,provider,context,slot,NULL,NULL,NULL,NULL);
}
/* Stage only after successful production, before preflight/acquire. No copying
 * of payload data: caller freezes its complete dependency graph through ACK and
 * every subsequent READY redraw. A replacement uses a distinct payload arena. */
static inline bool ayatsuri_stream_frame_payload(ayatsuri_stream_frame_owner*f,
    const void*id,uint32_t gen,const void*payload){
    if(!ay_stream_owner_valid(f,id,gen)||!f->produced||f->pinned)return false;
    f->payload[1-f->committed]=payload;return true;
}
/* Invalid token is all-zero. Acquire pins after complete preflight succeeds,
 * before first display submission. Committed frame is retained until ACK. */
static inline ayatsuri_stream_frame_token ayatsuri_stream_frame_acquire(
    ayatsuri_stream_frame_owner*f,const void*id,uint32_t gen,
    ayatsuri_stream_frame_preflight preflight,void*context){
    ayatsuri_stream_frame_token token={0};
    if(!ay_stream_owner_valid(f,id,gen)||!f->produced||f->pinned||!preflight)return token;
    f->busy=true;bool ok=preflight(context,&f->snapshots[1-f->committed]);f->busy=false;
    if(ok){f->pinned=true;token=(ayatsuri_stream_frame_token){f,gen,f->revision};}
    return token;
}
static inline const ksn_mesh_snapshot*ayatsuri_stream_frame_borrow(
    const ayatsuri_stream_frame_owner*f,ayatsuri_stream_frame_token token){
    if(!f||!f->initialized||f->busy||!f->pinned||token.owner!=f||
       token.generation!=f->generation||token.revision!=f->revision)return NULL;
    return &f->snapshots[1-f->committed];
}
/* drained is an external fact supplied ONLY by the authoritative display owner
 * after finish_display/completion. Token alone proves NO physical DMA completion.
 * false retains all pins. accepted=false after drain rolls back visible state;
 * display repair is caller-owned. accepted=true is presentation ACK promotion.
 * No-damage also requires the existing owner's completion/ACK ordering. */
static inline bool ayatsuri_stream_frame_complete(ayatsuri_stream_frame_owner*f,
    ayatsuri_stream_frame_token token,bool drained,bool accepted){
    if(!ayatsuri_stream_frame_borrow(f,token)||!drained)return false;
    if(accepted){f->committed=1-f->committed;f->ready=true;}
    else f->payload[1-f->committed]=NULL;
    f->pinned=false;f->produced=false;return true;
}
/* Borrow expires at next accepted completion, not at geometry production. */
static inline const ksn_mesh_snapshot*ayatsuri_stream_frame_ready(
    const ayatsuri_stream_frame_owner*f,const void*id,uint32_t gen){
    return ay_stream_owner_valid(f,id,gen)&&f->ready?&f->snapshots[f->committed]:NULL;
}
#endif
