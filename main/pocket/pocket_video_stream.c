#include "pocket_video_stream.h"
#include <string.h>

static uint16_t le16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p)
{ return (uint64_t)le32(p) | (uint64_t)le32(p+4) << 32; }
static bool exact(pocket_video_stream *s,uint32_t at,uint8_t *out,uint32_t n)
{ return s->read(s->ctx,at,out,n)==(int32_t)n; }

bool pocket_video_stream_open(pocket_video_stream *s,
                              pocket_video_stream_read read,void *ctx,
                              uint32_t size)
{
    if(!s||!read||size<POCKET_VIDEO_STREAM_HEADER)return false;
    uint8_t h[POCKET_VIDEO_STREAM_HEADER];
    pocket_video_stream temp={.read=read,.ctx=ctx,.size=size};
    if(!exact(&temp,0,h,sizeof h)||memcmp(h,"KSV1",4))return false;
    temp.width=le16(h+4);temp.height=le16(h+6);
    temp.frame_bytes=le32(h+8);
    if(!temp.width||!temp.height||
       (uint32_t)temp.width*temp.height>POCKET_VIDEO_STREAM_MAX_PIXELS||
       temp.frame_bytes!=(uint32_t)temp.width*temp.height*2||le32(h+12))
        return false;
    temp.offset=POCKET_VIDEO_STREAM_HEADER;
    *s=temp;
    return true;
}

pocket_video_stream_result pocket_video_stream_next(
    pocket_video_stream *s,uint64_t clock_us,uint8_t *pixels,
    size_t capacity,uint64_t *pts_us)
{
    if(!s||!s->read||!pixels||capacity<s->frame_bytes||!pts_us)
        return POCKET_VIDEO_STREAM_INVALID;
    if(s->ended)return POCKET_VIDEO_STREAM_END;
    if(s->offset==s->size){s->ended=true;return POCKET_VIDEO_STREAM_END;}
    if(s->size-s->offset<POCKET_VIDEO_STREAM_RECORD){
        s->ended=true;return POCKET_VIDEO_STREAM_INVALID;
    }
    uint64_t pts;
    uint32_t bytes;
    if(s->have_future){pts=s->future_pts;bytes=s->future_bytes;}
    else {
        uint8_t h[POCKET_VIDEO_STREAM_RECORD];
        if(!exact(s,s->offset,h,sizeof h)){
            s->ended=true;return POCKET_VIDEO_STREAM_IO;
        }
        pts=le64(h);bytes=le32(h+8);
    }
    if(bytes!=s->frame_bytes||bytes>s->size-s->offset-POCKET_VIDEO_STREAM_RECORD||
       (s->have_previous&&pts<=s->previous_pts)){
        s->ended=true;return POCKET_VIDEO_STREAM_INVALID;
    }
    if(pts>clock_us){
        s->future_pts=pts;s->future_bytes=bytes;s->have_future=true;
        return POCKET_VIDEO_STREAM_FUTURE;
    }
    uint32_t payload=s->offset+POCKET_VIDEO_STREAM_RECORD;
    if(!exact(s,payload,pixels,bytes)){
        s->ended=true;return POCKET_VIDEO_STREAM_IO;
    }
    s->offset=payload+bytes;
    s->have_future=false;
    s->previous_pts=pts;s->have_previous=true;
    *pts_us=pts;
    return POCKET_VIDEO_STREAM_FRAME;
}

pocket_video_stream_result pocket_video_stream_latest(
    pocket_video_stream *s,uint64_t clock_us,uint8_t *pixels,
    size_t capacity,uint64_t *pts_us,unsigned max_records)
{
    if(!s||!s->read||!pixels||capacity<s->frame_bytes||!pts_us||!max_records)
        return POCKET_VIDEO_STREAM_INVALID;
    if(s->ended)return POCKET_VIDEO_STREAM_END;
    if(!s->scanning){s->scan_clock=clock_us;s->scanning=true;}
    for(unsigned i=0;i<max_records;i++){
        if(s->offset==s->size){s->ended=true;break;}
        if(s->size-s->offset<POCKET_VIDEO_STREAM_RECORD){
            s->ended=true;return POCKET_VIDEO_STREAM_INVALID;
        }
        uint64_t pts;uint32_t bytes;
        if(s->have_future){pts=s->future_pts;bytes=s->future_bytes;}
        else {
            uint8_t h[POCKET_VIDEO_STREAM_RECORD];
            if(!exact(s,s->offset,h,sizeof h)){
                s->ended=true;return POCKET_VIDEO_STREAM_IO;
            }
            pts=le64(h);bytes=le32(h+8);
        }
        if(bytes!=s->frame_bytes||
           bytes>s->size-s->offset-POCKET_VIDEO_STREAM_RECORD||
           (s->have_previous&&pts<=s->previous_pts)){
            s->ended=true;return POCKET_VIDEO_STREAM_INVALID;
        }
        if(pts>s->scan_clock){
            s->future_pts=pts;s->future_bytes=bytes;s->have_future=true;
            break;
        }
        s->have_future=false;
        s->have_candidate=true;
        s->candidate_at=s->offset+POCKET_VIDEO_STREAM_RECORD;
        s->candidate_pts=pts;
        s->offset=s->candidate_at+bytes;
        s->previous_pts=pts;s->have_previous=true;
    }
    if(!s->ended&&!s->have_future)
        return POCKET_VIDEO_STREAM_SCANNING;
    s->scanning=false;
    if(!s->have_candidate)return s->ended?POCKET_VIDEO_STREAM_END:
                              POCKET_VIDEO_STREAM_FUTURE;
    if(!exact(s,s->candidate_at,pixels,s->frame_bytes)){
        s->ended=true;return POCKET_VIDEO_STREAM_IO;
    }
    *pts_us=s->candidate_pts;s->have_candidate=false;
    return POCKET_VIDEO_STREAM_FRAME;
}
