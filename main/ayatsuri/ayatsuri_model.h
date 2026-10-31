#ifndef AYATSURI_MODEL_H
#define AYATSURI_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* AYNM v1: immutable little-endian cold tables, no cached runtime work.
 * The caller keeps the complete span AND this model alive for every instance.
 * All APIs are synchronous and allocation-free. No JS pointer is retained.
 * Compile with IEEE arithmetic, -ffp-contract=off, and no excess precision. */
enum { AYATSURI_MODEL_MAX_NODES=256, AYATSURI_MODEL_MAX_BYTES=1024*1024,
       AYATSURI_MODEL_MAX_SECTIONS=64 };
typedef enum {
    AYATSURI_MODEL_OK, AYATSURI_MODEL_INVALID, AYATSURI_MODEL_RANGE,
    AYATSURI_MODEL_STATE, AYATSURI_MODEL_BUSY, AYATSURI_MODEL_DONE,
    AYATSURI_MODEL_ENDIAN
} ayatsuri_model_status;
typedef struct { uint32_t offset, count; } ayatsuri_model_section;
typedef struct {
    uint32_t eval_calls, geometry_updates, projection_updates, shade_updates;
    uint32_t target_changed, init_copy_bytes, target_copy_bytes;
} ayatsuri_model_stats;
typedef struct {
    const uint8_t *data;
    uint32_t bytes;
    uint16_t node_count, packet_count;
    uint32_t slot_bytes;
    /* Sections 1..7 are mandatory; optional localPlane rows use required
     * section 8. Mode 4 uses existing ellipse work; mode 5 adds 24 bytes
     * only to each masked blend's work slot. Mesh kind 3 requires sections
     * 9..12 as a set: descriptor32, vertex28, triangle6, hole XYZ12. */
    ayatsuri_model_section section[12];
} ayatsuri_model;

typedef struct {
    const ayatsuri_model *model;
    void *arena;
    const void *owner;
    size_t arena_bytes;
    uint32_t generation, mutation_epoch;
    float camera[9], light[6], lighting[9];
    ayatsuri_model_stats stats;
    bool alive, busy, evaluated;
} ayatsuri_model_instance;
enum {
    AYATSURI_TARGET_POSITION=1, AYATSURI_TARGET_ROTATION=2,
    AYATSURI_TARGET_SCALE=4, AYATSURI_TARGET_VISIBLE=8,
    AYATSURI_TARGET_HIDDEN=16, AYATSURI_TARGET_COLOR=32,
    AYATSURI_TARGET_OPACITY=64
};
typedef struct {
    const uint8_t *dirty;
    const float *pose;
    const uint32_t *color;
    const void *opacity;
    size_t count;
} ayatsuri_model_targets;
typedef struct {
    const uint8_t *storage;
    const float *lighting;
    uint16_t node_id, bytes;
    uint8_t kind, lobe_count;
} ayatsuri_model_packet;

/* Failure does not publish a model. Names are non-NUL ASCII spans. */
ayatsuri_model_status ayatsuri_model_open(const void *data,size_t bytes,ayatsuri_model *out);
ayatsuri_model_status ayatsuri_model_find_node(const ayatsuri_model *model,
        const char *name,size_t bytes,uint16_t *node_id);
/* Bytes includes the node cache and work slots. Arena must be 8-byte aligned,
 * disjoint from model data, and remain alive until instance_dispose. */
size_t ayatsuri_model_instance_bytes(const ayatsuri_model *model);
ayatsuri_model_status ayatsuri_model_instance_init(const ayatsuri_model *model,
        void *arena,size_t bytes,const void *owner,uint32_t generation,
        ayatsuri_model_instance *out);
ayatsuri_model_status ayatsuri_model_instance_dispose(ayatsuri_model_instance *instance,
        const void *owner,uint32_t generation);
/* Every source array has count=N entries (pose has N*9 floats). Opacity is
 * a byte-addressed span of N little-endian IEEE binary64 values; its base
 * need not be double-aligned. All inputs are borrowed only during the call.
 * Validate all selected values before any write. A rejected batch leaves
 * cached packets publishable; a same-value batch preserves the epoch.
 * Mesh cold materials require thickness=1, opacity=1, edgeSoftness=0; the
 * mesh descriptor owns the actual U8 thickness. Mesh nodes accept pose/visibility only; COLOR/OPACITY rejects the batch. */
ayatsuri_model_status ayatsuri_model_instance_targets(ayatsuri_model_instance *instance,
        const void *owner,uint32_t generation,const ayatsuri_model_targets *targets);
/* Immutable node kind (0 group, 1 ellipse, 2 blend, 3 mesh). No evaluation
 * is required. Invalid owner/state/index/output leaves the output unchanged. */
ayatsuri_model_status ayatsuri_model_instance_node_kind(ayatsuri_model_instance *instance,
        const void *owner,uint32_t generation,uint16_t node_id,uint8_t *out);
ayatsuri_model_status ayatsuri_model_instance_read_pose(ayatsuri_model_instance *instance,
        const void *owner,uint32_t generation,uint16_t node_id,float out[9]);
/* Counters saturate at UINT32_MAX. eval_calls counts validated-owner attempts;
 * work counters count completed node stages, including a later failed batch.
 * target_changed counts changed nodes; copy counters count bytes explicitly
 * copied by initialization or changed target fields, excluding zero fill. */
ayatsuri_model_status ayatsuri_model_instance_stats(ayatsuri_model_instance *instance,
        const void *owner,uint32_t generation,ayatsuri_model_stats *out);
/* camera: position3,target3,zoom,center2; light: normalized direction3,RGB3.
 * Failed evaluation invalidates iteration; an already copied frame is never
 * touched. Success borrows internal packets until the next mutation. */
ayatsuri_model_status ayatsuri_model_instance_evaluate(ayatsuri_model_instance *instance,
        const void *owner,uint32_t generation,const float camera[9],const float light[6]);
/* cursor begins at zero. Groups and inherited-hidden nodes are skipped.
 * storage uses existing 84/264/(316+108*lobes) layout; ellipse fields start at
 * 96,128,164,196,232; blend fields at 252,264,308,316+24*lobes.
 * Kind 3 is a masked blend: old blend work followed by plane[4] F32,
 * feather F32, RGB565 colorB U16, reserved U16=0 (24 bytes). Plane is
 * A*screenX+B*screenY+C*finalBlendedDepth+D in common node-local units.
 * Kind 4 is a Gouraud mesh: the common 84-byte transform/pose prefix, then
 * a 1040-byte ksn_proc_gouraud_patch, then xy[2*vertices]/depth[vertices]
 * F32 scratch; the slot is rounded to 8 bytes. Empty selection is skipped.
 * Normals use inverse-transpose; source winding is never reversed, including
 * reflected poses. No mutable vertices, expression morphs or mesh opacity. */
ayatsuri_model_status ayatsuri_model_instance_next(ayatsuri_model_instance *instance,
        const void *owner,uint32_t generation,uint32_t *cursor,ayatsuri_model_packet *out);
#endif
