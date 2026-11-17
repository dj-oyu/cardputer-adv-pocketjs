#include "pocket_stream_candidate.h"
#ifndef KASANE_DMA_OVERLAP
#error "Streaming display candidate requires Kasane completion support"
#endif
/* Separate declaration avoids bringing the QuickJS interface into this adapter. */
ksn_result pocket_kasane_present_stream_backdrop(const ksn_display_port*,
    const ksn_display_completion*,ksn_render_stats*,ksn_backdrop_loader);
typedef struct {
    pocket_stream_candidate *state;
    const ksn_display_port *port;
    const ksn_display_completion *completion;
    bool drained;
} display_call;
static bool outside_frame(const ayatsuri_stream_frame_owner*f,const void*p,size_t n){
    const void*base[]={f,f->stream,f->stream->core.arena,f->stream->core.model,
        f->stream->core.model->data,f->snapshots[0].packets.data,f->snapshots[1].packets.data};
    size_t size[]={sizeof *f,sizeof *f->stream,f->stream->core.arena_bytes,sizeof *f->stream->core.model,
        f->stream->core.model->bytes,f->snapshots[0].packets.capacity,f->snapshots[1].packets.capacity};
    for(unsigned j=0;j<7;j++)if(!ay_stream_owner_disjoint(p,n,base[j],size[j]))return false;
    return true;
}
static uint16_t*strip(void*ctx){display_call*c=ctx;return c->port->strip(c->port->ctx);}
static ksn_result send(void*ctx,uint16_t y,uint16_t rows,const uint16_t*p){
    display_call*c=ctx;c->drained=false;return c->port->present(c->port->ctx,y,rows,p);
}
static ksn_result send_rect(void*ctx,uint16_t x,uint16_t y,uint16_t cols,uint16_t rows,const uint16_t*p){
    display_call*c=ctx;c->drained=false;return c->port->present_rect(c->port->ctx,x,y,cols,rows,p);
}
static ksn_result finish(void*ctx){
    display_call*c=ctx;ksn_result r=c->completion->finish(c->completion->ctx);
    c->drained=r==KSN_OK;return r;
}
static ksn_result backdrop(void*ctx,uint16_t y,uint16_t rows,uint16_t*p){
    display_call*c=ctx;pocket_stream_candidate*s=c->state;
    const ksn_mesh_snapshot*frame=ayatsuri_stream_frame_borrow(s->frame,s->token);
    if(!frame||!p||y>135||rows>135-y)return KSN_INVALID;
    size_t bytes=(size_t)rows*240*sizeof *p;
    if(!outside_frame(s->frame,p,bytes)||!ay_stream_owner_disjoint(p,bytes,s,sizeof *s)||
       !ay_stream_owner_disjoint(p,bytes,s->depth,240*sizeof *s->depth))return KSN_INVALID;
    /* Explicit diagnostic background. Real material/shader composition is absent. */
    for(unsigned j=0;j<(unsigned)rows*240;j++)p[j]=0;
    return ksn_mesh_render(frame,p,y,rows,s->depth,1)?KSN_OK:KSN_INVALID;
}
static bool depth_valid(const pocket_stream_candidate*s,const ayatsuri_stream_frame_owner*f,float*d){
    return d&&outside_frame(f,d,240*sizeof *d)&&outside_frame(f,s,sizeof *s)&&
        ay_stream_owner_disjoint(d,240*sizeof *d,s,sizeof *s);
}
ksn_result pocket_stream_candidate_present(pocket_stream_candidate*s,
    ayatsuri_stream_frame_owner*f,const void*id,uint32_t gen,float depth[240],
    ayatsuri_stream_frame_preflight preflight,void*context,const ksn_display_port*port,
    const ksn_display_completion*completion,ksn_render_stats*stats){
    if(!s||s->busy||s->held)return KSN_BUSY;
    if(!ay_stream_owner_valid(f,id,gen)||!f->stream||!f->stream->core.alive||
       !f->stream->core.model||!port||!port->strip||!port->present||
       !completion||!completion->finish||!stats||!depth_valid(s,f,depth)||
       !outside_frame(f,stats,sizeof *stats)||
       !ay_stream_owner_disjoint(stats,sizeof *stats,s,sizeof *s)||
       !ay_stream_owner_disjoint(stats,sizeof *stats,depth,240*sizeof *depth))return KSN_INVALID;
    const void*inputs[]={port,completion};size_t input_bytes[]={sizeof *port,sizeof *completion};
    for(unsigned j=0;j<2;j++)if(!ay_stream_owner_disjoint(s,sizeof *s,inputs[j],input_bytes[j])||
        !ay_stream_owner_disjoint(stats,sizeof *stats,inputs[j],input_bytes[j])||
        !ay_stream_owner_disjoint(depth,240*sizeof *depth,inputs[j],input_bytes[j]))return KSN_INVALID;
    *stats=(ksn_render_stats){0};s->busy=true;
    /* Establish that no prior display borrow remains before acquiring a frame. */
    ksn_result r=ksn_runtime_finish_display(completion);
    if(r!=KSN_OK){s->busy=false;return r;}
    ayatsuri_stream_frame_token t=ayatsuri_stream_frame_acquire(f,id,gen,preflight,context);
    if(!t.owner){s->busy=false;return KSN_INVALID;}
    s->frame=f;s->token=t;s->depth=depth;s->held=true;
    display_call call={s,port,completion,true};
    ksn_display_port proxy=*port;proxy.ctx=&call;proxy.strip=strip;proxy.present=send;
    if(port->present_rect)proxy.present_rect=send_rect;
    ksn_display_completion done={&call,finish};
    r=pocket_kasane_present_stream_backdrop(&proxy,&done,stats,backdrop);
    if(call.drained){
        if(!ayatsuri_stream_frame_complete(f,t,true,r==KSN_OK))r=KSN_INVALID;
        else s->held=false;
    }
    /* Uncertain completion retains token and both snapshots. A fresh completion
     * context is required for recovery; no stack-local display pointer survives. */
    s->busy=false;return r;
}
ksn_result pocket_stream_candidate_recover(pocket_stream_candidate*s,const ksn_display_completion*c){
    if(!s||s->busy||!s->held||!c||!c->finish)return KSN_INVALID;
    s->busy=true;ksn_result r=ksn_runtime_finish_display(c);
    if(r==KSN_OK){
        if(ayatsuri_stream_frame_complete(s->frame,s->token,true,false)){
            s->held=false;ksn_runtime_invalidate();
        }else r=KSN_INVALID;
    }
    s->busy=false;return r;
}
