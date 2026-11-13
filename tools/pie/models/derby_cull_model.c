/* The arithmetic layer for main/pocket/derby_ser_pie.c (tools/pie/README.md):
 * is the int16 cull a safe stand-in for pocket_derby.c's double ou()?
 *
 * Claim checked: for every chord whose bit is in `valid`, the integer answer
 * "out" implies the double ou() answer "out" (never culls a chord the double
 * keeps). Also measured: the error of the lanes against the double
 * (the header's budget: at most 4.4 Q3 units, sec.10.2 of
 * docs/apps/derby-ser-division.md) and how many of the double's culls the
 * integer gives up to its 1 m margin.
 *
 * Two populations: the device self-test's vectors (derby_ser_pie_vec.h, many
 * more of them), and a wide one at the edges of the allowed ranges (f up to
 * 4,090 px, m up to 64 m, |w| up to 128 m, cameras up to 2,900 m off, so ends
 * sit on both sides of the 2,047 m edge). Not exhaustive -- the input space is
 * continuous -- so the bound is also argued in the doc; this is the check
 * that the argument and the code agree. Prints mismatches=<unsafe>. */
#define DERBY_SERPIE_HOST 1
#include <stdio.h>
#include <stdlib.h>
#include "../../../main/pocket/derby_ser_pie.c"
#include "../../../main/pocket/derby_ser_pie_vec.h"

static void wide_case(dspv_case *K, unsigned index) {
    uint32_t s = 0xa11ceu ^ (index * 2246822519u);
    for (int i = 0; i < 4; i++) dspv_rand(&s);
    double p[4];
    dspv_pose(dspv_range(&s, 150, 800), dspv_range(&s, -60, 120), p);
    const double off = (index & 1) ? 2900 : 300;
    p[0] += dspv_range(&s, -off, off); p[1] += dspv_range(&s, -off, off);
    const double h = dspv_range(&s, 0, 2 * M_PI);
    const double f = (index & 2) ? dspv_range(&s, 1, 60) : dspv_range(&s, 60, 4090);
    K->camx = dspv_q(p[0], 65536); K->camz = dspv_q(p[1], 65536);
    K->cx = dspv_q(cos(h), 1073741824.0); K->cz = dspv_q(sin(h), 1073741824.0);
    K->f = dspv_q(f, 65536);
    K->fr = (int32_t)ceil(sqrt((K->f / 65536.0) * (K->f / 65536.0) + 25600) * 65536);
    K->zf = dspv_q((index & 4) ? dspv_range(&s, 0, 60) : dspv_range(&s, 0, 5000), 65536);
    for (int i = 0; i < DSPV_SERIES; i++) {
        K->w[i] = dspv_q(dspv_range(&s, -128, 128), 65536);
        K->m[i] = dspv_q((i & 1) ? dspv_range(&s, 0, 1) : dspv_range(&s, 0, 64), 65536);
    }
}

int main(int argc, char **argv) {
    const unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 200000;
    static dspv_course C;
    static dsp_ends E;
    dsp_rot_k rk;
    dsp_cull_k k;
    dspv_course_init(&C);
    dsp_set_course(&E, DSPV_ENDS, C.nx, C.nz);
    unsigned long unsafe = 0, chords = 0, cull_i = 0, cull_d = 0, calls = 0, scalar_calls = 0, oor_chords = 0;
    double err_rot = 0, err_n = 0, err_ld = 0;
    for (int pop = 0; pop < 2; pop++)
        for (unsigned i = 0; i < n; i++) {
            dspv_case K;
            if (pop) wide_case(&K, i); else dspv_case_make(&K, i);
            dsp_set_camera(&E, &rk, C.px, C.pz, K.camx, K.camz, K.cx, K.cz);
            dsp_rotate_ref(&E, &rk);
            const double cx = K.cx / 1073741824.0, cz = K.cz / 1073741824.0;
            for (unsigned e = 0; e < E.ends; e++) {
                if (!(E.in[e / 8].v[0][e % 8] || E.in[e / 8].v[1][e % 8])) continue;
                const double x = ((int64_t)C.px[e] - K.camx) / 65536.0, z = ((int64_t)C.pz[e] - K.camz) / 65536.0,
                             nx = C.nx[e] / 1073741824.0, nz = C.nz[e] / 1073741824.0;
                const dsp_blk *b = &E.out[e / 8];
                const int L = (int)(e % 8);
                const double r0 = fabs(b->v[0][L] - 8 * (x * cz - z * cx)), r1 = fabs(b->v[1][L] - 8 * (x * cx + z * cz)),
                             n0 = fabs(b->v[2][L] - 16384 * (nx * cz - nz * cx)), n1 = fabs(b->v[3][L] - 16384 * (nx * cx + nz * cz));
                if (r0 > err_rot) err_rot = r0;
                if (r1 > err_rot) err_rot = r1;
                if (n0 > err_n) err_n = n0;
                if (n1 > err_n) err_n = n1;
                for (int s = 0; s < DSPV_SERIES; s++) {
                    const int32_t W = (K.w[s] + 4096) >> 13;
                    if (K.w[s] < -(1 << 23) || K.w[s] > (1 << 23)) continue;
                    const double w = K.w[s] / 65536.0;
                    const double l3 = sat16((b->v[0][L] * 16384 + b->v[2][L] * W) >> 14),
                                 d3 = sat16((b->v[1][L] * 16384 + b->v[3][L] * W) >> 14);
                    const double el = fabs(l3 - 8 * (x * cz - z * cx + w * (nx * cz - nz * cx))),
                                 ed = fabs(d3 - 8 * (x * cx + z * cz + w * (nx * cx + nz * cz)));
                    if (el > err_ld) err_ld = el;
                    if (ed > err_ld) err_ld = ed;
                }
            }
            for (int s = 0; s < DSPV_SERIES; s++) {
                if (!dsp_cull_consts(&k, K.f, K.fr, K.zf, K.w[s], K.m[s])) { scalar_calls++; continue; }
                calls++;
                const uint32_t r = dsp_cull_ref(&E, &k);
                for (unsigned j = 0; j < E.chords; j++) {
                    if (!(E.valid >> j & 1)) { oor_chords++; continue; }
                    chords++;
                    const bool d = dspv_ou_double(&C, &K, j, s), i_out = r >> j & 1;
                    cull_i += i_out; cull_d += d;
                    if (i_out && !d) {
                        if (++unsafe <= 5)
                            printf("UNSAFE pop %d case %u series %d chord %u\n", pop, i, s, j);
                    }
                }
            }
        }
    printf("derby_cull: %u+%u cases, %lu calls (%lu out of range -> scalar), %lu chords (+%lu scalar)\n",
           n, n, calls, scalar_calls, chords, oor_chords);
    printf("  max lane error, Q3 units: rotated %.2f, with w %.2f (budget 4.4); normal %.2f Q14 units\n",
           err_rot, err_ld, err_n);
    printf("  culled: double %lu, integer %lu (%.2f%% of the double's culls kept by the 1 m margin)\n",
           cull_d, cull_i, cull_d ? 100.0 * (double)(cull_d - cull_i) / (double)cull_d : 0.0);
    printf("  unsafe (integer out, double in): mismatches=%lu\n", unsafe);
    return unsafe != 0 || err_ld > 4.4;
}
