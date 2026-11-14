/* Fixed test vectors for the ser() PIE prototype (derby_ser_pie.h), shared
 * by the device self-test and the host checks (tools/pie/models/
 * derby_cull_model.c, tools/pie/test_derby_ser_pie.py), so the host and the
 * device check the same cases.
 *
 * The course is DERBY WATCH's oval as tools/games/serq_proto/serq.c builds it
 * (19 chord ends: the two +-10,000 m straights' ends, which are out of the
 * kernel's range on purpose, and 17 ends around the oval). Cameras come from a
 * fixed LCG: a point near the course looking any way, f 80..2,400 px, the far
 * depth 20..1,600 m, and one case in eight moved up to 1,800 m off the course
 * so that ends cross the +-2,047 m range edge. Each case has six ou() series:
 * view()'s (25.5 m, 30 m) and five ser()-like (w -30..60 m, m 0.25..40 m).
 *
 * dsp_vec_ou_double() is pocket_derby.c's ou() on the same Q16/Q30 inputs in
 * double: the integer cull must never call out a chord this calls in. */
#ifndef DERBY_SER_PIE_VEC_H
#define DERBY_SER_PIE_VEC_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DSPV_ENDS 19
#define DSPV_SERIES 6

typedef struct {
    int32_t px[DSPV_ENDS], pz[DSPV_ENDS];   /* Q16 m */
    int32_t nx[DSPV_ENDS], nz[DSPV_ENDS];   /* Q30 unit normal */
} dspv_course;

typedef struct {
    int32_t camx, camz, cx, cz;             /* Q16 m, Q30 */
    int32_t f, fr, zf;                       /* Q16 */
    int32_t w[DSPV_SERIES], m[DSPV_SERIES];  /* Q16 */
} dspv_case;

static double dspv_sec[3][7];

static inline void dspv_pose(double g, double w, double o[4]) {
    const double dnr = 11;
    int i = 0;
    while (i < 2 && g > dspv_sec[i][0]) g -= dspv_sec[i++][0];
    const double *e = dspv_sec[i];
    double x = e[2], z = e[3], c = e[4], d = e[5];
    if (e[1] != 0) {
        const double r = 1 / e[1] + dnr, C = cos(e[6] + e[1] * g), S = sin(e[6] + e[1] * g);
        x += (S - d) * r; z -= (C - c) * r; c = C; d = S; g = 0;
    }
    o[0] = x + g * c - w * d; o[1] = z + g * d + w * c; o[2] = c; o[3] = d;
}

static inline int32_t dspv_q(double v, double scale) {
    double x = floor(v * scale + 0.5);
    return x > 2147483647.0 ? 2147483647 : x < -2147483648.0 ? (int32_t)-2147483647 - 1 : (int32_t)x;
}

static inline void dspv_course_init(dspv_course *C) {
    const double QM = 65536.0, OB = 17891910;
    static const double OC[3][7] = {{OB, 0, 60490310, -14286848, -65536, 0, 0},
                                    {24706490, -7864320, 42598400, -14286848, -65536, 0, 32768},
                                    {131072000, 0, 42598400, 0, 65536, 0, 0}};
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 7; k++) dspv_sec[i][k] = OC[i][k] / QM;
        if (dspv_sec[i][1] != 0) { dspv_sec[i][1] = 1 / dspv_sec[i][1]; dspv_sec[i][6] *= 2 * M_PI; }
    }
    double ch[DSPV_ENDS];
    ch[0] = -655360000 / QM;
    for (int j = 0; j <= 16; j++) ch[1 + j] = (OB + j * 1544156.0) / QM;
    ch[18] = 655360000 / QM;
    for (int i = 0; i < DSPV_ENDS; i++) {
        double m[4];
        dspv_pose(ch[i], 0, m);
        C->px[i] = dspv_q(m[0], QM); C->pz[i] = dspv_q(m[1], QM);
        C->nx[i] = dspv_q(-m[3], 1073741824.0); C->nz[i] = dspv_q(m[2], 1073741824.0);
    }
}

static inline uint32_t dspv_rand(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
static inline double dspv_unit(uint32_t *s) { return dspv_rand(s) / 16777216.0; }       /* [0,1) */
static inline double dspv_range(uint32_t *s, double a, double b) { return a + (b - a) * dspv_unit(s); }

static inline void dspv_case_make(dspv_case *K, unsigned index) {
    uint32_t s = 0x5e2d1u + index * 2654435761u;
    for (int i = 0; i < 4; i++) dspv_rand(&s);
    double p[4];
    dspv_pose(dspv_range(&s, 200, 750), dspv_range(&s, -40, 80), p);
    if ((index & 7) == 7) { p[0] += dspv_range(&s, -1800, 1800); p[1] += dspv_range(&s, -1800, 1800); }
    const double h = dspv_range(&s, 0, 2 * M_PI), f = dspv_range(&s, 80, 2400);
    K->camx = dspv_q(p[0], 65536); K->camz = dspv_q(p[1], 65536);
    K->cx = dspv_q(cos(h), 1073741824.0); K->cz = dspv_q(sin(h), 1073741824.0);
    K->f = dspv_q(f, 65536);
    K->fr = (int32_t)ceil(sqrt((K->f / 65536.0) * (K->f / 65536.0) + 25600) * 65536);
    K->zf = dspv_q(dspv_range(&s, 20, 1600), 65536);
    K->w[0] = dspv_q(25.5, 65536); K->m[0] = dspv_q(30, 65536);
    for (int i = 1; i < DSPV_SERIES; i++) {
        K->w[i] = dspv_q(dspv_range(&s, -30, 60), 65536);
        K->m[i] = dspv_q(dspv_range(&s, 0.25, 40), 65536);
    }
}

/* pocket_derby.c's view() rotation and ou(j, w, m, zf), in double, on the
 * case's own fixed-point inputs. fr is the exact sqrt, not the case's ceiling. */
static inline bool dspv_ou_double(const dspv_course *C, const dspv_case *K, unsigned j, int series) {
    const double cx = K->cx / 1073741824.0, cz = K->cz / 1073741824.0;
    double V[8];
    for (int e = 0; e < 2; e++) {
        const double x = ((int64_t)C->px[j + e] - K->camx) / 65536.0,
                     z = ((int64_t)C->pz[j + e] - K->camz) / 65536.0,
                     nx = C->nx[j + e] / 1073741824.0, nz = C->nz[j + e] / 1073741824.0;
        V[4 * e] = x * cz - z * cx; V[4 * e + 1] = x * cx + z * cz;
        V[4 * e + 2] = nx * cz - nz * cx; V[4 * e + 3] = nx * cx + nz * cz;
    }
    const double f = K->f / 65536.0, fr = sqrt(f * f + 25600), zf = K->zf / 65536.0,
                 w = K->w[series] / 65536.0, m = K->m[series] / 65536.0, k = m * fr,
                 l = V[0] + w * V[2], d = V[1] + w * V[3], L = V[4] + w * V[6], D = V[5] + w * V[7];
    return (d < .02 * f - m && D < .02 * f - m) || (d > zf + m && D > zf + m) ||
           (160 * d + l * f < -k && 160 * D + L * f < -k) || (160 * d - l * f < -k && 160 * D - L * f < -k);
}

#endif
