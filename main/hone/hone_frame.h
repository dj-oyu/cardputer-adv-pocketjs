#ifndef HONE_FRAME_H
#define HONE_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Single-owner C API, version 1. Zero-initialize frames and presentations.
 * Keep a live handle at a stable address. The owner serializes all calls.
 * This profile accepts opaque, already projected RGB565 drawables covering the
 * viewport. Later submissions overwrite earlier ones inside their optional
 * clip. The projection and shading belong to the producer. */
typedef enum {
    HONE_OK, HONE_INVALID_ARGUMENT, HONE_UNSUPPORTED, HONE_LIMIT_EXCEEDED,
    HONE_BUSY, HONE_CLOSED, HONE_IO_ERROR
} hone_result;
typedef enum { HONE_OPAQUE_RGB565 = 1 } hone_profile;
typedef enum { HONE_RGB565_NATIVE = 1 } hone_format;
typedef struct {
    unsigned version;
    hone_profile profile;
    hone_format format;
    unsigned max_dimension, max_drawables;
} hone_capabilities;
hone_capabilities hone_frame_capabilities(void);

typedef struct { unsigned x, y, width, height; } hone_rect;
typedef struct { unsigned left, right; } hone_span;
typedef struct {
    uint16_t *pixels;
    unsigned width, height;
    size_t stride_bytes, capacity_bytes;
    hone_format format;
} hone_target;
typedef struct {
    hone_profile profile;
    unsigned width, height;
    uint16_t clear;
    /* max_bytes charges frame + reserved entry array. Providers own the storage
     * behind their generation. Work is viewport pixels + clipped source pixels
     * for a full render; this bounds composition, not producer evaluation. */
    size_t max_bytes;
    unsigned max_drawables;
    uint64_t max_work;
} hone_frame_options;

/* The source is one immutable generation with an owned reference. Successful
 * submit moves this reference from the caller; rejection leaves it untouched.
 * render fills its region at the target's top left. The span bounds nonzero
 * pixels in viewport x coordinates (empty = {0,0}); a full span is always safe.
 * release may return false while the producer is still busy; retry disposal.
 * A producer with permanently immutable data may omit release. */
typedef struct {
    void *context;
    bool (*render)(void *, hone_rect, const hone_target *, hone_span *);
    bool (*release)(void *);
} hone_drawable;
typedef struct { hone_drawable drawable; hone_rect clip; } hone_entry;

typedef enum { HONE_IDLE, HONE_BUILDING, HONE_COMMITTED, HONE_RELEASING } hone_phase;
typedef struct hone_presentation hone_presentation;
typedef struct {
    hone_frame_options options;
    hone_entry *entries;
    unsigned count;
    uint64_t work;
    hone_phase phase;
    bool busy;
    hone_presentation *presentation;
} hone_frame;

/* entries is exclusive scratch of capacity elements, retained until close.
 * A committed frame stays live until dispose. A new frame can be assembled in
 * separate storage, independently of a snapshot already being presented. */
hone_result hone_frame_begin(hone_frame *, const hone_frame_options *, hone_entry *, unsigned capacity);
hone_result hone_frame_submit(hone_frame *, hone_drawable *, const hone_rect *clip);
hone_result hone_frame_commit(hone_frame *);
hone_result hone_frame_cancel(hone_frame *);
hone_result hone_snapshot_render(hone_frame *, hone_rect, const hone_target *, hone_span *nonzero);
hone_result hone_snapshot_dispose(hone_frame *);

typedef enum { HONE_PENDING, HONE_PRESENTED, HONE_FAILED } hone_presentation_state;
struct hone_presentation {
    hone_frame *snapshot;
    hone_presentation_state state;
    hone_result reason;
};
/* The display adapter starts the borrow before its first submission. Only the
 * authoritative display owner reports physical drain. An uncertain drain
 * keeps the same operation pending. Terminal poll results are repeatable and
 * the committed generation remains renderable/retryable until disposed. */
hone_result hone_present(hone_frame *, hone_presentation *);
hone_result hone_presentation_complete(hone_presentation *, bool drained, hone_result outcome);
hone_presentation_state hone_presentation_poll(const hone_presentation *, hone_result *reason);
#endif
