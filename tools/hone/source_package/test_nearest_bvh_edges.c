/* Keep the original 3000-query fixture unchanged and add boundary coverage. */
#define main original_bvh_main
#include "test_nearest_bvh.c"
#undef main

static double failing_kernel(const double *point, const double *a,
                             const double *b, const double *c, double *output) {
    (void)point;
    (void)a;
    (void)b;
    (void)c;
    (void)output;
    return INFINITY;
}

int main(void) {
    assert(original_bvh_main() == 0);
    double vertices[9] = {1e20, 0, 0, 1e20, 4, 0, 1e20, 0, 4};
    uint32_t triangle[3] = {0, 1, 2};
    size_t bytes = hone_nearest_bvh_bytes(1);
    unsigned char *workspace = malloc(bytes + 1);
    assert(workspace);
    hone_nearest_bvh tree;
    assert(!hone_nearest_bvh_build(&tree, workspace + 1, bytes,
                                   vertices, 3, triangle, 1));
    assert(hone_nearest_bvh_build(&tree, workspace, bytes,
                                  vertices, 3, triangle, 1));
    assert(tree.used == 1 && tree.nodes[0].count == 1);
    double point[3] = {1e20, 1, 1}, expected[3], actual[3];
    kernel(point, vertices, vertices + 3, vertices + 6, expected);
    uint32_t face = 99;
    size_t tests = 0, visits = 0;
    assert(hone_nearest_bvh_query(&tree, point, kernel, actual, &face,
                                  &tests, &visits));
    assert(!memcmp(actual, expected, sizeof actual) && face == 0);
    memcpy(actual, (double[3]){7, 8, 9}, sizeof actual);
    face = 99;
    assert(!hone_nearest_bvh_query(&tree, point, failing_kernel, actual,
                                   &face, &tests, &visits));
    assert(actual[0] == 7 && actual[1] == 8 && actual[2] == 9 && face == 99);

    /* Float traversal may underestimate; it must never exceed the old bound. */
    for (unsigned sample = 0; sample < 20000; sample++) {
        hone_nearest_bvh_node box = {0};
        float query[3];
        double precise[3];
        for (unsigned component = 0; component < 3; component++) {
            box.lo[component] = (float)random_coord();
            box.hi[component] = box.lo[component] + (float)fabs(random_coord());
            query[component] = (float)random_coord();
            precise[component] = query[component];
        }
        assert(hone_nearest_bvh_bound_f32(&box, query) <=
               hone_nearest_bvh_bound(&box, precise));
    }
    free(workspace);
    puts("BVH EDGE PASS extreme exhaustive / no-finite-hit rollback / misalignment / 20000 lower bounds");
    return 0;
}
