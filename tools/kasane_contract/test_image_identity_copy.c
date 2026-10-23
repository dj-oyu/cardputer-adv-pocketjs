/* Opaque identity-copy boundaries. The source formula is an independent oracle;
 * provider request hashes also let a base/candidate runner compare call order. */
#include "core_fixture.h"
#include "ksn_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t panel[240*135],strip[240*8];
static unsigned fail_next,calls,cases;
static uint64_t requests=1469598103934665603ull,panels=1469598103934665603ull;
static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *p){
    (void)ctx;memcpy(panel+y*240,p,rows*240*sizeof(*p));return KSN_OK;
}
static ksn_result send_rect(void *ctx,uint16_t x,uint16_t y,uint16_t cols,
                            uint16_t rows,const uint16_t *p){
    (void)ctx;
    for(unsigned r=0;r<rows;r++)memcpy(panel+(y+r)*240+x,p+r*240+x,cols*sizeof(*p));
    return KSN_OK;
}
static uint16_t color(unsigned x,unsigned y,unsigned frame){return (uint16_t)(x*313+y*937+frame*3001);}
static ksn_result source(void *ctx,uint16_t variant,uint16_t frame,uint16_t y,
                         uint16_t x,uint16_t count,uint16_t *rgb,uint8_t *alpha){
    (void)ctx;assert(!variant&&frame<2&&y<160&&x+count<=256&&count>=1&&count<=32);
    unsigned fields[]={variant,frame,y,x,count};
    for(unsigned i=0;i<5;i++){requests^=fields[i];requests*=1099511628211ull;}
    calls++;
    if(fail_next){fail_next=0;return KSN_IO;}
    for(unsigned j=0;j<count;j++){rgb[j]=color(x+j,y,frame);alpha[j]=255;}
    return KSN_OK;
}
static void check(const ksn_draw *d,unsigned frame){
    for(int y=0;y<135;y++)for(int x=0;x<240;x++){
        bool inside=x>=d->bounds.x0&&x<d->bounds.x1&&y>=d->bounds.y0&&y<d->bounds.y1&&
            x>=d->clip.x0&&x<d->clip.x1&&y>=d->clip.y0&&y<d->clip.y1;
        uint16_t expected=inside?color((unsigned)(x-d->bounds.x0)+3,
                                      (unsigned)(y-d->bounds.y0)+7,frame):0;
        assert(panel[y*240+x]==expected);
        panels^=panel[y*240+x];panels*=1099511628211ull;
    }
}
static void run(unsigned width,bool edge,bool clipped){
    KSN_TEST_CORE(core,static);ksn_core_init(&core);
    ksn_image_port port={NULL,256,160,1,2,source,true};ksn_resource image;
    assert(ksn_core_register_image(&core,KSN_APP,&port,&image)==KSN_OK);
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx tx;ksn_ref ref;
    int x=edge?240-(int)width:1,y=edge?126:0;
    ksn_draw d={.kind=KSN_IMAGE,.bounds={x,y,x+(int)width,y+9},.clip={0,0,240,135},
        .opacity=255,.data.image={.resource=image,.scale=KSN_IMAGE_STRETCH,
            .source_x=3,.source_y=7,.source_width=width,.source_height=9}};
    if(clipped){d.clip=d.bounds;d.clip.y0+=2;d.clip.y1-=1;if(width>2){d.clip.x0++;d.clip.x1--;}}
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&d,&ref)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_display_port display={NULL,buffer,send,240,135,8,NULL,send_rect};ksn_render_stats stats;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);check(&d,0);
    /* Successful frame PATCH keeps damage narrow. Then fail an invalidated
     * image's first fetch and retry through the existing repair path. */
    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change c={.property=KSN_SET_IMAGE_FRAME,.value.image={0,1}};
    assert(app.ops->change(app.ctx,tx,ref,&c)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);check(&d,1);
    assert(ksn_core_invalidate_image(&core,image));
    fail_next=1;assert(ksn_render_rects(&core,&display,&stats)==KSN_IO);
    assert(ksn_core_needs_repair(&core));
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);check(&d,1);cases++;
}
int main(void){
    for(unsigned w=1;w<=65;w++)for(unsigned edge=0;edge<2;edge++)
        for(unsigned clip=0;clip<2;clip++)run(w,edge,clip);
    printf("opaque identity copy: PASS cases=%u calls=%u requests=%016llx panels=%016llx\n",
           cases,calls,(unsigned long long)requests,(unsigned long long)panels);
    return 0;
}
