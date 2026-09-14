#ifndef DS_COMPOSITION_TYPES_H
#define DS_COMPOSITION_TYPES_H
#include "ds_types.h"
/* Value types shared by the public endpoint and host implementation. */
typedef enum { DS_SUBMISSION_NONE, DS_SUBMITTED, DS_PRESENTED, DS_DISCARDED } ds_submission_status;
typedef struct { ds_tx ticket; ds_submission_status status; ds_result reason; ds_layer layer; } ds_submission;
typedef struct { uint32_t value; } ds_template;
typedef struct { uint32_t value; } ds_instance;
typedef struct { int16_t x,y; ds_rect clip; uint8_t opacity; bool visible; } ds_placement;
typedef struct {
    uint16_t commands,text_bytes;
    uint8_t templates,instances;
    uint32_t native_bytes;
} ds_cache_stats;
typedef enum { DS_MODAL_CLOSED, DS_MODAL_PREPARING, DS_MODAL_OPEN, DS_MODAL_CLOSING } ds_modal_phase;
typedef enum { DS_MODAL_SOLID, DS_MODAL_DIM_LIVE } ds_modal_backdrop;
typedef enum { DS_INPUT_BLOCKED, DS_INPUT_APP, DS_INPUT_MODAL, DS_INPUT_HOST } ds_input_scope;
#endif
