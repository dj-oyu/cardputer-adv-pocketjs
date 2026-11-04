#ifndef FOX_VISIBILITY_H
#define FOX_VISIBILITY_H
#include <stddef.h>
#include <stdint.h>
/* Host-only, no global state, no automatic quota growth. Not thread-safe. */
/* Opt-in storage admission v3: shared exact original-order xyz, uint16 index/
 * ordinal records, exact median-tree capacity. All hard and ray/work ceilings
 * are unchanged. Baseline modules retain the prior storage/API contracts. */
#define VIS_STORAGE_CONTRACT_VERSION 3u
#define VIS_MAX_VERTICES 4096u
#define VIS_MAX_TRIANGLES 2048u
#define VIS_MAX_NODES 4095u
#define VIS_SEGMENT_EPSILON 1e-5

typedef enum { VIS_OK=0, VIS_INVALID, VIS_CAPACITY, VIS_ALLOC,
               VIS_GENERATION, VIS_BUDGET } vis_status;
typedef struct { uint64_t geometry, pose, light; } vis_generation;
typedef struct vis_scene vis_scene;
typedef struct { vis_scene *scene; vis_generation generation; } vis_binding;
typedef struct { size_t max_triangles, max_nodes, max_bytes; } vis_limits;
typedef struct { uint64_t rays, node_visits, triangle_tests; } vis_counters;
typedef struct { vis_counters limit, used; } vis_budget;
/* All vertices are finite world-space doubles; indices contain 3 per triangle.
 * Nonzero generation tokens are caller-issued monotonic change counters. Caller
 * Tokens are NOT self-verifying: the adapter must bind actual snapshot/light
 * identities to these tokens. Caller must increment the relevant token whenever geometry, pose or light changes.
 * For replacement each token must be >= old and at least one must increase.
 * Failure leaves *slot and its ownership unchanged. Success releases old *slot.
 * Input arrays are copied; no borrowed storage survives. Original vertex IDs
 * are not welded, including equal-coordinate or unused vertices. uint32 input
 * indices are validated <vertices<=4096 before exact uint16 narrowing; original
 * triangle ordinals <=2047 also narrow exactly. All double bits are copied.
 * slot must point to an initialized NULL or valid owned scene pointer, outside
 * all scene allocations and input arrays. Detectable self/input aliases reject;
 * aliases of unrelated scenes cannot be discovered without a global registry. Empty scenes invalid.
 * Limits include every persistent requested heap byte (not allocator metadata),
 * separate from render snapshot. A build-only temporary allocation is additionally
 * required; see vis_scene_build_temporary_bytes_v3(). It is freed before publish.
 * Build peak = new persistent bytes + build temporary + all retained old scenes.
 * Caller arrays remain additional. max_bytes caps persistent bytes only.
 * Fixed traversal stack is 32 uint32_t entries; build recursion depth <= 10
 * for leaves of <=4 triangles. Source arrays remain caller-owned and additional.
 */
vis_status vis_scene_build(vis_scene **slot, const double *xyz, size_t vertices,
                           const uint32_t *indices, size_t triangles,
                           vis_generation generation, vis_limits limits);
vis_status vis_scene_retain(vis_scene *scene);
void vis_scene_release(vis_scene *scene);
size_t vis_scene_bytes(const vis_scene *scene);
/* Checks all four owned allocation spans, including all xyz and allocated nodes. */
int vis_scene_overlaps(const vis_scene *scene, const void *ptr, size_t bytes);
/* C(n)=1 for 1<=n<=4, else 1+C(floor(n/2))+C(ceil(n/2)). */
size_t vis_scene_required_nodes(size_t triangles); /* 0 if invalid/overflow */
/* v3 deliberately requires both counts: includes ALL original input vertices. */
size_t vis_scene_required_bytes_v3(size_t vertices,size_t triangles);
size_t vis_scene_build_temporary_bytes_v3(size_t triangles); /* 0 if invalid/overflow */
size_t vis_scene_triangle_count(const vis_scene *scene);
size_t vis_scene_node_count(const vis_scene *scene);
vis_generation vis_scene_generation(const vis_scene *scene);
/* Initialize binding to {0}. Rebind is atomic and retains replacement.
 * Binding storage must be outside every scene. Overlap with old/new scene is
 * rejected before mutation; invalid/forged pointers are not supported.
 * Clear is a no-op for detected overlap with its retained scene. */
vis_status vis_binding_set(vis_binding *binding, vis_scene *scene,
                           vis_generation expected);
void vis_binding_clear(vis_binding *binding);
/* Finite endpoints, length > 2*epsilon. Opaque, two-sided, endpoints excluded
 * by fixed world-space epsilon. Cumulative budget belongs to ONE render and
 * must not be reset between its rays. On any failure *occluded is unchanged;
 * counters reflect work already consumed. Never interpret failure as clear.
 */
vis_status vis_segment_occluded(const vis_binding *binding,
                                vis_generation expected,
                                const double from[3], const double to[3],
                                vis_budget *budget, int *occluded);
/* Nearest opaque two-sided ray hit. Direction must be unit length within 1e-12.
 * Distance is world-space, strictly greater than VIS_SEGMENT_EPSILON. Far bound
 * derives from all root bounds relative to origin. Miss succeeds with found=0
 * and all other fields zero. Normal is unit raw-cross winding (not face-forward).
 * Original input triangle ordinal breaks exact equal-distance ties.
 * Failure leaves the complete hit output unchanged; budget is cumulative. */
typedef struct {
 int found;
 uint32_t triangle_ordinal;
 double distance, bary[3], position[3], geometric_normal[3];
} vis_hit;
vis_status vis_ray_nearest(const vis_binding *binding, vis_generation expected,
                           const double origin[3], const double direction[3],
                           vis_budget *budget, vis_hit *hit);
#endif
