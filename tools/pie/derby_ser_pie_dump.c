/* Host side of tools/pie/test_derby_ser_pie.py: runs the scalar reference of
 * main/pocket/derby_ser_pie.c on the shared vectors (derby_ser_pie_vec.h) and
 * prints, per case, the kernel inputs and the reference outputs as integers,
 * so piesim.py can run the asm on the same inputs and compare.
 *
 *   R <eblk> <ends> <valid> <rot k x3> <in: eblk*32 int16> <out: eblk*32 int16>
 *   C <series> <13 consts> <ref mask>          (one per series in range)
 *   S <series>                                 (series out of range)
 *
 * After the n geometric cases come n synthetic ones: random lanes and random
 * constants inside the ranges the header allows, all 32 chords valid. The
 * course's geometry rarely has a chord with one end near and the other far,
 * or one end left and the other right; these do, so a kernel that merges two
 * plane bits fails here even where the geometry cannot tell. */
#define DERBY_SERPIE_HOST 1
#include <stdio.h>
#include <stdlib.h>
#include "../../main/pocket/derby_ser_pie.c"
#include "../../main/pocket/derby_ser_pie_vec.h"

int main(int argc, char **argv) {
    const unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 512;
    static dspv_course C;
    static dsp_ends E;
    dsp_rot_k rk;
    dsp_cull_k k;
    dspv_course_init(&C);
    dsp_set_course(&E, DSPV_ENDS, C.nx, C.nz);
    for (unsigned i = 0; i < n; i++) {
        dspv_case K;
        dspv_case_make(&K, i);
        dsp_set_camera(&E, &rk, C.px, C.pz, K.camx, K.camz, K.cx, K.cz);
        dsp_rotate_ref(&E, &rk);
        printf("R %u %u %u %d %d %d", E.eblk, E.ends, E.valid, rk.c[0], rk.c[1], rk.c[2]);
        for (unsigned b = 0; b < E.eblk; b++)
            for (int r = 0; r < 4; r++)
                for (int L = 0; L < 8; L++) printf(" %d", E.in[b].v[r][L]);
        for (unsigned b = 0; b < E.eblk; b++)
            for (int r = 0; r < 4; r++)
                for (int L = 0; L < 8; L++) printf(" %d", E.out[b].v[r][L]);
        printf("\n");
        for (int s = 0; s < DSPV_SERIES; s++) {
            if (!dsp_cull_consts(&k, K.f, K.fr, K.zf, K.w[s], K.m[s])) { printf("S %d\n", s); continue; }
            printf("C %d", s);
            for (int c = 0; c < DSPK_N; c++) printf(" %d", k.c[c]);
            printf(" %u\n", dsp_cull_ref(&E, &k));
        }
    }
    uint32_t seed = 12345;
    static const int32_t zero[DSP_ENDS_MAX];
    for (unsigned i = 0; i < n; i++) {
        dsp_set_course(&E, DSP_ENDS_MAX, zero, zero);   /* 33 ends: 32 chords, 5 blocks */
        for (unsigned e = 0; e < DSP_ENDS_MAX; e++) {
            dsp_blk *b = &E.in[e / 8];
            b->v[0][e % 8] = (int16_t)(dspv_rand(&seed) % 32753 - 16376);
            b->v[1][e % 8] = (int16_t)(dspv_rand(&seed) % 32753 - 16376);
            b->v[2][e % 8] = (int16_t)(dspv_rand(&seed) % 32769 - 16384);
            b->v[3][e % 8] = (int16_t)(dspv_rand(&seed) % 32769 - 16384);
        }
        E.valid = 0xFFFFFFFFu;
        const int16_t c = (int16_t)(dspv_rand(&seed) % 32769 - 16384), sn = (int16_t)(dspv_rand(&seed) % 32769 - 16384);
        rk.c[0] = sn; rk.c[1] = (int16_t)-c; rk.c[2] = c;
        dsp_rotate_ref(&E, &rk);
        printf("R %u %u %u %d %d %d", E.eblk, E.ends, E.valid, rk.c[0], rk.c[1], rk.c[2]);
        for (unsigned b = 0; b < E.eblk; b++)
            for (int r = 0; r < 4; r++)
                for (int L = 0; L < 8; L++) printf(" %d", E.in[b].v[r][L]);
        for (unsigned b = 0; b < E.eblk; b++)
            for (int r = 0; r < 4; r++)
                for (int L = 0; L < 8; L++) printf(" %d", E.out[b].v[r][L]);
        printf("\n");
        for (int s = 0; s < DSPV_SERIES; s++) {
            dsp_cull_consts(&k, 100 << 16, 189 << 16, 100 << 16, 0, 0);   /* the fixed fields */
            k.c[DSPK_W] = (int16_t)(dspv_rand(&seed) % 2049 - 1024);
            k.c[DSPK_TN] = (int16_t)(dspv_rand(&seed) % 8001 - 6000);
            k.c[DSPK_TF] = (int16_t)(dspv_rand(&seed) % 8001 - 2000);
            k.c[DSPK_F] = (int16_t)(dspv_rand(&seed) % 32768);
            k.c[DSPK_NF] = (int16_t)-k.c[DSPK_F];
            k.c[DSPK_KA] = (int16_t)(dspv_rand(&seed) % 16671);
            printf("C %d", s);
            for (int c2 = 0; c2 < DSPK_N; c2++) printf(" %d", k.c[c2]);
            printf(" %u\n", dsp_cull_ref(&E, &k));
        }
    }
    return 0;
}
