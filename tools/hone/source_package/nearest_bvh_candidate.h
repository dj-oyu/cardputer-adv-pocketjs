#ifndef HONE_NEAREST_BVH_CANDIDATE_H
#define HONE_NEAREST_BVH_CANDIDATE_H
#include <math.h>
#include <float.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "nearest BVH requires finite checks and ordinary IEEE rounding"
#endif
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128, "nearest BVH requires IEEE binary32");
/* Independent immutable-per-query BVH. Caller supplies complete disjoint
 * workspace. Rebuild each deformed target frame. Original face IDs survive
 * partitioning; nearest ties always choose the lowest original ID. The supplied
 * point kernel must obey the same conservative AABB error bound as the existing
 * batch candidate (128*FLT_EPSILON*scale^2). This is not a bound for arbitrary
 * user kernels. Accelerated bounds require maximum absolute referenced vertex
 * and query coordinates <= 1e10; larger finite coordinates use exhaustive
 * traversal. No contraction/fast math. Complete disjoint source/work/output
 * spans are caller preconditions; overlap is not runtime-checked. */
typedef struct {
    float lo[3], hi[3];
    uint32_t first, count, right;
} hone_nearest_bvh_node;
typedef struct {
    hone_nearest_bvh_node * nodes;
    uint32_t * ids;
    size_t used, capacity, nt, nv;
    const double * vertices;
    const uint32_t * triangles;
    double scale;
} hone_nearest_bvh;
typedef double(* hone_nearest_point_fn)(const double *, const double *, const double *, const double *, double *);
static inline size_t hone_nearest_bvh_nodes(size_t nt) {
    if (!nt || nt > 65535) return 0;
    size_t leaves = 1;
    while (leaves <(nt + 7) / 8) leaves *= 2;
    return 2 * leaves - 1;
}

static inline size_t hone_nearest_bvh_bytes(size_t nt) {
    size_t n = hone_nearest_bvh_nodes(nt);
    return n ? n * sizeof(hone_nearest_bvh_node) + nt * sizeof(uint32_t) : 0;
}

static inline double hone_nearest_bvh_key(const hone_nearest_bvh * tree, uint32_t face, unsigned axis) {
    return tree->vertices[3 *(size_t) tree->triangles[3 *(size_t) face] + axis];
}

static inline void hone_nearest_bvh_partition(hone_nearest_bvh * tree, size_t first, size_t count, unsigned axis) {
    size_t lo = first, hi = first + count - 1, mid = first + count / 2;
    while (lo < hi) {
        double pivot = hone_nearest_bvh_key(tree, tree->ids[lo +(hi - lo) / 2], axis);
        size_t left_cursor = lo, right_cursor = hi;
        for (;;) {
            while (hone_nearest_bvh_key(tree, tree->ids[left_cursor], axis) < pivot) left_cursor++;
            while (hone_nearest_bvh_key(tree, tree->ids[right_cursor], axis) > pivot) right_cursor--;
            if (left_cursor >= right_cursor) break;
            uint32_t swap_id = tree->ids[left_cursor];
            tree->ids[left_cursor++] = tree->ids[right_cursor];
            tree->ids[right_cursor--] = swap_id;
        }
        if (mid <= right_cursor) hi = right_cursor;
        else lo = right_cursor + 1;
    }
}

static inline int hone_nearest_bvh_build_node(hone_nearest_bvh * tree, size_t first, size_t count) {
    if (tree->used >= tree->capacity) return 0;
    size_t index = tree->used++;
    hone_nearest_bvh_node * node = tree->nodes + index;
    double lo[3] = {
        INFINITY, INFINITY, INFINITY
    }, hi[3] = {
        - INFINITY, - INFINITY, - INFINITY
    };
    for (size_t entry = first; entry < first + count; entry++) {
        for (unsigned corner = 0; corner < 3; corner++) {
            for (unsigned component = 0; component < 3; component++) {
                double coordinate = tree->vertices[3 *(size_t) tree->triangles[
                    3 *(size_t) tree->ids[entry] + corner] + component];
                if (coordinate < lo[component]) lo[component] = coordinate;
                if (coordinate > hi[component]) hi[component] = coordinate;
            }
        }
    }
    for (unsigned component = 0; component < 3; component++) {
        node->lo[component] = nextafterf((float) lo[component], - INFINITY);
        node->hi[component] = nextafterf((float) hi[component], INFINITY);
    }
    node->first =(uint32_t) first;
    node->count =(uint32_t) count;
    node->right = 0;
    if (count <= 8) return 1;
    unsigned axis = 0;
    for (unsigned component = 1; component < 3; component++) if (hi[component] - lo[component] > hi[axis] - lo[axis]) axis = component;
    hone_nearest_bvh_partition(tree, first, count, axis);
    size_t half = count / 2;
    if (!hone_nearest_bvh_build_node(tree, first, half)) return 0;
    node->right =(uint32_t) tree->used;
    node->count = 0;
    return hone_nearest_bvh_build_node(tree, first + half, count - half);
}

static inline int hone_nearest_bvh_build(hone_nearest_bvh * tree, void * workspace, size_t bytes, const double * vertices, size_t vertex_count, const uint32_t * triangles, size_t triangle_count) {
    size_t capacity = hone_nearest_bvh_nodes(triangle_count), required = hone_nearest_bvh_bytes(triangle_count);
    if (!tree || !workspace ||((uintptr_t) workspace % _Alignof(hone_nearest_bvh_node)) || bytes < required || !required || !vertices || !triangles || !vertex_count || vertex_count > SIZE_MAX / 3) return 0;
    * tree =(hone_nearest_bvh) {
        .nodes = workspace, .ids =(uint32_t *)((unsigned char *) workspace + capacity * sizeof(hone_nearest_bvh_node)), .capacity = capacity, .nt = triangle_count, .nv = vertex_count, .vertices = vertices, .triangles = triangles, .scale = 1
    };
    for (size_t face_id = 0; face_id < triangle_count; face_id++) {
        tree->ids[face_id] =(uint32_t) face_id;
        for (unsigned corner = 0; corner < 3; corner++) {
            uint32_t vertex_id = triangles[3 * face_id + corner];
            if (vertex_id >= vertex_count) return 0;
            for (unsigned component = 0; component < 3; component++) {
                double coordinate = vertices[3 *(size_t) vertex_id + component];
                if (!isfinite(coordinate)) return 0;
                if (fabs(coordinate) > tree->scale) tree->scale = fabs(coordinate);
            }
        }
    }
    /* Match the existing batch fallback before any narrowing of extreme doubles. */
    if (tree->scale > 1e10) {
        tree->nodes[0] =(hone_nearest_bvh_node) {
            .first = 0, .count =(uint32_t) triangle_count
        };
        tree->used = 1;
        return 1;
    }
    return hone_nearest_bvh_build_node(tree, 0, triangle_count);
}

static inline double hone_nearest_bvh_bound(const hone_nearest_bvh_node * node, const double * point) {
    double bound = 0;
    for (unsigned component = 0; component < 3; component++) {
        double distance = fmax(fmax((double) node->lo[component] - point[component], point[component] -(double) node->hi[component]), 0);
        bound += distance * distance;
    }
    return bound;
}
/* Directed one-ULP guards on positive binary32 operations. Lower bounds never
 * exceed exact box distance; the prune threshold is rounded upward. Only used
 * when the query is exactly representable as float, in the bounded scale domain.
 * This changes traversal arithmetic, never the closest-point kernel/result. */
static inline float hone_nearest_bvh_down(float x) {
    uint32_t u;
    memcpy(& u, & x, 4);
    if (u) u--;
    memcpy(& x, & u, 4);
    return x;
}

static inline float hone_nearest_bvh_up(float x) {
    uint32_t u;
    memcpy(& u, & x, 4);
    if (u < UINT32_C(0x7f800000)) u++;
    memcpy(& x, & u, 4);
    return x;
}

static inline float hone_nearest_bvh_bound_f32(const hone_nearest_bvh_node * node, const float * point) {
    float sum = 0;
    for (unsigned component = 0; component < 3; component++) {
        float a = node->lo[component] - point[component], b = point[component] - node->hi[component], distance = a > b ? a : b;
        if (distance > 0) {
            distance = hone_nearest_bvh_down(distance);
            float squared = hone_nearest_bvh_down(distance * distance);
            sum = hone_nearest_bvh_down(sum + squared);
        }
    }
    return sum;
}

static inline int hone_nearest_bvh_query(const hone_nearest_bvh * tree, const double * point_position, hone_nearest_point_fn point_kernel, double * out, uint32_t * face, size_t * tests, size_t * visits) {
    if (!tree || !tree->used || !point_position || !point_kernel || !out || !face || !tests || !visits) return 0;
    double scale = tree->scale;
    for (unsigned component = 0; component < 3; component++) {
        if (!isfinite(point_position[component])) return 0;
        if (fabs(point_position[component]) > scale) scale = fabs(point_position[component]);
    }
    float float_point[3];
    int fast = scale <= 1e10;
    if (fast) for (unsigned component = 0; component < 3; component++) {
        float_point[component] =(float) point_position[component];
        if ((double) float_point[component] != point_position[component]) fast = 0;
    }
    float margin = 0;
    if (fast) {
        float sf = hone_nearest_bvh_up((float) scale);
        margin = hone_nearest_bvh_up((128 * FLT_EPSILON) * hone_nearest_bvh_up(sf * sf));
    }
    double best = INFINITY, winner[3] = {
        0
    };
    uint32_t winning_face = UINT32_MAX;
    uint32_t stack[32];
    size_t depth = 1;
    stack[0] = 0;
    while (depth) {
        uint32_t index = stack[-- depth];
        const hone_nearest_bvh_node * node = tree->nodes + index;
        (* visits)++;
        if (fast) {
            if (hone_nearest_bvh_bound_f32(node, float_point) > hone_nearest_bvh_up(hone_nearest_bvh_up((float) best) + margin)) continue;
        }
        else if (scale <= 1e10 && hone_nearest_bvh_bound(node, point_position) > best + 128 * FLT_EPSILON * scale * scale) continue;
        if (!node->count) {
            uint32_t left = index + 1, right = node->right;
            if (depth + 2 > 32) return 0;
            int left_first = fast ? hone_nearest_bvh_bound_f32(tree->nodes + left, float_point) <= hone_nearest_bvh_bound_f32(tree->nodes + right, float_point) : hone_nearest_bvh_bound(tree->nodes + left, point_position) <= hone_nearest_bvh_bound(tree->nodes + right, point_position);
            if (left_first) {
                stack[depth++] = right;
                stack[depth++] = left;
            }
            else {
                stack[depth++] = left;
                stack[depth++] = right;
            }
            continue;
        }
        for (size_t entry = node->first; entry <(size_t) node->first + node->count; entry++) {
            uint32_t face_id = tree->ids[entry];
            const uint32_t * triangle = tree->triangles + 3 *(size_t) face_id;
            double hit[3];
            double distance = point_kernel(point_position, tree->vertices + 3 *(size_t) triangle[0], tree->vertices + 3 *(size_t) triangle[1], tree->vertices + 3 *(size_t) triangle[2], hit);
            (* tests)++;
            /* A failing kernel need not initialize hit. Never consume it when
             * the reported distance is nonfinite, including initial INF ties. */
            if (isfinite(distance) &&
                (distance < best || (distance == best && face_id < winning_face))) {
                best = distance;
                winning_face = face_id;
                for (unsigned component = 0; component < 3; component++) winner[component] = hit[component];
            }
        }
    }
    if (!isfinite(best)) return 0;
    for (unsigned component = 0; component < 3; component++) if (!isfinite(winner[component])) return 0;
    for (unsigned component = 0; component < 3; component++) out[component] = winner[component];
    * face = winning_face;
    return 1;
}
#endif
