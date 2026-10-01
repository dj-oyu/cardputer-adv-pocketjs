/* Experimental arm requires stable provider pixels within each command. */
#define main stretch_suite_main
#include "test_image_stretch_arms.c"
#undef main
extern bool g_ksn_image_stable_rows;
static bool opaque_pixels=true,mutable_pixels=false;
static unsigned fail_fetch;
static ksn_result stable_source(void *ctx,uint16_t variant,uint16_t frame,
        uint16_t y,uint16_t x,uint16_t n,uint16_t *rgb,uint8_t *alpha){
    ksn_result r=source(ctx,variant,frame,y,x,n,rgb,alpha);
    if(fail_fetch&&fetches==fail_fetch)return KSN_IO;
    for(unsigned j=0;j<n;j++){
        if(opaque_pixels)alpha[j]=255;
        if(mutable_pixels)rgb[j]+=(uint16_t)fetches;
    }
    return r;
}
static uint32_t run(const config *c,bool enabled,unsigned long long *reads){
    KSN_TEST_CORE(core,);ksn_core_init(&core);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_image_port port={NULL,240,160,1,2,stable_source,opaque_pixels};
    ksn_resource image;assert(ksn_core_register_image(&core,KSN_APP,&port,&image)==KSN_OK);
    ksn_display_port display={NULL,buffer,send,240,135,8,NULL,NULL};
    ksn_tx tx;ksn_ref ref;ksn_render_stats stats;
    ksn_draw d={.kind=KSN_IMAGE,.bounds={c->x,c->y,c->x+c->w,c->y+c->h},
        .clip=c->clip,.opacity=c->opacity,.data.image={.resource=image,
        .scale=KSN_IMAGE_STRETCH,.source_x=c->source_x,.source_y=c->source_y,
        .source_width=c->source_w,.source_height=c->source_h}};
    fetches=0;g_ksn_image_stable_rows=enabled;
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x183c60ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&d,&ref)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_result result=ksn_render_rects(&core,&display,&stats);
    if(fail_fetch){
        assert(result==KSN_IO);fail_fetch=0;
        assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    }else assert(result==KSN_OK);
    uint32_t hash=hash_bytes(panel,PANEL);
    for(unsigned step=0;step<c->steps;step++){
        assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
        ksn_change change={.property=KSN_SET_IMAGE_FRAME,.value.image={0,(uint8_t)(step%2)}};
        assert(app.ops->change(app.ctx,tx,ref,&change)==KSN_OK);
        assert(app.ops->end(app.ctx,tx)==KSN_OK);
        assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        hash^=hash_bytes(panel,PANEL);hash*=16777619u;
    }
    *reads=fetches;return hash;
}
int main(void){
    unsigned tested=0;unsigned long long a,b;
    config c={.w=240,.h=135,.source_w=48,.source_h=28,.opacity=255,
              .clip={0,0,240,135}};
    uint32_t ha=run(&c,false,&a),hb=run(&c,true,&b);
    assert(ha==hb&&a==2025&&b==600);
    printf("stable 48x28 to 240x135: reads %llu -> %llu, hash=%08x\n",a,b,ha);
    for(unsigned opacity=0;opacity<2;opacity++)for(unsigned mode=0;mode<4;mode++)
    for(unsigned sw=1;sw<=120;sw+=17)for(unsigned sh=1;sh<=135;sh+=19){
        c.x=mode==0?-13:7;c.y=mode==1?-9:3;c.w=mode==2?53:230;c.h=mode==3?41:129;
        c.source_x=5;c.source_y=7;c.source_w=sw;c.source_h=sh;
        c.opacity=opacity?137:255;c.clip=(ksn_rect){11,5,231,130};c.steps=2;
        ha=run(&c,false,&a);hb=run(&c,true,&b);
        assert(ha==hb&&b<=a);if(opacity)assert(a==b);tested++;
    }
    c=(config){.w=240,.h=135,.source_w=48,.source_h=28,.opacity=255,.clip={0,0,240,135}};
    opaque_pixels=false;ha=run(&c,false,&a);hb=run(&c,true,&b);assert(ha==hb&&a==b);
    opaque_pixels=true;hb=run(&c,true,&b);
    const unsigned fail_points[]={1,17,599};
    for(unsigned i=0;i<sizeof(fail_points)/sizeof(fail_points[0]);i++){
        fail_fetch=fail_points[i];ha=run(&c,true,&a);assert(ha==hb);
    }
    /* Coverage alone is insufficient: a mutable callback deliberately differs.
     * This negative control prevents adopting the arm under opaque alone. */
    mutable_pixels=true;ha=run(&c,false,&a);hb=run(&c,true,&b);assert(ha!=hb);
    printf("row reuse PASS: %u crop/clip/scale/opacity/PATCH cases; alpha exclusion, IO repair, mutable-provider negative control\n",tested);
    return stretch_suite_main();
}
