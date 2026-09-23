#include "ksn_schema_session.h"
#include "ksn_p0_probe.h"
#include <string.h>

static bool same_rect(ksn_rect a,ksn_rect b){
    return a.x0==b.x0&&a.y0==b.y0&&a.x1==b.x1&&a.y1==b.y1;
}
static ksn_rgba background(const ksn_schema *schema,const ksn_schema_value *values){
    return schema->dynamic_background?values[schema->background_slot].data.color:
                                      schema->background;
}
ksn_result ksn_schema_session_init(ksn_schema_session *session,
                                   const ksn_schema *schema){
    if(!session)return KSN_INVALID;
    ksn_schema_dependencies dependencies;
    ksn_result r=ksn_schema_dependencies_build(schema,&dependencies);
    if(r!=KSN_OK)return r;
    *session=(ksn_schema_session){.schema=schema,.dependencies=dependencies};
    return KSN_OK;
}
ksn_result ksn_schema_session_step(ksn_schema_session *session,ksn_view *view,
                                   ksn_rect viewport,const ksn_schema_value *values,
                                   uint64_t revision,bool *blocked){
    if(blocked)*blocked=false;
    if(!session||!session->schema||!view||!revision||
       viewport.x0>=viewport.x1||viewport.y0>=viewport.y1)return KSN_INVALID;
    if(session->ticket.value){
        ksn_submission outcome=ksn_view_poll(view);
        if(outcome.ticket.value!=session->ticket.value)return KSN_STALE;
        if(outcome.status==KSN_SUBMITTED){if(blocked)*blocked=true;return KSN_OK;}
        if(outcome.status==KSN_PRESENTED){
            if(session->pending_delta==KSN_SCHEMA_REPLACED){
                memcpy(session->active_refs,session->candidate_refs,
                       session->candidate_count*sizeof(ksn_ref));
                ksn_p0_probe_copy(KSN_P0_SCHEMA_REF_COMMIT,
                                  session->candidate_count*sizeof(ksn_ref));
                session->active_count=session->candidate_count;
                session->has_active=true;
            }
            session->active_viewport=session->candidate_viewport;
            session->active_background=session->candidate_background;
            session->applied_revision=session->pending_revision;
        }else if(outcome.status!=KSN_DISCARDED)return KSN_STALE;
        session->ticket=(ksn_tx){0};
    }
    if(session->has_active&&session->applied_revision==revision&&
       same_rect(session->active_viewport,viewport))return KSN_OK;
    ksn_schema_delta delta;ksn_tx tx={0};uint8_t count=0;
    ksn_result r=ksn_schema_update(view,viewport,session->schema,values,
        session->active_refs,session->has_active?session->active_count:UINT8_MAX,
        session->active_background,session->candidate_refs,&count,&tx,&delta);
    if(r!=KSN_OK)return r;
    if(delta==KSN_SCHEMA_NO_CHANGE){
        session->applied_revision=revision;session->active_viewport=viewport;
        return KSN_OK;
    }
    session->ticket=tx;session->pending_revision=revision;
    session->pending_delta=delta;session->candidate_count=count;
    session->candidate_background=background(session->schema,values);
    session->candidate_viewport=viewport;
    if(blocked)*blocked=true;
    return KSN_OK;
}
