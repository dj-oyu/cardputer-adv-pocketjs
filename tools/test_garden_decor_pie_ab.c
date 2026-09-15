// The decor switch's two arms, held to each other pixel by pixel: one frame, one
// row, g_garden_decor_pie=1 against 0 on the same 240 pixels, over four frames --
// the same sweep tools/test_garden_decor.c walks at a tenth of its cost, and the
// one tools/bench_decor_cost.py used for the group-width measurement.
//
// What the arms are allowed to differ by is the kernel's folded arithmetic and
// nothing else: red exact, green by one step of sixty-three in 0.098% of cases,
// blue exact (tools/pie/run_models.py decor; tools/pie/models/garden_model.c is
// the sweep). The asserts below are that, not a tolerance: a red or blue pixel
// that moves means the kernel was handed the wrong pointer, the wrong group or
// the wrong constants, and the run fails.
//
// The row buffers are 16-byte aligned, and the kernel only runs on groups whose
// pointer is 16-byte aligned (both its load and its store force the low four
// address bits to zero, TRM 1.8.88/1.8.192). A misaligned base would therefore
// turn this into a scalar-vs-scalar comparison, which is why the harness counts
// the kernel's calls and fails when it made none. The three buffers are separate
// so the two arms cannot see each other's stores.
//
//   gcc -std=c11 -O2 -I main/scene tools/test_garden_decor_pie_ab.c
//       tools/host_canopy_noop.c -lm -o /tmp/gdab && /tmp/gdab [frames]
//
// Four frames (129,600 pixels) is the sweep the group-width number was taken
// over; pass a frame count to average the green pixels over more of the
// animation, where a handful of pixels over four frames is a thin statistic.
#include "../main/scene/garden.c"
// The kernel's host build is the folded statement (scene/garden_decor_pie.c with
// GARDEN_DECOR_PIE=0), which is the arithmetic tools/pie/test_kernels.py checks
// the assembly against. Renaming it here lets this file define the entry point and
// count its calls.
#define garden_decor_mix8 garden_decor_mix8_impl
#include "../main/scene/garden_decor_pie.c"
#undef garden_decor_mix8
#include <stdio.h>
#include <string.h>

static unsigned long long decor_pie_blocks;
void garden_decor_mix8(uint16_t *row,int light,int shadow,int d) {
    decor_pie_blocks++;
    garden_decor_mix8_impl(row,light,shadow,d);
}

// One channel's step in its own field (r: 31 steps, g: 63, b: 31), and the
// largest of the three scaled to 255 so it can be read against the 33 that the
// group width moves (docs/perf/pie-opt-plan.md 9).
static int field_step(int dist,int max) { return dist*255/max; }

int main(int argc,char **argv) {
    static uint16_t base[240] __attribute__((aligned(16)));
    static uint16_t kernel_arm[240] __attribute__((aligned(16)));
    static uint16_t scalar_arm[240] __attribute__((aligned(16)));
    int frames=argc>1?atoi(argv[1]):4;
    unsigned long long pixels=0,differing=0,green=0;
    int worst_r=0,worst_g=0,worst_b=0,worst_step=0;
    for(int frame=0;frame<frames;frame++) {
        GardenFrame f={0};
        garden_prepare(&f,frame*1.25f);
        for(int y=0;y<135;y++) {
            garden_pixels_row(base,y,&f);
            memcpy(kernel_arm,base,sizeof base);
            memcpy(scalar_arm,base,sizeof base);
            g_garden_decor_pie=1;garden_decor_row(kernel_arm,y,&f);
            g_garden_decor_pie=0;garden_decor_row(scalar_arm,y,&f);
            for(int x=0;x<240;x++) {
                pixels++;
                if(kernel_arm[x]==scalar_arm[x])continue;
                differing++;
                int dr=abs((int)((kernel_arm[x]>>11)&31)-(int)((scalar_arm[x]>>11)&31));
                int dg=abs((int)((kernel_arm[x]>>5)&63)-(int)((scalar_arm[x]>>5)&63));
                int db=abs((int)(kernel_arm[x]&31)-(int)(scalar_arm[x]&31));
                if(dr>worst_r)worst_r=dr;
                if(dg>worst_g)worst_g=dg;
                if(db>worst_b)worst_b=db;
                if(dg)green++;
                int s=field_step(dr,31),g2=field_step(dg,63),s2=field_step(db,31);
                if(g2>s)s=g2;
                if(s2>s)s=s2;
                if(s>worst_step)worst_step=s;
            }
        }
    }
    printf("DECOR_PIE_AB frames=%d compared=%llu differing=%llu (%.3f%%) worst_step=%d(0..255) "
           "worst_r/g/b=%d/%d/%d(green_moved=%llu, %.2f per 129,600px) kernel_blocks=%llu kernel_pixels=%llu\n",
           frames,pixels,differing,100.0*differing/pixels,worst_step,worst_r,worst_g,worst_b,green,
           green*129600.0/pixels,decor_pie_blocks,decor_pie_blocks*8);
    int ok=differing>0&&decor_pie_blocks>0&&!worst_r&&!worst_b&&worst_g<=1;
    printf(ok?"DECOR_PIE_AB_OK the arms differ only by the folded arithmetic (red/blue exact, green <= 1 of 63)\n"
             :"DECOR_PIE_AB_FAIL the switch moved a channel it cannot move, or the kernel never ran\n");
    return !ok;
}
