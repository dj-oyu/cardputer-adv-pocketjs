#include "ui/kasane/ksn_video_frames.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { W=16,H=8,N=W*H };
static uint16_t backing[KSN_VIDEO_SLOTS][N];

static ksn_video_handle produce(ksn_video_frames *pool,uint16_t color,
                                 uint64_t pts)
{
    ksn_video_handle handle={0};
    uint16_t *pixels=NULL;
    assert(ksn_video_begin(pool,&handle,&pixels)==KSN_VIDEO_OK);
    assert(pixels);
    for (unsigned i=0;i<N;++i) pixels[i]=color;
    assert(ksn_video_publish(pool,handle,pts)==KSN_VIDEO_OK);
    return handle;
}

static void check_span(const ksn_image_port *port,ksn_video_handle handle,
                       uint16_t color)
{
    uint16_t pixels[6]; uint8_t alpha[6];
    assert(port->read_span(port->ctx,0,handle.slot,4,3,6,pixels,alpha)==KSN_OK);
    for (unsigned i=0;i<6;++i) assert(pixels[i]==color && alpha[i]==255);
}

int main(void)
{
    uint16_t *buffers[KSN_VIDEO_SLOTS]={backing[0],backing[1],backing[2]};
    ksn_video_frames pool;
    assert(!ksn_video_frames_init(&pool,W,H,
        (uint16_t *[KSN_VIDEO_SLOTS]){backing[0],backing[0]+1,backing[2]},N));
    assert(ksn_video_frames_init(&pool,W,H,buffers,N));
    ksn_image_port port;
    ksn_video_image_port(&pool,&port);
    assert(port.width==W && port.height==H && port.frames==3 && port.opaque);
    ksn_video_handle aborted={0};uint16_t *aborted_pixels=NULL;
    assert(ksn_video_begin(&pool,&aborted,&aborted_pixels)==KSN_VIDEO_OK);
    assert(ksn_video_abort_write(&pool,aborted));
    assert(!ksn_video_abort_write(&pool,aborted));
    assert(ksn_video_publish(&pool,aborted,1)==KSN_VIDEO_INVALID);
    ksn_video_handle a=produce(&pool,0x1234,100), chosen={0};
    assert(a.generation!=aborted.generation);
    assert(ksn_video_select(&pool,99,&chosen)==KSN_VIDEO_EMPTY);
    assert(ksn_video_select(&pool,100,&chosen)==KSN_VIDEO_OK);
    assert(chosen.generation==a.generation && chosen.slot==a.slot);
    check_span(&port,a,0x1234);
    assert(!ksn_video_finish(&pool,(ksn_video_handle){0},true));
    assert(ksn_video_finish(&pool,a,true));
    assert(!ksn_video_release(&pool,a)); /* committed internal pin */
    assert(ksn_video_pin(&pool,a));
    assert(ksn_video_release(&pool,a)); /* external pin only */

    ksn_video_handle b=produce(&pool,0x2222,120);
    ksn_video_handle c=produce(&pool,0x3333,130);
    assert(ksn_video_select(&pool,130,&chosen)==KSN_VIDEO_OK);
    assert(chosen.generation==c.generation); /* latest wins */
    check_span(&port,a,0x1234);check_span(&port,c,0x3333);
    assert(ksn_video_select(&pool,130,&chosen)==KSN_VIDEO_BUSY);
    assert(ksn_video_timeline_reset(&pool)==KSN_VIDEO_BUSY);
    ksn_video_handle writing={0}; uint16_t *write_pixels=NULL;
    assert(ksn_video_begin(&pool,&writing,&write_pixels)==KSN_VIDEO_OK);
    assert(writing.slot==b.slot);
    assert(!ksn_video_pin(&pool,b)); /* stale generation cannot alias reuse */
    assert(ksn_video_begin(&pool,&chosen,&write_pixels)==KSN_VIDEO_BUSY);
    assert(ksn_video_publish(&pool,writing,140)==KSN_VIDEO_OK);

    assert(ksn_video_finish(&pool,c,false));
    assert(ksn_video_needs_repair(&pool));
    assert(ksn_video_select(&pool,140,&chosen)==KSN_VIDEO_BUSY);
    check_span(&port,a,0x1234); /* old frame survives failed send */
    ksn_video_repair_done(&pool);
    assert(!ksn_video_needs_repair(&pool));
    assert(ksn_video_select(&pool,139,&chosen)==KSN_VIDEO_OK);
    assert(chosen.generation==c.generation);
    assert(ksn_video_pin(&pool,c));
    assert(ksn_video_release(&pool,c));
    assert(ksn_video_finish(&pool,c,true));
    assert(ksn_video_committed(&pool).generation==c.generation);
    assert(port.read_span(port.ctx,0,a.slot,0,0,1,
                          (uint16_t[1]){0},(uint8_t[1]){0})==KSN_INVALID);
    assert(ksn_video_select(&pool,140,&chosen)==KSN_VIDEO_OK);
    assert(chosen.generation==writing.generation);
    assert(ksn_video_finish(&pool,writing,true));
    assert(ksn_video_select(&pool,140,&chosen)==KSN_VIDEO_EMPTY);
    (void)produce(&pool,0x4444,125); /* arrived late, never play backward */
    assert(ksn_video_select(&pool,200,&chosen)==KSN_VIDEO_EMPTY);
    ksn_video_handle future=produce(&pool,0x5555,220);
    assert(ksn_video_select(&pool,219,&chosen)==KSN_VIDEO_EMPTY);
    assert(ksn_video_select(&pool,220,&chosen)==KSN_VIDEO_OK);
    assert(chosen.generation==future.generation);
    assert(ksn_video_finish(&pool,future,true));
    /* Arrival order can differ from presentation time. An early-published
     * future frame must survive a later-published nearer frame. */
    ksn_video_handle far=produce(&pool,0x6666,300);
    ksn_video_handle near=produce(&pool,0x7777,260);
    assert(ksn_video_select(&pool,260,&chosen)==KSN_VIDEO_OK);
    assert(chosen.generation==near.generation);
    assert(ksn_video_finish(&pool,near,true));
    assert(ksn_video_select(&pool,300,&chosen)==KSN_VIDEO_OK);
    assert(chosen.generation==far.generation);
    assert(ksn_video_finish(&pool,far,true));
    assert(ksn_video_timeline_reset(&pool)==KSN_VIDEO_OK);
    ksn_video_handle restart=produce(&pool,0x8888,20);
    assert(ksn_video_select(&pool,20,&chosen)==KSN_VIDEO_OK);
    assert(chosen.generation==restart.generation);
    check_span(&port,far,0x6666); /* old displayed frame remains pinned */
    assert(ksn_video_finish(&pool,restart,true));
    check_span(&port,restart,0x8888);
    puts("video frame pool latest/pin/repair passed");
    return 0;
}
