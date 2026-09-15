/* Candidate 4c of docs/perf/kasane-opt-survey.md built as a row profile: the
 * value `sample` returns is a function of x alone (a horizontal gradient's
 * ramp), of y alone (a vertical gradient, i.e. one value per row) or of neither
 * (every shape and text command), and g_ksn_row_table has the renderer build a
 * row's values once and read them per pixel instead of calling `sample` per
 * pixel.
 *
 * ksn_render.c is included for its statics: `sample` is the reference the table
 * has to reproduce and `row_table_build` is deliberately not exported from it.
 * Do NOT also compile ksn_render.c into this binary.
 *
 * Two proofs, both host-only:
 *   1. the table's values are the pixel path's values: `row_table_build` is
 *      called for every kind, box, clip, radius, stroke width and row the
 *      primitive API allows, over the window the coverage solver names, and
 *      every entry it produces is compared against `sample`; the ramp is then
 *      swept over channel pairs and lengths, including windows that start part
 *      way into a ramp, the widest ramp the int16 bounds allow, and windows the
 *      builder must refuse;
 *   2. 120 frames of a demo.js-shaped scene render in FOUR arms -- both switches
 *      in both positions -- and every frame is compared pixel by pixel over the
 *      whole 240x135 panel, not only hashed.
 *
 * Built with -DKSN_COUNT_CALLS it also counts entries into `sample` and into
 * `row_table_for` through -finstrument-functions, so "per-pixel evaluation
 * became a load" is a count of calls that did not happen, not a claim. */
#include "core_fixture.h"
#include "ksn_cache.h"
#include "ksn_render.c"
#include <stdio.h>
#include <string.h>
#ifdef KSN_COUNT_CALLS
#include <inttypes.h>
static uint64_t sample_entries,build_entries,arm_samples[4],arm_builds[4];
void __cyg_profile_func_enter(void *,void *) __attribute__((no_instrument_function));
void __cyg_profile_func_enter(void *this_fn,void *call_site);
void __cyg_profile_func_enter(void *this_fn,void *call_site){
    (void)call_site;
    if(this_fn==(void *)&sample)sample_entries++;
    else if(this_fn==(void *)&row_table_for)build_entries++;
}
#endif

#define CHECK(x) do{if(!(x)){fprintf(stderr,"row table line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static unsigned compared_rows,compared_pixels,failed_shapes,reported;
static void report(const ksn_draw *draw,const char *what,int y,int left,int right,int x){
    failed_shapes++;
    if(reported++>=8)return;
    fprintf(stderr,"%s: kind %d bounds %d,%d,%d,%d clip %d,%d,%d,%d axis %u radius %u width %u "
                   "opacity %u from %08x to %08x row %d window [%d,%d) x %d\n",what,(int)draw->kind,
            draw->bounds.x0,draw->bounds.y0,draw->bounds.x1,draw->bounds.y1,
            draw->clip.x0,draw->clip.y0,draw->clip.x1,draw->clip.y1,
            draw->data.gradient.axis,
            draw->kind==KSN_GRADIENT?draw->data.gradient.radius:draw->data.shape.radius,
            draw->data.shape.width,draw->opacity,draw->data.gradient.from,
            draw->data.gradient.to,y,left,right,x);
}

/* ---- 1. the table's values must be the values `sample` returns ----------- */
/* One row, over the window the callsite would name: [left,right) has to hold
 * only x the row covers, which is what the runs arm passes (the span of its
 * runs). A decline is not a failure -- the caller then asks `sample` -- but one
 * moved entry is. */
static int same_row(const ksn_frame_view *command,int y,int left,int right){
    ksn_row_table table;
    if(!row_table_build(&table,command,y,left,right))return 1;
    unsigned counted=0;
    for(int x=left;x<right;x++){
        if(sample(command,x,y)!=row_table_value(&table,x)){
            report(&command->draw,"row value",y,left,right,x);
            return 0;
        }
        counted++;compared_pixels++;
    }
    if(!table.constant&&table.count!=counted)return 0; /* every column is built */
    compared_rows++;
    return 1;
}
/* Every row of the box, over the window the runs arm names for that row: the
 * span from the first run's start to the last run's end. That is exactly the
 * window ksn_render.c hands the builder, the gap between two stroke runs and
 * the arc of a rounded row included. */
static int same_shape_rows(const ksn_draw *draw,int y0,int y1){
    ksn_frame_view command={0};
    command.visible=true;
    command.draw=*draw;
    int left=draw->bounds.x0>draw->clip.x0?draw->bounds.x0:draw->clip.x0;
    int right=draw->bounds.x1<draw->clip.x1?draw->bounds.x1:draw->clip.x1;
    if(left<0)left=0;
    if(right>240)right=240;
    for(int y=y0;y<y1;y++){
        ksn_x_run runs[KSN_ROW_RUNS];
        unsigned count=coverage_runs(&command,y,left,right,runs);
        if(!count)continue;
        if(!same_row(&command,y,runs[0].x0,runs[count-1].x1))return 1;
    }
    return 0;
}
static int constants_and_refusals(void){
    ksn_frame_view command={0};
    ksn_row_table table;
    command.draw.kind=KSN_RECT;command.draw.bounds=(ksn_rect){0,0,240,135};
    command.draw.clip=(ksn_rect){0,0,240,135};command.draw.opacity=255;
    command.draw.data.shape.color=0x8f7c6bff;
    CHECK(row_table_build(&table,&command,0,0,240));
    CHECK(table.constant&&table.color==0x8f7c6bff);
    command.draw.kind=KSN_TEXT;command.draw.data.text.color=0xa15f37b7;
    CHECK(row_table_build(&table,&command,0,0,240));
    CHECK(table.constant&&table.color==0xa15f37b7);
    /* An empty window: no column to fill, and the caller has nothing to
     * composite, so a decline is the whole answer. */
    CHECK(!row_table_build(&table,&command,0,7,7));
    CHECK(!row_table_build(&table,&command,0,9,7));
    return 0;
}
static const ksn_rect clip_cases[]={{0,0,240,135},{-40,-30,400,300},{7,3,53,44},
                                    {0,0,0,0},{11,9,11,9},{20,20,21,21},{160,-5,240,135},
                                    {0,0,240,8},{1,1,239,134}};
#define CLIP_CASES (sizeof(clip_cases)/sizeof(clip_cases[0]))
/* Every kind over the box/offset/radius/clip space, every row of the box: the
 * sweep shape the coverage solver's own test uses, so the two agree about which
 * pixels exist as well as about which value they take. */
static int exhaustive_sweep(void){
    const int offsets[][2]={{0,0},{-3,-2},{5,3},{-40,-30},{300,200}};
    for(unsigned o=0;o<sizeof(offsets)/sizeof(offsets[0]);o++)
    for(unsigned c=0;c<CLIP_CASES;c++){
        int bx0=offsets[o][0],by0=offsets[o][1];
        for(int w=1;w<=20;w++)for(int h=1;h<=20;h++){
            ksn_draw draw={.kind=KSN_RECT,.bounds={(int16_t)bx0,(int16_t)by0,
                                                  (int16_t)(bx0+w),(int16_t)(by0+h)},
                           .clip=clip_cases[c],.opacity=200,.data.shape={0x8f7c6bff,0,0}};
            if(same_shape_rows(&draw,by0,by0+h))return 1;
            draw.kind=KSN_TEXT;draw.data.text.color=0x2f6f4fd1;
            if(same_shape_rows(&draw,by0,by0+h))return 1;
            for(unsigned radius=0;radius<=8;radius++){
                draw.kind=KSN_ROUND_RECT;draw.data.shape.radius=(uint8_t)radius;
                if(same_shape_rows(&draw,by0,by0+h))return 1;
                draw.kind=KSN_GRADIENT;draw.data.gradient.radius=(uint8_t)radius;
                draw.data.gradient.from=0x102030ff;draw.data.gradient.to=0xf0e0d0ff;
                draw.data.gradient.axis=0;draw.data.gradient.dither=false;
                if(same_shape_rows(&draw,by0,by0+h))return 1;
                draw.data.gradient.axis=1; /* and the one value per row case */
                if(same_shape_rows(&draw,by0,by0+h))return 1;
                draw.data.gradient.dither=true;
                if(same_shape_rows(&draw,by0,by0+h))return 1;
            }
            for(unsigned width=0;width<=18;width++){
                draw.kind=KSN_STROKE;draw.data.shape.width=(uint8_t)width;
                if(same_shape_rows(&draw,by0,by0+h))return 1;
            }
        }
    }
    return 0;
}
/* Boxes wider than the panel and boxes narrower than the ramp: a window can
 * start part way into a ramp (index > 0), and the ramp itself can be longer than
 * the 240 columns of a row. */
static int wide_box_sweep(void){
    const int widths[]={1,2,3,17,64,239,240,241,320,700};
    for(unsigned w=0;w<sizeof(widths)/sizeof(widths[0]);w++)
    for(int x0=-300;x0<=300;x0+=37){
        unsigned length=(unsigned)widths[w];
        ksn_draw draw={.kind=KSN_GRADIENT,.bounds={(int16_t)x0,4,(int16_t)(x0+widths[w]),12},
                       .clip={0,0,240,135},.opacity=219,
                       .data.gradient={0x253849cc,0xe0a972d2,0,7,true}};
        int left=x0>0?x0:0,right=x0+widths[w]<240?x0+widths[w]:240;
        if(left>=right)continue;
        ksn_frame_view command={0};
        command.visible=true;command.draw=draw;
        for(int y=4;y<12;y++){
            ksn_x_run runs[KSN_ROW_RUNS];
            unsigned count=coverage_runs(&command,y,left,right,runs);
            if(!count)continue;
            if(!same_row(&command,y,runs[0].x0,runs[count-1].x1))return 1;
            /* A window reaching past the last ramp column is not the window the
             * runs arm names, but the builder has to refuse it rather than
             * invent values `sample` never produces. A one-column ramp is one
             * colour, so it accepts any window. */
            ksn_row_table table;
            bool fits=row_table_build(&table,&command,y,0,240);
            if(length>=2)CHECK(fits==(draw.bounds.x0<=0&&draw.bounds.x1>=240));
            else CHECK(fits&&table.constant);
        }
    }
    return 0;
}
/* The ramp itself: every channel pair over a cover set of lengths, then random
 * RGBA over every length. The four channels run the same arithmetic on their own
 * byte, so one channel driven through every pair plus random RGBA covers the
 * rest, packing and channel order included. */
static const unsigned lengths[]={2,3,4,5,7,8,15,16,17,31,32,33,63,64,65,100,127,128,
                                 129,200,254,255,256,257,1000,32767};
#define LENGTHS (sizeof(lengths)/sizeof(lengths[0]))
/* The pair sweep is the expensive one -- 65,536 channel pairs each paying for
 * every column it builds -- so it runs over a cover set of ramp lengths and the
 * random pass below covers the rest of the range. */
static const unsigned pair_lengths[]={2,3,5,17,64,255,256,32767};
#define PAIR_LENGTHS (sizeof(pair_lengths)/sizeof(pair_lengths[0]))
static uint32_t rng_state=0x9e3779b9u;
static unsigned next_rand(unsigned bound){
    rng_state^=rng_state<<13;rng_state^=rng_state>>17;rng_state^=rng_state<<5;
    return (unsigned)(rng_state%bound);
}
/* One row of a ramp, over the panel columns [x0,x0+count): the table's entry i
 * is the ramp's value at index (x0 - bounds.x0) + i, which is where `sample`
 * reads it from. */
static int ramp_row(const ksn_draw *draw,unsigned length,int x0,unsigned count){
    ksn_frame_view command={0};
    command.visible=true;command.draw=*draw;
    ksn_row_table table;
    if(!row_table_build(&table,&command,3,x0,x0+(int)count))return 1;
    unsigned index=(unsigned)(x0-draw->bounds.x0);
    for(unsigned i=0;i<count;i++){
        if(table.value[i]!=interpolate(draw->data.gradient.from,draw->data.gradient.to,
                                       index+i,length)){
            report(draw,"ramp entry",3,x0,x0+(int)count,(int)(index+i));
            return 1;
        }
        compared_pixels++;
    }
    compared_rows++;
    return 0;
}
/* A window inside the ramp: at most 240 columns (a row is 240 wide), starting
 * anywhere the ramp defines and never running past its last column. */
static unsigned window_start(unsigned length){return next_rand(length);}
static unsigned window_count(unsigned length,unsigned start){
    unsigned room=length-start;
    return 1+next_rand(room>240?240:room);
}
static int ramp_sweep(void){
    for(unsigned a=0;a<256;a++)for(unsigned b=0;b<256;b++)
    for(unsigned n=0;n<PAIR_LENGTHS;n++){
        unsigned length=pair_lengths[n],start=window_start(length);
        ksn_draw draw={.kind=KSN_GRADIENT,.bounds={0,3,(int16_t)length,5},.clip={0,0,240,135},
                       .opacity=255,
                       .data.gradient={(uint32_t)(a<<24|a<<16|a<<8|a),
                                       (uint32_t)(b<<24|b<<16|b<<8|b),0,0,false}};
        if(ramp_row(&draw,length,(int)start,window_count(length,start)))return 1;
    }
    for(unsigned n=0;n<200000;n++){
        unsigned length=lengths[next_rand((unsigned)LENGTHS)];
        unsigned start=window_start(length);
        /* axis 0: this is the ramp's own sweep. A vertical gradient has no ramp
         * over x and is compared against `sample` by same_row above. */
        ksn_draw draw={.kind=KSN_GRADIENT,.bounds={0,3,(int16_t)length,5},.clip={0,0,240,135},
                       .opacity=255,.data.gradient={(ksn_rgba)rng_state|0xffu,
                                                    (ksn_rgba)(rng_state*2654435761u)|0xffu,
                                                    0,(uint8_t)next_rand(9),next_rand(2)!=0}};
        if(ramp_row(&draw,length,(int)start,window_count(length,start)))return 1;
    }
    /* The widest ramp the types allow: ksn_rect is int16_t, so x1-x0 tops out at
     * 65535 with x0 = -32768, and every column of the panel then starts 32,768
     * columns into the ramp. That is the case the 32-bit bound ksn_render.c
     * states has to survive. */
    for(unsigned a=0;a<256;a+=13)for(unsigned b=0;b<256;b+=13){
        ksn_draw draw={.kind=KSN_GRADIENT,.bounds={-32768,3,32767,5},.clip={0,0,240,135},
                       .opacity=255,
                       .data.gradient={(uint32_t)(a<<24|b<<16|a<<8|b),
                                       (uint32_t)(b<<24|a<<16|b<<8|a),0,0,false}};
        if(ramp_row(&draw,65535,0,240))return 1;
    }
    return 0;
}
/* The switch itself: NULL is the old path (the caller asks `sample` per pixel),
 * and a buildable row must not decline. */
static int switch_means_old_path(void){
    ksn_draw draw={.kind=KSN_GRADIENT,.bounds={0,4,240,12},.clip={0,0,240,135},
                   .opacity=255,.data.gradient={0x102030ff,0xf0e0d0ff,0,0,false}};
    ksn_frame_view command={0};
    command.visible=true;command.draw=draw;
    const ksn_rgba ref_from=draw.data.gradient.from,ref_to=draw.data.gradient.to;
    g_ksn_row_table=0;
    CHECK(row_table_for(&command,4,0,240)==NULL);
    g_ksn_row_table=1;
    const ksn_row_table *table=row_table_for(&command,4,0,240);
    CHECK(table!=NULL&&!table->constant&&table->count==240);
    CHECK(table->first==0);
    CHECK(row_table_value(table,0)==interpolate(ref_from,ref_to,0,240));
    CHECK(row_table_value(table,239)==interpolate(ref_from,ref_to,239,240));
    CHECK(table->color==table->value[0]);
    return 0;
}

/* ---- 2. whole frame, four arms, every pixel of every frame ----------------- */
#define FRAMES 120u
#define SCENE 13u
/* Arms: both switches in both positions. Arm 0 is the reference -- the coverage
 * solver on (its shipping default) and the row table off, which is the pixel
 * path this file ran before the table existed. Arm 1 is the shipping pair. */
#define ARMS 4u
static const int arm_coverage[ARMS]={1,1,0,0};
static const int arm_table[ARMS]={0,1,0,1};
KSN_TEST_CORE(arm_core0,static);
KSN_TEST_CORE(arm_core1,static);
KSN_TEST_CORE(arm_core2,static);
KSN_TEST_CORE(arm_core3,static);
static ksn_core *const cores[ARMS]={&arm_core0,&arm_core1,&arm_core2,&arm_core3};
static ksn_cache caches[ARMS];
static ksn_cache_command_block cache_commands[ARMS];
static ksn_cache_text_block cache_text[ARMS];
static uint16_t panels[ARMS][240*135],strips[ARMS][240*8];
static ksn_display_port displays[ARMS];
static ksn_ref refs[ARMS][SCENE];
static ksn_template templates[ARMS];
static ksn_instance instances[ARMS][2];
static ksn_placement places[ARMS][2];
static uint32_t frame_hash[ARMS][FRAMES],arm_hash[ARMS];
static unsigned full_frames,compared_panels,differing_pixels,worst_step;

static uint16_t *get_strip(void *ctx){
    for(unsigned arm=0;arm<ARMS;arm++)if(ctx==&displays[arm])return strips[arm];
    return strips[0];
}
static ksn_result send_strip(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    unsigned arm=0;
    for(unsigned i=0;i<ARMS;i++)if(ctx==&displays[i])arm=i;
    memcpy(panels[arm]+(unsigned)y*240,pixels,(unsigned)rows*240*sizeof(*pixels));
    return KSN_OK;
}
/* Synthetic face: coverage depends only on x, y and reveal, so every arm sees
 * exactly the same ink (the device's jpfont face is not the subject here). */
static unsigned ink(int x,int y,uint16_t reveal){
    static const uint8_t values[]={0,1,127,128,254,255};
    if(x<0)return 0;
    if(((unsigned)(x+3*y)&7u)>(reveal%11u))return 0;
    return values[(unsigned)(x*3+y)%6u];
}
static ksn_result span(void *ctx,const ksn_draw *draw,uint16_t reveal,int x,int y,
                       unsigned count,uint8_t *out){
    (void)ctx;(void)draw;
    for(unsigned i=0;i<count;i++)out[i]=(uint8_t)ink(x+(int)i,y,reveal);
    return KSN_OK;
}
static const ksn_text_port text_port={.span=span};
static void hash_panel(unsigned arm,uint32_t *out){
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<240*135;i++){hash^=panels[arm][i];hash*=16777619u;}
    *out=hash;
}
/* Every pixel of one frame against the reference arm. The 120-frame harness
 * compares a hash; this says which pixel moved and by how much when one does,
 * over the full 240x135 panel rather than a sampled subset. */
static int compare_panels(unsigned arm,unsigned frame){
    unsigned differ=0,worst=0;
    for(unsigned i=0;i<240*135;i++){
        uint16_t a=panels[0][i],b=panels[arm][i];
        if(a==b)continue;
        differ++;
        unsigned ar=(a>>11)&31,ag=(a>>5)&63,ab=a&31;
        unsigned br=(b>>11)&31,bg=(b>>5)&63,bb=b&31;
        int dr=(int)((ar<<3)|(ar>>2))-(int)((br<<3)|(br>>2));
        int dg=(int)((ag<<2)|(ag>>4))-(int)((bg<<2)|(bg>>4));
        int db=(int)((ab<<3)|(ab>>2))-(int)((bb<<3)|(bb>>2));
        if(dr<0)dr=-dr;
        if(dg<0)dg=-dg;
        if(db<0)db=-db;
        unsigned step=(unsigned)(dr>dg?dr:dg);
        if((unsigned)db>step)step=(unsigned)db;
        if(step>worst)worst=step;
        if(differ<=4)fprintf(stderr,"frame %u arm %u pixel %u: %04x against %04x\n",
                             frame,arm,i,a,b);
    }
    compared_panels++;
    differing_pixels+=differ;
    if(worst>worst_step)worst_step=worst;
    return differ!=0;
}
static char counter_text[32]="tick 0";
static const char title_text[]="Kasane 日本語";
static ksn_draw scene[SCENE];
/* The scene holds one of every shape the table has to be right about: the
 * demo's dithered 240-column gradient (the ramp), a gradient wider than the
 * panel (a window that starts part way into the ramp), a vertical gradient (one
 * value per row), a gradient whose ramp is a single column, text, a
 * semi-transparent rect and round rect (one colour per row), a stroke (two
 * runs), an opaque rect (which never reaches the per-pixel path) and a
 * two-child group whose children are a gradient and a rect (the group's own
 * composite, not the direct path). Two cached instances of a two-command
 * template add a gradient child to the group path as well. */
static const ksn_draw scene_base[SCENE]={
    {.kind=KSN_GRADIENT,.bounds={0,0,240,18},.clip={0,0,240,135},.opacity=255,
     .data.gradient={0x0d2940ff,0x498781ff,0,0,true}},
    {.kind=KSN_GRADIENT,.bounds={-40,20,280,44},.clip={0,0,240,135},.opacity=219,
     .data.gradient={0x253849cc,0xe0a972d2,0,8,true}},
    {.kind=KSN_GRADIENT,.bounds={150,46,240,60},.clip={0,0,240,135},.opacity=255,
     .data.gradient={0x123456ff,0xffff80ff,1,0,false}},
    {.kind=KSN_GRADIENT,.bounds={168,62,169,86},.clip={0,0,240,135},.opacity=200,
     .data.gradient={0xbe3344ff,0x00ff00ff,0,0,false}},
    {.kind=KSN_TEXT,.bounds={8,3,232,17},.clip={0,0,240,135},.opacity=255,
     .data.text={.utf8=title_text,.bytes=sizeof(title_text)-1,.capacity=64,
                 .font=KSN_BODY,.color=0xffffffff}},
    {.kind=KSN_TEXT,.bounds={12,100,180,113},.clip={0,0,240,135},.opacity=255,
     .data.text={.utf8=counter_text,.bytes=6,.capacity=24,.font=KSN_CAPTION,
                 .color=0xa8d8efff}},
    {.kind=KSN_RECT,.bounds={12,28,112,56},.clip={0,0,240,135},.opacity=216,
     .data.shape={0x164c70d8,0,0}},
    {.kind=KSN_ROUND_RECT,.bounds={16,76,36,96},.clip={0,0,240,135},.opacity=210,
     .data.shape={0xf5bd4fff,6,0}},
    {.kind=KSN_STROKE,.bounds={10,117,232,129},.clip={0,0,240,135},.opacity=170,
     .data.shape={0x80b5cfaa,0,1}},
    {.kind=KSN_RECT,.bounds={150,30,200,60},.clip={0,0,240,135},.opacity=255,
     .data.shape={0x2f6f4fff,0,0}},
    {.kind=KSN_GRADIENT,.bounds={20,62,140,90},.clip={0,0,240,135},.opacity=230,
     .data.gradient={0x7788aaff,0x221133ff,0,5,true}},
    {.kind=KSN_RECT,.bounds={24,66,136,86},.clip={0,0,240,135},.opacity=220,
     .data.shape={0x63d7bccc,0,0}},
    {.kind=KSN_ROUND_RECT,.bounds={196,110,239,134},.clip={0,0,240,135},.opacity=255,
     .data.shape={0x62e0a8ff,3,0}}
};
/* A cached template is RECT/ROUND_RECT/STROKE only (ksn_cache_create), so it is
 * the constant case of the table that a template instance exercises; the group
 * with a gradient child is scene commands 10 and 11. */
static const ksn_draw tile_draws[2]={
    {.kind=KSN_ROUND_RECT,.bounds={0,0,42,24},.clip={0,0,240,135},.opacity=255,
     .data.shape={0x185071ff,4,0}},
    {.kind=KSN_RECT,.bounds={4,4,38,20},.clip={0,0,240,135},.opacity=220,
     .data.shape={0x63d7bccc,0,0}}
};
static int render_frame(unsigned arm,unsigned frame){
    ksn_core *core=cores[arm];
    ksn_client app=ksn_core_client(core,KSN_APP);
    ksn_tx tx;ksn_render_stats stats;ksn_frame sealed;
    ksn_change change;
    memcpy(scene,scene_base,sizeof(scene));
    snprintf(counter_text,sizeof(counter_text),"tick %u",frame);
    scene[5].data.text.utf8=counter_text;
    scene[5].data.text.bytes=(uint16_t)strlen(counter_text);
    if(frame&&frame%17==0)ksn_core_invalidate(core); /* must repaint every band */
    if(!frame){
        CHECK(ksn_cache_create(&caches[arm],KSN_APP,tile_draws,2,&templates[arm])==KSN_OK);
        CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
        CHECK(app.ops->background(app.ctx,tx,0x071425ff)==KSN_OK);
        for(unsigned i=0;i<SCENE;i++)CHECK(app.ops->add(app.ctx,tx,&scene[i],&refs[arm][i])==KSN_OK);
        /* Commands 10 and 11 are one isolated group: a gradient and a rect. */
        CHECK(ksn_core_group(core,KSN_APP,tx,refs[arm][10],2,208)==KSN_OK);
        places[arm][0]=(ksn_placement){142,28,{0,0,240,135},230,true};
        places[arm][1]=(ksn_placement){188,72,{0,0,240,135},175,true};
        for(unsigned i=0;i<2;i++)
            CHECK(ksn_cache_instantiate(&caches[arm],core,tx,templates[arm],&places[arm][i],
                                        &instances[arm][i])==KSN_OK);
        CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    }else{
        CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_RECT,
                            .value.rect={20,62,(int16_t)(46+frame*7%180),90}};
        CHECK(app.ops->change(app.ctx,tx,refs[arm][10],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_RECT,
                            .value.rect={(int16_t)(-40+frame*3%80),20,
                                         (int16_t)(-40+frame*3%80)+240,44}};
        CHECK(app.ops->change(app.ctx,tx,refs[arm][1],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_COLOR,
                            .value.color=(frame&16)?0x295d86d8u:0x164c70d8u};
        CHECK(app.ops->change(app.ctx,tx,refs[arm][6],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_REVEAL,.value.reveal=(uint16_t)(frame/6%11)};
        CHECK(app.ops->change(app.ctx,tx,refs[arm][4],&change)==KSN_OK);
        /* Move and show/hide the cached instances: the group path with a
         * gradient child runs with a different placement every frame. */
        places[arm][0].y=(int16_t)(28+frame%18);
        CHECK(ksn_cache_place(&caches[arm],core,tx,instances[arm][0],&places[arm][0])==KSN_OK);
        CHECK(ksn_cache_set_visible(&caches[arm],core,tx,instances[arm][1],(frame%40)<31)==KSN_OK);
        CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    }
    CHECK(ksn_core_frame(core,&sealed)==KSN_OK);
#ifdef KSN_COUNT_CALLS
    sample_entries=0;build_entries=0;
#endif
    CHECK(ksn_render_rects(core,&displays[arm],&stats)==KSN_OK);
    CHECK(ksn_cache_resolve(&caches[arm],core,sealed.ticket,true)==KSN_OK);
#ifdef KSN_COUNT_CALLS
    arm_samples[arm]=sample_entries;arm_builds[arm]=build_entries;
#endif
    if(!frame||frame%17==0){
        CHECK(stats.transferred_bytes==64800u); /* invalidated: repaint everything */
        full_frames++;
    }
    return 0;
}
static int run_all(void){
    for(unsigned arm=0;arm<ARMS;arm++){
        displays[arm].ctx=&displays[arm];
        displays[arm].text=&text_port;
        displays[arm].width=240;displays[arm].height=135;displays[arm].strip_rows=8;
        displays[arm].strip=get_strip;displays[arm].present=send_strip;
        CHECK(ksn_cache_bind(&caches[arm],&cache_commands[arm],&cache_text[arm])==KSN_OK);
        ksn_core_init(cores[arm]);
        memset(panels[arm],0,sizeof(panels[arm]));
        arm_hash[arm]=0;
    }
    for(unsigned frame=0;frame<FRAMES;frame++){
        for(unsigned arm=0;arm<ARMS;arm++){
            g_ksn_row_coverage=arm_coverage[arm];
            g_ksn_row_table=arm_table[arm];
            if(render_frame(arm,frame))return 1;
            hash_panel(arm,&frame_hash[arm][frame]);
            arm_hash[arm]=arm_hash[arm]*31u+frame_hash[arm][frame];
        }
        for(unsigned arm=1;arm<ARMS;arm++){
            if(frame_hash[arm][frame]!=frame_hash[0][frame]){
                fprintf(stderr,"frame %u: arm %u hash %08x, reference %08x\n",frame,arm,
                        frame_hash[arm][frame],frame_hash[0][frame]);
                return 1;
            }
            if(compare_panels(arm,frame))return 1;
        }
    }
    g_ksn_row_coverage=1;g_ksn_row_table=1;
    return 0;
}

int main(void){
    CHECK(constants_and_refusals()==0);
    CHECK(switch_means_old_path()==0);
    CHECK(exhaustive_sweep()==0);
    CHECK(wide_box_sweep()==0);
    CHECK(ramp_sweep()==0);
    CHECK(failed_shapes==0);
    printf("row table: table values match sample on %u pixels over %u rows "
           "(rect/text/round rect/stroke/gradient, both axes, ramp lengths to 65,535)\n",
           compared_pixels,compared_rows);
    CHECK(compared_rows>3000000&&compared_pixels>40000000);
    CHECK(run_all()==0);
    CHECK(failed_shapes==0);
    CHECK(full_frames==ARMS*(1+FRAMES/17));
    CHECK(compared_panels==(ARMS-1)*FRAMES);
    CHECK(differing_pixels==0&&worst_step==0);
    printf("row table: %u frames, %u arms (table/coverage on and off), %u panels of 32,400 "
           "pixels compared pixel by pixel, %u pixels differed (worst channel step %u)\n",
           FRAMES,ARMS,compared_panels,differing_pixels,worst_step);
    printf("row table: rolling hashes %08x %08x %08x %08x (reference (coverage,table)=(1,0) first)\n",
           (uint32_t)arm_hash[0],(uint32_t)arm_hash[1],(uint32_t)arm_hash[2],
           (uint32_t)arm_hash[3]);
#ifdef KSN_COUNT_CALLS
    printf("row table: sample calls in one full REPLACE frame: (1,0) reference=%" PRIu64
           " (1,1) table=%" PRIu64 " (0,0) predicate=%" PRIu64 " (0,1) predicate=%" PRIu64
           "; row builds: %" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n",
           arm_samples[0],arm_samples[1],arm_samples[2],arm_samples[3],
           arm_builds[0],arm_builds[1],arm_builds[2],arm_builds[3]);
    /* The claim: with the table on the per-pixel sampling is gone. The direct
     * path and the group's children ask `sample` only where a row is built --
     * the vertical gradient's one colour per row -- instead of once per pixel,
     * and the reference arm enters the builder for the same call sites and
     * declines (the switch off), which is why its build count matches. */
    CHECK(arm_samples[1]*100<arm_samples[0]);
    CHECK(arm_builds[1]==arm_builds[0]&&arm_builds[0]>0);
    CHECK(arm_builds[2]==0&&arm_builds[3]==0); /* no window, no build */
    CHECK(arm_samples[3]==arm_samples[2]);
#endif
    puts("row table PASS: every arm byte-identical over the whole panel");
    return 0;
}
