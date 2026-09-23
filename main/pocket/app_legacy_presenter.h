#ifndef APP_LEGACY_PRESENTER_H
#define APP_LEGACY_PRESENTER_H
#include "ksn_view.h"

/* A small, flat native presenter. The plans contain only visible commands;
 * changing a page never leaves invisible commands in the renderer's band walk. */
#define KSN_PRESENTER_TEXT_MAX 47u
#define KSN_PRESENTER_ITEMS 12u
#define KSN_PRESENTER_SLOTS 2u
typedef enum { KSN_PRESENTER_MUSIC } ksn_presenter_kind;
typedef struct {
    ksn_rect bounds;
    ksn_rgba color;
    uint8_t kind;
    uint8_t text_id;    /* 0..1: values; 16+: flash constants */
    uint8_t font;
    uint8_t radius;
    uint8_t variant,frame,reveal;
} ksn_presenter_item;
typedef struct {
    ksn_presenter_item items[KSN_PRESENTER_ITEMS];
    char text[KSN_PRESENTER_SLOTS][KSN_PRESENTER_TEXT_MAX+1u];
    uint8_t bytes[KSN_PRESENTER_SLOTS];
    ksn_rgba background;
    uint8_t count;
    bool patchable;
} ksn_presenter_plan;
typedef struct {
    char text[KSN_PRESENTER_SLOTS][KSN_PRESENTER_TEXT_MAX+1u];
    uint8_t bytes[KSN_PRESENTER_SLOTS];
    uint32_t position_ms,duration_ms,phase;
    bool playing,help;
} ksn_presenter_values;

/* Truncation is scalar-safe and never stores a partial UTF-8 sequence. */
ksn_result ksn_presenter_copy_text(char out[KSN_PRESENTER_TEXT_MAX+1u],
                                   uint8_t *out_bytes,const char *src,size_t bytes);
ksn_result ksn_presenter_make(ksn_presenter_kind kind,const ksn_presenter_values *values,
                              uint16_t width,uint16_t height,ksn_presenter_plan *out);
bool ksn_presenter_equal(const ksn_presenter_plan *a,const ksn_presenter_plan *b);
ksn_result ksn_presenter_submit(ksn_view *view,ksn_rect viewport,ksn_resource image,
                                const ksn_presenter_plan *plan,
                                ksn_ref refs[KSN_PRESENTER_ITEMS],ksn_tx *out);
ksn_result ksn_presenter_patch(ksn_view *view,ksn_rect viewport,
                               const ksn_presenter_plan *plan,
                               const ksn_ref refs[KSN_PRESENTER_ITEMS],ksn_tx *out);
#endif
