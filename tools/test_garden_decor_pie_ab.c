// The decor switch's per-call contract, and the row it compounds into.
//
// One frame, one row, g_garden_decor_pie=1 against 0 on the same 240 pixels,
// over four frames -- the same sweep tools/test_garden_decor.c walks at a tenth
// of its cost, and the one tools/bench_decor_cost.py used for the group-width
// measurement.
//
// What the switch promises is per call: handed the same arguments, the kernel's
// folded arithmetic and garden_decor_mix's scalar statement differ in nothing but
// green, and there by at most one step of its sixty-three -- red and blue are
// exact. tools/pie/models/garden_model.c measures the rate over the whole domain,
// tools/pie/test_kernels.py pins the assembly to the folded statement, and this
// file is the third layer: that the row hands the kernel the arguments the two
// statements were compared on. It asserts, and only asserts:
//
//   - the contract on every call the row actually makes -- the group's eight
//     input pixels with the light, shadow and d that call was handed (ab_pixel),
//   - the contract over the pixel domain -- all 65,536 values against the tuples
//     the row handed down, collected by the call wrapper and swept afterwards,
//   - not vacuously: the kernel ran (blocks > 0), the contract ran on every one
//     of those calls (lanes == blocks*8), and every pointer was 16-byte aligned.
//     That last one is the whole reason the caller may call the kernel at all:
//     EE.VLD.128 and EE.VST.128 force the low four address bits of the pointer to
//     zero (TRM 1.8.88/1.8.192), so an unaligned group is silently the neighbour's
//     eight pixels.
//
// The whole-row comparison is reported and never asserted, because the row's
// number is compounded: garden_decor_row runs the same row through up to four
// decor layers, so a one-step deviation left by one layer is re-mixed by the next
// (the green layer's gain, Mg/1024, reaches 1.25). The layer sweep below measures
// that decomposition on the same frames -- it seeds every layer at or past k
// invisible, which is what cutting the loop to k layers does -- and the row's
// number is what the four of them come to: 1 layer 1 step, 2 layers 2, 4 layers 3
// over the same window.
//
// Asserting "green <= 1 step" on the row was never a property of this switch, and
// never held: HEAD's own wiring, at 42% of mixed pixels on the kernel, fails that
// assert at 1024 frames (worst green 2 steps, 2,118 pixels) and again at 4096, so
// a row-level bound measures the horizon of the sweep rather than the mix. What
// the phase grid changes is how often the kernel is the arm that leaves the step,
// not the step.
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
// the assembly against. Renaming it here lets this file define the entry point,
// count its calls and hold each one to the contract. On the host the stand-in IS
// the folded statement, so what the lane check compares is folded against scalar
// -- which is the contract; the assembly against folded is piesim's half.
#define garden_decor_mix8 garden_decor_mix8_impl
#include "../main/scene/garden_decor_pie.c"
#undef garden_decor_mix8
#include <stdio.h>
#include <string.h>

// One channel's step in its own field (r: 31 steps, g: 63, b: 31), and the
// largest of the three scaled to 255 so it can be read against the 33 that the
// group width moves (docs/perf/pie-opt-plan.md 9).
static int field_step(int dist,int max) { return dist*255/max; }

// ---- the per-call contract ------------------------------------------------
struct ab_acc {
    unsigned long long lanes,green,violations;
    int worst_r,worst_g,worst_b,worst_step;
    int bad;                             // the first pixel that broke it
    uint16_t bad_p,bad_scalar,bad_folded;
    int bad_light,bad_shadow,bad_d;
};
// The two statements on one argument set. Two C functions and not one behind a
// flag, so this cannot be satisfied by construction: the fold either holds or the
// run fails, and a red or blue pixel that moves means the kernel was handed the
// wrong pointer, the wrong group or the wrong constants.
static void ab_pixel(struct ab_acc *a,uint16_t p,int light,int shadow,int d) {
    uint16_t scalar=garden_decor_mix(p,light,shadow,d);
    uint16_t folded=garden_decor_mix_folded(p,light,shadow,d);
    int dr=abs((int)((scalar>>11)&31)-(int)((folded>>11)&31));
    int dg=abs((int)((scalar>>5)&63)-(int)((folded>>5)&63));
    int db=abs((int)(scalar&31)-(int)(folded&31));
    a->lanes++;
    if(dg)a->green++;
    if(dr>a->worst_r)a->worst_r=dr;
    if(dg>a->worst_g)a->worst_g=dg;
    if(db>a->worst_b)a->worst_b=db;
    int s=field_step(dr,31),g2=field_step(dg,63),s2=field_step(db,31);
    if(g2>s)s=g2;
    if(s2>s)s=s2;
    if(s>a->worst_step)a->worst_step=s;
    if((dr||db||dg>1)&&!a->bad) {
        a->bad=1;a->bad_p=p;a->bad_scalar=scalar;a->bad_folded=folded;
        a->bad_light=light;a->bad_shadow=shadow;a->bad_d=d;
    }
    if(dr||db||dg>1)a->violations++;
}
// The tuples the row hands down: light and shadow are eight bits of profile times
// gain (both <= 254), d is the dither times 64 plus 32, so a triple fits in the
// key below. A set, not a list, because the sweep wants each of them once.
#define AB_TUPLES (1<<14)
static uint32_t ab_tuple_slot[AB_TUPLES];    // key+1, 0 is empty
static uint32_t ab_tuple_key[AB_TUPLES];     // insertion order, for the sweep
static int ab_tuple_n,ab_tuple_overflow;
static void ab_tuple_note(int light,int shadow,int d) {
    uint32_t k=((uint32_t)light<<24)|((uint32_t)shadow<<16)|(uint32_t)d;
    uint32_t h=(k*2654435761u)>>(32-14);
    for(int i=0;i<AB_TUPLES;i++,h=(h+1u)&(AB_TUPLES-1)) {
        if(!ab_tuple_slot[h]) {
            ab_tuple_slot[h]=k+1u;
            if(ab_tuple_n<AB_TUPLES)ab_tuple_key[ab_tuple_n++]=k;
            else ab_tuple_overflow++;
            return;
        }
        if(ab_tuple_slot[h]==k+1u)return;
    }
    ab_tuple_overflow++;
}
// Every kernel call goes through here. The counting is the non-vacuity check the
// old assert was reaching for -- a buffer that is not aligned would make both arms
// scalar and the row would compare clean while the kernel never ran at all.
static unsigned long long ab_blocks,ab_unaligned;
static int ab_contract;                      // checked on the shipped arm only
static struct ab_acc ab_call;
void garden_decor_mix8(uint16_t *row,int light,int shadow,int d) {
    uint16_t in[8];
    memcpy(in,row,sizeof in);                // the group's inputs, before the kernel writes
    if((uintptr_t)row&15u)ab_unaligned++;
    ab_blocks++;
    garden_decor_mix8_impl(row,light,shadow,d);
    if(!ab_contract)return;
    ab_tuple_note(light,shadow,d);
    for(int i=0;i<8;i++)ab_pixel(&ab_call,in[i],light,shadow,d);
}
// All 65,536 pixel values against a spread of the tuples above, so the contract is
// not only checked on the pixel values that happened to appear in the sweep.
#define AB_SWEEP_TUPLES 256
static void ab_sweep(struct ab_acc *a,int *swept,int *tuples) {
    int n=ab_tuple_n;if(!n)return;
    int stride=n/AB_SWEEP_TUPLES;if(stride<1)stride=1;
    for(int i=0;i<n;i+=stride) {
        uint32_t k=ab_tuple_key[i];
        int light=(int)(k>>24),shadow=(int)((k>>16)&255),d=(int)(k&0xffff);
        for(int p=0;p<65536;p++)ab_pixel(a,(uint16_t)p,light,shadow,d);
        (*swept)++;
    }
    if(n>1&&(n-1)%stride) {                  // and the last one, so the tail is covered too
        uint32_t k=ab_tuple_key[n-1];
        int light=(int)(k>>24),shadow=(int)((k>>16)&255),d=(int)(k&0xffff);
        for(int p=0;p<65536;p++)ab_pixel(a,(uint16_t)p,light,shadow,d);
        (*swept)++;
    }
    *tuples=n;
}
// ---- the row, as a compounded number --------------------------------------
struct ab_row {
    unsigned long long pixels,differing,green,blocks,kernel_pixels;
    int worst_r,worst_g,worst_b,worst_step;
};
static void ab_row_compare(struct ab_row *s,const uint16_t *kernel,const uint16_t *scalar,int n) {
    for(int x=0;x<n;x++) {
        s->pixels++;
        if(kernel[x]==scalar[x])continue;
        s->differing++;
        int dr=abs((int)((kernel[x]>>11)&31)-(int)((scalar[x]>>11)&31));
        int dg=abs((int)((kernel[x]>>5)&63)-(int)((scalar[x]>>5)&63));
        int db=abs((int)(kernel[x]&31)-(int)(scalar[x]&31));
        if(dr>s->worst_r)s->worst_r=dr;
        if(dg>s->worst_g)s->worst_g=dg;
        if(db>s->worst_b)s->worst_b=db;
        if(dg)s->green++;
        int t=field_step(dr,31),g2=field_step(dg,63),b2=field_step(db,31);
        if(g2>t)t=g2;
        if(b2>t)t=b2;
        if(t>s->worst_step)s->worst_step=t;
    }
}
// The layer sweep's knob, and the reason it is a knob on the frame rather than a
// second copy of the loop: garden_decor_row reads a layer's opening through
// f->decor_seed[layer] whenever f->decor_ready is set, and seed 0 has (h&12)==0,
// which garden_decor_seeded returns with fade 0 -- so the row's own
// `if(!decor.fade)continue` skips that layer exactly as cutting the loop would.
// The layers that stay are seeded with the value garden_decor would have derived
// from the phase, so the four-layer run here is the same row as before the knob
// existed (and the row number below says so: it matches the pre-knob run).
static GardenFrame ab_frame_layers(const GardenFrame *f,int layers) {
    GardenFrame g=*f;
    g.decor_ready=true;
    for(int slot=0;slot<4;slot++) {
        unsigned p=((unsigned)g.phase+(unsigned)slot*4317u)&65535u;
        g.decor_seed[slot]=slot<layers
            ?garden_hash((p>>14)+1709u+(unsigned)slot*313u)
            :0u;
    }
    return g;
}

int main(int argc,char **argv) {
    static uint16_t base[240] __attribute__((aligned(16)));
    static uint16_t kernel_arm[240] __attribute__((aligned(16)));
    static uint16_t scalar_arm[240] __attribute__((aligned(16)));
    int frames=argc>1?atoi(argv[1]):4;
    struct ab_row rs[5];
    memset(rs,0,sizeof rs);
    for(int frame=0;frame<frames;frame++) {
        GardenFrame f={0};
        garden_prepare(&f,frame*1.25f);
        for(int y=0;y<135;y++) {
            garden_pixels_row(base,y,&f);
            // The same row at one, two, three and four layers. The four-layer pass
            // is the switch as it ships, so it is the one the contract is counted
            // on; the others are the decomposition of its number.
            for(int layers=1;layers<=4;layers++) {
                GardenFrame g=ab_frame_layers(&f,layers);
                memcpy(kernel_arm,base,sizeof base);
                memcpy(scalar_arm,base,sizeof base);
                ab_blocks=0;ab_contract=(layers==4);
                g_garden_decor_pie=1;garden_decor_row(kernel_arm,y,&g);
                unsigned long long blocks=ab_blocks;
                g_garden_decor_pie=0;garden_decor_row(scalar_arm,y,&g);
                ab_contract=0;
                ab_row_compare(&rs[layers],kernel_arm,scalar_arm,240);
                rs[layers].blocks+=blocks;
                rs[layers].kernel_pixels+=blocks*8;
            }
        }
    }
    struct ab_acc ab_sweep_acc;
    memset(&ab_sweep_acc,0,sizeof ab_sweep_acc);
    int swept=0,tuples=0;
    ab_sweep(&ab_sweep_acc,&swept,&tuples);

    printf("DECOR_PIE_AB_CALL frames=%d blocks=%llu lanes=%llu green_moved=%llu worst_r/g/b=%d/%d/%d "
           "worst_step=%d(0..255) unaligned=%llu violations=%llu\n",
           frames,rs[4].blocks,ab_call.lanes,ab_call.green,ab_call.worst_r,ab_call.worst_g,ab_call.worst_b,
           ab_call.worst_step,ab_unaligned,ab_call.violations);
    printf("DECOR_PIE_AB_SWEEP tuples=%d of %d distinct swept=%d compared=%llu green_moved=%llu "
           "worst_r/g/b=%d/%d/%d worst_step=%d(0..255) violations=%llu\n",
           tuples,tuples+ab_tuple_overflow,swept,ab_sweep_acc.lanes,ab_sweep_acc.green,ab_sweep_acc.worst_r,
           ab_sweep_acc.worst_g,ab_sweep_acc.worst_b,ab_sweep_acc.worst_step,ab_sweep_acc.violations);
    // The row: reported, and compounded -- see the header.
    printf("DECOR_PIE_AB_ROW  frames=%d compared=%llu differing=%llu (%.3f%%) worst_step=%d(0..255) "
           "worst_r/g/b=%d/%d/%d(green_moved=%llu, %.2f per 129,600px) kernel_blocks=%llu kernel_pixels=%llu\n",
           frames,rs[4].pixels,rs[4].differing,100.0*rs[4].differing/rs[4].pixels,rs[4].worst_step,
           rs[4].worst_r,rs[4].worst_g,rs[4].worst_b,rs[4].green,
           rs[4].green*129600.0/rs[4].pixels,rs[4].blocks,rs[4].kernel_pixels);
    // The same comparison at 1..4 layers, which is what the row's number is made
    // of: the layers after the one that left the step re-mix it.
    printf("DECOR_PIE_AB_LAYERS");
    for(int layers=1;layers<=4;layers++)
        printf(" %d:diff=%llu,worst_g=%d,worst_step=%d",layers,rs[layers].differing,rs[layers].worst_g,
               rs[layers].worst_step);
    printf("\n");
    int ok=!ab_call.violations&&!ab_sweep_acc.violations&&!ab_unaligned&&rs[4].blocks>0
        &&ab_call.lanes==rs[4].blocks*8&&ab_sweep_acc.lanes>0;
    if(!ok) {
        if(ab_call.bad)printf("DECOR_PIE_AB_BAD p=%u light=%d shadow=%d d=%d scalar=%04x folded=%04x\n",
                              ab_call.bad_p,ab_call.bad_light,ab_call.bad_shadow,ab_call.bad_d,
                              ab_call.bad_scalar,ab_call.bad_folded);
        if(ab_sweep_acc.bad)printf("DECOR_PIE_AB_BAD_SWEEP p=%u light=%d shadow=%d d=%d scalar=%04x folded=%04x\n",
                                   ab_sweep_acc.bad_p,ab_sweep_acc.bad_light,ab_sweep_acc.bad_shadow,
                                   ab_sweep_acc.bad_d,ab_sweep_acc.bad_scalar,ab_sweep_acc.bad_folded);
    }
    printf(ok?"DECOR_PIE_AB_OK the per-call contract holds (red/blue exact, green <= 1 of 63) on every call "
              "and over the pixel domain; the row's number above is compounded and reported\n"
            :"DECOR_PIE_AB_FAIL the two statements disagree by more than the fold, the kernel was handed an "
             "unaligned pointer, or the kernel never ran\n");
    return !ok;
}
