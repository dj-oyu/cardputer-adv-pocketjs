/* Host-only APP-local multi-surface composition test. */
#include "ksn_procedural_layers.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define I(OP,D,A,B,V,C) {OP,D,A,B,V,C}
static const ksn_proc_inst dot_code[]={
    I(KSN_PROC_INPUT,0,0,0,0,0),I(KSN_PROC_INPUT,1,1,0,0,0),
    I(KSN_PROC_INPUT,2,2,0,0,0),I(KSN_PROC_PLOT_COLOR_REG,2,0,1,0,0)
};
static const ksn_proc_program dot={dot_code,4};
static ksn_proc_layers layers;
static ksn_proc_surface lower,upper;
static ksn_proc_frame scratch;
static ksn_proc_vm vm;
static uint16_t committed[KSN_PROC_W*KSN_PROC_H];
static uint16_t candidate[KSN_PROC_W*KSN_PROC_H];
static uint16_t strips[KSN_PROC_W*KSN_PROC_H];
static unsigned backdrop_calls;
static uint16_t bg(int x,int y){return (uint16_t)((((x+y)&31)<<5)|((y/3)&31));}
static bool backdrop(void *ctx,uint16_t *pixels,int y,int height){
    (void)ctx;backdrop_calls++;
    for(int row=0;row<height;row++)for(int x=0;x<KSN_PROC_W;x++)
        pixels[row*KSN_PROC_W+x]=bg(x,y+row);
    return true;
}
static void prepare(float x,float y,float color){
    const float input[KSN_PROC_INPUTS]={x,y,color,0};
    assert(ksn_proc_begin(&vm,&dot,input,&scratch)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
}
static void stage_finish(unsigned index){
    uint32_t ticket=ksn_proc_layers_stage(&layers,index,&scratch);
    assert(ticket&&ksn_proc_layers_finish(&layers,ticket,true));
}
int main(void){
    ksn_proc_surface_init(&lower);ksn_proc_surface_init(&upper);
    ksn_proc_layers_init(&layers,backdrop,0);
    assert(ksn_proc_layers_add(&layers,&lower));
    assert(ksn_proc_layers_add(&layers,&upper));
    assert(!ksn_proc_layers_add(&layers,&upper));
    prepare(10,10,0xf800);stage_finish(0);
    prepare(10,10,0x07e0);stage_finish(1);
    backdrop_calls=0;
    assert(ksn_proc_layers_render_committed(&layers,committed,0,KSN_PROC_H));
    assert(backdrop_calls==1&&committed[10*KSN_PROC_W+10]==0x07e0);

    prepare(20,10,0xf800);
    uint32_t move_lower=ksn_proc_layers_stage(&layers,0,&scratch);
    assert(move_lower);
    const ksn_proc_damage *damage=ksn_proc_layers_pending_damage(&layers);
    assert(damage&&damage->bands==(1u<<1));
    assert(damage->x0[1]==10&&damage->x1[1]==21);
    assert(ksn_proc_layers_stage(&layers,1,&scratch)==0);
    backdrop_calls=0;
    assert(ksn_proc_layers_render_candidate(&layers,candidate,0,KSN_PROC_H));
    assert(backdrop_calls==1);
    assert(candidate[10*KSN_PROC_W+10]==0x07e0); // upper survives lower update
    assert(candidate[10*KSN_PROC_W+20]==0xf800);
    backdrop_calls=0;
    for(int band=KSN_PROC_BANDS-1;band>=0;band--){
        int y=band*8,h=KSN_PROC_H-y<8?KSN_PROC_H-y:8;
        assert(ksn_proc_layers_render_candidate(&layers,strips+y*KSN_PROC_W,y,h));
    }
    assert(backdrop_calls==KSN_PROC_BANDS);
    assert(!memcmp(candidate,strips,sizeof candidate));
    memcpy(strips,committed,sizeof strips);
    assert(ksn_proc_layers_render_candidate(&layers,strips+8*KSN_PROC_W,8,8));
    assert(!ksn_proc_layers_finish(&layers,move_lower+1,true));
    assert(ksn_proc_layers_finish(&layers,move_lower,false));
    assert(ksn_proc_layers_needs_repair(&layers));
    assert(ksn_proc_layers_stage(&layers,1,&scratch)==0);
    assert(ksn_proc_layers_render_committed(&layers,strips,0,KSN_PROC_H));
    assert(!memcmp(strips,committed,sizeof strips));
    ksn_proc_layers_repair_done(&layers);
    assert(!ksn_proc_layers_needs_repair(&layers));

    prepare(30,10,0x07e0);stage_finish(1);
    assert(ksn_proc_layers_render_committed(&layers,committed,0,KSN_PROC_H));
    assert(committed[10*KSN_PROC_W+10]==0xf800);
    assert(committed[10*KSN_PROC_W+30]==0x07e0);
    prepare(20,10,0xf800);stage_finish(0);
    assert(ksn_proc_layers_render_committed(&layers,committed,0,KSN_PROC_H));
    assert(committed[10*KSN_PROC_W+10]==bg(10,10));
    assert(committed[10*KSN_PROC_W+20]==0xf800);
    assert(committed[10*KSN_PROC_W+30]==0x07e0);
    printf("PROCEDURAL_LAYERS_OK: 2 ordered APP surfaces, one backdrop load per band, reverse-strip replay, lower update under upper, failure repair; frames %zu B\n",
           sizeof lower.slots+sizeof upper.slots);
}
