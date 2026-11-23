#ifndef KOBAKO_VIDEO_STREAM_H
#define KOBAKO_VIDEO_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* KSV1: 16-byte header (magic, width u16, height u16, frame bytes u32,
 * reserved u32=0), then records (PTS us u64, byte count u32, RGB565 LE).
 * PTS values must increase strictly. No index, compression or padding. */
#define KOBAKO_VIDEO_STREAM_MAX_PIXELS 4096u
#define KOBAKO_VIDEO_STREAM_HEADER 16u
#define KOBAKO_VIDEO_STREAM_RECORD 12u

typedef int32_t (*kobako_video_stream_read)(void *ctx, uint32_t offset,
                                             uint8_t *out, uint32_t size);
typedef enum {
    KOBAKO_VIDEO_STREAM_FRAME,
    KOBAKO_VIDEO_STREAM_SCANNING,
    KOBAKO_VIDEO_STREAM_FUTURE,
    KOBAKO_VIDEO_STREAM_END,
    KOBAKO_VIDEO_STREAM_INVALID,
    KOBAKO_VIDEO_STREAM_IO
} kobako_video_stream_result;
typedef struct {
    kobako_video_stream_read read;
    void *ctx;
    uint32_t size, offset, frame_bytes;
    uint16_t width, height;
    uint64_t previous_pts;
    uint64_t future_pts;
    uint64_t candidate_pts, scan_clock;
    uint32_t future_bytes;
    uint32_t candidate_at;
    bool have_previous, have_future, have_candidate, scanning, ended;
} kobako_video_stream;

/* Caller owns the read lease and a frame-sized output buffer. The header and
 * each record are validated before any frame can be published. */
bool kobako_video_stream_open(kobako_video_stream *stream,
                              kobako_video_stream_read read, void *ctx,
                              uint32_t size);
kobako_video_stream_result kobako_video_stream_next(
    kobako_video_stream *stream, uint64_t clock_us, uint8_t *pixels,
    size_t capacity, uint64_t *pts_us);
/* Scan at most max_records headers, skipping stale RGB565 payload reads.
 * SCANNING means the budget was reached; repeat until FRAME/FUTURE/END.
 * One payload is read only when the scan reaches a future record or EOF. */
kobako_video_stream_result kobako_video_stream_latest(
    kobako_video_stream *stream,uint64_t clock_us,uint8_t *pixels,
    size_t capacity,uint64_t *pts_us,unsigned max_records);

#endif
