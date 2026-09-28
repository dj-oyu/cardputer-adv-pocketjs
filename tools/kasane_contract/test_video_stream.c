#include "pocket/pocket_video_stream.h"
#include "ui/kasane/ksn_video_frames.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t data[256];
    uint32_t size, reads, bytes, short_at;
    bool fail;
} memory;
static int32_t read_memory(void *ctx,uint32_t at,uint8_t *out,uint32_t want){
    memory *m=ctx;m->reads++;
    if(m->fail||at>m->size||want>m->size-at)return -1;
    if(m->short_at&&at==m->short_at){
        memcpy(out,m->data+at,3);
        m->bytes+=3;
        return 3;
    }
    memcpy(out,m->data+at,want);m->bytes+=want;return (int32_t)want;
}
static void u16(uint8_t *p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void u32(uint8_t *p,uint32_t n){
    for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));
}
static void u64(uint8_t *p,uint64_t n){
    for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(8*i));
}
static void make(memory *m){
    memset(m,0,sizeof *m);memcpy(m->data,"KSV1",4);
    u16(m->data+4,2);u16(m->data+6,2);u32(m->data+8,8);
    for(unsigned i=0;i<3;i++){
        uint32_t at=16+i*20;
        u64(m->data+at,100+i*10);u32(m->data+at+8,8);
        for(unsigned j=0;j<8;j++)m->data[at+12+j]=(uint8_t)(i+1);
    }
    m->size=76;
}
int main(void){
    memory m;make(&m);
    pocket_video_stream s;
    assert(pocket_video_stream_open(&s,read_memory,&m,m.size));
    assert(s.width==2&&s.height==2&&s.frame_bytes==8);
    uint8_t pixels[8]={0};uint64_t pts=0;
    assert(pocket_video_stream_next(&s,99,pixels,sizeof pixels,&pts)==
           POCKET_VIDEO_STREAM_FUTURE);
    uint32_t reads=m.reads;
    assert(pocket_video_stream_next(&s,99,pixels,sizeof pixels,&pts)==
           POCKET_VIDEO_STREAM_FUTURE&&m.reads==reads);
    assert(pocket_video_stream_next(&s,125,pixels,sizeof pixels,&pts)==
           POCKET_VIDEO_STREAM_FRAME&&pts==100&&pixels[0]==1);
    assert(pocket_video_stream_next(&s,125,pixels,sizeof pixels,&pts)==
           POCKET_VIDEO_STREAM_FRAME&&pts==110&&pixels[0]==2);
    assert(pocket_video_stream_next(&s,125,pixels,sizeof pixels,&pts)==
           POCKET_VIDEO_STREAM_FRAME&&pts==120&&pixels[0]==3);
    assert(pocket_video_stream_next(&s,125,pixels,sizeof pixels,&pts)==
           POCKET_VIDEO_STREAM_END);
    assert(pocket_video_stream_next(&s,125,pixels,sizeof pixels,&pts)==
           POCKET_VIDEO_STREAM_END);
    make(&m);m.size=75;
    assert(pocket_video_stream_open(&s,read_memory,&m,m.size));
    assert(pocket_video_stream_next(&s,200,pixels,sizeof pixels,&pts)==
           POCKET_VIDEO_STREAM_FRAME);
    assert(pocket_video_stream_next(&s,200,pixels,sizeof pixels,&pts)==
           POCKET_VIDEO_STREAM_FRAME);
    assert(pocket_video_stream_next(&s,200,pixels,sizeof pixels,&pts)==
           POCKET_VIDEO_STREAM_INVALID);
    make(&m);u64(m.data+36,100);
    assert(pocket_video_stream_open(&s,read_memory,&m,m.size));
    assert(pocket_video_stream_next(&s,200,pixels,sizeof pixels,&pts)==
           POCKET_VIDEO_STREAM_FRAME);
    assert(pocket_video_stream_next(&s,200,pixels,sizeof pixels,&pts)==
           POCKET_VIDEO_STREAM_INVALID);
    make(&m);u16(m.data+4,240);u16(m.data+6,135);
    assert(!pocket_video_stream_open(&s,read_memory,&m,m.size));
    make(&m);u32(m.data+8,10);
    assert(!pocket_video_stream_open(&s,read_memory,&m,m.size));
    make(&m);m.fail=true;
    assert(!pocket_video_stream_open(&s,read_memory,&m,m.size));

    make(&m);
    for(unsigned i=0;i<10;i++){
        uint32_t at=16+i*20;
        u64(m.data+at,100+i*10);u32(m.data+at+8,8);
        memset(m.data+at+12,(int)(i+1),8);
    }
    m.size=216;
    assert(pocket_video_stream_open(&s,read_memory,&m,m.size));
    uint32_t before=m.bytes;
    assert(pocket_video_stream_latest(&s,1000,pixels,sizeof pixels,&pts,4)==
           POCKET_VIDEO_STREAM_SCANNING);
    assert(m.bytes-before==4*12); /* stale payloads were skipped */
    before=m.bytes;
    assert(pocket_video_stream_latest(&s,1000,pixels,sizeof pixels,&pts,4)==
           POCKET_VIDEO_STREAM_SCANNING);
    assert(m.bytes-before==4*12);
    before=m.bytes;
    assert(pocket_video_stream_latest(&s,1000,pixels,sizeof pixels,&pts,4)==
           POCKET_VIDEO_STREAM_FRAME&&pts==190&&pixels[0]==10);
    assert(m.bytes-before==2*12+8); /* only final payload read */
    assert(pocket_video_stream_latest(&s,1000,pixels,sizeof pixels,&pts,4)==
           POCKET_VIDEO_STREAM_END);
    make(&m);m.size=75;
    assert(pocket_video_stream_open(&s,read_memory,&m,m.size));
    assert(pocket_video_stream_latest(&s,200,pixels,sizeof pixels,&pts,8)==
           POCKET_VIDEO_STREAM_INVALID);
    make(&m);m.short_at=48; /* second RGB565 payload */
    assert(pocket_video_stream_open(&s,read_memory,&m,m.size));
    assert(pocket_video_stream_latest(&s,100,pixels,sizeof pixels,&pts,8)==
           POCKET_VIDEO_STREAM_FRAME&&pts==100);
    assert(pixels[0]==1&&pixels[7]==1);
    assert(pocket_video_stream_latest(&s,110,pixels,sizeof pixels,&pts,8)==
           POCKET_VIDEO_STREAM_IO);
    assert(pixels[0]==2&&pixels[2]==2&&pixels[3]==1);
    /* A short payload read modified the output buffer without producing a
     * frame. The worker must mark this staging slot EMPTY, never READY. */
    make(&m);u64(m.data+36,100);
    assert(pocket_video_stream_open(&s,read_memory,&m,m.size));
    assert(pocket_video_stream_latest(&s,200,pixels,sizeof pixels,&pts,8)==
           POCKET_VIDEO_STREAM_INVALID);

    /* A selected candidate survives producer data reuse until ACK; failed
     * transfer keeps it pending, explicit discard requires old-image repair. */
    uint16_t a[4],b[4],c[4];uint16_t *buffers[3]={a,b,c};
    ksn_video_frames pool;ksn_video_handle h,chosen;uint16_t *dst;
    assert(ksn_video_frames_init(&pool,2,2,buffers,4));
    assert(ksn_video_begin(&pool,&h,&dst)==KSN_VIDEO_OK);
    for(unsigned i=0;i<4;i++)dst[i]=0x1111;
    assert(ksn_video_publish(&pool,h,100)==KSN_VIDEO_OK);
    assert(ksn_video_select(&pool,100,&chosen)==KSN_VIDEO_OK);
    assert(ksn_video_finish(&pool,chosen,true));
    assert(ksn_video_begin(&pool,&h,&dst)==KSN_VIDEO_OK);
    for(unsigned i=0;i<4;i++)dst[i]=0x2222;
    assert(ksn_video_publish(&pool,h,110)==KSN_VIDEO_OK);
    assert(ksn_video_select(&pool,110,&chosen)==KSN_VIDEO_OK);
    assert(ksn_video_select(&pool,120,&h)==KSN_VIDEO_BUSY);
    assert(ksn_video_finish(&pool,chosen,false));
    assert(ksn_video_needs_repair(&pool));
    assert(pool.slots[pool.committed.slot].pixels[0]==0x1111);
    ksn_video_repair_done(&pool);
    assert(ksn_video_select(&pool,110,&chosen)==KSN_VIDEO_OK);
    assert(ksn_video_finish(&pool,chosen,true));
    puts("KSV1 bounds, PTS and candidate repair passed");
}
