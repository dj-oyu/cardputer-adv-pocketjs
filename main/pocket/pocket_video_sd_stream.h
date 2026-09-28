#ifndef POCKET_VIDEO_SD_STREAM_H
#define POCKET_VIDEO_SD_STREAM_H
#include <stdbool.h>
#include <stdint.h>

typedef struct pocket_video_sd_stream pocket_video_sd_stream;
/* Owner task only. The caller must already have an open video image of the
 * container dimensions. The worker owns only the SD lease and staging slot. */
pocket_video_sd_stream *pocket_video_sd_stream_start(const char *path,
    uint16_t width,uint16_t height,const char **code);
void pocket_video_sd_stream_clock(pocket_video_sd_stream *s,uint64_t clock_us);
void pocket_video_sd_stream_pause(pocket_video_sd_stream *s,bool paused);
/* Borrowed bytes stay immutable until release; no pool/JS call from worker. */
bool pocket_video_sd_stream_borrow(pocket_video_sd_stream *s,
    const uint8_t **pixels,uint32_t *bytes,uint64_t *pts_us);
void pocket_video_sd_stream_release(pocket_video_sd_stream *s);
/* -1 invalid/I/O, 0 running, 1 clean EOF. A final staged frame remains
 * borrowable after EOF. */
int pocket_video_sd_stream_state(const pocket_video_sd_stream *s);
bool pocket_video_sd_stream_stop(pocket_video_sd_stream *s);
void pocket_video_sd_stream_reap(void);
#endif
