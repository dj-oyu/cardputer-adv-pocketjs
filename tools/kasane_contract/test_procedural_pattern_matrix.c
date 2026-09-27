/* Structural pattern matrix: damage must cover every changed pixel, and
 * replay order must not depend on the order of 8-row strip requests. */
#include "ksn_procedural_layers.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define I(OP,D,A,B,V,C) {OP,D,A,B,V,C}
static const ksn_proc_inst wave_code[]={
    I(KSN_PROC_SET,0,0,0,10,0),I(KSN_PROC_SET,1,0,0,2,0),
    I(KSN_PROC_INPUT,2,0,0,0,0),I(KSN_PROC_SET,3,0,0,22,0),
    I(KSN_PROC_SET,4,0,0,0.13f,0),I(KSN_PROC_REPEAT,0,96,0,0,0),
    I(KSN_PROC_MUL,5,0,4,0,0),I(KSN_PROC_SIN,6,5,0,0,0),
    I(KSN_PROC_MUL,6,6,3,0,0),I(KSN_PROC_ADD,7,6,2,0,0),
    I(KSN_PROC_LINE,0,0,7,0,0xf800),I(KSN_PROC_ADD,0,0,1,0,0),
    I(KSN_PROC_END,0,0,0,0,0)
};
static const ksn_proc_inst grid_code[]={
    I(KSN_PROC_SET,1,0,0,20,0),I(KSN_PROC_SET,2,0,0,19,0),
    I(KSN_PROC_SET,3,0,0,18,0),I(KSN_PROC_REPEAT,0,6,0,0,0),
    I(KSN_PROC_SET,0,0,0,25,0),I(KSN_PROC_REPEAT,0,10,0,0,0),
    I(KSN_PROC_PLOT,0,0,1,0,0x07ff),I(KSN_PROC_ADD,0,0,2,0,0),
    I(KSN_PROC_END,0,0,0,0,0),I(KSN_PROC_ADD,1,1,3,0,0),
    I(KSN_PROC_END,0,0,0,0,0)
};
static const ksn_proc_inst cross_code[]={
    I(KSN_PROC_SET,0,0,0,-20,0),I(KSN_PROC_SET,1,0,0,126,0),
    I(KSN_PROC_MOVE,0,0,1,0,0),I(KSN_PROC_SET,0,0,0,260,0),
    I(KSN_PROC_LINE,0,0,1,0,0xffff),
    I(KSN_PROC_SET,0,0,0,120,0),I(KSN_PROC_SET,1,0,0,-30,0),
    I(KSN_PROC_MOVE,0,0,1,0,0),I(KSN_PROC_SET,1,0,0,165,0),
    I(KSN_PROC_LINE,0,0,1,0,0xf81f)
};
static const ksn_proc_inst red_code[]={
    I(KSN_PROC_SET,0,0,0,80,0),I(KSN_PROC_SET,1,0,0,80,0),
    I(KSN_PROC_PLOT,0,0,1,0,0xf800)
};
static const ksn_proc_inst green_code[]={
    I(KSN_PROC_SET,0,0,0,80,0),I(KSN_PROC_SET,1,0,0,80,0),
    I(KSN_PROC_PLOT,0,0,1,0,0x07e0)
};
static const ksn_proc_inst empty_code[]={I(KSN_PROC_SET,0,0,0,1,0)};
static const ksn_proc_inst outside_code[]={
    I(KSN_PROC_SET,0,0,0,300,0),I(KSN_PROC_SET,1,0,0,20,0),
    I(KSN_PROC_PLOT,0,0,1,0,0xffff)
};
#define PROGRAM(code) {code,(uint8_t)(sizeof code/sizeof code[0])}
static const ksn_proc_program wave=PROGRAM(wave_code),grid=PROGRAM(grid_code),
    cross=PROGRAM(cross_code),red=PROGRAM(red_code),green=PROGRAM(green_code),
    empty=PROGRAM(empty_code),outside=PROGRAM(outside_code);
static ksn_proc_surface bottom,top;
static ksn_proc_layers layers;
static ksn_proc_frame scratch;
static ksn_proc_vm vm;
static uint16_t before[KSN_PROC_W*KSN_PROC_H],after[KSN_PROC_W*KSN_PROC_H];
static uint16_t strips[KSN_PROC_W*KSN_PROC_H];
static unsigned backdrops;
static bool backdrop(void *ctx,uint16_t *pixels,int y,int rows){
    (void)ctx;backdrops++;
    for(int row=0;row<rows;row++)for(int x=0;x<KSN_PROC_W;x++)
        pixels[row*KSN_PROC_W+x]=(uint16_t)(0x1000|(((x+y+row)&31)<<5));
    return true;
}
static void prepare(const ksn_proc_program *p,float input0){
    const float input[KSN_PROC_INPUTS]={input0,0,0,0};
    assert(ksn_proc_begin(&vm,p,input,&scratch)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
}
static void check_damage(const ksn_proc_damage *damage){
    for(int y=0;y<KSN_PROC_H;y++)for(int x=0;x<KSN_PROC_W;x++){
        if(before[y*KSN_PROC_W+x]==after[y*KSN_PROC_W+x])continue;
        unsigned band=(unsigned)y/8;
        assert(damage->bands&(1u<<band));
        assert(x>=damage->x0[band]&&x<damage->x1[band]);
    }
}
static void transition(unsigned index,const ksn_proc_program *p,float input0,
                       bool fail_transfer){
    prepare(p,input0);
    assert(ksn_proc_layers_render_committed(&layers,before,0,KSN_PROC_H));
    uint32_t ticket=ksn_proc_layers_stage(&layers,index,&scratch);
    assert(ticket);
    const ksn_proc_damage *damage=ksn_proc_layers_pending_damage(&layers);
    assert(damage);
    backdrops=0;
    assert(ksn_proc_layers_render_candidate(&layers,after,0,KSN_PROC_H));
    assert(backdrops==1);
    check_damage(damage);
    backdrops=0;
    for(int band=KSN_PROC_BANDS-1;band>=0;band--){
        int y=band*8,rows=KSN_PROC_H-y<8?KSN_PROC_H-y:8;
        assert(ksn_proc_layers_render_candidate(&layers,strips+y*KSN_PROC_W,y,rows));
    }
    assert(backdrops==KSN_PROC_BANDS);
    assert(!memcmp(after,strips,sizeof after));
    if(fail_transfer){
        assert(ksn_proc_layers_finish(&layers,ticket,false));
        assert(ksn_proc_layers_needs_repair(&layers));
        assert(ksn_proc_layers_render_committed(&layers,strips,0,KSN_PROC_H));
        assert(!memcmp(before,strips,sizeof before));
        ksn_proc_layers_repair_done(&layers);
    }else{
        assert(ksn_proc_layers_finish(&layers,ticket,true));
        assert(ksn_proc_layers_render_committed(&layers,strips,0,KSN_PROC_H));
        assert(!memcmp(after,strips,sizeof after));
    }
}
int main(void){
    ksn_proc_surface_init(&bottom);ksn_proc_surface_init(&top);
    ksn_proc_layers_init(&layers,backdrop,0);
    assert(ksn_proc_layers_add(&layers,&bottom));
    assert(ksn_proc_layers_add(&layers,&top));
    transition(0,&wave,60,false);
    transition(1,&grid,0,false);
    transition(0,&wave,78,false);
    transition(1,&cross,0,false);
    transition(0,&wave,40,true); /* Partial transfer, restore old wave. */
    transition(0,&wave,40,false);
    transition(1,&red,0,false);
    transition(1,&green,0,false); /* Same bounds, different colour. */
    assert(after[80*KSN_PROC_W+80]==0x07e0);
    transition(1,&empty,0,false); /* Erase upper surface. */
    transition(0,&outside,0,false); /* Clip a complete old frame. */
    transition(0,&empty,0,false); /* No on-screen damage. */
    assert(ksn_proc_layers_render_committed(&layers,after,0,KSN_PROC_H));
    printf("PROCEDURAL_PATTERN_MATRIX_OK: wave, grid, clipped crossings, colour update, erase, offscreen, rollback; damage and reverse bands\n");
}
