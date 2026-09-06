/* PIE (ESP32-S3 SIMD) accelerator for pocketjs_rgb565_render_strip().
 *
 * Two of the three hooks are implemented: fill_rgb565 (opaque rectangle) and
 * blend_a8_rgb565 (8-bit coverage mask x one colour over RGB565). srm stays
 * NULL (never requested on this device, and PIE has no lane permute for the
 * mirrored case). Every hook returns false when it cannot honour the exact
 * contract, and the renderer then draws the op itself.
 *
 * Contract (engine/backends/rgb565/src/lib.rs, MockPpa + core/src/raster.rs
 * blend_rgb565_pixel): `destination` is one strip of `width` x `height`
 * pixels, `rect` and `mask` are strip-local, mask stride == width, and
 *     a  = (mask * global_alpha + 127) / 255
 *     d8 = 5/6/5 -> 8 bit by bit replication: (r5<<3)|(r5>>2), (g6<<2)|(g6>>4), (b5<<3)|(b5>>2)
 *     c  = (s*a + d8*(255-a) + 127) / 255       per channel
 *     out= pack_rgb565(c)  = ((r&0xF8)<<8) | ((g&0xFC)<<3) | (b>>3)
 * The SIMD path below is bit-exact with that formula (verified exhaustively on
 * the host: every s, every 5/6-bit destination value, every alpha; and the
 * whole C function against a reference on random rects/masks with
 * RENDER_ACCEL_HOST_MODEL).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pocketjs/render_rgb565.h"

/* ------------------------------------------------------------------------ */
/* Scalar reference: used for the unaligned head/tail of every row, and as    */
/* the readable definition of what the assembly computes.                     */
/* ------------------------------------------------------------------------ */
static inline uint16_t pack565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t blend_px(uint16_t p, unsigned r, unsigned g, unsigned b, unsigned a) {
    unsigned r5 = (p >> 11) & 31, g6 = (p >> 5) & 63, b5 = p & 31;
    unsigned dr = (r5 << 3) | (r5 >> 2), dg = (g6 << 2) | (g6 >> 4), db = (b5 << 3) | (b5 >> 2);
    unsigned ia = 255 - a;
    return pack565((r * a + dr * ia + 127) / 255,
                   (g * a + dg * ia + 127) / 255,
                   (b * a + db * ia + 127) / 255);
}

/* ------------------------------------------------------------------------ */
/* Row kernels. `dst` and `mask` must be 16-byte aligned, n = 8-pixel blocks. */
/* ------------------------------------------------------------------------ */
#if defined(__XTENSA__) && !defined(RENDER_ACCEL_HOST_MODEL)

/* Fill n blocks of 8 pixels with `color`.  QR: q0 = colour x8. No SAR use. */
static void __attribute__((noinline)) fill_blocks_pie(uint16_t *dst, const uint16_t *color, int n) {
    __asm__ volatile(
        "ee.vldbc.16    q0, %[c]\n"                   /* q0 = colour in all 8 lanes         (1.8.94) */
        "loopgtz        %[n], 1f\n"                   /* zero-overhead loop (n > 0 checked by caller) */
        "  ee.vst.128.ip q0, %[d], 16\n"              /* 8 pixels, dst += 16               (1.8.192) */
        "1:\n"
        : [d] "+a"(dst) : [c] "a"(color), [n] "a"(n) : "memory");
}

/* Blend n blocks of 8 pixels. k[] holds the broadcast constants in the order the
 * walk pointer reads them (see blend_constants()).
 *
 * QR map per block:
 *   q0 = destination pixels -> mask alpha (16-bit lanes, 0..255)
 *   q1 = dst red 8-bit   -> result red      q2 = dst green 8-bit -> result green
 *   q3 = dst blue 8-bit  -> result blue     q4 = broadcast constant / scratch
 *   q5 = lo = min(s,d)   q6 = diff = |s-d|  q7 = a' (alpha, flipped when s<d)
 * Special registers: SAR = 11 (set once, before the loop) for every EE.VMUL.U16;
 *   QACC used per channel (EE.ZERO.QACC / EE.VMULAS.U16.QACC / EE.SRCMB.S16.QACC,
 *   whose shift amount comes from an AR register, not SAR).
 *
 * Per channel the mix (s*a + d*(255-a) + 127)/255 is computed as
 *   lo + floor((diff*a' + 127) / 255),   lo=min(s,d), diff=|s-d|, a' = (s<d) ? 255-a : a
 * (exact rewrite) and floor(x/255) for x <= 65152 as (x + (x>>8) + 1) >> 8, done in the
 * 40-bit QACC lanes: first pass QACC = x, SRCMB>>8 leaves t = x>>8 in QACC, second pass
 * adds x + 1 again and SRCMB>>8 gives (x + t + 1) >> 8.
 */
static void __attribute__((noinline)) blend_blocks_pie(uint16_t *dst, const uint8_t *mask, const int16_t *k, int n) {
    const int16_t *kp;
    int sar = 11, sh8 = 8;
    __asm__ volatile(
        "wsr.sar        %[sar]\n"                     /* SAR = 11 for all EE.VMUL.U16 below (1.8.128) */
        "2:\n"                                        /* software loop: body is > 256 bytes, so no LOOP insn */
        "  mov          %[kp], %[k]\n"                /* rewind the constant walk */
        "  ee.vld.128.ip   q0, %[d], 0\n"             /* q0 = 8 destination pixels          (1.8.88) */
        /* ---- unpack RGB565 -> 8-bit with bit replication ---- */
        "  ee.vldbc.16.ip  q1, %[kp], 2\n"            /* 1                                  (1.8.95) */
        "  ee.vmul.u16     q1, q0, q1\n"              /* r5 = p*1 >> 11 */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* 64 */
        "  ee.vmul.u16     q2, q0, q2\n"              /* p*64 >> 11 = p >> 5 */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 63 */
        "  ee.andq         q2, q2, q4\n"              /* g6                                 (1.8.1) */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 31 */
        "  ee.andq         q3, q0, q4\n"              /* b5 */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 16384: x*16384>>11 = x<<3 */
        "  ee.vldbc.16.ip  q5, %[kp], 2\n"            /* 512:   x*512>>11   = x>>2 */
        "  ee.vmul.u16     q6, q1, q4\n"              /* r5<<3 */
        "  ee.vmul.u16     q7, q1, q5\n"              /* r5>>2 */
        "  ee.orq          q1, q6, q7\n"              /* dr = (r5<<3)|(r5>>2)               (1.8.45) */
        "  ee.vmul.u16     q6, q3, q4\n"              /* b5<<3 */
        "  ee.vmul.u16     q7, q3, q5\n"              /* b5>>2 */
        "  ee.orq          q3, q6, q7\n"              /* db */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 8192: x*8192>>11 = x<<2 */
        "  ee.vldbc.16.ip  q5, %[kp], 2\n"            /* 128:  x*128>>11  = x>>4 */
        "  ee.vmul.u16     q6, q2, q4\n"              /* g6<<2 */
        "  ee.vmul.u16     q7, q2, q5\n"              /* g6>>4 */
        "  ee.orq          q2, q6, q7\n"              /* dg */
        /* ---- mask: 8 bytes -> 8 x 16-bit lanes ---- */
        "  ee.vld.l.64.ip  q0, %[m], 8\n"             /* q0[63:0] = 8 mask bytes, mask += 8 (1.8.92) */
        "  ee.zero.q       q4\n"                      /* q4 = 0                             (1.8.216) */
        "  ee.vzip.8       q0, q4\n"                  /* q0 = {m0,0,m1,0,...}: alpha x8      (1.8.212) */
        /* ---- red: q1 = lo + floor((diff*a'+127)/255) ---- */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* s_r */
        "  ee.vmin.s16     q5, q4, q1\n"              /* lo   = min(s,d)                    (1.8.113) */
        "  ee.vmax.s16     q6, q4, q1\n"              /* hi   = max(s,d)                    (1.8.104) */
        "  ee.vsubs.s16    q6, q6, q5\n"              /* diff = hi-lo                       (1.8.198) */
        "  ee.vcmp.lt.s16  q7, q4, q1\n"              /* 0xFFFF where s<d                   (1.8.85) */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 0x00FF */
        "  ee.andq         q7, q7, q4\n"              /* 0x00FF where s<d */
        "  ee.xorq         q7, q0, q7\n"              /* a' = a ^ mask = (s<d) ? 255-a : a  (1.8.214) */
        "  ee.zero.qacc\n"                            /*                                    (1.8.217) */
        "  ee.vmulas.u16.qacc q6, q7\n"               /* QACC  = diff*a'                    (1.8.163) */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 127 */
        "  ee.vldbc.16.ip  q1, %[kp], 2\n"            /* 1   (d red no longer needed) */
        "  ee.vmulas.u16.qacc q4, q1\n"               /* QACC += 127            -> x */
        "  ee.srcmb.s16.qacc q4, %[sh8], 0\n"         /* QACC  = x>>8 (= t), q4 unused      (1.8.54) */
        "  ee.vmulas.u16.qacc q6, q7\n"               /* QACC += diff*a' */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 128 */
        "  ee.vmulas.u16.qacc q4, q1\n"               /* QACC += 128            -> x + t + 1 */
        "  ee.srcmb.s16.qacc q4, %[sh8], 0\n"         /* q4 = (x+t+1)>>8 = floor(x/255) */
        "  ee.vadds.s16    q1, q5, q4\n"              /* red = lo + q                       (1.8.70) */
        /* ---- green: same with q2 ---- */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* s_g */
        "  ee.vmin.s16     q5, q4, q2\n"
        "  ee.vmax.s16     q6, q4, q2\n"
        "  ee.vsubs.s16    q6, q6, q5\n"
        "  ee.vcmp.lt.s16  q7, q4, q2\n"
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 0x00FF */
        "  ee.andq         q7, q7, q4\n"
        "  ee.xorq         q7, q0, q7\n"
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc q6, q7\n"
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 127 */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"            /* 1 */
        "  ee.vmulas.u16.qacc q4, q2\n"
        "  ee.srcmb.s16.qacc q4, %[sh8], 0\n"
        "  ee.vmulas.u16.qacc q6, q7\n"
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 128 */
        "  ee.vmulas.u16.qacc q4, q2\n"
        "  ee.srcmb.s16.qacc q4, %[sh8], 0\n"
        "  ee.vadds.s16    q2, q5, q4\n"              /* green */
        /* ---- blue: same with q3 ---- */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* s_b */
        "  ee.vmin.s16     q5, q4, q3\n"
        "  ee.vmax.s16     q6, q4, q3\n"
        "  ee.vsubs.s16    q6, q6, q5\n"
        "  ee.vcmp.lt.s16  q7, q4, q3\n"
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 0x00FF */
        "  ee.andq         q7, q7, q4\n"
        "  ee.xorq         q7, q0, q7\n"
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc q6, q7\n"
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 127 */
        "  ee.vldbc.16.ip  q3, %[kp], 2\n"            /* 1 */
        "  ee.vmulas.u16.qacc q4, q3\n"
        "  ee.srcmb.s16.qacc q4, %[sh8], 0\n"
        "  ee.vmulas.u16.qacc q6, q7\n"
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 128 */
        "  ee.vmulas.u16.qacc q4, q3\n"
        "  ee.srcmb.s16.qacc q4, %[sh8], 0\n"
        "  ee.vadds.s16    q3, q5, q4\n"              /* blue */
        /* ---- pack RGB565 = ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3) ---- */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 0xF8 */
        "  ee.andq         q1, q1, q4\n"
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 32768: x*32768>>11 = x<<4, twice = x<<8 */
        "  ee.vmul.u16     q1, q1, q4\n"
        "  ee.vmul.u16     q1, q1, q4\n"              /* red field */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 0xFC */
        "  ee.andq         q2, q2, q4\n"
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 16384: x<<3 */
        "  ee.vmul.u16     q2, q2, q4\n"              /* green field */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"            /* 256: x>>3 */
        "  ee.vmul.u16     q3, q3, q4\n"              /* blue field */
        "  ee.orq          q1, q1, q2\n"
        "  ee.orq          q1, q1, q3\n"
        "  ee.vst.128.ip   q1, %[d], 16\n"            /* 8 pixels out, dst += 16            (1.8.192) */
        "  addi         %[n], %[n], -1\n"
        "  bnez         %[n], 2b\n"
        : [d] "+a"(dst), [m] "+a"(mask), [n] "+a"(n), [kp] "=&a"(kp)
        : [k] "a"(k), [sar] "a"(sar), [sh8] "a"(sh8)
        : "memory");
}

#else /* host model: the same lane arithmetic in C, for testing the wrappers */

static void __attribute__((noinline)) fill_blocks_pie(uint16_t *dst, const uint16_t *color, int n) {
    for (int i = 0; i < n * 8; i++) dst[i] = *color;
}
static void __attribute__((noinline)) blend_blocks_pie(uint16_t *dst, const uint8_t *mask, const int16_t *k, int n) {
    unsigned sr = (uint16_t)k[8], sg = (uint16_t)k[13], sb = (uint16_t)k[18];
    for (int i = 0; i < n * 8; i++) {
        /* lane model of the assembly: identical arithmetic, verified separately (blend_model.c) */
        unsigned p = dst[i], a = mask[i];
        unsigned r5 = p >> 11, g6 = (p >> 5) & 63, b5 = p & 31;
        unsigned d[3] = { (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2) };
        unsigned s[3] = { sr, sg, sb }, out[3];
        for (int c = 0; c < 3; c++) {
            unsigned lo = s[c] < d[c] ? s[c] : d[c], diff = (s[c] < d[c] ? d[c] : s[c]) - lo;
            unsigned ap = s[c] < d[c] ? a ^ 0xFF : a;
            unsigned x = diff * ap + 127, t = x >> 8;
            out[c] = lo + ((x + t + 1) >> 8);
        }
        dst[i] = pack565(out[0], out[1], out[2]);
    }
}
#endif

/* Constant walk for blend_blocks_pie, in the order the kernel reads it. */
static void blend_constants(int16_t k[24], unsigned r, unsigned g, unsigned b) {
    const int16_t head[8] = { 1, 64, 63, 31, 16384, 512, 8192, 128 };
    for (int i = 0; i < 8; i++) k[i] = head[i];
    const unsigned s[3] = { r, g, b };
    for (int c = 0; c < 3; c++) {           /* s, 0x00FF, 127, 1, 128 */
        int16_t *e = k + 8 + c * 5;
        e[0] = (int16_t)s[c]; e[1] = 0x00FF; e[2] = 127; e[3] = 1; e[4] = 128;
    }
    k[23] = 0x00F8;
    /* pack constants continue right after; see blend_a8 below */
}

/* ------------------------------------------------------------------------ */
/* Hooks                                                                     */
/* ------------------------------------------------------------------------ */
static bool accel_fill(void *u, uint16_t *dst, size_t n, uint32_t w, uint32_t h,
                       pocketjs_rgb565_rect_t r, uint16_t color) {
    (void)u;
    if (r.width == 0 || r.height == 0) return true;
    if (r.x + r.width > w || r.y + r.height > h || (size_t)w * h > n) return false;
    if (((uintptr_t)dst & 15) != 0 || (w & 7) != 0) return false;   /* rows must stay 16-byte aligned */
    for (uint32_t y = r.y; y < r.y + r.height; y++) {
        uint16_t *row = dst + (size_t)y * w + r.x;
        uint32_t left = r.width;
        while (left && (((uintptr_t)row) & 15)) { *row++ = color; left--; }   /* head to 16 B */
        uint32_t blocks = left >> 3;
        if (blocks) { fill_blocks_pie(row, &color, (int)blocks); row += blocks * 8; left -= blocks * 8; }
        while (left--) *row++ = color;                                        /* tail */
    }
    return true;
}

static bool accel_blend(void *u, uint16_t *dst, size_t n, uint32_t w, uint32_t h,
                        const uint8_t *mask, size_t mask_size, pocketjs_rgb565_rect_t r,
                        uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    (void)u;
    if (alpha != 255) return false;             /* a = (mask*alpha+127)/255 would need a pre-pass */
    if (r.width == 0 || r.height == 0) return true;
    if (r.x + r.width > w || r.y + r.height > h || (size_t)w * h > n || mask_size < (size_t)w * h) return false;
    if (((uintptr_t)dst & 15) != 0 || ((uintptr_t)mask & 15) != 0 || (w & 7) != 0) return false;
    int16_t k[32] __attribute__((aligned(4)));
    blend_constants(k, red, green, blue);
    k[24] = (int16_t)0x8000; k[25] = 0x00FC; k[26] = 16384; k[27] = 256;      /* pack constants */
    for (uint32_t y = r.y; y < r.y + r.height; y++) {
        uint16_t *row = dst + (size_t)y * w + r.x;
        const uint8_t *m = mask + (size_t)y * w + r.x;
        uint32_t left = r.width;
        while (left && (((uintptr_t)row) & 15)) { *row = blend_px(*row, red, green, blue, *m); row++; m++; left--; }
        uint32_t blocks = left >> 3;
        if (blocks) {                       /* row is 16-byte aligned here, so m (same offset) is 8-byte aligned */
            blend_blocks_pie(row, m, k, (int)blocks);
            row += blocks * 8; m += blocks * 8; left -= blocks * 8;
        }
        while (left--) { *row = blend_px(*row, red, green, blue, *m); row++; m++; }
    }
    return true;
}

const pocketjs_rgb565_accelerator_t render_accel = {
    .struct_size = sizeof(pocketjs_rgb565_accelerator_t),
    .user_data = NULL,
    .fill_rgb565 = accel_fill,
    .blend_a8_rgb565 = accel_blend,
    .srm_psm5650_rgb565 = NULL,
};
