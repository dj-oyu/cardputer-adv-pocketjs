/* The renderer's RGB565 per-pixel blend and pack as an eight-lane PIE kernel.
 *
 * ksn_render.c composites a direct primitive one pixel at a time:
 *
 *     pixels[index] = blend(pixels[index], sample(&command,x,py), d->opacity,
 *                           dither, x, py);
 *
 * and `blend` (ksn_render.c:190-199) expands the destination, mixes the source
 * over it and hands the three eight-bit channels to `pack565` (:106-111), which
 * either shifts them into five/six/five bits or dithers them through
 * `quantize` (:101-105). The survey counts that family at blend 5,534 +
 * pack565 11,590 + quantize 12,960 calls a frame, about 1.0 M of the per-pixel
 * boundary's estimated 3.1 M instructions (docs/perf/kasane-opt-survey.md §5).
 * This file is that path as a vector kernel -- eight pixels a pass, one source
 * colour for the block -- the way docs/perf/pie-simd.md:542 records the same
 * class of 565 blend going 38.5 ms to about 10 ms when it was vectorised.
 *
 * Nothing here is wired into the renderer. ksn_render.c is unchanged and this
 * file is not in main/CMakeLists.txt: one kernel, one commit, and deciding
 * which eight-pixel runs are blendable is the caller's job in the next commit.
 *
 * ---------------------------------------------------------------------------
 * 1. What the kernel has to reproduce, and the space it has to do it over
 *
 * The scalar statement, copied below as ksn_blend_scalar_ref() so that the
 * assembly has a definition next to it rather than a description:
 *
 *     a  = mul8(src&255, opacity)                       (ksn_render.c:191)
 *     if(!a) return dst                                 (nothing is written)
 *     d8 = replicate(dst field)                  (:194, 31->255 and 63->255)
 *     c  = (s*a + d8*(255-a) + 127)/255                 (:195-197)
 *     out= dither ? quantize(r,31,t)<<11 | quantize(g,63,t)<<5 | quantize(b,31,t)
 *                 : (r>>3)<<11|(g>>2)<<5|(b>>3)         (:106-111)
 *
 * The input space is therefore: every 16-bit destination word (the strip is a
 * plain uint16_t buffer -- fill565, the group path's pack and the frost pass
 * can all leave any of the 65,536 values in it), every eight-bit source channel
 * (API colours are 8-bit, and the text path scales only the alpha through mul8,
 * :262), every opacity 0..255, and, on the dither arm, the sixteen bayer4
 * thresholds (:43). tools/pie/models/blend_pack_model.c sweeps that space: the
 * per-channel mix exhaustively over its own domain, and the whole kernel over
 * all 65,536 destination words for parameter sets the renderer can produce.
 *
 * No approximation is used and none is needed: every step below is an identity,
 * so the model measures zero moved pixels and a worst step of zero -- it prints
 * those counts rather than asserting them, and would print the moved set if the
 * arithmetic ever stopped being exact.
 *
 * ---------------------------------------------------------------------------
 * 2. The lane algebra
 *
 * 2.1 Unpacking. The destination's five/six-bit fields are expanded to eight
 * bits the way the scalar does, (r5<<3)|(r5>>2), but two of the three
 * expansions collapse into a single multiply, because for a five-bit field
 *
 *     (v<<3)|(v>>2) == (v*33)>>2      (v < 32; the halves do not overlap, so
 *                                      the OR is an add)
 *
 * and PIE's EE.VMUL.U16 keeps the full 32-bit product and shifts it right by SAR
 * before taking the low sixteen bits (TRM 1.8.128). So a multiply by
 * 33*2^(12-2) at SAR 12 is that expression. The extraction uses the same
 * property: dst*2>>12 is dst>>11 and dst*128>>12 is dst>>5, exact because SAR 12
 * only drops bits those shifts wanted dropped.
 *
 *   2      r5 = dst>>11                          (no mask: five bits already)
 *   33792  r8 = (r5*33)>>2 = (r5<<3)|(r5>>2)
 *   128    dst>>5, then 63 -> g6 = that & 63
 *   16640  g8 = (g6*65)>>4 = (g6<<2)|(g6>>4)     (65*2^(12-4))
 *   31     b5 = dst&31                           (mask first: dst's high bits
 *   33792  b8 = (b5*33)>>2                        would leak otherwise)
 *
 * 2.2 The mix, in sixteen-bit lanes. s*a + d8*(255-a) reaches 65,025 and does
 * not fit a signed lane, so the scalar's rounded division is written as
 *
 *     c = min(s,d8) + (|s-d8|*a' + 127)/255,   a' = (s<d8) ? 255-a : a
 *
 * which is exact: writing the larger of the two as min+diff, s*a + d8*(255-a)
 * is min*255 + diff*a', and 255*min is a multiple of 255, so the floor of the
 * sum is min plus the floor of the rest. a' needs no branch: the s<d8 predicate
 * (EE.VCMP.LT.S16, TRM 1.8.85) is 0xFFFF or 0, masked to 0x00FF and XORed with
 * a, and 255^a is 255-a for a <= 255. min, max and |s-d8| come from
 * EE.VMIN/EE.VMAX/EE.VSUBS.S16 (docs/perf/pie-simd.md §5.2).
 *
 * 2.3 The division by 255. pie-simd.md §5.3 proves floor(y/255) ==
 * (y + (y>>8) + 1)>>8 through 65,152 -- exactly the largest y here, 255*255+127
 * -- but that form costs six instructions a channel, because it has to bring the
 * accumulator's own shifted value back. This kernel uses the equivalent
 *
 *     floor(y/255) == (257*(y+1))>>16
 *
 * for y = |s-d8|*a' + 127. (Proof, which the model sweeps exhaustively through
 * 65,278: 257/65536 = 1/255 - 1/(255*65536), so this form over-shoots y/255 by
 * (65535-y)/16711680, which is below 1/255 for y < 65279, and the floor of an
 * over-estimate that stays inside the same step is the floor of the true value.)
 * The accumulator holds 257*(y+1): a' is pre-scaled by 257 in the lanes --
 * multiplying by 4112 at SAR 4 gives (a'*257*16)>>4 = 257*a' -- so the
 * accumulator adds diff*257a', at most 255*65,535 = 16.7 M, well inside QACC's
 * forty bits, to the constant 257*128 = 32,896. EE.SRCMB.S16.QACC (TRM 1.8.54)
 * shifts by an address register and saturates on the way out; the readout is at
 * most 255, nowhere near the sixteen-bit saturation, and c = min + q <= 255, so
 * EE.VADDS.S16 cannot saturate either. The model prints those bounds from the
 * sweep instead of asserting them.
 *
 * 2.4 Packing. (c>>3)<<11 and (c>>2)<<5 are one multiply each at SAR 4 (by
 * 32768 and by 512) and the three fields do not overlap, so two EE.ORQ build the
 * word.
 *
 * 2.5 The dither arm. quantize() is floor(v*M/255) plus one when
 * 32*(v*M mod 255) > (2*bayer+1)*255 and q is still below M. Two facts let the
 * lane version be four instructions a channel instead of the plain division plus
 * a conditional add:
 *
 *  - q < M is redundant: q reaches M only when v*M is a multiple of 255, which
 *    for M = 31 means v = 255 and for M = 63 means v in {85,170,255}; in all four
 *    cases the remainder is zero and the increment is already suppressed.
 *  - The condition is a rounded division. With T' = floor((2*bayer+1)*255/32),
 *    "32*rem > (2b+1)*255" is "rem > T'" exactly (the model proves both
 *    directions), and
 *        q + (rem > T') == ceil((v*M - T')/255) == floor((y+254)/255)
 *    with y = v*M - T', which is the same 257*(y+1)>>16 form with a per-lane
 *    preload
 *        P = 65535 - 257*T' = 257*(255 - T')
 *    in 2,313..63,736: an unsigned sixteen-bit lane, which is why the preload is
 *    EE.MOV.U16.QACC and not its sign-extending twin (TRM 1.8.116; the same
 *    reason garden_decor_pie.c:46 gives). T' >= 7 for every bayer value, so y
 *    never goes negative and the accumulator stays below 257*16,319 = 4.2 M.
 *
 * T' and P are loop invariant, so the dither entry point computes them once per
 * call from the caller's eight threshold lanes -- one bayer value per column,
 * bayer4[y&3][(x+i)&3], because the renderer indexes that table with the
 * absolute column (ksn_render.c:108).
 *
 * ---------------------------------------------------------------------------
 * 3. Shape, registers, and why the two arms are written out twice
 *
 *  - The caller owns the run: `blocks` eight-pixel blocks from a 16-byte aligned
 *    `pixels` pointer (EE.VLD.128/EE.VST.128.IP force the low four address bits
 *    to zero, TRM 1.8.88/1.8.192, so a run that does not start on an eight-pixel
 *    boundary would read and write its neighbour), one source colour and one
 *    opacity for the whole run, and, on the dither arm, eight bayer thresholds
 *    for the run's column phase. That is the shape the renderer's direct path
 *    has: RECT/ROUND_RECT/STROKE sample one colour, and the dither phase repeats
 *    every four columns, so it is constant across a row of eight-pixel blocks.
 *  - a == 0 returns before anything is written, matching blend()'s early return.
 *  - The two arms share the unpack and the mix but not the pack, and their
 *    instruction streams differ in the middle of the block body: the dither
 *    tail cannot be skipped by a branch without a jump instruction these kernels
 *    otherwise do not need. They are two static functions, one asm block each.
 *    tools/pie/test_kernels.py runs both against one scalar reference and
 *    compares their shared prefix instruction by instruction, so a drift between
 *    the two copies fails the test rather than the picture.
 *  - Eight q registers are the whole budget and the mix needs five live values at
 *    its peak. The unpack runs at SAR 12 and leaves the three expanded channels
 *    in q1/q2/q3; the mix and pack run at SAR 4 and take each channel's constants
 *    in the register its source has just vacated, so 255/4112/32896 are re-read
 *    once per channel instead of being held. The dither arm holds P in a register
 *    across the loop and re-reads the quantize constants per channel.
 *  - The constant table is walked with EE.VLDBC.16.IP, so the pointer is reset
 *    from a second register at the top of every block: the loads happen three
 *    times a block, and the table must be read from its start each time.
 *  - The loop counter is addi/bnez rather than loopgtz: the dither body is over
 *    the 256-byte limit loopgtz imposes, and an explicit counter behaves the same
 *    in the instruction-level model as on the part (loopgtz leaves its register
 *    alone there, so a shared counter would model differently from the device).
 *  - Cost, measured with the tools in this repository (nothing here is a device
 *    measurement; docs/perf/kasane-blend-pie.md has the commands):
 *
 *      ksn_blend8_thin     90 instructions a call, 74 of them EE, 11 before the
 *                          first EE; the per-block body is 79 instructions in
 *                          235 bytes, 5 stalls, 84.6 cycles (estimate)
 *      ksn_blend8_dither  115 a call, 97 EE, 11 before the first EE; 22 of the
 *                          115 are the once-a-call T'/P computation; the body is
 *                          91 instructions in 271 bytes (over loopgtz's limit,
 *                          hence addi/bnez), 22 stalls, 113.6 cycles
 *      ksn_blend8_pie      41 instructions: the alpha, the argument array, the
 *                          branch on the threshold pointer
 *
 *    Against the scalar path's own objdump (survey §7: blend 50, pack565 35,
 *    quantize 19) that is 8 x 85 = 680 instructions for eight thin pixels and
 *    8 x 142 = 1,136 for eight dithered ones: about 8.6x and 12.5x fewer
 *    instructions, at 0 moved pixels.
 */
#include <stdint.h>
#include "ksn_types.h"

/* ------------------------------------------------------------------------ */
/* The definition the assembly is checked against: ksn_render.c:101-111 and
 * :190-199, copied rather than called because they are static there. The
 * duplicates (bayer4, quantize, pack565) are what a wiring commit folds back
 * into ksn_render.c; until then this copy is the scalar reference. */

/* ksn_render.c:43 */
static const uint8_t ksn_blend_bayer4[4][4]=
    {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};

/* ksn_render.c:101-105 */
static unsigned ksn_blend_quantize(unsigned value,unsigned maximum,unsigned bayer){
    unsigned q=value*maximum/255u,remainder=value*maximum-255u*q;
    if(q<maximum&&32u*remainder>(2u*bayer+1u)*255u)q++;
    return q;
}

/* ksn_render.c:106-111, taking the three channels separately. */
static uint16_t ksn_blend_pack565(unsigned r,unsigned g,unsigned b,unsigned bayer,bool dither){
    if(!dither)return (uint16_t)((r>>3)<<11|(g>>2)<<5|(b>>3));
    return (uint16_t)(ksn_blend_quantize(r,31,bayer)<<11|
                      ksn_blend_quantize(g,63,bayer)<<5|ksn_blend_quantize(b,31,bayer));
}

/* ksn_render.c:190-199 plus :44's mul8 and :46-54's bayer lookup. dst comes back
 * unchanged when the effective alpha is zero, which is blend()'s early return. */
__attribute__((unused))
static uint16_t ksn_blend_scalar_ref(uint16_t dst,ksn_rgba src,unsigned opacity,
                                     bool dither,int x,int y){
    unsigned a=((src&255u)*opacity+127u)/255u;
    if(!a)return dst;
    unsigned r=(dst>>11)&31u,g=(dst>>5)&63u,b=dst&31u;
    r=(r<<3)|(r>>2);g=(g<<2)|(g>>4);b=(b<<3)|(b>>2);
    r=((src>>24)*a+r*(255u-a)+127u)/255u;
    g=(((src>>16)&255u)*a+g*(255u-a)+127u)/255u;
    b=(((src>>8)&255u)*a+b*(255u-a)+127u)/255u;
    return ksn_blend_pack565(r,g,b,ksn_blend_bayer4[(unsigned)y&3u][(unsigned)x&3u],dither);
}

/* ------------------------------------------------------------------------ */
/* The lane arithmetic, instruction for instruction, so that a host build of
 * this file runs the same sequence the assembly below runs. Lane semantics are
 * the TRM's (1.8.x): VMUL keeps the 32-bit product, shifts it by SAR and
 * truncates to the low sixteen bits; QACC is eight forty-bit lanes with unsigned
 * saturation; SRCMB shifts QACC by an amount taken from a register and writes
 * the saturated value both back to QACC and to its destination. */

#define KSN_LANES 8
typedef uint16_t ksn_v8[KSN_LANES];

static int16_t ksn_lane_s16(uint16_t v){return v<0x8000u?(int16_t)v:(int16_t)(v-0x10000u);}
static uint16_t ksn_lane_sat16(int v){return (uint16_t)(v>32767?32767:v<-32768?-32768:v);}
static void ksn_v_splat(ksn_v8 d,unsigned v){for(int i=0;i<KSN_LANES;i++)d[i]=(uint16_t)v;}
static void ksn_v_vmul(ksn_v8 d,const ksn_v8 x,const ksn_v8 y,int sar){
    for(int i=0;i<KSN_LANES;i++)d[i]=(uint16_t)(((uint32_t)x[i]*y[i])>>sar);
}
static void ksn_v_vadds(ksn_v8 d,const ksn_v8 x,const ksn_v8 y){
    for(int i=0;i<KSN_LANES;i++)d[i]=ksn_lane_sat16(ksn_lane_s16(x[i])+ksn_lane_s16(y[i]));
}
static void ksn_v_vsubs(ksn_v8 d,const ksn_v8 x,const ksn_v8 y){
    for(int i=0;i<KSN_LANES;i++)d[i]=ksn_lane_sat16(ksn_lane_s16(x[i])-ksn_lane_s16(y[i]));
}
static void ksn_v_vmin(ksn_v8 d,const ksn_v8 x,const ksn_v8 y){
    for(int i=0;i<KSN_LANES;i++)d[i]=ksn_lane_s16(x[i])<ksn_lane_s16(y[i])?x[i]:y[i];
}
static void ksn_v_vmax(ksn_v8 d,const ksn_v8 x,const ksn_v8 y){
    for(int i=0;i<KSN_LANES;i++)d[i]=ksn_lane_s16(x[i])>ksn_lane_s16(y[i])?x[i]:y[i];
}
static void ksn_v_vcmp_lt(ksn_v8 d,const ksn_v8 x,const ksn_v8 y){
    for(int i=0;i<KSN_LANES;i++)d[i]=ksn_lane_s16(x[i])<ksn_lane_s16(y[i])?0xFFFFu:0u;
}
static void ksn_v_and(ksn_v8 d,const ksn_v8 x,const ksn_v8 y){
    for(int i=0;i<KSN_LANES;i++)d[i]=x[i]&y[i];
}
static void ksn_v_xor(ksn_v8 d,const ksn_v8 x,const ksn_v8 y){
    for(int i=0;i<KSN_LANES;i++)d[i]=x[i]^y[i];
}
static void ksn_v_or(ksn_v8 d,const ksn_v8 x,const ksn_v8 y){
    for(int i=0;i<KSN_LANES;i++)d[i]=x[i]|y[i];
}
static void ksn_v_load(ksn_v8 d,const uint16_t *src){for(int i=0;i<KSN_LANES;i++)d[i]=src[i];}
static void ksn_v_store(uint16_t *dst,const ksn_v8 v){for(int i=0;i<KSN_LANES;i++)dst[i]=v[i];}

static uint64_t ksn_lane_qacc[KSN_LANES];
static void ksn_v_mov_u16_qacc(const ksn_v8 x){          /* 1.8.116 */
    for(int i=0;i<KSN_LANES;i++)ksn_lane_qacc[i]=x[i];
}
static void ksn_v_vmulas(const ksn_v8 x,const ksn_v8 y){ /* 1.8.163 */
    for(int i=0;i<KSN_LANES;i++){
        ksn_lane_qacc[i]+=(uint64_t)x[i]*(uint64_t)y[i];
        if(ksn_lane_qacc[i]>0xFFFFFFFFFFull)ksn_lane_qacc[i]=0xFFFFFFFFFFull;
    }
}
static void ksn_v_srcmb(ksn_v8 d,int shift){             /* 1.8.54 */
    for(int i=0;i<KSN_LANES;i++){
        ksn_lane_qacc[i]>>=shift;
        d[i]=ksn_lane_sat16((int)ksn_lane_qacc[i]);
    }
}

/* One channel of the mix: source channel `s`, expanded destination channel `d`,
 * effective alpha `a`, result in `c`. The instruction order is the assembly's. */
#define KSN_LANE_MIX(s,d,a,c,one) do{                                   \
    ksn_v8 sr,mask,mn,mx,delta,ap;                                      \
    ksn_v_splat(sr,(s));                                                \
    ksn_v_vcmp_lt(mask,sr,(d));      /* s < d8 */                       \
    ksn_v_splat(one,255u);                                              \
    ksn_v_and(mask,mask,one);                                           \
    ksn_v_xor(ap,mask,(a));          /* a' = 255-a or a */              \
    ksn_v_vmin(mn,sr,(d));                                              \
    ksn_v_vmax(mx,sr,(d));                                              \
    ksn_v_vsubs(delta,mx,mn);        /* |s-d8| */                       \
    ksn_v_splat(one,4112u);                                             \
    ksn_v_vmul(ap,ap,one,4);         /* 257*a' */                       \
    ksn_v_splat(one,32896u);                                            \
    ksn_v_mov_u16_qacc(one);                                            \
    ksn_v_vmulas(delta,ap);                                             \
    ksn_v_srcmb(c,16);               /* floor(y/255), y = |s-d8|*a'+127 */ \
    ksn_v_vadds(c,c,mn);             /* c = min + q */                  \
}while(0)

/* The two arms as the assembly writes them: same unpack, same mix, different
 * pack. `args` is {a, sr, sg, sb} as the dispatcher builds it. */
__attribute__((unused))
static void ksn_blend8_lane_thin(uint16_t *pixels,int blocks,const int16_t *args){
    ksn_v8 word,r5,r8,g6,g8,b5,b8,a,one,partial,f;
    for(int b=0;b<blocks;b++,pixels+=KSN_LANES){
        ksn_v_load(word,pixels);                                  /* SAR 12 */
        ksn_v_splat(one,2u);     ksn_v_vmul(r5,word,one,12);      /* dst>>11 */
        ksn_v_splat(one,33792u); ksn_v_vmul(r8,r5,one,12);        /* (r5*33)>>2 */
        ksn_v_splat(one,128u);   ksn_v_vmul(g6,word,one,12);      /* dst>>5 */
        ksn_v_splat(one,63u);    ksn_v_and(g6,g6,one);
        ksn_v_splat(one,16640u); ksn_v_vmul(g8,g6,one,12);        /* (g6*65)>>4 */
        ksn_v_splat(one,31u);    ksn_v_and(b5,word,one);
        ksn_v_splat(one,33792u); ksn_v_vmul(b8,b5,one,12);
        ksn_v_splat(a,(unsigned)(args[0]&255));                   /* SAR 4 */
        KSN_LANE_MIX((unsigned)(args[1]&255),r8,a,f,one);         /* red */
        ksn_v_splat(one,2u);     ksn_v_vmul(f,f,one,4);           /* c>>3 */
        ksn_v_splat(one,32768u); ksn_v_vmul(partial,f,one,4);     /* <<11 */
        KSN_LANE_MIX((unsigned)(args[2]&255),g8,a,f,one);         /* green */
        ksn_v_splat(one,4u);     ksn_v_vmul(f,f,one,4);           /* c>>2 */
        ksn_v_splat(one,512u);   ksn_v_vmul(f,f,one,4);           /* <<5 */
        ksn_v_or(partial,partial,f);
        KSN_LANE_MIX((unsigned)(args[3]&255),b8,a,f,one);         /* blue */
        ksn_v_splat(one,2u);     ksn_v_vmul(f,f,one,4);           /* c>>3 */
        ksn_v_or(partial,partial,f);
        ksn_v_store(pixels,partial);
    }
}

__attribute__((unused))
static void ksn_blend8_lane_dither(uint16_t *pixels,int blocks,const int16_t *args,
                                   const uint16_t *thresholds){
    ksn_v8 word,r5,r8,g6,g8,b5,b8,a,one,partial,f,t,preload;
    /* T' = floor((2*bayer+1)*255/32) and P = 257*(255-T'), once per call. */
    ksn_v_load(t,thresholds);
    ksn_v_splat(one,8160u);  ksn_v_vmul(t,t,one,4);          /* 510*bayer */
    ksn_v_splat(one,255u);   ksn_v_vadds(t,t,one);           /* (2*b+1)*255 */
    ksn_v_mov_u16_qacc(t);
    ksn_v_srcmb(t,5);                                        /* T' */
    ksn_v_splat(one,255u);   ksn_v_vsubs(t,one,t);           /* 255-T' */
    ksn_v_splat(one,4112u);  ksn_v_vmul(preload,t,one,4);    /* P */
    for(int b=0;b<blocks;b++,pixels+=KSN_LANES){
        ksn_v_load(word,pixels);                                 /* SAR 12 */
        ksn_v_splat(one,2u);     ksn_v_vmul(r5,word,one,12);
        ksn_v_splat(one,33792u); ksn_v_vmul(r8,r5,one,12);
        ksn_v_splat(one,128u);   ksn_v_vmul(g6,word,one,12);
        ksn_v_splat(one,63u);    ksn_v_and(g6,g6,one);
        ksn_v_splat(one,16640u); ksn_v_vmul(g8,g6,one,12);
        ksn_v_splat(one,31u);    ksn_v_and(b5,word,one);
        ksn_v_splat(one,33792u); ksn_v_vmul(b8,b5,one,12);
        ksn_v_splat(a,(unsigned)(args[0]&255));                  /* SAR 4 */
        KSN_LANE_MIX((unsigned)(args[1]&255),r8,a,f,one);        /* red */
        ksn_v_splat(one,16u*31u); ksn_v_vmul(f,f,one,4);         /* 31*c */
        ksn_v_mov_u16_qacc(preload);
        ksn_v_splat(one,257u);    ksn_v_vmulas(f,one);           /* P + 257*31*c */
        ksn_v_srcmb(f,16);                                       /* the five bits */
        ksn_v_splat(one,32768u);  ksn_v_vmul(partial,f,one,4);   /* <<11 */
        KSN_LANE_MIX((unsigned)(args[2]&255),g8,a,f,one);        /* green */
        ksn_v_splat(one,16u*63u); ksn_v_vmul(f,f,one,4);         /* 63*c */
        ksn_v_mov_u16_qacc(preload);
        ksn_v_splat(one,257u);    ksn_v_vmulas(f,one);
        ksn_v_srcmb(f,16);                                       /* the six bits */
        ksn_v_splat(one,512u);    ksn_v_vmul(f,f,one,4);         /* <<5 */
        ksn_v_or(partial,partial,f);
        KSN_LANE_MIX((unsigned)(args[3]&255),b8,a,f,one);        /* blue */
        ksn_v_splat(one,16u*31u); ksn_v_vmul(f,f,one,4);
        ksn_v_mov_u16_qacc(preload);
        ksn_v_splat(one,257u);    ksn_v_vmulas(f,one);
        ksn_v_srcmb(f,16);                                       /* already low */
        ksn_v_or(partial,partial,f);
        ksn_v_store(pixels,partial);
    }
}

/* ------------------------------------------------------------------------ */
/* The device kernel: two entry points, one asm block each. The dispatcher below
 * picks by the threshold pointer. Nothing here runs on a host build. */

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define KSN_BLEND_PIE_ASM 1
#else
#define KSN_BLEND_PIE_ASM 0
#endif

#if KSN_BLEND_PIE_ASM

/* The thin arm: no dither. One multiply and one shift a channel for the mix, one
 * each for the pack. */
static void ksn_blend8_thin(uint16_t *pixels,int blocks,const int16_t *args){
    /* In load order: the program below walks this table with EE.VLDBC.16.IP and
     * resets the pointer at the top of every block, so moving a load past
     * another swaps two constants and no amount of reading the diff shows it.
     * tools/pie/test_kernels.py evaluates this initializer and runs the assembly
     * with it, so the order and the values are both checked. */
    static const int16_t k[21] __attribute__((aligned(4)))={
        2,                        /* dst>>11, SAR 12 */
        33*1024,                  /* r5 -> (r5<<3)|(r5>>2) */
        128,                      /* dst>>5 */
        63,                       /* the six-bit field */
        65*256,                   /* g6 -> (g6<<2)|(g6>>4) */
        31,                       /* the five-bit field */
        33792,                    /* b5 -> (b5<<3)|(b5>>2) */
        255,4112,32896,           /* the a' mask, its 257 scale, the preload */
        2,32768,                  /* c>>3 and its placement */
        255,4112,32896,           /* the same three, green */
        4,512,                    /* c>>2 and its placement */
        255,4112,32896,           /* and blue */
        2                         /* c>>3 (the five bits are already low) */
    };
    const int16_t *kp=k,*kb=k;
    unsigned sh12=12,sh4=4,sh16=16;
    __asm__ volatile(
        "1:\n"
        "wsr.sar         %[sh12]\n"
        "mov             %[kp], %[kb]\n"
        "ee.vld.128.ip   q0, %[dst], 0\n"
        "ee.vldbc.16.ip  q4, %[kp], 2\n"
        "ee.vldbc.16.ip  q7, %[kp], 2\n"
        "ee.vmul.u16     q5, q0, q4\n"
        "ee.vldbc.16.ip  q4, %[kp], 2\n"
        "ee.vmul.u16     q1, q5, q7\n"
        "ee.vldbc.16.ip  q7, %[kp], 2\n"
        "ee.vmul.u16     q5, q0, q4\n"
        "ee.vldbc.16.ip  q4, %[kp], 2\n"
        "ee.andq         q5, q5, q7\n"
        "ee.vldbc.16.ip  q7, %[kp], 2\n"
        "ee.vmul.u16     q2, q5, q4\n"
        "ee.andq         q5, q0, q7\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vmul.u16     q3, q5, q0\n"
        "wsr.sar         %[sh4]\n"
        "ee.vldbc.16     q7, %[pa]\n"
        /* red */
        "ee.vldbc.16     q4, %[pr]\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vcmp.lt.s16  q5, q4, q1\n"
        "ee.andq         q5, q5, q0\n"
        "ee.xorq         q5, q5, q7\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vmin.s16     q6, q4, q1\n"
        "ee.vmul.u16     q5, q5, q0\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vmax.s16     q4, q4, q1\n"
        "ee.vsubs.s16    q4, q4, q6\n"
        "ee.mov.u16.qacc q0\n"
        "ee.vmulas.u16.qacc q4, q5\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.srcmb.s16.qacc q5, %[sh16], 0\n"
        "ee.vadds.s16    q5, q5, q6\n"
        "ee.vmul.u16     q5, q5, q0\n"
        "ee.vldbc.16.ip  q6, %[kp], 2\n"
        "ee.vmul.u16     q1, q5, q6\n"
        /* green */
        "ee.vldbc.16     q4, %[pg]\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vcmp.lt.s16  q5, q4, q2\n"
        "ee.andq         q5, q5, q0\n"
        "ee.xorq         q5, q5, q7\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vmin.s16     q6, q4, q2\n"
        "ee.vmul.u16     q5, q5, q0\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vmax.s16     q4, q4, q2\n"
        "ee.vsubs.s16    q4, q4, q6\n"
        "ee.mov.u16.qacc q0\n"
        "ee.vmulas.u16.qacc q4, q5\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.srcmb.s16.qacc q5, %[sh16], 0\n"
        "ee.vadds.s16    q5, q5, q6\n"
        "ee.vmul.u16     q5, q5, q0\n"
        "ee.vldbc.16.ip  q6, %[kp], 2\n"
        "ee.vmul.u16     q5, q5, q6\n"
        "ee.orq          q1, q1, q5\n"
        /* blue */
        "ee.vldbc.16     q4, %[pb]\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vcmp.lt.s16  q5, q4, q3\n"
        "ee.andq         q5, q5, q0\n"
        "ee.xorq         q5, q5, q7\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vmin.s16     q6, q4, q3\n"
        "ee.vmul.u16     q5, q5, q0\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vmax.s16     q4, q4, q3\n"
        "ee.vsubs.s16    q4, q4, q6\n"
        "ee.mov.u16.qacc q0\n"
        "ee.vmulas.u16.qacc q4, q5\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.srcmb.s16.qacc q5, %[sh16], 0\n"
        "ee.vadds.s16    q5, q5, q6\n"
        "ee.vmul.u16     q5, q5, q0\n"
        "ee.orq          q1, q1, q5\n"
        "ee.vst.128.ip   q1, %[dst], 16\n"
        "addi            %[blocks], %[blocks], -1\n"
        "bnez            %[blocks], 1b\n"
        : [dst] "+&a"(pixels), [kp] "+&a"(kp), [blocks] "+&a"(blocks)
        : [kb] "a"(kb), [pa] "a"(args+0), [pr] "a"(args+1), [pg] "a"(args+2),
          [pb] "a"(args+3), [sh12] "a"(sh12), [sh4] "a"(sh4), [sh16] "a"(sh16)
        : "memory");
}

/* The dither arm: the same unpack and mix, and quantize()'s dither pack -- one
 * rounded division a channel with T' folded into its preload. */
static void ksn_blend8_dither(uint16_t *pixels,int blocks,const int16_t *args,
                              const uint16_t *thresholds){
    static const int16_t k[28] __attribute__((aligned(4)))={
        8160,                     /* 510*bayer, SAR 4 */
        255,                      /* (2*bayer+1)*255 */
        255,                      /* 255 - T' */
        4112,                     /* T' -> 257*T' */
        2,                        /* dst>>11, SAR 12 */
        33*1024,                  /* r5 -> (r5<<3)|(r5>>2) */
        128,                      /* dst>>5 */
        63,                       /* the six-bit field */
        65*256,                   /* g6 -> (g6<<2)|(g6>>4) */
        31,                       /* the five-bit field */
        33792,                    /* b5 -> (b5<<3)|(b5>>2) */
        255,4112,32896,           /* red: the a' mask, its 257 scale, the preload */
        16*31,257,32768,          /* red: 31*c, the quantize scale, the placement */
        255,4112,32896,           /* green */
        16*63,257,512,            /* green: 63*c, the scale, the placement */
        255,4112,32896,           /* blue */
        16*31,257                 /* blue: 31*c and the scale (no placement) */
    };
    const int16_t *kp=k,*kb=k+4;
    unsigned sh12=12,sh4=4,sh5=5,sh16=16;
    __asm__ volatile(
        "wsr.sar         %[sh4]\n"
        "ee.vld.128.ip   q5, %[thr], 0\n"        /* the eight bayer lanes */
        "ee.vldbc.16.ip  q4, %[kp], 2\n"
        "ee.vmul.u16     q5, q5, q4\n"           /* 510*bayer */
        "ee.vldbc.16.ip  q4, %[kp], 2\n"
        "ee.vadds.s16    q5, q5, q4\n"           /* T = (2*bayer+1)*255 */
        "ee.mov.u16.qacc q5\n"
        "ee.srcmb.s16.qacc q5, %[sh5], 0\n"      /* T' = floor(T/32) */
        "ee.vldbc.16.ip  q4, %[kp], 2\n"
        "ee.vsubs.s16    q5, q4, q5\n"           /* 255 - T' */
        "ee.vldbc.16.ip  q4, %[kp], 2\n"
        "ee.vmul.u16     q6, q5, q4\n"           /* P = 257*(255-T') */
        "1:\n"
        "wsr.sar         %[sh12]\n"
        "mov             %[kp], %[kb]\n"
        "ee.vld.128.ip   q0, %[dst], 0\n"
        "ee.vldbc.16.ip  q4, %[kp], 2\n"
        "ee.vldbc.16.ip  q7, %[kp], 2\n"
        "ee.vmul.u16     q5, q0, q4\n"
        "ee.vldbc.16.ip  q4, %[kp], 2\n"
        "ee.vmul.u16     q1, q5, q7\n"
        "ee.vldbc.16.ip  q7, %[kp], 2\n"
        "ee.vmul.u16     q5, q0, q4\n"
        "ee.vldbc.16.ip  q4, %[kp], 2\n"
        "ee.andq         q5, q5, q7\n"
        "ee.vldbc.16.ip  q7, %[kp], 2\n"
        "ee.vmul.u16     q2, q5, q4\n"
        "ee.andq         q5, q0, q7\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vmul.u16     q3, q5, q0\n"
        "wsr.sar         %[sh4]\n"
        "ee.vldbc.16     q7, %[pa]\n"
        /* red: q1 = r8, q6 = P, partial ends in q4 */
        "ee.vldbc.16     q4, %[pr]\n"
        "ee.vcmp.lt.s16  q5, q4, q1\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.andq         q5, q5, q0\n"
        "ee.xorq         q5, q5, q7\n"
        "ee.vmin.s16     q0, q4, q1\n"
        "ee.vmax.s16     q4, q4, q1\n"
        "ee.vsubs.s16    q4, q4, q0\n"
        "ee.vldbc.16.ip  q1, %[kp], 2\n"
        "ee.vmul.u16     q5, q5, q1\n"
        "ee.vldbc.16.ip  q1, %[kp], 2\n"
        "ee.mov.u16.qacc q1\n"
        "ee.vmulas.u16.qacc q4, q5\n"
        "ee.srcmb.s16.qacc q5, %[sh16], 0\n"
        "ee.vadds.s16    q5, q5, q0\n"
        "ee.vldbc.16.ip  q4, %[kp], 2\n"
        "ee.vmul.u16     q4, q5, q4\n"
        "ee.mov.u16.qacc q6\n"
        "ee.vldbc.16.ip  q5, %[kp], 2\n"
        "ee.vmulas.u16.qacc q4, q5\n"
        "ee.srcmb.s16.qacc q4, %[sh16], 0\n"
        "ee.vldbc.16.ip  q5, %[kp], 2\n"
        "ee.vmul.u16     q4, q4, q5\n"
        /* green */
        "ee.vldbc.16     q5, %[pg]\n"
        "ee.vcmp.lt.s16  q0, q5, q2\n"
        "ee.vldbc.16.ip  q1, %[kp], 2\n"
        "ee.andq         q0, q0, q1\n"
        "ee.xorq         q0, q0, q7\n"
        "ee.vmin.s16     q1, q5, q2\n"
        "ee.vmax.s16     q5, q5, q2\n"
        "ee.vsubs.s16    q5, q5, q1\n"
        "ee.vldbc.16.ip  q2, %[kp], 2\n"
        "ee.vmul.u16     q0, q0, q2\n"
        "ee.vldbc.16.ip  q2, %[kp], 2\n"
        "ee.mov.u16.qacc q2\n"
        "ee.vmulas.u16.qacc q5, q0\n"
        "ee.srcmb.s16.qacc q0, %[sh16], 0\n"
        "ee.vadds.s16    q0, q0, q1\n"
        "ee.vldbc.16.ip  q5, %[kp], 2\n"
        "ee.vmul.u16     q5, q0, q5\n"
        "ee.mov.u16.qacc q6\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vmulas.u16.qacc q5, q0\n"
        "ee.srcmb.s16.qacc q5, %[sh16], 0\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vmul.u16     q5, q5, q0\n"
        "ee.orq          q4, q4, q5\n"
        /* blue */
        "ee.vldbc.16     q5, %[pb]\n"
        "ee.vcmp.lt.s16  q0, q5, q3\n"
        "ee.vldbc.16.ip  q1, %[kp], 2\n"
        "ee.andq         q0, q0, q1\n"
        "ee.xorq         q0, q0, q7\n"
        "ee.vmin.s16     q1, q5, q3\n"
        "ee.vmax.s16     q5, q5, q3\n"
        "ee.vsubs.s16    q5, q5, q1\n"
        "ee.vldbc.16.ip  q3, %[kp], 2\n"
        "ee.vmul.u16     q0, q0, q3\n"
        "ee.vldbc.16.ip  q3, %[kp], 2\n"
        "ee.mov.u16.qacc q3\n"
        "ee.vmulas.u16.qacc q5, q0\n"
        "ee.srcmb.s16.qacc q0, %[sh16], 0\n"
        "ee.vadds.s16    q0, q0, q1\n"
        "ee.vldbc.16.ip  q1, %[kp], 2\n"
        "ee.vmul.u16     q1, q0, q1\n"
        "ee.mov.u16.qacc q6\n"
        "ee.vldbc.16.ip  q0, %[kp], 2\n"
        "ee.vmulas.u16.qacc q1, q0\n"
        "ee.srcmb.s16.qacc q1, %[sh16], 0\n"
        "ee.orq          q4, q4, q1\n"
        "ee.vst.128.ip   q4, %[dst], 16\n"
        "addi            %[blocks], %[blocks], -1\n"
        "bnez            %[blocks], 1b\n"
        : [dst] "+&a"(pixels), [kp] "+&a"(kp), [blocks] "+&a"(blocks)
        : [thr] "a"(thresholds), [kb] "a"(kb), [pa] "a"(args+0), [pr] "a"(args+1),
          [pg] "a"(args+2), [pb] "a"(args+3),
          [sh12] "a"(sh12), [sh4] "a"(sh4), [sh5] "a"(sh5), [sh16] "a"(sh16)
        : "memory");
}

#else

static void ksn_blend8_thin(uint16_t *pixels,int blocks,const int16_t *args){
    ksn_blend8_lane_thin(pixels,blocks,args);
}
static void ksn_blend8_dither(uint16_t *pixels,int blocks,const int16_t *args,
                              const uint16_t *thresholds){
    ksn_blend8_lane_dither(pixels,blocks,args,thresholds);
}

#endif

/* Eight destination pixels from a 16-byte aligned pointer, one source colour and
 * one opacity. `thresholds` is NULL for the thin pack, or eight bayer values
 * (0..15) for the run's column phase otherwise. Writes nothing when the
 * effective alpha is zero. PIE is coprocessor 3: owner task only. */
void ksn_blend8_pie(uint16_t *pixels,int blocks,ksn_rgba src,uint8_t opacity,
                    const uint16_t *thresholds){
    if(!pixels||blocks<=0)return;
    unsigned a=((src&255u)*opacity+127u)/255u;
    if(!a)return;
    int16_t args[4] __attribute__((aligned(4)))={
        (int16_t)a,(int16_t)(src>>24),(int16_t)((src>>16)&255u),(int16_t)((src>>8)&255u)
    };
    if(thresholds)ksn_blend8_dither(pixels,blocks,args,thresholds);
    else ksn_blend8_thin(pixels,blocks,args);
}
