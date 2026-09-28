#include "ksn_depth_lines.h"
#include "ksn_render.h"
#include "core_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t strip[240*8],panel[240*135];
static uint16_t *strip_get(void *ctx){(void)ctx;return strip;}
static ksn_result present(void *ctx,uint16_t y,uint16_t rows,
                          const uint16_t *pixels){
    (void)ctx;
    memcpy(panel+(size_t)y*240,pixels,(size_t)rows*240*sizeof *pixels);
    return KSN_OK;
}

int main(void){
    ksn_depth_frame a,b;
    ksn_depth_frame_reset(&a);ksn_depth_frame_reset(&b);
    const ksn_depth_line far={0,32,63,32,300,300,0xf800};
    const ksn_depth_line near={32,0,32,63,100,100,0x07e0};
    assert(ksn_depth_add(&a,far)&&ksn_depth_add(&a,near));
    assert(ksn_depth_add(&b,near)&&ksn_depth_add(&b,far));
    ksn_depth_image image;
    assert(ksn_depth_image_init(&image,&a,64,64));
    ksn_image_port port;ksn_depth_image_port(&image,&port);
    assert(!port.opaque&&port.width==64&&port.height==64);
    uint16_t colors[64];uint8_t alpha[64];
    for(int y=63;y>=0;y--){
        assert(port.read_span(port.ctx,0,0,(uint16_t)y,0,64,colors,alpha)==KSN_OK);
        if(y==32){assert(colors[31]==0xf800&&colors[32]==0x07e0);}
        if(y==10){assert(alpha[10]==0&&alpha[32]==255);}
    }
    ksn_depth_image_bind(&image,&b);
    assert(port.read_span(port.ctx,0,0,32,31,3,colors,alpha)==KSN_OK);
    assert(colors[0]==0xf800&&colors[1]==0x07e0&&colors[2]==0xf800);

    KSN_TEST_CORE(core,static);ksn_core_init(&core);
    ksn_resource resource;
    assert(ksn_core_register_image(&core,KSN_APP,&port,&resource)==KSN_OK);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_tx tx;ksn_ref ref;
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={0,0,64,64},
        .clip={0,0,240,135},.opacity=255,
        .data.image={.resource=resource,.scale=KSN_IMAGE_1X}};
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x203040ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&draw,&ref)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_display_port display={.ctx=NULL,.strip=strip_get,.present=present,
        .width=240,.height=135,.strip_rows=8};
    ksn_render_stats stats;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(panel[32*240+32]==0x07e0&&panel[32*240+31]==0xf800);
    assert(panel[10*240+10]==panel[10*240+100]);

    ksn_depth_frame_reset(&a);
    assert(ksn_depth_add(&a,(ksn_depth_line){0,32,63,32,50,50,0x001f}));
    ksn_depth_image_bind(&image,&a);
    assert(ksn_core_invalidate_image(&core,resource));
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(panel[32*240+32]==0x001f);
    assert(panel[10*240+32]==panel[10*240+100]);
    unsigned kept=a.count;
    assert(!ksn_depth_add(&a,(ksn_depth_line){-480,0,720,0,65535,1,1}));
    assert(a.count==kept);
    for(unsigned i=0;i<KSN_DEPTH_LINES_MAX-kept;i++)
        assert(ksn_depth_add(&a,(ksn_depth_line){0,0,0,0,1,1,1}));
    assert(!ksn_depth_add(&a,(ksn_depth_line){0,0,0,0,1,1,1}));
    puts("depth lines: z occlusion, order, transparency, image damage and limits passed");
    return 0;
}
