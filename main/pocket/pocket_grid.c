#include "pocket_grid.h"
#include "pocket_api.h"
#ifdef KSN_GRID_APP_HOST_TEST
JSValue pocket_kasane_grid_resource(JSContext *ctx, unsigned slot,
                                    const ksn_image_port *port);
void pocket_kasane_grid_invalidate(unsigned slot);
bool pocket_kasane_grid_source_port(JSContext *ctx, JSValueConst object,
                                    ksn_image_port *out, uint32_t *id);
#else
#include "pocket_kasane.h"
#endif
#include "ui/kasane/ksn_proc_grid_image.h"
#include "ui/kasane/ksn_proc_grid_resize.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(KASANE_PROC_DEVICE_PROBE) && defined(ESP_PLATFORM)
#include "esp_cpu.h"
static pocket_grid_resize_profile resize_profile;
void pocket_grid_resize_profile_read(pocket_grid_resize_profile *out)
{
    *out = resize_profile;
    resize_profile = (pocket_grid_resize_profile){0};
}
#define GRID_RESIZE_START() esp_cpu_get_cycle_count()
#define GRID_RESIZE_END(field, count, start) do { \
    resize_profile.field += (uint32_t)(esp_cpu_get_cycle_count() - (start)); \
    resize_profile.count++; \
} while (0)
#else
#define GRID_RESIZE_START() 0u
#define GRID_RESIZE_END(field, count, start) ((void)(start))
#endif

#define GRID_APP_SLOTS 4u
/* No PSRAM is configured on this board. Two output generations and copied
 * inputs must fit beside the VM and Kasane arena. */
#define GRID_APP_MAX_PIXELS 4096u
#define GRID_APP_MAX_INPUTS 8192u

typedef struct {
    ksn_image_port source;
    uint32_t source_id;
    uint16_t rows[2][256];
    uint8_t alpha[256];
    uint16_t cached_y;
    bool cached;
} resize_stream;

#ifndef KSN_GRID_APP_HOST_TEST
extern const char grid_fold_start[] asm("_binary_grid_fold_js_start");
extern const char grid_fold_end[] asm("_binary_grid_fold_js_end");
#else
extern const char *pocket_grid_test_frontend;
#endif

typedef struct {
    uint32_t handle;
    ksn_grid_plan *plan;
    ksn_grid_resize_plan *resize;
    resize_stream *stream;
    ksn_grid_shape shape;
    ksn_grid_image image;
    void *output_raw[2];
    int16_t *output[2];
    void *input_raw[KSN_GRID_BUFFERS];
    int16_t *input[KSN_GRID_BUFFERS];
    size_t input_capacity[KSN_GRID_BUFFERS];
    uint8_t committed_index, candidate_index;
    bool has_output, pending, resource, resize_pie, resize_nearest;
} grid_slot;

static grid_slot slots[GRID_APP_SLOTS];
static uint32_t next_handle;
static bool js_call_active;
static bool frontend_loaded;

static JSValue fail(JSContext *ctx, const char *op, const char *code,
                    const char *message)
{
    return pocket_api_throw(ctx, code, op, message, false,
                            POCKET_OUTCOME_NOT_APPLIED);
}

static bool number(JSContext *ctx, JSValueConst value, int64_t low,
                   int64_t high, int64_t *out)
{
    double n;
    if (!JS_IsNumber(value) || JS_ToFloat64(ctx, &n, value) < 0 ||
        !isfinite(n) || n < (double)low || n > (double)high ||
        n != floor(n)) return false;
    *out = (int64_t)n;
    return true;
}

static bool field(JSContext *ctx, JSValueConst object, const char *name,
                  int64_t low, int64_t high, int64_t *out)
{
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    bool ok = !JS_IsException(value) && number(ctx, value, low, high, out);
    JS_FreeValue(ctx, value);
    return ok;
}

static bool array_length(JSContext *ctx, JSValueConst value, uint32_t *out)
{
    int64_t length;
    if (!JS_IsArray(value) ||
        !field(ctx, value, "length", 0, UINT32_MAX, &length)) return false;
    *out = (uint32_t)length;
    return true;
}

static bool at(JSContext *ctx, JSValueConst array, uint32_t index,
               int64_t low, int64_t high, int64_t *out)
{
    JSValue value = JS_GetPropertyUint32(ctx, array, index);
    bool ok = !JS_IsException(value) && number(ctx, value, low, high, out);
    JS_FreeValue(ctx, value);
    return ok;
}

static bool read_index(JSContext *ctx, JSValueConst source,
                       ksn_grid_index *out)
{
    uint32_t count;
    if (!array_length(ctx, source, &count) || count != 5) return false;
    for (unsigned i = 0; i < 5; ++i) {
        JSValue term = JS_GetPropertyUint32(ctx, source, i);
        uint32_t n;
        int64_t a, b, c;
        bool ok = !JS_IsException(term) &&
            array_length(ctx, term, &n) && n == 3 &&
            at(ctx, term, 0, INT32_MIN, INT32_MAX, &a) &&
            at(ctx, term, 1, INT16_MIN, INT16_MAX, &b) &&
            at(ctx, term, 2, 0, UINT8_MAX, &c);
        JS_FreeValue(ctx, term);
        if (!ok) return false;
        out->term[i] = (ksn_grid_coeff){(int32_t)a, (int16_t)b,
                                       (uint8_t)c};
    }
    return true;
}

static bool read_program(JSContext *ctx, JSValueConst spec,
                         ksn_grid_program *program, ksn_grid_shape *shape)
{
    int64_t count, reg, shift, initial;
    if (!JS_IsObject(spec) ||
        !field(ctx, spec, "count", 1, KSN_GRID_CODE, &count) ||
        !field(ctx, spec, "result_reg", 0, KSN_GRID_REGS - 1, &reg) ||
        !field(ctx, spec, "final_shift", 0, 30, &shift) ||
        !field(ctx, spec, "initial", INT32_MIN, INT32_MAX, &initial))
        return false;
    *program = (ksn_grid_program){0};
    program->count = (uint8_t)count;
    program->result_reg = (uint8_t)reg;
    program->final_shift = (uint8_t)shift;
    program->initial = initial;
    JSValue body = JS_GetPropertyStr(ctx, spec, "body");
    uint32_t length;
    bool ok = !JS_IsException(body) && array_length(ctx, body, &length) &&
              length == (uint32_t)count;
    for (unsigned i = 0; ok && i < length; ++i) {
        JSValue row = JS_GetPropertyUint32(ctx, body, i);
        ksn_grid_instruction *in = &program->body[i];
        int64_t op, dst, a, b, buffer, immediate;
        ok = !JS_IsException(row) &&
             field(ctx, row, "op", 0, UINT8_MAX, &op) &&
             field(ctx, row, "dst", 0, UINT8_MAX, &dst) &&
             field(ctx, row, "a", 0, UINT8_MAX, &a) &&
             field(ctx, row, "b", 0, UINT8_MAX, &b) &&
             field(ctx, row, "buffer", 0, UINT8_MAX, &buffer) &&
             field(ctx, row, "immediate", INT16_MIN, INT16_MAX, &immediate);
        if (ok) {
            in->op = (uint8_t)op; in->dst = (uint8_t)dst;
            in->a = (uint8_t)a; in->b = (uint8_t)b;
            in->buffer = (uint8_t)buffer;
            in->immediate = (int16_t)immediate;
            JSValue index = JS_GetPropertyStr(ctx, row, "index");
            ok = !JS_IsException(index) && read_index(ctx, index, &in->index);
            JS_FreeValue(ctx, index);
        }
        JS_FreeValue(ctx, row);
    }
    JS_FreeValue(ctx, body);
    if (!ok) return false;
    JSValue output = JS_GetPropertyStr(ctx, spec, "output");
    ok = !JS_IsException(output) && read_index(ctx, output, &program->output);
    JS_FreeValue(ctx, output);
    if (!ok) return false;
    JSValue dimensions = JS_GetPropertyStr(ctx, spec, "shape");
    ok = !JS_IsException(dimensions) &&
         array_length(ctx, dimensions, &length) && length == 4;
    int64_t sizes[4];
    for (unsigned i = 0; ok && i < 4; ++i)
        ok = at(ctx, dimensions, i, 1, UINT16_MAX, &sizes[i]);
    JS_FreeValue(ctx, dimensions);
    if (!ok) return false;
    *shape = (ksn_grid_shape){(uint16_t)sizes[0], (uint16_t)sizes[1],
                              (uint16_t)sizes[2], (uint16_t)sizes[3]};
    uint64_t pixels = (uint64_t)shape->width * shape->height;
    uint64_t work = pixels * shape->tap_width * shape->tap_height;
    return pixels <= GRID_APP_MAX_PIXELS && work <= 131072u;
}

static grid_slot *find(JSContext *ctx, JSValueConst value)
{
    int64_t handle;
    if (!number(ctx, value, 1, INT32_MAX, &handle)) return NULL;
    for (unsigned i = 0; i < GRID_APP_SLOTS; ++i)
        if (slots[i].handle == (uint32_t)handle) return &slots[i];
    return NULL;
}

static bool aligned_buffer(size_t count, void **raw, int16_t **aligned)
{
    *raw = malloc(count * sizeof(int16_t) + 15u);
    if (!*raw) return false;
    *aligned = (int16_t *)(((uintptr_t)*raw + 15u) & ~(uintptr_t)15u);
    return true;
}

static JSValue register_impl(JSContext *ctx, int argc, JSValueConst *argv)
{
    const char *op = "kasane.grid.register";
    if (argc != 1) return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT,
                               "expected a gridFold program");
    unsigned free_slot = 0;
    while (free_slot < GRID_APP_SLOTS && slots[free_slot].handle) ++free_slot;
    if (free_slot == GRID_APP_SLOTS || next_handle == INT32_MAX)
        return fail(ctx, op, POCKET_ERR_LIMIT_EXCEEDED, "grid plan limit");
    ksn_grid_program program;
    ksn_grid_shape shape;
    if (!read_program(ctx, argv[0], &program, &shape))
        return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT, "invalid grid program");
    grid_slot fresh = {0};
    fresh.plan = calloc(1, sizeof *fresh.plan);
    if (!fresh.plan) return fail(ctx, op, POCKET_ERR_OUT_OF_MEMORY,
                                 "grid plan allocation failed");
    if (ksn_grid_prepare(&program, fresh.plan) != KSN_GRID_OK) {
        free(fresh.plan);
        return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT, "invalid grid IR");
    }
    size_t count = (size_t)shape.width * shape.height;
    if (!aligned_buffer(count, &fresh.output_raw[0], &fresh.output[0]) ||
        !aligned_buffer(count, &fresh.output_raw[1], &fresh.output[1])) {
        free(fresh.output_raw[0]); free(fresh.output_raw[1]); free(fresh.plan);
        return fail(ctx, op, POCKET_ERR_OUT_OF_MEMORY,
                    "grid output allocation failed");
    }
    fresh.shape = shape;
    memset(fresh.output[0], 0, count * sizeof(int16_t));
    memset(fresh.output[1], 0, count * sizeof(int16_t));
    fresh.handle = ++next_handle;
    fresh.candidate_index = 1;
    slots[free_slot] = fresh;
    return JS_NewInt32(ctx, (int32_t)fresh.handle);
}

static JSValue register_resize_impl(JSContext *ctx, int argc,
                                    JSValueConst *argv)
{
    const char *op = "kasane.grid.registerResize";
    if (argc != 1 || !JS_IsObject(argv[0]))
        return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT,
                    "expected sourceWidth, sourceHeight, width, height");
    unsigned free_slot = 0;
    while (free_slot < GRID_APP_SLOTS && slots[free_slot].handle) ++free_slot;
    if (free_slot == GRID_APP_SLOTS || next_handle == INT32_MAX)
        return fail(ctx, op, POCKET_ERR_LIMIT_EXCEEDED, "grid plan limit");
    int64_t sw, sh, width, height;
    if (!field(ctx, argv[0], "sourceWidth", 1, 256, &sw) ||
        !field(ctx, argv[0], "sourceHeight", 1, 256, &sh) ||
        !field(ctx, argv[0], "width", 1, 256, &width) ||
        !field(ctx, argv[0], "height", 1, 256, &height))
        return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT,
                    "invalid resize dimensions");
    grid_slot fresh = {0};
    fresh.resize = calloc(1, sizeof *fresh.resize);
    if (!fresh.resize) return fail(ctx, op, POCKET_ERR_OUT_OF_MEMORY,
                                   "resize plan allocation failed");
    if (!ksn_grid_resize_prepare(fresh.resize, (unsigned)sw, (unsigned)sh,
                                 (unsigned)width, (unsigned)height)) {
        free(fresh.resize);
        return fail(ctx, op, POCKET_ERR_LIMIT_EXCEEDED,
                    "resize exceeds grid image limits");
    }
    size_t count = (size_t)width * (size_t)height;
    if (!aligned_buffer(count, &fresh.output_raw[0], &fresh.output[0]) ||
        !aligned_buffer(count, &fresh.output_raw[1], &fresh.output[1])) {
        free(fresh.output_raw[0]); free(fresh.output_raw[1]); free(fresh.resize);
        return fail(ctx, op, POCKET_ERR_OUT_OF_MEMORY,
                    "resize output allocation failed");
    }
    memset(fresh.output[0], 0, count * sizeof(int16_t));
    memset(fresh.output[1], 0, count * sizeof(int16_t));
    fresh.shape = (ksn_grid_shape){(uint16_t)width, (uint16_t)height, 1, 1};
    fresh.handle = ++next_handle;
    fresh.candidate_index = 1;
    slots[free_slot] = fresh;
    return JS_NewInt32(ctx, (int32_t)fresh.handle);
}

static JSValue register_resize_source_impl(JSContext *ctx, int argc,
                                           JSValueConst *argv)
{
    const char *op = "kasane.grid.registerResizeSource";
    if (argc != 1 || !JS_IsObject(argv[0]))
        return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT,
                    "expected source image, width, height");
    unsigned free_slot = 0;
    while (free_slot < GRID_APP_SLOTS && slots[free_slot].handle) ++free_slot;
    if (free_slot == GRID_APP_SLOTS || next_handle == INT32_MAX)
        return fail(ctx, op, POCKET_ERR_LIMIT_EXCEEDED, "grid plan limit");
    int64_t width, height;
    JSValue source = JS_GetPropertyStr(ctx, argv[0], "source");
    grid_slot fresh = {0};
    fresh.stream = calloc(1, sizeof *fresh.stream);
    bool valid = !JS_IsException(source) && fresh.stream &&
        field(ctx, argv[0], "width", 1, 256, &width) &&
        field(ctx, argv[0], "height", 1, 256, &height) &&
        pocket_kasane_grid_source_port(ctx, source, &fresh.stream->source,
                                       &fresh.stream->source_id);
    JS_FreeValue(ctx, source);
    if (!fresh.stream) return fail(ctx, op, POCKET_ERR_OUT_OF_MEMORY,
                                   "source row allocation failed");
    if (!valid || !fresh.stream->source.opaque ||
        fresh.stream->source.variants != 1 ||
        fresh.stream->source.frames != 1 ||
        fresh.stream->source.width > 256 ||
        fresh.stream->source.height > 256) {
        free(fresh.stream);
        return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT,
                    "source must be an opaque, single-frame Kasane image");
    }
    JSValue sampling = JS_GetPropertyStr(ctx, argv[0], "sampling");
    if (JS_IsException(sampling)) {
        free(fresh.stream);
        return JS_EXCEPTION;
    }
    if (!JS_IsUndefined(sampling)) {
        const char *name = JS_IsString(sampling) ?
            JS_ToCString(ctx, sampling) : NULL;
        bool nearest = name && !strcmp(name, "nearest");
        bool bilinear = name && !strcmp(name, "bilinear");
        if (name) JS_FreeCString(ctx, name);
        if (!nearest && !bilinear) {
            JS_FreeValue(ctx, sampling);
            free(fresh.stream);
            return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT,
                        "sampling must be nearest or bilinear");
        }
        fresh.resize_nearest = nearest;
    }
    JS_FreeValue(ctx, sampling);
    fresh.resize = calloc(1, sizeof *fresh.resize);
    if (!fresh.resize) { free(fresh.stream); return fail(ctx, op,
        POCKET_ERR_OUT_OF_MEMORY, "resize plan allocation failed"); }
    if (!ksn_grid_resize_prepare_stream(fresh.resize,
            fresh.stream->source.width, fresh.stream->source.height,
            (unsigned)width, (unsigned)height)) {
        free(fresh.resize); free(fresh.stream);
        return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT,
                    "invalid resize dimensions");
    }
    fresh.shape = (ksn_grid_shape){(uint16_t)width, (uint16_t)height, 1, 1};
    fresh.handle = ++next_handle;
    fresh.has_output = true;
    fresh.resize_pie = !fresh.resize_nearest &&
        ksn_grid_resize_pie_available() && width >= 8 &&
        (fresh.resize->source_width != (unsigned)width ||
         fresh.resize->source_height != (unsigned)height);
    slots[free_slot] = fresh;
    return JS_NewInt32(ctx, (int32_t)fresh.handle);
}

static JSValue run_impl(JSContext *ctx, int argc, JSValueConst *argv)
{
    const char *op = "kasane.grid.run";
    grid_slot *slot = argc ? find(ctx, argv[0]) : NULL;
    if (argc < 2 || argc > 3 || !slot || !JS_IsObject(argv[1]))
        return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT,
                    "expected handle and input buffer map");
    if (slot->stream) return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT,
                                  "source-backed resize runs during image reads");
    if (slot->pending) return fail(ctx, op, POCKET_ERR_BUSY,
                                   "previous grid image awaits presentation");
    ksn_grid_binding binding = {0};
    size_t total = 0;
    bool valid = true, oom = false;
    for (unsigned i = 0; i < KSN_GRID_BUFFERS && valid; ++i) {
        if (i == KSN_GRID_DEST) continue;
        JSValue input = JS_GetPropertyUint32(ctx, argv[1], i);
        if (JS_IsException(input)) { valid = false; JS_FreeValue(ctx, input); break; }
        if (!JS_IsUndefined(input) && !JS_IsNull(input)) {
            if (JS_GetTypedArrayType(input) != JS_TYPED_ARRAY_INT16) valid = false;
            else {
                size_t offset = 0, bytes = 0, per = 0, size = 0;
                JSValue buffer = JS_GetTypedArrayBuffer(ctx, input,
                                                        &offset, &bytes, &per);
                uint8_t *data = JS_IsException(buffer) ? NULL :
                                JS_GetArrayBuffer(ctx, &size, buffer);
                size_t count = bytes / sizeof(int16_t);
                if (!data || per != sizeof(int16_t) || bytes % 2 ||
                    offset > size || bytes > size - offset ||
                    count > GRID_APP_MAX_INPUTS - total) valid = false;
                else {
                    if (count > slot->input_capacity[i]) {
                        void *new_raw = NULL;
                        int16_t *new_aligned = NULL;
                        if (!aligned_buffer(count, &new_raw, &new_aligned)) oom = true;
                        else {
                            free(slot->input_raw[i]);
                            slot->input_raw[i] = new_raw;
                            slot->input[i] = new_aligned;
                            slot->input_capacity[i] = count;
                        }
                    }
                    binding.data[i] = slot->input[i];
                    if (oom) { JS_FreeValue(ctx, buffer); JS_FreeValue(ctx, input); break; }
                    if (count) memcpy(binding.data[i], data + offset, bytes);
                    binding.count[i] = count;
                    total += count;
                }
                JS_FreeValue(ctx, buffer);
            }
        }
        JS_FreeValue(ctx, input);
        if (oom) break;
    }
    if (valid && !oom && argc == 3 && !JS_IsUndefined(argv[2])) {
        uint32_t count;
        valid = array_length(ctx, argv[2], &count) && count <= KSN_GRID_PARAMS;
        for (unsigned i = 0; valid && i < count; ++i) {
            int64_t value;
            valid = at(ctx, argv[2], i, INT32_MIN, INT32_MAX, &value);
            if (valid) binding.param[i] = (int32_t)value;
        }
    }
    binding.data[KSN_GRID_DEST] = slot->output[slot->candidate_index];
    binding.count[KSN_GRID_DEST] = (size_t)slot->shape.width * slot->shape.height;
    if (valid && !oom)
        memcpy(binding.data[KSN_GRID_DEST],
               slot->output[slot->committed_index],
               binding.count[KSN_GRID_DEST] * sizeof(int16_t));
    ksn_grid_image next = {0};
    ksn_grid_status status = KSN_GRID_BAD_IR;
    if (valid && !oom) {
        if (slot->resize) {
            bool only_source = argc < 3 || JS_IsUndefined(argv[2]);
            for (unsigned i = 2; i < KSN_GRID_BUFFERS; ++i)
                if (binding.count[i]) only_source = false;
            if (only_source && ksn_grid_resize_run(slot->resize,
                    binding.data[0], binding.count[0],
                    binding.data[KSN_GRID_DEST], binding.count[KSN_GRID_DEST],
                    true, &slot->resize_pie)) status = KSN_GRID_OK;
        } else {
            const ksn_grid_pie_policy policy = {true, 8, 8, 8};
            status = ksn_grid_image_bind(&next, slot->plan, &slot->shape,
                                         &binding, policy);
            if (status == KSN_GRID_OK) status = ksn_grid_image_run(&next);
        }
    }
    if (oom) return fail(ctx, op, POCKET_ERR_OUT_OF_MEMORY,
                         "grid input allocation failed");
    if (!valid || status != KSN_GRID_OK)
        return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT,
                    "grid bind or execution failed");
    /* Image spans only read DEST. Do not retain pointers to released inputs. */
    for (unsigned i = 0; i < KSN_GRID_BUFFERS; ++i)
        if (i != KSN_GRID_DEST) next.execution.binding.data[i] = NULL;
    if (!slot->resize) slot->image = next;
    if (slot->resource) {
        slot->pending = true;
        pocket_kasane_grid_invalidate((unsigned)(slot - slots));
    } else {
        slot->has_output = true;
        uint8_t old = slot->committed_index;
        slot->committed_index = slot->candidate_index;
        slot->candidate_index = old;
    }
    return JS_NewString(ctx, (slot->resize ? slot->resize_pie :
                         next.execution.pie_backend_selected) ? "PIE" : "scalar");
}

static ksn_result resize_span(void *ctx, uint16_t variant, uint16_t frame,
                              uint16_t y, uint16_t x, uint16_t count,
                              uint16_t *rgb565, uint8_t *alpha)
{
    const grid_slot *slot = ctx;
    if (!slot || !slot->resize || variant || frame ||
        y >= slot->shape.height || x > slot->shape.width ||
        count > slot->shape.width - x ||
        (count && (!rgb565 || !alpha))) return KSN_INVALID;
    if (count && slot->stream) {
        resize_stream *stream = slot->stream;
        if (slot->resize_nearest) {
            unsigned sw=slot->resize->source_width;
            unsigned sh=slot->resize->source_height;
            unsigned sy=((unsigned)y*sh+sh/2u)/slot->shape.height;
            if (sy>=sh) sy=sh-1;
            unsigned first=((unsigned)x*sw+sw/2u)/slot->shape.width;
            unsigned last=((unsigned)(x+count-1u)*sw+sw/2u)/
                          slot->shape.width;
            if (first>=sw) first=sw-1;
            if (last>=sw) last=sw-1;
            uint32_t source_start=GRID_RESIZE_START();
            ksn_result result=stream->source.read_span(stream->source.ctx,
                0,0,(uint16_t)sy,(uint16_t)first,
                (uint16_t)(last-first+1u),stream->rows[0],stream->alpha);
            GRID_RESIZE_END(source_cycles,source_reads,source_start);
            if (result!=KSN_OK)return result;
            for(unsigned i=0;i<count;i++){
                unsigned sx=((x+i)*sw+sw/2u)/slot->shape.width;
                if(sx>=sw)sx=sw-1;
                rgb565[i]=stream->rows[0][sx-first];
            }
            memset(alpha,255,count);
            return KSN_OK;
        }
        unsigned sy = slot->resize->y0[y];
        unsigned sy1 = sy + (sy + 1u < slot->resize->source_height);
        if (!stream->cached || stream->cached_y != sy) {
            /* A failed provider read must not leave a partially replaced pair
             * advertised under the previous row key. */
            stream->cached = false;
            uint32_t source_start = GRID_RESIZE_START();
            ksn_result result = stream->source.read_span(stream->source.ctx,
                0, 0, (uint16_t)sy, 0, slot->resize->source_width,
                stream->rows[0], stream->alpha);
            GRID_RESIZE_END(source_cycles, source_reads, source_start);
            if (result != KSN_OK) return result;
            if (sy1 == sy)
                memcpy(stream->rows[1], stream->rows[0],
                       (size_t)slot->resize->source_width * sizeof(uint16_t));
            else {
                source_start = GRID_RESIZE_START();
                result = stream->source.read_span(stream->source.ctx,
                    0, 0, (uint16_t)sy1, 0, slot->resize->source_width,
                    stream->rows[1], stream->alpha);
                GRID_RESIZE_END(source_cycles, source_reads, source_start);
                if (result != KSN_OK) return result;
            }
            stream->cached_y = (uint16_t)sy;
            stream->cached = true;
        }
        uint32_t kernel_start = GRID_RESIZE_START();
        bool resized = ksn_grid_resize_span(slot->resize, y, x, count,
                stream->rows[0], stream->rows[1], rgb565, true, NULL);
        GRID_RESIZE_END(kernel_cycles, kernel_spans, kernel_start);
        if (!resized)
            return KSN_INVALID;
        memset(alpha, 255, count);
    } else if (count) {
        const int16_t *pixels = slot->output[slot->pending ?
            slot->candidate_index : slot->committed_index];
        memcpy(rgb565, pixels + (size_t)y * slot->shape.width + x,
               (size_t)count * sizeof *rgb565);
        memset(alpha, 255, count);
    }
    return KSN_OK;
}

static JSValue resource_impl(JSContext *ctx, int argc, JSValueConst *argv)
{
    const char *op = "kasane.grid.resource";
    grid_slot *slot = argc ? find(ctx, argv[0]) : NULL;
    if (argc != 1 || !slot || !slot->has_output || slot->pending)
        return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT,
                    "run grid before creating its image resource");
    ksn_image_port port;
    if (slot->resize) {
        port = (ksn_image_port){.ctx=slot, .width=slot->shape.width,
            .height=slot->shape.height, .variants=1, .frames=1,
            .read_span=resize_span, .opaque=true};
    } else if (!ksn_grid_image_port(&slot->image, &port))
        return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT, "grid image unavailable");
    JSValue object = pocket_kasane_grid_resource(ctx, (unsigned)(slot - slots),
                                                  &port);
    if (!JS_IsException(object)) slot->resource = true;
    return object;
}

static JSValue explain_impl(JSContext *ctx, int argc, JSValueConst *argv)
{
    const char *op = "kasane.grid.explain";
    grid_slot *slot = argc ? find(ctx, argv[0]) : NULL;
    if (argc != 1 || !slot || !slot->has_output)
        return fail(ctx, op, POCKET_ERR_INVALID_ARGUMENT,
                    "run grid before inspecting its route");
    if (slot->resize) {
        JSValue info = JS_NewObject(ctx);
        if (JS_IsException(info)) return info;
        if (JS_SetPropertyStr(ctx, info, "backend", JS_NewString(ctx,
                slot->resize_pie ? "PIE" : "scalar")) < 0 ||
            JS_SetPropertyStr(ctx, info, "strategy",
                              JS_NewString(ctx, slot->resize_nearest ?
                                           "NEAREST" : "BILINEAR")) < 0 ||
            JS_SetPropertyStr(ctx, info, "reason",
                              JS_NewString(ctx, slot->resize_nearest ?
                                           "EXPLICIT" : "EXPERIMENT")) < 0 ||
            JS_SetPropertyStr(ctx, info, "profileKey",
                              JS_NewString(ctx, "0000000000000000")) < 0) {
            JS_FreeValue(ctx, info); return JS_EXCEPTION;
        }
        return info;
    }
    const ksn_grid_execution *execution = &slot->image.execution;
    static const char *const strategies[] = {"AUTO", "GATHER", "AFFINE", "FUSED"};
    static const char *const reasons[] = {"NONE", "FALLBACK", "PROFILE", "FORCED"};
    unsigned strategy = execution->selected_strategy;
    unsigned reason = execution->selection_reason;
    if (strategy >= sizeof strategies / sizeof strategies[0] ||
        reason >= sizeof reasons / sizeof reasons[0]) return JS_EXCEPTION;
    char key[17];
    snprintf(key, sizeof key, "%016llx",
             (unsigned long long)execution->profile_key);
    JSValue info = JS_NewObject(ctx);
    if (JS_IsException(info)) return info;
    if (JS_SetPropertyStr(ctx, info, "backend", JS_NewString(ctx,
            execution->pie_backend_selected ? "PIE" : "scalar")) < 0 ||
        JS_SetPropertyStr(ctx, info, "strategy",
                          JS_NewString(ctx, strategies[strategy])) < 0 ||
        JS_SetPropertyStr(ctx, info, "reason",
                          JS_NewString(ctx, reasons[reason])) < 0 ||
        JS_SetPropertyStr(ctx, info, "profileKey", JS_NewString(ctx, key)) < 0) {
        JS_FreeValue(ctx, info);
        return JS_EXCEPTION;
    }
    return info;
}

typedef JSValue (*grid_method)(JSContext *, int, JSValueConst *);
static JSValue guarded(JSContext *ctx, int argc, JSValueConst *argv,
                       grid_method method)
{
    if (js_call_active) return fail(ctx, "kasane.grid", POCKET_ERR_BUSY,
                                    "reentrant grid call");
    js_call_active = true;
    JSValue result = method(ctx, argc, argv);
    js_call_active = false;
    return result;
}
static JSValue js_register(JSContext *ctx, JSValueConst self, int argc,
                           JSValueConst *argv)
{ (void)self; return guarded(ctx, argc, argv, register_impl); }
static JSValue js_register_resize(JSContext *ctx, JSValueConst self, int argc,
                                  JSValueConst *argv)
{ (void)self; return guarded(ctx, argc, argv, register_resize_impl); }
static JSValue js_register_resize_source(JSContext *ctx, JSValueConst self,
                                         int argc, JSValueConst *argv)
{ (void)self; return guarded(ctx, argc, argv, register_resize_source_impl); }
static JSValue js_run(JSContext *ctx, JSValueConst self, int argc,
                      JSValueConst *argv)
{ (void)self; return guarded(ctx, argc, argv, run_impl); }
static JSValue js_resource(JSContext *ctx, JSValueConst self, int argc,
                           JSValueConst *argv)
{ (void)self; return guarded(ctx, argc, argv, resource_impl); }
static JSValue js_explain(JSContext *ctx, JSValueConst self, int argc,
                          JSValueConst *argv)
{ (void)self; return guarded(ctx, argc, argv, explain_impl); }

/* Ordinary apps should not pay the symbolic frontend's parse cost unless
 * they use it. These three small proxies load the same embedded JS on first
 * use, then delegate to the frontend object it installs. */
static JSValue lazy_frontend(JSContext *ctx, const char *name,
                             int argc, JSValueConst *argv)
{
    if (!frontend_loaded) {
#ifdef KSN_GRID_APP_HOST_TEST
        const char *source = pocket_grid_test_frontend;
        size_t length = source ? strlen(source) : 0;
#else
        const char *source = grid_fold_start;
        size_t length = (size_t)(grid_fold_end - grid_fold_start - 1);
#endif
        JSValue loaded = JS_Eval(ctx, source, length,
                                 "grid_fold.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(loaded)) return loaded;
        JS_FreeValue(ctx, loaded);
        frontend_loaded = true;
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue frontend = JS_GetPropertyStr(ctx, global, "gridFold");
    JS_FreeValue(ctx, global);
    if (JS_IsException(frontend)) return frontend;
    JSValue method = JS_GetPropertyStr(ctx, frontend, name);
    if (JS_IsException(method)) { JS_FreeValue(ctx, frontend); return method; }
    JSValue result = JS_Call(ctx, method, frontend, argc, argv);
    JS_FreeValue(ctx, method);
    JS_FreeValue(ctx, frontend);
    return result;
}
static JSValue js_fold_lazy(JSContext *ctx, JSValueConst self, int argc,
                            JSValueConst *argv)
{ (void)self; return lazy_frontend(ctx, "fold", argc, argv); }
static JSValue js_index_lazy(JSContext *ctx, JSValueConst self, int argc,
                             JSValueConst *argv)
{ (void)self; return lazy_frontend(ctx, "index", argc, argv); }
static JSValue js_view_lazy(JSContext *ctx, JSValueConst self, int argc,
                            JSValueConst *argv)
{ (void)self; return lazy_frontend(ctx, "view", argc, argv); }

esp_err_t pocket_grid_install(JSContext *ctx, JSValueConst ns)
{
    JSValue grid = JS_NewObject(ctx);
    if (JS_IsException(grid)) return ESP_ERR_NO_MEM;
    static const JSCFunctionListEntry methods[] = {
        JS_CFUNC_DEF("register", 1, js_register),
        JS_CFUNC_DEF("registerResize", 1, js_register_resize),
        JS_CFUNC_DEF("registerResizeSource", 1, js_register_resize_source),
        JS_CFUNC_DEF("run", 3, js_run),
        JS_CFUNC_DEF("resource", 1, js_resource),
        JS_CFUNC_DEF("explain", 1, js_explain),
    };
    if (JS_SetPropertyFunctionList(ctx, grid, methods, 6) < 0) {
        JS_FreeValue(ctx, grid); return ESP_ERR_NO_MEM;
    }
    if (JS_SetPropertyStr(ctx, ns, "grid", grid) < 0) return ESP_ERR_NO_MEM;
    frontend_loaded = false;
    JSValue lazy = JS_NewObject(ctx);
    if (JS_IsException(lazy)) return ESP_ERR_NO_MEM;
    static const JSCFunctionListEntry frontend_methods[] = {
        JS_CFUNC_DEF("fold", 2, js_fold_lazy),
        JS_CFUNC_DEF("index", 5, js_index_lazy),
        JS_CFUNC_DEF("view", 2, js_view_lazy),
    };
    if (JS_SetPropertyFunctionList(ctx, lazy, frontend_methods, 3) < 0) {
        JS_FreeValue(ctx, lazy); return ESP_ERR_NO_MEM;
    }
    JSValue global = JS_GetGlobalObject(ctx);
    if (JS_IsException(global)) { JS_FreeValue(ctx, lazy); return ESP_ERR_NO_MEM; }
    int result = JS_SetPropertyStr(ctx, global, "gridFold", lazy);
    JS_FreeValue(ctx, global);
    if (result < 0) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void pocket_grid_reset(void)
{
    for (unsigned i = 0; i < GRID_APP_SLOTS; ++i) {
        free(slots[i].plan);
        free(slots[i].resize);
        free(slots[i].stream);
        free(slots[i].output_raw[0]);
        free(slots[i].output_raw[1]);
        for (unsigned j = 0; j < KSN_GRID_BUFFERS; ++j)
            free(slots[i].input_raw[j]);
        slots[i] = (grid_slot){0};
    }
    js_call_active = false;
    frontend_loaded = false;
}
void pocket_grid_source_invalidated(uint32_t resource_id)
{
    if (!resource_id) return;
    for (unsigned i = 0; i < GRID_APP_SLOTS; ++i) {
        grid_slot *slot = &slots[i];
        if (!slot->stream || slot->stream->source_id != resource_id) continue;
        slot->stream->cached = false;
        if (slot->resource) pocket_kasane_grid_invalidate(i);
    }
}
bool pocket_grid_pending(void)
{
    for (unsigned i = 0; i < GRID_APP_SLOTS; ++i)
        if (slots[i].pending) return true;
    return false;
}
void pocket_grid_present_result(ksn_result result)
{
    for (unsigned i = 0; i < GRID_APP_SLOTS; ++i) {
        grid_slot *slot = &slots[i];
        if (!slot->pending) continue;
        if (result == KSN_OK) {
            uint8_t old = slot->committed_index;
            slot->committed_index = slot->candidate_index;
            slot->candidate_index = old;
            slot->has_output = true;
            slot->pending = false;
        }
        /* On I/O failure the same candidate remains visible for repair. */
    }
}
