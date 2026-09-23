#ifndef KSN_SCHEMA_H
#define KSN_SCHEMA_H
#include "ksn_view.h"

/* Immutable, app-independent description. Static definitions may live in
 * flash; runtime definitions use the same validated representation. Values
 * are borrowed for this owner turn only and are copied by the core on submit. */
#define KSN_SCHEMA_MAX_SLOTS 24u
#define KSN_SCHEMA_MAX_NODES 24u
#define KSN_SCHEMA_TEXT_MAX 47u
#define KSN_SCHEMA_LITERAL UINT8_MAX
typedef enum {
    KSN_SLOT_TEXT, KSN_SLOT_RECT, KSN_SLOT_COLOR, KSN_SLOT_BOOL,
    KSN_SLOT_U16, KSN_SLOT_RESOURCE
} ksn_slot_type;
typedef struct {
    const char *name;
    ksn_slot_type type;
    uint8_t capacity; /* TEXT only; zero for all other types. */
    uint16_t initial_number,maximum; /* U16; maximum 0 means UINT16_MAX. */
} ksn_schema_slot;
typedef struct {
    const char *utf8;
    uint16_t bytes;
} ksn_schema_text;
typedef struct {
    union {
        ksn_schema_text text;
        ksn_rect rect;
        ksn_rgba color;
        bool boolean;
        uint16_t number;
        ksn_resource resource;
    } data;
} ksn_schema_value;
typedef struct {
    uint8_t slot; /* KSN_SCHEMA_LITERAL selects literal. */
    union {
        ksn_schema_text text;
        ksn_rect rect;
        ksn_rgba color;
        bool boolean;
        uint16_t number;
        ksn_resource resource;
    } literal;
} ksn_schema_binding;
typedef enum {
    KSN_NODE_RECT, KSN_NODE_ROUND_RECT, KSN_NODE_TEXT,
    KSN_NODE_IMAGE, KSN_NODE_PLATE_TEXT
} ksn_schema_node_kind;
#define KSN_SCHEMA_HAS_VISIBLE 1u
#define KSN_SCHEMA_HAS_PAGE 2u
#define KSN_SCHEMA_HAS_REVEAL 4u
#define KSN_SCHEMA_ADD_X0 1u
#define KSN_SCHEMA_ADD_Y0 2u
#define KSN_SCHEMA_ADD_X1 4u
#define KSN_SCHEMA_ADD_Y1 8u
typedef struct {
    ksn_schema_node_kind kind;
    ksn_schema_binding bounds,color,plate_color,text,visible,page,resource,variant,frame,reveal;
    ksn_font font;
    uint8_t radius,flags,rect_add_mask;
    uint8_t rect_add_slot[4]; /* x0,y0,x1,y1; U16 slots selected by mask */
    uint16_t source_width,source_height,page_equals;
    /* plateText uses bounds.x0/y0 as origin; the measured plate has four
     * pixels of horizontal padding and two pixels of vertical padding. */
} ksn_schema_node;
typedef struct {
    uint8_t version,slot_count,node_count;
    ksn_rgba background;
    uint8_t background_slot;
    bool dynamic_background;
    const ksn_schema_slot *slots;
    const ksn_schema_node *nodes;
} ksn_schema;
typedef struct {
    uint32_t nodes[KSN_SCHEMA_MAX_SLOTS]; /* bit i: node i reads this slot */
    uint32_t background_slots;
} ksn_schema_dependencies;

/* No allocation, JS, app name, source name, or mutable descriptor tree. The
 * supplied values must match the schema's slot order and remain stable until
 * submit returns. Hidden nodes consume no command or text capacity. */
ksn_result ksn_schema_validate(const ksn_schema *schema);
ksn_result ksn_schema_values_validate(const ksn_schema *schema,
                                      const ksn_schema_value *values);
ksn_result ksn_schema_dependencies_build(const ksn_schema *schema,
                                         ksn_schema_dependencies *out);
/* Validate a proposed complete value set before an adapter commits it. This
 * performs no transaction and catches derived geometry/reveal failures. */
ksn_result ksn_schema_preflight(const ksn_schema *schema,
                                const ksn_schema_value *values,ksn_rect viewport);
/* Host-aware variant additionally validates image ownership and dimensions
 * before an adapter commits a partial set. No transaction or allocation. */
ksn_result ksn_schema_preflight_view(const ksn_view *view,const ksn_schema *schema,
                                     const ksn_schema_value *values,ksn_rect viewport);
ksn_result ksn_schema_submit(ksn_view *view,ksn_rect viewport,
                             const ksn_schema *schema,
                             const ksn_schema_value *values,
                             ksn_ref refs[KSN_SCHEMA_MAX_NODES*2u],
                             uint8_t *ref_count,ksn_tx *out);
typedef enum { KSN_SCHEMA_NO_CHANGE, KSN_SCHEMA_PATCHED,
               KSN_SCHEMA_REPLACED } ksn_schema_delta;
/* Uses committed refs as the exact comparison baseline; no duplicate plan or
 * text snapshot is retained. On REPLACE, candidate refs are promoted by the
 * caller only after PRESENTED. On PATCH, active refs remain unchanged. The
 * immutable schema must have passed ksn_schema_validate at mount time. */
ksn_result ksn_schema_update(ksn_view *view,ksn_rect viewport,
                             const ksn_schema *schema,
                             const ksn_schema_value *values,
                             const ksn_ref active_refs[KSN_SCHEMA_MAX_NODES*2u],
                             uint8_t active_count,ksn_rgba active_background,
                             ksn_ref candidate_refs[KSN_SCHEMA_MAX_NODES*2u],
                             uint8_t *candidate_count,ksn_tx *out,
                             ksn_schema_delta *delta);
#endif
