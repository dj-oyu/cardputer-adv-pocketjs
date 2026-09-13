#ifndef DS_TYPES_H
#define DS_TYPES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum { DS_OK, DS_INVALID, DS_LIMIT, DS_OOM, DS_STALE,
               DS_BUSY, DS_UNSUPPORTED, DS_IO } ds_result;
typedef struct { int16_t x0,y0,x1,y1; } ds_rect;
typedef uint32_t ds_rgba;
typedef struct { uint32_t value; } ds_ref;
typedef struct { uint32_t value; } ds_tx;
typedef struct { uint32_t value; } ds_resource;
typedef struct { uint32_t value; } ds_animation;
typedef enum { DS_APP, DS_SYSTEM } ds_layer;
typedef enum { DS_REPLACE, DS_PATCH } ds_update_mode;
typedef enum { DS_RECT, DS_ROUND_RECT, DS_STROKE, DS_GRADIENT, DS_TEXT, DS_IMAGE } ds_kind;
typedef enum { DS_CAPTION, DS_BODY, DS_DISPLAY } ds_font;
/* Descriptors and strings are borrowed for the call only. Colors: RRGGBBAA. */
typedef struct {
    ds_kind kind;
    ds_rect bounds,clip;
    uint8_t opacity;
    union {
        struct { ds_rgba color; uint8_t radius,width; } shape;
        struct { ds_rgba from,to; uint8_t axis,radius; bool dither; } gradient;
        struct { const char *utf8; uint16_t bytes,capacity; ds_font font; ds_rgba color; } text;
        struct { ds_resource resource; uint16_t variant,frame; } image;
    } data;
} ds_draw;
typedef enum { DS_SET_RECT, DS_SET_CLIP, DS_SET_COLOR, DS_SET_TEXT,
               DS_SET_REVEAL, DS_SET_VISIBLE, DS_SET_IMAGE_FRAME } ds_property;
typedef struct {
    ds_property property;
    union {
        ds_rect rect; ds_rgba color; uint16_t reveal; bool visible;
        struct { const char *utf8; uint16_t bytes; } text;
        struct { uint16_t variant,frame; } image;
    } value;
} ds_change;
typedef enum { DS_TRANSLATE, DS_OPACITY, DS_COLOR, DS_REVEAL } ds_motion_property;
typedef enum { DS_LINEAR, DS_EASE_OUT_CUBIC, DS_EASE_IN_OUT_CUBIC, DS_STEP } ds_easing;
typedef enum { DS_ONCE, DS_LOOP, DS_PINGPONG } ds_repeat;
typedef union { struct { int16_t x,y; } offset; ds_rgba color; uint16_t scalar; } ds_motion_value;
typedef struct {
    ds_ref first; uint16_t count; ds_motion_property property;
    ds_motion_value from,to; uint32_t duration_ms; ds_easing easing; ds_repeat repeat;
} ds_motion;
typedef struct { uint16_t commands,text_bytes; uint8_t tracks; } ds_capacity;
typedef struct { ds_capacity app,system; uint32_t native_bytes,backdrop_bytes; } ds_limits;
typedef struct { ds_capacity used[2]; uint32_t native_current,native_peak,dirty_bands,transferred_bytes; } ds_stats;
/* Internal storage candidate; public descriptors need not fit in 32 bytes. */
typedef struct {
    uint8_t kind,flags,opacity,reserved;
    ds_rect bounds,clip;
    uint32_t payload[3];
} ds_command_storage;
#ifdef __cplusplus
static_assert(sizeof(ds_command_storage)==32,"command budget");
#else
_Static_assert(sizeof(ds_command_storage)==32,"command budget");
#endif
#endif
