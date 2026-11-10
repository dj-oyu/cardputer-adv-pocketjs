#include "diagnostic_control.h"
#include <string.h>
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"diagnostic snapshots require lock-free uint atomics");
/* Internal phases are deliberately separate from transport state numbers. */
enum { P_PENDING=1,P_DRAINING,P_WORKING,P_WORK_END,P_RESTORING,P_FINISHING,
       P_COMPLETE,P_CANCELLED,P_REFUSED,P_QUARANTINED };
enum { CANCEL_BIT=1u<<16,STARTED_BIT=1u<<17,FLAGS_MASK=0xff00u };
static unsigned phase(unsigned w){return w&255u;}
static bool terminal(unsigned w){return phase(w)>=P_COMPLETE;}
static unsigned packed(unsigned p,unsigned flags){return p|(flags<<8);}
static diagnostic_control_snapshot unknown(void){return (diagnostic_control_snapshot){DC_REFUSED,0,false};}
static diagnostic_control_snapshot refused(void){return (diagnostic_control_snapshot){DC_REFUSED,DC_NOT_STARTED,false};}
static bool nonzero(const uint8_t *id){
    if(!id)return false;
    unsigned v=0;for(unsigned i=0;i<DC_ID_BYTES;i++)v|=id[i];return v!=0;
}
static diagnostic_control_snapshot snapshot(const diagnostic_control_receipt *r){
    unsigned w=atomic_load(&r->word);diagnostic_control_state s;
    switch(phase(w)){
    case P_COMPLETE:s=DC_COMPLETED;break;
    case P_CANCELLED:s=DC_CANCELLED;break;
    case P_REFUSED:s=DC_REFUSED;break;
    case P_QUARANTINED:s=DC_QUARANTINED;break;
    default:s=(w&CANCEL_BIT)?DC_CANCEL_REQUESTED:(w&STARTED_BIT)?DC_RUNNING:DC_PENDING;break;
    }
    return (diagnostic_control_snapshot){s,(uint8_t)(w>>8),true};
}
static int find(const diagnostic_control *c,const uint8_t *id){
    unsigned n=atomic_load(&c->used);
    for(unsigned i=0;i<n;i++)if(!memcmp(c->receipt[i].id,id,DC_ID_BYTES))return (int)i;
    return -1;
}
bool diagnostic_control_init(diagnostic_control *c,uint64_t boot,bool enabled,diagnostic_session *s){
    if(!c||!boot||(enabled&&!s))return false;
    c->boot=boot;c->enabled=enabled;c->session=s;
    atomic_init(&c->used,0);atomic_init(&c->active,0);
    for(unsigned i=0;i<DC_SLOTS;i++){
        memset(c->receipt[i].id,0,DC_ID_BYTES);atomic_init(&c->receipt[i].word,0);
    }
    return true;
}
diagnostic_control_snapshot diagnostic_control_status(const diagnostic_control *c,uint64_t boot,const uint8_t id[DC_ID_BYTES]){
    if(!c||boot!=c->boot||!nonzero(id))return unknown();
    int i=find(c,id);return i<0?unknown():snapshot(c->receipt+i);
}
diagnostic_control_snapshot diagnostic_control_request(diagnostic_control *c,uint64_t boot,const uint8_t id[DC_ID_BYTES]){
    if(!c||boot!=c->boot||!nonzero(id))return unknown();
    int existing=find(c,id);if(existing>=0)return snapshot(c->receipt+existing);
    if(!c->enabled)return refused();
    unsigned n=atomic_load(&c->used);if(n==DC_SLOTS)return refused();
    bool busy=atomic_load(&c->active)!=0||atomic_load(&c->session->command.state)!=DIAG_CMD_IDLE;
    diagnostic_control_receipt *r=c->receipt+n;
    memcpy(r->id,id,DC_ID_BYTES);
    atomic_store(&r->word,packed(busy?P_REFUSED:P_PENDING,DC_NOT_STARTED));
    /* Publish immutable ID and initial receipt before UI can select the slot. */
    atomic_store(&c->used,n+1);
    if(!busy){
        unsigned idle=0;
        if(!atomic_compare_exchange_strong(&c->active,&idle,n+1))
            atomic_store(&r->word,packed(P_REFUSED,DC_NOT_STARTED));
    }
    return snapshot(r);
}
static void mark_cancel(diagnostic_control_receipt *r){
    unsigned w=atomic_load(&r->word);
    while(!terminal(w)&&!(w&CANCEL_BIT))
        if(atomic_compare_exchange_weak(&r->word,&w,w|CANCEL_BIT))break;
}
diagnostic_control_snapshot diagnostic_control_cancel(diagnostic_control *c,uint64_t boot,const uint8_t id[DC_ID_BYTES]){
    if(!c||boot!=c->boot||!nonzero(id))return unknown();
    int i=find(c,id);if(i<0)return unknown();
    mark_cancel(c->receipt+i);return snapshot(c->receipt+i);
}
void diagnostic_control_local_cancel(diagnostic_control *c){
    unsigned i=atomic_load(&c->active);if(i)mark_cancel(c->receipt+i-1);
}
bool diagnostic_control_busy(const diagnostic_control *c){return atomic_load(&c->active)!=0;}
static void progress(diagnostic_control_receipt *r,unsigned p,unsigned add,unsigned clear){
    unsigned w=atomic_load(&r->word);
    for(;;){
        unsigned flags=((w&FLAGS_MASK)>>8);flags=(flags|add)&~clear;
        unsigned next=(w&~(255u|FLAGS_MASK))|packed(p,flags);
        if(atomic_compare_exchange_weak(&r->word,&w,next))return;
    }
}
static void close_receipt(diagnostic_control_receipt *r,unsigned p){
    unsigned w=atomic_load(&r->word);
    for(;;){
        unsigned next_phase=p;
        if(p==P_COMPLETE&&(w&CANCEL_BIT))next_phase=P_CANCELLED;
        unsigned next=(w&~255u)|next_phase;
        if(atomic_compare_exchange_weak(&r->word,&w,next))return;
    }
}
typedef struct {
    diagnostic_control_receipt *r;
    diagnostic_session *session;
    const diagnostic_lifecycle_ops *base;
} callbacks;
static void *acquire(void *opaque){callbacks *x=opaque;return x->base->acquire(x->base->context);}
static bool drain(void *opaque){
    callbacks *x=opaque;progress(x->r,P_DRAINING,0,DC_DRAINED);
    bool ok=x->base->drain(x->base->context);
    if(ok)progress(x->r,P_DRAINING,DC_DRAINED,0);
    return ok;
}
static bool work(void *opaque,void *owner){
    callbacks *x=opaque;
    if(atomic_load(&x->session->command.state)==DIAG_CMD_CANCEL_RUNNING)mark_cancel(x->r);
    unsigned w=atomic_load(&x->r->word);
    for(;;){
        if(w&CANCEL_BIT){progress(x->r,P_WORK_END,0,0);return true;}
        unsigned next=(w&~(255u|FLAGS_MASK))|STARTED_BIT|packed(P_WORKING,0);
        /* This CAS is the start linearization point; a later cancel is deferred. */
        if(atomic_compare_exchange_weak(&x->r->word,&w,next))break;
    }
    bool ok=x->base->work(x->base->context,owner);
    progress(x->r,P_WORK_END,DC_WORK_DONE|(ok?DC_DRAINED:0),DC_NOT_STARTED);
    return ok;
}
static bool restore(void *opaque){
    callbacks *x=opaque;
    progress(x->r,P_RESTORING,0,DC_DRAINED|DC_HOME|DC_BORROWS_RELEASED|DC_OWNER_RELEASED);
    bool ok=x->base->restore(x->base->context);
    if(ok)progress(x->r,P_FINISHING,DC_DRAINED|DC_HOME|DC_BORROWS_RELEASED,0);
    return ok;
}
static void release(void *opaque,void *owner){
    callbacks *x=opaque;x->base->release(x->base->context,owner);
    progress(x->r,P_FINISHING,DC_BORROWS_RELEASED|DC_OWNER_RELEASED,0);
}
static diagnostic_control_owner_result refuse_active(diagnostic_control *c,diagnostic_control_receipt *r){
    progress(r,P_REFUSED,DC_NOT_STARTED,DC_WORK_DONE|DC_DRAINED|DC_HOME|DC_BORROWS_RELEASED|DC_OWNER_RELEASED);
    atomic_store(&c->active,0);return DC_OWNER_REFUSED;
}
diagnostic_control_owner_result diagnostic_control_pump(diagnostic_control *c,const diagnostic_lifecycle_ops *ops,bool admitted){
    if(!c||!c->enabled)return DC_OWNER_NONE;
    unsigned i=atomic_load(&c->active);if(!i)return DC_OWNER_NONE;
    diagnostic_control_receipt *r=c->receipt+i-1;
    if(phase(atomic_load(&r->word))==P_QUARANTINED)return DC_OWNER_TERMINAL;
    if(!admitted||c->session->lifecycle.quarantined||c->session->lifecycle.owner||
       atomic_load(&c->session->command.state)!=DIAG_CMD_IDLE)
        return refuse_active(c,r);
    if(!ops||!ops->acquire||!ops->drain||!ops->work||!ops->restore||!ops->release)
        return refuse_active(c,r);
    if(diagnostic_command_request(&c->session->command)!=DIAG_REQUEST_QUEUED)
        return refuse_active(c,r);
    callbacks context={r,c->session,ops};
    diagnostic_lifecycle_ops wrapped={&context,acquire,drain,work,restore,release};
    diagnostic_session_result result=diagnostic_session_pump(c->session,&wrapped,true);
    if(result==DIAG_SESSION_WAITING||
       (result==DIAG_SESSION_CANCELLED&&!(atomic_load(&r->word)&(DC_OWNER_RELEASED<<8)))){
        /* A physical cancel suppressed begin. Keep its identity and retry only
         * checked no-work cleanup on a later admitted owner turn. */
        mark_cancel(r);return DC_OWNER_WAITING;
    }
    if(result==DIAG_SESSION_TERMINAL){close_receipt(r,P_QUARANTINED);return DC_OWNER_TERMINAL;}
    if(result==DIAG_SESSION_UNAVAILABLE)return refuse_active(c,r);
    if(result==DIAG_SESSION_CANCELLED)mark_cancel(r);
    close_receipt(r,P_COMPLETE);atomic_store(&c->active,0);return DC_OWNER_SAFE_END;
}
