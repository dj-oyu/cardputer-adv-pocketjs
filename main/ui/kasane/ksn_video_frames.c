#include "ksn_video_frames.h"
#include <string.h>

static bool same(ksn_video_handle a,ksn_video_handle b)
{
    return a.generation && a.generation == b.generation && a.slot == b.slot;
}

static ksn_video_slot *lookup(ksn_video_frames *frames,ksn_video_handle handle)
{
    if (!frames || !handle.generation || handle.slot >= KSN_VIDEO_SLOTS)
        return NULL;
    ksn_video_slot *slot = &frames->slots[handle.slot];
    return slot->generation == handle.generation ? slot : NULL;
}

bool ksn_video_frames_init(ksn_video_frames *frames,uint16_t width,
                           uint16_t height,uint16_t *buffers[KSN_VIDEO_SLOTS],
                           size_t pixels_per_buffer)
{
    if (!frames || !buffers || !width || !height ||
        (size_t)width * height > KSN_VIDEO_MAX_PIXELS ||
        pixels_per_buffer < (size_t)width * height) return false;
    for (unsigned i = 0; i < KSN_VIDEO_SLOTS; ++i) {
        if (!buffers[i]) return false;
        for (unsigned j = 0; j < i; ++j) {
            uintptr_t a = (uintptr_t)buffers[i], b = (uintptr_t)buffers[j];
            size_t bytes = (size_t)width * height * sizeof(uint16_t);
            if (a <= b ? b-a < bytes : a-b < bytes) return false;
        }
    }
    memset(frames, 0, sizeof *frames);
    frames->width = width; frames->height = height;
    frames->epoch = 1;
    for (unsigned i = 0; i < KSN_VIDEO_SLOTS; ++i)
        frames->slots[i].pixels = buffers[i];
    return true;
}

ksn_video_result ksn_video_begin(ksn_video_frames *frames,
                                  ksn_video_handle *handle,uint16_t **pixels)
{
    if (!frames || !frames->width || !handle || !pixels) return KSN_VIDEO_INVALID;
    int free_slot = -1;
    for (unsigned i = 0; i < KSN_VIDEO_SLOTS; ++i) {
        ksn_video_slot *slot = &frames->slots[i];
        if (!slot->pins && slot->state != KSN_VIDEO_WRITING &&
            !same(frames->committed,(ksn_video_handle){slot->generation,(uint8_t)i}) &&
            !same(frames->pending,(ksn_video_handle){slot->generation,(uint8_t)i}) &&
            (free_slot < 0 || slot->state == KSN_VIDEO_FREE ||
             slot->sequence < frames->slots[free_slot].sequence))
            free_slot = (int)i;
        if (free_slot >= 0 && frames->slots[free_slot].state == KSN_VIDEO_FREE)
            break;
    }
    if (free_slot < 0) return KSN_VIDEO_BUSY;
    ksn_video_slot *slot = &frames->slots[free_slot];
    if (++frames->next_generation == 0) ++frames->next_generation;
    slot->generation = frames->next_generation;
    slot->sequence = 0; slot->pts_us = 0;
    slot->state = KSN_VIDEO_WRITING;
    *handle = (ksn_video_handle){slot->generation,(uint8_t)free_slot};
    *pixels = slot->pixels;
    return KSN_VIDEO_OK;
}

ksn_video_result ksn_video_publish(ksn_video_frames *frames,
                                    ksn_video_handle handle,uint64_t pts_us)
{
    ksn_video_slot *slot = lookup(frames, handle);
    if (!slot || slot->state != KSN_VIDEO_WRITING) return KSN_VIDEO_INVALID;
    slot->pts_us = pts_us;
    slot->epoch = frames->epoch;
    slot->sequence = ++frames->next_sequence;
    slot->state = KSN_VIDEO_READY;
    return KSN_VIDEO_OK;
}

bool ksn_video_abort_write(ksn_video_frames *frames,ksn_video_handle handle)
{
    ksn_video_slot *slot=lookup(frames,handle);
    if (!slot || slot->state != KSN_VIDEO_WRITING) return false;
    slot->state=KSN_VIDEO_FREE;
    return true;
}

ksn_video_result ksn_video_select(ksn_video_frames *frames,uint64_t now_us,
                                   ksn_video_handle *handle)
{
    if (!frames || !handle || !frames->width) return KSN_VIDEO_INVALID;
    if (frames->pending.generation || frames->repair_required)
        return KSN_VIDEO_BUSY;
    uint64_t committed_sequence = 0, committed_pts = 0;
    if (frames->committed.generation &&
        frames->slots[frames->committed.slot].epoch == frames->epoch) {
        const ksn_video_slot *old = &frames->slots[frames->committed.slot];
        committed_sequence = old->sequence; committed_pts = old->pts_us;
    }
    int newest = -1;
    for (unsigned i = 0; i < KSN_VIDEO_SLOTS; ++i) {
        const ksn_video_slot *slot = &frames->slots[i];
        if (slot->state != KSN_VIDEO_READY || slot->epoch != frames->epoch ||
            slot->pins == UINT32_MAX ||
            slot->pts_us > now_us ||
            (slot->pts_us < committed_pts ||
             (slot->pts_us == committed_pts &&
              slot->sequence <= committed_sequence)) ||
            same(frames->committed,
                 (ksn_video_handle){slot->generation,(uint8_t)i})) continue;
        if (newest < 0 || slot->pts_us > frames->slots[newest].pts_us ||
            (slot->pts_us == frames->slots[newest].pts_us &&
             slot->sequence > frames->slots[newest].sequence))
            newest = (int)i;
    }
    if (newest < 0) return KSN_VIDEO_EMPTY;
    ksn_video_slot *slot = &frames->slots[newest];
    ++slot->pins;
    frames->pending = (ksn_video_handle){slot->generation,(uint8_t)newest};
    *handle = frames->pending;
    return KSN_VIDEO_OK;
}

ksn_video_result ksn_video_timeline_reset(ksn_video_frames *frames)
{
    if (!frames || !frames->width) return KSN_VIDEO_INVALID;
    if (frames->pending.generation || frames->repair_required)
        return KSN_VIDEO_BUSY;
    if (++frames->epoch == 0) ++frames->epoch;
    for (unsigned i = 0; i < KSN_VIDEO_SLOTS; ++i) {
        ksn_video_slot *slot = &frames->slots[i];
        if (slot->state == KSN_VIDEO_READY && !slot->pins)
            slot->state = KSN_VIDEO_FREE;
    }
    return KSN_VIDEO_OK;
}

bool ksn_video_finish(ksn_video_frames *frames,ksn_video_handle handle,
                       bool transferred)
{
    if (!frames || !same(frames->pending,handle)) return false;
    if (transferred) {
        if (frames->committed.generation) {
            ksn_video_slot *old = lookup(frames,frames->committed);
            if (old && old->pins) --old->pins;
        }
        frames->committed = handle;
        frames->repair_required = false;
    } else {
        ksn_video_slot *slot = lookup(frames,handle);
        if (slot && slot->pins) --slot->pins;
        frames->repair_required = true;
    }
    frames->pending = (ksn_video_handle){0};
    return true;
}

bool ksn_video_pin(ksn_video_frames *frames,ksn_video_handle handle)
{
    ksn_video_slot *slot = lookup(frames,handle);
    if (!slot || slot->state != KSN_VIDEO_READY || slot->pins == UINT32_MAX)
        return false;
    ++slot->pins; return true;
}

bool ksn_video_release(ksn_video_frames *frames,ksn_video_handle handle)
{
    ksn_video_slot *slot = lookup(frames,handle);
    if (!slot) return false;
    unsigned internal = (unsigned)same(frames->committed,handle) +
                        (unsigned)same(frames->pending,handle);
    if (slot->pins <= internal) return false;
    --slot->pins; return true;
}

ksn_video_handle ksn_video_committed(const ksn_video_frames *frames)
{ return frames ? frames->committed : (ksn_video_handle){0}; }
bool ksn_video_needs_repair(const ksn_video_frames *frames)
{ return frames && frames->repair_required; }
void ksn_video_repair_done(ksn_video_frames *frames)
{ if (frames && !frames->pending.generation) frames->repair_required = false; }

static ksn_result video_span(void *ctx,uint16_t variant,uint16_t frame,
                              uint16_t y,uint16_t x,uint16_t count,
                              uint16_t *rgb565,uint8_t *alpha)
{
    const ksn_video_frames *frames = ctx;
    if (!frames || variant || frame >= KSN_VIDEO_SLOTS ||
        y >= frames->height || x > frames->width ||
        count > frames->width - x || (count && (!rgb565 || !alpha)))
        return KSN_INVALID;
    const ksn_video_slot *slot = &frames->slots[frame];
    if (slot->state != KSN_VIDEO_READY || !slot->pins) return KSN_INVALID;
    if (count) {
        memcpy(rgb565,slot->pixels + (size_t)y*frames->width+x,
               (size_t)count*sizeof *rgb565);
        memset(alpha,255,count);
    }
    return KSN_OK;
}

void ksn_video_image_port(ksn_video_frames *frames,ksn_image_port *out)
{
    if (!out) return;
    *out = frames && frames->width ? (ksn_image_port){
        .ctx=frames,.width=frames->width,.height=frames->height,
        .variants=1,.frames=KSN_VIDEO_SLOTS,.read_span=video_span,.opaque=true
    } : (ksn_image_port){0};
}
