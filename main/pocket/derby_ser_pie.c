/* DERBY WATCH ser(): chord cull and chord-end rotation in int16 lanes, PIE
 * kernel and scalar reference. The contract is in derby_ser_pie.h; the design,
 * the error budget and the verification are docs/apps/derby-ser-division.md
 * sec.10. Firmware: compiled only with CONFIG_POCKET_DERBY_SER_PIE (default
 * off). Host: tools/pie/ builds the scalar half with DERBY_SERPIE_HOST and
 * runs the asm text through piesim.py. */
#if defined(DERBY_SERPIE_HOST)
#include "derby_ser_pie.h"
#else
#include "sdkconfig.h"
#endif

#if defined(DERBY_SERPIE_HOST) || defined(CONFIG_POCKET_DERBY_SER_PIE)
#include "derby_ser_pie.h"
#include <string.h>

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static int64_t floor_div(int64_t a, int64_t b) { int64_t q = a / b; return (a % b && (a < 0) != (b < 0)) ? q - 1 : q; }
static int64_t ceil_div(int64_t a, int64_t b) { return -floor_div(-a, b); }
static int16_t clamp16(int64_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
/* Q30 -> Q14 rounded half up (an arithmetic shift is a floor); Q16 -> Q3 the
 * same way, inline in dsp_set_camera(). */
static int32_t q30_q14(int32_t v) { return (int32_t)(((int64_t)v + 32768) >> 16); }

void dsp_set_course(dsp_ends *E, unsigned ends, const int32_t *nx, const int32_t *nz) {
    memset(E, 0, sizeof *E);
    if (ends > DSP_ENDS_MAX) ends = 0;
    E->ends = ends;
    E->chords = ends ? ends - 1 : 0;
    E->eblk = (E->chords + DSP_LANES - 1) / DSP_LANES + 1;
    for (unsigned i = 0; i < ends; i++) {
        E->in[i / 8].v[2][i % 8] = clamp16(q30_q14(nx[i]));
        E->in[i / 8].v[3][i % 8] = clamp16(q30_q14(nz[i]));
    }
}

void dsp_set_camera(dsp_ends *E, dsp_rot_k *rk, const int32_t *px, const int32_t *pz,
                    int32_t camx, int32_t camz, int32_t cx, int32_t cz) {
    uint64_t ok = 0;
    for (unsigned i = 0; i < E->ends; i++) {
        const int64_t x = ((int64_t)px[i] - camx + 4096) >> 13, z = ((int64_t)pz[i] - camz + 4096) >> 13;
        const bool in = x >= -DSP_REL_MAX_Q3 && x <= DSP_REL_MAX_Q3 && z >= -DSP_REL_MAX_Q3 && z <= DSP_REL_MAX_Q3;
        E->in[i / 8].v[0][i % 8] = in ? (int16_t)x : 0;
        E->in[i / 8].v[1][i % 8] = in ? (int16_t)z : 0;
        ok |= (uint64_t)in << i;
    }
    E->valid = (uint32_t)(ok & ok >> 1 & (((uint64_t)1 << E->chords) - 1));
    const int16_t c = clamp16(q30_q14(cx)), s = clamp16(q30_q14(cz));
    memset(rk, 0, sizeof *rk);
    rk->c[0] = s; rk->c[1] = (int16_t)-c; rk->c[2] = c;
}

bool dsp_cull_consts(dsp_cull_k *k, int32_t f, int32_t fr, int32_t zf, int32_t w, int32_t m) {
    if (f < 65536 || f >= (1 << 28) - 4096 || fr < f || fr < 160 * 65536 || (int64_t)fr > 4100LL * 65536 ||
        zf < 0 || w < -(1 << 23) || w > (1 << 23) || m < 0 || m > 64 * 65536)
        return false;
    memset(k, 0, sizeof *k);
    const int64_t m1 = (int64_t)m + 65536;                    /* m + 1 m, Q16 */
    const int64_t K6 = ceil_div(m1 * fr, (int64_t)1 << 26) + 64 * 200;
    const int64_t KA = ceil_div(K6, 1024);
    if (KA > 32767) return false;
    k->c[DSPK_W] = (int16_t)((w + 4096) >> 13);
    k->c[DSPK_ONE] = 16384;
    k->c[DSPK_TN] = clamp16(floor_div(8 * (int64_t)f - 400 * m1, 50 * 65536));
    k->c[DSPK_TF] = clamp16(ceil_div((int64_t)zf + m1, 8192));
    k->c[DSPK_B1] = 1; k->c[DSPK_B2] = 2; k->c[DSPK_B4] = 4; k->c[DSPK_B8] = 8;
    k->c[DSPK_C1280] = 1280;
    k->c[DSPK_KA] = (int16_t)KA; k->c[DSPK_KB] = 1024;
    k->c[DSPK_F] = (int16_t)((f + 4096) >> 13);
    k->c[DSPK_NF] = (int16_t)-k->c[DSPK_F];
    for (int i = 0; i < 8; i++) k->negw[i] = (int16_t)-(1 << i);
    return true;
}

/* ---- scalar reference: what each lane of the kernels computes ----
 * Every sum fits int32 for any int16 lanes, so plain int32 is exact: the
 * rotation's are at most 2 x 32768 x 16384 = 2^30, l's and d's at most
 * 32768 x 16384 + 32768^2 < 1.62e9, and the planes' at most
 * 1280 x 32768 + K6 + 32767 x 32768 < 1.14e9 (dsp_cull_consts() keeps
 * KA x KB = 1024 ceil(K6 / 1024) <= 17.1e6). QACC (40-bit lanes) holds the
 * same sums exactly, and SRCMB of an exact sum is the reference's
 * sat16(sum >> s) (TRM p.130: arithmetic shift, then saturate). */
void dsp_rotate_ref(dsp_ends *E, const dsp_rot_k *rk) {
    const int32_t cz = rk->c[0], ncx = rk->c[1], cx = rk->c[2];
    /* The real ends only, as a scalar ser() would: the padding lanes past
     * them hold zero inputs, which the kernel turns into zero outputs, and
     * dsp_set_course() left their outputs zero. */
    for (unsigned e = 0; e < E->ends; e++) {
        const dsp_blk *i = &E->in[e / 8];
        dsp_blk *o = &E->out[e / 8];
        const int L = (int)(e % 8);
        {
            const int32_t rx = i->v[0][L], rz = i->v[1][L], nx = i->v[2][L], nz = i->v[3][L];
            o->v[0][L] = sat16((rx * cz + rz * ncx) >> 14);
            o->v[1][L] = sat16((rx * cx + rz * cz) >> 14);
            o->v[2][L] = sat16((nx * cz + nz * ncx) >> 14);
            o->v[3][L] = sat16((nx * cx + nz * cz) >> 14);
        }
    }
}

/* The plane flags of one end: bit 0 near, 1 far, 2 left, 3 right. */
static inline unsigned end_flags(const dsp_blk *b, int L, const int16_t *c) {
    const int32_t l = sat16((b->v[0][L] * c[DSPK_ONE] + b->v[2][L] * c[DSPK_W]) >> 14),
                  d = sat16((b->v[1][L] * c[DSPK_ONE] + b->v[3][L] * c[DSPK_W]) >> 14),
                  P = d * c[DSPK_C1280] + c[DSPK_KA] * c[DSPK_KB], fl = c[DSPK_F] * l;
    return (unsigned)(d < c[DSPK_TN]) | (unsigned)(c[DSPK_TF] < d) << 1 |
           (unsigned)(P + fl < 0) << 2 | (unsigned)(P - fl < 0) << 3;
}

uint32_t dsp_cull_ref(const dsp_ends *E, const dsp_cull_k *k) {
    uint32_t out = 0;
    unsigned prev = end_flags(&E->out[0], 0, k->c);
    for (unsigned j = 0; j < E->chords; j++) {
        const unsigned e = j + 1, next = end_flags(&E->out[e / 8], (int)(e % 8), k->c);
        out |= (uint32_t)((prev & next) != 0) << j;
        prev = next;
    }
    return out & E->valid;
}

/* ---- PIE kernels ---- */
#if defined(__XTENSA__) && defined(CONFIG_POCKET_DERBY_SER_PIE)
#include <stdint.h>
#include "esp_cpu.h"
#include "esp_log.h"

/* Rotation: per block of 8 ends, 4 loads, 4 x (ZERO.QACC, 2 VMULAS, SRCMB by
 * 14, store). SRCMB is an arithmetic shift (a floor) with saturation (TRM
 * p.130), which is dsp_rotate_ref()'s `sat16(x >> 14)`. */
void dsp_rotate_pie(dsp_ends *E, const dsp_rot_k *rk) {
    const dsp_blk *in = E->in;
    dsp_blk *out = E->out;
    const int16_t *kp = rk->c;
    const int n = (int)E->eblk, sh = 14;
    __asm__ volatile(
        "ee.vldbc.16.ip  q5, %[k], 2\n"          /* cz */
        "ee.vldbc.16.ip  q6, %[k], 2\n"          /* -cx */
        "ee.vldbc.16.ip  q7, %[k], 0\n"          /* cx */
        "loopgtz %[n], 1f\n"
        "ee.vld.128.ip   q0, %[in], 16\n"        /* rx */
        "ee.vld.128.ip   q1, %[in], 16\n"        /* rz */
        "ee.zero.qacc\n"
        "ee.vmulas.s16.qacc q0, q5\n"
        "ee.vmulas.s16.qacc q1, q6\n"
        "ee.srcmb.s16.qacc q2, %[sh], 0\n"       /* lat = (rx cz - rz cx) >> 14 */
        "ee.zero.qacc\n"
        "ee.vmulas.s16.qacc q0, q7\n"
        "ee.vmulas.s16.qacc q1, q5\n"
        "ee.srcmb.s16.qacc q3, %[sh], 0\n"       /* dep = (rx cx + rz cz) >> 14 */
        "ee.vld.128.ip   q0, %[in], 16\n"        /* nx */
        "ee.vld.128.ip   q1, %[in], 16\n"        /* nz */
        "ee.vst.128.ip   q2, %[out], 16\n"
        "ee.vst.128.ip   q3, %[out], 16\n"
        "ee.zero.qacc\n"
        "ee.vmulas.s16.qacc q0, q5\n"
        "ee.vmulas.s16.qacc q1, q6\n"
        "ee.srcmb.s16.qacc q2, %[sh], 0\n"       /* nlat */
        "ee.zero.qacc\n"
        "ee.vmulas.s16.qacc q0, q7\n"
        "ee.vmulas.s16.qacc q1, q5\n"
        "ee.srcmb.s16.qacc q3, %[sh], 0\n"       /* ndep */
        "ee.vst.128.ip   q2, %[out], 16\n"
        "ee.vst.128.ip   q3, %[out], 16\n"
        "1:\n"
        : [in] "+&a"(in), [out] "+&a"(out), [k] "+&a"(kp)
        : [n] "a"(n), [sh] "a"(sh)
        : "memory");
}

/* Cull: per block of 8 ends e, the plane flags F_e (bits 1/2/4/8 per lane);
 * then chord block e-1 = F_{e-1} & (F_{e-1}, F_e shifted down one lane by
 * EE.SRC.Q with SAR_BYTE = 2), nonzero lanes -> -1, folded to 8 bits by
 * EE.VMULAS.S16.ACCX against (-1, -2, ..., -128). Iteration 0 folds a
 * garbage block into m[0], which is dropped: the loop body stays one piece
 * with no peeled first block. q6 = 0, q7 = F_{e-1}. */
uint32_t dsp_cull_pie(const dsp_ends *E, const dsp_cull_k *k) {
    uint8_t m[DSP_EBLK_MAX];
    const dsp_blk *v = E->out;
    const int16_t *kp = k->c;
    uint8_t *mp = m;
    const int n = (int)E->eblk, s14 = 14, s0 = 0, two = 2;
    int t;
    __asm__ volatile(
        "wur.sar_byte    %[two]\n"
        "ee.zero.q       q6\n"
        "ee.zero.q       q7\n"
        "loopgtz %[n], 1f\n"
        "ee.vldbc.16.ip  q4, %[k], 2\n"          /* W */
        "ee.vldbc.16.ip  q5, %[k], 2\n"          /* 16384 */
        "ee.vld.128.ip   q0, %[v], 16\n"         /* lat */
        "ee.vld.128.ip   q1, %[v], 16\n"         /* dep */
        "ee.vld.128.ip   q2, %[v], 16\n"         /* nlat */
        "ee.vld.128.ip   q3, %[v], 16\n"         /* ndep */
        "ee.zero.qacc\n"
        "ee.vmulas.s16.qacc q0, q5\n"
        "ee.vmulas.s16.qacc q2, q4\n"
        "ee.srcmb.s16.qacc q0, %[s14], 0\n"      /* l = lat + w nlat */
        "ee.zero.qacc\n"
        "ee.vmulas.s16.qacc q1, q5\n"
        "ee.vmulas.s16.qacc q3, q4\n"
        "ee.srcmb.s16.qacc q1, %[s14], 0\n"      /* d = dep + w ndep */
        "ee.vldbc.16.ip  q2, %[k], 2\n"          /* TN */
        "ee.vldbc.16.ip  q3, %[k], 2\n"          /* TF */
        "ee.vcmp.lt.s16  q2, q1, q2\n"           /* near: d < TN */
        "ee.vcmp.lt.s16  q3, q3, q1\n"           /* far: TF < d */
        "ee.vldbc.16.ip  q4, %[k], 2\n"          /* 1 */
        "ee.vldbc.16.ip  q5, %[k], 2\n"          /* 2 */
        "ee.andq         q2, q2, q4\n"
        "ee.andq         q3, q3, q5\n"
        "ee.orq          q2, q2, q3\n"
        "ee.vldbc.16.ip  q3, %[k], 2\n"          /* 1280 */
        "ee.vldbc.16.ip  q4, %[k], 2\n"          /* KA */
        "ee.vldbc.16.ip  q5, %[k], 2\n"          /* KB */
        "ee.zero.qacc\n"
        "ee.vmulas.s16.qacc q1, q3\n"            /* 1280 d */
        "ee.vmulas.s16.qacc q4, q5\n"            /* + K */
        "ee.vldbc.16.ip  q3, %[k], 2\n"          /* F */
        "ee.vldbc.16.ip  q4, %[k], 2\n"          /* -F */
        "ee.vmulas.s16.qacc q0, q3\n"            /* + F l */
        "ee.srcmb.s16.qacc q3, %[s0], 0\n"       /* left: sign kept, QACC unchanged */
        "ee.vmulas.s16.qacc q0, q4\n"
        "ee.vmulas.s16.qacc q0, q4\n"            /* - 2 F l */
        "ee.srcmb.s16.qacc q4, %[s0], 0\n"       /* right */
        "ee.vldbc.16.ip  q0, %[k], 2\n"          /* 4 */
        "ee.vldbc.16.ip  q1, %[k], 2\n"          /* 8 */
        "ee.vcmp.lt.s16  q3, q3, q6\n"
        "ee.vcmp.lt.s16  q4, q4, q6\n"
        "ee.andq         q3, q3, q0\n"
        "ee.andq         q4, q4, q1\n"
        "ee.orq          q2, q2, q3\n"
        "ee.orq          q2, q2, q4\n"           /* F_e */
        "addi            %[k], %[k], 6\n"        /* -> negw at byte 32 */
        "ee.vld.128.ip   q1, %[k], -32\n"        /* weights; k back to the table */
        "ee.src.q        q0, q7, q2\n"           /* F of ends 8(e-1)+1 .. 8e */
        "ee.andq         q0, q0, q7\n"           /* both ends out by one plane */
        "ee.vcmp.lt.s16  q0, q6, q0\n"           /* -> 0xFFFF */
        "ee.zero.accx\n"
        "ee.vmulas.s16.accx q0, q1\n"            /* ACCX = the 8-bit chord mask */
        "ee.orq          q7, q2, q2\n"
        "rur.accx_0      %[t]\n"
        "s8i             %[t], %[m], 0\n"
        "addi            %[m], %[m], 1\n"
        "1:\n"
        : [v] "+&a"(v), [k] "+&a"(kp), [m] "+&a"(mp), [t] "=&a"(t)
        : [n] "a"(n), [s14] "a"(s14), [s0] "a"(s0), [two] "a"(two)
        : "memory");
    uint32_t out = 0;
    for (unsigned b = 0; b + 1 < E->eblk; b++) out |= (uint32_t)m[b + 1] << (8 * b);
    return out & E->valid;
}

/* ---- device self-test and microbenchmark ---- */
#include "derby_ser_pie_vec.h"

#ifndef DSP_SELFTEST_CASES
#define DSP_SELFTEST_CASES 512
#endif
#ifndef DSP_SELFTEST_DOUBLE
#define DSP_SELFTEST_DOUBLE 64
#endif

static dsp_ends st_a, st_b;   /* PIE and reference copies (about 0.7 KB each) */

void dsp_selftest(void) {
    static dspv_course C;
    static dsp_rot_k rk;
    static dsp_cull_k kk;
    dspv_course_init(&C);
    dsp_set_course(&st_a, DSPV_ENDS, C.nx, C.nz);
    unsigned ok = 0, mismatch = 0, rot_mismatch = 0, unsafe = 0, scalar_calls = 0, valid_chords = 0,
             culled = 0, culled_double = 0, frames = 0;
    uint32_t rot_s = 0, rot_p = 0, cull_s = 0, cull_p = 0;
    uint32_t min_rs = UINT32_MAX, min_rp = UINT32_MAX, min_cs = UINT32_MAX, min_cp = UINT32_MAX;
    for (unsigned i = 0; i < DSP_SELFTEST_CASES; i++) {
        dspv_case K;
        dspv_case_make(&K, i);
        dsp_set_camera(&st_a, &rk, C.px, C.pz, K.camx, K.camz, K.cx, K.cz);
        st_b = st_a;
        uint32_t t0 = esp_cpu_get_cycle_count();
        dsp_rotate_ref(&st_b, &rk);
        uint32_t t1 = esp_cpu_get_cycle_count();
        dsp_rotate_pie(&st_a, &rk);
        uint32_t t2 = esp_cpu_get_cycle_count();
        rot_s += t1 - t0; rot_p += t2 - t1;
        if (t1 - t0 < min_rs) min_rs = t1 - t0;
        if (t2 - t1 < min_rp) min_rp = t2 - t1;
        if (memcmp(st_a.out, st_b.out, sizeof st_a.out)) rot_mismatch++;
        frames++;
        for (int s = 0; s < DSPV_SERIES; s++) {
            if (!dsp_cull_consts(&kk, K.f, K.fr, K.zf, K.w[s], K.m[s])) { scalar_calls++; continue; }
            t0 = esp_cpu_get_cycle_count();
            const uint32_t r = dsp_cull_ref(&st_b, &kk);
            t1 = esp_cpu_get_cycle_count();
            const uint32_t p = dsp_cull_pie(&st_a, &kk);
            t2 = esp_cpu_get_cycle_count();
            cull_s += t1 - t0; cull_p += t2 - t1;
            if (t1 - t0 < min_cs) min_cs = t1 - t0;
            if (t2 - t1 < min_cp) min_cp = t2 - t1;
            if (r == p) ok++; else mismatch++;
            /* The double ou() is soft-float here (about 4k cycles a chord):
             * the device checks the first DSP_SELFTEST_DOUBLE cases, the host
             * model all of them and 400k more (tools/pie/run_models.py). */
            for (unsigned j = 0; i < DSP_SELFTEST_DOUBLE && j < st_a.chords; j++) {
                if (!(st_a.valid >> j & 1)) continue;
                valid_chords++;
                const bool d = dspv_ou_double(&C, &K, j, s);
                culled += r >> j & 1; culled_double += d;
                if ((r >> j & 1) && !d) unsafe++;
            }
        }
    }
    /* scalar_cy / pie_cy: one frame's work (one rotation + six culls; DERBY's
     * view() and five ser() calls), from the mean per call. The *_min fields
     * are the fastest single call, the least disturbed by preemption.
     * Measured on the same binary, same inputs, reference first. */
    const unsigned calls = ok + mismatch ? ok + mismatch : 1;
    ESP_LOGI("derby", "SERPIE ok=%u mismatch=%u scalar_cy=%u pie_cy=%u rot_mismatch=%u unsafe=%u "
             "cases=%u calls=%u scalar_calls=%u rot_scalar_cy=%u rot_pie_cy=%u cull_scalar_cy=%u "
             "cull_pie_cy=%u rot_min=%u/%u cull_min=%u/%u double_chords=%u culled=%u culled_double=%u",
             ok, mismatch,
             (unsigned)(rot_s / frames + 6u * (cull_s / calls)), (unsigned)(rot_p / frames + 6u * (cull_p / calls)),
             rot_mismatch, unsafe, frames, calls, scalar_calls,
             (unsigned)(rot_s / frames), (unsigned)(rot_p / frames),
             (unsigned)(cull_s / calls), (unsigned)(cull_p / calls),
             (unsigned)min_rs, (unsigned)min_rp, (unsigned)min_cs, (unsigned)min_cp,
             valid_chords, culled, culled_double);
}
#endif /* __XTENSA__ && CONFIG_POCKET_DERBY_SER_PIE */
#endif /* DERBY_SERPIE_HOST || CONFIG_POCKET_DERBY_SER_PIE */
