/* Boundary 4 of docs/perf/kasane-opt-survey.md, the quantized-key table of
 * ksn_render.c ("Boundary 4"): measured, not asserted.
 *
 * ksn_render.c is included for its statics -- `blend`, the table builder, the
 * table itself and the pack are deliberately not exported, and comparing the
 * table against the reference chain is the whole point here. ksn_core.c and
 * ksn_cache.c are linked as usual; do NOT also compile ksn_render.c into this
 * binary.
 *
 * Four proofs, all host-only:
 *   1. the solid arm, exhaustively: every effective alpha 0..255 x every source
 *      channel value 0..255 x the thin pack and the sixteen bayer thresholds x
 *      every 32/64 destination channel value, plus the whole 65,536-word
 *      destination space over the command-level parameters the API can produce
 *      (so the a == mul8(colour alpha, opacity) factorization and the row cache
 *      are in the comparison, not just the per-channel expression);
 *   2. the alpha arm's 16-level parameter quantization, exhaustively per
 *      channel: every (a, source channel, destination channel) triple against
 *      the row the arm actually indexes, printing the moved-pixel count and the
 *      worst channel step instead of bounding them;
 *   3. 120 frames of a demo.js-shaped scene (translucent rect, round rect,
 *      stroke, two texts, a dithered gradient whose ends differ -- the reference
 *      chain -- a dithered gradient whose ends are equal -- the table's dither
 *      rows -- and an opaque rect on the fill path), rendered once per arm and
 *      compared per frame as FNV-1a hashes of the 240x135 panel, with the moved
 *      pixels and the worst channel step counted per frame;
 *   4. built with -DKSN_COUNT_LUT it counts entries into `blend`,
 *      `blend_lut_pack` and the row builder through -finstrument-functions, so
 *      "pixels taken by the table" and "rows rebuilt" are counted call sites.
 *
 * Time is not measured here and is not claimed: the host has no rsr.ccount
 * (docs/perf/pie-simd.md 6.7), so the per-pixel cost is an -Os objdump count and
 * the coverage is an entry count. The instruction counts are in
 * docs/perf/kasane-lut.md with the commands that produced them. */
#include "core_fixture.h"
#include "ksn_cache.h"
#include "ksn_render.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef KSN_COUNT_LUT
#include <inttypes.h>
#endif

#define CHECK(x) do{if(!(x)){fprintf(stderr,"blend lut line %d: %s\n",__LINE__,#x);return 1;}}while(0)

/* The bayer cell holding a given threshold: bayer4 is a permutation of 0..15,
 * so every value has exactly one home, and asking blend() at that pixel is
 * asking it about the row this table row was built for. */
static int bayer_spot(unsigned threshold,int *x,int *y){
    for(unsigned row=0;row<4;row++)for(unsigned column=0;column<4;column++)
        if(bayer4[row][column]==threshold){*x=(int)column;*y=(int)row;return 1;}
    return 0;
}
static const unsigned channel_shifts[3]={11,5,0},channel_masks[3]={31,63,31};
static const unsigned channel_values[3]={32,64,32};
static unsigned compared_words,moved_pixels,worst_step;
static unsigned stepped[3];
/* One whole word of the arm against one whole word of the reference chain. */
static void compare_word(uint16_t dst,ksn_rgba color,unsigned opacity,bool dither,
                         const uint8_t *row,unsigned threshold){
    int x=0,y=0;
    if(!bayer_spot(threshold,&x,&y))return;
    uint16_t want=blend(dst,color,opacity,dither,x,y);
    uint16_t got=blend_lut_pack(dst,row);
    compared_words++;
    if(want!=got)moved_pixels++;
    for(unsigned channel=0;channel<3;channel++){
        unsigned a=(want>>channel_shifts[channel])&channel_masks[channel];
        unsigned b=(got>>channel_shifts[channel])&channel_masks[channel];
        if(a!=b){
            stepped[channel]++;
            unsigned step=a>b?a-b:b-a;
            if(step>worst_step)worst_step=step;
        }
    }
}

/* ---- 1. the solid arm, exhaustively -------------------------------------- */
static unsigned solid_parameters;
/* Every (effective alpha, source channel value, threshold) the arm can be built
 * for, against every destination value of each channel. a == 0 is excluded on
 * the dithered rows: blend() returns the destination there and the caller keeps
 * the reference chain (blend_lut_solid_prime), so those pixels are not in the
 * arm -- the guard is checked directly in solid_words_over_commands(). */
static int solid_exhaustive(void){
    for(unsigned alpha=0;alpha<256;alpha++)
    for(unsigned value=0;value<256;value++){
        ksn_rgba color=(value<<24)|(value<<16)|(value<<8)|alpha;
        for(unsigned threshold=0;threshold<17;threshold++){
            bool dither=threshold!=0;
            unsigned use=dither?threshold-1:0;
            if(dither&&!alpha)continue;
            uint8_t row[KSN_BLEND_LUT_ROW];
            blend_lut_build_row(row,color,255,use,dither);
            int x=0,y=0;
            if(!bayer_spot(use,&x,&y))return 1;
            for(unsigned channel=0;channel<3;channel++)
            for(unsigned index=0;index<channel_values[channel];index++){
                uint16_t dst=(uint16_t)(index<<channel_shifts[channel]);
                uint16_t want=blend(dst,color,255,dither,x,y);
                uint16_t got=blend_lut_pack(dst,row);
                unsigned a=(want>>channel_shifts[channel])&channel_masks[channel];
                unsigned b=(got>>channel_shifts[channel])&channel_masks[channel];
                compared_words++;
                if(a!=b){
                    moved_pixels++;
                    unsigned step=a>b?a-b:b-a;
                    if(step>worst_step)worst_step=step;
                    if(moved_pixels<4)
                        fprintf(stderr,"solid: alpha %u value %u threshold %u channel %u "
                                       "index %u want %u got %u\n",
                                alpha,value,use,channel,index,a,b);
                }
            }
            solid_parameters++;
        }
    }
    return 0;
}
/* The whole destination word, over the command-level parameters the API hands
 * the renderer: the table must agree for every 16-bit word a strip can hold. */
static unsigned solid_words,solid_rows;
static int solid_words_over_commands(void){
    static const unsigned opacities[]={1,127,200,255};
    static const unsigned alphas[]={1,127,254,255};
    static const unsigned values[]={0,64,128,192,255};
    for(unsigned o=0;o<sizeof(opacities)/sizeof(opacities[0]);o++)
    for(unsigned a=0;a<sizeof(alphas)/sizeof(alphas[0]);a++)
    for(unsigned v=0;v<sizeof(values)/sizeof(values[0]);v++){
        ksn_rgba color=(values[v]<<24)|(values[v]<<16)|(values[v]<<8)|alphas[a];
        for(unsigned threshold=0;threshold<17;threshold++){
            bool dither=threshold!=0;
            unsigned use=dither?threshold-1:0;
            if(!blend_lut_solid_prime(color,(uint8_t)opacities[o],dither))continue;
            for(unsigned dst=0;dst<65536;dst++){
                compare_word((uint16_t)dst,color,(uint8_t)opacities[o],dither,
                             blend_lut_solid[use],use);
                solid_words++;
            }
            solid_rows++;
        }
    }
    /* The guard that keeps a == 0 pixels on the reference chain: blend() returns
     * the destination there, and a dithered row would not. */
    CHECK(!blend_lut_solid_prime(0x12345600u,255,true));
    CHECK(!blend_lut_solid_prime(0x123456ffu,0,false));
    CHECK(blend_lut_solid_prime(0x123456ffu,255,false));
    /* The row cache must not serve a row built for another colour: priming with
     * a second colour must overwrite the first. */
    CHECK(blend_lut_solid_prime(0x102030ffu,200,false));
    CHECK(blend_lut_solid_prime(0x90807040u,200,false));
    uint8_t row[KSN_BLEND_LUT_ROW];
    blend_lut_build_row(row,0x90807040u,200,0,false);
    CHECK(!memcmp(row,blend_lut_solid[0],KSN_BLEND_LUT_ROW));
    return 0;
}

/* ---- 2. the alpha arm's 16-level parameter ------------------------------- */
static unsigned alpha_compared,alpha_moved,alpha_worst,alpha_stepped[3];
static int alpha_exhaustive(void){
    for(unsigned level=0;level<KSN_BLEND_LUT_ROWS;level++){
        unsigned representative=level*16u+8u;
        for(unsigned value=0;value<256;value++){
            ksn_rgba color=(value<<24)|(value<<16)|(value<<8)|representative;
            if(!blend_lut_alpha_prime(color,255))return 1;
            const uint8_t *row=blend_lut_alpha[level];
            for(unsigned a=level*16u;a<level*16u+16u&&a<256;a++){
                if(!a)continue; /* the arm skips a == 0 (blend returns dst) */
                for(unsigned channel=0;channel<3;channel++)
                for(unsigned index=0;index<channel_values[channel];index++){
                    ksn_rgba exact=(value<<24)|(value<<16)|(value<<8)|a;
                    uint16_t dst=(uint16_t)(index<<channel_shifts[channel]);
                    uint16_t want=blend(dst,exact,255,false,0,0);
                    uint16_t got=blend_lut_pack(dst,row);
                    unsigned r=(want>>channel_shifts[channel])&channel_masks[channel];
                    unsigned g=(got>>channel_shifts[channel])&channel_masks[channel];
                    alpha_compared++;
                    if(r!=g){
                        alpha_moved++;
                        unsigned step=r>g?r-g:g-r;
                        if(step>alpha_worst)alpha_worst=step;
                        alpha_stepped[channel]++;
                    }
                }
            }
        }
    }
    /* A level's row is a function of the RGB and the bucket midpoint only: the
     * command's own alpha/opacity pair picks the level, it does not change the
     * row. */
    unsigned char seen[KSN_BLEND_LUT_ROWS]={0};
    for(unsigned coverage=0;coverage<256;coverage++){
        unsigned a=mul8(mul8(255,coverage),255);
        if(a)seen[a>>4]=1;
    }
    unsigned reachable=0;
    for(unsigned level=0;level<KSN_BLEND_LUT_ROWS;level++)if(seen[level])reachable++;
    CHECK(reachable==KSN_BLEND_LUT_ROWS);
    CHECK(blend_lut_alpha_prime(0x33669980u,200));
    CHECK(blend_lut_alpha_prime(0x336699f0u,130));
    CHECK(blend_lut_alpha_prime(0x336699f0u,130));
    CHECK(!blend_lut_alpha_prime(0x33669900u,255)); /* alpha 0: nothing to draw */
    return 0;
}
/* The coverage path's own arithmetic: the effective alpha the arm buckets is
 * mul8(mul8(colour alpha, coverage), opacity), the same one blend() computes,
 * so the quantization error is bounded by half a bucket and nothing else. */
static int alpha_coverage_path(void){
    unsigned checked=0,worst=0;
    for(unsigned alpha=0;alpha<256;alpha+=7)for(unsigned opacity=1;opacity<256;opacity+=7)
    for(unsigned coverage=0;coverage<256;coverage+=7){
        unsigned a=mul8(mul8(alpha,coverage),opacity);
        unsigned reference=mul8((uint8_t)((alpha*coverage+127u)/255u),opacity);
        if(a!=reference)return 1;
        if(a){
            unsigned level=a>>4,midpoint=level*16u+8u;
            unsigned step=a>midpoint?a-midpoint:midpoint-a;
            if(step>worst)worst=step;
            if(level>=KSN_BLEND_LUT_ROWS)return 1;
        }
        checked++;
    }
    CHECK(checked>50000&&worst<=8);
    return 0;
}

/* ---- 3. 120 frames, one arm each ----------------------------------------- */
#define FRAMES 120
#define SCENE 9
#define CHILDREN 2
static uint16_t strip[240*8],panel[240*135];
static uint16_t *reference_panel; /* FRAMES x 240 x 135 words. */
static uint32_t frame_hash[3][FRAMES];

static uint64_t arm_hash[3];
static unsigned full_frames;
KSN_TEST_CORE(core,static);
static ksn_cache cache;
static ksn_cache_command_block cache_commands;
static ksn_cache_text_block cache_text;
static uint16_t *get_strip(void *ctx){(void)ctx;return strip;}
static ksn_result send_strip(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));return KSN_OK;
}
/* Synthetic face: coverage depends only on x, y and reveal, so every arm sees
 * exactly the same ink and the counts are comparable across arms. It is denser
 * than a real face, which is stated where the numbers are used: the device's
 * jpfont ink decides how many pixels reach the alpha arm, this decides only
 * whether the code path is correct. */
static uint8_t ink(int x,int y,unsigned reveal){
    static const uint8_t values[]={0,1,9,63,127,128,196,254,255};
    if(x<0)return 0;
    if(((unsigned)(x+3*y)&7u)>(reveal%11u))return 0;
    return values[(unsigned)(x*3+y)%(sizeof(values)/sizeof(values[0]))];
}
static ksn_result span(void *ctx,const ksn_draw *draw,uint16_t reveal,int x,int y,
                       unsigned count,uint8_t *out){
    (void)ctx;(void)draw;
    for(unsigned i=0;i<count;i++)out[i]=ink(x+(int)i,y,reveal);
    return KSN_OK;
}
static const ksn_text_port text_port={.span=span};
static const ksn_display_port display={NULL,get_strip,send_strip,240,135,8,&text_port};
static const uint32_t background=0x071425ff;
static const char title_text[]="Kasane 日本語";
static char counter_text[32]="tick 0";
static ksn_draw scene[SCENE];
static const ksn_draw scene_base[SCENE]={
    /* 0: translucent RECT (also the group's first child): the solid thin row. */
    {.kind=KSN_RECT,.bounds={12,28,112,56},.clip={0,0,240,135},.opacity=216,
     .data.shape={0x164c70d8,0,0}},
    /* 1: ROUND_RECT, radius 6: the solid arm on an arc. */
    {.kind=KSN_ROUND_RECT,.bounds={16,76,36,96},.clip={0,0,240,135},.opacity=210,
     .data.shape={0xf5bd4fff,6,0}},
    /* 2: STROKE, one pixel: two runs a row. */
    {.kind=KSN_STROKE,.bounds={10,117,232,129},.clip={0,0,240,135},.opacity=170,
     .data.shape={0x80b5cfaa,0,1}},
    /* 3: TEXT with a per-pixel coverage alpha: the alpha arm. */
    {.kind=KSN_TEXT,.bounds={8,3,232,17},.clip={0,0,240,135},.opacity=255,
     .data.text={.utf8=title_text,.bytes=sizeof(title_text)-1,.capacity=64,
                 .font=KSN_BODY,.color=0xffffffff}},
    /* 4: dithered horizontal gradient, ends differ: reference chain, because its
     * colour is a function of x and no row of the table can carry it. */
    {.kind=KSN_GRADIENT,.bounds={0,0,240,18},.clip={0,0,240,135},.opacity=255,
     .data.gradient={0x0d2940ff,0x498781ff,0,0,true}},
    /* 5: dithered gradient whose ends are equal -- interpolate() returns `from`
     * for every i -- so the table's sixteen dither rows are live here. */
    {.kind=KSN_GRADIENT,.bounds={120,20,220,44},.clip={0,0,240,135},.opacity=190,
     .data.gradient={0x3060a0c0,0x3060a0c0,1,0,true}},
    /* 6: opaque RECT: the fill path, which the table must not touch. */
    {.kind=KSN_RECT,.bounds={146,3,236,60},.clip={0,0,240,135},.opacity=255,
     .data.shape={0x9b6c2aff,0,0}},
    /* 7: opaque opacity, translucent colour: still the direct path. */
    {.kind=KSN_ROUND_RECT,.bounds={150,70,238,132},.clip={0,0,240,135},.opacity=255,
     .data.shape={0x4080c055,8,0}},
    /* 8: a second text, different colour and opacity. */
    {.kind=KSN_TEXT,.bounds={12,100,180,113},.clip={0,0,240,135},.opacity=177,
     .data.text={.utf8=counter_text,.bytes=6,.capacity=24,.font=KSN_CAPTION,
                 .color=0xa8d8efff}}
};
/* Two rects, as apps/kasane/demo.js caches: an instance is isolated
 * premultiplied composition, so its children render through render_group, where
 * there is no one colour a row could be built for. */
static const ksn_draw tile_draws[CHILDREN]={
    {.kind=KSN_RECT,.bounds={0,0,42,24},.clip={0,0,240,135},.opacity=255,
     .data.shape={0x185071ff,0,0}},
    {.kind=KSN_RECT,.bounds={4,4,38,20},.clip={0,0,240,135},.opacity=220,
     .data.shape={0x63d7bccc,0,0}}
};
static uint32_t hash_panel(void){
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<240*135;i++){hash^=panel[i];hash*=16777619u;}
    return hash;
}
/* What an arm did to the pixels, against the reference arm's panel for the same
 * frame: how many it moved, the worst channel step, that step per channel, and
 * how many moved pixels fell in each step bucket. */
typedef struct { unsigned moved,worst,worst_channel[3],buckets[4]; } arm_diff;
static arm_diff frame_diff[3][FRAMES];
static void against_reference(unsigned frame,arm_diff *diff){
    const uint16_t *want=reference_panel+(size_t)frame*240u*135u;
    memset(diff,0,sizeof(*diff));
    for(unsigned i=0;i<240*135;i++)if(panel[i]!=want[i]){
        unsigned pixel_worst=0;
        diff->moved++;
        for(unsigned channel=0;channel<3;channel++){
            unsigned a=(want[i]>>channel_shifts[channel])&channel_masks[channel];
            unsigned b=(panel[i]>>channel_shifts[channel])&channel_masks[channel];
            unsigned step=a>b?a-b:b-a;
            if(step>diff->worst_channel[channel])diff->worst_channel[channel]=step;
            if(step>pixel_worst)pixel_worst=step;
        }
        if(pixel_worst>diff->worst)diff->worst=pixel_worst;
        diff->buckets[pixel_worst>3?3:pixel_worst]++;
    }
}
#ifdef KSN_COUNT_LUT
static uint64_t entries[3],arm_entries[3][3],frame0_entries[3][3];
#endif
static int run_arm(unsigned arm){
    g_ksn_blend_lut=arm>=1;
    g_ksn_blend_lut_alpha=arm>=2;
    g_ksn_row_coverage=1;
    ksn_core_init(&core);ksn_cache_init(&cache);
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx tx;ksn_ref refs[SCENE];
    ksn_template tile;ksn_instance left,right;
    ksn_render_stats stats;
    memcpy(scene,scene_base,sizeof(scene));
    strcpy(counter_text,"tick 0");
    arm_hash[arm]=0;
    CHECK(ksn_cache_create(&cache,KSN_APP,tile_draws,CHILDREN,&tile)==KSN_OK);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,background)==KSN_OK);
    for(unsigned i=0;i<SCENE;i++)CHECK(app.ops->add(app.ctx,tx,&scene[i],&refs[i])==KSN_OK);
    CHECK(ksn_core_group(&core,KSN_APP,tx,refs[0],2,208)==KSN_OK);
    ksn_placement place_left={142,28,{0,0,240,135},230,true};
    ksn_placement place_right={188,72,{0,0,240,135},175,true};
    CHECK(ksn_cache_instantiate(&cache,&core,tx,tile,&place_left,&left)==KSN_OK);
    CHECK(ksn_cache_instantiate(&cache,&core,tx,tile,&place_right,&right)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    for(unsigned tick=0;tick<FRAMES;tick++){
        if(tick){
            CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
            int x=12+(int)(tick*3%184),width=12+(int)(tick*5%208);
            ksn_change change={.property=KSN_SET_RECT,.value.rect={x,76,x+20,96}};
            CHECK(app.ops->change(app.ctx,tx,refs[1],&change)==KSN_OK);
            change=(ksn_change){.property=KSN_SET_RECT,.value.rect={12,119,12+width,127}};
            CHECK(app.ops->change(app.ctx,tx,refs[2],&change)==KSN_OK);
            change=(ksn_change){.property=KSN_SET_COLOR,
                                .value.color=(tick&16)?0x295d86d8u:0x164c70d8u};
            CHECK(app.ops->change(app.ctx,tx,refs[0],&change)==KSN_OK);
            change=(ksn_change){.property=KSN_SET_RECT,
                                .value.rect={100,20,100+(int)(tick%120),44}};
            CHECK(app.ops->change(app.ctx,tx,refs[5],&change)==KSN_OK);
            place_left.y=28+(int)(tick%18);
            CHECK(ksn_cache_place(&cache,&core,tx,left,&place_left)==KSN_OK);
            CHECK(ksn_cache_set_visible(&cache,&core,tx,right,(tick%40)<31)==KSN_OK);
            change=(ksn_change){.property=KSN_SET_REVEAL,.value.reveal=(uint16_t)(tick/6%11)};
            CHECK(app.ops->change(app.ctx,tx,refs[3],&change)==KSN_OK);
            snprintf(counter_text,sizeof(counter_text),"tick %u",tick);
            change=(ksn_change){.property=KSN_SET_TEXT,
                                .value.text={counter_text,(uint16_t)strlen(counter_text)}};
            CHECK(app.ops->change(app.ctx,tx,refs[8],&change)==KSN_OK);
            CHECK(ksn_core_group(&core,KSN_APP,tx,refs[0],2,(uint8_t)(128+tick%128))==KSN_OK);
            CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
            if(tick%17==0)ksn_core_invalidate(&core);
        }
        ksn_frame frame;
        CHECK(ksn_core_frame(&core,&frame)==KSN_OK);
#ifdef KSN_COUNT_LUT
        uint64_t before[3]={entries[0],entries[1],entries[2]};
#endif
        CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
#ifdef KSN_COUNT_LUT
        for(unsigned e=0;e<3;e++){
            uint64_t delta=entries[e]-before[e];
            arm_entries[arm][e]+=delta;
            if(!tick)frame0_entries[arm][e]=delta;
        }
#endif
        CHECK(ksn_cache_resolve(&cache,&core,frame.ticket,true)==KSN_OK);
        if(!tick||tick%17==0){ /* invalidated frames must repaint everything */
            CHECK(stats.transferred_bytes==64800);
            full_frames++;
        }
        if(arm==0)memcpy(reference_panel+(size_t)tick*240u*135u,panel,sizeof(panel));
        else against_reference(tick,&frame_diff[arm][tick]);
        frame_hash[arm][tick]=hash_panel();
        arm_hash[arm]=arm_hash[arm]*31u+frame_hash[arm][tick];
    }
    return 0;
}

#ifdef KSN_COUNT_LUT
/* -finstrument-functions hands over the callee's own address, so the counters
 * cannot drift from the code they claim to measure. */
void __cyg_profile_func_enter(void *,void *) __attribute__((no_instrument_function));
void __cyg_profile_func_enter(void *this_fn,void *call_site);
void __cyg_profile_func_enter(void *this_fn,void *call_site){
    (void)call_site;
    if(this_fn==(void *)&blend)entries[0]++;
    else if(this_fn==(void *)&blend_lut_pack)entries[1]++;
    else if(this_fn==(void *)&blend_lut_build_row)entries[2]++;
}
#endif

int main(void){
    reference_panel=malloc((size_t)FRAMES*240u*135u*sizeof(uint16_t));
    CHECK(reference_panel!=NULL);
    CHECK(ksn_cache_bind(&cache,&cache_commands,&cache_text)==KSN_OK);
    CHECK(solid_exhaustive()==0);
    CHECK(solid_words_over_commands()==0);
    printf("blend lut: solid arm, %u per-channel comparisons over %u (alpha, value, "
           "threshold) parameters, %u whole words over %u command parameter rows "
           "(a == 0 rows stay on the reference chain): %u moved, worst channel step %u\n",
           compared_words,solid_parameters,solid_words,solid_rows,moved_pixels,worst_step);
    CHECK(moved_pixels==0&&worst_step==0); /* an identity, not a bound */
    CHECK(alpha_exhaustive()==0);
    CHECK(alpha_coverage_path()==0);
    printf("blend lut: alpha arm 16 levels, %u (a, source, destination) comparisons: "
           "%u moved (%.3f%%), worst channel step %u (red %u green %u blue %u)\n",
           alpha_compared,alpha_moved,100.0*(double)alpha_moved/(double)alpha_compared,
           alpha_worst,alpha_stepped[0],alpha_stepped[1],alpha_stepped[2]);
    CHECK(alpha_moved>0&&alpha_worst>0);
    for(unsigned arm=0;arm<3;arm++){
#ifdef KSN_COUNT_LUT
        memset(entries,0,sizeof(entries));
#endif
        CHECK(run_arm(arm)==0);
    }
    unsigned frames_ok=1,solid_moved=0;
    for(unsigned tick=0;tick<FRAMES;tick++){
        if(frame_hash[1][tick]!=frame_hash[0][tick]||frame_diff[1][tick].moved){
            fprintf(stderr,"frame %u: solid arm hash %08x, reference %08x, moved %u, step %u\n",
                    tick,frame_hash[1][tick],frame_hash[0][tick],frame_diff[1][tick].moved,
                    frame_diff[1][tick].worst);
            frames_ok=0;
        }
        solid_moved+=frame_diff[1][tick].moved;
    }
    CHECK(frames_ok&&solid_moved==0);
    unsigned text_moved=0,text_worst=0,text_frames=0,text_worst_channel[3]={0,0,0};
    unsigned text_buckets[4]={0,0,0,0};
    for(unsigned tick=0;tick<FRAMES;tick++){
        text_moved+=frame_diff[2][tick].moved;
        if(frame_diff[2][tick].worst>text_worst)text_worst=frame_diff[2][tick].worst;
        for(unsigned channel=0;channel<3;channel++)
            if(frame_diff[2][tick].worst_channel[channel]>text_worst_channel[channel])
                text_worst_channel[channel]=frame_diff[2][tick].worst_channel[channel];
        for(unsigned bucket=0;bucket<4;bucket++)text_buckets[bucket]+=frame_diff[2][tick].buckets[bucket];
        if(frame_diff[2][tick].moved)text_frames++;
    }
    CHECK(full_frames==3*(1+FRAMES/17));
    printf("blend lut: %u frames per arm, %u full 64,800-byte repaints each; the solid "
           "arm matches the reference frame for frame (rolling hash %08x)\n",
           (unsigned)FRAMES,full_frames/3,(unsigned)arm_hash[1]);
    printf("blend lut: alpha arm moved %u pixels over %u of %u frames, worst channel step %u "
           "(red %u green %u blue %u; step 1: %u, step 2: %u, step 3+: %u; rolling hash %08x vs "
           "reference %08x)\n",
           text_moved,text_frames,(unsigned)FRAMES,text_worst,
           text_worst_channel[0],text_worst_channel[1],text_worst_channel[2],
           text_buckets[1],text_buckets[2],text_buckets[3],
           (unsigned)arm_hash[2],(unsigned)arm_hash[0]);
    /* The bound the quantization promises: half a bucket (8) of effective alpha
     * is at most 8 of the 8-bit blend, so at most two of the six-bit steps. */
    CHECK(text_worst<=2&&text_worst_channel[0]<=1&&text_worst_channel[2]<=1);
    CHECK(text_buckets[0]==0&&text_buckets[3]==0);
#ifdef KSN_COUNT_LUT
    static const char *names[3]={"blend (reference chain)","blend_lut_pack (table)","rows built"};
    for(unsigned arm=0;arm<3;arm++){
        printf("blend lut counts arm %u (lut %u alpha %u), %u frames:\n",
               arm,arm>=1,arm>=2,(unsigned)FRAMES);
        for(unsigned e=0;e<3;e++)
            printf("  %-26s %" PRIu64 " entries total, %" PRIu64 " in the first (full "
                   "REPLACE) frame\n",names[e],arm_entries[arm][e],frame0_entries[arm][e]);
    }
    /* The counters must be the shapes they claim: the reference arm takes no
     * table entry at all, and each arm takes strictly fewer blend entries than
     * the one before it. */
    CHECK(arm_entries[0][1]==0&&arm_entries[0][2]==0);
    CHECK(arm_entries[1][1]>0&&arm_entries[2][1]>arm_entries[1][1]);
    CHECK(arm_entries[1][0]<arm_entries[0][0]&&arm_entries[2][0]<arm_entries[1][0]);
    CHECK(arm_entries[1][2]>0&&arm_entries[2][2]>arm_entries[1][2]);
#endif
    free(reference_panel);
    puts("blend lut PASS");
    return 0;
}
