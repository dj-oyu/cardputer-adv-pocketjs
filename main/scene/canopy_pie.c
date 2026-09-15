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
//   the identity that lets `if(!f)continue` be deleted, and the channel bounds.
//   tools/pie/test_kernels.py runs the assembly below through piesim and compares
//   all eight pixels of a block against the same model.
//
// What the assembly does per block, and why each step is the shape it is:
//
//   1. dx = x - cx per lane, from an eight-lane x vector the caller built.
//      dx*dx at SAR = 0, because a shift would be wrong here.
//   2. t = (dx2*256*hi + dx2*lo) >> 18 through the accumulator, which is
//      dx2*mrr >> 18 rewritten: mrr is 17 bits and cannot sit in a lane, so it
//      travels as hi*256+lo and the x256 lives in the two operands -- dx2*16
//      (at most 30976) times hi*16 (at most 5344). Their true product is 1.7e8,
//      which is why this multiply has to leave through EE.SRCMB.S16.QACC. The
//      x16 on dx2 is one extra lane multiply, because the term that carries the
//      256 and the term that does not cannot share one scaling of dx2.
//   3. q = qbase - t, clamped by EE.VRELU.S16 in place (its multiplier operand is
//      zero, so anything negative lands on zero). That is not the scalar's clamp
//      dressed differently: with q at zero, f is zero, and a zero-alpha blend
//      returns the pixel unchanged -- so the scalar's `if(!f)continue` has nothing
//      left to do and the kernel has no branch anywhere.
//   4. f = (q*39322)>>16 through the accumulator again, g = 256 - f.
//   5. The row word is unpacked into r5/g6/b5 lanes: shifts through SAR with a
//      multiply by one, masks through ANDQ. The word itself is overwritten by its
//      own blue field, which is what keeps the live set inside eight registers.
//   6. Each channel blends as ((c-l)*g)>>8 + l, which is the scalar's
//      ((c*g + l*f)>>8) rearranged with f = 256-g: adding l*256 before a floor
//      shift by eight is the same as adding l after it, for every sign. One
//      subtract, one multiply, one add, per channel.
//   7. The three fields are shifted back into place and ORQ'd into one word.
//
// Register plan: q0 is dx2 and dies; q1 walks the constants and doubles as the
// blend scratch; q2 is q and dies into the f multiply; q3 is a mask, then f, then
// one l at a time; q4 is g and lives to the last channel; q5/q6/q7 are r5/g6/b5.
// Eight names, and no more: this project's simulator carries eight vector
// registers (piesim's qi() also reads a name as the character after the q, so a
// q10 would alias q1 rather than fail), and the kernels in garden.c live inside
// the same bound.
//
// SAR is set four times a block (0 for dx*dx, 11 and 5 to unpack, 8 to blend,
// then 0 again to pack). The garden pixel pass avoids that by holding SAR = 0,
// but it pays a QACC round trip for every shift; here the shifts ride multiplies
// that have to happen anyway.
#include <stdint.h>
#include <stddef.h>

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
    int16_t k[16] __attribute__((aligned(4))) = {
        (int16_t)(-cx),                  /* dx = x - cx */
        (int16_t)(mrr&255),              /* mlo, the term that does not need the x256 */
        16,                              /* dx2*16, which is what fits the lane */
        (int16_t)((mrr>>8)*16),          /* mhi, already multiplied by sixteen */
        (int16_t)(256-qy),               /* qbase */
        39322,                           /* q*3/5 exactly, with a shift of 16 */
        256,
        1,                               /* multiply by one: a shift through SAR */
        31, 63,                          /* the r and g field masks */
        31,                              /* and the b mask, from the same walk */
        (int16_t)((leafy>>11)&31),       /* lr */
        (int16_t)((leafy>>5)&63),        /* lg */
        (int16_t)(leafy&31),             /* lb */
        2048, 32                         /* the two packing shifts */
    };
    int16_t kv[16][8] __attribute__((aligned(16)));
    int16_t xv[8] __attribute__((aligned(16)));
    for (int i=0;i<8;i++) xv[i]=(int16_t)(x0+i);
    const int16_t *kp,*xp=xv;
    int nk=16,sh0=0,sh5=5,sh8=8,sh11=11,sh16=16,sh18=18;
    canopy_broadcast(k,&kv[0][0],nk);
    __asm__ volatile(
        "mov              %[kp], %[kv]\n"
        "loopgtz          %[n], 9f\n"
        "  mov            %[kp], %[kv]\n"
        "  wsr.sar        %[sh0]\n"
        "  ee.vld.128.ip  q0, %[xp], 16\n"          /* x, one per lane */
        "  ee.vld.128.ip  q1, %[kp], 16\n"          /* -cx */
        "  ee.vadds.s16   q0, q0, q1\n"             /* dx */
        "  ee.vmul.s16    q0, q0, q0\n"             /* dx2, at most 1936: exact at SAR=0 */
        "  ee.zero.qacc\n"
        "  ee.vld.128.ip  q1, %[kp], 16\n"          /* mlo */
        "  ee.vmulas.u16.qacc q0, q1\n"             /* += dx2*mlo, 19 bits */
        "  ee.vld.128.ip  q1, %[kp], 16\n"          /* 16 */
        "  ee.vmul.s16    q0, q0, q1\n"             /* dx2*16, at most 30976: still a lane */
        "  ee.vld.128.ip  q1, %[kp], 16\n"          /* mhi, already x16 */
        "  ee.vmulas.u16.qacc q0, q1\n"             /* += dx2*256*hi: 1.7e8, through QACC */
        "  ee.vld.128.ip  q1, %[kp], 16\n"          /* qbase */
        "  ee.srcmb.s16.qacc q2, %[sh18], 0\n"      /* t = the sum >> 18 */
        "  ee.vsubs.s16   q2, q1, q2\n"             /* qbase - t */
        "  ee.vrelu.s16   q2, %[sh0], %[sh0]\n"     /* q, clamped at zero in place */
        "  ee.zero.qacc\n"
        "  ee.vld.128.ip  q1, %[kp], 16\n"          /* 39322 */
        "  ee.vmulas.u16.qacc q2, q1\n"             /* q*3/5, exact */
        "  ee.srcmb.s16.qacc q3, %[sh16], 0\n"      /* f */
        "  ee.vld.128.ip  q1, %[kp], 16\n"          /* 256 */
        "  ee.vsubs.s16   q4, q1, q3\n"             /* g = 256 - f */
        "  ee.vld.128.ip  q5, %[row], 0\n"          /* eight packed pixels */
        "  wsr.sar        %[sh11]\n"
        "  ee.vld.128.ip  q1, %[kp], 16\n"          /* 1 */
        "  ee.vmul.u16    q6, q5, q1\n"             /* p >> 11 */
        "  ee.vld.128.ip  q3, %[kp], 16\n"          /* 31 */
        "  ee.andq        q6, q6, q3\n"             /* r5 */
        "  wsr.sar        %[sh5]\n"
        "  ee.vmul.u16    q7, q5, q1\n"             /* p >> 5 */
        "  ee.vld.128.ip  q3, %[kp], 16\n"          /* 63 */
        "  ee.andq        q7, q7, q3\n"             /* g6 */
        "  ee.andq        q5, q5, q3\n"             /* the blue field, one AND from 31 */
        "  ee.vld.128.ip  q3, %[kp], 16\n"          /* 31 */
        "  ee.andq        q5, q5, q3\n"             /* b5, in the word's own register */
        "  wsr.sar        %[sh8]\n"
        "  ee.vld.128.ip  q3, %[kp], 16\n"          /* lr */
        "  ee.vsubs.s16   q1, q6, q3\n"             /* c - l */
        "  ee.vmul.s16    q1, q1, q4\n"             /* (c-l)*g >> 8 */
        "  ee.vadds.s16   q6, q1, q3\n"             /* + l */
        "  ee.vld.128.ip  q3, %[kp], 16\n"          /* lg */
        "  ee.vsubs.s16   q1, q7, q3\n"
        "  ee.vmul.s16    q1, q1, q4\n"
        "  ee.vadds.s16   q7, q1, q3\n"
        "  ee.vld.128.ip  q3, %[kp], 16\n"          /* lb */
        "  ee.vsubs.s16   q1, q5, q3\n"
        "  ee.vmul.s16    q1, q1, q4\n"
        "  ee.vadds.s16   q5, q1, q3\n"
        "  wsr.sar        %[sh0]\n"
        "  ee.vld.128.ip  q1, %[kp], 16\n"          /* 2048 */
        "  ee.vmul.u16    q6, q6, q1\n"             /* r << 11 */
        "  ee.vld.128.ip  q1, %[kp], 16\n"          /* 32 */
        "  ee.vmul.u16    q7, q7, q1\n"             /* g << 5 */
        "  ee.orq         q6, q6, q7\n"
        "  ee.orq         q6, q6, q5\n"
        "  ee.vst.128.ip  q6, %[row], 16\n"
        "9:\n"
        : [kp]"=&a"(kp), [xp]"=&a"(xp), [row]"+a"(row)
        : [n]"a"(n), [kv]"a"(&kv[0][0]), [sh0]"a"(sh0), [sh5]"a"(sh5), [sh8]"a"(sh8),
          [sh11]"a"(sh11), [sh16]"a"(sh16), [sh18]"a"(sh18)
        : "memory");
}
#else
// Host builds take the scalar path, so anything that includes this file still
// works off-device and the model keeps being the only arithmetic in play.
void __attribute__((unused))
canopy_pie(uint16_t *row,int n,int cx,int mrr,int qy,uint16_t leafy,int x0) {
    canopy_scalar(row,x0,x0+n*8-1,cx,mrr,qy,leafy);
}
#endif
