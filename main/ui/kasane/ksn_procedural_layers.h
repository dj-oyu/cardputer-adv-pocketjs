#ifndef KSN_PROCEDURAL_LAYERS_H
#define KSN_PROCEDURAL_LAYERS_H
#include "ksn_procedural_surface.h"

#define KSN_PROC_LAYER_MAX 3
/* APP-local compositor: ordered, opaque RGB565 procedural surfaces over one
 * frozen native backdrop. It owns no frames; attached surfaces must outlive it
 * and may only be staged through this coordinator while attached. */
typedef struct {
    ksn_proc_surface *surface[KSN_PROC_LAYER_MAX];
    ksn_proc_backdrop_load backdrop;
    void *backdrop_ctx;
    uint32_t generation,surface_ticket;
    uint8_t count,pending_index,repair_index;
    bool pending,repair_required;
} ksn_proc_layers;

void ksn_proc_layers_init(ksn_proc_layers *layers,ksn_proc_backdrop_load backdrop,void *ctx);
bool ksn_proc_layers_add(ksn_proc_layers *layers,ksn_proc_surface *surface);
uint32_t ksn_proc_layers_stage(ksn_proc_layers *layers,unsigned index,const ksn_proc_frame *candidate);
const ksn_proc_damage *ksn_proc_layers_pending_damage(const ksn_proc_layers *layers);
bool ksn_proc_layers_render_candidate(const ksn_proc_layers *layers,uint16_t *pixels,int y,int height);
bool ksn_proc_layers_render_committed(const ksn_proc_layers *layers,uint16_t *pixels,int y,int height);
bool ksn_proc_layers_finish(ksn_proc_layers *layers,uint32_t generation,bool transferred);
bool ksn_proc_layers_needs_repair(const ksn_proc_layers *layers);
void ksn_proc_layers_repair_done(ksn_proc_layers *layers);
#endif
