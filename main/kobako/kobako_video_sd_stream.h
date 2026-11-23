#ifndef KOBAKO_VIDEO_SD_STREAM_H
#define KOBAKO_VIDEO_SD_STREAM_H
#include <stdbool.h>
#include <stdint.h>

typedef struct kobako_video_sd_stream kobako_video_sd_stream;
/* Owner task only. The caller must already have an open video image of the
 * container dimensions. The worker owns only the SD lease and staging slot. */
kobako_video_sd_stream *kobako_video_sd_stream_start(const char *path,
    uint16_t width,uint16_t height,const char **code);
void kobako_video_sd_stream_clock(kobako_video_sd_stream *s,uint64_t clock_us);
void kobako_video_sd_stream_pause(kobako_video_sd_stream *s,bool paused);
/* Borrowed bytes stay immutable until release; no pool/JS call from worker. */
bool kobako_video_sd_stream_borrow(kobako_video_sd_stream *s,
    const uint8_t **pixels,uint32_t *bytes,uint64_t *pts_us);
void kobako_video_sd_stream_release(kobako_video_sd_stream *s);
/* -1 invalid/I/O, 0 running, 1 clean EOF. A final staged frame remains
 * borrowable after EOF. */
int kobako_video_sd_stream_state(const kobako_video_sd_stream *s);
bool kobako_video_sd_stream_stop(kobako_video_sd_stream *s);
void kobako_video_sd_stream_reap(void);
#endif
