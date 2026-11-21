/* DERBY WATCH ser(): the chord cull (ou()) and the chord-end rotation in
 * int16 lanes, as a PIE kernel and as the scalar integer reference it must
 * match bit for bit. Stage S3 of docs/apps/derby-ser-division.md (sec.6.3,
 * sec.10, sec.13, sec.14): pocket_derby.c's view() rotates once a frame and
 * culls every series in one batch with these when CONFIG_POCKET_DERBY_SER_PIE
 * is on (default y);
 * the up key's A/B swaps them for derby_ser_int.c's scalar rotation and cull.
 * The host and r5sim (no PIE) run the scalar reference in the kernels' place
 * (DERBY_SERPIE_HOST) or the scalar path.
 *
 * The data, per frame (all int16, 16-byte aligned, SoA in blocks of 8 ends):
 *   in  block: rx, rz  chord end minus camera, world axes, Q3 m (0.125 m)
 *              nx, nz  the end's unit normal (-tangent z, tangent x), Q14
 *   out block: lat, dep    rx,rz turned into the camera frame, Q3 m
 *              nlat, ndep  the normal turned the same way, Q14
 * (the double ser()'s VE[4j..4j+3] of before, in 16-bit form).
 *
 * Chord j joins ends j and j+1. The cull is the double ou() with the margin
 * widened so that the integer answer "out" implies the double answer "out"
 * (proved on the host by tools/pie/models/derby_cull_model.c, not assumed):
 *   near  d < 0.02 f - m        -> dep < TN,  TN = floor(8 (0.02 f - m - 1))
 *   far   d > zf + m            -> dep > TF,  TF = ceil(8 (zf + m + 1))
 *   left  160 d + l f < -m fr   -> 1280 dep + F lat' + K < 0
 *   right 160 d - l f < -m fr   -> 1280 dep - F lat' + K < 0
 * with lat' = lat + w nlat, dep' = dep + w ndep (Q3), F = f in Q3 px, and
 * K = 64 ((m + 1) fr + 200) in Q6 (px m): the extra 1 m covers the Q3/Q14
 * rounding (at most 4.4 Q3 units, sec.10.2), the 200 px m the Q3 rounding of
 * f against |l| <= 3,023 m. A chord is out when both ends are out by the same
 * plane, exactly the double ou()'s form.
 *
 * Ranges (checked by the scalar setup; outside them the caller must use its
 * own scalar ou()): |rx|,|rz| <= 2,047 m per end, f < 4,096 px, |w| <= 128 m,
 * 0 <= m <= 64 m. Inside them no lane value saturates and every sum fits int32,
 * which is what lets the scalar reference use plain int32 arithmetic.
 */
#ifndef DERBY_SER_PIE_H
#define DERBY_SER_PIE_H

#include <stdbool.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
/* The per-frame PIE functions (dsp_set_camera, dsp_rotate_pie,
 * dsp_cull_frame, dsp_cull_series, dsp_cull_pie) in IRAM instead of flash: a
 * measurement build's (CONFIG_POCKET_DERBY_SER_PIE_IRAM, default n, or
 * POCKET_DERBY_SER_PIE_IRAM defined here). +1,092 B of DIRAM taken from
 * every app (idf.py size); docs/apps/derby-ser-division.md sec.14. */
#if defined(CONFIG_POCKET_DERBY_SER_PIE_IRAM) && !defined(POCKET_DERBY_SER_PIE_IRAM)
#define POCKET_DERBY_SER_PIE_IRAM 1
#endif
#if defined(POCKET_DERBY_SER_PIE_IRAM) && defined(ESP_PLATFORM)
#include "esp_attr.h"
#define DSP_HOT IRAM_ATTR
#else
#define DSP_HOT
#endif

#define DSP_LANES 8
#define DSP_CBLK_MAX 4                        /* chord blocks: 32 chords */
#define DSP_EBLK_MAX (DSP_CBLK_MAX + 1)       /* end blocks: 40 ends */
#define DSP_CHORDS_MAX (DSP_CBLK_MAX * DSP_LANES)
#define DSP_ENDS_MAX (DSP_CHORDS_MAX + 1)     /* pocket_derby.c's DERBY_ENDS_MAX is 32 */
#define DSP_REL_MAX_Q3 16376                  /* 2,047 m */

typedef struct { int16_t v[4][DSP_LANES]; } dsp_blk;

typedef struct __attribute__((aligned(16))) {
    dsp_blk in[DSP_EBLK_MAX];
    dsp_blk out[DSP_EBLK_MAX];
    unsigned ends, chords, eblk;  /* eblk = chord blocks + 1: the cull reads one block past */
    uint32_t valid;               /* bit j: chord j < chords with both ends in range */
} dsp_ends;

/* The rotation's broadcast constants (Q14): cz, -cx, cx. */
typedef struct __attribute__((aligned(16))) { int16_t c[8]; } dsp_rot_k;

/* The cull's constants, in the order the kernel reads them (13 broadcasts,
 * then the fold weights at byte 32). */
enum { DSPK_W, DSPK_ONE, DSPK_TN, DSPK_TF, DSPK_B1, DSPK_B2, DSPK_C1280, DSPK_KA, DSPK_KB,
       DSPK_F, DSPK_NF, DSPK_B4, DSPK_B8, DSPK_N };
typedef struct __attribute__((aligned(16))) {
    int16_t c[16];                /* DSPK_* then padding to 32 bytes */
    int16_t negw[8];              /* -1, -2, ..., -128: ACCX = the lane mask */
    int32_t f, fr, zf;            /* the frame's (Q16), for dsp_cull_series() */
} dsp_cull_k;

/* Setup (scalar, every build). */
/* Once per course: the chord ends' unit normals, Q30 (pocket_derby.c's pose()
 * gives the tangent (c, d); the normal is (-d, c)). Clears in/out. */
void dsp_set_course(dsp_ends *E, unsigned ends, const int32_t *nx_q30, const int32_t *nz_q30);
/* Per frame: the ends' world positions (end i at px_q16[i * stride],
 * pz_q16[i * stride]) and the camera position, Q16 m, and the camera's unit
 * direction, Q30. Fills in[].rx/rz, valid and the rotation's constants. An
 * end out of range gets zero lanes; its chords leave `valid`. */
void dsp_set_camera(dsp_ends *E, dsp_rot_k *rk, const int32_t *px_q16, const int32_t *pz_q16, unsigned stride,
                    int32_t camx_q16, int32_t camz_q16, int32_t cx_q30, int32_t cz_q30);
/* Per ou() series: f, fr = sqrt(f^2 + 160^2) (px), zf, w and m (m), all Q16.
 * False when a value is out of range: the whole call goes to the scalar ou().
 * The same table in two steps (docs/apps/derby-ser-division.md sec.14): once a
 * frame, dsp_cull_frame() (f, fr, zf: the broadcasts that do not depend on the
 * series, the fold weights), then per series dsp_cull_series() (W, TN, TF,
 * KA only). dsp_cull_consts() is the two in a row; the table is the same
 * byte for byte. A false dsp_cull_frame() leaves the table unusable for every
 * series of the frame. */
bool dsp_cull_consts(dsp_cull_k *k, int32_t f_q16, int32_t fr_q16, int32_t zf_q16,
                     int32_t w_q16, int32_t m_q16);
bool dsp_cull_frame(dsp_cull_k *k, int32_t f_q16, int32_t fr_q16, int32_t zf_q16);
bool dsp_cull_series(dsp_cull_k *k, int32_t w_q16, int32_t m_q16);

/* The scalar integer reference (the specification of the kernels). */
void dsp_rotate_ref(dsp_ends *E, const dsp_rot_k *rk);
/* Bit j set: chord j wholly out. Only bits in E->valid mean anything; the
 * result is already masked with it. */
uint32_t dsp_cull_ref(const dsp_ends *E, const dsp_cull_k *k);

/* The PIE kernels (ESP32-S3 with CONFIG_POCKET_DERBY_SER_PIE only). Same
 * contract as the references; E, rk and k must be 16-byte aligned. */
#if defined(__XTENSA__) && defined(CONFIG_POCKET_DERBY_SER_PIE)
void dsp_rotate_pie(dsp_ends *E, const dsp_rot_k *rk);
uint32_t dsp_cull_pie(const dsp_ends *E, const dsp_cull_k *k);
#ifdef CONFIG_POCKET_DERBY_SER_PIE_SELFTEST
/* The device self-test (docs/apps/derby-ser-division.md sec.10.4): logs one
 * "SERPIE ok=... mismatch=... scalar_cy=... pie_cy=..." line. A measurement
 * build's (CONFIG_POCKET_DERBY_SER_PIE_SELFTEST, default n): its test copies
 * are about 1.5 KB of .bss. */
void dsp_selftest(void);
#endif
#endif

#endif
