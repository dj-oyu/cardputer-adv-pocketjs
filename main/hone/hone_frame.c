#include "hone_frame.h"

#include <limits.h>

hone_capabilities hone_frame_capabilities(void)
{
    return (hone_capabilities){1, HONE_OPAQUE_RGB565, HONE_RGB565_NATIVE, UINT16_MAX, UINT8_MAX};
}

static bool valid_rect(hone_rect r, unsigned width, unsigned height)
{
    return r.width && r.height && r.x < width && r.y < height &&
           r.width <= width - r.x && r.height <= height - r.y;
}

hone_result hone_frame_begin(hone_frame *f, const hone_frame_options *o, hone_entry *entries,
                             unsigned capacity)
{
    if (!f || !o || (capacity && !entries) || !o->width || !o->height) return HONE_INVALID_ARGUMENT;
    if (f->phase != HONE_IDLE || f->busy) return HONE_BUSY;
    if (o->profile != HONE_OPAQUE_RGB565) return HONE_UNSUPPORTED;
    if (o->width > UINT16_MAX || o->height > UINT16_MAX || capacity > UINT8_MAX ||
        o->max_drawables > capacity) return HONE_LIMIT_EXCEEDED;
    uint64_t work = (uint64_t)o->width * o->height;
    if (o->max_bytes < sizeof *f + capacity * sizeof *entries || work > o->max_work)
        return HONE_LIMIT_EXCEEDED;
    *f = (hone_frame){.options = *o, .entries = entries,
                      .work = work, .phase = HONE_BUILDING};
    return HONE_OK;
}

hone_result hone_frame_submit(hone_frame *f, hone_drawable *source, const hone_rect *clip)
{
    if (!f || !source || !source->render) return HONE_INVALID_ARGUMENT;
    if (f->busy) return HONE_BUSY;
    if (f->phase != HONE_BUILDING) return HONE_CLOSED;
    hone_rect r = clip ? *clip : (hone_rect){0, 0, f->options.width, f->options.height};
    if (!valid_rect(r, f->options.width, f->options.height)) return HONE_INVALID_ARGUMENT;
    uint64_t work = (uint64_t)r.width * r.height;
    if (f->count >= f->options.max_drawables || work > f->options.max_work - f->work)
        return HONE_LIMIT_EXCEEDED;
    f->entries[f->count++] = (hone_entry){*source, r};
    f->work += work;
    *source = (hone_drawable){0};
    return HONE_OK;
}

hone_result hone_frame_commit(hone_frame *f)
{
    if (!f) return HONE_INVALID_ARGUMENT;
    if (f->busy) return HONE_BUSY;
    if (f->phase != HONE_BUILDING) return HONE_CLOSED;
    /* This profile has no cross-entry constraints. Each entry was fully
     * admitted before its ownership transfer; an empty frame clears the view. */
    f->phase = HONE_COMMITTED;
    return HONE_OK;
}

static hone_result release_frame(hone_frame *f, hone_phase expected)
{
    if (!f) return HONE_INVALID_ARGUMENT;
    if (f->busy || f->presentation) return HONE_BUSY;
    if (f->phase != expected && f->phase != HONE_RELEASING) return HONE_CLOSED;
    f->phase = HONE_RELEASING;
    f->busy = true;
    while (f->count) {
        hone_drawable *source = &f->entries[f->count - 1].drawable;
        if (source->release && !source->release(source->context)) {
            f->busy = false;
            return HONE_BUSY;
        }
        --f->count;
    }
    *f = (hone_frame){0};
    return HONE_OK;
}

hone_result hone_frame_cancel(hone_frame *f) { return release_frame(f, HONE_BUILDING); }
hone_result hone_snapshot_dispose(hone_frame *f) { return release_frame(f, HONE_COMMITTED); }

static bool intersect(hone_rect a, hone_rect b, hone_rect *out)
{
    unsigned x = a.x > b.x ? a.x : b.x, y = a.y > b.y ? a.y : b.y;
    unsigned right = a.x + a.width < b.x + b.width ? a.x + a.width : b.x + b.width;
    unsigned bottom = a.y + a.height < b.y + b.height ? a.y + a.height : b.y + b.height;
    if (right <= x || bottom <= y) return false;
    *out = (hone_rect){x, y, right - x, bottom - y};
    return true;
}

static void clear_region(const hone_target *t, hone_rect r, uint16_t color)
{
    for (unsigned y = 0; y < r.height; ++y) {
        uint16_t *row = (uint16_t *)((uint8_t *)t->pixels + y * t->stride_bytes);
        for (unsigned x = 0; x < r.width; ++x) row[x] = color;
    }
}

hone_result hone_snapshot_render(hone_frame *f, hone_rect r, const hone_target *t, hone_span *nonzero)
{
    if (!f || !t || !t->pixels) return HONE_INVALID_ARGUMENT;
    if (f->busy) return HONE_BUSY;
    if (f->phase != HONE_COMMITTED) return HONE_CLOSED;
    if (t->format != HONE_RGB565_NATIVE) return HONE_UNSUPPORTED;
    if (!valid_rect(r, f->options.width, f->options.height) ||
        t->width < r.width || t->height < r.height || t->stride_bytes % sizeof(uint16_t) ||
        (uintptr_t)t->pixels % _Alignof(uint16_t) ||
        t->width > t->stride_bytes / sizeof(uint16_t)) return HONE_INVALID_ARGUMENT;
    size_t row_bytes = (size_t)t->width * sizeof(uint16_t);
    if (t->capacity_bytes < row_bytes ||
        t->height - 1u > (t->capacity_bytes - row_bytes) / t->stride_bytes ||
        t->capacity_bytes - 1u > UINTPTR_MAX - (uintptr_t)t->pixels)
        return HONE_INVALID_ARGUMENT;

    f->busy = true;
    hone_span bounds = {0};
    bool initialized = false;
    hone_result result = HONE_OK;
    for (unsigned i = 0; i < f->count; ++i) {
        const hone_entry *entry = &f->entries[i];
        hone_rect part;
        if (!intersect(r, entry->clip, &part)) continue;
        if (!initialized && (part.width != r.width || part.height != r.height)) {
            clear_region(t, r, f->options.clear);
            if (f->options.clear) bounds = (hone_span){r.x, r.x + r.width};
        }
        initialized = true;
        size_t offset = (part.y - r.y) * t->stride_bytes + (part.x - r.x) * sizeof(uint16_t);
        hone_target sub = {.pixels = (uint16_t *)((uint8_t *)t->pixels + offset),
                           .width = part.width, .height = part.height, .stride_bytes = t->stride_bytes,
                           .capacity_bytes = t->capacity_bytes - offset, .format = t->format};
        hone_span span = {part.x, part.x + part.width};
        if (!entry->drawable.render(entry->drawable.context, part, &sub, &span) ||
            (span.right && (span.left < part.x || span.left >= span.right || span.right > part.x + part.width)) ||
            (!span.right && span.left)) {
            result = HONE_IO_ERROR;
            break;
        }
        if (span.right) {
            if (!bounds.right || span.left < bounds.left) bounds.left = span.left;
            if (span.right > bounds.right) bounds.right = span.right;
        }
    }
    if (!initialized) {
        clear_region(t, r, f->options.clear);
        if (f->options.clear) bounds = (hone_span){r.x, r.x + r.width};
    }
    f->busy = false;
    if (nonzero && result == HONE_OK) *nonzero = bounds;
    return result;
}

hone_result hone_present(hone_frame *f, hone_presentation *op)
{
    if (!f || !op) return HONE_INVALID_ARGUMENT;
    if (f->busy || f->presentation || op->snapshot) return HONE_BUSY;
    if (f->phase != HONE_COMMITTED) return HONE_CLOSED;
    *op = (hone_presentation){.snapshot = f, .state = HONE_PENDING};
    f->presentation = op;
    return HONE_OK;
}

hone_result hone_presentation_complete(hone_presentation *op, bool drained, hone_result outcome)
{
    if (!op) return HONE_INVALID_ARGUMENT;
    if (!op->snapshot || op->snapshot->presentation != op) return HONE_CLOSED;
    if (!drained || op->snapshot->busy) return HONE_BUSY;
    op->snapshot->presentation = NULL;
    op->snapshot = NULL;
    op->state = outcome == HONE_OK ? HONE_PRESENTED : HONE_FAILED;
    op->reason = outcome;
    return HONE_OK;
}

hone_presentation_state hone_presentation_poll(const hone_presentation *op, hone_result *reason)
{
    if (!op || (!op->snapshot && op->state == HONE_PENDING)) {
        if (reason) *reason = HONE_CLOSED;
        return HONE_FAILED;
    }
    if (reason) *reason = op->reason;
    return op->state;
}
