#include "core_fixture.h"
#include "ksn_procedural_present.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ksn_proc_surface lower,upper;
static ksn_proc_layers layers;
static ksn_proc_frame candidate;
static uint16_t panel[240*135],strip_pixels[240*8];
static int fail_y=-1;
static int fail_load_y=-1;
static unsigned loads,transfers;
static bool load(void *ctx,uint16_t *pixels,int y,int rows){
    (void)ctx;loads++;
    if(y==fail_load_y)return false;
    for(int i=0;i<240*rows;i++)pixels[i]=0x001f;
    return true;
}
static uint16_t *strip(void *ctx){(void)ctx;return strip_pixels;}
static ksn_result present(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;transfers++;
    memcpy(panel+y*240,pixels,(size_t)rows*240*sizeof *pixels);
    return y==fail_y?KSN_IO:KSN_OK;
}
static void dot(int x,int y,uint16_t color){
    memset(&candidate,0,sizeof candidate);
    candidate.ready=true;candidate.count=1;candidate.raster_steps=1;
    candidate.segments[0]=(ksn_proc_segment){x,y,x,y,color};
}
static uint16_t at(int x,int y){return panel[y*240+x];}
int main(void){
    KSN_TEST_CORE(core,static);ksn_core_init(&core);
    ksn_proc_surface_init(&lower);ksn_proc_surface_init(&upper);
    ksn_proc_layers_init(&layers,load,0);
    assert(ksn_proc_layers_add(&layers,&lower));
    assert(ksn_proc_layers_add(&layers,&upper));
    ksn_display_port display={.strip=strip,.present=present,
        .width=240,.height=135,.strip_rows=8};
    ksn_render_stats stats;
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_client system=ksn_core_client(&core,KSN_SYSTEM);
    ksn_tx tx;ksn_ref ref,system_ref;
    ksn_draw rect={.kind=KSN_RECT,.bounds={50,8,54,16},
        .clip={0,0,240,135},.opacity=255,.data.shape.color=0xffff00ff};
    assert(system.ops->begin(system.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(system.ops->add(system.ctx,tx,&rect,&system_ref)==KSN_OK);
    assert(system.ops->end(system.ctx,tx)==KSN_OK);
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_OK);
    assert(loads==17&&at(0,0)==0x001f&&at(50,10)==0xffe0);
    rect.bounds=(ksn_rect){2,2,5,5};rect.data.shape.color=0xf80000ff;
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&rect,&ref)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_OK);
    assert(at(2,2)==0xf800&&at(50,10)==0xffe0);

    dot(20,10,0xf800);
    uint32_t ticket=ksn_proc_layers_stage(&layers,0,&candidate);
    assert(ticket);
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_OK);
    assert(stats.bands==(1u<<1)&&at(20,10)==0xf800);
    dot(20,10,0x07e0);
    ticket=ksn_proc_layers_stage(&layers,1,&candidate);
    assert(ticket);
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_OK);
    assert(at(20,10)==0x07e0);

    dot(40,10,0xf800);
    ticket=ksn_proc_layers_stage(&layers,0,&candidate);
    assert(ticket);
    fail_y=8;
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_IO);
    assert(ksn_proc_layers_needs_repair(&layers));
    assert(ksn_core_needs_repair(&core));
    assert(at(40,10)==0xf800); /* Failed transfer physically reached panel. */
    assert(at(20,10)==0x07e0); /* Higher committed surface still masks lower. */
    fail_y=-1;
    unsigned before=transfers;
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_OK);
    assert(transfers-before==17&&!ksn_proc_layers_needs_repair(&layers));
    assert(at(40,10)==0x001f&&at(20,10)==0x07e0);
    assert(at(50,10)==0xffe0&&at(2,2)==0xf800);

    dot(30,10,0x07e0);
    ticket=ksn_proc_layers_stage(&layers,1,&candidate);
    assert(ticket);
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_OK);
    assert(at(20,10)==0xf800&&at(30,10)==0x07e0);
    dot(50,10,0xf800);
    ticket=ksn_proc_layers_stage(&layers,0,&candidate);
    assert(ticket);
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_OK);
    assert(at(20,10)==0x001f&&at(50,10)==0xffe0);

    /* A core submission goes first over the committed procedural stack. */
    dot(60,10,0xf800);
    ticket=ksn_proc_layers_stage(&layers,0,&candidate);
    assert(ticket);
    assert(system.ops->begin(system.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change shift={.property=KSN_SET_RECT,.value.rect={70,8,74,16}};
    assert(system.ops->change(system.ctx,tx,system_ref,&shift)==KSN_OK);
    assert(system.ops->end(system.ctx,tx)==KSN_OK);
    before=transfers;
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_OK);
    assert(layers.pending&&at(60,10)==0x001f);
    assert(transfers>before&&at(50,10)==0xf800&&at(70,10)==0xffe0);
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_OK);
    assert(at(60,10)==0xf800);

    /* A failure before the first transfer keeps the candidate retryable. */
    dot(80,10,0xf800);
    ticket=ksn_proc_layers_stage(&layers,0,&candidate);
    assert(ticket);
    fail_load_y=8;before=transfers;
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_IO);
    assert(transfers==before&&layers.pending);
    assert(!ksn_proc_layers_needs_repair(&layers));
    fail_load_y=-1;
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_OK);
    assert(at(60,10)==0x001f&&at(80,10)==0xf800);

    /* Invisible-to-invisible updates can publish without touching the panel. */
    dot(300,10,0xf800);
    ticket=ksn_proc_layers_stage(&layers,0,&candidate);
    assert(ticket);
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_OK);
    dot(301,10,0xf800);
    ticket=ksn_proc_layers_stage(&layers,0,&candidate);
    assert(ticket);
    before=transfers;
    assert(ksn_proc_layers_present(&layers,&core,&display,&stats)==KSN_OK);
    assert(transfers==before&&!layers.pending);
    printf("PROCEDURAL_PRESENT_OK: APP surfaces + Kasane commands, damage, transfer repair, retry, queued candidate\n");
}
