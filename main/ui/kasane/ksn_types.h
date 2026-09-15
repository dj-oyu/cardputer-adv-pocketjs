#ifndef KSN_TYPES_H
#define KSN_TYPES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum { KSN_OK, KSN_INVALID, KSN_LIMIT, KSN_OOM, KSN_STALE,
               KSN_BUSY, KSN_UNSUPPORTED, KSN_IO, KSN_CANCELLED } ksn_result;
typedef struct { int16_t x0,y0,x1,y1; } ksn_rect;
typedef uint32_t ksn_rgba;
typedef struct { uint32_t value; } ksn_ref;
typedef struct { uint32_t value; } ksn_tx;
typedef struct { uint32_t value; } ksn_resource;
typedef struct { uint32_t value; } ksn_animation;
typedef enum { KSN_APP, KSN_SYSTEM } ksn_layer;
typedef enum { KSN_REPLACE, KSN_PATCH } ksn_update_mode;
typedef enum { KSN_RECT, KSN_ROUND_RECT, KSN_STROKE, KSN_GRADIENT, KSN_TEXT, KSN_IMAGE } ksn_kind;
typedef enum { KSN_CAPTION, KSN_BODY, KSN_DISPLAY } ksn_font;
/* Crop origin in source pixels; its extent follows bounds and scale. 2X
 * requires even destination extents. HALF samples source pixel centers. */
typedef enum { KSN_IMAGE_1X, KSN_IMAGE_2X, KSN_IMAGE_HALF, KSN_IMAGE_STRETCH } ksn_image_scale;
/* Descriptors and strings are borrowed for the call only. Colors: RRGGBBAA. */
typedef struct {
    ksn_kind kind;
    ksn_rect bounds,clip;
    uint8_t opacity;
    union {
        struct { ksn_rgba color; uint8_t radius,width; } shape;
        struct { ksn_rgba from,to; uint8_t axis,radius; bool dither; } gradient;
        struct { const char *utf8; uint16_t bytes,capacity; ksn_font font; ksn_rgba color; } text;
        struct { ksn_resource resource; uint16_t variant,frame,source_x,source_y; ksn_image_scale scale;
                 uint16_t source_width,source_height,rotation; } image;
    } data;
} ksn_draw;
typedef enum { KSN_SET_RECT, KSN_SET_CLIP, KSN_SET_COLOR, KSN_SET_TEXT,
               KSN_SET_REVEAL, KSN_SET_VISIBLE, KSN_SET_IMAGE_FRAME, KSN_SET_ROTATION } ksn_property;
typedef struct {
    ksn_property property;
    union {
        ksn_rect rect; ksn_rgba color; uint16_t reveal,rotation; bool visible;
        struct { const char *utf8; uint16_t bytes; } text;
        struct { uint16_t variant,frame; } image;
    } value;
} ksn_change;
typedef enum { KSN_TRANSLATE, KSN_OPACITY, KSN_COLOR, KSN_REVEAL } ksn_motion_property;
typedef enum { KSN_LINEAR, KSN_EASE_OUT_CUBIC, KSN_EASE_IN_OUT_CUBIC, KSN_STEP } ksn_easing;
typedef enum { KSN_ONCE, KSN_LOOP, KSN_PINGPONG } ksn_repeat;
typedef union { struct { int16_t x,y; } offset; ksn_rgba color; uint16_t scalar; } ksn_motion_value;
typedef struct {
    ksn_ref first; uint16_t count; ksn_motion_property property;
    ksn_motion_value from,to; uint32_t duration_ms; ksn_easing easing; ksn_repeat repeat;
} ksn_motion;
typedef struct { uint16_t commands,text_bytes; uint8_t tracks; } ksn_capacity;
typedef struct { ksn_capacity app,system; uint32_t native_bytes,backdrop_bytes; } ksn_limits;
typedef struct { ksn_capacity used[2]; uint32_t native_current,native_peak,dirty_bands,transferred_bytes; } ksn_stats;
/* Internal storage candidate; public descriptors need not fit in 32 bytes. */
typedef struct {
    uint8_t kind,flags,opacity,reserved;
    ksn_rect bounds,clip;
    uint32_t payload[3];
} ksn_command_storage;
#ifdef __cplusplus
static_assert(sizeof(ksn_command_storage)==32,"command budget");
#else
_Static_assert(sizeof(ksn_command_storage)==32,"command budget");
#endif
#endif
