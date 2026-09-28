#include "core_fixture.h"
#include "ksn_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint16_t panel[240*135],strip[240*8];
    unsigned sends,fail_send,under_reads;
    bool transparent;
} scene;
static uint16_t reference[240*135];
static uint16_t patch_reference[240*135];
static uint16_t *strip(void *ctx){return ((scene *)ctx)->strip;}
static ksn_result present(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    scene *s=ctx;
    if(s->fail_send&&++s->sends==s->fail_send)return KSN_IO;
    memcpy(s->panel+y*240,pixels,(size_t)rows*240*sizeof(uint16_t));return KSN_OK;
}
static ksn_result present_rect(void *ctx,uint16_t x,uint16_t y,uint16_t cols,
                               uint16_t rows,const uint16_t *pixels){
    scene *s=ctx;
    if(s->fail_send&&++s->sends==s->fail_send)return KSN_IO;
    for(unsigned r=0;r<rows;r++)
        memcpy(s->panel+(y+r)*240+x,pixels+(size_t)r*240+x,(size_t)cols*sizeof(uint16_t));
    return KSN_OK;
}
typedef struct { scene *s; bool under; } source_ctx;
typedef struct { unsigned patch_reads,invalidation_reads; } read_counts;
static ksn_result source(void *ctx,uint16_t variant,uint16_t frame,uint16_t y,uint16_t x,
                         uint16_t count,uint16_t *rgb,uint8_t *alpha){
    source_ctx *p=ctx;
    assert(!variant&&!frame&&y<135&&x+count<=240);
    if(p->under)p->s->under_reads++;
    for(unsigned j=0;j<count;j++){
        rgb[j]=(uint16_t)(p->under?0x07e0u:(0x8000u|((x+j+y)&0x7ffu)));
        alpha[j]=(uint8_t)(p->under?127:(p->s->transparent?127:255));
    }
    return KSN_OK;
}
typedef struct { uint16_t color; unsigned reads; } flat_source;
static ksn_result flat_span(void *ctx,uint16_t variant,uint16_t frame,uint16_t y,
                            uint16_t x,uint16_t count,uint16_t *rgb,uint8_t *alpha){
    flat_source *source=ctx;
    assert(!variant&&!frame&&y<135&&x+count<=240);
    source->reads++;
    for(unsigned i=0;i<count;i++){rgb[i]=source->color;alpha[i]=255;}
    return KSN_OK;
}
static void hidden_image_after_add(void){
    KSN_TEST_CORE(core,static);
    scene s={0};flat_source full={0x07e0,0},monitor={0xf800,0};
    ksn_core_init(&core);
    ksn_image_port full_port={&full,240,135,1,1,flat_span,true};
    ksn_image_port monitor_port={&monitor,112,63,1,1,flat_span,true};
    ksn_resource full_id,monitor_id;
    assert(ksn_core_register_image(&core,KSN_APP,&full_port,&full_id)==KSN_OK);
    assert(ksn_core_register_image(&core,KSN_APP,&monitor_port,&monitor_id)==KSN_OK);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_tx tx;ksn_ref full_ref,monitor_ref;
    ksn_draw full_draw={.kind=KSN_IMAGE,.bounds={0,0,240,135},.clip={0,0,240,135},
        .opacity=255,.data.image={.resource=full_id,.scale=KSN_IMAGE_STRETCH,
            .source_width=240,.source_height=135}};
    ksn_draw monitor_draw={.kind=KSN_IMAGE,.bounds={64,20,176,83},
        .clip={0,0,240,135},.opacity=255,
        .data.image={.resource=monitor_id,.scale=KSN_IMAGE_STRETCH,
            .source_width=112,.source_height=63}};
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&full_draw,&full_ref)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&monitor_draw,&monitor_ref)==KSN_OK);
    ksn_change hide={.property=KSN_SET_VISIBLE,.value.visible=false};
    assert(app.ops->change(app.ctx,tx,monitor_ref,&hide)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_display_port display={&s,strip,present,240,135,8,NULL,present_rect};
    ksn_render_stats stats;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(s.panel[40*240+100]==full.color&&monitor.reads==0);
    assert(!ksn_core_invalidate_image(&core,monitor_id));
    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change show={.property=KSN_SET_VISIBLE,.value.visible=true};
    assert(app.ops->change(app.ctx,tx,monitor_ref,&show)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(s.panel[40*240+100]==monitor.color&&monitor.reads>0);
}
static read_counts run(bool optimized,bool transparent,bool clipped,bool grouped,bool repair){
    KSN_TEST_CORE(core,static);
    scene s={0};s.transparent=transparent;
    source_ctx under={&s,true},cover={&s,false};
    ksn_core_init(&core);
    ksn_image_port under_port={&under,240,135,1,1,source,false};
    ksn_image_port cover_port={&cover,240,135,1,1,source,!transparent};
    ksn_resource under_id,cover_id;
    assert(ksn_core_register_image(&core,KSN_APP,&under_port,&under_id)==KSN_OK);
    assert(ksn_core_register_image(&core,KSN_APP,&cover_port,&cover_id)==KSN_OK);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_client system=ksn_core_client(&core,KSN_SYSTEM);
    ksn_tx tx;ksn_ref first,ref;
    ksn_rect image_bounds=clipped?(ksn_rect){32,8,208,128}:(ksn_rect){0,0,240,135};
    ksn_draw lower={.kind=KSN_IMAGE,.bounds={0,0,240,135},.clip={0,0,240,135},
                    .opacity=255,.data.image={.resource=under_id,.scale=KSN_IMAGE_STRETCH,
                                              .source_width=240,.source_height=135}};
    ksn_draw upper={.kind=KSN_IMAGE,.bounds=image_bounds,.clip=image_bounds,
                    .opacity=255,.data.image={.resource=cover_id,.scale=KSN_IMAGE_STRETCH,
                                              .source_width=240,.source_height=135}};
    ksn_draw foreground={.kind=KSN_RECT,.bounds={40,24,200,48},.clip={0,0,240,135},
                         .opacity=180,.data.shape={.color=0xf040a0df}};
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x112233ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&lower,&first)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&upper,&ref)==KSN_OK);
    if(grouped)assert(ksn_core_group(&core,KSN_APP,tx,first,2,255)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&foreground,&ref)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_display_port display={&s,strip,present,240,135,8,NULL,present_rect};
    ksn_render_stats stats;
    g_ksn_opaque_image_occlusion=optimized;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    ksn_draw notice={.kind=KSN_RECT,.bounds={80,32,160,40},.clip={0,0,240,135},
                     .opacity=170,.data.shape={.color=0xff00ffff}};
    assert(system.ops->begin(system.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(system.ops->add(system.ctx,tx,&notice,&ref)==KSN_OK);
    assert(system.ops->end(system.ctx,tx)==KSN_OK);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    /* The lower image's old and new boxes both contribute damage. The upper
     * image remains the last APP write, including over those old pixels. */
    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change move={.property=KSN_SET_RECT,.value.rect={8,8,232,127}};
    assert(app.ops->change(app.ctx,tx,first,&move)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    s.under_reads=0;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    unsigned patch_reads=s.under_reads;
    if(!optimized)memcpy(patch_reference,s.panel,sizeof(patch_reference));
    else assert(memcmp(patch_reference,s.panel,sizeof(patch_reference))==0);
    s.under_reads=0;
    assert(ksn_core_invalidate_image(&core,cover_id));
    if(repair){
        s.sends=0;s.fail_send=3;
        assert(ksn_render_rects(&core,&display,&stats)==KSN_IO);
        assert(ksn_core_needs_repair(&core));
        s.fail_send=0;s.under_reads=0;
    }
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    if(!optimized)memcpy(reference,s.panel,sizeof(reference));
    else assert(memcmp(reference,s.panel,sizeof(reference))==0);
    return (read_counts){patch_reads,s.under_reads};
}
int main(void){
    hidden_image_after_add();
    for(unsigned transparent=0;transparent<2;transparent++)
      for(unsigned clipped=0;clipped<2;clipped++)
        for(unsigned grouped=0;grouped<2;grouped++)
          for(unsigned repair=0;repair<2;repair++){
            read_counts baseline=run(false,transparent,clipped,grouped,repair);
            read_counts optimized=run(true,transparent,clipped,grouped,repair);
            assert(baseline.patch_reads>0&&baseline.invalidation_reads>0);
            if(!transparent&&!grouped&&clipped)
                assert(optimized.patch_reads<baseline.patch_reads);
            if(transparent||grouped||(clipped&&repair))assert(optimized.invalidation_reads>0);
            else assert(optimized.invalidation_reads==0);
          }
    puts("opaque image occlusion: PASS");
    return 0;
}
