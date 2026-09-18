/* Candidate 3c of docs/perf/kasane-opt-survey.md (boundary 3 and 4), step 1:
 * a group whose opacity is 255 and whose children are all opaque folds its layer
 * chain into a single affine map per group -- out = A*src + B*dst + C with
 * A = 255 and B = C = 0 on every channel, applied as one store per covered
 * pixel. See docs/perf/kasane-group-affine.md.
 *
 * ksn_render.c is included for its statics: `group_opaque_row` is the folded
 * arm's per-row entry, and the number of entries into it is how this file shows
 * the fold actually ran. A switch that silently never matches would make every
 * "identical" comparison below meaningless, so the non-foldable scenes are
 * checked for zero entries, not just for equal pixels. ksn_core.c and
 * ksn_cache.c are linked as usual; do NOT also compile ksn_render.c in.
 *
 * Three proofs, all host-only:
 *   1. whole panel against an independent scalar reference that composites the
 *      scene itself (it calls no renderer helper): both arms must agree with it
 *      pixel for pixel on the opaque scenes, so "the arms agree" cannot mean
 *      "both are wrong";
 *   2. the two arms against each other over the parameter space that reaches
 *      tile edges, band edges, every Bayer phase, all four child kinds, clips,
 *      hidden children, zero-opacity children, and the scenes that must stay OUT
 *      of the folded arm (a non-opaque child, a non-opaque group, a TEXT child,
 *      a colour alpha of 254);
 *   3. 120 frames of a demo.js-shaped scene under both arms, compared per frame
 *      as FNV-1a hashes of the 240x135 RGB565 panel, with PATCH/full and
 *      repeated-placement invariance -- ksn_cache.h:54-55 leaves opacity-255
 *      instances on the isolated chain precisely to keep rounding stable under
 *      PATCH, and that is the contract this file has to keep if the fold owns
 *      such a group.
 *
 * Built with -DKSN_AFFINE_COUNT it counts entries into `group_opaque_row`
 * through -finstrument-functions: the fold's before/after numbers are entries,
 * not estimates, and the switch-off arm must count zero. */
#include "core_fixture.h"
#include "ksn_cache.h"
#include "ksn_render.c"
#include <stdio.h>
#include <string.h>
#ifdef KSN_AFFINE_COUNT
#include <inttypes.h>
static uint64_t fold_rows[2],fold_rows_scene;
/* -finstrument-functions hands over the callee's own address, so the counter
 * cannot drift from the code it claims to measure. */
void __cyg_profile_func_enter(void *,void *) __attribute__((no_instrument_function));
void __cyg_profile_func_enter(void *this_fn,void *call_site);
void __cyg_profile_func_enter(void *this_fn,void *call_site){
    (void)call_site;
    if(this_fn==(void *)&group_opaque_row)fold_rows_scene++;
}
#endif

#define CHECK(x) do{if(!(x)){fprintf(stderr,"group affine line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
#ifdef KSN_AFFINE_COUNT
/* Claim about the *last* render: 1 = it entered the folded row, 0 = it did not.
 * A no-op in the plain build, where nothing counts. The count arm is what keeps
 * the "identical panels" claim from being vacuously true. */
#define folded(want) CHECK((want)?(fold_rows_scene>0):(fold_rows_scene==0))
#else
#define folded(want) do{(void)(want);}while(0)
#endif

/* ---- the scene under test ------------------------------------------------- */
#define SCENE_MAX 10
KSN_TEST_CORE(core,static);
static uint16_t strip[240*8],panel[240*135],left_arm[240*135],right_arm[240*135];
static uint16_t pred_tile[240*135],pred_fold[240*135];
static ksn_draw scene[SCENE_MAX];
static bool visible[SCENE_MAX];
static uint32_t background;
static unsigned scene_count,frames_full;
static const char title_text[]="Kasane";

/* ---- independent scalar reference ---------------------------------------- */
/* Same shape as test_group_dither.c's reference: it reads the scene, not the
 * renderer, and reproduces the documented chain (premultiplied `over` into a
 * tile whose alpha starts at 0, then the group's opacity against the expanded
 * 565 background). Nothing here calls covers/coverage_runs/sample/mul8. */
static const ksn_draw *ref_scene;
static const bool *ref_visible;
static uint8_t ref_opacity;
static uint32_t ref_background;
static unsigned ref_count,ref_dither_differences,ref_dithered_pixels;

static bool covered(const ksn_draw *d,int x,int y){
    if(x<d->clip.x0||x>=d->clip.x1||y<d->clip.y0||y>=d->clip.y1||
       x<d->bounds.x0||x>=d->bounds.x1||y<d->bounds.y0||y>=d->bounds.y1)return false;
    if(d->kind==KSN_STROKE){
        int w=d->data.shape.width;
        return x-d->bounds.x0<w||d->bounds.x1-x<=w||y-d->bounds.y0<w||d->bounds.y1-y<=w;
    }
    unsigned radius=d->kind==KSN_GRADIENT?d->data.gradient.radius:d->data.shape.radius;
    double dx=radius-(x-d->bounds.x0+0.5),far_x=radius-(d->bounds.x1-x-0.5);
    double dy=radius-(y-d->bounds.y0+0.5),far_y=radius-(d->bounds.y1-y-0.5);
    if(dx<far_x)dx=far_x;
    if(dy<far_y)dy=far_y;
    if(dx<0)dx=0;
    if(dy<0)dy=0;
    return dx*dx+dy*dy<=radius*radius;
}
static unsigned product(unsigned a,unsigned b){return (a*b+127)/255;}
static unsigned limited(unsigned value){return value<256?value:255;}
static unsigned component(uint32_t color,unsigned c){return (color>>(24-c*8))&255;}
static void sample_rgba(const ksn_draw *d,int x,int y,unsigned out[4]){
    if(d->kind!=KSN_GRADIENT){
        for(unsigned c=0;c<4;c++)out[c]=component(d->data.shape.color,c);
        return;
    }
    int n=d->data.gradient.axis?d->bounds.y1-d->bounds.y0:d->bounds.x1-d->bounds.x0;
    int at=d->data.gradient.axis?y-d->bounds.y0:x-d->bounds.x0;
    for(unsigned c=0;c<4;c++){
        unsigned first=component(d->data.gradient.from,c),last=component(d->data.gradient.to,c);
        out[c]=n<=1?first:((unsigned)(n-1-at)*first+(unsigned)at*last+(unsigned)(n-1)/2)/(unsigned)(n-1);
    }
}
static unsigned threshold(unsigned x,unsigned y){
    return 4*(2*((x^y)&1)+(y&1))+2*(((x>>1)^(y>>1))&1)+((y>>1)&1);
}
static unsigned quantized(unsigned channel_value,unsigned maximum,unsigned t){
    return (32*channel_value*maximum+(31-2*t)*255-1)/(32*255);
}
static uint16_t reference(int x,int y,bool enable_dither){
    unsigned p[4]={0,0,0,0};bool marked=false;
    for(unsigned i=0;i<ref_count;i++){
        if(!ref_visible[i]||!covered(&ref_scene[i],x,y))continue;
        unsigned rgba[4];sample_rgba(&ref_scene[i],x,y,rgba);
        unsigned a=product(rgba[3],ref_scene[i].opacity);
        if(a==255)marked=false;
        if(a&&ref_scene[i].kind==KSN_GRADIENT&&ref_scene[i].data.gradient.dither)marked=true;
        for(unsigned c=0;c<3;c++)p[c]=limited(product(rgba[c],a)+product(p[c],255-a));
        p[3]=limited(a+product(p[3],255-a));
    }
    unsigned bg[3]={component(ref_background,0)>>3,component(ref_background,1)>>2,
                    component(ref_background,2)>>3};
    uint16_t original=(uint16_t)(bg[0]<<11|bg[1]<<5|bg[2]);
    unsigned alpha=product(p[3],ref_opacity);
    if(!alpha)return original;
    bg[0]=bg[0]*8+bg[0]/4;bg[1]=bg[1]*4+bg[1]/16;bg[2]=bg[2]*8+bg[2]/4;
    for(unsigned c=0;c<3;c++)p[c]=limited(product(p[c],ref_opacity)+product(bg[c],255-alpha));
    if(marked&&enable_dither){
        unsigned t=threshold((unsigned)x,(unsigned)y);
        return (uint16_t)(quantized(p[0],31,t)<<11|quantized(p[1],63,t)<<5|quantized(p[2],31,t));
    }
    return (uint16_t)((p[0]/8)<<11|(p[1]/4)<<5|(p[2]/8));
}
/* Against the scalar reference for the scene the caller installed in ref_*. */
static int against_reference(const uint16_t *got,const char *what,unsigned *mismatches){
    for(int y=0;y<135;y++)for(int x=0;x<240;x++){
        uint16_t expected=reference(x,y,true);
        if(expected!=reference(x,y,false)){ref_dither_differences++;ref_dithered_pixels++;}
        if(got[y*240+x]!=expected){
            if(*mismatches<4)
                fprintf(stderr,"%s: reference pixel %d,%d got %04x expected %04x\n",
                        what,x,y,got[y*240+x],expected);
            (*mismatches)++;
        }
    }
    return 0;
}
/* ---- the two arms -------------------------------------------------------- */
static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result transfer(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));return KSN_OK;
}
static unsigned coverage(int x,int y){
    static const unsigned values[]={0,1,127,128,254,255};
    if(x<2||x>=77||y<7||y>=22)return 0;
    return values[(x+y)%6];
}
static ksn_result span(void *port,const ksn_draw *d,uint16_t reveal,int x,int y,unsigned count,uint8_t *out){
    (void)port;(void)reveal;(void)d;
    for(unsigned i=0;i<count;i++)out[i]=(uint8_t)coverage(x+(int)i,y);
    return KSN_OK;
}
static const ksn_text_port text_port={.span=span};
static const ksn_display_port display={NULL,buffer,transfer,240,135,8,&text_port,NULL};

/* One REPLACE frame of `scene`, with the whole command list as one group. `arm`
 * is g_ksn_group_affine, so this is the same binary on both sides. */
static int render_scene(const ksn_draw *draws,const bool *vis,unsigned count,uint8_t opacity,
                        uint32_t back,int arm,uint16_t *out){
#ifdef KSN_AFFINE_COUNT
    fold_rows_scene=0;
#endif
    g_ksn_group_affine=arm;
    ref_scene=draws;ref_visible=vis;ref_count=count;ref_opacity=opacity;ref_background=back;
    ksn_core_init(&core);
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx tx;ksn_ref refs[SCENE_MAX];
    ksn_render_stats stats;
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,back)==KSN_OK);
    for(unsigned i=0;i<count;i++){
        CHECK(app.ops->add(app.ctx,tx,&draws[i],&refs[i])==KSN_OK);
        if(!vis[i]){
            ksn_change change={.property=KSN_SET_VISIBLE,.value.visible=false};
            CHECK(app.ops->change(app.ctx,tx,refs[i],&change)==KSN_OK);
        }
    }
    CHECK(ksn_core_group(&core,KSN_APP,tx,refs[0],(uint16_t)count,opacity)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(stats.transferred_bytes==64800);
#ifdef KSN_AFFINE_COUNT
    fold_rows[arm?1:0]+=fold_rows_scene;
#endif
    memcpy(out,panel,sizeof(panel));
    return 0;
}
static unsigned compare_panels(const uint16_t *a,const uint16_t *b,const char *what){
    unsigned differing=0;
    for(int y=0;y<135;y++)for(int x=0;x<240;x++)if(a[y*240+x]!=b[y*240+x]){
        if(differing<4)fprintf(stderr,"%s: arms differ at %d,%d: %04x vs %04x\n",
                               what,x,y,a[y*240+x],b[y*240+x]);
        differing++;
    }
    return differing;
}
static void translate(int dx,int dy){
    for(unsigned i=0;i<scene_count;i++){
        scene[i].bounds.x0+=(int16_t)dx;scene[i].bounds.x1+=(int16_t)dx;
        scene[i].bounds.y0+=(int16_t)dy;scene[i].bounds.y1+=(int16_t)dy;
        scene[i].clip.x0+=(int16_t)dx;scene[i].clip.x1+=(int16_t)dx;
        scene[i].clip.y0+=(int16_t)dy;scene[i].clip.y1+=(int16_t)dy;
    }
}
/* ---- 1+2: the foldable scene, its reference, and the arms ----------------- */
static unsigned compared_pixels,nonfold_pixels;
static int foldable_scenes(void){
    /* All four child kinds, overlapping, exactly opaque, plus a hidden child and
     * a zero-opacity child (both no-ops for the chain). */
    const ksn_draw base[]={
        {.kind=KSN_RECT,.bounds={3,3,147,47},.clip={0,0,240,135},.opacity=255,
         .data.shape={0xd98672ff,0,0}},
        {.kind=KSN_ROUND_RECT,.bounds={38,10,90,36},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x357ecbff,6,0}},
        {.kind=KSN_STROKE,.bounds={10,6,232,129},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x80b5cfff,0,2}},
        {.kind=KSN_GRADIENT,.bounds={9,5,138,40},.clip={11,7,130,34},.opacity=255,
         .data.gradient={0x253849ff,0xe0a972ff,1,0,true}},
        {.kind=KSN_GRADIENT,.bounds={80,6,145,44},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0xa9b67aff,0x789bc4ff,0,5,false}},
        {.kind=KSN_RECT,.bounds={100,20,130,40},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x123456ff,0,0}},
        {.kind=KSN_ROUND_RECT,.bounds={-6,90,60,140},.clip={2,92,58,133},.opacity=255,
         .data.shape={0xff00ffff,8,0}},
        {.kind=KSN_RECT,.bounds={180,-4,246,40},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x00ff88ff,0,0}},
        {.kind=KSN_GRADIENT,.bounds={4,60,236,120},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0xff0000ff,0x00ff00ff,0,0,true}},
        {.kind=KSN_RECT,.bounds={0,0,20,20},.clip={0,0,240,135},.opacity=0,
         .data.shape={0xffffffff,0,0}}}; /* a zero-opacity child: a no-op, and the
                                           precondition has to step over it (a
                                           zero-*alpha* child with opacity 255 is
                                           a different case: it is in the
                                           non-foldable set below) */
    unsigned mismatches=0;
    scene_count=(unsigned)COUNT(base);
    for(unsigned variant=0;variant<3;variant++){
        memcpy(scene,base,sizeof(base));
        background=0x315d7bff;
        for(unsigned i=0;i<scene_count;i++)visible[i]=true;
        if(variant==1)visible[6]=false;                 /* a hidden child */
        if(variant==2)scene[8].opacity=255;             /* ... and awake */
        for(int ty=0;ty<4;ty++)for(int tx=0;tx<4;tx++){
            memcpy(scene,base,sizeof(base));
            for(unsigned i=0;i<scene_count;i++)visible[i]=variant==1?i!=6:true;
            if(variant==2)scene[8].opacity=255;
            translate(tx,ty);
            CHECK(render_scene(scene,visible,scene_count,255,background,1,right_arm)==0);
            if(variant==0)folded(1);
            CHECK(render_scene(scene,visible,scene_count,255,background,0,left_arm)==0);
            folded(0);
            CHECK(compare_panels(left_arm,right_arm,"foldable")==0);
            CHECK(against_reference(left_arm,"foldable/old arm",&mismatches)==0);
            CHECK(against_reference(right_arm,"foldable/folded arm",&mismatches)==0);
            compared_pixels+=240u*135u;
            /* The other coverage solver: the folded arm's predicate form against
             * the tile chain with the same solver, and against the folded arm's
             * runs form. Both must agree or the fold is only correct on one
             * coverage arm. */
            if(variant==0&&ty==0&&tx==0){
                g_ksn_row_coverage=0;
                CHECK(render_scene(scene,visible,scene_count,255,background,1,pred_fold)==0);
                folded(1);
                CHECK(render_scene(scene,visible,scene_count,255,background,0,pred_tile)==0);
                folded(0);
                g_ksn_row_coverage=1;
                CHECK(compare_panels(pred_tile,pred_fold,"predicate coverage arm")==0);
                CHECK(compare_panels(right_arm,pred_fold,"coverage arms, fold on")==0);
                CHECK(against_reference(pred_fold,"predicate arm/folded",&mismatches)==0);
                compared_pixels+=2u*240u*135u;
            }
        }
        /* One origin that lands the box edges on band and tile boundaries. */
        memcpy(scene,base,sizeof(base));
        for(unsigned i=0;i<scene_count;i++)visible[i]=variant==1?i!=6:true;
        if(variant==2)scene[8].opacity=255;
        translate(-20,-12);
        CHECK(render_scene(scene,visible,scene_count,255,background,1,right_arm)==0);
        CHECK(render_scene(scene,visible,scene_count,255,background,0,left_arm)==0);
        CHECK(compare_panels(left_arm,right_arm,"foldable(-20,-12)")==0);
        CHECK(against_reference(right_arm,"foldable(-20,-12)",&mismatches)==0);
        compared_pixels+=240u*135u;
    }
    CHECK(mismatches==0);
    CHECK(ref_dither_differences>1000); /* the dither rule was actually exercised */
    /* The topmost child wins: an opaque child over a dithered gradient clears
     * the provenance, and the folded arm has to say the same. One child at a
     * time, at nine origins. */
    for(unsigned top=0;top<scene_count;top++){
        for(int ty=0;ty<3;ty++)for(int tx=0;tx<3;tx++){
            memcpy(scene,base,sizeof(base));
            for(unsigned i=0;i<scene_count;i++)visible[i]=i==top;
            translate(tx,ty);
            CHECK(render_scene(scene,visible,scene_count,255,background,1,right_arm)==0);
            CHECK(render_scene(scene,visible,scene_count,255,background,0,left_arm)==0);
            CHECK(compare_panels(left_arm,right_arm,"single child")==0);
            CHECK(against_reference(right_arm,"single child/folded arm",&mismatches)==0);
            compared_pixels+=240u*135u;
        }
    }
    CHECK(mismatches==0);
    return 0;
}
/* Scenes that must NOT fold: a non-opaque group, a non-opaque child, a colour
 * alpha of 254, a TEXT child. Each is compared between the arms and (with
 * -DKSN_AFFINE_COUNT) checked for zero entries into the folded row. */
static int nonfoldable_scenes(void){
    unsigned mismatches=0,differing=0;
    /* The same shapes with one alpha lowered, so the two scenes differ only in
     * the property the fold's precondition reads. */
    const ksn_draw one_child[]={
        {.kind=KSN_RECT,.bounds={20,20,120,100},.clip={0,0,240,135},.opacity=255,
         .data.shape={0xd98672ff,0,0}},
        {.kind=KSN_RECT,.bounds={40,30,100,80},.clip={0,0,240,135},.opacity=254,
         .data.shape={0x357ecbff,0,0}}};
    const ksn_draw alpha254[]={
        {.kind=KSN_RECT,.bounds={20,20,120,100},.clip={0,0,240,135},.opacity=255,
         .data.shape={0xd98672ff,0,0}},
        {.kind=KSN_RECT,.bounds={40,30,100,80},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x357ecbfe,0,0}}};
    const ksn_draw gradient254[]={
        {.kind=KSN_GRADIENT,.bounds={20,20,120,100},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0xff0000ff,0x00ff00fe,0,0,true}},
        {.kind=KSN_ROUND_RECT,.bounds={40,30,100,80},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x357ecbff,5,0}}};
    const ksn_draw alpha0[]={
        {.kind=KSN_RECT,.bounds={20,20,120,100},.clip={0,0,240,135},.opacity=255,
         .data.shape={0xd98672ff,0,0}},
        {.kind=KSN_RECT,.bounds={40,30,100,80},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x357ecb00,0,0}}};
    const ksn_draw with_text[]={
        {.kind=KSN_RECT,.bounds={20,20,120,100},.clip={0,0,240,135},.opacity=255,
         .data.shape={0xd98672ff,0,0}},
        {.kind=KSN_TEXT,.bounds={-3,5,145,29},.clip={2,7,77,22},.opacity=255,
         .data.text={.utf8=title_text,.bytes=sizeof(title_text)-1,.capacity=64,
                     .font=KSN_BODY,.color=0xa15f37ff}}};
    /* `reference` is false for the TEXT scene: the scalar reference below has
     * no text port and does not model coverage[i], so it cannot answer for ink.
     * The arms still have to agree there, which is what that case is for. */
    struct { const ksn_draw *draws; unsigned count; uint8_t opacity; const char *name;
             bool reference; } cases[]={
        {one_child,2,255,"child opacity 254",true},
        {alpha254,2,255,"child colour alpha 254",true},
        {gradient254,2,255,"gradient end alpha 254",true},
        {alpha0,2,255,"child colour alpha 0",false},
        {with_text,2,255,"TEXT child",false},
        {one_child,2,254,"group opacity 254",true},
        {one_child,2,0,"group opacity 0",true},
        {one_child,2,1,"group opacity 1",true}};
    for(unsigned c=0;c<COUNT(cases);c++){
        memcpy(scene,cases[c].draws,cases[c].count*sizeof(ksn_draw));
        scene_count=cases[c].count;
        for(unsigned i=0;i<scene_count;i++)visible[i]=true;
        for(int ty=0;ty<3;ty++)for(int tx=0;tx<3;tx++){
            memcpy(scene,cases[c].draws,cases[c].count*sizeof(ksn_draw));
            translate(tx,ty);
            CHECK(render_scene(scene,visible,scene_count,cases[c].opacity,0x0b1727ff,1,right_arm)==0);
            folded(0);
            CHECK(render_scene(scene,visible,scene_count,cases[c].opacity,0x0b1727ff,0,left_arm)==0);
            differing=compare_panels(left_arm,right_arm,cases[c].name);
            CHECK(differing==0);
            if(cases[c].reference)CHECK(against_reference(left_arm,cases[c].name,&mismatches)==0);
            nonfold_pixels+=240u*135u;
        }
    }
    CHECK(mismatches==0);
    return 0;
}
/* ---- 3: PATCH/full invariance and 120 frames ----------------------------- */
/* ---- step 2: what the approximate fold moves ----------------------------- */
/* The fold's second step drops the chain's 8-bit floors, so pixels can move.
 * This section measures that, in the same binary and against the pre-3c arm:
 * how many pixels, the worst channel step (8-bit, after expanding both 565
 * words) and the worst 565 step, and which pixels -- classified by the covering
 * children the scene itself names, because the claim is "a pixel can only move
 * under a child with a < 255, and step 2 is exact on the chains step 1 folds". */
static unsigned moved_pixels,approx_pixels,worst_step_r,worst_step_g,worst_step_b;
static unsigned worst_565,class_moved[4],class_pixels[4],nonopaque_moved,nonopaque_pixels;
static unsigned moved_unexplained;
static void unpack565(uint16_t value,unsigned out[3]){
    unsigned r=value>>11,g=(value>>5)&63,b=value&31;
    out[0]=(r<<3)|(r>>2);out[1]=(g<<2)|(g>>4);out[2]=(b<<3)|(b>>2);
}
/* The child's own alpha at this pixel as the chain defines it: coverage-scaled
 * for TEXT (which this reference cannot model, so TEXT scenes are not
 * classified), mul8(color alpha, child opacity) otherwise. */
static bool child_semi(const ksn_draw *d,int x,int y){
    if(d->kind==KSN_TEXT)return true; /* unmodelled: treat as if it could move */
    unsigned rgba[4];sample_rgba(d,x,y,rgba);
    return product(rgba[3],d->opacity)!=255;
}
static void measure_move(const uint16_t *old,const uint16_t *folded,bool classify,const char *what){
    static unsigned reported;
    for(int y=0;y<135;y++)for(int x=0;x<240;x++){
        uint16_t a=old[y*240+x],b=folded[y*240+x];
        unsigned covering=0;bool semi=false;
        if(classify){
            for(unsigned i=0;i<ref_count;i++)if(ref_visible[i]&&ref_scene[i].opacity&&
                                             covered(&ref_scene[i],x,y)){
                covering++;
                if(child_semi(&ref_scene[i],x,y))semi=true;
            }
            class_pixels[covering>2?3:covering]++;
            if(semi)nonopaque_pixels++;
        }
        if(a==b)continue;
        moved_pixels++;
        if(classify){
            unsigned cls=covering>2?3:covering;
            class_moved[cls]++;if(semi)nonopaque_moved++;else moved_unexplained++;
            if(!semi&&reported<4){reported++;fprintf(stderr,"%s: moved at %d,%d under %u opaque "
                "covering child(ren): %04x vs %04x\n",what,x,y,covering,a,b);}
        }
        unsigned pa[3],pb[3];unpack565(a,pa);unpack565(b,pb);
        unsigned dr=pa[0]>pb[0]?pa[0]-pb[0]:pb[0]-pa[0];
        unsigned dg=pa[1]>pb[1]?pa[1]-pb[1]:pb[1]-pa[1];
        unsigned db=pa[2]>pb[2]?pa[2]-pb[2]:pb[2]-pa[2];
        if(dr>worst_step_r)worst_step_r=dr;
        if(dg>worst_step_g)worst_step_g=dg;
        if(db>worst_step_b)worst_step_b=db;
        unsigned sa=a>>11,sb=b>>11,ga=(a>>5)&63,gb=(b>>5)&63,ba=a&31,bb=b&31;
        unsigned step=sa>sb?sa-sb:sb-sa;
        unsigned gstep=ga>gb?ga-gb:gb-ga;
        unsigned bstep=ba>bb?ba-bb:bb-ba;
        if(gstep>step)step=gstep;
        if(bstep>step)step=bstep;
        if(step>worst_565)worst_565=step;
    }
}
static unsigned approximate_pixels;
static int approx_scenes(void){
    /* The chains that step 1 folds: step 2 has to be exact on them, whatever the
     * group's opacity is (its alpha chain and group stage are the old formulas). */
    const ksn_draw opaque_group[]={
        {.kind=KSN_RECT,.bounds={4,4,140,44},.clip={0,0,240,135},.opacity=255,
         .data.shape={0xd98672ff,0,0}},
        {.kind=KSN_ROUND_RECT,.bounds={38,10,90,36},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x357ecbff,6,0}},
        {.kind=KSN_STROKE,.bounds={10,6,232,129},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x80b5cfff,0,2}},
        {.kind=KSN_GRADIENT,.bounds={9,5,138,40},.clip={11,7,130,34},.opacity=255,
         .data.gradient={0x253849ff,0xe0a972ff,1,0,true}},
        {.kind=KSN_RECT,.bounds={100,20,130,40},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x123456ff,0,0}}};
    /* ... and the chains it does not: one semi-transparent child at a time, a
     * fully mixed scene, an alpha ramp, and a TEXT child. */
    const ksn_draw semi_first[]={
        {.kind=KSN_RECT,.bounds={4,4,140,44},.clip={0,0,240,135},.opacity=200,
         .data.shape={0xd98672d0,0,0}},
        {.kind=KSN_ROUND_RECT,.bounds={38,10,90,36},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x357ecbff,6,0}},
        {.kind=KSN_RECT,.bounds={100,20,130,40},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x123456ff,0,0}}};
    const ksn_draw semi_last[]={
        {.kind=KSN_RECT,.bounds={4,4,140,44},.clip={0,0,240,135},.opacity=255,
         .data.shape={0xd98672ff,0,0}},
        {.kind=KSN_ROUND_RECT,.bounds={38,10,90,36},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x357ecbff,6,0}},
        {.kind=KSN_RECT,.bounds={100,20,130,40},.clip={0,0,240,135},.opacity=211,
         .data.shape={0x12345680,0,0}}};
    const ksn_draw ramp[]={
        {.kind=KSN_GRADIENT,.bounds={6,6,234,120},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0x7788ff00,0xffeeaa88,0,0,true}},
        {.kind=KSN_ROUND_RECT,.bounds={40,30,200,90},.clip={0,0,240,135},.opacity=219,
         .data.shape={0x357ecbbb,7,0}}};
    const ksn_draw text_group[]={
        {.kind=KSN_RECT,.bounds={4,4,180,60},.clip={0,0,240,135},.opacity=255,
         .data.shape={0xd98672ff,0,0}},
        {.kind=KSN_TEXT,.bounds={-3,5,145,29},.clip={2,7,77,22},.opacity=255,
         .data.text={.utf8=title_text,.bytes=sizeof(title_text)-1,.capacity=64,
                     .font=KSN_BODY,.color=0xa15f3780}}};
    struct { const ksn_draw *draws; unsigned count; uint8_t opacity; bool classify;
             const char *name; } cases[]={
        {opaque_group,COUNT(opaque_group),255,true,"opaque chain"},
        {opaque_group,COUNT(opaque_group),254,true,"opaque chain, group 254"},
        {opaque_group,COUNT(opaque_group),1,true,"opaque chain, group 1"},
        {semi_first,COUNT(semi_first),255,true,"semi-transparent first child"},
        {semi_last,COUNT(semi_last),255,true,"semi-transparent last child"},
        {ramp,COUNT(ramp),255,true,"alpha ramp + translucent round rect"},
        {ramp,COUNT(ramp),128,true,"alpha ramp + translucent round rect, group 128"},
        {text_group,COUNT(text_group),255,false,"TEXT child"},
        {text_group,COUNT(text_group),200,false,"TEXT child, group 200"}};
    const int origins[4][2]={{0,0},{1,2},{3,3},{-20,-12}};
    for(unsigned c=0;c<COUNT(cases);c++)for(unsigned o=0;o<COUNT(origins);o++){
        memcpy(scene,cases[c].draws,cases[c].count*sizeof(ksn_draw));
        scene_count=cases[c].count;
        for(unsigned i=0;i<scene_count;i++)visible[i]=true;
        translate(origins[o][0],origins[o][1]);
        g_ksn_group_affine=0;
        ref_scene=scene;ref_visible=visible;ref_count=scene_count;ref_opacity=cases[c].opacity;
        ref_background=0x315d7bff;
        CHECK(render_scene(scene,visible,scene_count,cases[c].opacity,0x315d7bff,0,left_arm)==0);
        CHECK(render_scene(scene,visible,scene_count,cases[c].opacity,0x315d7bff,2,right_arm)==0);
        /* ref_* still names the translated scene the two arms just rendered, so
         * measure_move classifies pixels against the same geometry. */
        measure_move(left_arm,right_arm,cases[c].classify,cases[c].name);
        approximate_pixels+=240u*135u;
    }
    CHECK(moved_unexplained==0);
    CHECK(worst_565<=1);   /* one 5/6/5 level is the whole of it */
    return 0;
}
static ksn_cache cache;
static ksn_cache_command_block cache_commands;
static ksn_cache_text_block cache_text;
/* Three children, all opaque, one of each kind the cache accepts (its v0.2
 * subset is RECT/ROUND_RECT/STROKE, ksn_cache.h:50): with a placement opacity of
 * 255 the whole chain is step 1's case, at anything else it is not. */
static const ksn_draw tile_draws[3]={
    {.kind=KSN_RECT,.bounds={0,0,42,24},.clip={0,0,240,135},.opacity=255,
     .data.shape={0x185071ff,0,0}},
    {.kind=KSN_ROUND_RECT,.bounds={2,2,40,22},.clip={0,0,240,135},.opacity=255,
     .data.shape={0x63d7bcff,4,0}},
    {.kind=KSN_STROKE,.bounds={6,6,36,18},.clip={0,0,240,135},.opacity=255,
     .data.shape={0x000000ff,0,2}}};
/* Patch a scene that has already been rendered once, then compare the patched
 * panel with the same patch re-rendered from scratch (invalidate): the placement
 * a cache instance leaves behind must not drift between the two. */
static int patch_and_full(uint8_t opacity,const ksn_placement *place){
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx tx;ksn_render_stats stats;
    ksn_ref refs[SCENE_MAX]={0};ksn_template tile;ksn_instance instance;
    ksn_draw draws[3];memcpy(draws,tile_draws,sizeof(draws));
    for(unsigned i=0;i<3;i++)visible[i]=true;
    g_ksn_group_affine=1;
    (void)refs;
    ksn_core_init(&core);ksn_cache_init(&cache);
    CHECK(ksn_cache_create(&cache,KSN_APP,draws,3,&tile)==KSN_OK);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x0b1727ff)==KSN_OK);
    /* The template's children are not also added directly: the panel is the
     * background plus this one instance, so the scalar reference below can
     * answer for the whole panel. */
    CHECK(ksn_cache_instantiate(&cache,&core,tx,tile,place,&instance)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(ksn_cache_resolve(&cache,&core,tx,true)==KSN_OK);
    /* The group's opacity is the placement's; opacity 255 with these three
     * opaque children is exactly the folded arm's case. */
    for(unsigned round=0;round<100;round++){
        ksn_placement moved=*place;moved.x=(int16_t)(place->x+round%3);moved.opacity=opacity;
        CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
        CHECK(ksn_cache_place(&cache,&core,tx,instance,&moved)==KSN_OK);
        CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
        CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        CHECK(ksn_cache_resolve(&cache,&core,tx,true)==KSN_OK);
    }
    memcpy(left_arm,panel,sizeof(panel));
    ksn_core_invalidate(&core);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK&&stats.transferred_bytes==64800);
    memcpy(right_arm,panel,sizeof(panel));
    CHECK(compare_panels(left_arm,right_arm,"repeated placement")==0);
    /* The reference answers for the placement too: the same three children, moved
     * by the placement and composited with the placement's opacity. The last
     * PATCH left the instance at (place->x, place->y). */
    ksn_draw placed[3];memcpy(placed,tile_draws,sizeof(placed));
    for(unsigned i=0;i<3;i++){
        placed[i].bounds.x0+=(int16_t)place->x;placed[i].bounds.x1+=(int16_t)place->x;
        placed[i].bounds.y0+=(int16_t)place->y;placed[i].bounds.y1+=(int16_t)place->y;
        placed[i].clip.x0+=(int16_t)place->x;placed[i].clip.x1+=(int16_t)place->x;
        placed[i].clip.y0+=(int16_t)place->y;placed[i].clip.y1+=(int16_t)place->y;
    }
    unsigned mismatches=0;
    ref_scene=placed;ref_visible=visible;ref_count=3;ref_opacity=opacity;
    ref_background=0x0b1727ff;
    CHECK(against_reference(right_arm,"repeated placement",&mismatches)==0);
    return 0;
}
static const ksn_draw frame_scene[]={
    {.kind=KSN_GRADIENT,.bounds={0,0,240,18},.clip={0,0,240,135},.opacity=255,
     .data.gradient={0x0d2940ff,0x498781ff,0,0,true}},
    {.kind=KSN_RECT,.bounds={12,28,112,56},.clip={0,0,240,135},.opacity=255,
     .data.shape={0x164c70ff,0,0}},
    {.kind=KSN_ROUND_RECT,.bounds={34,36,134,64},.clip={0,0,240,135},.opacity=255,
     .data.shape={0x65d7bcff,7,0}},
    {.kind=KSN_ROUND_RECT,.bounds={16,76,36,96},.clip={0,0,240,135},.opacity=210,
     .data.shape={0xf5bd4fff,6,0}},
    {.kind=KSN_STROKE,.bounds={10,117,232,129},.clip={0,0,240,135},.opacity=170,
     .data.shape={0x80b5cfaa,0,1}},
    {.kind=KSN_TEXT,.bounds={12,100,180,113},.clip={0,0,240,135},.opacity=255,
     .data.text={.utf8=title_text,.bytes=sizeof(title_text)-1,.capacity=24,
                 .font=KSN_CAPTION,.color=0xa8d8efff}}};
#define FRAMES 120
static uint32_t frame_hash[3][FRAMES],arm_hash[3];
/* The pre-3c arm's own frames, so the approximate arm can be measured frame by
 * frame in the same run order (7.8 MB of host memory; a test-static panel is
 * what the existing harnesses already use). */
static uint16_t frame_store[FRAMES][240*135];
static unsigned frame_moved[FRAMES];
static uint32_t hash_panel(void){
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<240*135;i++){hash^=panel[i];hash*=16777619u;}
    return hash;
}
static int run_frames(unsigned arm,uint16_t (*store)[240*135],bool measure){
    g_ksn_group_affine=arm;
    ksn_core_init(&core);ksn_cache_init(&cache);
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx tx;ksn_ref refs[6];
    ksn_template tile;ksn_instance top,bottom;
    ksn_render_stats stats;ksn_draw draws[6];
    memcpy(draws,frame_scene,sizeof(draws));
    /* The instance's children are opaque, so a placement opacity of 255 is the
     * folded case and 230 (below) is not: the same instance exercises both.
     * One child of the group is not opaque, so the group itself never folds
     * here -- that is the scene's second half. */
    ksn_placement high={142,28,{0,0,240,135},255,true};
    ksn_placement low={188,72,{0,0,240,135},175,true};
    arm_hash[arm]=0;
    CHECK(ksn_cache_create(&cache,KSN_APP,tile_draws,3,&tile)==KSN_OK);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x071425ff)==KSN_OK);
    for(unsigned i=0;i<6;i++)CHECK(app.ops->add(app.ctx,tx,&draws[i],&refs[i])==KSN_OK);
    CHECK(ksn_core_group(&core,KSN_APP,tx,refs[1],2,255)==KSN_OK);      /* folds */
    CHECK(ksn_core_group(&core,KSN_APP,tx,refs[3],2,208)==KSN_OK);      /* does not */
    CHECK(ksn_cache_instantiate(&cache,&core,tx,tile,&high,&top)==KSN_OK);
    CHECK(ksn_cache_instantiate(&cache,&core,tx,tile,&low,&bottom)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    for(unsigned tick=0;tick<FRAMES;tick++){
        if(tick){
            CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
            int x=12+(int)(tick*3%184),width=12+(int)(tick*5%208);
            ksn_change change={.property=KSN_SET_RECT,.value.rect={x,76,x+20,96}};
            CHECK(app.ops->change(app.ctx,tx,refs[3],&change)==KSN_OK);
            change=(ksn_change){.property=KSN_SET_RECT,.value.rect={12,117,12+width,127}};
            CHECK(app.ops->change(app.ctx,tx,refs[4],&change)==KSN_OK);
            change=(ksn_change){.property=KSN_SET_COLOR,
                                .value.color=(tick&16)?0x295d86ffu:0x164c70ffu};
            CHECK(app.ops->change(app.ctx,tx,refs[1],&change)==KSN_OK);
            /* The rect keeps radius 7 valid: width/2 >= radius or the core
             * rejects the change. */
            change=(ksn_change){.property=KSN_SET_RECT,.value.rect={34,36,134-(int)(tick%60),64}};
            CHECK(app.ops->change(app.ctx,tx,refs[2],&change)==KSN_OK);
            high.y=28+(int)(tick%18);
            high.opacity=(tick%5)?255:200;   /* 255 folds, 200 keeps the tile */
            CHECK(ksn_cache_place(&cache,&core,tx,top,&high)==KSN_OK);
            low.x=188-(int)(tick%20);
            CHECK(ksn_cache_place(&cache,&core,tx,bottom,&low)==KSN_OK);
            CHECK(ksn_cache_set_visible(&cache,&core,tx,bottom,(tick%40)<31)==KSN_OK);
            /* The folded group's own opacity stays 255, its children stay opaque
             * and their geometry moves; the second group is re-opaqued every
             * other tick so both sides of the precondition are crossed. */
            change=(ksn_change){.property=KSN_SET_CLIP,.value.rect={0,0,240,135}};
            CHECK(app.ops->change(app.ctx,tx,refs[1],&change)==KSN_OK);
            change=(ksn_change){.property=KSN_SET_CLIP,.value.rect={0,0,240,135}};
            CHECK(app.ops->change(app.ctx,tx,refs[2],&change)==KSN_OK);
            CHECK(ksn_core_group(&core,KSN_APP,tx,refs[3],2,(uint8_t)((tick%3)?208:255))==KSN_OK);
            CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
            if(tick%17==0)ksn_core_invalidate(&core);
        }
        ksn_frame frame;
        CHECK(ksn_core_frame(&core,&frame)==KSN_OK);
        CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        CHECK(ksn_cache_resolve(&cache,&core,frame.ticket,true)==KSN_OK);
        if((!tick||tick%17==0)&&arm<2){CHECK(stats.transferred_bytes==64800);frames_full++;}
        frame_hash[arm][tick]=hash_panel();
        if(store&&arm==0)memcpy(store[tick],panel,sizeof(panel));
        else if(measure){
            unsigned before=moved_pixels;
            measure_move(store[tick],panel,false,"120 frames");
            frame_moved[tick]=moved_pixels-before;
        }
        arm_hash[arm]=arm_hash[arm]*31u+frame_hash[arm][tick];
    }
    return 0;
}
int main(void){
    CHECK(ksn_cache_bind(&cache,&cache_commands,&cache_text)==KSN_OK);
    CHECK(foldable_scenes()==0);
    CHECK(nonfoldable_scenes()==0);
    ksn_placement place={142,28,{0,0,240,135},255,true};
    CHECK(patch_and_full(255,&place)==0);
    place.opacity=254;
    CHECK(patch_and_full(254,&place)==0);
    CHECK(approx_scenes()==0);
    CHECK(run_frames(0,frame_store,false)==0);
    CHECK(run_frames(1,NULL,false)==0);
    CHECK(run_frames(2,frame_store,true)==0);
    for(unsigned tick=0;tick<FRAMES;tick++)if(frame_hash[0][tick]!=frame_hash[1][tick]){
        fprintf(stderr,"frame %u differs: old %08x folded %08x\n",tick,
                frame_hash[0][tick],frame_hash[1][tick]);
        CHECK(0);
    }
    printf("group affine: folded arm and isolated tile chain agree on %u pixels "
           "(whole panel, independent scalar reference, both arms; %u dither differences)\n",
           compared_pixels+nonfold_pixels,ref_dither_differences);
    printf("group affine: %u frames identical both ways (arms 0/1), %u of them full "
           "64,800-byte repaints (rolling hash %08x); the measured arm 2 has its own "
           "hash %08x\n",FRAMES,frames_full,arm_hash[1],arm_hash[2]);
    printf("group affine: step 2 moved %u of %u pixels over the parameter space "
           "(worst 8-bit channel step r/g/b=%u/%u/%u, worst 565 step %u; %u of %u "
           "pixels under a covering child with a<255 moved, %u elsewhere)\n",
           moved_pixels,approx_pixels+FRAMES*240u*135u,worst_step_r,worst_step_g,worst_step_b,
           worst_565,nonopaque_moved,nonopaque_pixels,moved_unexplained);
    printf("group affine: step 2 covering-child classes (moved/total): none=%u/%u, "
           "one=%u/%u, two=%u/%u, three+=%u/%u\n",
           class_moved[0],class_pixels[0],class_moved[1],class_pixels[1],
           class_moved[2],class_pixels[2],class_moved[3],class_pixels[3]);
    {
        unsigned frames_with_move=0,worst_frame=0;
        for(unsigned tick=0;tick<FRAMES;tick++){
            if(frame_moved[tick])frames_with_move++;
            if(frame_moved[tick]>worst_frame)worst_frame=frame_moved[tick];
        }
        printf("group affine: step 2 over 120 frames: %u frames moved a pixel, worst "
               "frame %u of 32,400\n",frames_with_move,worst_frame);
    }
#ifdef KSN_AFFINE_COUNT
    printf("group affine: folded rows: switch off=%" PRIu64 ", on=%" PRIu64
           " (entries into group_opaque_row)\n",fold_rows[0],fold_rows[1]);
    /* The switch-on arm must actually have folded, and the switch-off arm must
     * not have entered the folded row at all. */
    CHECK(fold_rows[0]==0);
    CHECK(fold_rows[1]>5000);
#endif
    puts("group affine PASS: the opaque group's chain folds to one map per group, "
         "pixel for pixel");
    return 0;
}
