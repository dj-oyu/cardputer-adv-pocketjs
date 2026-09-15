// The canopy blend as a PIE kernel: eight pixels a pass, no divide, no branch.
//
// garden_canopy_row in garden.c is the scalar statement of this and stays the
// only definition of what the picture is; this file is the lane version of it,
// written the way the other kernels in garden.c are -- the constants arrive as
// a broadcast table the loop walks, the loop is `n` blocks of eight, and the
// caller keeps the tails and the clipping.
//
// The arithmetic is proven, not assumed, before any of this runs:
//
//   tools/pie/models/canopy_model.c sweeps every reachable (dx2, mrr) pair and
//   every step below: the radicand split, the reciprocal's exactness, q*3/5,
//   the identity that lets `if(!f)continue` be deleted, the blend rewritten
//   around f alone, and the channel bounds.
//   tools/pie/test_kernels.py runs the assembly below through piesim and compares
//   every pixel of one to five blocks against the scalar statement.
//
// What the assembly does per block, and why each step is the shape it is:
//
//   1. q7 holds dx = x - cx per lane and moves on by eight; dx*dx at SAR 0.
//   2. t = (dx2*256*hi + dx2*lo) >> 18 through the accumulator, which is
//      dx2*mrr >> 18 rewritten: mrr is 17 bits and cannot sit in a lane, so it
//      travels as hi*256+lo and the x256 lives in the two operands -- dx2*16
//      (at most 30976) times hi*16 (at most 5344). Their true product, 1.7e8,
//      does not fit a lane, which is why this sum leaves through
//      EE.SRCMB.S16.QACC.
//   3. q = qbase - t, clamped by EE.VRELU.S16 in place (its multiplier operand is
//      zero, so anything negative lands on zero). With q at zero, f is zero and
//      the blend below returns the pixel unchanged, so there is no branch.
//   4. f = (q*39322)>>16 as ONE EE.VMUL.U16 at SAR 16. The multiply keeps the
//      full 32-bit product and shifts by SAR[5:0] before taking the low sixteen
//      bits (TRM 1.8.128), and q*39322 is at most 1.0e7, so no accumulator round
//      trip is needed for it.
//   5. The row word is unpacked at the same SAR 16: p*32>>16 is p>>11, which is
//      already the five-bit red field; p*2048>>16 is p>>5, masked to six bits;
//      blue is p & 31.
//   6. Each channel blends as c + (((l-c)*f)>>8). That is the scalar's
//      (c*g + l*f)>>8 with g = 256-f: the numerator is 256c + (l-c)f and 256c
//      shifts out whole, for every sign. EE.VMUL.S16 at SAR 8 is that floor
//      shift. g is never formed.
//   7. The fields are packed at SAR 0 (r*2048, g*32) and summed with EE.VADDS.S16:
//      the three fields do not overlap, so no lane ever reaches the saturation
//      point and the sum is the OR.
//
// Scheduling. Every instruction that defines its result at pipeline stage 2 --
// a load, a fused load's loaded register, a multiply, the ReLU -- has at least
// one instruction between it and the first reader of that register (TRM table
// 1.7-2, docs/perf/pie-simd.md 4), and every constant load that can ride an
// add, subtract or multiply does, as its .LD.INCP form (docs/perf/pie-simd.md 3.5:
// the fused load costs nothing). python tools/pie/stalls.py main/scene/canopy_pie.c
// canopy_pie reports zero stalls. The constant table below is in the order the
// loads appear in the program; moving a load past another swaps two constants.
//
// Register plan: q0 dx2 -> constants -> blue delta; q1 constants -> q -> red
// delta; q2 qbase -> f (lives to the last channel); q3 constants -> green delta;
// q4 p>>5 -> g6 -> green; q5 the row word -> b5 -> the packed word; q6 r5 ->
// red; q7 dx. Eight names and no more: piesim reads a register name as the one
// character after the q, so a q10 would alias q1 rather than fail.
#include <stdint.h>
#include <stddef.h>
#include "canopy_pie.h"

// The scalar statement, kept here so the kernel and its reference cannot drift:
// this is garden_canopy_row with the A/B switch removed (a caller-side choice).
static void __attribute__((unused))
canopy_scalar(uint16_t *row, int lo, int hi, int cx, int mrr, int qy, uint16_t leafy) {
    int mhi = (mrr >> 8) * 16, mlo = mrr & 255, qbase = 256 - qy;
    int lr = (leafy >> 11) & 31, lg = (leafy >> 5) & 63, lb = leafy & 31;
    for (int x = lo; x <= hi; x++) {
        int dx = x - cx, dx2 = dx * dx;
        int t = ((dx2 * 16) * mhi + dx2 * mlo) >> 18;
        int q = qbase - t;
        if (q < 0) q = 0;
        int f = (q * 39322) >> 16, g = 256 - f;
        unsigned a = row[x];
        row[x] = (uint16_t)(((((a >> 11) & 31) * g + lr * f) >> 8) * 2048
                          + ((((a >> 5) & 63) * g + lg * f) >> 8) * 32
                          +  (((a & 31) * g + lb * f) >> 8));
    }
}

#ifdef ESP_PLATFORM
#define CANOPY_PIE 1
#else
#define CANOPY_PIE 0
#endif

#if CANOPY_PIE
// The same broadcast garden.c's kernels use, copied because that one is static
// there and this file has to stand alone until the kernel moves in beside them.
// tools/pie/test_kernels.py runs this copy, so it is not an untested twin.
static void __attribute__((noinline))
canopy_broadcast(const int16_t *k,int16_t *kv,int nk) {
    const int16_t *ks;
    int16_t *kp;
    __asm__ volatile(
        "mov            %[ks], %[k]\n"
        "mov            %[kp], %[kv]\n"
        "loopgtz        %[nk], 0f\n"
        "  ee.vldbc.16.ip  q0, %[ks], 2\n"
        "  ee.vst.128.ip   q0, %[kp], 16\n"
        "0:\n"
        : [ks]"=&a"(ks), [kp]"=&a"(kp)
        : [k]"a"(k), [kv]"a"(kv), [nk]"a"(nk)
        : "memory");
}
// n blocks of eight pixels. `row` must point at the first pixel of the run and
// the run must be a multiple of eight long: the caller clips the ellipse, keeps
// the head and tail scalar, and only calls this with |x - cx| <= rx inside.
// `x0` is the x of row[0], because dx = x - cx and the lane vector has to know
// where in the row it is.
void __attribute__((noinline))
canopy_pie(uint16_t *row,int n,int cx,int mrr,int qy,uint16_t leafy,int x0) {
    // In load order (see Scheduling above).
    int16_t k[15] __attribute__((aligned(4))) = {
        8,                               /* how far dx moves on, per block */
        (int16_t)(mrr&255),              /* mlo, the term that does not need the x256 */
        16,                              /* dx2*16, which is what fits the lane */
        (int16_t)((mrr>>8)*16),          /* mhi, already multiplied by sixteen */
        (int16_t)(256-qy),               /* qbase */
        (int16_t)39322,                  /* q*3/5 exactly, at SAR 16 */
        32,                              /* p*32>>16 is the red field */
        2048,                            /* p*2048>>16 is p>>5 */
        63, 31,                          /* the green and blue masks */
        (int16_t)((leafy>>11)&31),       /* lr */
        (int16_t)((leafy>>5)&63),        /* lg */
        (int16_t)(leafy&31),             /* lb */
        2048, 32                         /* the packing shifts, at SAR 0 */
    };
    int16_t kv[15][8] __attribute__((aligned(16)));
    int16_t xv[8] __attribute__((aligned(16)));
    for (int i=0;i<8;i++) xv[i]=(int16_t)(x0+i-cx);
    const int16_t *kp;
    int nk=15,sh0=0,sh8=8,sh16=16,sh18=18;
    canopy_broadcast(k,&kv[0][0],nk);
    __asm__ volatile(
        "wsr.sar          %[sh0]\n"                 /* every block ends at SAR 0 too */
        "ee.vld.128.ip    q7, %[xp], 0\n"           /* dx, one per lane */
        "loopgtz          %[n], 9f\n"
        "  mov            %[kp], %[kv]\n"
        "  ee.vmul.s16    q0, q7, q7\n"                         /* dx2, at most 1936 */
        "  ee.vld.128.ip  q1, %[kp], 16\n"                      /* 8 */
        "  ee.vld.128.ip  q3, %[kp], 16\n"                      /* mlo */
        "  ee.vadds.s16.ld.incp q1, %[kp], q7, q7, q1\n"        /* next block's dx; load 16 */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc q0, q3\n"                         /* += dx2*mlo */
        "  ee.vmul.s16.ld.incp q3, %[kp], q0, q0, q1\n"         /* dx2*16; load mhi */
        "  ee.vld.128.ip  q2, %[kp], 16\n"                      /* qbase */
        "  ee.vmulas.u16.qacc q0, q3\n"                         /* += dx2*256*hi */
        "  ee.srcmb.s16.qacc q1, %[sh18], 0\n"                  /* t = the sum >> 18 */
        "  ee.vsubs.s16.ld.incp q3, %[kp], q1, q2, q1\n"        /* qbase - t; load 39322 */
        "  ee.vrelu.s16   q1, %[sh0], %[sh0]\n"                 /* q, clamped at zero */
        "  wsr.sar        %[sh16]\n"
        "  ee.vld.128.ip  q5, %[row], 0\n"                      /* eight packed pixels */
        "  ee.vmul.u16.ld.incp q0, %[kp], q2, q1, q3\n"         /* f = q*39322>>16; load 32 */
        "  ee.vld.128.ip  q4, %[kp], 16\n"                      /* 2048 */
        "  ee.vmul.u16.ld.incp q3, %[kp], q6, q5, q0\n"         /* r5 = p>>11; load 63 */
        "  ee.vmul.u16.ld.incp q0, %[kp], q4, q5, q4\n"         /* p>>5; load 31 */
        "  wsr.sar        %[sh8]\n"
        "  ee.andq        q5, q5, q0\n"                         /* b5 */
        "  ee.andq        q4, q4, q3\n"                         /* g6 */
        "  ee.vld.128.ip  q1, %[kp], 16\n"                      /* lr */
        "  ee.vld.128.ip  q3, %[kp], 16\n"                      /* lg */
        "  ee.vsubs.s16.ld.incp q0, %[kp], q1, q1, q6\n"        /* lr - r5; load lb */
        "  ee.vmul.s16    q1, q1, q2\n"                         /* ((l-c)*f)>>8 */
        "  ee.vsubs.s16   q3, q3, q4\n"                         /* lg - g6 */
        "  ee.vadds.s16.ld.incp q1, %[kp], q6, q6, q1\n"        /* red; load 2048 */
        "  ee.vmul.s16    q3, q3, q2\n"
        "  ee.vsubs.s16   q0, q0, q5\n"                         /* lb - b5 */
        "  ee.vadds.s16.ld.incp q3, %[kp], q4, q4, q3\n"        /* green; load 32 */
        "  ee.vmul.s16    q0, q0, q2\n"
        "  wsr.sar        %[sh0]\n"
        "  ee.vmul.u16    q6, q6, q1\n"                         /* red << 11 */
        "  ee.vmul.u16    q4, q4, q3\n"                         /* green << 5 */
        "  ee.vadds.s16   q5, q5, q0\n"                         /* blue */
        "  ee.vadds.s16   q5, q5, q6\n"
        "  ee.vadds.s16   q5, q5, q4\n"
        "  ee.vst.128.ip  q5, %[row], 16\n"
        "9:\n"
        : [kp]"=&a"(kp), [row]"+a"(row)
        : [n]"a"(n), [kv]"a"(&kv[0][0]), [xp]"a"(xv), [sh0]"a"(sh0),
          [sh8]"a"(sh8), [sh16]"a"(sh16), [sh18]"a"(sh18)
        : "memory");
}
#else
// Host builds take the lane model in C: the same eight-lane arithmetic, the same
// order, with the SAR shifts written as shifts. It is not a copy of the scalar
// reference -- it is the arithmetic tools/pie/test_kernels.py checks the assembly
// against, so a host harness that links this file is testing the kernel's
// algebra and not a different program.
void __attribute__((unused))
canopy_pie(uint16_t *row,int n,int cx,int mrr,int qy,uint16_t leafy,int x0) {
    int mhi=(mrr>>8)*16, mlo=mrr&255, qbase=256-qy;
    int lr=(leafy>>11)&31, lg=(leafy>>5)&63, lb=leafy&31;
    for(int b=0;b<n;b++) {
        for(int i=0;i<8;i++) {
            int dx=(x0+b*8+i)-cx, dx2=dx*dx;
            int64_t acc=(int64_t)dx2*mlo + (int64_t)(dx2*16)*mhi;
            int q=qbase-(int)(acc>>18); if(q<0)q=0;  /* VRELU in place */
            int f=(q*39322)>>16;                     /* VMUL.U16 at SAR 16 */
            unsigned w=row[b*8+i];           /* the pointer is the block's first pixel */
            int r5=(int)((w*32)>>16), g6=(int)((w*2048)>>16)&63, b5=(int)(w&31);
            int r=r5+(((lr-r5)*f)>>8);                /* VMUL.S16 at SAR 8, then VADDS */
            int gg=g6+(((lg-g6)*f)>>8);
            int bb=b5+(((lb-b5)*f)>>8);
            row[b*8+i]=(uint16_t)(r*2048+gg*32+bb);  /* the packing shifts, VADDS */
        }
    }
}
#endif
