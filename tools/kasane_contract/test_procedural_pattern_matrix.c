/* Structural pattern matrix: damage must cover every changed pixel, and
 * replay order must not depend on the order of 8-row strip requests. */
#include "ksn_shiki_layers.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
/* The frames are built here from segments: the float VM whose programs
 * first drew these shapes went in Shiki v2.0, and the surfaces and layers
 * only ever saw the finished frame. raster_steps is recounted by the
 * surface's validation, so seg() keeps it exact. */
typedef void (*shape_fn)(float input0);
static ksn_proc_frame scratch;
static void seg(int x0,int y0,int x1,int y1,uint16_t color){
    assert(scratch.count<KSN_PROC_SEGMENTS);
    scratch.segments[scratch.count++]=(ksn_proc_segment){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1,color};
    int dx=abs(x1-x0),dy=abs(y1-y0);
    scratch.raster_steps=(uint16_t)(scratch.raster_steps+(dx>dy?dx:dy)+1);
}
static void wave(float baseline){
    int px=0,py=0;
    for(int n=0;n<96;n++){
        int x=10+2*n,y=(int)lroundf(baseline+22.f*sinf(.13f*(float)x));
        if(n)seg(px,py,x,y,0xf800);
        px=x;py=y;
    }
}
static void grid(float unused){
    (void)unused;
    for(int row=0;row<6;row++)for(int col=0;col<10;col++){
        int x=25+19*col,y=20+18*row;
        seg(x,y,x,y,0x07ff);
    }
}
static void cross(float unused){
    (void)unused;
    seg(-20,126,260,126,0xffff);
    seg(120,-30,120,165,0xf81f);
}
static void red(float unused){(void)unused;seg(80,80,80,80,0xf800);}
static void green(float unused){(void)unused;seg(80,80,80,80,0x07e0);}
static void empty(float unused){(void)unused;}
static void outside(float unused){(void)unused;seg(300,20,300,20,0xffff);}
static ksn_proc_surface bottom,top;
static ksn_proc_layers layers;
static uint16_t before[KSN_PROC_W*KSN_PROC_H],after[KSN_PROC_W*KSN_PROC_H];
static uint16_t strips[KSN_PROC_W*KSN_PROC_H];
static unsigned backdrops;
static bool backdrop(void *ctx,uint16_t *pixels,int y,int rows){
    (void)ctx;backdrops++;
    for(int row=0;row<rows;row++)for(int x=0;x<KSN_PROC_W;x++)
        pixels[row*KSN_PROC_W+x]=(uint16_t)(0x1000|(((x+y+row)&31)<<5));
    return true;
}
static void prepare(shape_fn shape,float input0){
    memset(&scratch,0,sizeof scratch);
    shape(input0);
    scratch.ready=true;
}
static void check_damage(const ksn_proc_damage *damage){
    for(int y=0;y<KSN_PROC_H;y++)for(int x=0;x<KSN_PROC_W;x++){
        if(before[y*KSN_PROC_W+x]==after[y*KSN_PROC_W+x])continue;
        unsigned band=(unsigned)y/8;
        assert(damage->bands&(1u<<band));
        assert(x>=damage->x0[band]&&x<damage->x1[band]);
    }
}
static void transition(unsigned index,shape_fn p,float input0,
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
    transition(0,wave,60,false);
    transition(1,grid,0,false);
    transition(0,wave,78,false);
    transition(1,cross,0,false);
    transition(0,wave,40,true); /* Partial transfer, restore old wave. */
    transition(0,wave,40,false);
    transition(1,red,0,false);
    transition(1,green,0,false); /* Same bounds, different colour. */
    assert(after[80*KSN_PROC_W+80]==0x07e0);
    transition(1,empty,0,false); /* Erase upper surface. */
    transition(0,outside,0,false); /* Clip a complete old frame. */
    transition(0,empty,0,false); /* No on-screen damage. */
    assert(ksn_proc_layers_render_committed(&layers,after,0,KSN_PROC_H));
    printf("PROCEDURAL_PATTERN_MATRIX_OK: wave, grid, clipped crossings, colour update, erase, offscreen, rollback; damage and reverse bands\n");
}
