#include "ocean.h"
#include "scene_mem.h"
#include "scene_tables.h"
#include "stars.h"
#include "board.h"
#include "esp_cpu.h"
#include <math.h>
#include <stdbool.h>

// OCEAN + STARS. Perspective compresses the swell spacing towards a visible
// horizon; noise bends the coherent wave fronts. Lifted out of shell.c, which
// held this and LEVEL WAVE inline while every other background was already its
// own file -- the asymmetry that made shell.c grow with each one added.
//
// The assembly below is moved verbatim. tools/pie/test_kernels.py extracts it
// from this file now, and tools/pie/stalls.py is pointed here too.

#define W LCD_W
#define H LCD_H

// Everything this scene needs for a frame, in one block borrowed from
// scene_mem: 3,480 bytes that used to be .bss held from boot to power-off, on
// a board where the heap is the binding constraint. See scene_mem.h.
//
// ocean_cols is why scene_mem promises 16-byte alignment. The row below walks
// it with EE.VLD.128, and a 128-bit load forces the low four address bits to
// zero rather than faulting -- a malloc-aligned block would have drawn a
// plausible picture from the wrong addresses.
//
// 31 blocks for 30 of pixels. The row's loop is software-pipelined: the last
// two loads of each iteration fetch the *next* block's two phase planes, so
// the final iteration reads planes 0 and 1 of a 31st block, ending at byte
// 1,472 of 1,488. That block is declared here and owned -- padding, not an
// overrun -- and the values it yields land in registers the loop exits without
// using. What does not exist is a 31st block of *pixels*.
typedef struct {
    int16_t ocean_cols[W/8+1][3][8];
    int16_t distortion[W];
    int     depth_phase[H], cross_phase[H];
    star_t  stars[STARS_N];
} ocean_block_t;
static ocean_block_t *ob;
// Named exactly as the arrays were, so every use site below reads unchanged.
static int16_t (*ocean_cols)[3][8];
static int16_t *distortion;
static int     *depth_phase,*cross_phase;
// Any address unique to this file identifies it to the block.
static const char ocean_owner;

// This scene's own animation clock, in seconds, as solar_sail and flower keep
// theirs. It used to be an absolute microsecond clock computed in shell.c and
// wrapped with `% 3600000000` before it reached a float; the wrap survives here
// for a reason the old one only half stated. A double accumulator needs no wrap
// of its own -- it holds seconds exactly for longer than the hardware will last
// -- but perlin() below takes a float, and its noise grid loses resolution as
// its argument grows: at an hour the step is 3e-5, at a hundred hours it is
// coarser than one frame's advance and the swell would visibly stutter. So the
// clock is folded at the same 3,600 s it always was, which also means the
// hourly phase discontinuity the old modulus produced is unchanged rather than
// quietly removed.
#define OCEAN_CLOCK_FOLD 3600.0
static double elapsed;

static float fade(float x) {return x*x*x*(x*(x*6-15)+10);}
static float mix(float a,float b,float t) {return a+(b-a)*t;}
static unsigned hash(int x,int y) {
    unsigned h=(unsigned)x*374761393u+(unsigned)y*668265263u;
    h=(h^(h>>13))*1274126177u;return h^(h>>16);
}
static float gradient(int x,int y,float dx,float dy) {
    switch(hash(x,y)&7) {
        case 0:return dx;case 1:return -dx;case 2:return dy;case 3:return -dy;
        case 4:return (dx+dy)*0.7071f;case 5:return (dx-dy)*0.7071f;
        case 6:return (-dx+dy)*0.7071f;default:return (-dx-dy)*0.7071f;
    }
}
static float perlin(float x,float y) {
    int ix=(int)floorf(x),iy=(int)floorf(y);float fx=x-ix,fy=y-iy;
    return mix(mix(gradient(ix,iy,fx,fy),gradient(ix+1,iy,fx-1,fy),fade(fx)),
               mix(gradient(ix,iy+1,fx,fy-1),gradient(ix+1,iy+1,fx-1,fy-1),fade(fx)),fade(fy));
}
static unsigned clamp(int v) {return v<0?0:v>255?255:(unsigned)v;}


// One row of the ocean, below the horizon. `depth` and `cross` are the row's
// two phases, `span` the half-width of the reflection and `haze` its distance
// fade — all constant across the row.
//
// Not called: ocean_row_pie below does this, and this is what it means. The
// assembly cannot be read without it, and the two were checked against each
// other over every input before the scalar one was retired, so anything that
// changes here has to change there.
static void __attribute__((unused))
ocean_row_scalar(uint16_t *row, int depth, int cross, int span, int haze) {
    for(int x=0;x<LCD_W;x++) {
        int bend=distortion[x];
        int swell=sine[(depth+bend)&255];
        int ripple=sine[(cross+x*2+bend*2)&255];
        int crest=swell-180+ripple/6;if(crest<0)crest=0;
        int dx=x-160;if(dx<0)dx=-dx;
        int reflection=dx<span?(span-dx)*128/span:0;
        int glint=crest*(40+reflection)/128;
        int shade=(swell+256)/32;
        int lift=shade+haze+glint;
        row[x]=board_rgb(clamp(3+glint),clamp(20+lift),clamp(39+lift));
    }
}


// The same row on the PIE unit, eight pixels a pass, without the sine tables.
// Not bit-exact any more: the sine is a parabola with one refinement, and
// over every input one pixel in ten moves by one RGB565 step, none by more
// than two in green (checked against ocean_row_scalar with a lane-exact model
// of the instructions below; the animation itself is unchanged). Everything
// else — the reciprocal, the clamps that are not needed, SAR=11 and the RGB565
// placement by multiply — is as before.
//
// The phases arrive as 16*phase. Multiplying by 32768 with SAR=11 gives x*16,
// and the low 16 bits of that (1.8.129 keeps only those) are exactly
// (phase mod 256 - 128) * 256: the index mask and the centering fall out of
// one multiply. Call it s = 256u. Then
//     sine*16  ~=  u*(128-|u|)  =  (s/32) * (32767-|s|) >> 11
// with |s| from EE.VPRELU.S16 against -32768 shifted by 15 (1.8.182), which
// negates the lanes that are <= 0 and leaves the rest alone. The +1 in the
// phase constants keeps s off -32768 itself. The swell is then refined as
// y*(0.775+0.225|y|), which takes the worst error from 15/256 to 1.4/256; the
// ripple is divided by 6 on its way in and does not need it. The divide by 6
// is 341/2048 on |s|. shade+haze+20 is a single add, because 512*(haze+20)
// rides through the >>9 without touching its floor; blue is green+19.
//
// Every hazard in TRM table 1.7-2 is scheduled away, so the body issues one
// instruction a cycle: nothing loads a constant on its own. Each arithmetic
// instruction that has a .LD.INCP form (1.8.71, 1.8.123, 1.8.129, 1.8.199)
// also fetches, into the register it has just finished with, the constant that
// will be wanted two instructions later, from a per-row table of the constants
// already broadcast to 16 bytes; the same instructions bring in the next
// block's planes, so the loop carries four values across the block boundary.
// q7 holds 32768 for the whole row; the rest were allocated so those four land
// where the next block reads them. The rewind of the constant walk is the one
// plain instruction in the body.
//
// Section numbers are the ESP32-S3 TRM's.
static void __attribute__((noinline))
ocean_row_pie(uint16_t *row, int depth, int cross, int span, int haze) {
    int16_t k[21] __attribute__((aligned(4))) = {
        (int16_t)(((depth & 255) << 4) + 1),   /* depth16: the phase x16, +1 keeps s off -32768 */
        (int16_t)(((cross & 255) << 4) + 1),   /* cross16 */
        64,                                    /* s*64>>11 = s/32 */
        32767,                                 /* 32768-|s|, one short */
        341,                                   /* 2048/6: |s|/6 */
        5461,                                  /* 32768/6 */
        230,                                   /* 0.225 at x2048, halved for |y| at x4096 */
        1587,                                  /* 0.775 at x2048 */
        -180*16,                               /* the crest offset, at the x16 scale */
        (int16_t)span,
        (int16_t)((262144 + span - 1) / span), /* ceil(128*2048/span) */
        40,
        (int16_t)(4096 + 512*(haze + 20)),     /* 256*16 and haze+20 in one add */
        4,                                     /* >>9 */
        3,
        19,                                    /* blue = green + 19 */
        0x00F8,
        0x00FC,
        16384,                                 /* x<<3 */
        256,                                   /* x>>3 */
        (int16_t)0x8000                        /* x<<4, and the sine fold; resident in q7 */
    };
    int16_t kv[21][8] __attribute__((aligned(16)));   /* each of the above, broadcast */
    /* kp and ks are early-clobber: they start equal to kv and k, and without
       the & GCC hands them the same registers, so the walks never rewind. */
    const int16_t *in=&ocean_cols[0][0][0];
    const int16_t *kp, *ks;
    const int16_t *k8=&kv[20][0];
    int zero=0, s15=15, nk=21, blocks=LCD_W/8, sar=11;
    __asm__ volatile(
        /* broadcast the 21 constants once: some 60 cycles a row, two a block */
        "mov            %[ks], %[k]\n"
        "mov            %[kp], %[kv]\n"
        "loopgtz        %[nk], 0f\n"
        "  ee.vldbc.16.ip  q0, %[ks], 2\n"            /*                                 1.8.95 */
        "  ee.vst.128.ip   q0, %[kp], 16\n"           /*                                 1.8.192 */
        "0:\n"
        "wsr.sar        %[sar]\n"                     /* every EE.VMUL below shifts by this */
        "mov            %[kp], %[kv]\n"
        "ee.vld.128.ip  q7, %[k8], 16\n"              /* 32768, for the row              1.8.88 */
        "ee.vld.128.ip  q0, %[in], 16\n"              /* block 0: bend*16 */
        "ee.vld.128.ip  q1, %[in], 16\n"              /* block 0: the ripple phase */
        "ee.vld.128.ip  q2, %[kp], 16\n"              /* depth16 */
        "ee.vld.128.ip  q3, %[kp], 16\n"              /* cross16 */
        "loopgtz        %[blocks], 1f\n"              /* 30 blocks of 8 pixels, no branch inside */
        /* on entry: q0 = bend*16, q1 = ripple phase, q2 = depth16, q3 = cross16, kp -> 64 */
        "  ee.vadds.s16.ld.incp   q2, %[kp], q0, q0, q2\n"  /* bend16 + depth16: the swell phase, x16; load 64 */
        "  ee.vadds.s16.ld.incp   q3, %[kp], q1, q1, q3\n"  /* the ripple phase, x16; load 32767 */
        "  ee.vmul.u16            q0, q0, q7\n"  /* s = 256u: x*16 kept to 16 bits folds mod 4096 and signs it */
        "  ee.vmul.u16            q1, q1, q7\n"
        "  ee.vmul.s16            q4, q0, q2\n"  /* s/32 = 8u */
        "  ee.vmul.s16.ld.incp    q5, %[kp], q2, q1, q2\n"  /* load 341 = 2048/6 */
        "  ee.vprelu.s16          q0, q0, q7, %[s15]\n"  /* |s|: -32768*x >> 15 = -x on the lanes <= 0 */
        "  ee.vprelu.s16          q1, q1, q7, %[s15]\n"
        "  ee.vsubs.s16.ld.incp   q0, %[kp], q3, q3, q0\n"  /* 32767 - |s|; load 5461 = 32768/6 */
        "  ee.vmul.s16.ld.incp    q5, %[kp], q1, q1, q5\n"  /* |s|/6; load 230 */
        "  ee.vmul.s16.ld.incp    q3, %[kp], q4, q4, q3\n"  /* swell*16 = 8u*(32767-|s|) >> 11 ~ u*(128-|u|); load 1587 */
        "  ee.vsubs.s16.ld.incp   q1, %[kp], q0, q0, q1\n"  /* 5461 - |s|/6; load -180*16 */
        "  ee.vprelu.s16          q6, q4, q7, %[s15]\n"  /* |swell16| */
        "  ee.vmul.s16.ld.incp    q0, %[kp], q2, q2, q0\n"  /* (ripple/6)*16; load span */
        "  ee.vmul.s16.ld.incp    q5, %[in], q6, q6, q5\n"  /* 0.225*|y| at x2048; load |x-160| */
        "  ee.vadds.s16.ld.incp   q2, %[kp], q1, q2, q1\n"  /* ripple/6*16 - 180*16; load ceil(262144/span) */
        "  ee.vadds.s16.ld.incp   q3, %[kp], q6, q6, q3\n"  /* 0.775 + 0.225|y| at x2048; load 40 */
        "  ee.vsubs.s16.ld.incp   q5, %[kp], q0, q0, q5\n"  /* span - |x-160|; load 4096+512*(haze+20) */
        "  ee.vmul.s16.ld.incp    q6, %[kp], q4, q4, q6\n"  /* swell16 = y*(0.775+0.225|y|): the refinement; load 4 */
        "  ee.vrelu.s16           q0, %[zero], %[zero]\n"  /* no reflection past the span */
        "  ee.vadds.s16           q1, q1, q4\n"  /* crest*16, maybe negative */
        "  ee.vmul.s16.ld.incp    q2, %[kp], q0, q0, q2\n"  /* (span-dx)*128/span; load 3 */
        "  ee.vrelu.s16           q1, %[zero], %[zero]\n"  /* max(crest,0) */
        "  ee.vadds.s16.ld.incp   q5, %[kp], q4, q4, q5\n"  /* swell16 + 4096 + 512*(haze+20); load 19 */
        "  ee.vadds.s16.ld.incp   q3, %[kp], q0, q0, q3\n"  /* 40 + reflection; load 0xF8 */
        "  ee.vmul.s16.ld.incp    q1, %[kp], q0, q1, q0\n"  /* glint = crest*(40+refl)/128; load 0xFC */
        "  ee.vmul.s16.ld.incp    q6, %[kp], q4, q4, q6\n"  /* shade+haze+20: the 512*(haze+20) rides through >>9 exactly; load 16384 */
        "  ee.vadds.s16           q2, q0, q2\n"  /* red = 3+glint */
        "  ee.vadds.s16.ld.incp   q0, %[kp], q4, q4, q0\n"  /* green = 20+lift; load 256 */
        "  mov                    %[kp], %[kv]\n"  /* rewind the constant walk */
        "  ee.andq                q3, q2, q3\n"
        "  ee.vadds.s16.ld.incp   q2, %[kp], q5, q4, q5\n"  /* blue = 39+lift; load next block: depth16 */
        "  ee.vmul.u16            q3, q3, q7\n"
        "  ee.andq                q1, q4, q1\n"
        "  ee.vmul.u16.ld.incp    q3, %[kp], q4, q3, q7\n"  /* red field; load next block: cross16 */
        "  ee.vmul.u16.ld.incp    q0, %[in], q5, q5, q0\n"  /* blue field; load next block: bend16 */
        "  ee.vmul.u16.ld.incp    q1, %[in], q6, q1, q6\n"  /* green field; load next block: ripple phase */
        "  ee.orq                 q4, q4, q5\n"
        "  ee.orq                 q4, q4, q6\n"
        "  ee.vst.128.ip          q4, %[row], 16\n"  /* eight pixels out */
        "1:\n"
        : [row] "+a"(row), [in] "+a"(in), [kp] "=&a"(kp), [ks] "=&a"(ks)
        : [k] "a"(k), [kv] "a"(&kv[0][0]), [k8] "a"(k8), [zero] "a"(zero), [s15] "a"(s15),
          [nk] "a"(nk), [blocks] "a"(blocks), [sar] "a"(sar)
        : "memory");
}

// The vector row reads ocean_cols, so both planes that change per frame have
// to be rebuilt whenever distortion does.
static void build_columns(void) {
    for(int x=0;x<LCD_W;x++) {
        ocean_cols[x>>3][0][x&7]=(int16_t)(distortion[x]*16);
        ocean_cols[x>>3][1][x&7]=(int16_t)((2*x+2*distortion[x])*16);
    }
}

void ocean_prepare(float dt,int tilt_x,int tilt_y,unsigned variant) {
    (void)tilt_x;(void)variant;
    // A return from an app must not advance the swell by minutes in one frame:
    // dt is the wall time since the last home-screen frame, and a guest can own
    // the display for as long as it likes. The neighbours clamp for the same
    // reason.
    if(!isfinite(dt)||dt<0)dt=0;
    if(dt>0.1f)dt=0.033f;
    elapsed=fmod(elapsed+dt,OCEAN_CLOCK_FOLD);

    bool rebuild;
    ob=scene_mem(&ocean_owner,sizeof *ob,&rebuild);
    if(!ob) { ocean_cols=NULL;distortion=NULL;depth_phase=cross_phase=NULL;return; }
    ocean_cols=ob->ocean_cols; distortion=ob->distortion;
    depth_phase=ob->depth_phase; cross_phase=ob->cross_phase;
    if(rebuild) {
        // Plane 2 is |x-160|, constant for the life of the display but living
        // inside this block because the row walks all three planes with one
        // pointer. It is the only part of build_tables() that could not be
        // baked into flash with the sine and the softness curves.
        for(int x=0;x<W;x++)
            ocean_cols[x>>3][2][x&7]=(int16_t)(x<160?160-x:x-160);
    }

    float t=(float)elapsed;
    for(int x=0;x<W;x++)distortion[x]=(int16_t)(perlin(x*0.018f,t*0.12f)*28);
    build_columns();
    for(int y=37;y<H;y++) {
        float depth=800.0f/(y-28);
        depth_phase[y]=(int)((depth*1.8f+t*1.2f)*40.7437f);
        cross_phase[y]=(int)((depth*3.1f-t*0.7f)*40.7437f);
    }
    stars_prepare(ob->stars,elapsed,tilt_x,tilt_y,false);
}

uint32_t ocean_draw(uint16_t *strip,int y,int height) {
    if(!strip)return 0;
    uint32_t kernel=0;
    for(int row_y=y;row_y<y+height;row_y++) {
        uint16_t *row=strip+(size_t)(row_y-y)*W;
        // The sky above the horizon is not the kernel, and counting it as one
        // is how "loop" got read as the vector cost for a long time.
        if(row_y<=36||!ocean_cols) {
            uint16_t sky=board_rgb(5+row_y/12,13+row_y/3,29+row_y/2);
            for(int x=0;x<W;x++) row[x]=sky;
            continue;
        }
        uint32_t c0=esp_cpu_get_cycle_count();
        ocean_row_pie(row,depth_phase[row_y],cross_phase[row_y],
                      12+(row_y-36)/3, 24-(row_y-36)/5);
        kernel+=esp_cpu_get_cycle_count()-c0;
    }
    return kernel;
}

void ocean_overlay(uint16_t *strip,int y,int height) {
    if(ob) stars_draw_points(ob->stars,strip,y,height);
}
