#include "core_fixture.h"
#include "ksn_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

KSN_TEST_CORE(core,static);
static uint16_t strip[240*8],panel[240*135],color=0x1234;
static unsigned bytes,send_count,fail_send,late_invalidate;
static bool special_pixel;
static ksn_resource resource;
static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result send_full(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;
    if(fail_send&&++send_count==fail_send)return KSN_IO;
    memcpy(panel+y*240,pixels,(size_t)rows*240*2);
    bytes+=(unsigned)rows*240*2;return KSN_OK;
}
static ksn_result send_rect(void *ctx,uint16_t x,uint16_t y,uint16_t cols,
                            uint16_t rows,const uint16_t *pixels){
    (void)ctx;
    if(fail_send&&++send_count==fail_send)return KSN_IO;
    for(unsigned row=0;row<rows;row++)
        memcpy(panel+(y+row)*240+x,pixels+(size_t)row*240+x,cols*2);
    bytes+=(unsigned)rows*cols*2;
    if(late_invalidate){late_invalidate=0;assert(ksn_core_invalidate_image(&core,resource));}
    return KSN_OK;
}
static ksn_result source(void *ctx,uint16_t variant,uint16_t frame,uint16_t y,
                         uint16_t x,uint16_t count,uint16_t *rgb,uint8_t *alpha){
    (void)ctx;assert(!variant&&!frame&&y<16&&x+count<=20);
    for(unsigned i=0;i<count;i++){
        rgb[i]=special_pixel&&y==4&&x+i==5?0xffff:color;alpha[i]=255;
    }
    return KSN_OK;
}
static void check_region(unsigned x,unsigned y,unsigned w,unsigned h,uint16_t expected){
    for(unsigned row=y;row<y+h;row++)for(unsigned col=x;col<x+w;col++)
        assert(panel[row*240+col]==expected);
}
int main(void){
    ksn_core_init(&core);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_image_port image={NULL,20,16,1,1,source,false};
    assert(ksn_core_register_image(&core,KSN_APP,&image,&resource)==KSN_OK);
    ksn_display_port display={NULL,buffer,send_full,240,135,8,NULL,send_rect};
    ksn_render_stats stats;ksn_tx tx;ksn_ref ref;
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={20,16,40,32},.clip={0,0,240,135},
                   .opacity=255,.data.image={.resource=resource,.source_width=20,
                                             .source_height=16,.scale=KSN_IMAGE_1X}};
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&draw,&ref)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    check_region(20,16,20,16,color);

    color=0x5678;bytes=0;
    assert(ksn_core_invalidate_image(&core,resource));
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(bytes==32*16*2);check_region(20,16,20,16,color);
    printf("image-only bytes=%u\n",bytes);

    color=0x1234;bytes=0;
    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change move={.property=KSN_SET_RECT,.value.rect={60,16,80,32}};
    assert(app.ops->change(app.ctx,tx,ref,&move)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_core_invalidate_image(&core,resource));
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(bytes==64*16*2);
    check_region(20,16,20,16,0);check_region(60,16,20,16,color);
    printf("move bytes=%u\n",bytes);

    /* A callback's later request must survive ACK of the current rectangle. */
    color=0x5678;bytes=0;late_invalidate=1;
    assert(ksn_core_invalidate_image(&core,resource));
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(bytes==32*16*2&&ksn_core_needs_repair(&core));
    bytes=0;assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(bytes==32*16*2&&!ksn_core_needs_repair(&core));

    /* A partial failed transfer must retry a complete panel repair. */
    color=0x1234;bytes=0;send_count=0;fail_send=1;
    assert(ksn_core_invalidate_image(&core,resource));
    assert(ksn_render_rects(&core,&display,&stats)==KSN_IO);
    assert(ksn_core_needs_repair(&core));
    fail_send=0;bytes=0;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(bytes==240*135*2);check_region(60,16,20,16,color);
    printf("IO repair bytes=%u\n",bytes);

    /* Independent owner damage shares the same per-band rectangle queue as
     * image invalidation. Two distant boxes coalesce only within their band. */
    bytes=0;
    ksn_core_invalidate_rect(&core,(ksn_rect){5,40,10,48});
    ksn_core_invalidate_rect(&core,(ksn_rect){53,40,58,48});
    ksn_core_invalidate_rect(&core,(ksn_rect){-20,150,-1,160});
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(bytes==64*8*2&&!ksn_core_needs_repair(&core));
    printf("owner rectangle union bytes=%u\n",bytes);

    /* The source-space point maps to the current 1:1 destination node. */
    special_pixel=true;bytes=0;
    assert(ksn_core_invalidate_image_source_rect(&core,resource,(ksn_rect){5,4,6,5}));
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(bytes==16*8*2&&panel[20*240+65]==0xffff);
    assert(panel[20*240+66]==color);

    /* A scaled node cannot use the 1:1 source mapping. It owes its complete
     * destination box until an exact inverse transform is implemented. */
    ksn_core_init(&core);app=ksn_core_client(&core,KSN_APP);
    special_pixel=false;color=0x1234;
    assert(ksn_core_register_image(&core,KSN_APP,&image,&resource)==KSN_OK);
    draw.bounds=(ksn_rect){20,16,60,48};
    draw.data.image.resource=resource;
    draw.data.image.scale=KSN_IMAGE_2X;
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&draw,&ref)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    color=0x5678;bytes=0;
    assert(ksn_core_invalidate_image_source_rect(&core,resource,(ksn_rect){5,4,6,5}));
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(bytes==48*32*2); /* 40 displayed columns round to 16-column transfer. */
    check_region(20,16,40,32,color);
    return 0;
}
