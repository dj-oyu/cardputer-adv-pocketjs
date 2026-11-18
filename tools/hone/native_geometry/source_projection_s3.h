#ifndef HONE_SOURCE_PROJECTION_S3_H
#define HONE_SOURCE_PROJECTION_S3_H

/* Caller has admitted the matrix, vertex count and disjoint scratch.
 * S3 has sixteen scalar FP registers. Keep all nine linear coefficients
 * resident and interleave independent rows. Separate MUL.S and ADD.S match
 * the live fixture's -ffp-contract=off operation order. */
static inline bool hone_source_project_s3(const float p[12], const float *src,
                                         size_t count, float dst[][3]) {
    float *output = &dst[0][0];
    unsigned x, y, z;
    const unsigned max_xy = 0x8b000000u; /* bits(4096.f) << 1 */
    const unsigned infinity = 0xff000000u; /* bits(+infinity) << 1 */
    __asm__ volatile (
        "lsi f0, %[p], 0\n\t"   "lsi f1, %[p], 16\n\t" "lsi f2, %[p], 32\n\t"
        "lsi f3, %[p], 4\n\t"   "lsi f4, %[p], 20\n\t" "lsi f5, %[p], 36\n\t"
        "lsi f6, %[p], 8\n\t"   "lsi f7, %[p], 24\n\t" "lsi f8, %[p], 40\n\t"
        "1:\n\t"
        "l32i %[x], %[src], 0\n\t" "l32i %[y], %[src], 4\n\t" "l32i %[z], %[src], 8\n\t"
        "wfr f9, %[x]\n\t" "wfr f10, %[y]\n\t" "wfr f11, %[z]\n\t"
        "slli %[x], %[x], 1\n\t" "slli %[y], %[y], 1\n\t" "slli %[z], %[z], 1\n\t"
        "maxu %[x], %[x], %[y]\n\t" "maxu %[x], %[x], %[z]\n\t" "bltu %[bound], %[x], 2f\n\t"
        "mul.s f12, f0, f9\n\t" "mul.s f13, f1, f9\n\t" "mul.s f14, f2, f9\n\t"
        "mul.s f15, f3, f10\n\t" "mul.s f9, f4, f10\n\t" "mul.s f10, f5, f10\n\t"
        "add.s f12, f12, f15\n\t" "add.s f13, f13, f9\n\t" "add.s f14, f14, f10\n\t"
        "mul.s f15, f6, f11\n\t" "mul.s f9, f7, f11\n\t" "mul.s f10, f8, f11\n\t"
        "add.s f12, f12, f15\n\t" "add.s f13, f13, f9\n\t" "add.s f14, f14, f10\n\t"
        "lsi f15, %[p], 12\n\t" "lsi f9, %[p], 28\n\t" "lsi f10, %[p], 44\n\t"
        "add.s f12, f12, f15\n\t" "add.s f13, f13, f9\n\t" "add.s f14, f14, f10\n\t"
        "rfr %[x], f12\n\t" "rfr %[y], f13\n\t" "rfr %[z], f14\n\t"
        "s32i %[x], %[out], 0\n\t" "s32i %[y], %[out], 4\n\t" "s32i %[z], %[out], 8\n\t"
        "slli %[x], %[x], 1\n\t" "slli %[y], %[y], 1\n\t" "slli %[z], %[z], 1\n\t"
        "maxu %[x], %[x], %[y]\n\t" "bltu %[bound], %[x], 2f\n\t" "bgeu %[z], %[inf], 2f\n\t"
        "addi %[src], %[src], 12\n\t" "addi %[out], %[out], 12\n\t"
        "addi %[n], %[n], -1\n\t" "bnez %[n], 1b\n\t" "2:"
        : [src] "+&a"(src), [out] "+&a"(output), [n] "+&a"(count),
          [x] "=&a"(x), [y] "=&a"(y), [z] "=&a"(z)
        : [p] "a"(p), [bound] "a"(max_xy), [inf] "a"(infinity)
        : "f0","f1","f2","f3","f4","f5","f6","f7","f8","f9",
          "f10","f11","f12","f13","f14","f15","memory");
    return count == 0;
}
#endif
