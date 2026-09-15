/* Candidate 4b of docs/perf/kasane-opt-survey.md: coverage as per-row x runs
 * instead of one predicate call per pixel.
 *
 * ksn_render.c is included for its statics: `covers` is the reference for the
 * pixel set and is deliberately not exported, and comparing the two paths
 * directly is the point here. ksn_core.c and ksn_cache.c are linked as usual;
 * do NOT also compile ksn_render.c into this binary.
 *
 * Two proofs, both host-only:
 *   1. exhaustive: `coverage_runs` and `covers` name the same pixels over the
 *      span the primitive API allows (kind, box, offset, radius, stroke width,
 *      clip, row), including radii far wider than the box, where the two clamp
 *      windows overlap;
 *   2. whole frame: 120 frames of a demo.js-shaped scene render to identical
 *      panels with g_ksn_row_coverage off and on, compared per frame as FNV-1a
 *      hashes of the 240x135 RGB565 panel (and PATCH/full damage both hit the
 *      path, since ticks that invalidate must transfer all 64,800 bytes).
 *
 * Built with -DKSN_COUNT_CALLS it also counts entries into `covers` and into
 * `coverage_runs` through -finstrument-functions: the before/after numbers are
 * entries, not estimates. */
#include "core_fixture.h"
#include "ksn_cache.h"
#include "ksn_render.c"
#include <stdio.h>
#include <string.h>
#ifdef KSN_COUNT_CALLS
#include <inttypes.h>
static uint64_t covers_entries,run_entries,arm_covers[2],arm_runs[2];
static uint64_t frame0_covers[2],frame0_runs[2];
/* -finstrument-functions hands over the callee's own address, so the counters
 * cannot drift from the code they claim to measure. */
void __cyg_profile_func_enter(void *,void *) __attribute__((no_instrument_function));
void __cyg_profile_func_enter(void *this_fn,void *call_site);
void __cyg_profile_func_enter(void *this_fn,void *call_site){
    (void)call_site;
    if(this_fn==(void *)&covers)covers_entries++;
    else if(this_fn==(void *)&coverage_runs)run_entries++;
}
#endif

#define CHECK(x) do{if(!(x)){fprintf(stderr,"coverage runs line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static unsigned compared_rows,compared_pixels,failed_shapes,reported;

/* ---- 1. predicate and runs must name the same pixels ---------------------- */
static unsigned pixel_in_runs(const ksn_x_run *runs,unsigned count,int x){
    for(unsigned i=0;i<count;i++)if(x>=runs[i].x0&&x<runs[i].x1)return 1;
    return 0;
}
/* Compare one row inside the caller's window: the runs must stay inside it, be
 * ascending and disjoint, name no empty run, and cover exactly the pixels the
 * predicate accepts. */
static int same_row(const ksn_frame_command *command,int y,int left,int right){
    ksn_x_run runs[KSN_ROW_RUNS];
    unsigned count=coverage_runs(command,y,left,right,runs);
    if(count>KSN_ROW_RUNS)return 0;
    for(unsigned i=0;i<count;i++){
        if(runs[i].x0>=runs[i].x1)return 0;             /* empty run named */
        if(runs[i].x0<left||runs[i].x1>right)return 0;  /* outside the caller's window */
        if(i+1<count&&runs[i].x1>runs[i+1].x0)return 0; /* not ascending and disjoint */
    }
    for(int x=left;x<right;x++){
        if(!!pixel_in_runs(runs,count,x)!=!!covers(command,x,y))return 0;
        compared_pixels++;
    }
    compared_rows++;
    return 1;
}
static void report_shape(const ksn_draw *draw,int y,int left,int right){
    failed_shapes++;
    if(reported++>=8)return;
    fprintf(stderr,"runs differ: kind %d bounds %d,%d,%d,%d clip %d,%d,%d,%d radius %u width %u "
                   "row %d window [%d,%d)\n",(int)draw->kind,
            draw->bounds.x0,draw->bounds.y0,draw->bounds.x1,draw->bounds.y1,
            draw->clip.x0,draw->clip.y0,draw->clip.x1,draw->clip.y1,
            draw->kind==KSN_GRADIENT?draw->data.gradient.radius:draw->data.shape.radius,
            draw->data.shape.width,y,left,right);
}
static int same_shape(const ksn_draw *draw,int left,int right,int y0,int y1){
    ksn_frame_command command={0};
    command.visible=true;
    command.draw=*draw;
    for(int y=y0;y<y1;y++)if(!same_row(&command,y,left,right)){
        report_shape(draw,y,left,right);
        return 1;
    }
    return 0;
}
/* The predicate's first condition is `visible`, and an IMAGE command is not in
 * the supported subset (ksn_render_rects rejects it before any transfer), so
 * both must yield no runs at all. */
static int invisible_and_unsupported(void){
    ksn_frame_command command={0};
    ksn_x_run runs[KSN_ROW_RUNS];
    command.draw.kind=KSN_RECT;command.draw.bounds=(ksn_rect){0,0,240,135};
    command.draw.clip=(ksn_rect){0,0,240,135};command.draw.opacity=255;
    command.visible=false;
    CHECK(coverage_runs(&command,0,-4,244,runs)==0);
    command.visible=true;command.draw.kind=KSN_IMAGE;
    CHECK(coverage_runs(&command,0,-4,244,runs)==0);
    command.draw.kind=KSN_RECT;
    CHECK(coverage_runs(&command,0,-4,244,runs)==1);
    return 0;
}
static const ksn_rect clip_cases[]={{0,0,240,135},{-40,-30,400,300},{7,3,53,44},
                                    {0,0,0,0},{11,9,11,9},{20,20,21,21},{160,-5,240,135},
                                    {0,0,240,8},{1,1,239,134}};
#define CLIP_CASES (sizeof(clip_cases)/sizeof(clip_cases[0]))
/* Every kind over the box/offset/radius/clip space, every row of the box and
 * two pixels of margin on each side. */
static int exhaustive_sweep(void){
    const int offsets[][2]={{0,0},{-3,-2},{5,3},{-40,-30},{300,200}};
    for(unsigned o=0;o<sizeof(offsets)/sizeof(offsets[0]);o++)
    for(unsigned c=0;c<CLIP_CASES;c++){
        int bx0=offsets[o][0],by0=offsets[o][1];
        for(int w=1;w<=20;w++)for(int h=1;h<=20;h++){
            ksn_draw draw={.kind=KSN_RECT,.bounds={bx0,by0,bx0+w,by0+h},.clip=clip_cases[c],
                           .opacity=200,.data.shape={0x8f7c6bff,0,0}};
            same_shape(&draw,bx0-2,bx0+w+2,by0-2,by0+h+2);
            draw.kind=KSN_TEXT;
            same_shape(&draw,bx0-2,bx0+w+2,by0-2,by0+h+2);
            for(unsigned radius=0;radius<=8;radius++){
                draw.kind=KSN_ROUND_RECT;draw.data.shape.radius=(uint8_t)radius;
                same_shape(&draw,bx0-2,bx0+w+2,by0-2,by0+h+2);
                draw.kind=KSN_GRADIENT;draw.data.gradient.radius=(uint8_t)radius;
                draw.data.gradient.from=0x102030ff;draw.data.gradient.to=0xf0e0d0ff;
                draw.data.gradient.axis=0;draw.data.gradient.dither=false;
                same_shape(&draw,bx0-2,bx0+w+2,by0-2,by0+h+2);
            }
            for(unsigned width=0;width<=18;width++){
                draw.kind=KSN_STROKE;draw.data.shape.width=(uint8_t)width;
                same_shape(&draw,bx0-2,bx0+w+2,by0-2,by0+h+2);
            }
        }
    }
    return 0;
}
/* The API's radius is a byte, so sweep the far end of it too: 0..255 against
 * boxes narrower than 2*radius (overlapping clamp windows) and much wider. */
static int wide_radius_sweep(void){
    const int sizes[]={1,2,3,5,8,16,40};
    for(unsigned radius=0;radius<=255;radius++)
    for(unsigned s=0;s<sizeof(sizes)/sizeof(sizes[0]);s++)
    for(unsigned t=0;t<sizeof(sizes)/sizeof(sizes[0]);t++){
        ksn_draw draw={.kind=KSN_ROUND_RECT,.bounds={-7,-5,-7+sizes[s],-5+sizes[t]},
                       .clip={0,0,240,135},.opacity=200,
                       .data.shape={0x334455ff,(uint8_t)radius,0}};
        same_shape(&draw,-11,-3+sizes[s],-9,-1+sizes[t]);
        draw.kind=KSN_GRADIENT;draw.data.gradient.radius=(uint8_t)radius;
        draw.data.gradient.from=0x102030ff;draw.data.gradient.to=0xf0e0d0ff;
        same_shape(&draw,-11,-3+sizes[s],-9,-1+sizes[t]);
    }
    return 0;
}
/* Deterministic PRNG over the whole byte-radius span, wide coordinates and
 * random clipped windows, in case the sweeps above miss a combination. */
static uint32_t rng_state=0x9e3779b9u;
static unsigned next_rand(unsigned bound){
    rng_state^=rng_state<<13;rng_state^=rng_state>>17;rng_state^=rng_state<<5;
    return (unsigned)(rng_state%bound);
}
static int random_sweep(void){
    for(unsigned n=0;n<20000;n++){
        int w=(int)next_rand(60)+1,h=(int)next_rand(40)+1;
        int bx0=(int)next_rand(600)-300,by0=(int)next_rand(600)-300;
        unsigned radius=next_rand(256),kind=next_rand(4);
        ksn_rect clip={0,0,0,0};
        clip.x0=bx0-(int)next_rand(50);clip.y0=by0-(int)next_rand(50);
        clip.x1=clip.x0+(int)next_rand(300)+1;clip.y1=clip.y0+(int)next_rand(300)+1;
        ksn_draw draw={.kind=kind==0?KSN_RECT:kind==1?KSN_ROUND_RECT:kind==2?KSN_STROKE:KSN_GRADIENT,
                       .bounds={bx0,by0,bx0+w,by0+h},.clip=clip,.opacity=200,
                       .data.shape={0x556677ff,(uint8_t)radius,(uint8_t)next_rand(20)}};
        if(draw.kind==KSN_GRADIENT){
            draw.data.gradient.radius=(uint8_t)radius;
            draw.data.gradient.from=0x102030ff;draw.data.gradient.to=0xf0e0d0ff;
        }
        int y=by0-4+(int)next_rand((unsigned)h+8);
        same_shape(&draw,bx0-3,bx0+w+3,y,y+1);
    }
    return 0;
}

/* ---- 2. whole frame, switch off against switch on ------------------------- */
#define FRAMES 120
#define SCENE 8
static uint16_t strip[240*8],panel[240*135];
static uint32_t frame_hash[2][FRAMES];
static uint64_t arm_hash[2];
static unsigned full_frames;
KSN_TEST_CORE(core,static);
static ksn_cache cache;
static ksn_cache_command_block cache_commands;
static ksn_cache_text_block cache_text;
static uint16_t *get_strip(void *ctx){(void)ctx;return strip;}
static ksn_result send_strip(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));return KSN_OK;
}
/* Synthetic face: coverage depends only on x, y and reveal, so both arms see
 * exactly the same ink (the device's jpfont face is not the subject here). */
static uint8_t ink(int x,int y,unsigned reveal){
    static const uint8_t values[]={0,1,127,128,254,255};
    if(x<0)return 0;
    if(((unsigned)(x+3*y)&7u)>(reveal%11u))return 0;
    return values[(unsigned)(x*3+y)%6u];
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
    {.kind=KSN_GRADIENT,.bounds={0,0,240,18},.clip={0,0,240,135},.opacity=255,
     .data.gradient={0x0d2940ff,0x498781ff,0,0,true}},
    {.kind=KSN_TEXT,.bounds={8,3,232,17},.clip={0,0,240,135},.opacity=255,
     .data.text={.utf8=title_text,.bytes=sizeof(title_text)-1,.capacity=64,
                 .font=KSN_BODY,.color=0xffffffff}},
    {.kind=KSN_RECT,.bounds={12,28,112,56},.clip={0,0,240,135},.opacity=216,
     .data.shape={0x164c70d8,0,0}},
    {.kind=KSN_RECT,.bounds={34,36,134,64},.clip={0,0,240,135},.opacity=172,
     .data.shape={0x65d7bcac,0,0}},
    {.kind=KSN_ROUND_RECT,.bounds={16,76,36,96},.clip={0,0,240,135},.opacity=210,
     .data.shape={0xf5bd4fff,6,0}},
    {.kind=KSN_ROUND_RECT,.bounds={12,119,24,127},.clip={0,0,240,135},.opacity=255,
     .data.shape={0x62e0a8ff,3,0}},
    {.kind=KSN_STROKE,.bounds={10,117,232,129},.clip={0,0,240,135},.opacity=170,
     .data.shape={0x80b5cfaa,0,1}},
    {.kind=KSN_TEXT,.bounds={12,100,180,113},.clip={0,0,240,135},.opacity=255,
     .data.text={.utf8=counter_text,.bytes=6,.capacity=24,.font=KSN_CAPTION,
                 .color=0xa8d8efff}}
};
/* Two rects, as apps/kasane/demo.js caches: an instance is isolated
 * premultiplied composition, so its children render through render_group. */
static const ksn_draw tile_draws[2]={
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
static int run_arm(unsigned arm){
    g_ksn_row_coverage=arm?1:0;
    ksn_core_init(&core);ksn_cache_init(&cache);
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx tx;ksn_ref refs[SCENE];
    ksn_template tile;ksn_instance left,right;
    ksn_render_stats stats;
    memcpy(scene,scene_base,sizeof(scene));
    strcpy(counter_text,"tick 0");
    arm_hash[arm]=0;
    CHECK(ksn_cache_create(&cache,KSN_APP,tile_draws,2,&tile)==KSN_OK);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,background)==KSN_OK);
    for(unsigned i=0;i<SCENE;i++)CHECK(app.ops->add(app.ctx,tx,&scene[i],&refs[i])==KSN_OK);
    CHECK(ksn_core_group(&core,KSN_APP,tx,refs[2],2,208)==KSN_OK);
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
            CHECK(app.ops->change(app.ctx,tx,refs[4],&change)==KSN_OK);
            change=(ksn_change){.property=KSN_SET_RECT,.value.rect={12,119,12+width,127}};
            CHECK(app.ops->change(app.ctx,tx,refs[5],&change)==KSN_OK);
            change=(ksn_change){.property=KSN_SET_COLOR,
                                .value.color=(tick&16)?0x295d86d8u:0x164c70d8u};
            CHECK(app.ops->change(app.ctx,tx,refs[2],&change)==KSN_OK);
            place_left.y=28+(int)(tick%18);
            CHECK(ksn_cache_place(&cache,&core,tx,left,&place_left)==KSN_OK);
            CHECK(ksn_cache_set_visible(&cache,&core,tx,right,(tick%40)<31)==KSN_OK);
            change=(ksn_change){.property=KSN_SET_REVEAL,.value.reveal=(uint16_t)(tick/6%11)};
            CHECK(app.ops->change(app.ctx,tx,refs[1],&change)==KSN_OK);
            snprintf(counter_text,sizeof(counter_text),"tick %u",tick);
            change=(ksn_change){.property=KSN_SET_TEXT,
                                .value.text={counter_text,(uint16_t)strlen(counter_text)}};
            CHECK(app.ops->change(app.ctx,tx,refs[7],&change)==KSN_OK);
            CHECK(ksn_core_group(&core,KSN_APP,tx,refs[2],2,(uint8_t)(128+tick%128))==KSN_OK);
            CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
            if(tick%17==0)ksn_core_invalidate(&core);
        }
        ksn_frame frame;
        CHECK(ksn_core_frame(&core,&frame)==KSN_OK);
#ifdef KSN_COUNT_CALLS
        if(!tick){covers_entries=0;run_entries=0;}
#endif
        CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        CHECK(ksn_cache_resolve(&cache,&core,frame.ticket,true)==KSN_OK);
#ifdef KSN_COUNT_CALLS
        if(!tick){frame0_covers[arm]=covers_entries;frame0_runs[arm]=run_entries;}
#endif
        if(!tick||tick%17==0){ /* invalidated frames must repaint everything */
            CHECK(stats.transferred_bytes==64800);
            full_frames++;
        }
        frame_hash[arm][tick]=hash_panel();
        arm_hash[arm]=arm_hash[arm]*31u+frame_hash[arm][tick];
    }
#ifdef KSN_COUNT_CALLS
    arm_covers[arm]=covers_entries;arm_runs[arm]=run_entries;
#endif
    return 0;
}

int main(void){
    CHECK(ksn_cache_bind(&cache,&cache_commands,&cache_text)==KSN_OK);
    CHECK(invisible_and_unsupported()==0);
    CHECK(exhaustive_sweep()==0);
    CHECK(wide_radius_sweep()==0);
    CHECK(random_sweep()==0);
    CHECK(failed_shapes==0);
    printf("coverage runs: predicate and runs agree on %u pixels over %u rows "
           "(rect/text/round rect/gradient/stroke, radius 0..255, clipped)\n",
           compared_pixels,compared_rows);
    CHECK(compared_rows>1000000&&compared_pixels>100000000);
    CHECK(run_arm(0)==0);
    CHECK(run_arm(1)==0);
    for(unsigned tick=0;tick<FRAMES;tick++)if(frame_hash[0][tick]!=frame_hash[1][tick]){
        fprintf(stderr,"frame %u differs: predicate %08x runs %08x\n",tick,
                frame_hash[0][tick],frame_hash[1][tick]);
        CHECK(0);
    }
    CHECK(full_frames==2*(1+FRAMES/17));
    printf("coverage runs: %u frames identical both ways, %u of them full 64,800-byte "
           "repaints (rolling hash %08x)\n",FRAMES,2*(1+FRAMES/17),(uint32_t)arm_hash[1]);
#ifdef KSN_COUNT_CALLS
    printf("coverage runs: calls per full REPLACE frame: predicate arm covers=%" PRIu64
           " runs=%" PRIu64 "%s; runs arm covers=%" PRIu64 " runs=%" PRIu64 "%s\n",
           frame0_covers[0],frame0_runs[0],g_ksn_row_coverage?"":" (switch off)",
           frame0_covers[1],frame0_runs[1]," (switch on)");
    printf("coverage runs: calls over %u mixed frames: predicate arm covers=%" PRIu64
           " runs=%" PRIu64 "; runs arm covers=%" PRIu64 " runs=%" PRIu64 "\n",
           FRAMES,arm_covers[0],arm_runs[0],arm_covers[1],arm_runs[1]);
    /* The arms must be the shapes they claim to be: the predicate arm never
     * solves a row, the runs arm never asks the predicate, and a row solve is
     * strictly cheaper than the pixels it replaces. */
    CHECK(frame0_covers[0]>0&&frame0_runs[0]==0);
    CHECK(frame0_covers[1]==0&&frame0_runs[1]>0&&frame0_runs[1]<frame0_covers[0]);
    CHECK(arm_covers[0]>frame0_covers[0]&&arm_covers[1]==0&&arm_runs[1]>frame0_runs[1]);
#endif
    return 0;
}
