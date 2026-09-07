#include "wave.h"
#include "scene_mem.h"
#include "scene_tables.h"
#include "stars.h"
#include "board.h"
#include "esp_cpu.h"
#include <math.h>
#include <stdbool.h>

// LEVEL WAVE. Three ribbons of light that roll with the board's tilt, over a
// parallaxing star field. Lifted out of shell.c, which held this and
// OCEAN + STARS inline while every other background was already its own file.
//
// The assembly below is moved verbatim. tools/pie/test_kernels.py extracts it
// from this file now, and tools/pie/stalls.py is pointed here too.

#define W LCD_W
#define H LCD_H

// 1,872 bytes borrowed from scene_mem rather than held in .bss from boot to
// power-off. wave_cols is why scene_mem promises 16-byte alignment: the row
// below walks it with EE.VLD.128, and a 128-bit load forces the low four
// address bits to zero rather than faulting, so a malloc-aligned block would
// have drawn a plausible picture from the wrong addresses.
//
// Unlike the ocean's, this block needs no padding block: the row loads all
// three planes inside its body and stops exactly at the end of the array.
typedef struct {
    int16_t wave_cols[W/8][3][8];
    star_t  stars[STARS_N];
} wave_block_t;
static wave_block_t *wb;
// Named exactly as the array was, so the row below reads unchanged.
static int16_t (*wave_cols)[3][8];
// Any address unique to this file identifies it to the block.
static const char wave_owner;

// This scene's own animation clock, in seconds, as solar_sail and flower keep
// theirs. It used to be an absolute microsecond clock computed in shell.c and
// folded with `% 3600000000` before it reached a float. A double accumulator
// needs no fold of its own, but the fold is kept: it is what the ribbons have
// always done, and dropping it would silently remove the hourly phase reset
// this background has had since it was written. The rate multiplies below
// happen in double and only the product reaches a float, so the phases stay
// exact for as long as the fold allows.
#define WAVE_CLOCK_FOLD 3600.0
static double elapsed;


// One row of the wave background, on the vector unit. Bit-exact with the loop
// it replaces: the three channel weights (/4, /3, /2) are folded into the
// lookup table's high half, so an indexed load fetches the light and its
// weighted form together and the unzip separates them. "Further than 64 rows
// from the ribbon" is min(d,64) into an entry that holds zero, which is how a
// per-pixel branch disappears.
//
// q0,q1 work out |y-ribbon| then carry red; q2 is the broadcast constant; q3
// and q4 accumulate the plain and weighted sums; q5 holds 64 throughout; q6
// and q7 take each layer's lookup. All eight are in use.
//
// The early-clobber on kp is load-bearing. Without it GCC gave kp and k the
// same register — they start equal — and the rewind at the top of each block
// became an increment, so the constants marched off the end of the array.
static void __attribute__((noinline))
wave_row_pie(uint16_t *row, int y, unsigned green, unsigned blue) {
    int16_t k[10] __attribute__((aligned(4))) = {
        64, (int16_t)y, 5, (int16_t)green, (int16_t)blue,
        0x00F8, (int16_t)0x8000, 0x00FC, 16384, 256
    };
    const int16_t *in = &wave_cols[0][0][0];
    const int16_t *kp;
    const uint32_t *t0 = wave_lut[0], *t1 = wave_lut[1], *t2 = wave_lut[2];
    int blocks = LCD_W / 8, sar = 11;
    __asm__ volatile(
        "wsr.sar        %[sar]\n"                        /* SAR=11 for the EE.VMUL.U16 pack (1.8.128) */
        "mov            %[kp], %[k]\n"
        "ee.vldbc.16.ip q5, %[kp], 2\n"                  /* q5 = 64, resident                    (1.8.95) */
        "loopgtz        %[blocks], 1f\n"
        "  addi         %[kp], %[k], 2\n"                /* constant walk restarts at k[1] */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"               /* q2 = y */
        "  ee.vld.128.ip   q0, %[in], 16\n"              /* ribbons[0]                           (1.8.88) */
        "  ee.vld.128.ip   q1, %[in], 16\n"              /* ribbons[1] */
        "  ee.vsubs.s16    q6, q2, q0\n"                 /* y - r0            (q2 3 back, q0 2 back) (1.8.198) */
        "  ee.vsubs.s16    q7, q2, q1\n"                 /* y - r1 */
        "  ee.vsubs.s16    q0, q0, q2\n"                 /* r0 - y */
        "  ee.vsubs.s16    q1, q1, q2\n"                 /* r1 - y */
        "  ee.vmax.s16     q0, q0, q6\n"                 /* |y - r0|                             (1.8.104) */
        "  ee.vmax.s16     q1, q1, q7\n"                 /* |y - r1| */
        "  ee.vmin.s16     q0, q0, q5\n"                 /* idx0 = min(d,64)                     (1.8.113) */
        "  ee.vmin.s16     q1, q1, q5\n"                 /* idx1 */
        /* layer 0 -> q3/q4, layer 1 -> q6/q7, interleaved                              (1.8.37) */
        "  ee.ldxq.32      q3, q0, %[t0], 0, 0\n"
        "  ee.ldxq.32      q6, q1, %[t1], 0, 0\n"
        "  ee.ldxq.32      q3, q0, %[t0], 1, 1\n"
        "  ee.ldxq.32      q6, q1, %[t1], 1, 1\n"
        "  ee.ldxq.32      q3, q0, %[t0], 2, 2\n"
        "  ee.ldxq.32      q6, q1, %[t1], 2, 2\n"
        "  ee.ldxq.32      q3, q0, %[t0], 3, 3\n"
        "  ee.ldxq.32      q6, q1, %[t1], 3, 3\n"
        "  ee.ldxq.32      q4, q0, %[t0], 0, 4\n"
        "  ee.ldxq.32      q7, q1, %[t1], 0, 4\n"
        "  ee.ldxq.32      q4, q0, %[t0], 1, 5\n"
        "  ee.ldxq.32      q7, q1, %[t1], 1, 5\n"
        "  ee.ldxq.32      q4, q0, %[t0], 2, 6\n"
        "  ee.ldxq.32      q7, q1, %[t1], 2, 6\n"
        "  ee.ldxq.32      q4, q0, %[t0], 3, 7\n"
        "  ee.ldxq.32      q7, q1, %[t1], 3, 7\n"
        "  ee.vld.128.ip   q0, %[in], 16\n"              /* ribbons[2] */
        "  ee.vunzip.16    q3, q4\n"                     /* q3 = light0, q4 = light0/4  (q4 written 3 back) (1.8.207) */
        "  ee.vunzip.16    q6, q7\n"                     /* q6 = light1, q7 = light1/3  (q7 written 3 back) */
        "  ee.vsubs.s16    q1, q2, q0\n"                 /* y - r2            (q0 3 back) */
        "  ee.vsubs.s16    q0, q0, q2\n"                 /* r2 - y */
        "  ee.vadds.s16    q3, q3, q6\n"                 /* light0+light1                        (1.8.70) */
        "  ee.vadds.s16    q4, q4, q7\n"                 /* light0/4+light1/3 */
        "  ee.vmax.s16     q0, q0, q1\n"                 /* |y - r2| */
        "  ee.vmin.s16     q0, q0, q5\n"                 /* idx2 */
        "  ee.ldxq.32      q6, q0, %[t2], 0, 0\n"
        "  ee.ldxq.32      q6, q0, %[t2], 1, 1\n"
        "  ee.ldxq.32      q6, q0, %[t2], 2, 2\n"
        "  ee.ldxq.32      q6, q0, %[t2], 3, 3\n"
        "  ee.ldxq.32      q7, q0, %[t2], 0, 4\n"
        "  ee.ldxq.32      q7, q0, %[t2], 1, 5\n"
        "  ee.ldxq.32      q7, q0, %[t2], 2, 6\n"
        "  ee.ldxq.32      q7, q0, %[t2], 3, 7\n"
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"               /* 5      (y no longer needed) */
        "  ee.vldbc.16.ip  q1, %[kp], 2\n"               /* green */
        "  ee.vunzip.16    q6, q7\n"                     /* q6 = light2, q7 = light2/2  (q7 written 3 back) */
        "  ee.vadds.s16    q6, q6, q3\n"                 /* light0+light1+light2 */
        "  ee.vadds.s16    q3, q3, q7\n"                 /* light0+light1+light2/2 */
        "  ee.vadds.s16    q4, q4, q7\n"                 /* light0/4+light1/3+light2/2 */
        "  ee.vldbc.16.ip  q7, %[kp], 2\n"               /* blue */
        "  ee.vadds.s16    q0, q4, q2\n"                 /* r = 5 + ...      (q2 loaded 6 back) */
        "  ee.vadds.s16    q3, q3, q1\n"                 /* g = green + ...  (q1 loaded 5 back) */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"               /* 0xF8 */
        "  ee.vldbc.16.ip  q1, %[kp], 2\n"               /* 32768 */
        "  ee.vldbc.16.ip  q4, %[kp], 2\n"               /* 0xFC */
        "  ee.vadds.s16    q6, q6, q7\n"                 /* b = blue + ...   (q7 loaded 4 back) */
        "  ee.vldbc.16.ip  q7, %[kp], 2\n"               /* 16384 */
        "  ee.andq         q0, q0, q2\n"                 /* r & 0xF8         (q2 4 back)         (1.8.1) */
        "  ee.andq         q3, q3, q4\n"                 /* g & 0xFC         (q4 3 back) */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"               /* 256 */
        "  ee.vmul.u16     q0, q0, q1\n"                 /* r*16                                 (1.8.128) */
        "  ee.vmul.u16     q3, q3, q7\n"                 /* g<<3             (q7 3 back) */
        "  ee.vmul.u16     q0, q0, q1\n"                 /* r*256            (q0 mul 2 back) */
        "  ee.vmul.u16     q6, q6, q2\n"                 /* b>>3             (q2 3 back) */
        "  ee.orq          q0, q0, q3\n"                 /*                  (q0 2 back, q3 3 back) (1.8.45) */
        "  ee.orq          q0, q0, q6\n"                 /*                  (q6 mul 2 back) */
        "  ee.vst.128.ip   q0, %[row], 16\n"             /*                                      (1.8.192) */
        "1:\n"
        : [row] "+a"(row), [in] "+a"(in), [kp] "=&a"(kp)
        : [k] "a"(k), [t0] "a"(t0), [t1] "a"(t1), [t2] "a"(t2),
          [blocks] "a"(blocks), [sar] "a"(sar)
        : "memory");
}

void wave_prepare(float dt,int tilt_x,int tilt_y,unsigned variant) {
    (void)variant;
    // A return from an app must not advance the ribbons by minutes in one
    // frame: dt is the wall time since the last home-screen frame, and a guest
    // can own the display for as long as it likes. The neighbours clamp for
    // the same reason.
    if(!isfinite(dt)||dt<0)dt=0;
    if(dt>0.1f)dt=0.033f;
    elapsed=fmod(elapsed+dt,WAVE_CLOCK_FOLD);

    bool rebuild;
    wb=scene_mem(&wave_owner,sizeof *wb,&rebuild);
    if(!wb) { wave_cols=NULL; return; }
    (void)rebuild;   // every column is rewritten below, every frame
    wave_cols=wb->wave_cols;

    // tan of the tilt, so the horizon rolls with the board rather than sliding.
    int level_slope=(int)(tanf(tilt_x/256.0f)*256);
    // The clock is folded to under an hour above and narrowed to float exactly
    // once, here. This core's FPU is single precision -- solar_sail's Kepler
    // comment says the same thing -- so a double that reaches the loop below
    // is 720 iterations of software emulation every frame, for a phase that
    // ends up as one of 256 table indices. The accumulator stays double
    // because that is what keeps the fold exact; the arithmetic does not.
    float t=(float)elapsed;
    for(int l=0;l<3;l++)for(int x=0;x<W;x++) {
        const float speeds[]={0.20f,0.60f,0.32f};const int depths[]={4,12,28};
        const int centers[]={54,82,116};
        float px=x+tilt_x*depths[l]/256.0f;
        // `ribbons[3][240]` used to hold this before it reached wave_cols. GCC
        // had already eliminated it -- nm showed no such symbol, because
        // nothing read it after the store -- so the declaration was 1,440 bytes
        // of nothing, which is worse than either having them or not.
        int a=(int)((px*(0.014f+l*0.004f)+t*speeds[l]+l*1.6f)*40.7437f);
        int b=(int)((px*0.009f-t*0.24f+l)*40.7437f);
        wave_cols[x>>3][l][x&7]=(int16_t)(centers[l]+tilt_y*depths[l]/256.0f
            +(x-W/2)*level_slope/256
            +(sine[a&255]*(9+l*4)+sine[b&255]*6)/256.0f);
    }
    stars_prepare(wb->stars,elapsed,tilt_x,tilt_y,true);
}

uint32_t wave_draw(uint16_t *strip,int y,int height) {
    if(!strip)return 0;
    if(!wave_cols) {
        // Without the block there is no ribbon, but there is still a night sky.
        for(int j=0;j<height;j++) {
            uint16_t *row=strip+(size_t)j*W;
            uint16_t sky=board_rgb(3,7,17);
            for(int x=0;x<W;x++) row[x]=sky;
        }
        return 0;
    }
    uint32_t c0=esp_cpu_get_cycle_count();
    for(int row_y=y;row_y<y+height;row_y++)
        wave_row_pie(strip+(size_t)(row_y-y)*W,row_y,14+row_y/7,30+row_y/5);
    return esp_cpu_get_cycle_count()-c0;
}

void wave_overlay(uint16_t *strip,int y,int height) {
    if(wb) stars_draw_layers(wb->stars,strip,y,height);
}
