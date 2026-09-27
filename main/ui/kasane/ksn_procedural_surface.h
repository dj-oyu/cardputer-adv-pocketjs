#ifndef KSN_PROCEDURAL_SURFACE_H
#define KSN_PROCEDURAL_SURFACE_H
#include "ksn_procedural.h"

#define KSN_PROC_BANDS ((KSN_PROC_H+7)/8)
typedef struct {
    uint32_t bands;
    int16_t x0[KSN_PROC_BANDS],x1[KSN_PROC_BANDS]; /* x1 exclusive */
} ksn_proc_damage;
typedef bool (*ksn_proc_backdrop_load)(void *ctx,uint16_t *pixels,int y,int height);
typedef struct {
    ksn_proc_frame slots[2];
    ksn_proc_damage pending_damage;
    uint32_t generation;
    uint8_t committed_slot,pending_slot;
    bool has_committed,pending,repair_required;
} ksn_proc_surface;

void ksn_proc_surface_init(ksn_proc_surface *surface);
/* Copies the completed candidate into the free slot. Zero means BUSY or an
 * invalid candidate. The caller may reuse its source immediately after stage. */
uint32_t ksn_proc_surface_stage(ksn_proc_surface *surface,const ksn_proc_frame *candidate);
const ksn_proc_damage *ksn_proc_surface_pending_damage(const ksn_proc_surface *surface);
/* Immutable views for a compositor that loads the backdrop once and draws
 * several APP surfaces in order. A pending view expires at finish. */
const ksn_proc_frame *ksn_proc_surface_committed_frame(const ksn_proc_surface *surface);
const ksn_proc_frame *ksn_proc_surface_pending_frame(const ksn_proc_surface *surface);
/* A stale completion cannot publish or discard a newer candidate. On transfer
 * failure the committed frame remains and full-screen repair is required. */
bool ksn_proc_surface_finish(ksn_proc_surface *surface,uint32_t generation,bool transferred);
bool ksn_proc_surface_needs_repair(const ksn_proc_surface *surface);
void ksn_proc_surface_repair_done(ksn_proc_surface *surface);
/* Backdrop is always regenerated before drawing the chosen immutable frame. */
bool ksn_proc_surface_render_pending(const ksn_proc_surface *surface,
    ksn_proc_backdrop_load backdrop,void *ctx,uint16_t *pixels,int y,int height);
bool ksn_proc_surface_render_committed(const ksn_proc_surface *surface,
    ksn_proc_backdrop_load backdrop,void *ctx,uint16_t *pixels,int y,int height);
#endif
