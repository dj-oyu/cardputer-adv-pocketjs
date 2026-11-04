/* Synthetic storage-v3 fixtures. White-box access verifies exact ownership. */
#include <stdlib.h>
#include <assert.h>
static struct { void *ptr; size_t bytes; } tracked[16];
static size_t live_bytes, peak_bytes;
static void account(void *ptr, size_t bytes) {
    if (!ptr) return;
    for (size_t i = 0; i < 16; i++) if (!tracked[i].ptr) {
        tracked[i].ptr = ptr; tracked[i].bytes = bytes;
        live_bytes += bytes;
        if (live_bytes > peak_bytes) peak_bytes = live_bytes;
        return;
    }
    assert(0);
}
static void *tracked_malloc(size_t n) { void *p = malloc(n); account(p, n); return p; }
static void *tracked_calloc(size_t n, size_t z) { void *p = calloc(n, z); account(p, n*z); return p; }
static void tracked_free(void *p) {
    if (!p) return;
    for (size_t i = 0; i < 16; i++) if (tracked[i].ptr == p) {
        live_bytes -= tracked[i].bytes; tracked[i].ptr = NULL; free(p); return;
    }
    assert(0);
}
#define malloc tracked_malloc
#define calloc tracked_calloc
#define free tracked_free
#include "visibility.c"
#undef malloc
#undef calloc
#undef free
#include <stdio.h>

static size_t nodes_breadth_first(size_t triangles) {
    size_t queue[VIS_MAX_NODES], head = 0, tail = 1;
    queue[0] = triangles;
    while (head < tail) {
        size_t count = queue[head++];
        if (count > 4) {
            assert(tail + 2 <= VIS_MAX_NODES);
            queue[tail++] = count / 2;
            queue[tail++] = count - count / 2;
        }
    }
    return tail;
}

int main(void) {
    assert(VIS_STORAGE_CONTRACT_VERSION == 3);
    assert(sizeof(triangle) == 4 * sizeof(uint16_t));
    assert(sizeof(build_center) == 3 * sizeof(double));
    const size_t vertex_counts[] = {3, 8, 64, VIS_MAX_VERTICES};
    for (size_t n = 1; n <= VIS_MAX_TRIANGLES; n++) {
        size_t nodes = nodes_breadth_first(n);
        assert(vis_scene_required_nodes(n) == nodes);
        assert(vis_scene_build_temporary_bytes_v3(n) == n * 3 * sizeof(double));
        for (size_t i = 0; i < sizeof(vertex_counts) / sizeof(vertex_counts[0]); i++) {
            size_t v = vertex_counts[i];
            size_t expected = sizeof(vis_scene) + v * 3 * sizeof(double)
                            + n * sizeof(triangle) + nodes * sizeof(node);
            assert(vis_scene_required_bytes_v3(v, n) == expected);
        }
    }
    assert(!vis_scene_required_nodes(0));
    assert(!vis_scene_required_nodes(VIS_MAX_TRIANGLES + 1));
    assert(!vis_scene_required_nodes(SIZE_MAX));
    assert(!vis_scene_required_bytes_v3(0, 1));
    assert(!vis_scene_required_bytes_v3(VIS_MAX_VERTICES + 1, 1));
    assert(!vis_scene_required_bytes_v3(3, 0));
    assert(!vis_scene_required_bytes_v3(3, VIS_MAX_TRIANGLES + 1));
    assert(!vis_scene_build_temporary_bytes_v3(0));
    assert(!vis_scene_build_temporary_bytes_v3(SIZE_MAX));

    /* Duplicate positions retain distinct IDs; unused signed-zero vertices
     * remain owned verbatim. Alternating references exercise indexed ties. */
    double xyz[] = {
        -1, -1, 0, 1, -1, 0, 0, 1, 0,
        -1, -1, 0, 1, -1, 0, 0, 1, 0,
        -0.0, 0.0, -0.0, 7, 8, 9
    };
    double original[sizeof(xyz) / sizeof(xyz[0])];
    memcpy(original, xyz, sizeof(xyz));
    uint32_t indices[3 * 9];
    for (size_t i = 0; i < 9; i++)
        for (size_t j = 0; j < 3; j++) indices[i * 3 + j] = (uint32_t)((i % 2) * 3 + j);
    vis_generation generation = {1, 1, 1};
    size_t bytes = vis_scene_required_bytes_v3(8, 9);
    size_t nodes = vis_scene_required_nodes(9);
    vis_limits limits = {9, nodes, bytes};
    vis_scene *scene = NULL;
    vis_limits too_small = limits;
    too_small.max_bytes--;
    assert(vis_scene_build(&scene, xyz, 8, indices, 9, generation, too_small) == VIS_CAPACITY);
    assert(!scene);
    too_small = limits;
    too_small.max_nodes--;
    assert(vis_scene_build(&scene, xyz, 8, indices, 9, generation, too_small) == VIS_CAPACITY);
    assert(!scene);
    assert(vis_scene_build(&scene, xyz, 8, indices, 9, generation, limits) == VIS_OK);
    assert(live_bytes == bytes);
    assert(peak_bytes == bytes + 9 * 3 * sizeof(double));
    assert(scene->vertices == 8 && scene->bytes == bytes && scene->nodes == nodes);
    assert(!memcmp(scene->xyz, original, sizeof(original)));
    assert(vis_scene_overlaps(scene, scene->xyz + 23, sizeof(double)));
    assert(!vis_scene_overlaps(scene, xyz, sizeof(xyz)));
    for (size_t i = 0; i < 9; i++) {
        size_t ordinal = scene->tri[i].ordinal;
        assert(ordinal < 9);
        for (size_t j = 0; j < 3; j++)
            assert(scene->tri[i].vertex[j] == indices[ordinal * 3 + j]);
    }
    memset(xyz, 0xa5, sizeof(xyz));
    memset(indices, 0xa5, sizeof(indices));
    assert(!memcmp(scene->xyz, original, sizeof(original)));
    vis_binding binding = {0};
    assert(vis_binding_set(&binding, scene, generation) == VIS_OK);
    double origin[] = {0, 0, -1}, direction[] = {0, 0, 1};
    vis_budget budget = {{1, 100, 100}, {0, 0, 0}};
    vis_hit hit;
    assert(vis_ray_nearest(&binding, generation, origin, direction, &budget, &hit) == VIS_OK);
    assert(hit.found && hit.triangle_ordinal == 0 && hit.distance == 1);
    vis_binding_clear(&binding);
    vis_scene_release(scene);
    assert(live_bytes == 0);
    puts("PASS: storage-v3 exhaustive capacity formulas; exact-byte admission; original vertex IDs, unused vertices, signed zero, poison and ties");
    return 0;
}
