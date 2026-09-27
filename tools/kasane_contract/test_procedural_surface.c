/* Host-only surface ownership and repair test. Compile with
 * ksn_procedural.c, ksn_procedural_surface.c, and -lm. */
#include "ksn_procedural_surface.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define I(OP,D,A,B,V,C) {OP,D,A,B,V,C}
static const ksn_proc_inst dot_code[]={
    I(KSN_PROC_INPUT,0,0,0,0,0),I(KSN_PROC_SET,1,0,0,10,0),
    I(KSN_PROC_PLOT,0,0,1,0,0xf800)
};
static const ksn_proc_program dot={dot_code,3};
static ksn_proc_surface surface,empty;
static ksn_proc_frame candidate;
static uint16_t old_panel[KSN_PROC_W*KSN_PROC_H];
static uint16_t new_panel[KSN_PROC_W*KSN_PROC_H];
static uint16_t panel[KSN_PROC_W*KSN_PROC_H];
static ksn_proc_vm vm;
static uint16_t bg(int x,int y){return (uint16_t)(((x+y)&31)|(((y/2)&63)<<5));}
static bool backdrop(void *ctx,uint16_t *pixels,int y,int h){
    (void)ctx;
    for(int row=0;row<h;row++)for(int x=0;x<KSN_PROC_W;x++)
        pixels[row*KSN_PROC_W+x]=bg(x,y+row);
    return true;
}
static void prepare(float x){
    const float input[KSN_PROC_INPUTS]={x,0,0,0};
    assert(ksn_proc_begin(&vm,&dot,input,&candidate)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
}
int main(void){
    ksn_proc_surface_init(&surface);
    assert(ksn_proc_surface_render_committed(&surface,backdrop,0,panel,0,KSN_PROC_H));
    assert(panel[10*KSN_PROC_W+10]==bg(10,10));
    prepare(10);
    uint32_t first=ksn_proc_surface_stage(&surface,&candidate);
    assert(first&&ksn_proc_surface_pending_damage(&surface));
    const ksn_proc_damage *damage=ksn_proc_surface_pending_damage(&surface);
    assert(damage->bands==(1u<<1)&&damage->x0[1]==10&&damage->x1[1]==11);
    candidate.segments[0].color=0x07e0; // stage copied the source.
    assert(ksn_proc_surface_stage(&surface,&candidate)==0); // in-flight BUSY
    assert(ksn_proc_surface_render_pending(&surface,backdrop,0,panel,0,KSN_PROC_H));
    assert(panel[10*KSN_PROC_W+10]==0xf800);
    assert(!ksn_proc_surface_finish(&surface,first+1,true));
    assert(ksn_proc_surface_finish(&surface,first,true));
    assert(ksn_proc_surface_render_committed(&surface,backdrop,0,old_panel,0,KSN_PROC_H));

    prepare(20);
    uint32_t second=ksn_proc_surface_stage(&surface,&candidate);
    assert(second&&second!=first);
    damage=ksn_proc_surface_pending_damage(&surface);
    assert(damage->bands==(1u<<1)&&damage->x0[1]==10&&damage->x1[1]==21);
    assert(ksn_proc_surface_render_pending(&surface,backdrop,0,new_panel,0,KSN_PROC_H));
    assert(new_panel[10*KSN_PROC_W+10]==bg(10,10));
    assert(new_panel[10*KSN_PROC_W+20]==0xf800);
    memcpy(panel,old_panel,sizeof panel);
    assert(ksn_proc_surface_render_pending(&surface,backdrop,0,
           panel+8*KSN_PROC_W,8,8)); // partial transfer of candidate
    assert(ksn_proc_surface_finish(&surface,second,false));
    assert(ksn_proc_surface_needs_repair(&surface));
    assert(ksn_proc_surface_stage(&surface,&candidate)==0); // repair gate
    assert(ksn_proc_surface_render_committed(&surface,backdrop,0,panel,0,KSN_PROC_H));
    assert(!memcmp(panel,old_panel,sizeof panel));
    ksn_proc_surface_repair_done(&surface);
    assert(!ksn_proc_surface_needs_repair(&surface));
    uint32_t third=ksn_proc_surface_stage(&surface,&candidate);
    assert(third&&third!=second);
    assert(!ksn_proc_surface_finish(&surface,second,true)); // stale ticket
    assert(ksn_proc_surface_finish(&surface,third,true));
    assert(ksn_proc_surface_render_committed(&surface,backdrop,0,panel,0,KSN_PROC_H));
    assert(!memcmp(panel,new_panel,sizeof panel));

    const float bad_input[KSN_PROC_INPUTS]={NAN,0,0,0};
    assert(ksn_proc_begin(&vm,&dot,bad_input,&candidate)==KSN_PROC_INVALID);
    assert(ksn_proc_surface_stage(&surface,&candidate)==0);
    candidate.ready=true;candidate.count=1;candidate.raster_steps=1;
    candidate.segments[0]=(ksn_proc_segment){-32000,10,32000,10,0xf800};
    assert(ksn_proc_surface_stage(&surface,&candidate)==0); // reject forged work
    assert(ksn_proc_surface_render_committed(&surface,backdrop,0,panel,0,KSN_PROC_H));
    assert(!memcmp(panel,new_panel,sizeof panel));

    ksn_proc_surface_init(&empty);
    prepare(10);
    uint32_t empty_ticket=ksn_proc_surface_stage(&empty,&candidate);
    assert(empty_ticket&&ksn_proc_surface_finish(&empty,empty_ticket,false));
    assert(ksn_proc_surface_needs_repair(&empty));
    assert(ksn_proc_surface_render_committed(&empty,backdrop,0,panel,0,KSN_PROC_H));
    assert(panel[10*KSN_PROC_W+10]==bg(10,10));
    printf("PROCEDURAL_SURFACE_OK: owned candidate, stale ticket, old/new damage, failed partial transfer and committed-frame repair; two slots %zu B, total %zu B\n",
           sizeof surface.slots,sizeof surface);
}
