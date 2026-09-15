/* Rotated image spans: prove the stepped arm and the per-pixel division arm
 * produce the same panel, in one binary, on a scene rebuilt from scratch for
 * each arm (the same shape test_render_prof.c uses). Each configuration runs
 * every stage twice: a REPLACE frame, then PATCH frames that move the image and
 * change its rotation, one of them rotating a scaled copy several turns. The
 * provider counts its fetches, so the two arms must also ask for the same
 * blocks of source pixels, not merely land on the same colours. */
#include "core_fixture.h"
#include "ksn_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define PANEL (240*135)
static uint16_t panel[PANEL],strip[240*8];
static unsigned long long fetches;
static uint16_t *buffer(void *p){(void)p;return strip;}
static ksn_result send(void *p,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)p;memcpy(panel+y*240,pixels,rows*240*2);return KSN_OK;}
static uint16_t source_color(unsigned x,unsigned y,unsigned frame){return (uint16_t)(x*313+y*937+frame*3001);}
static uint8_t source_coverage(unsigned x,unsigned y){const uint8_t a[]={0,1,127,128,254,255};return a[(x+y*3)%6];}
static ksn_result source(void *p,uint16_t variant,uint16_t frame,uint16_t y,uint16_t x,uint16_t n,uint16_t *rgb,uint8_t *alpha){
    (void)p;assert(variant<1&&frame<2&&y<160&&x+n<=160&&n<=32);
    fetches++;
    for(unsigned i=0;i<n;i++){rgb[i]=source_color(x+i,y,frame);alpha[i]=source_coverage(x+i,y);}
    return KSN_OK;
}
static uint32_t hash_bytes(const uint16_t *pixels,unsigned count){
    uint32_t h=2166136261u;const uint8_t *bytes=(const uint8_t *)pixels;
    for(unsigned i=0;i<count*2;i++){h^=bytes[i];h*=16777619u;}
    return h;
}
static unsigned long long panel_diff(const uint16_t *a,const uint16_t *b,unsigned long long *worst){
    unsigned long long n=0;
    for(unsigned i=0;i<PANEL;i++)if(a[i]!=b[i]){
        unsigned long long d=a[i]>b[i]?(unsigned long long)(a[i]-b[i]):(unsigned long long)(b[i]-a[i]);
        if(d>*worst)*worst=d;
        n++;
    }
    return n;
}

typedef struct {
    int x,y,w,h;              /* destination bounds */
    unsigned source_x,source_y,source_w,source_h;
    unsigned rotation;        /* 0..1023 */
    unsigned opacity;         /* 0..255 */
    int group;                /* isolated group opacity, or the direct path */
    ksn_rect clip;
    unsigned steps;           /* PATCH frames after the REPLACE frame */
} config;

/* One arm of the rotated span path: 0 reads a row's anchors from the per-row
 * table, 1 divides per pixel (the pre-optimisation path), 2 divides per span.
 * All three must produce the same panel, the same number of source fetches and
 * the same frame hashes; the host builds every scene from scratch per arm. */
static unsigned long long anchor_served;   /* biggest table the renderer served */
static void set_arm(unsigned arm){
    g_ksn_image_rotate_step=arm!=1;
    g_ksn_image_rotate_anchor=arm==0;
}
static void note_anchor(void){
    int base_x=0;
    unsigned spans=ksn_render_rotate_anchor_state(&base_x);
    if(g_ksn_image_rotate_anchor&&spans>anchor_served)anchor_served=spans;
}

/* One scene, one arm. Returns a hash over every rendered frame and leaves the
 * last frame in `panel`. */
static uint32_t scene(const config *c,unsigned arm,unsigned long long *fetch_count){
    KSN_TEST_CORE(core,);ksn_core_init(&core);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_image_port port={NULL,160,160,1,2,source};ksn_resource image;
    assert(ksn_core_register_image(&core,KSN_APP,&port,&image)==KSN_OK);
    ksn_display_port display={NULL,buffer,send,240,135,8,NULL};
    ksn_render_stats stats;ksn_tx tx;ksn_ref ref;uint32_t hash=2166136261u;
    ksn_draw d={.kind=KSN_IMAGE,.clip=c->clip,.opacity=(uint8_t)c->opacity,
        .data.image={.resource=image,.scale=KSN_IMAGE_STRETCH,.source_x=(uint16_t)c->source_x,
                     .source_y=(uint16_t)c->source_y,.source_width=(uint16_t)c->source_w,
                     .source_height=(uint16_t)c->source_h,.rotation=(uint16_t)c->rotation}};
    d.bounds=(ksn_rect){(int16_t)c->x,(int16_t)c->y,(int16_t)(c->x+(int)c->w),(int16_t)(c->y+(int)c->h)};
    set_arm(arm);
    fetches=0;
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x183c60ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&d,&ref)==KSN_OK);
    if(c->group)assert(ksn_core_group(&core,KSN_APP,tx,ref,1,(uint8_t)(137+(c->group&1)))==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    note_anchor();
    hash^=hash_bytes(panel,PANEL);hash*=16777619u;
    for(unsigned s=1;s<=c->steps;s++){
        d.bounds=(ksn_rect){(int16_t)(c->x+(int)(s*11)-70),(int16_t)(c->y+(int)(s*3)-12),
                            (int16_t)(c->x+(int)(s*11)-70+(int)c->w+(int)s),
                            (int16_t)(c->y+(int)(s*3)-12+(int)c->h+(int)(s*7))};
        d.data.image.rotation=(uint16_t)((c->rotation+s*37)&1023);
        assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
        ksn_change change={.property=KSN_SET_RECT,.value.rect=d.bounds};
        assert(app.ops->change(app.ctx,tx,ref,&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_ROTATION,.value.rotation=d.data.image.rotation};
        assert(app.ops->change(app.ctx,tx,ref,&change)==KSN_OK);
        assert(app.ops->end(app.ctx,tx)==KSN_OK);
        assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        note_anchor();
        hash^=hash_bytes(panel,PANEL);hash*=16777619u;
    }
    *fetch_count=fetches;
    set_arm(0);
    return hash;
}
/* The 120-frame animated transform track: position, scale and multi-turn
 * rotation, 30 fps, looping. `arm_mode` is the arm, or 3 to alternate per
 * frame, so a dependence on which arm rendered the previous frame cannot
 * hide. */
static uint32_t animate_120(unsigned arm_mode,unsigned long long *fetch_count,unsigned *frames_out){
    KSN_TEST_CORE(core,);ksn_core_init(&core);
    static ksn_core_animation_block blocks[2];
    assert(ksn_core_enable_animation(&core,&blocks[0],&blocks[1])==KSN_OK);
    assert(ksn_core_animation_bytes(&core)<=1024);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_image_port port={NULL,160,160,1,2,source};ksn_resource image;
    assert(ksn_core_register_image(&core,KSN_APP,&port,&image)==KSN_OK);
    ksn_display_port display={NULL,buffer,send,240,135,8,NULL};
    ksn_render_stats stats;ksn_tx tx;ksn_ref ref;uint32_t hash=2166136261u;unsigned frames=0;
    ksn_draw d={.kind=KSN_IMAGE,.clip={0,0,240,135},.opacity=193,.bounds={10,20,74,84},
        .data.image={.resource=image,.scale=KSN_IMAGE_STRETCH,.source_x=3,.source_y=2,
                     .source_width=64,.source_height=64}};
    ksn_motion motion={.count=1,.property=KSN_TRANSFORM,.from.pose={{10,20,74,84},0},
        .to.pose={{150,40,214,104},2048},.duration_ms=1000,.easing=KSN_LINEAR,.repeat=KSN_LOOP};
    fetches=0;
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x183c60ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&d,&ref)==KSN_OK);
    motion.first=ref;
    ksn_animation animation;
    assert(app.ops->animate(app.ctx,tx,&motion,&animation)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    hash^=hash_bytes(panel,PANEL);hash*=16777619u;
    ksn_core_start_animations(&core,0);
    for(unsigned f=1;f<=120;f++){
        ksn_tx atx={0};
        assert(ksn_core_advance_animations(&core,(uint64_t)f*33334u,false,&atx)==KSN_OK);
        if(!atx.value)continue;
        frames++;
        if(arm_mode==3)set_arm(f&1u);else set_arm(arm_mode);
        assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        note_anchor();
        hash^=hash_bytes(panel,PANEL);hash*=16777619u;
    }
    *fetch_count=fetches;*frames_out=frames;
    set_arm(0);
    return hash;
}

static void compare(const config *c,const char *what,unsigned field,
                    unsigned long long *compared,unsigned long long *configs,
                    unsigned long long *worst){
    static uint16_t first[PANEL];
    unsigned long long fa,fb;
    /* The per-pixel division arm is the reference: render it once, then every
     * optimised arm against its panel, frame hash and fetch count. */
    uint32_t hb=scene(c,1,&fb);memcpy(first,panel,sizeof(panel));
    for(unsigned arm=0;arm<3;arm+=2){
        uint32_t ha=scene(c,arm,&fa);
        (*compared)+=2;(*configs)++;
        if(ha!=hb||memcmp(first,panel,sizeof(panel))!=0||fa!=fb){
            unsigned long long n=panel_diff(first,panel,worst);
            printf("image rotate arms MISMATCH %s field=%u arm=%u diff=%llu fetches=%llu/%llu hash=%08x/%08x\n",
                what,field,arm,n,fa,fb,ha,hb);assert(false);
        }
    }
}

int main(void){
    static uint16_t first[PANEL];
    unsigned long long worst_step=0,compared=0,configs=0;
    config c={.x=50,.y=30,.w=64,.h=64,.source_x=3,.source_y=2,.source_w=64,.source_h=64,
              .rotation=0,.opacity=255,.group=0,.clip={0,0,240,135},.steps=2};
    /* 1. Every rotation at a fixed pose, in the table arm and in the per-span
     *    division arm, each against the per-pixel division arm. */
    for(unsigned rotation=0;rotation<1024;rotation++){
        c.rotation=rotation;
        compare(&c,"rotation",rotation,&compared,&configs,&worst_step);
    }
    /* 2. Destination sizes and source windows, including heavy minification
     *    where the per-pixel quotient step is many source columns. */
    {const unsigned sizes[][2]={{1,1},{2,3},{17,9},{32,32},{33,64},{96,48}};
     const unsigned windows[][2]={{16,16},{48,48},{96,96}};
     for(unsigned s=0;s<sizeof(sizes)/sizeof(sizes[0]);s++)
         for(unsigned w=0;w<sizeof(windows)/sizeof(windows[0]);w++)
             for(unsigned rotation=0;rotation<1024;rotation+=37){
                 c.x=40;c.y=20;c.w=(int)sizes[s][0];c.h=(int)sizes[s][1];
                 c.source_w=windows[w][0];c.source_h=windows[w][1];c.rotation=rotation;
                 compare(&c,"size",s*100+w,&compared,&configs,&worst_step);
             }}
    /* 3. Source origins, so the 16-column block grid starts off the boundary. */
    for(unsigned ox=0;ox<4;ox++)for(unsigned oy=0;oy<4;oy++)
        for(unsigned rotation=0;rotation<1024;rotation+=97){
            c.x=37;c.y=17;c.w=64;c.h=64;
            c.source_x=ox?ox*15u+1u:0u;c.source_y=oy?oy*15u+1u:0u;
            c.source_w=48;c.source_h=48;c.rotation=rotation;
            compare(&c,"origin",ox*4+oy,&compared,&configs,&worst_step);
        }
    c.source_x=3;c.source_y=2;c.source_w=64;c.source_h=64;
    /* 4. Clip, opacity and isolated group opacity, both render paths. */
    {const ksn_rect clips[]={{0,0,240,135},{9,11,225,122},{50,30,60,40},{120,60,240,135}};
     const unsigned opacities[]={0,1,127,128,193,254,255};
     const unsigned rotations[]={1,255,341,512,683,777,1023};
     for(unsigned ci=0;ci<4;ci++)for(unsigned oi=0;oi<7;oi++)for(unsigned g=0;g<2;g++)
         for(unsigned r=0;r<7;r++){
             c.x=45;c.y=25;c.w=64;c.h=64;c.clip=clips[ci];c.opacity=opacities[oi];
             c.group=(int)g;c.rotation=rotations[r];
             compare(&c,"clip",ci*100+oi,&compared,&configs,&worst_step);
         }}
    /* 5. Long motion: an off-screen image entering the panel, rotation turning
     *    with it, 20 PATCH frames per configuration. The first span of a row is
     *    off the panel here, so the table has to rebuild once per row. */
    c.clip=(ksn_rect){0,0,240,135};c.opacity=193;c.steps=20;
    for(unsigned origin=0;origin<3;origin++)for(unsigned group=0;group<2;group++){
        c.x=-64+(int)origin*30;c.y=-32+(int)origin*10;c.w=64;c.h=64;
        c.group=(int)group;c.rotation=origin*341;
        compare(&c,"motion",origin*2+group,&compared,&configs,&worst_step);
    }
    /* 6. Wider sweep for the anchor table: every kind of 16-pixel span grid
     *    the table has to build over, in the table arm and in the per-span
     *    division arm, each against the per-pixel division arm. The small
     *    destinations take a fine rotation step (a row of a few pixels still
     *    crosses several spans when the source window is small); the ones
     *    whose footprint fills the panel take a coarse one so the sweep stays
     *    bounded. */
    c.steps=1;
    {const unsigned fine[][2]={{13,7},{64,13},{13,64}};
     const unsigned coarse[][2]={{211,97},{240,135},{5,240}};
     const unsigned windows[][2]={{16,32},{64,16},{128,128}};
     for(unsigned pass=0;pass<2;pass++)
         for(unsigned s=0;s<(pass?sizeof(coarse):sizeof(fine))/sizeof(fine[0]);s++)
             for(unsigned w=0;w<sizeof(windows)/sizeof(windows[0]);w++)
                 for(unsigned rotation=1;rotation<1024;rotation+=pass?67:13){
                     const unsigned *size=pass?coarse[s]:fine[s];
                     c.x=(int)(rotation%97)-48;c.y=(int)(rotation%53)-26;
                     c.w=(int)size[0];c.h=(int)size[1];
                     c.source_x=(uint16_t)(rotation%24);c.source_y=(uint16_t)(rotation%12);
                     c.source_w=windows[w][0];c.source_h=windows[w][1];c.rotation=rotation;
                     compare(&c,"sweep",pass*1000+s*100+w,&compared,&configs,&worst_step);
                 }}
    c.source_x=3;c.source_y=2;c.source_w=64;c.source_h=64;c.steps=2;c.group=0;
    c.clip=(ksn_rect){0,0,240,135};c.opacity=255;
    /* 7. 120-frame animated track: table arm, per-span arm and an arm that
     *    alternates per frame, all against the per-pixel division arm. */
    {unsigned long long f_tab,f_span,f_alt;unsigned frames_tab=0,frames_span=0,frames_alt=0;
     uint32_t h_tab=animate_120(0,&f_tab,&frames_tab);memcpy(first,panel,sizeof(panel));
     uint32_t h_div=animate_120(1,&f_span,&frames_span);
     unsigned long long diff=panel_diff(first,panel,&worst_step);
     compared+=2;
     if(h_tab!=h_div||diff||f_tab!=f_span||frames_tab!=120||frames_span!=120){
         printf("image rotate arms MISMATCH animated frames=%u/%u diff=%llu fetches=%llu/%llu hash=%08x/%08x\n",
             frames_tab,frames_span,diff,f_tab,f_span,h_tab,h_div);assert(false);}
     uint32_t h_alt=animate_120(2,&f_alt,&frames_alt);
     compared+=1;
     if(h_alt!=h_tab||f_alt!=f_tab){
         printf("image rotate arms MISMATCH animated per-span fetches=%llu hash=%08x/%08x\n",
             f_alt,h_alt,h_tab);assert(false);}
     uint32_t h_alt2=animate_120(3,&f_alt,&frames_alt);
     compared+=1;
     if(h_alt2!=h_tab||f_alt!=f_tab){
         printf("image rotate arms MISMATCH animated alternating fetches=%llu hash=%08x/%08x\n",
             f_alt,h_alt2,h_tab);assert(false);}
     printf("animated track: %u frames in all arms, alternating arm identical\n",frames_tab);
     configs+=3;}
    /* 8. Non-vacuity: the table arm is only evidence if the table actually
     *    served spans. The largest row it built is read out of the renderer. */
    assert(anchor_served>1);
#ifdef KSN_ANCHOR_COUNT
    printf("anchor table: %u builds, largest row served %llu entries\n",
        g_ksn_image_anchor_builds,anchor_served);
    assert(g_ksn_image_anchor_builds>0);
#else
    printf("anchor table: largest row served %llu entries (build count needs -DKSN_ANCHOR_COUNT)\n",
        anchor_served);
#endif
    printf("image rotate arms PASS: %llu configs, %llu panel hashes, worst pixel step %llu, fetches identical\n",
        configs,compared,worst_step);
    return 0;
}
