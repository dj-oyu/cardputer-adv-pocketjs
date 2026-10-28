#include "wave.h"
#include "fxmath.h"
#include "scene_mem.h"
#include "scene_tables.h"
#include "stars.h"
#include "board.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>
#ifdef __XTENSA__
#include "esp_cpu.h"
#endif

// LEVEL WAVE. A body of water that stays level as the board tilts, under a
// night sky with two neon aurorae, lit by shafts of light falling from above.
//
// Three things are simulated rather than drawn, and each is cheap on purpose:
//
//   the water  a 1-D shallow-water model across the screen (61 nodes, 4 px
//              apart). Gravity follows the tilt, so tipping the board sends
//              a slosh across the tank that reflects off the walls and dies
//              away; the volume is conserved exactly by the update below.
//   the light  one ray per column, falling at the sun's angle. Where it meets
//              the surface it refracts by Snell's law (n = 1.333) about the
//              local normal, so the shafts visibly bend at the waterline.
//   caustics   every ray is followed down, row by row, and splatted into the
//              row it crosses. Where the curved surface focuses rays they
//              pile up into the bright, shifting net seen on a pool floor;
//              nothing draws the net, it is the density of the rays.
//
// The glowing lines (both aurorae and the lit edge of the water) are three
// distance-to-line lookups on the vector unit, as the ribbons always were;
// everything above adds into a per-row light plane that the same kernel sums
// in before it packs RGB565.

#define W LCD_W
#define H LCD_H

// The water model. Nodes sit every SIM_DX pixels from x=0 to x=240.
#define SIM_N  61
#define SIM_DX 4.0f
// Wave speed in px/s. The tank's first slosh mode has period 2*240/c, about
// 1.5 s, which reads as water rather than as jelly or as a spring. The step is
// held under the CFL limit (c*dt/dx = 0.67 at 120 Hz) by substepping.
#define SIM_C       320.0f
#define SIM_HZ      120.0f
#define SIM_DAMPING 0.7f
// px^2/s. Explicit diffusion is stable while nu*dt/dx^2 < 0.5; this is 0.16.
#define SIM_VISCOSITY 300.0f
// Where the surface rests, in rows. SLOSH_LIMIT is a guard against a runaway,
// not a shape: at the IMU's largest tilt (0.70 rad) a level surface is already
// 101 rows above or below the middle at the screen's edges, which the drawing
// handles (the water simply fills or leaves the screen).
#define WATER_LEVEL 84.0f
#define SLOSH_LIMIT 240.0f

// The sun's angle from vertical, in radians, added to the tilt. With the sun
// straight overhead a calm surface would refract nothing; this much slant is
// what makes the bend at the waterline visible at rest.
#define SUN_ANGLE 0.30f
#define WATER_INDEX 1.333f

// Rays start this far beyond each edge, so a slanted shaft fills the corner
// it would otherwise leave dark.
#define RAY_PAD 64
#define RAYS    (W+2*RAY_PAD)
// Sky shafts are a function of where a ray crosses y=0. The tangent of the
// ray angle is clamped to +-1 below, so that is at most H pixels either side.
#define SHAFT_OFF (RAY_PAD+H)
#define SHAFT_N   (RAYS+2*H)

// Light planes and ribbons are channel*128, the scale wave_lut is baked at.
#define CH(v) ((int)((v)*128))

// What the vector row reads, then what only C reads. wave_cols is first
// because scene_mem promises the block 16-byte alignment and the row walks it
// with EE.VLD.128, which forces the low four address bits to zero rather than
// faulting; light follows at 2,880, also a multiple of 16.
typedef struct {
    // Per eight columns: line l spans rows lo..hi, i.e. 4*centre -+ the
    // pulse offset, in quarter pixels. Planes: lo0 hi0 lo1 hi1 lo2 hi2.
    int16_t wave_cols[W/8][6][8];
    // Per eight columns: the row's light, blue, green, red, channel*128.
    int16_t light[W/8][3][8];
    // Light reaching the current row, per pixel: differences while the
    // beams are added, values after the prefix sum (wave_light_row).
    int32_t beam[W+1];
    // Per ray: surface y in 1/16 px, refracted dx/dy in 1/1024, and the
    // brightness of the shaft it carries (0..255).
    int16_t surf[RAYS];
    int16_t slope[RAYS];
    uint8_t power[RAYS];
    uint8_t shaft[SHAFT_N];
    // Shallow-water state: displacement at each node and the flow through
    // each face between them. face 0 and face SIM_N are the tank walls.
    float   h[SIM_N], u[SIM_N+1];
    star_t  stars[STARS_N];
} wave_block_t;
static wave_block_t *wb;
// Any address unique to this file identifies it to the block.
static const char wave_owner;

// This scene's own animation clock, in seconds. Folded hourly in double, as it
// always has been; the rate multiplies happen in double and only the product
// reaches a float.
#define WAVE_CLOCK_FOLD 3600.0
static double elapsed;

// Per frame: the ray angle above the water and the topmost row the surface
// reaches. The water's colour and transmission by depth are constants, baked
// into flash as wave_water_body and wave_water_light (scene_tables.h).
static float sky_tan;
static int surface_top;

// Sky colour per row, channel*128: deep indigo at the top warming to violet at
// the horizon behind the water.
static void sky_row(int y, int *r, int *g, int *b) {
    int f=y*256/(H-1);
    *r=CH(2)+CH(14)*f*f/65536;
    *g=CH(4)+CH(4)*f/256;
    *b=CH(16)+CH(20)*f/256;
}

// 1/sqrt(x) for x > 0, from the bit-trick seed and two Newton steps (relative
// error under 5e-6). sqrtf and a float divide are both calls out of this file
// on this core (fxmath.h); this needs neither.
static float rsqrt(float x) {
    union { float f; uint32_t i; } v={x};
    v.i=0x5f3759dfu-(v.i>>1);
    float y=v.f;
    y*=1.5f-0.5f*x*y*y;
    y*=1.5f-0.5f*x*y*y;
    return y;
}

#ifdef __XTENSA__
// One row of the three glowing lines plus the row's light plane, on the vector
// unit, eight pixels a pass. For each line l and pixel x:
//
//     d = min(max(y4 - lo, hi - y4), 255)        = |4y - 4c| + pulse, clamped
//     light += wave_lut[2l][d] (blue | green<<16), wave_lut[2l+1][d] (red)
//
// then the light plane is added and the three sums packed to RGB565. Every
// value is channel*128 and every add saturates, so a sum of lights clips at
// 32767 (255.99) instead of wrapping: crossing lines go white, not black. The
// pack is then a mask and one multiply per channel at SAR=11:
//     red   (R & 0x7C00) * 4096 >> 11 = R<<1  -> bits 15..11
//     green (G & 0x7E00) *  128 >> 11 = G>>4  -> bits 10..5
//     blue  (B & 0x7C00) *    2 >> 11 = B>>10 -> bits 4..0
//
// q3, q4, q5 accumulate blue, green, red; q2 holds 4y until the light plane
// needs it; q0 is the distance, q1 its partner and then the clamp; q6 and q7
// take each lookup. Each line's last unzip and add is held back to sit
// between the next line's clamp load and its use, and the light plane is
// loaded into whichever register is free first, so nothing waits on a load
// (tools/pie/stalls.py: 0 stalls). The six tables are contiguous and
// exactly 1 KiB apart, so one pointer walks them with addmi.
//
// The body is about 378 bytes, past loopgtz's 256, so this is a counted
// branch loop. The early-clobbers keep kp and tp off the registers holding
// k and t: without them GCC may share one register between a pointer and the
// base it is rewound from (the old wave row learned this the hard way).
static void __attribute__((noinline))
wave_row_pie(uint16_t *row, int y, const int16_t *light) {
    int16_t k[7] __attribute__((aligned(4))) = {
        (int16_t)(4*y), 255, 0x7C00, 0x7E00, 2, 128, 4096
    };
    const int16_t *in = &wb->wave_cols[0][0][0];
    const int16_t *kp;
    const uint32_t *tp, *t = &wave_lut[0][0];
    int blocks = W/8, sar = 11;
    __asm__ volatile(
        "wsr.sar        %[sar]\n"
        "1:\n"
        "  mov             %[kp], %[k]\n"
        "  mov             %[tp], %[t]\n"
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"                      /* q2 = 4y */
        /* line 0, straight into the accumulators */
        "  ee.vldbc.16     q6, %[kp]\n"                         /* 255, in q6 while it is free */
        "  ee.vld.128.ip   q0, %[in], 16\n"                     /* lo0 */
        "  ee.vld.128.ip   q1, %[in], 16\n"                     /* hi0 */
        "  ee.vsubs.s16    q0, q2, q0\n"                        /* 4y - lo */
        "  ee.vsubs.s16    q1, q1, q2\n"                        /* hi - 4y */
        "  ee.vmax.s16     q0, q0, q1\n"                        /* |4y - 4c| + pulse */
        "  ee.vmin.s16     q0, q0, q6\n"                        /* d = min(.., 255) */
        "  ee.ldxq.32      q3, q0, %[tp], 0, 0\n"
        "  ee.ldxq.32      q3, q0, %[tp], 1, 1\n"
        "  ee.ldxq.32      q3, q0, %[tp], 2, 2\n"
        "  ee.ldxq.32      q3, q0, %[tp], 3, 3\n"
        "  ee.ldxq.32      q4, q0, %[tp], 0, 4\n"
        "  ee.ldxq.32      q4, q0, %[tp], 1, 5\n"
        "  ee.ldxq.32      q4, q0, %[tp], 2, 6\n"
        "  ee.ldxq.32      q4, q0, %[tp], 3, 7\n"
        "  addmi           %[tp], %[tp], 1024\n"                /* red table */
        "  ee.ldxq.32      q5, q0, %[tp], 0, 0\n"
        "  ee.ldxq.32      q5, q0, %[tp], 1, 1\n"
        "  ee.ldxq.32      q5, q0, %[tp], 2, 2\n"
        "  ee.ldxq.32      q5, q0, %[tp], 3, 3\n"
        "  ee.ldxq.32      q6, q0, %[tp], 0, 4\n"
        "  ee.ldxq.32      q6, q0, %[tp], 1, 5\n"
        "  ee.ldxq.32      q6, q0, %[tp], 2, 6\n"
        "  ee.ldxq.32      q6, q0, %[tp], 3, 7\n"
        "  addmi           %[tp], %[tp], 1024\n"
        /* line 1 */
        "  ee.vld.128.ip   q0, %[in], 16\n"                     /* lo1 */
        "  ee.vld.128.ip   q1, %[in], 16\n"                     /* hi1 */
        "  ee.vsubs.s16    q0, q2, q0\n"                        /* 4y - lo */
        "  ee.vsubs.s16    q1, q1, q2\n"                        /* hi - 4y */
        "  ee.vmax.s16     q0, q0, q1\n"                        /* |4y - 4c| + pulse */
        "  ee.vldbc.16     q1, %[kp]\n"                         /* 255 */
        "  ee.vunzip.16    q3, q4\n"                            /* q3 = blue, q4 = green */
        "  ee.vunzip.16    q5, q6\n"                            /* q5 = red, q6 = 0 */
        "  ee.vmin.s16     q0, q0, q1\n"                        /* d = min(.., 255) */
        "  ee.ldxq.32      q6, q0, %[tp], 0, 0\n"
        "  ee.ldxq.32      q6, q0, %[tp], 1, 1\n"
        "  ee.ldxq.32      q6, q0, %[tp], 2, 2\n"
        "  ee.ldxq.32      q6, q0, %[tp], 3, 3\n"
        "  ee.ldxq.32      q7, q0, %[tp], 0, 4\n"
        "  ee.ldxq.32      q7, q0, %[tp], 1, 5\n"
        "  ee.ldxq.32      q7, q0, %[tp], 2, 6\n"
        "  ee.ldxq.32      q7, q0, %[tp], 3, 7\n"
        "  addmi           %[tp], %[tp], 1024\n"
        "  ee.vunzip.16    q6, q7\n"
        "  ee.vadds.s16    q3, q3, q6\n"
        "  ee.vadds.s16    q4, q4, q7\n"
        "  ee.ldxq.32      q6, q0, %[tp], 0, 0\n"
        "  ee.ldxq.32      q6, q0, %[tp], 1, 1\n"
        "  ee.ldxq.32      q6, q0, %[tp], 2, 2\n"
        "  ee.ldxq.32      q6, q0, %[tp], 3, 3\n"
        "  ee.ldxq.32      q7, q0, %[tp], 0, 4\n"
        "  ee.ldxq.32      q7, q0, %[tp], 1, 5\n"
        "  ee.ldxq.32      q7, q0, %[tp], 2, 6\n"
        "  ee.ldxq.32      q7, q0, %[tp], 3, 7\n"
        "  addmi           %[tp], %[tp], 1024\n"
        /* line 2 */
        "  ee.vld.128.ip   q0, %[in], 16\n"                     /* lo2 */
        "  ee.vld.128.ip   q1, %[in], 16\n"                     /* hi2 */
        "  ee.vsubs.s16    q0, q2, q0\n"                        /* 4y - lo */
        "  ee.vsubs.s16    q1, q1, q2\n"                        /* hi - 4y */
        "  ee.vmax.s16     q0, q0, q1\n"                        /* |4y - 4c| + pulse */
        "  ee.vldbc.16.ip  q1, %[kp], 2\n"                      /* 255, and on to the pack constants */
        "  ee.vunzip.16    q6, q7\n"
        "  ee.vadds.s16    q5, q5, q6\n"
        "  ee.vmin.s16     q0, q0, q1\n"                        /* d = min(.., 255) */
        "  ee.vld.128.ip   q1, %[lt], 16\n"                     /* the light plane's blue, early */
        "  ee.ldxq.32      q6, q0, %[tp], 0, 0\n"
        "  ee.ldxq.32      q6, q0, %[tp], 1, 1\n"
        "  ee.ldxq.32      q6, q0, %[tp], 2, 2\n"
        "  ee.ldxq.32      q6, q0, %[tp], 3, 3\n"
        "  ee.ldxq.32      q7, q0, %[tp], 0, 4\n"
        "  ee.ldxq.32      q7, q0, %[tp], 1, 5\n"
        "  ee.ldxq.32      q7, q0, %[tp], 2, 6\n"
        "  ee.ldxq.32      q7, q0, %[tp], 3, 7\n"
        "  addmi           %[tp], %[tp], 1024\n"
        "  ee.vunzip.16    q6, q7\n"
        "  ee.vadds.s16    q3, q3, q6\n"
        "  ee.vadds.s16    q4, q4, q7\n"
        "  ee.ldxq.32      q6, q0, %[tp], 0, 0\n"
        "  ee.ldxq.32      q6, q0, %[tp], 1, 1\n"
        "  ee.ldxq.32      q6, q0, %[tp], 2, 2\n"
        "  ee.ldxq.32      q6, q0, %[tp], 3, 3\n"
        "  ee.ldxq.32      q7, q0, %[tp], 0, 4\n"
        "  ee.ldxq.32      q7, q0, %[tp], 1, 5\n"
        "  ee.ldxq.32      q7, q0, %[tp], 2, 6\n"
        "  ee.ldxq.32      q7, q0, %[tp], 3, 7\n"
        "  ee.vld.128.ip   q0, %[lt], 16\n"                     /* green: q0 is free after the last lookup */
        "  ee.vunzip.16    q6, q7\n"
        "  ee.vadds.s16    q5, q5, q6\n"
        "  ee.vld.128.ip   q2, %[lt], 16\n"                     /* red */
        "  ee.vadds.s16    q3, q3, q1\n"
        "  ee.vadds.s16    q4, q4, q0\n"
        "  ee.vadds.s16    q5, q5, q2\n"
        /* pack */
        "  ee.vldbc.16.ip  q0, %[kp], 2\n"                      /* 0x7C00 */
        "  ee.vldbc.16.ip  q1, %[kp], 2\n"                      /* 0x7E00 */
        "  ee.andq         q3, q3, q0\n"
        "  ee.andq         q5, q5, q0\n"
        "  ee.andq         q4, q4, q1\n"
        "  ee.vldbc.16.ip  q0, %[kp], 2\n"                      /* 2 */
        "  ee.vldbc.16.ip  q1, %[kp], 2\n"                      /* 128 */
        "  ee.vldbc.16.ip  q2, %[kp], 2\n"                      /* 4096 */
        "  ee.vmul.u16     q3, q3, q0\n"                        /* B >> 10 */
        "  ee.vmul.u16     q4, q4, q1\n"                        /* (G & 0x7E00) >> 4 */
        "  ee.vmul.u16     q5, q5, q2\n"                        /* R << 1 */
        "  ee.orq          q3, q3, q4\n"
        "  ee.orq          q3, q3, q5\n"
        "  ee.vst.128.ip   q3, %[row], 16\n"
        "  addi            %[n], %[n], -1\n"
        "  bnez            %[n], 1b\n"
        : [row] "+a"(row), [in] "+a"(in), [lt] "+a"(light), [n] "+a"(blocks),
          [kp] "=&a"(kp), [tp] "=&a"(tp)
        : [k] "a"(k), [t] "a"(t), [sar] "a"(sar)
        : "memory");
}
#else
// The same row in C, for the host preview (tools/preview_wave.c). This is what
// the assembly means; tools/pie/test_kernels.py checks the assembly against a
// transcription of it, instruction by instruction.
static void wave_row_pie(uint16_t *row, int y, const int16_t *light) {
    for(int x=0;x<W;x++) {
        int acc[3]={0,0,0};   // blue, green, red
        for(int l=0;l<3;l++) {
            int lo=wb->wave_cols[x>>3][2*l][x&7], hi=wb->wave_cols[x>>3][2*l+1][x&7];
            int d=4*y-lo; if(hi-4*y>d)d=hi-4*y; if(d>255)d=255;
            uint32_t gb=wave_lut[2*l][d], r=wave_lut[2*l+1][d];
            acc[0]+=gb&0xFFFF; acc[1]+=gb>>16; acc[2]+=r&0xFFFF;
        }
        for(int c=0;c<3;c++) {
            acc[c]+=light[(x>>3)*24+c*8+(x&7)];
            if(acc[c]>32767)acc[c]=32767;
        }
        row[x]=(uint16_t)(((acc[2]&0x7C00)<<1)|((acc[1]&0x7E00)>>4)|((acc[0]&0x7C00)>>10));
    }
}
#endif

// Advance the water. `level` is the slope a still surface takes at this tilt.
// The update is the linearised shallow-water pair
//     du/dt = c^2 (level - dh/dx)        dh/dt = -du/dx
// on a staggered grid with closed walls (u = 0 at both ends). Because every
// face's flow leaves one node and enters the next, the sum of h -- the volume
// of water -- does not change, whatever the tilt does.
static void water_step(float dt, float level) {
    float *h=wb->h, *u=wb->u;
    int steps=(int)(dt*SIM_HZ)+1;
    float sdt=dt/steps, damp=1.0f-SIM_DAMPING*sdt;
    for(int s=0;s<steps;s++) {
        // A little viscosity on the flow rounds the front a big tip throws
        // up; the linear model alone carries it across as a hard step.
        float prev=u[0];
        for(int j=1;j<SIM_N;j++) {
            float here=u[j];
            u[j]=(here+sdt*(SIM_C*SIM_C*(level-(h[j]-h[j-1])*(1.0f/SIM_DX))
                 +SIM_VISCOSITY*(prev-2*here+u[j+1])*(1.0f/(SIM_DX*SIM_DX))))*damp;
            prev=here;
        }
        for(int i=0;i<SIM_N;i++) {
            h[i]-=sdt*(u[i+1]-u[i])*(1.0f/SIM_DX);
            // Only a blow-up reaches this; clamping a node would break the
            // conservation above, so it is a guard rather than a wall.
            if(h[i]>SLOSH_LIMIT)h[i]=SLOSH_LIMIT;
            if(h[i]<-SLOSH_LIMIT)h[i]=-SLOSH_LIMIT;
        }
    }
}

// Surface displacement at column x (any x; the ends hold flat past the
// walls). Catmull-Rom through the nodes rather than straight segments: the
// light refracts by the slope, and a slope that steps every four pixels
// focuses the rays into a comb of caustics four pixels apart.
static float water_at(float x) {
    const float *h=wb->h;
    float p=x*(1.0f/SIM_DX);
    if(p<=0)return h[0];
    if(p>=SIM_N-1)return h[SIM_N-1];
    int i=(int)p; float f=p-i;
    float p0=h[i>0?i-1:0], p1=h[i], p2=h[i+1], p3=h[i+2<SIM_N?i+2:SIM_N-1];
    return p1+0.5f*f*(p2-p0+f*(2*p0-5*p1+4*p2-p3+f*(3*(p1-p2)+p3-p0)));
}

// The sine table at a fractional index, for the same reason: the ripples'
// slope is what bends the light, and a table read at whole indices is flat
// between steps.
static float sine_at(float i) {
    int k=(int)(i+65536.0f)-65536; float f=i-k;
    return sine[k&255]+(sine[(k+1)&255]-sine[k&255])*f;
}

// The surface at column x for this frame: the simulated water plus wind
// ripples, so the caustics keep moving on a board lying still.
static float surface_lift, surface_t, surface_shift;
static float surface_y(float x) {
    float px=x+surface_shift, t=surface_t;
    return surface_lift+water_at(x)
        +(sine_at((px*0.031f+t*0.9f)*40.7437f)*3.2f
         +sine_at((px*0.047f-t*1.3f+1.7f)*40.7437f)*1.6f
         // The short ripples are the ones that focus: a sine of amplitude A
         // and wavenumber k curves the surface by A k^2, and the light it
         // refracts comes to a focus about 4/(A k^2) rows down -- here 20 to
         // 60, inside the water. The long swells above only steer.
         +sine_at((px*0.224f-t*2.1f)*40.7437f)*1.1f
         +sine_at((px*0.331f+t*1.7f+0.6f)*40.7437f)*0.55f)*(1.0f/256);
}

// The comet that runs along each line: 0 at the head, rising over 70 steps of
// tail, and a short ramp ahead of the head so it fades in rather than popping.
// Added to the distance, so it dims the narrow core and barely touches the
// wide halo.
static int pulse(int ph) {
    if(ph<12)return 8-ph*8/12;
    int o=(ph-12)*8/70;
    return o>8?8:o;
}

void wave_prepare(float dt,int tilt_x,int tilt_y,unsigned variant) {
    (void)variant;
    // A return from an app must not advance the scene by minutes in one frame.
    if(!isfinite(dt)||dt<0)dt=0;
    if(dt>0.1f)dt=0.033f;
    elapsed=fmod(elapsed+dt,WAVE_CLOCK_FOLD);

    bool rebuild;
    wb=scene_mem(&wave_owner,sizeof *wb,&rebuild);
    if(!wb)return;

    float tilt=tilt_x/256.0f;
    // tan of the tilt: the slope a level surface has on this screen.
    float level=fx_tanf(tilt);
    if(rebuild) {
        // Come back to still water rather than to whatever slosh was running.
        for(int i=0;i<SIM_N;i++)wb->h[i]=level*(i*SIM_DX-W/2);
        for(int i=0;i<=SIM_N;i++)wb->u[i]=0;
    }
    water_step(dt,level);

    float t=(float)elapsed;
    // The rate multiplies stay in double so the phases are exact for the hour.
    float breathe[3], phase[3];
    for(int l=0;l<3;l++) {
        breathe[l]=1.0f+0.3f*fx_sinf((float)fmod(elapsed*0.37,6.283185307)+l*2.1f);
        phase[l]=(float)fmod(elapsed*(70.0+30.0*l),256.0)+l*90;
    }

    // The surface, for every ray column, and its slope from the unrounded
    // curve either side (rounding to 1/16 px first would add a jitter of
    // the slope that the depth magnifies into streaks).
    surface_lift=WATER_LEVEL+tilt_y*12/256.0f;
    surface_t=t;
    surface_shift=tilt_x*12/256.0f;
    for(int r=0;r<RAYS;r++) {
        float x=(float)(r-RAY_PAD);
        wb->surf[r]=(int16_t)((int)(surface_y(x)*16.0f+16384.5f)-16384);
        float m=surface_y(x+0.5f)-surface_y(x-0.5f);
        wb->slope[r]=(int16_t)((m>8?8:m<-8?-8:m)*1024);   // inside int16 at any slosh
    }

    // The three lines. 0 and 2 are aurorae, moving as the ribbons always
    // have; 1 is the waterline itself.
    static const float speeds[]={0.20f,0.60f,0.32f};
    static const int depths[]={4,12,28}, centers[]={22,0,48}, amps[]={8,0,10};
    for(int l=0;l<3;l++)for(int x=0;x<W;x++) {
        float px=x+tilt_x*depths[l]/256.0f;
        int c4;
        if(l==1) {
            c4=(wb->surf[x+RAY_PAD]+2)>>2;
        } else {
            int a=(int)((px*(0.014f+l*0.004f)+t*speeds[l]+l*1.6f)*40.7437f);
            int b=(int)((px*0.009f-t*0.24f+l)*40.7437f);
            float c=centers[l]+tilt_y*depths[l]/256.0f+(x-W/2)*level
                +(sine[a&255]*amps[l]*breathe[l]+sine[b&255]*5)*(1.0f/256);
            c4=(int)(c*4.0f+4096.5f)-4096;
        }
        int o=pulse((int)(phase[l]-px*1.6f+1024.0f)&255);
        wb->wave_cols[x>>3][2*l][x&7]=(int16_t)(c4-o);
        wb->wave_cols[x>>3][2*l+1][x&7]=(int16_t)(c4+o);
    }

    // Light from above. The angle follows gravity, as the water does, plus
    // the sun's slant; the tangent is clamped so a shaft's origin stays in
    // the table.
    float ray=tilt+SUN_ANGLE;
    if(ray>1.2f)ray=1.2f;      // fx_tanf's accurate range is +-1.27 rad
    if(ray<-1.2f)ray=-1.2f;
    sky_tan=fx_tanf(ray);
    if(sky_tan>1)sky_tan=1;
    if(sky_tan<-1)sky_tan=-1;
    // Shafts: two drifting soft bands multiplied, so they gather into a few
    // beams that open and close, over a floor of ambient light.
    for(int i=0;i<SHAFT_N;i++) {
        int x0=i-SHAFT_OFF;
        int a=(int)((x0*0.045f+t*0.35f)*40.7437f), b=(int)((x0*0.11f-t*0.6f+1.3f)*40.7437f);
        int v=(sine[a&255]+256)>>1, w=(sine[b&255]+256)>>1;   // 0..256
        int m=v*w>>8;
        wb->shaft[i]=(uint8_t)(56+(m*m>>8)*199/256);
    }
    // Each ray: the shaft it carries, and where it goes once in the water.
    // Incoming d = (sin, cos) of the ray angle; the surface normal points
    // down into the water from the local slope; Snell in vector form,
    //     t = e d - (e cos_i - sqrt(1 - e^2 (1 - cos_i^2))) n,  e = 1/1.333.
    float ds=sky_tan*rsqrt(1+sky_tan*sky_tan), dc=rsqrt(1+sky_tan*sky_tan);
    const float e=1.0f/WATER_INDEX;
    int top=H;
    for(int r=0;r<RAYS;r++) {
        float m=wb->slope[r]*(1.0f/1024);   // the surface's, until replaced below
        float k=rsqrt(1+m*m), nx=-m*k, ny=k;
        float ci=ds*nx+dc*ny;
        float tt=1-e*e*(1-ci*ci);
        float g=e*ci-tt*rsqrt(tt);
        float tx=e*ds-g*nx, ty=e*dc-g*ny;
        float sl=tx*rsqrt(ty)*rsqrt(ty);   // tx/ty; ty > 0 going down
        if(sl>2)sl=2;
        if(sl<-2)sl=-2;
        wb->slope[r]=(int16_t)(sl*1024);
        float ys=wb->surf[r]*(1.0f/16);
        int o=(int)(r-RAY_PAD-ys*sky_tan)+SHAFT_OFF;
        wb->power[r]=wb->shaft[o<0?0:o>=SHAFT_N?SHAFT_N-1:o];
        int row=(int)ys;
        if(r>=RAY_PAD&&r<RAY_PAD+W&&row<top)top=row;
    }
    surface_top=top<0?0:top;

    stars_prepare(wb->stars,elapsed,tilt_x,tilt_y,true);
}

// The light plane for row y: sky and air-lit shafts above the water; water
// colour plus the refracted light, followed ray by ray, below it.
// Adds v to pixel x of the row as a difference: the prefix sum in
// wave_light_row turns these back into values. Off-screen pixels are dropped.
static inline void beam_point(int32_t *diff, int x, int v) {
    if(x<0||x>=W)return;
    diff[x]+=v; diff[x+1]-=v;
}

static void wave_light_row(int y) {
    int32_t *beam=wb->beam;
    if(y>surface_top) {
        // The light between two neighbouring rays is a beam. Its power is
        // spread evenly over the width the beam has on this row, so a beam
        // the surface has focused to half its width is twice as bright and
        // one spread to three pixels is a third as bright -- the caustics are
        // that and nothing else. A beam narrower than a pixel lands whole on
        // the two pixels its centre falls between. Everything is written as
        // differences, so a beam of any width costs four adds, and one prefix
        // sum at the end gives the row. Units: one full-power ray on one
        // pixel is 255*256.
        memset(beam,0,sizeof wb->beam);
        int y16=y*16, prev=0, prev_x=0, prev_p=0;
        for(int r=0;r<RAYS;r++) {
            int depth=y16-wb->surf[r];
            if(depth<=0) { prev=0; continue; }
            // Where the ray is on this row, in 1/256 px.
            int xp=(r-RAY_PAD)*256+(depth*wb->slope[r]>>6), p=wb->power[r];
            if(prev) {
                int a=prev_x, b=xp;
                if(a>b) { int t=a; a=b; b=t; }
                int w=b-a, power=(p+prev_p)>>1;
                if(w<256) {
                    // Pixel i covers [i, i+1), so its centre is i+0.5: the
                    // split is about the centre, or it disagrees with the
                    // wide branch by half a pixel and the two alternate.
                    int c=((a+b)>>1)-128, i=c>>8, f=c&255;
                    beam_point(beam,i,power*(256-f));
                    beam_point(beam,i+1,power*f);
                } else if(b>0&&a<(W<<8)) {
                    int v=(power<<16)/w;   // < 2^24 / 256: no 64-bit divide
                    // Whole pixels from a's to b's, then the parts of the
                    // two end pixels the beam does not cover taken back off.
                    int ia=a>>8, fa=a&255, ib=b>>8, fb=b&255;
                    if(a<0) { ia=0; fa=0; }
                    if(b>=(W<<8)) { ib=W; fb=0; }
                    beam[ia]+=v; beam[ib]-=v;
                    beam_point(beam,ia,-(v*fa>>8));
                    beam_point(beam,ib,v*fb>>8);
                }
            }
            prev=1; prev_x=xp; prev_p=p;
        }
        int run=0;
        for(int x=0;x<W;x++) { run+=beam[x]; beam[x]=run; }
    }
    int sr,sg,sb;
    sky_row(y,&sr,&sg,&sb);
    int origin=(int)(-y*sky_tan+4096.5f)-4096+SHAFT_OFF;
    for(int x=0;x<W;x++) {
        int16_t *px=&wb->light[x>>3][0][x&7];
        int depth=(y*16-wb->surf[x+RAY_PAD]);
        int b,g,r;
        if(depth<0) {
            // Shafts in the air are faint: only the haze scatters them.
            int s=wb->shaft[origin+x];
            b=sb+s*CH(34)/256; g=sg+s*CH(26)/256; r=sr+s*CH(24)/256;
        } else {
            int d=depth>>4; if(d>H)d=H;
            // One full-power ray on one pixel reads 255 here.
            int lum=beam[x]>>8;
            b=wave_water_body[d][0]+(lum*wave_water_light[d][0]>>8);
            g=wave_water_body[d][1]+(lum*wave_water_light[d][1]>>8);
            r=wave_water_body[d][2]+(lum*wave_water_light[d][2]>>8);
        }
        px[0]=(int16_t)(b>32767?32767:b);
        px[8]=(int16_t)(g>32767?32767:g);
        px[16]=(int16_t)(r>32767?32767:r);
    }
}

uint32_t wave_draw(uint16_t *strip,int y,int height) {
    if(!strip)return 0;
    if(!wb) {
        // Without the block there is no water, but there is still a night sky.
        for(int j=0;j<height;j++) {
            uint16_t *row=strip+(size_t)j*W;
            uint16_t sky=board_rgb(3,5,22);
            for(int x=0;x<W;x++) row[x]=sky;
        }
        return 0;
    }
    uint32_t spent=0;
    for(int row_y=y;row_y<y+height;row_y++) {
        wave_light_row(row_y);
#ifdef __XTENSA__
        uint32_t c0=esp_cpu_get_cycle_count();
#endif
        wave_row_pie(strip+(size_t)(row_y-y)*W,row_y,&wb->light[0][0][0]);
#ifdef __XTENSA__
        spent+=esp_cpu_get_cycle_count()-c0;
#endif
    }
    return spent;
}

void wave_overlay(uint16_t *strip,int y,int height) {
    if(!wb)return;
    // Stars belong to the sky: one under the waterline is moved off the strip
    // rather than drawn into the water.
    star_t seen[STARS_N];
    memcpy(seen,wb->stars,sizeof seen);
    for(int i=0;i<STARS_N;i++) {
        int x=seen[i].x;
        if(x<0||x>=W||seen[i].y*16+48>=wb->surf[x+RAY_PAD])seen[i].y=-16;
    }
    stars_draw_layers(seen,strip,y,height);
}
