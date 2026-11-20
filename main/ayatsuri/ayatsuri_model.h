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
    AYATSURI_MODEL_ENDIAN, AYATSURI_MODEL_UNSUPPORTED
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
     * 9..12 as a set: descriptor32, vertex28, triangle6, hole XYZ12.
     * Required extension 13: material sidedness U8 + three zero bytes,
     * one 4-byte row per material; absent means legacy single-sided. */
    ayatsuri_model_section section[13];
    /* Native capability, never serialized. True forbids every cold fallback.
     * Model and cold span remain immutable throughout instance lifetime.
     * Appended C ABI field: rebuild all callers; never persist raw handles. */
    bool provider_only;
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
    /* Internal cache key: ordinary evaluate must restore cold geometry. */
    bool mesh_overridden;
    uint8_t evaluation_outputs;
    uint64_t evaluation_serial;
} ayatsuri_model_instance;

/* Camera-independent native evaluation of ordinary AYNM v1 models.
 * Handles and the cold byte span remain caller-owned. Names and row spans
 * may be unaligned; their lengths exclude any terminator. */
enum {
    AYATSURI_OUTPUT_POSE = 1, AYATSURI_OUTPUT_DRAWABLES = 2,
    AYATSURI_OUTPUT_BOTH = 3, AYATSURI_NO_CLIP = -1
};
typedef enum {
    AYATSURI_CONTROL_POSITION, AYATSURI_CONTROL_ROTATION,
    AYATSURI_CONTROL_SCALE, AYATSURI_CONTROL_VISIBLE
} ayatsuri_model_property;
typedef union { float xyz[3]; uint8_t visible; } ayatsuri_model_value;
typedef struct { uint16_t id; ayatsuri_model_value value; } ayatsuri_model_control;
typedef struct {
    uint16_t format_version, max_nodes, max_mesh_vertices, max_mesh_triangles;
    uint32_t max_asset_bytes;
    uint8_t outputs;
    bool clips_supported;
} ayatsuri_model_caps;
typedef struct { uint16_t nodes, controls, clips; } ayatsuri_model_info;
typedef struct {
    const char *name;
    uint16_t name_bytes, id;
    int16_t parent_id; /* -1 for a root */
    uint8_t kind; /* 0 group, 1 ellipsoid, 2 blend, 3 mesh */
} ayatsuri_model_node_info;
typedef struct {
    uint16_t id, node_id;
    /* Name is (node name, property). Position/rotation are finite binary32
     * triples, rotation in radians. Scale additionally excludes zero.
     * Visible is an integer boolean in [0,1]. */
    ayatsuri_model_property property;
    ayatsuri_model_value default_value;
} ayatsuri_model_control_info;
ayatsuri_model_caps ayatsuri_model_capabilities(void);
ayatsuri_model_status ayatsuri_model_describe(const ayatsuri_model*, ayatsuri_model_info*);
ayatsuri_model_status ayatsuri_model_describe_node(const ayatsuri_model*, uint16_t id, ayatsuri_model_node_info*);
ayatsuri_model_status ayatsuri_model_describe_control(const ayatsuri_model*, uint16_t id, ayatsuri_model_control_info*);

typedef struct {
    double time_seconds; /* finite; v1 has no time-dependent tracks */
    int32_t clip_id; /* AYATSURI_NO_CLIP */
    uint8_t outputs;
    const ayatsuri_model_control *controls;
    size_t control_count;
} ayatsuri_model_input;
typedef struct {
    ayatsuri_model_instance *instance;
    const void *owner;
    uint32_t generation;
    uint64_t serial;
} ayatsuri_model_evaluation;
typedef struct {
    const float *local_trs; /* nine floats, position, Euler XYZ, scale */
    bool local_visible, visible; /* visible includes all parents */
} ayatsuri_model_pose_view;
/* A span of little-endian AYNM v1 records. Use byte loads or memcpy. */
typedef struct { const uint8_t *data; uint16_t count, stride; } ayatsuri_model_rows;
typedef struct {
    uint16_t node_id, material_id;
    uint8_t kind;
    const float *to_model; /* row-major 3x4, object-local to model space */
    /* Object-local records: ellipsoid radii XYZ or blend lobes (center XYZ,
     * radii XYZ); blend is the authored smooth union parameter. A localPlane
     * record is the optional AYNM section 8 row (mode at byte 2, normal XYZ
     * at 4, offset at 16, feather at 20, alternate RGB888 at 24). */
    ayatsuri_model_rows shape, local_plane;
    float blend;
    /* Mesh vertices are XYZ, normal XYZ, mask (7 F32). Triangles are three
     * vertex indices (U16). Hole points are XYZ F32, in loop order. */
    ayatsuri_model_rows vertices, triangles, hole_points;
    uint8_t hole_sizes[2], triangle_order, mesh_thickness;
    uint16_t albedo_rgb565[2];
    bool double_sided;
    /* Material is the original Shiki 11-F32 lighting vector, RGB888,
     * thickness, opacity and edge softness in [0,1]. */
    float lighting[11];
    uint32_t rgb;
    double thickness, opacity, edge_softness;
} ayatsuri_model_drawable;
/* Controls are unique model-local IDs, in any order. Each call starts from
 * authored defaults, including visibility and material. Entry rejection keeps
 * the last result; a hierarchy overflow during evaluation invalidates it.
 * Output pointers are synchronous borrows until the next accepted evaluation,
 * changed legacy target batch or disposal. A projected-packet evaluate also
 * expires the borrow on entry to its evaluation stage. Keep instance storage
 * alive while using the result. Reinitialization requires a fresh generation. */
ayatsuri_model_status ayatsuri_model_evaluate_input(ayatsuri_model_instance*, const void *owner,
        uint32_t generation, const ayatsuri_model_input*, ayatsuri_model_evaluation*);
ayatsuri_model_status ayatsuri_model_evaluation_pose(const ayatsuri_model_evaluation*, uint16_t node,
        ayatsuri_model_pose_view*);
ayatsuri_model_status ayatsuri_model_evaluation_next(const ayatsuri_model_evaluation*, uint32_t *cursor,
        ayatsuri_model_drawable*); /* start at zero; skips groups and hidden nodes */
/* Project the same drawables into the instance's legacy packet cache, for
 * consumers of instance_next/mesh_view. Another projection replaces packets.
 * Model-space drawables and pose remain valid across all such projections. */
ayatsuri_model_status ayatsuri_model_evaluation_project(const ayatsuri_model_evaluation*,
        const float camera[9],const float light[6]);
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

/* A synchronous borrowed view of the CURRENT evaluated mesh, not an owned
 * snapshot. Copy before any targets/evaluate/dispose call. Offset knowledge
 * remains inside the model implementation. Failure leaves out unchanged. */
typedef struct {
    const void *patch; /* const ksn_proc_gouraud_patch */
    const float *depth;
    uint16_t node_id;
    uint8_t vertices;
} ayatsuri_model_mesh_view;
ayatsuri_model_status ayatsuri_model_instance_mesh_view(ayatsuri_model_instance *instance,
        const void *owner,uint32_t generation,uint16_t node_id,ayatsuri_model_mesh_view *out);

/* Kind 5 is a sided mesh. All fields are immutable borrows until the next
 * mutation/evaluation/dispose, like ordinary packets. back_faces bit t selects
 * back_colors[2*v + A/B] for canonical-positive triangle t. */
typedef struct {
    const void *patch;
    const float *depth;
    const uint16_t *back_colors;
    const uint8_t *back_faces;
    uint16_t node_id;
    uint8_t vertices;
} ayatsuri_model_sided_mesh_view;
ayatsuri_model_status ayatsuri_model_instance_sided_mesh_view(ayatsuri_model_instance *instance,
        const void *owner,uint32_t generation,uint16_t node_id,ayatsuri_model_sided_mesh_view *out);

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
 * Kind 5 is an explicit double-sided mesh: use sided_mesh_view rather than
 * assuming a legacy Gouraud packet. Its positive-winding triangles retain the
 * global selected face order; side bits choose a second vertex color pair.
 * The extra aligned cache is 4*vertices+8 bytes, at most 136 bytes. The old
 * instance_next cursor and stats.packets still count logical draw nodes, not
 * the number of emitted contiguous side-run records.
 * Normals use inverse-transpose; source winding is never reversed, including
 * reflected poses. No mutable vertices, expression morphs or mesh opacity. */
ayatsuri_model_status ayatsuri_model_instance_next(ayatsuri_model_instance *instance,
        const void *owner,uint32_t generation,uint32_t *cursor,ayatsuri_model_packet *out);
#endif
