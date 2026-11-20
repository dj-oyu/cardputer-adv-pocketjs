#include "hone/hone_frame.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    hone_frame *frame;
    uint16_t color;
    int renders, releases;
    bool release_busy;
} source;

static bool draw(void *context, hone_rect r, const hone_target *t, hone_span *span)
{
    source *s = context;
    assert(hone_snapshot_dispose(s->frame) == HONE_BUSY);
    ++s->renders;
    for (unsigned y = 0; y < r.height; ++y) {
        uint16_t *p = (uint16_t *)((uint8_t *)t->pixels + y * t->stride_bytes);
        for (unsigned x = 0; x < r.width; ++x) p[x] = s->color + 10 * (r.y + y) + r.x + x;
    }
    *span = (hone_span){r.x, r.x + r.width};
    return true;
}

static bool release(void *context)
{
    source *s = context;
    ++s->releases;
    return !s->release_busy;
}

int main(void)
{
    hone_frame frame = {0}, other = {0};
    hone_entry slots[2];
    hone_frame_options options = {HONE_OPAQUE_RGB565, 8, 5, 7, sizeof frame + sizeof slots, 2, 54};
    hone_rect base_clip = {1, 1, 4, 3}, top_clip = {2, 1, 1, 2};
    source base = {&frame, 100, 0, 0, false}, top = {&frame, 200, 0, 0, false};
    hone_drawable a = {&base, draw, release}, b = {&top, draw, release};
    assert(hone_frame_capabilities().profile == options.profile);
    --options.max_bytes;
    assert(hone_frame_begin(&frame, &options, slots, 2) == HONE_LIMIT_EXCEEDED);
    ++options.max_bytes;
    assert(hone_frame_begin(&frame, &options, slots, 2) == HONE_OK);
    assert(hone_frame_submit(&frame, &a, &(hone_rect){7, 4, 2, 1}) == HONE_INVALID_ARGUMENT);
    assert(hone_frame_submit(&frame, &a, NULL) == HONE_LIMIT_EXCEEDED);
    assert(a.render == draw && frame.count == 0);
    assert(hone_frame_submit(&frame, &a, &base_clip) == HONE_OK && !a.render);
    assert(hone_frame_submit(&frame, &b, &top_clip) == HONE_OK && !b.render);
    assert(hone_frame_commit(&frame) == HONE_OK);
    hone_presentation op = {0};
    assert(hone_present(&frame, &op) == HONE_OK);
    assert(hone_presentation_complete(&op, false, HONE_IO_ERROR) == HONE_BUSY);
    assert(hone_presentation_poll(&op, NULL) == HONE_PENDING);
    assert(hone_snapshot_dispose(&frame) == HONE_BUSY && !base.releases);
    assert(hone_presentation_complete(&op, true, HONE_IO_ERROR) == HONE_OK);
    hone_result reason;
    assert(hone_presentation_poll(&op, &reason) == HONE_FAILED && reason == HONE_IO_ERROR);
    assert(hone_present(&frame, &op) == HONE_OK);

    uint16_t pixels[4][8];
    memset(pixels, 0xab, sizeof pixels);
    hone_rect region = {1, 0, 4, 4};
    hone_target t = {&pixels[0][0], 4, 4, sizeof pixels[0], sizeof pixels, HONE_RGB565_NATIVE};
    hone_target invalid = t;
    invalid.capacity_bytes = 55; /* 3 strides + 4 pixels require 56 bytes */
    assert(hone_snapshot_render(&frame, region, &invalid, NULL) == HONE_INVALID_ARGUMENT);
    invalid = t;
    invalid.stride_bytes = 7;
    assert(hone_snapshot_render(&frame, region, &invalid, NULL) == HONE_INVALID_ARGUMENT);
    invalid = t;
    invalid.format = 0;
    assert(hone_snapshot_render(&frame, region, &invalid, NULL) == HONE_UNSUPPORTED);
    assert(pixels[0][0] == 0xabab && !base.renders);
    assert(hone_snapshot_render(&frame, region, &t, NULL) == HONE_OK);
    for (unsigned y = 0; y < 4; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            unsigned color = (x == 1 && y < 3) ? 200 : 100;
            assert(pixels[y][x] == (y ? color + 10 * y + 1 + x : 7));
        }
        for (unsigned x = 4; x < 8; ++x) assert(pixels[y][x] == 0xabab);
    }

    hone_entry empty;
    options.max_drawables = 1;
    assert(hone_frame_begin(&other, &options, &empty, 1) == HONE_OK);
    assert(hone_frame_cancel(&other) == HONE_OK);
    assert(hone_presentation_complete(&op, true, HONE_OK) == HONE_OK);
    assert(hone_presentation_poll(&op, &reason) == HONE_PRESENTED && reason == HONE_OK);
    base.release_busy = true;
    assert(hone_snapshot_dispose(&frame) == HONE_BUSY && top.releases == 1);
    base.release_busy = false;
    assert(hone_snapshot_dispose(&frame) == HONE_OK && top.releases == 1 && base.releases == 2);
    assert(hone_snapshot_render(&frame, region, &t, NULL) == HONE_CLOSED);
    puts("PASS bounded composition, cropped RGB565, rollback, DMA pin and retry");
}
