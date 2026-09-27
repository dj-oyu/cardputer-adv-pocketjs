#include "ksn_procedural_layers.h"
#include <string.h>

void ksn_proc_layers_init(ksn_proc_layers *layers,ksn_proc_backdrop_load backdrop,void *ctx){
    if(!layers)return;
    memset(layers,0,sizeof *layers);
    layers->backdrop=backdrop;layers->backdrop_ctx=ctx;
}
bool ksn_proc_layers_add(ksn_proc_layers *layers,ksn_proc_surface *surface){
    if(!layers||!surface||!layers->backdrop||layers->count==KSN_PROC_LAYER_MAX||
       layers->generation||layers->pending||layers->repair_required||
       surface->pending||ksn_proc_surface_needs_repair(surface))return false;
    for(unsigned i=0;i<layers->count;i++)if(layers->surface[i]==surface)return false;
    layers->surface[layers->count++]=surface;
    return true;
}
uint32_t ksn_proc_layers_stage(ksn_proc_layers *layers,unsigned index,const ksn_proc_frame *candidate){
    if(!layers||index>=layers->count||layers->pending||layers->repair_required)return 0;
    for(unsigned i=0;i<layers->count;i++)
        if(layers->surface[i]->pending||ksn_proc_surface_needs_repair(layers->surface[i]))return 0;
    uint32_t ticket=ksn_proc_surface_stage(layers->surface[index],candidate);
    if(!ticket)return 0;
    layers->surface_ticket=ticket;
    layers->pending_index=(uint8_t)index;
    layers->pending=true;
    if(++layers->generation==0)++layers->generation;
    return layers->generation;
}
const ksn_proc_damage *ksn_proc_layers_pending_damage(const ksn_proc_layers *layers){
    return layers&&layers->pending?
        ksn_proc_surface_pending_damage(layers->surface[layers->pending_index]):0;
}
static bool render(const ksn_proc_layers *layers,bool candidate,uint16_t *pixels,int y,int height){
    if(!layers||!layers->backdrop||!pixels||y<0||height<0||
       y>KSN_PROC_H||height>KSN_PROC_H-y||
       (candidate&&!layers->pending))return false;
    if(!layers->backdrop(layers->backdrop_ctx,pixels,y,height))return false;
    for(unsigned i=0;i<layers->count;i++){
        const ksn_proc_frame *frame=candidate&&i==layers->pending_index?
            ksn_proc_surface_pending_frame(layers->surface[i]):
            ksn_proc_surface_committed_frame(layers->surface[i]);
        if(frame&&!ksn_proc_render_band(frame,pixels,y,height))return false;
    }
    return true;
}
bool ksn_proc_layers_render_candidate(const ksn_proc_layers *layers,uint16_t *pixels,int y,int height){
    return render(layers,true,pixels,y,height);
}
bool ksn_proc_layers_render_committed(const ksn_proc_layers *layers,uint16_t *pixels,int y,int height){
    return render(layers,false,pixels,y,height);
}
bool ksn_proc_layers_finish(ksn_proc_layers *layers,uint32_t generation,bool transferred){
    if(!layers||!layers->pending||!generation||generation!=layers->generation)return false;
    if(!ksn_proc_surface_finish(layers->surface[layers->pending_index],
                                layers->surface_ticket,transferred))return false;
    layers->pending=false;
    if(!transferred){
        layers->repair_required=true;
        layers->repair_index=layers->pending_index;
    }
    return true;
}
bool ksn_proc_layers_needs_repair(const ksn_proc_layers *layers){
    return layers&&layers->repair_required;
}
void ksn_proc_layers_repair_done(ksn_proc_layers *layers){
    if(!layers||!layers->repair_required||layers->pending)return;
    ksn_proc_surface_repair_done(layers->surface[layers->repair_index]);
    layers->repair_required=false;
}
