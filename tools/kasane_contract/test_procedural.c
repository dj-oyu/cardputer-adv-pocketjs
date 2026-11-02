// Host: compile ksn_procedural.c and this file with -std=c11 -O2 -Wall
// -Wextra -Werror -Imain/ui/kasane -lm.
//
// The band renderer's own contract: a frame replayed band by band (in any
// order) equals the whole-panel replay, later segments cover earlier ones, and
// a frame that is not ready draws nothing. The float VM that used to write
// these frames went in Shiki v2.0, so the frames are built here directly from
// segments -- the renderer never knew who wrote them.
#include "ksn_procedural.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static ksn_proc_frame frozen,second,over;
static uint16_t whole[KSN_PROC_W*KSN_PROC_H],joined[KSN_PROC_W*KSN_PROC_H],changed[KSN_PROC_W*KSN_PROC_H];

static void push(ksn_proc_frame *f,int x0,int y0,int x1,int y1,uint16_t color){
    assert(f->count<KSN_PROC_SEGMENTS);
    f->segments[f->count++]=(ksn_proc_segment){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1,color};
}
/* The sine graph the VM test drew: 100 samples joined by 99 lines; the
 * baseline moves the whole graph, so two baselines give two frames. */
static void graph(ksn_proc_frame *f,float baseline){
    memset(f,0,sizeof *f);
    int px=0,py=0;
    for(int n=0;n<100;n++){
        int x=10+n,y=(int)lroundf(baseline+12.f*sinf(.1f*(float)x));
        if(n)push(f,px,py,x,y,0xffff);
        px=x;py=y;
    }
    f->ready=true;
}

int main(void){
    graph(&frozen,60);
    assert(frozen.count==99);
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)whole[i]=joined[i]=changed[i]=0x001f;
    assert(ksn_proc_render_band(&frozen,whole,0,KSN_PROC_H));
    for(int band=(KSN_PROC_H-1)/8;band>=0;band--){
        int y=band*8,h=KSN_PROC_H-y<8?KSN_PROC_H-y:8;
        assert(ksn_proc_render_band(&frozen,joined+y*KSN_PROC_W,y,h));
    }
    assert(!memcmp(whole,joined,sizeof whole));
    graph(&second,100);
    assert(ksn_proc_render_band(&second,changed,0,KSN_PROC_H));
    assert(memcmp(whole,changed,sizeof whole));
    assert(ksn_proc_render_band(&frozen,joined,0,KSN_PROC_H));
    assert(!memcmp(whole,joined,sizeof whole));

    // Later segments cover earlier ones.
    memset(&over,0,sizeof over);
    push(&over,20,30,20,30,0xf800);
    push(&over,20,30,20,30,0x07e0);
    over.ready=true;
    assert(ksn_proc_render_band(&over,joined,0,KSN_PROC_H));
    assert(joined[30*KSN_PROC_W+20]==0x07e0);

    // A frame that is not ready (Shiki rejected it, or it hit a limit) and a
    // band outside the panel draw nothing.
    over.ready=false;
    memcpy(changed,joined,sizeof joined);
    assert(!ksn_proc_render_band(&over,joined,0,KSN_PROC_H));
    assert(!memcmp(changed,joined,sizeof joined));
    over.ready=true;
    assert(!ksn_proc_render_band(&over,joined,-1,8));
    assert(!ksn_proc_render_band(&over,joined,KSN_PROC_H-4,8));
    assert(!ksn_proc_render_band(NULL,joined,0,8));

    // Lines that leave the panel sideways are clipped, not wrapped.
    memset(&over,0,sizeof over);
    for(int i=0;i<6;i++)push(&over,-480,i,720,i,0xffff);
    over.ready=true;
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)joined[i]=0;
    assert(ksn_proc_render_band(&over,joined,0,KSN_PROC_H));
    for(int x=0;x<KSN_PROC_W;x++)assert(joined[5*KSN_PROC_W+x]==0xffff&&joined[6*KSN_PROC_W+x]==0);
    printf("PROCEDURAL_OK: %u segments; frame capacity %zu B; reverse-band replay, ordered overdraw, not-ready/out-of-panel rejection, horizontal clip\n",
           frozen.count,sizeof frozen);
}
