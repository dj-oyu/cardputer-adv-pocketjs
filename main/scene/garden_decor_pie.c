/* The garden decor mix as an eight-lane PIE kernel: eight pixels a pass, one
 * multiply and one shift a channel.
 *
 * garden.c's garden_decor_row walks its span in groups and calls
 * garden_decor_mix once per pixel:
 *
 *     int d=garden_dither(x,y)*64+32;
 *     for(int j=0;j<n;j++)row[x+j]=garden_decor_mix(row[x+j],light,shadow,d);
 *
 * with light, shadow and d constant across the group -- they are the group's
 * protection ramp, its two profile values and its dither, evaluated once at the
 * group's first column. That is what makes a lane version possible at all: the
 * three channel formulas differ only in their shift and their additive
 * constant, so eight pixels share both.
 *
 * The statement stays in garden.c, and this file keeps the folded version of it
 * next to the kernel as the definition the assembly is checked against. Red is
 * exact, in three steps that tools/pie/models/garden_model.c sweeps on their
 * own:
 *
 *   (light*(r+6))>>1 == (light*r)>>1 + 3*light        (6*light is even)
 *   r*E + (light*r)>>1 == (r*(2E+light))>>1           (2E*r is even)
 *   the two nested floors fold: floor(floor(t/2)/256) == floor(t/512)
 *
 * so r_out = (r*Mr + Kr) >> 9 with E = 256-(shadow>>3), Mr = 2E+light and
 * Kr = 6*light+2*d. Green and blue carry a constant inside their >>2 that is
 * not a multiple of 4 (10*light, 4*light), so floor((light*g + 10*light)/4)
 * cannot be split at the floor without a correction that depends on the low two
 * bits of light*g. The kernel floors that constant on its own -- it keeps it,
 * rather than dropping it -- and that is the whole approximation: over every
 * light, shadow and d, 0..255 each -- a superset of what the row can hand down
 * -- the model measures green in 0.098% of cases, never by more than one step of
 * its sixty-three, and blue never at all. Dropping the term instead would move green
 * by up to two and a half steps, uniformly darker, on every pixel of every ray.
 *
 * The lane shape, eight pixels in q7:
 *
 *   1. Extract the fields. PIE has no per-lane shift for 16-bit data, but
 *      EE.VMUL.U16 keeps the full 32-bit product and shifts it by SAR[5:0]
 *      before taking the low sixteen bits (TRM 1.8.128), so a multiply by a
 *      power of two at SAR 16 *is* a shift: p*32>>16 is p>>11, already the
 *      five-bit red field and needing no mask; p*2048>>16 is p>>5, masked to
 *      six; blue is p & 31. Two multiplies and two masks for three channels,
 *      the same trick scene/canopy_pie.c unpacks its row word with.
 *   2. Per channel: preload the accumulator with the additive constant
 *      (EE.MOV.U16.QACC, 1.8.116 -- unsigned, so a constant above 32767 would
 *      come out negative through its sign-extending twin), accumulate x*M into
 *      it (EE.VMULAS.U16.QACC, 1.8.163), read it back shifted and saturated
 *      (EE.SRCMB.S16.QACC, 1.8.54) and clamp the field (EE.VMIN.S16, 1.8.113).
 *      The order matters: the scalar adds its constant *before* the >>8, so
 *      the accumulator has to be primed rather than added to afterwards -- that
 *      add is one shift away from the first of the three exact steps above, and
 *      moving it would cost the channel more than the fold does.
 *   3. Pack: r*2048 and g*32 at SAR 0 place the fields, and two EE.ORQ combine
 *      them with blue. Four instructions for eight pixels, shared by the three
 *      channels.
 *
 * Cost, measured rather than estimated (tools/pie/models/garden_model.c for the
 * ranges: the longest accumulator run is 82,617 of QACC's forty bits and the
 * SRCMB readout peaks at 80, nowhere near its 16-bit saturation).
 *
 *   - python tools/pie/stalls.py reports 36 instructions per block, 35 in the
 *     program below plus the pointer copy, and no stage-2 producer followed by
 *     its consumer. Read that as "no QR hazard" and not as a floor: the tool
 *     models neither QACC nor SAR (tools/pie/README.md, limits) and this kernel
 *     takes the accumulator three times a block, so the accumulator side of the
 *     cost needs the device.
 *   - objdump is the honest total: 73 instructions a call, 37 of them before the
 *     first EE. instruction. Those 37 are the entry, the four shift registers
 *     and the eleven halfwords of the constant table -- five expressions out of
 *     light, shadow and d, which the compiler builds with addx2/addx4 and stores
 *     two at a time. It is paid once per group, not once per pixel.
 *   - so against the scalar mix this is 73/8 + 34/8 = 13.4 instructions a pixel
 *     versus 21 + 34/4 = 29.5 (the group's own prologue is garden.c's measured
 *     ~8.5 instructions a pixel at four columns, which both paths pay). About
 *     2.2x, not the 5x the lane count suggests, and the gap is that C table: a
 *     caller that fills the table itself would move those 37 instructions out of
 *     the kernel, not out of the frame.
 *
 * The constants arrive as an int16_t k[] filled here and walked in load order,
 * which is why the initializer reads the way the program does; moving a load
 * past another swaps two constants, and tools/pie/test_kernels.py reads this
 * text rather than the binary, so it catches that.
 *
 * The caller owns the clipping and the tails: eight pixels at a time from a
 * 16-byte-aligned pointer (EE.VLD.128 forces the low four address bits to zero,
 * so a group that does not start on an eight-pixel boundary reads its
 * neighbour), and the group width has to be eight -- at the current
 * g_garden_decor_group of 4 the widest group is four columns.
 */
#include <stdint.h>
#include "garden_decor_pie.h"

/* The folded statement, kept as the definition the kernel is checked against
 * (tools/pie/test_kernels.py mirrors it line for line, and
 * tools/pie/models/garden_model.c is where the difference between it and
 * garden_decor_mix -- zero for red, one step of green in 0.098% of cases, zero
 * for blue -- is measured rather than asserted). */
__attribute__((unused))
static uint16_t garden_decor_mix_folded(uint16_t p,int light,int shadow,int d) {
    int r=(p>>11)&31,g=(p>>5)&63,b=p&31;
    int e=256-(shadow>>3);
    int ro=(r*(2*e+light)+6*light+2*d)>>9;
    int go=(g*(4*e+light)+4*((10*light)>>2)+4*d)>>10;
    int bo=(b*(4*e+light)+4*((4*light)>>2)+4*d)>>10;
    if(ro>31)ro=31;
    if(go>63)go=63;
    if(bo>31)bo=31;
    return (uint16_t)(ro<<11|go<<5|bo);
}

#ifdef ESP_PLATFORM
#define GARDEN_DECOR_PIE 1
#else
#define GARDEN_DECOR_PIE 0
#endif

#if GARDEN_DECOR_PIE
// The kernel. One block of eight pixels, no loop: the constants change with the
// group, so there is nothing to carry across blocks.
void __attribute__((noinline))
garden_decor_mix8(uint16_t *row,int light,int shadow,int d) {
    // In load order. 32 and 2048 appear twice because the extraction and the
    // pack both need them, and the body has taken their registers in between.
    int16_t k[11] __attribute__((aligned(4))) = {
        32,                       /* the red extraction, and green's placement */
        2048,                     /* green's extraction, and red's placement */
        63,                       /* green's field, for the extraction and the clamp */
        31,                       /* blue's field, and red's */
        (int16_t)(2*(256-(shadow>>3))+light),          /* Mr */
        (int16_t)(6*light+2*d),                        /* Kr */
        (int16_t)(4*(256-(shadow>>3))+light),          /* Mg, green and blue share it */
        (int16_t)(4*((10*light)>>2)+4*d),              /* Kg */
        (int16_t)(4*((4*light)>>2)+4*d),               /* Kb */
        2048,                     /* red's placement */
        32                        /* green's placement */
    };
    const int16_t *kp;
    int sh0=0,sh9=9,sh10=10,sh16=16;
    __asm__ volatile(
        "mov            %[kp], %[k]\n"
        "ee.vld.128.ip  q7, %[row], 0\n"              /* the eight pixels */
        "ee.vldbc.16.ip q0, %[kp], 2\n"               /* 32 */
        "wsr.sar        %[sh16]\n"                    /* the extraction's shift */
        "ee.vldbc.16.ip q1, %[kp], 2\n"               /* 2048 */
        "ee.vmul.u16    q6, q7, q0\n"                 /* r = p*32>>16 */
        "ee.vldbc.16.ip q2, %[kp], 2\n"               /* 63 */
        "ee.vmul.u16    q5, q7, q1\n"                 /* p>>5, six bits too wide */
        "ee.vldbc.16.ip q3, %[kp], 2\n"               /* 31 */
        "ee.andq        q5, q5, q2\n"                 /* g */
        "ee.andq        q4, q7, q3\n"                 /* b */
        "ee.vldbc.16.ip q0, %[kp], 2\n"               /* Mr */
        "ee.vldbc.16.ip q7, %[kp], 2\n"               /* Kr */
        "ee.mov.u16.qacc q7\n"
        "ee.vmulas.u16.qacc q6, q0\n"                 /* Kr + r*Mr */
        "ee.srcmb.s16.qacc q6, %[sh9], 0\n"           /* >> 9, and saturated */
        "ee.vmin.s16    q6, q6, q3\n"                 /* red, clamped to five bits */
        "ee.vldbc.16.ip q1, %[kp], 2\n"               /* Mg */
        "ee.vldbc.16.ip q7, %[kp], 2\n"               /* Kg */
        "ee.mov.u16.qacc q7\n"
        "ee.vmulas.u16.qacc q5, q1\n"                 /* Kg + g*Mg */
        "ee.srcmb.s16.qacc q5, %[sh10], 0\n"          /* >> 10 */
        "ee.vmin.s16    q5, q5, q2\n"                 /* green, clamped to six */
        "ee.vldbc.16.ip q7, %[kp], 2\n"               /* Kb */
        "ee.mov.u16.qacc q7\n"
        "ee.vmulas.u16.qacc q4, q1\n"                 /* Kb + b*Mg */
        "ee.srcmb.s16.qacc q4, %[sh10], 0\n"          /* >> 10 */
        "ee.vmin.s16    q4, q4, q3\n"                 /* blue, clamped to five */
        "ee.vldbc.16.ip q0, %[kp], 2\n"               /* 2048 */
        "wsr.sar        %[sh0]\n"                     /* the placement's shift */
        "ee.vldbc.16.ip q1, %[kp], 2\n"               /* 32 */
        "ee.vmul.u16    q6, q6, q0\n"                 /* red << 11 */
        "ee.vmul.u16    q5, q5, q1\n"                 /* green << 5 */
        "ee.orq         q4, q4, q6\n"
        "ee.orq         q4, q4, q5\n"
        "ee.vst.128.ip  q4, %[row], 16\n"
        : [kp]"=&a"(kp), [row]"+a"(row)
        : [k]"a"(k), [sh0]"a"(sh0), [sh9]"a"(sh9), [sh10]"a"(sh10), [sh16]"a"(sh16)
        : "memory");
}
#else
// Host builds take the folded statement, which is the arithmetic the assembly
// is checked against: a host harness that links this file runs the kernel's
// algebra, not a different program, and tools/pie/test_kernels.py is what keeps
// the two together.
void
garden_decor_mix8(uint16_t *row,int light,int shadow,int d) {
    for(int i=0;i<8;i++)row[i]=garden_decor_mix_folded(row[i],light,shadow,d);
}
#endif
