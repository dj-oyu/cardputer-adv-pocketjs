/* Candidate 4e of docs/perf/kasane-opt-survey.md (boundary 4, per-pixel): the
 * visible threshold skip, MEASURED before it is implemented.
 *
 * A 5/6/5 panel has 32/64/32 levels per channel, so a blend whose source and
 * destination name the same level -- or a neighbour -- could be left unwritten.
 * The price of that is a per-pixel test, which is only worth paying if the
 * share of such pixels is large and if leaving them unwritten stays invisible.
 * This harness answers both from measured counts, per scene and per command
 * kind, before any shipping code exists:
 *
 *   - `step`  : the source's own levels are within one step of dst in every
 *               channel (the reading "src and dst differ by at most one step");
 *   - `bound` : alpha * max channel |src8 - dst8| <= 2*255, the composite-aware
 *               bound, which is what catches an anti-aliased text edge (tiny
 *               alpha) that `step` misses;
 *   - `both`  : the two together, the criterion a non-dithering skip ships with;
 *   - `exact` : the source's levels equal dst exactly, the only arm a dithered
 *               command may use (quantize() can add one level on top).
 * `moved` and `worst` are what the CHAIN ITSELF would have written on the
 * pixels an arm selects: moved pixels and the worst channel step, in 5/6/5
 * levels. Those two are the error rule (worst step <= 1) and are measured
 * against the chain's real word, not argued from the criterion.
 *
 * ksn_render.c is included for its counters and statics (as
 * test_coverage_runs.c does); do NOT also compile ksn_render.c into this
 * binary. Built WITHOUT -DKSN_COUNT_VISIBLE it prints only the scene hashes:
 * the instrumented and the shipping build must print the same numbers, which
 * is how "the counters change no pixel" is shown rather than claimed.
 *
 * Scenes, 120 frames each (REPLACE every 8th frame, PATCH otherwise):
 *   demo   - the 11-command scene test_decode_reuse.c renders (two dithered
 *            gradients, two texts, a two-child group, round rects, a stroke);
 *   ui     - the demo.js-shaped scene test_coverage_runs.c renders (dithered
 *            header, two texts, two rects in a group, round rects, a
 *            non-dithered footer gradient, a stroke);
 *   modal  - an overlay panel over a backdrop with its own text, two children
 *            in a group: the shape a modal or a notice has;
 *   redraw - the favourable case, deliberately: a translucent panel and a text
 *            drawn twice per frame, so the second draw's destination is very
 *            nearly its own source. This is the upper end a threshold skip can
 *            reach, and it is reported separately for that reason;
 *   demo-edge - `demo` again, with the ink distribution a real anti-aliased
 *            glyph has (most non-zero coverage is a small edge value) instead
 *            of the spread test_decode_reuse.c uses. The device jpfont face is
 *            not available to a host build, so the text arm is reported under
 *            both distributions rather than one. */
#include "core_fixture.h"
#include "ksn_render.c"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"visible skip line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define SCRIT(x) do{if(!(x)){fprintf(stderr,"visible skip line %d: %s\n",__LINE__,#x);return -1;}}while(0)

#define FRAMES 120u
#define MIN_PIXELS 100000u /* the corpus must not be vacuous */

static uint16_t strip[240*8],panel[240*135];
static uint32_t frame_hash;
KSN_TEST_CORE(core,static);
static ksn_client app;
static ksn_tx tx;
static ksn_ref refs[16];
static ksn_render_stats stats;

static uint16_t *get_strip(void *ctx){(void)ctx;return strip;}
static ksn_result send_strip(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));return KSN_OK;
}
/* Ink depends on the counted text bytes, on reveal, and on the clip, so a
 * stale cached view or a wrong reveal changes ink and not just a count. */
static const uint8_t ink_spread[]={0,1,63,127,128,254,255};
static const uint8_t ink_edges[]={0,1,2,3,4,8,16,32,96,255};
static unsigned ink_edges_mode;
static uint8_t ink(int x,int y,uint16_t reveal,const ksn_draw *d){
    if(x<d->bounds.x0||x>=d->bounds.x1||y<d->bounds.y0||y>=d->bounds.y1||
       x<d->clip.x0||x>=d->clip.x1||y<d->clip.y0||y>=d->clip.y1)return 0;
    unsigned mask=0;
    for(unsigned i=0;i<d->data.text.bytes;i++)mask=mask*31u+(unsigned char)d->data.text.utf8[i];
    unsigned slot=(mask+(unsigned)x*3u+(unsigned)y*5u+reveal*7u)%7u;
    if(ink_edges_mode)return ink_edges[(slot*3u+(unsigned)(x+y))%10u];
    return ink_spread[slot];
}
static ksn_result span(void *ctx,const ksn_draw *d,uint16_t reveal,int x,int y,
                       unsigned count,uint8_t *out){
    (void)ctx;
    if(!d->data.text.utf8)return KSN_INVALID; /* the preflight must address text */
    for(unsigned i=0;i<count;i++)out[i]=ink(x+(int)i,y,reveal,d);
    return KSN_OK;
}
static ksn_text_port text_port={.span=span};
static ksn_display_port display={NULL,get_strip,send_strip,240,135,8,&text_port,NULL};
static uint32_t hash_panel(void){
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<240*135;i++){hash^=panel[i];hash*=16777619u;}
    return hash;
}
static int render_frame(void){
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    frame_hash=hash_panel();
    return 0;
}
static char glyphs_in[40];
static void glyphs(unsigned scene,const char *prefix){
    static const char set[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -#";
    unsigned at=0;
    while(prefix[at]){glyphs_in[at]=prefix[at];at++;}
    for(unsigned i=0;i<24;i++)glyphs_in[at+i]=set[(scene*7u+i*3u)%(sizeof(set)-1u)];
}

/* ---- scene: demo (11 commands, the 2a/decode scene) ----------------------- */
#define DEMO_COMMANDS 11u
static void demo_build(unsigned scene,ksn_draw *d){
    memset(d,0,sizeof(ksn_draw)*DEMO_COMMANDS);
    d[0]=(ksn_draw){.kind=KSN_RECT,.bounds={(int16_t)(4u+scene%16u),3,150,45},.clip={0,0,240,135},
                    .opacity=201,.data.shape={0xd9867243,0,0}};
    d[1]=(ksn_draw){.kind=KSN_GRADIENT,.bounds={9,5,138,40},.clip={11,7,130,34},.opacity=219,
                    .data.gradient={0x25384900,0xe0a972d2,0,5,true}};
    d[2]=(ksn_draw){.kind=KSN_TEXT,.bounds={6,6,120,20},.clip={0,0,240,135},.opacity=255,
                    .data.text={glyphs_in,24,80,KSN_BODY,0xa15f37b7}};
    d[3]=(ksn_draw){.kind=KSN_TEXT,.bounds={10,24,132,40},.clip={0,0,240,135},.opacity=177,
                    .data.text={glyphs_in,24,80,KSN_CAPTION,0x3f7fbfc7}};
    d[4]=(ksn_draw){.kind=KSN_RECT,.bounds={30,12,42,27},.clip={0,0,240,135},.opacity=255,
                    .data.shape={0x708ca4ff,0,0}};
    d[5]=(ksn_draw){.kind=KSN_ROUND_RECT,.bounds={38,10,90,36},.clip={0,0,240,135},.opacity=177,
                    .data.shape={(0x357ecb00u|(scene*5u&0xffu)),6,0}};
    d[6]=(ksn_draw){.kind=KSN_ROUND_RECT,.bounds={44,16,96,40},.clip={0,0,240,135},.opacity=147,
                    .data.shape={0xcbb57e93,3,0}};
    d[7]=(ksn_draw){.kind=KSN_STROKE,.bounds={100,8,140,44},.clip={0,0,240,135},.opacity=255,
                    .data.shape={0x2f6f4fd1,0,1}};
    d[8]=(ksn_draw){.kind=KSN_RECT,.bounds={146,3,236,60},.clip={0,0,240,135},.opacity=255,
                    .data.shape={0x9b6c2aff,0,0}};
    d[9]=(ksn_draw){.kind=KSN_GRADIENT,.bounds={0,46,178,96},
                    .clip={0,(int16_t)(scene%8u),240,135},.opacity=255,
                    .data.gradient={0x123456ff,0xffff80ff,0,0,true}};
    d[10]=(ksn_draw){.kind=KSN_ROUND_RECT,.bounds={150,70,238,132},.clip={0,0,240,135},
                     .opacity=255,.data.shape={0x4080c0ff,8,0}};
}
static int frame_demo(unsigned frame){
    ksn_draw d[DEMO_COMMANDS];
    bool replace=(frame%8u)==0u;
    glyphs(frame,"");
    demo_build(frame,d);
    SCRIT(app.ops->begin(app.ctx,replace?KSN_REPLACE:KSN_PATCH,&tx)==KSN_OK);
    if(replace){
        SCRIT(app.ops->background(app.ctx,tx,0x2b4b6bff)==KSN_OK);
        for(unsigned i=0;i<DEMO_COMMANDS;i++)SCRIT(app.ops->add(app.ctx,tx,&d[i],&refs[i])==KSN_OK);
    }else{
        ksn_change change={.property=KSN_SET_RECT,.value.rect=d[0].bounds};
        SCRIT(app.ops->change(app.ctx,tx,refs[0],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_COLOR,.value.color=d[5].data.shape.color};
        SCRIT(app.ops->change(app.ctx,tx,refs[5],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_VISIBLE,.value.visible=(frame/5u)%2u==0};
        SCRIT(app.ops->change(app.ctx,tx,refs[6],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_REVEAL,.value.reveal=(uint16_t)(frame%24u)};
        SCRIT(app.ops->change(app.ctx,tx,refs[2],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_TEXT,.value.text={glyphs_in,24}};
        SCRIT(app.ops->change(app.ctx,tx,refs[3],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_CLIP,.value.rect=d[9].clip};
        SCRIT(app.ops->change(app.ctx,tx,refs[9],&change)==KSN_OK);
    }
    SCRIT(ksn_core_group(&core,KSN_APP,tx,refs[4],2,(uint8_t)(64u+frame%128u))==KSN_OK);
    SCRIT(app.ops->end(app.ctx,tx)==KSN_OK);
    return render_frame();
}

/* ---- scene: ui (demo.js shaped, 9 commands) ------------------------------ */
#define UI_COMMANDS 9u
static char ui_title[16]="Kasane demo";
static char ui_counter[24]="tick 0";
static void ui_build(unsigned frame,ksn_draw *d){
    memset(d,0,sizeof(ksn_draw)*UI_COMMANDS);
    d[0]=(ksn_draw){.kind=KSN_GRADIENT,.bounds={0,0,240,18},.clip={0,0,240,135},.opacity=255,
                    .data.gradient={0x0d2940ff,0x498781ff,0,0,true}};
    d[1]=(ksn_draw){.kind=KSN_TEXT,.bounds={8,3,232,17},.clip={0,0,240,135},.opacity=255,
                    .data.text={ui_title,11,64,KSN_BODY,0xffffffff}};
    d[2]=(ksn_draw){.kind=KSN_RECT,.bounds={12,28,112,56},.clip={0,0,240,135},.opacity=216,
                    .data.shape={0x164c70d8,0,0}};
    d[3]=(ksn_draw){.kind=KSN_RECT,.bounds={34,36,134,64},.clip={0,0,240,135},.opacity=172,
                    .data.shape={0x65d7bcac,0,0}};
    d[4]=(ksn_draw){.kind=KSN_ROUND_RECT,.bounds={12+(int16_t)(frame*3u%184u),76,
                                                 32+(int16_t)(frame*3u%184u),96},
                    .clip={0,0,240,135},.opacity=210,.data.shape={0xf5bd4fff,6,0}};
    d[5]=(ksn_draw){.kind=KSN_ROUND_RECT,.bounds={12,119,12+(int16_t)(12u+frame*5u%208u),127},
                    .clip={0,0,240,135},.opacity=255,.data.shape={0x62e0a8ff,3,0}};
    d[6]=(ksn_draw){.kind=KSN_STROKE,.bounds={10,117,232,129},.clip={0,0,240,135},.opacity=170,
                    .data.shape={0x80b5cfaa,0,1}};
    d[7]=(ksn_draw){.kind=KSN_TEXT,.bounds={12,100,180,113},.clip={0,0,240,135},.opacity=255,
                    .data.text={ui_counter,6,24,KSN_CAPTION,0xa8d8efff}};
    d[8]=(ksn_draw){.kind=KSN_GRADIENT,.bounds={0,104,240,116},
                    .clip={(int16_t)(frame%20u),0,240,135},.opacity=255,
                    .data.gradient={0x203040ff,0x40a0c0ff,1,0,false}};
}
static int frame_ui(unsigned frame){
    ksn_draw d[UI_COMMANDS];
    bool replace=(frame%8u)==0u;
    snprintf(ui_counter,sizeof(ui_counter),"tick %u",frame);
    unsigned chars=(frame/4u)%6u+4u; /* a PATCH that rewrites only some bytes */
    if(chars>strlen(ui_counter))chars=(unsigned)strlen(ui_counter);
    ui_build(frame,d);
    SCRIT(app.ops->begin(app.ctx,replace?KSN_REPLACE:KSN_PATCH,&tx)==KSN_OK);
    if(replace){
        SCRIT(app.ops->background(app.ctx,tx,0x071425ff)==KSN_OK);
        for(unsigned i=0;i<UI_COMMANDS;i++)SCRIT(app.ops->add(app.ctx,tx,&d[i],&refs[i])==KSN_OK);
    }else{
        ksn_change change={.property=KSN_SET_RECT,.value.rect=d[4].bounds};
        SCRIT(app.ops->change(app.ctx,tx,refs[4],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_RECT,.value.rect=d[5].bounds};
        SCRIT(app.ops->change(app.ctx,tx,refs[5],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_COLOR,
                            .value.color=(frame&16)?0x295d86d8u:0x164c70d8u};
        SCRIT(app.ops->change(app.ctx,tx,refs[2],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_REVEAL,.value.reveal=(uint16_t)(frame%11u)};
        SCRIT(app.ops->change(app.ctx,tx,refs[1],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_TEXT,.value.text={ui_counter,(uint16_t)chars}};
        SCRIT(app.ops->change(app.ctx,tx,refs[7],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_CLIP,.value.rect=d[8].clip};
        SCRIT(app.ops->change(app.ctx,tx,refs[8],&change)==KSN_OK);
    }
    SCRIT(ksn_core_group(&core,KSN_APP,tx,refs[2],2,208)==KSN_OK);
    SCRIT(app.ops->end(app.ctx,tx)==KSN_OK);
    return render_frame();
}

/* ---- scene: modal (an overlay panel over a backdrop) --------------------- */
#define MODAL_COMMANDS 5u
static char modal_text[24]="Are you sure?";
static void modal_build(unsigned frame,ksn_draw *d){
    memset(d,0,sizeof(ksn_draw)*MODAL_COMMANDS);
    d[0]=(ksn_draw){.kind=KSN_RECT,.bounds={0,0,240,135},.clip={0,0,240,135},.opacity=255,
                    .data.shape={0x2a3d52ff,0,0}};
    d[1]=(ksn_draw){.kind=KSN_ROUND_RECT,.bounds={28,27,212,109},.clip={0,0,240,135},
                    .opacity=(uint8_t)(204u+frame%24u),.data.shape={0x1c3043d8,8,0}};
    d[2]=(ksn_draw){.kind=KSN_RECT,.bounds={40,80,200,88},.clip={0,0,240,135},.opacity=230,
                    .data.shape={0x2b4055e0,0,0}};
    d[3]=(ksn_draw){.kind=KSN_TEXT,.bounds={36,40,204,58},.clip={0,0,240,135},.opacity=255,
                    .data.text={modal_text,13,48,KSN_BODY,0xdce8efff}};
    d[4]=(ksn_draw){.kind=KSN_ROUND_RECT,.bounds={180,90,210,110},.clip={0,0,240,135},
                    .opacity=200,.data.shape={0x63d7bccc,4,0}};
}
static int frame_modal(unsigned frame){
    ksn_draw d[MODAL_COMMANDS];
    bool replace=(frame%8u)==0u;
    modal_build(frame,d);
    SCRIT(app.ops->begin(app.ctx,replace?KSN_REPLACE:KSN_PATCH,&tx)==KSN_OK);
    if(replace){
        SCRIT(app.ops->background(app.ctx,tx,0x2b4b6bff)==KSN_OK);
        for(unsigned i=0;i<MODAL_COMMANDS;i++)SCRIT(app.ops->add(app.ctx,tx,&d[i],&refs[i])==KSN_OK);
    }else{
        ksn_change change={.property=KSN_SET_RECT,.value.rect=d[2].bounds};
        SCRIT(app.ops->change(app.ctx,tx,refs[2],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_COLOR,.value.color=d[1].data.shape.color};
        SCRIT(app.ops->change(app.ctx,tx,refs[1],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_REVEAL,.value.reveal=(uint16_t)(frame%13u)};
        SCRIT(app.ops->change(app.ctx,tx,refs[3],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_VISIBLE,.value.visible=(frame%11u)!=3u};
        SCRIT(app.ops->change(app.ctx,tx,refs[4],&change)==KSN_OK);
    }
    SCRIT(ksn_core_group(&core,KSN_APP,tx,refs[1],2,224)==KSN_OK);
    SCRIT(app.ops->end(app.ctx,tx)==KSN_OK);
    return render_frame();
}

/* ---- scene: redraw (the favourable case, on purpose) --------------------- */
#define REDRAW_COMMANDS 5u
static char redraw_text[24]="redrawn panel";
static void redraw_build(unsigned frame,ksn_draw *d){
    (void)frame;
    memset(d,0,sizeof(ksn_draw)*REDRAW_COMMANDS);
    d[0]=(ksn_draw){.kind=KSN_RECT,.bounds={0,0,240,135},.clip={0,0,240,135},.opacity=255,
                    .data.shape={0x1b2b3bff,0,0}};
    /* The same translucent panel and the same text, twice. The second draw's
     * destination is the first draw's own output, which is the case a
     * threshold skip exists for. */
    for(unsigned copy=0;copy<2;copy++){
        d[1+copy]=(ksn_draw){.kind=KSN_ROUND_RECT,.bounds={20,20,220,115},
                             .clip={0,0,240,135},.opacity=200,.data.shape={0x33507aff,8,0}};
        d[3+copy]=(ksn_draw){.kind=KSN_TEXT,.bounds={28,32,212,50},.clip={0,0,240,135},
                             .opacity=255,.data.text={redraw_text,13,32,KSN_BODY,0xf0f4f8ff}};
    }
}
static int frame_redraw(unsigned frame){
    ksn_draw d[REDRAW_COMMANDS];
    bool replace=(frame%8u)==0u;
    (void)frame;
    redraw_build(frame,d);
    SCRIT(app.ops->begin(app.ctx,replace?KSN_REPLACE:KSN_PATCH,&tx)==KSN_OK);
    if(replace){
        SCRIT(app.ops->background(app.ctx,tx,0x1b2b3bff)==KSN_OK);
        for(unsigned i=0;i<REDRAW_COMMANDS;i++)SCRIT(app.ops->add(app.ctx,tx,&d[i],&refs[i])==KSN_OK);
    }else{
        ksn_change change={.property=KSN_SET_REVEAL,.value.reveal=(uint16_t)(frame%9u)};
        SCRIT(app.ops->change(app.ctx,tx,refs[3],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_REVEAL,.value.reveal=(uint16_t)(frame%9u)};
        SCRIT(app.ops->change(app.ctx,tx,refs[4],&change)==KSN_OK);
    }
    SCRIT(app.ops->end(app.ctx,tx)==KSN_OK);
    return render_frame();
}

/* ---- reporting ----------------------------------------------------------- */
#ifdef KSN_COUNT_VISIBLE
static const char *kind_names[KSN_VIS_KINDS]={"TEXT","RECT","ROUND_RECT","STROKE","GRADIENT","GROUP"};
static const char *arm_names[KSN_VIS_ARMS]={"step","bound","both","exact"};
static ksn_visible_slot corpus[KSN_VIS_KINDS*2];
static uint64_t corpus_pixels;
/* The error rule per slot: a non-dithering skip uses `both`, a dithering one
 * only `exact`, and neither may move a pixel by more than one channel step.
 * These are the matches the arms actually wrote against the chain's word. */
static int check_slot(const char *scene,const char *kind,unsigned dith,const ksn_visible_slot *s){
    if(!dith&&s->worst[2]>1u){
        fprintf(stderr,"%s: %s dither=0 both arm moved a pixel %u steps\n",
                scene,kind,(unsigned)s->worst[2]);return 1;
    }
    if(s->worst[3]>1u){
        fprintf(stderr,"%s: %s dither=%u exact arm moved a pixel %u steps\n",
                scene,kind,dith,(unsigned)s->worst[3]);return 1;
    }
    CHECK(s->sel[0]<=s->pixels&&s->sel[1]<=s->pixels&&s->sel[2]<=s->pixels);
    CHECK(s->sel[3]<=s->sel[0]&&s->moved[2]<=s->sel[2]&&s->moved[3]<=s->sel[3]);
    CHECK(s->exact==s->sel[3]&&s->chain_moved<=s->pixels);
    return 0;
}
static void add_slot(unsigned slot,const ksn_visible_slot *s){
    ksn_visible_slot *t=&corpus[slot];
    t->empty+=s->empty;t->pixels+=s->pixels;t->exact+=s->exact;
    t->chain_moved+=s->chain_moved;
    if(s->chain_worst>t->chain_worst)t->chain_worst=s->chain_worst;
    for(unsigned n=0;n<KSN_VIS_ARMS;n++){
        t->sel[n]+=s->sel[n];t->moved[n]+=s->moved[n];
        if(s->worst[n]>t->worst[n])t->worst[n]=s->worst[n];
    }
}
#endif

static int run_scene(const char *name,int (*frame)(unsigned),unsigned edges,uint64_t *pixels){
    ink_edges_mode=edges;
    *pixels=0;
    memset(panel,0,sizeof(panel));memset(strip,0,sizeof(strip));
    ksn_core_init(&core);app=ksn_core_client(&core,KSN_APP);
#ifdef KSN_COUNT_VISIBLE
    ksn_visible_reset();
#endif
    uint32_t rolling=2166136261u;
    for(unsigned f=0;f<FRAMES;f++){
        if(frame(f))return 1;
        rolling=rolling*31u+frame_hash;
    }
    printf("scene %-5s frames=%u ink=%s hash=%08x\n",name,(unsigned)FRAMES,
           edges?"edges":"spread",rolling);
#ifdef KSN_COUNT_VISIBLE
    {
        uint64_t total=0,sel[KSN_VIS_ARMS]={0,0,0,0},moved[KSN_VIS_ARMS]={0,0,0,0},empty=0;
        unsigned worst[KSN_VIS_ARMS]={0,0,0,0},chain_worst=0;
        for(unsigned kind=0;kind<KSN_VIS_KINDS;kind++)for(unsigned dith=0;dith<2;dith++){
            const ksn_visible_slot *s=&g_ksn_visible[kind*2u+dith];
            if(!s->pixels)continue;
            printf("  kind=%-10s dither=%u pixels=%-8" PRIu64,kind_names[kind],dith,s->pixels);
            for(unsigned n=0;n<KSN_VIS_ARMS;n++)
                printf(" %s=%.1f%%",arm_names[n],100.0*(double)s->sel[n]/(double)s->pixels);
            printf(" moved_both=%" PRIu64 " worst_both=%" PRIu64 " moved_exact=%" PRIu64
                   " worst_exact=%" PRIu64 " chain_worst=%" PRIu64 "\n",
                   s->moved[2],s->worst[2],s->moved[3],s->worst[3],s->chain_worst);
            if(check_slot(name,kind_names[kind],dith,s))return 1;
            total+=s->pixels;empty+=s->empty;
            for(unsigned n=0;n<KSN_VIS_ARMS;n++){
                sel[n]+=s->sel[n];moved[n]+=s->moved[n];
                if(s->worst[n]>worst[n])worst[n]=s->worst[n];
            }
            if(s->chain_worst>chain_worst)chain_worst=s->chain_worst;
            add_slot(kind*2u+dith,s);
        }
        printf("  scene %-5s TOTAL pixels=%" PRIu64 " (empty=%" PRIu64 " already free): "
               "step=%.1f%% bound=%.1f%% both=%.1f%% exact=%.1f%%; moved(both)=%" PRIu64
               " worst(both)=%u moved(exact)=%" PRIu64 " worst(exact)=%u chain_worst=%u\n",
               name,total,empty,
               100.0*(double)sel[0]/(double)total,100.0*(double)sel[1]/(double)total,
               100.0*(double)sel[2]/(double)total,100.0*(double)sel[3]/(double)total,
               moved[2],worst[2],moved[3],worst[3],chain_worst);
        *pixels=total;
    }
#else
    printf("  (no counters in this tree: hashes only)\n");
#endif
    return 0;
}

int main(void){
    uint64_t pixels=0,total=0;
    /* The counters sit inside the chain (blend, group_over). One arm added after
     * this harness was written replaces those calls for some pixels: the
     * quantized-key table (g_ksn_blend_lut / _alpha) answers a constant-colour
     * command without entering blend at all, so with the shipping default the
     * solid kinds (RECT/ROUND_RECT/STROKE) count zero pixels and this harness
     * stops seeing them -- the corpus check below catches exactly that. The
     * question here is the threshold's hit rate ON THE CHAIN, so the table is
     * turned off and the counters see every pixel the chain would have taken.
     * The row table stays on: it replaces sample(), not the chain, so the
     * counted population is unchanged.
     * It is turned off in BOTH builds on purpose: the instrumented and the
     * shipping build are compared scene hash for scene hash ("the counters
     * change no pixel"), and that comparison has to run the same arms. */
    g_ksn_blend_lut=0;g_ksn_blend_lut_alpha=0;
    static const struct { const char *name; int (*frame)(unsigned); unsigned edges; } scenes[]={
        {"demo",frame_demo,0},{"ui",frame_ui,0},{"modal",frame_modal,0},
        {"redraw",frame_redraw,0},{"demo-edge",frame_demo,1}};
    for(unsigned i=0;i<sizeof(scenes)/sizeof(scenes[0]);i++){
        CHECK(run_scene(scenes[i].name,scenes[i].frame,scenes[i].edges,&pixels)==0);
        total+=pixels;
    }
#ifdef KSN_COUNT_VISIBLE
    CHECK(total>MIN_PIXELS);
    /* The corpus has to contain every kind somewhere, a non-dithered gradient
     * among them, or the tables would report on scenes that never ran it. */
    for(unsigned kind=0;kind<KSN_VIS_KINDS;kind++)
        CHECK(corpus[kind*2u].pixels+corpus[kind*2u+1u].pixels>0);
    CHECK(corpus[KSN_VIS_GRADIENT*2u].pixels>0);
    CHECK(corpus[KSN_VIS_GRADIENT*2u+1u].pixels>0);
    {
        uint64_t sel[KSN_VIS_ARMS]={0,0,0,0},moved[KSN_VIS_ARMS]={0,0,0,0},empty=0;
        unsigned worst[KSN_VIS_ARMS]={0,0,0,0},chain_worst=0;
        for(unsigned slot=0;slot<KSN_VIS_KINDS*2;slot++){
            const ksn_visible_slot *s=&corpus[slot];
            if(!s->pixels)continue;
            empty+=s->empty;
            for(unsigned n=0;n<KSN_VIS_ARMS;n++){
                sel[n]+=s->sel[n];moved[n]+=s->moved[n];
                if(s->worst[n]>worst[n])worst[n]=s->worst[n];
            }
            if(s->chain_worst>chain_worst)chain_worst=s->chain_worst;
        }
        corpus_pixels=total;
        printf("CORPUS %u scenes x %u frames, %" PRIu64 " blend pixels (empty=%" PRIu64
               " already free): step=%.1f%% bound=%.1f%% both=%.1f%% exact=%.1f%%; "
               "moved(both)=%" PRIu64 " worst(both)=%u moved(exact)=%" PRIu64
               " worst(exact)=%u chain_worst=%u\n",
               (unsigned)(sizeof(scenes)/sizeof(scenes[0])),(unsigned)FRAMES,total,empty,
               100.0*(double)sel[0]/(double)total,100.0*(double)sel[1]/(double)total,
               100.0*(double)sel[2]/(double)total,100.0*(double)sel[3]/(double)total,
               moved[2],worst[2],moved[3],worst[3],chain_worst);
        for(unsigned kind=0;kind<KSN_VIS_KINDS;kind++)for(unsigned dith=0;dith<2;dith++){
            const ksn_visible_slot *s=&corpus[kind*2u+dith];
            if(!s->pixels)continue;
            printf("  CORPUS kind=%-10s dither=%u pixels=%-8" PRIu64,kind_names[kind],dith,s->pixels);
            for(unsigned n=0;n<KSN_VIS_ARMS;n++)
                printf(" %s=%.1f%%",arm_names[n],100.0*(double)s->sel[n]/(double)s->pixels);
            printf(" moved_both=%" PRIu64 " worst_both=%" PRIu64 " chain_worst=%" PRIu64 "\n",
                   s->moved[2],s->worst[2],s->chain_worst);
        }
    }
#endif
    return 0;
}
