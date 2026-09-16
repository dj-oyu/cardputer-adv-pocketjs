#ifndef KSN_COMPOSITION_TYPES_H
#define KSN_COMPOSITION_TYPES_H
#include "ksn_types.h"
/* Value types shared by the public endpoint and host implementation. */
typedef enum { KSN_SUBMISSION_NONE, KSN_SUBMITTED, KSN_PRESENTED, KSN_DISCARDED } ksn_submission_status;
typedef struct { ksn_tx ticket; ksn_submission_status status; ksn_result reason; ksn_layer layer; } ksn_submission;
typedef struct { uint32_t value; } ksn_template;
typedef struct { uint32_t value; } ksn_instance;
typedef struct { int16_t x,y; ksn_rect clip; uint8_t opacity; bool visible; } ksn_placement;
typedef struct {
    uint16_t commands,text_bytes;
    uint8_t templates,instances;
    uint32_t native_bytes;
} ksn_cache_stats;
typedef enum { KSN_MODAL_CLOSED, KSN_MODAL_PREPARING, KSN_MODAL_OPEN, KSN_MODAL_CLOSING } ksn_modal_phase;
typedef enum { KSN_MODAL_SOLID, KSN_MODAL_DIM_LIVE } ksn_modal_backdrop;
typedef enum { KSN_INPUT_BLOCKED, KSN_INPUT_APP, KSN_INPUT_MODAL, KSN_INPUT_HOST } ksn_input_scope;
#endif
