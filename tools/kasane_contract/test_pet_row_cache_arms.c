/* Both arms of the PET provider's decoded-row cache, in one binary.
 *
 * Every scene is rebuilt per arm (the shape test_image_rotate_arms.c uses) and
 * the reference peer - the old path, one decode per fetch - renders first; the
 * peer under test must land on exactly the same panel, byte for byte, in every
 * frame. The provider counts its fetches and its row decodes
 * (KSN_PET_ROW_STATS), so the two arms must also ask for the same source
 * blocks and the cache arm must decode strictly less: a cache that answered
 * from a stale row would show up as pixels, and one that never hits as counts.
 *
 * The animated scene is the 120-frame transform track (position, scale and two
 * turns of rotation, 30 fps, looping) on the real embedded pet image, run with
 * the arms fixed and with the arm alternating per frame, so a dependence on
 * which arm drew the previous frame cannot hide. `measure_rotate` then prints
 * the fetch/decode table for the rotated 64x64 scene at a fixed key, which is
 * the scene docs/perf/kasane-image-transform-recon.md §4 counted.
 *
 * Built against the pre-change provider with -DKSN_PET_ROW_STATE_ABSENT this is
 * the same test with no switch and no counters: its printed hashes are the
 * pre-change hashes the off arm has to reproduce in the new tree. */
#include "core_fixture.h"
#include "ksn_render.h"
#include "ksn_pet.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(KSN_PET_ROW_STATE_ABSENT)
#define STATS 0
#else
#define STATS 1
#endif
#if STATS
#define COUNTERS_RESET() (g_ksn_pet_fetches=0,g_ksn_pet_decodes=0)
#define FETCHES g_ksn_pet_fetches
#define DECODES g_ksn_pet_decodes
#define SET_ARM(cache) (g_ksn_pet_row_cache=(cache))
#else
static unsigned long long absent_fetches,absent_decodes;
#define COUNTERS_RESET() ((void)0)
#define FETCHES absent_fetches
#define DECODES absent_decodes
#define SET_ARM(cache) ((void)(cache))
#endif

#define PANEL (240*135)
#define PEERS 2
static uint16_t panel[PANEL],reference[PANEL],strip[240*8];
static uint8_t *pets;
static size_t pets_bytes;

typedef struct { ksn_core_command_block commands[2];ksn_core_text_block text[2]; } core_storage;
typedef struct {
    ksn_core core;
    ksn_client client;
    ksn_resource image,second;
    ksn_ref ref;
    unsigned long long fetches,decodes;
    unsigned frames;
    uint32_t hash;
} peer;
static core_storage storage[PEERS];
static peer peers[PEERS];

static uint16_t *buffer(void *p){(void)p;return strip;}
static ksn_result send(void *p,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)p;memcpy(panel+y*240,pixels,rows*240*2);return KSN_OK;}
static uint32_t hash_bytes(const uint16_t *pixels,unsigned count){
    uint32_t h=2166136261u;const uint8_t *bytes=(const uint8_t *)pixels;
    for(unsigned i=0;i<count*2;i++){h^=bytes[i];h*=16777619u;}
    return h;}
static ksn_display_port display={NULL,buffer,send,240,135,8,NULL};
static ksn_render_stats stats;

/* One scene: only what the compositor resolves, rebuilt per arm from that arm's
 * own resource handles. */
typedef struct {
    ksn_rect bounds;
    unsigned source_x,source_y,source_w,source_h,rotation,variant,frame,opacity;
    ksn_image_scale scale;
    uint8_t group;          /* 0: direct path, otherwise the group opacity */
    bool two_images;        /* also draw the second registered image */
} scene;

static void open_peer(unsigned i){
    peers[i]=(peer){0};
    peers[i].core=(ksn_core){.state={.banks={
        {.commands=storage[i].commands[0].commands,.text=storage[i].text[0].bytes},
        {.commands=storage[i].commands[1].commands,.text=storage[i].text[1].bytes}}}};
    ksn_core_init(&peers[i].core);
    peers[i].client=ksn_core_client(&peers[i].core,KSN_APP);
    ksn_image_port port;assert(ksn_pet_image(pets,pets_bytes,&port)==KSN_OK);
    assert(ksn_core_register_image(&peers[i].core,KSN_APP,&port,&peers[i].image)==KSN_OK);
    peers[i].hash=2166136261u;
}
static ksn_draw draw_of(const scene *s,ksn_resource resource,ksn_rect bounds){
    ksn_draw d={.kind=KSN_IMAGE,.bounds=bounds,.clip={0,0,240,135},.opacity=(uint8_t)s->opacity,
        .data.image={.resource=resource,.scale=s->scale,.variant=(uint16_t)s->variant,
                     .frame=(uint16_t)s->frame,.source_x=(uint16_t)s->source_x,
                     .source_y=(uint16_t)s->source_y,.source_width=(uint16_t)s->source_w,
                     .source_height=(uint16_t)s->source_h,.rotation=(uint16_t)s->rotation}};
    return d;
}
/* REPLACE frame: background, the image, and the isolated group when asked. */
static void mount(peer *p,const scene *s){
    ksn_draw d=draw_of(s,p->image,s->bounds);
    ksn_tx tx;
    assert(p->client.ops->begin(p->client.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(p->client.ops->background(p->client.ctx,tx,0x183c60ff)==KSN_OK);
    assert(p->client.ops->add(p->client.ctx,tx,&d,&p->ref)==KSN_OK);
    if(s->two_images){
        ksn_draw e=draw_of(s,p->second,(ksn_rect){120,60,184,92});
        ksn_ref second_ref;
        assert(p->client.ops->add(p->client.ctx,tx,&e,&second_ref)==KSN_OK);
    }
    if(s->group)
        assert(ksn_core_group(&p->core,KSN_APP,tx,p->ref,s->two_images?2:1,s->group)==KSN_OK);
    assert(p->client.ops->end(p->client.ctx,tx)==KSN_OK);
}
/* PATCH frame: the pose, the rotation and the (variant,frame) key move. */
static void update(peer *p,const scene *s){
    ksn_tx tx;
    assert(p->client.ops->begin(p->client.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change change={.property=KSN_SET_RECT,.value.rect=s->bounds};
    assert(p->client.ops->change(p->client.ctx,tx,p->ref,&change)==KSN_OK);
    if(s->scale==KSN_IMAGE_STRETCH){ /* rotation lives on the stretch payload */
        change=(ksn_change){.property=KSN_SET_ROTATION,.value.rotation=(uint16_t)s->rotation};
        assert(p->client.ops->change(p->client.ctx,tx,p->ref,&change)==KSN_OK);
    }
    change=(ksn_change){.property=KSN_SET_IMAGE_FRAME,
                        .value.image={(uint16_t)s->variant,(uint16_t)s->frame}};
    assert(p->client.ops->change(p->client.ctx,tx,p->ref,&change)==KSN_OK);
    assert(p->client.ops->end(p->client.ctx,tx)==KSN_OK);
}
/* Renders peer i under one arm and books its counters. */
static void render_peer(unsigned i,bool cache){
    SET_ARM(cache);
    COUNTERS_RESET();
    assert(ksn_render_rects(&peers[i].core,&display,&stats)==KSN_OK);
    peers[i].fetches+=FETCHES;peers[i].decodes+=DECODES;peers[i].frames++;
    peers[i].hash^=hash_bytes(panel,PANEL);peers[i].hash*=16777619u;
}
static unsigned long long compare_panel(unsigned long long *worst){
    unsigned long long diff=0;
    for(unsigned i=0;i<PANEL;i++)if(panel[i]!=reference[i]){
        unsigned long long d=panel[i]>reference[i]?(unsigned long long)(panel[i]-reference[i]):
                                                    (unsigned long long)(reference[i]-panel[i]);
        if(d>*worst)*worst=d;
        diff++;
    }
    return diff;
}
/* The reference arm (off) draws first, then the arm under test; the panels must
 * be identical. Leaves the arm-under-test panel in `panel`, the reference in
 * `reference`, and returns the difference (always 0). */
static unsigned long long render_pair(bool cache_under_test,const char *tag,
                                      unsigned long long *worst){
    render_peer(0,false);memcpy(reference,panel,sizeof(panel));
    render_peer(1,cache_under_test);
    unsigned long long diff=compare_panel(worst);
    if(diff){printf("pet row cache MISMATCH %s: %llu pixels\n",tag,diff);assert(false);}
    return diff;
}

/* 1. The provider itself: every pet, mood, row and span, both arms in one
 *    process. Off decodes once per fetch, on decodes once per (pet,mood,row). */
static unsigned long long provider_arms(unsigned long long *worst){
    static uint16_t cached[66],plain[66];static uint8_t cached_a[66],plain_a[66];
    ksn_image_port port;assert(ksn_pet_image(pets,pets_bytes,&port)==KSN_OK);
    unsigned long long off_fetches=0,off_decodes=0,on_fetches=0,on_decodes=0,spans=0;
    for(unsigned pet=0;pet<12;pet++)for(unsigned mood=0;mood<6;mood++)for(unsigned y=0;y<64;y++)
        for(unsigned x=0;x<64;x+=7){
            unsigned count=64-x;if(count>31)count=31;
            unsigned long long before=FETCHES,decoded=DECODES;
            SET_ARM(false);
            assert(port.read_span(port.ctx,pet,mood,(uint16_t)y,(uint16_t)x,(uint16_t)count,
                                  plain+1,plain_a+1)==KSN_OK);
            off_fetches+=FETCHES-before;off_decodes+=DECODES-decoded;
            before=FETCHES;decoded=DECODES;
            SET_ARM(true);
            assert(port.read_span(port.ctx,pet,mood,(uint16_t)y,(uint16_t)x,(uint16_t)count,
                                  cached+1,cached_a+1)==KSN_OK);
            on_fetches+=FETCHES-before;on_decodes+=DECODES-decoded;
            spans++;
            for(unsigned i=0;i<count;i++){
                unsigned long long d=plain[i+1]>cached[i+1]?(unsigned long long)(plain[i+1]-cached[i+1]):
                                                             (unsigned long long)(cached[i+1]-plain[i+1]);
                if(d>*worst)*worst=d;
                assert(plain[i+1]==cached[i+1]&&plain_a[i+1]==cached_a[i+1]);
            }
        }
    SET_ARM(true);
#if STATS
    assert(off_fetches==on_fetches&&off_fetches==spans);
    assert(off_decodes==spans&&on_decodes==12*6*64);
    printf("provider arms: %llu spans, fetches off/on %llu/%llu, decodes off/on %llu/%llu\n",
        spans,off_fetches,on_fetches,off_decodes,on_decodes);
#else
    printf("provider arms: %llu spans, pixels compared, no counters in this build\n",spans);
#endif
    /* Invalid arguments must leave the cache and the counters alone. */
    unsigned long long before=FETCHES;int decoded=DECODES;SET_ARM(true);
    assert(port.read_span(port.ctx,12,0,0,0,1,plain,plain_a)==KSN_INVALID);
    assert(port.read_span(port.ctx,0,6,0,0,1,plain,plain_a)==KSN_INVALID);
    assert(port.read_span(port.ctx,0,0,64,0,1,plain,plain_a)==KSN_INVALID);
    assert(port.read_span(port.ctx,0,0,0,63,2,plain,plain_a)==KSN_INVALID);
    assert(port.read_span(port.ctx,0,0,0,64,0,NULL,NULL)==KSN_OK);
    assert(port.read_span(port.ctx,0,0,0,0,1,NULL,plain_a)==KSN_INVALID);
    assert(FETCHES==before&&(int)DECODES==decoded);
    return spans*1;
}

/* 2. Crop and stretch, direct and behind a group, with the (variant,frame) key
 *    changing every frame so the invalidation itself is under test. */
static unsigned long long still_scenes(unsigned long long *worst){
    /* The 64x64 port bounds every source window: 1X follows the destination
     * extent from the crop origin, stretch carries its own window. */
    const scene cases[]={
        {{10,10,74,42},0,0,64,64,0,0,0,255,KSN_IMAGE_1X,0,false},
        {{50,30,110,62},4,3,60,64,0,0,0,255,KSN_IMAGE_1X,1,false},
        {{20,12,84,44},0,0,64,32,0,0,0,255,KSN_IMAGE_STRETCH,0,false},
        {{90,40,154,72},4,4,56,28,0,0,0,193,KSN_IMAGE_STRETCH,1,false},
    };
    unsigned long long frames=0;
    for(unsigned c=0;c<sizeof(cases)/sizeof(cases[0]);c++){
        for(unsigned i=0;i<PEERS;i++)open_peer(i);
        for(unsigned i=0;i<PEERS;i++)mount(&peers[i],&cases[c]);
        render_pair(true,"still",worst);frames++;
        for(unsigned step=1;step<=72;step++){
            scene s=cases[c];
            int x=s.bounds.x0+step-36,y=s.bounds.y0+step/3-12;
            s.bounds=(ksn_rect){x,y,x+(s.bounds.x1-s.bounds.x0),y+(s.bounds.y1-s.bounds.y0)};
            s.rotation=s.scale==KSN_IMAGE_STRETCH?(step*13)&1023:0;
            s.variant=step%12;s.frame=step%6;
            for(unsigned i=0;i<PEERS;i++)update(&peers[i],&s);
            render_pair(true,"still",worst);frames++;
        }
    }
    return frames;
}

/* 3. Every rotation of the 64x64 source, direct and behind a group. */
static unsigned long long rotate_scenes(unsigned long long *worst){
    unsigned long long frames=0;
    for(unsigned group=0;group<2;group++){
        for(unsigned i=0;i<PEERS;i++)open_peer(i);
        const scene s={{88,36,152,100},0,0,64,64,0,0,0,(unsigned)(group?193:255),
                       KSN_IMAGE_STRETCH,(uint8_t)(group?137:0),false};
        for(unsigned i=0;i<PEERS;i++)mount(&peers[i],&s);
        render_pair(true,group?"rotate-group":"rotate",worst);frames++;
        for(unsigned rotation=1;rotation<1024;rotation++){
            scene r=s;r.rotation=rotation;r.variant=rotation%12;r.frame=(rotation/8)%6;
            for(unsigned i=0;i<PEERS;i++)update(&peers[i],&r);
            render_pair(true,"rotate",worst);frames++;
        }
    }
    return frames;
}

/* 4. Two registered images, identical bytes at different addresses, drawn in
 *    the same frames: the key has to notice the different source bytes. */
static unsigned long long two_image_scenes(unsigned long long *worst){
    uint8_t *copy=malloc(pets_bytes);assert(copy);memcpy(copy,pets,pets_bytes);
    for(unsigned i=0;i<PEERS;i++)open_peer(i);
    ksn_image_port extra;assert(ksn_pet_image(copy,pets_bytes,&extra)==KSN_OK);
    for(unsigned i=0;i<PEERS;i++)
        assert(ksn_core_register_image(&peers[i].core,KSN_APP,&extra,&peers[i].second)==KSN_OK);
    const scene s={{4,4,68,36},0,0,64,64,0,0,0,255,KSN_IMAGE_1X,0,true};
    for(unsigned i=0;i<PEERS;i++)mount(&peers[i],&s);
    render_pair(true,"two-image",worst);
    unsigned long long frames=1;
    for(unsigned step=1;step<=72;step++){
        scene u=s;u.variant=step%12;u.frame=step%6;
        for(unsigned i=0;i<PEERS;i++)update(&peers[i],&u);
        render_pair(true,"two-image",worst);frames++;
    }
    free(copy);
    return frames;
}

/* 5. The 120-frame animated transform track on the real pet image: the arms
 *    fixed, then the arm under test alternating per frame. */
static unsigned long long animated_track(bool alternate,unsigned long long *worst){
    static ksn_core_animation_block blocks[PEERS][2];
    for(unsigned i=0;i<PEERS;i++){
        open_peer(i);
        assert(ksn_core_enable_animation(&peers[i].core,&blocks[i][0],&blocks[i][1])==KSN_OK);
        assert(ksn_core_animation_bytes(&peers[i].core)<=1024);
        /* A key no earlier scene uses, so this track starts from a cold cache. */
        const scene s={{10,20,74,84},0,0,64,64,0,1,1,193,KSN_IMAGE_STRETCH,0,false};
        ksn_draw d=draw_of(&s,peers[i].image,s.bounds);
        ksn_motion motion={.count=1,.property=KSN_TRANSFORM,.from.pose={{10,20,74,84},0},
            .to.pose={{150,40,214,104},2048},.duration_ms=1000,.easing=KSN_LINEAR,.repeat=KSN_LOOP};
        ksn_tx tx;ksn_animation animation;
        assert(peers[i].client.ops->begin(peers[i].client.ctx,KSN_REPLACE,&tx)==KSN_OK);
        assert(peers[i].client.ops->background(peers[i].client.ctx,tx,0x183c60ff)==KSN_OK);
        assert(peers[i].client.ops->add(peers[i].client.ctx,tx,&d,&peers[i].ref)==KSN_OK);
        motion.first=peers[i].ref;
        assert(peers[i].client.ops->animate(peers[i].client.ctx,tx,&motion,&animation)==KSN_OK);
        assert(peers[i].client.ops->end(peers[i].client.ctx,tx)==KSN_OK);
    }
    render_pair(alternate?false:true,"animated-f0",worst);
    /* The clock starts once the REPLACE frame is presented, not before. */
    for(unsigned i=0;i<PEERS;i++)ksn_core_start_animations(&peers[i].core,0);
    unsigned long long frames=1;
    for(unsigned f=1;f<=120;f++){
        for(unsigned i=0;i<PEERS;i++){
            ksn_tx tx={0};
            assert(ksn_core_advance_animations(&peers[i].core,(uint64_t)f*33334u,false,&tx)==KSN_OK);
        }
        render_pair(alternate?(f&1u)!=0:true,"animated",worst);
        frames++;
    }
    SET_ARM(true);
    return frames;
}

/* 6. The rotated 64x64 scene at a fixed key, one line per frame and arm: this
 *    is the measurement docs/perf/kasane-image-transform-recon.md §4 quotes
 *    (2,112 fetches per frame, a re-decoded row for each of them). */
#if STATS
static void measure_rotate(void){
    const unsigned rotations[]={0,128,200,341,512,683,900};
    /* Variant 1: a key no earlier scene filled, so the first frame pays the
     * whole row set and every later frame pays nothing. */
    const scene base={{88,36,152,100},0,0,64,64,0,1,0,255,KSN_IMAGE_STRETCH,0,false};
    for(unsigned arm=0;arm<PEERS;arm++){
        unsigned long long fetches=0,decodes=0;
        open_peer(arm);
        mount(&peers[arm],&base);
        for(unsigned i=0;i<sizeof(rotations)/sizeof(rotations[0]);i++){
            scene s=base;s.rotation=rotations[i];
            if(i)update(&peers[arm],&s);
            unsigned long long before_f=peers[arm].fetches,before_d=peers[arm].decodes;
            render_peer(arm,arm!=0);
            fetches+=peers[arm].fetches-before_f;decodes+=peers[arm].decodes-before_d;
            printf("  rotation %4u %s arm: fetches %6llu decodes %6llu\n",rotations[i],
                arm?"cache":"old  ",peers[arm].fetches-before_f,peers[arm].decodes-before_d);
        }
        printf("  total %s arm: fetches %llu decodes %llu\n",arm?"cache":"old  ",fetches,decodes);
    }
}
#endif

int main(void){
    FILE *f=fopen("apps/pet/assets/pets-compact.bin","rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long length=ftell(f);assert(length>800);rewind(f);
    pets_bytes=(size_t)length;pets=malloc(pets_bytes);assert(pets);
    assert(fread(pets,1,pets_bytes,f)==pets_bytes);fclose(f);
    unsigned long long worst=0,compared=0;
    compared+=provider_arms(&worst);
    compared+=still_scenes(&worst);
    compared+=rotate_scenes(&worst);
    compared+=two_image_scenes(&worst);
    unsigned long long frames_fixed=animated_track(false,&worst);
    unsigned long long off_fetches=peers[0].fetches,off_decodes=peers[0].decodes;
    unsigned long long cache_fetches=peers[1].fetches,cache_decodes=peers[1].decodes;
    unsigned int cache_frames=peers[1].frames,off_frames=peers[0].frames;
    uint32_t hash_off=peers[0].hash,hash_fixed=peers[1].hash;
    compared++;
    unsigned long long frames_alt=animated_track(true,&worst);
    /* The arm under test is always peer 1; peer 0 is the reference. */
    unsigned long long alt_fetches=peers[1].fetches,alt_decodes=peers[1].decodes;
    unsigned long long alt_off_fetches=peers[0].fetches,alt_off_decodes=peers[0].decodes;
    uint32_t hash_alt=peers[1].hash,hash_alt_off=peers[0].hash;
    compared++;
    (void)alt_off_decodes;(void)alt_off_fetches;
    if(frames_fixed!=121||frames_alt!=121||cache_frames!=121||off_frames!=121||
       hash_off!=hash_fixed||hash_alt!=hash_off||hash_alt_off!=hash_off){
        printf("pet row cache MISMATCH animated frames=%llu/%llu hashes %08x/%08x/%08x/%08x\n",
            frames_fixed,frames_alt,hash_off,hash_fixed,hash_alt,hash_alt_off);assert(false);}
    if(off_fetches!=cache_fetches||off_fetches!=alt_fetches||off_fetches!=alt_off_fetches){
        printf("pet row cache MISMATCH fetch counts %llu/%llu/%llu/%llu\n",
            off_fetches,cache_fetches,alt_fetches,alt_off_fetches);assert(false);}
    printf("animated 120 frames: %llu fetches in all four arm runs, decodes old/cache "
           "%llu/%llu, alternating arm decodes %llu of %llu fetches\n",
        off_fetches,off_decodes,cache_decodes,alt_decodes,alt_fetches);
#if STATS
    if(cache_decodes>=off_decodes||alt_decodes>=alt_off_decodes||!cache_decodes||!alt_decodes){
        printf("pet row cache MISSING SAVING: fetches %llu, decodes %llu/%llu/%llu/%llu\n",
            off_fetches,off_decodes,cache_decodes,alt_decodes,alt_off_decodes);assert(false);}
    printf("rotate 64x64 fixed key, per frame:\n");
    measure_rotate();
#else
    printf("pre-change provider: no cache switch, no counters\n");
#endif
    SET_ARM(true);
#if STATS
    printf("pet row cache arms PASS: %llu panel comparisons, worst pixel step %llu, "
           "hash %08x, cache decodes %.3f%% of the fetches, %llu of %llu\n",
        compared,worst,hash_off,100.0*(double)cache_decodes/(double)cache_fetches,
        cache_decodes,cache_fetches);
#else
    printf("pre-change provider PASS: %llu panel comparisons, worst pixel step %llu, "
           "hash %08x\n",compared,worst,hash_off);
#endif
    free(pets);
    return 0;
}
