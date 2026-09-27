#include "ksn_procedural_present.h"

typedef struct {
    const ksn_display_port *base;
    const ksn_proc_layers *layers;
    bool candidate;
    bool transfer_attempted;
} proc_port;

static uint16_t *strip(void *ctx){
    proc_port *port=(proc_port *)ctx;
    return port->base->strip(port->base->ctx);
}
static ksn_result present(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    proc_port *port=(proc_port *)ctx;
    port->transfer_attempted=true;
    return port->base->present(port->base->ctx,y,rows,pixels);
}
static ksn_result present_rect(void *ctx,uint16_t x,uint16_t y,uint16_t cols,
                               uint16_t rows,const uint16_t *pixels){
    proc_port *port=(proc_port *)ctx;
    port->transfer_attempted=true;
    return port->base->present_rect(port->base->ctx,x,y,cols,rows,pixels);
}
static ksn_result backdrop(void *ctx,uint16_t y,uint16_t rows,uint16_t *pixels){
    proc_port *port=(proc_port *)ctx;
    bool ok=port->candidate?
        ksn_proc_layers_render_candidate(port->layers,pixels,y,rows):
        ksn_proc_layers_render_committed(port->layers,pixels,y,rows);
    return ok?KSN_OK:KSN_IO;
}
ksn_result ksn_proc_layers_present(ksn_proc_layers *layers,ksn_core *core,
                                   const ksn_display_port *display,
                                   ksn_render_stats *stats){
    if(!layers||!core||!display||!stats||!layers->backdrop||
       !display->strip||!display->present||display->width!=KSN_PROC_W||
       display->height!=KSN_PROC_H||display->strip_rows!=8)return KSN_INVALID;
    *stats=(ksn_render_stats){0};
    bool candidate=layers->pending&&!ksn_core_has_submission(core);
    if(candidate){
        const ksn_proc_damage *damage=ksn_proc_layers_pending_damage(layers);
        if(!damage)return KSN_INVALID;
        if(!damage->bands){
            if(!ksn_proc_layers_finish(layers,layers->generation,true))return KSN_INVALID;
            candidate=false;
        }else ksn_core_invalidate_bands(core,damage->bands);
    }
    if(layers->repair_required)ksn_core_invalidate(core);
    if(!candidate&&!ksn_core_has_submission(core)&&
       !ksn_core_needs_repair(core))return KSN_OK;
    proc_port context={display,layers,candidate,false};
    ksn_display_port wrapped=*display;
    wrapped.ctx=&context;
    wrapped.strip=strip;
    wrapped.present=present;
    wrapped.present_rect=display->present_rect?present_rect:0;
    ksn_result result=ksn_render_rects_backdrop(core,&wrapped,backdrop,false,stats);
    if(candidate){
        if(result==KSN_OK){
            if(!ksn_proc_layers_finish(layers,layers->generation,true))return KSN_INVALID;
        }else if(context.transfer_attempted){
            /* Once any transfer was attempted, old committed pixels must be
             * replayed. Zero-transfer failures leave the candidate retryable. */
            if(!ksn_proc_layers_finish(layers,layers->generation,false))return KSN_INVALID;
        }
    }else if(result==KSN_OK&&layers->repair_required&&
             !ksn_core_needs_repair(core))ksn_proc_layers_repair_done(layers);
    return result;
}
