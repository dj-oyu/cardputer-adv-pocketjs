#ifndef KSN_VIDEO_FRAMES_H
#define KSN_VIDEO_FRAMES_H

#include "ksn_ports.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Small, rereadable RGB565 frames. The owner supplies three buffers; no
 * decoder, allocation or I/O runs from the image provider. A draw command's
 * frame index is valid only while its handle remains pinned by the owner.
 * Keep the pool and buffers alive until Kasane has reset its image provider. */
#define KSN_VIDEO_SLOTS 3u
#define KSN_VIDEO_MAX_PIXELS 4096u

typedef struct { uint32_t generation; uint8_t slot; } ksn_video_handle;
typedef enum { KSN_VIDEO_OK, KSN_VIDEO_EMPTY, KSN_VIDEO_BUSY,
               KSN_VIDEO_INVALID } ksn_video_result;
typedef enum { KSN_VIDEO_FREE, KSN_VIDEO_WRITING,
               KSN_VIDEO_READY } ksn_video_slot_state;
typedef struct {
    uint16_t *pixels;
    uint64_t pts_us, sequence;
    uint32_t generation, pins, epoch;
    ksn_video_slot_state state;
} ksn_video_slot;
typedef struct {
    ksn_video_slot slots[KSN_VIDEO_SLOTS];
    uint16_t width,height;
    uint32_t next_generation;
    uint32_t epoch;
    uint64_t next_sequence;
    ksn_video_handle committed,pending;
    bool repair_required;
} ksn_video_frames;

bool ksn_video_frames_init(ksn_video_frames *frames,uint16_t width,
                           uint16_t height,uint16_t *buffers[KSN_VIDEO_SLOTS],
                           size_t pixels_per_buffer);
/* A BUSY producer skips this input frame. It never overwrites a pinned frame. */
ksn_video_result ksn_video_begin(ksn_video_frames *frames,
                                  ksn_video_handle *handle,uint16_t **pixels);
ksn_video_result ksn_video_publish(ksn_video_frames *frames,
                                    ksn_video_handle handle,uint64_t pts_us);
bool ksn_video_abort_write(ksn_video_frames *frames,ksn_video_handle handle);
/* Pick the newest available frame at or before the audio/display clock.
 * Selection pins a candidate until finish; previous committed remains pinned. */
ksn_video_result ksn_video_select(ksn_video_frames *frames,uint64_t now_us,
                                   ksn_video_handle *handle);
/* Start a new PTS domain after seek/restart. A displayed old frame stays pinned
 * until its replacement is acknowledged. A pending/repair frame blocks reset. */
ksn_video_result ksn_video_timeline_reset(ksn_video_frames *frames);
/* finish(true) follows Kasane's presented ACK. An I/O failure leaves Kasane's
 * submission sealed for retry, so keep the candidate pinned. Call
 * finish(false) only after the owner explicitly discards that submission;
 * then repair the committed image and call repair_done. */
bool ksn_video_finish(ksn_video_frames *frames,ksn_video_handle handle,
                       bool transferred);
bool ksn_video_pin(ksn_video_frames *frames,ksn_video_handle handle);
bool ksn_video_release(ksn_video_frames *frames,ksn_video_handle handle);
ksn_video_handle ksn_video_committed(const ksn_video_frames *frames);
bool ksn_video_needs_repair(const ksn_video_frames *frames);
void ksn_video_repair_done(ksn_video_frames *frames);
void ksn_video_image_port(ksn_video_frames *frames,ksn_image_port *out);

#endif
