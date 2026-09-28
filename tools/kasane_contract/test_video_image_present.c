#include "core_fixture.h"
#include "ksn_render.h"
#include "ui/kasane/ksn_video_frames.h"
#include "ui/kasane/ksn_depth_lines.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t buffers[KSN_VIDEO_SLOTS][16*16];
static uint16_t panel[240*135],strip[240*8];
static unsigned sends,fail_on_send;
static uint16_t *display_buffer(void *ctx) { (void)ctx; return strip; }
static ksn_result display_send(void *ctx,uint16_t y,uint16_t rows,
                                const uint16_t *pixels)
{
    (void)ctx;
    if (++sends==fail_on_send) return KSN_IO;
    memcpy(panel+(size_t)y*240,pixels,(size_t)rows*240*sizeof *pixels);
    return KSN_OK;
}

static ksn_video_handle produce(ksn_video_frames *frames,uint16_t color,
                                 uint64_t pts)
{
    ksn_video_handle handle;uint16_t *pixels;
    assert(ksn_video_begin(frames,&handle,&pixels)==KSN_VIDEO_OK);
    for(unsigned i=0;i<16*16;i++)pixels[i]=color;
    assert(ksn_video_publish(frames,handle,pts)==KSN_VIDEO_OK);
    assert(ksn_video_select(frames,pts,&handle)==KSN_VIDEO_OK);
    return handle;
}

int main(void)
{
    uint16_t *storage[KSN_VIDEO_SLOTS]={buffers[0],buffers[1],buffers[2]};
    ksn_video_frames frames;
    assert(ksn_video_frames_init(&frames,16,16,storage,16*16));
    ksn_image_port image_port;ksn_video_image_port(&frames,&image_port);
    ksn_depth_frame depth_frame;ksn_depth_frame_reset(&depth_frame);
    assert(ksn_depth_add(&depth_frame,(ksn_depth_line){0,8,15,8,100,100,0xf800}));
    ksn_depth_image depth_image;
    assert(ksn_depth_image_init(&depth_image,&depth_frame,16,16));
    ksn_image_port depth_port;ksn_depth_image_port(&depth_image,&depth_port);
    KSN_TEST_CORE(core,);ksn_core_init(&core);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_client system=ksn_core_client(&core,KSN_SYSTEM);
    ksn_resource image,depth;
    assert(ksn_core_register_image(&core,KSN_APP,&image_port,&image)==KSN_OK);
    assert(ksn_core_register_image(&core,KSN_APP,&depth_port,&depth)==KSN_OK);
    ksn_display_port display={NULL,display_buffer,display_send,240,135,8,
                              NULL,NULL};
    ksn_render_stats stats;ksn_tx tx;ksn_ref ref;
    ksn_video_handle a=produce(&frames,0x1234,100);
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={10,3,26,19},
                   .clip={0,0,240,135},.opacity=255,
                   .data.image={.resource=image,.frame=a.slot,
                                .scale=KSN_IMAGE_1X}};
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&draw,&ref)==KSN_OK);
    ksn_draw depth_draw=draw;
    depth_draw.data.image.resource=depth;
    ksn_ref depth_ref;
    assert(app.ops->add(app.ctx,tx,&depth_draw,&depth_ref)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    sends=0;fail_on_send=0;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(ksn_video_finish(&frames,a,true));
    assert(panel[3*240+10]==0x1234);
    assert(panel[11*240+10]==0xf800);
    assert(panel[12*240+10]==0x1234);

    /* A translucent SYSTEM notice overlays both native surfaces. Replacing
     * it later must restore video pixels in its old bounds. */
    ksn_draw notice={.kind=KSN_RECT,.bounds={12,4,20,12},
        .clip={0,0,240,135},.opacity=128,.data.shape={0x00ff00ff,0,0}};
    ksn_ref notice_ref;
    assert(system.ops->begin(system.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(system.ops->add(system.ctx,tx,&notice,&notice_ref)==KSN_OK);
    assert(system.ops->end(system.ctx,tx)==KSN_OK);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(panel[4*240+12]!=0x1234);

    ksn_video_handle b=produce(&frames,0x5678,200);
    ksn_change change={.property=KSN_SET_IMAGE_FRAME,
                       .value.image={0,b.slot}};
    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    assert(app.ops->change(app.ctx,tx,ref,&change)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    sends=0;fail_on_send=2;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_IO);
    assert(ksn_video_committed(&frames).generation==a.generation);
    /* Kasane retains the submission after I/O failure: keep B pinned and
     * retry the same sealed pixels, then acknowledge both owners together. */
    sends=0;fail_on_send=0;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(ksn_video_finish(&frames,b,true));
    assert(panel[3*240+10]==0x5678);
    assert(panel[11*240+10]==0xf800);
    assert(panel[12*240+10]==0x5678);

    notice.bounds=(ksn_rect){30,4,38,12};
    assert(system.ops->begin(system.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(system.ops->add(system.ctx,tx,&notice,&notice_ref)==KSN_OK);
    assert(system.ops->end(system.ctx,tx)==KSN_OK);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(panel[4*240+12]==0x5678);

    ksn_video_handle c=produce(&frames,0xabcd,300);
    change.value.image.frame=c.slot;
    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    assert(app.ops->change(app.ctx,tx,ref,&change)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    sends=0;fail_on_send=2;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_IO);
    assert(ksn_core_discard(&core,tx)==KSN_OK);
    assert(ksn_video_finish(&frames,c,false));
    assert(ksn_video_needs_repair(&frames));
    sends=0;fail_on_send=0;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    ksn_video_repair_done(&frames);
    assert(panel[3*240+10]==0x5678);
    assert(!ksn_video_needs_repair(&frames));
    assert(system.ops->begin(system.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(system.ops->end(system.ctx,tx)==KSN_OK);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(panel[4*240+30]==0);
    puts("video/depth/translucent UI present retry/discard/move repair passed");
    return 0;
}
