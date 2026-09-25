#include "core_fixture.h"
#include "app_legacy_presenter.h"
#include "ksn_view_host.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if(!(x)){fprintf(stderr,"music light fast line %d: %s\n",__LINE__,#x);return 1;} } while(0)
KSN_TEST_CORE(fast_core,static);
KSN_TEST_CORE(full_core,static);
static ksn_cache fast_cache,full_cache;
static ksn_cache_command_block fast_commands,full_commands;
static ksn_cache_text_block fast_text,full_text;
typedef struct {uint16_t strip[240*8],panel[240*135];int fail_y;} display;
static display fast_display={.fail_y=-1},full_display={.fail_y=-1};
static uint16_t committed[240*135];
static uint16_t *strip(void *ctx){return ((display *)ctx)->strip;}
static ksn_result transfer(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    display *d=ctx;
    memcpy(d->panel+y*240,pixels,(size_t)rows*240*sizeof(*pixels));
    return (int)y==d->fail_y?KSN_IO:KSN_OK;
}
static ksn_result span(void *ctx,const ksn_draw *draw,uint16_t reveal,
                       int x,int y,unsigned count,uint8_t *out){
    (void)ctx;(void)draw;(void)reveal;(void)x;(void)y;
    if(count)memset(out,255,count);
    return KSN_OK;
}
static const ksn_text_port text_port={.span=span};
static ksn_display_port port(display *d){
    return (ksn_display_port){d,strip,transfer,240,135,8,&text_port,NULL};
}
static int light_indices(const ksn_presenter_plan *p,uint8_t indices[3]){
    static const ksn_rgba colors[]={0x78c8ffffu,0x3c78aaffu,0x1e3c5affu};
    for(unsigned light=0;light<3;light++)indices[light]=UINT8_MAX;
    for(unsigned i=0;i<p->count;i++){
        if(p->items[i].kind!=KSN_RECT)continue;
        for(unsigned light=0;light<3;light++)
            if(p->items[i].color==colors[light])indices[light]=(uint8_t)i;
    }
    return 0;
}
static int geometry(void){
    ksn_presenter_values values={.playing=true};
    ksn_presenter_plan plan;
    static const ksn_rgba colors[]={0x78c8ffffu,0x3c78aaffu,0x1e3c5affu};
    for(unsigned width=25;width<=240;width++){
        unsigned track=width-24;
        for(unsigned phase=0;phase<270;phase++){
            values.phase=phase;
            CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&values,
                                     (uint16_t)width,135,&plan)==KSN_OK);
            uint64_t key=ksn_presenter_music_light_key(phase,(uint16_t)track);
            for(unsigned light=0;light<3;light++){
                unsigned part=(unsigned)((key>>(light*16))&0xffffu);
                const ksn_presenter_item *found=NULL;
                for(unsigned i=0;i<plan.count;i++)
                    if(plan.items[i].kind==KSN_RECT&&plan.items[i].color==colors[light])
                        found=&plan.items[i];
                CHECK((found!=NULL)==((part&0xffu)!=0));
                if(found){
                    CHECK(found->bounds.x0==12+(int)(part>>8));
                    CHECK(found->bounds.x1==found->bounds.x0+(int)(part&0xffu));
                    CHECK(found->bounds.y0==109&&found->bounds.y1==111);
                }
            }
        }
    }
    return 0;
}
int main(void){
    CHECK(geometry()==0);
    CHECK(ksn_cache_bind(&fast_cache,&fast_commands,&fast_text)==KSN_OK);
    CHECK(ksn_cache_bind(&full_cache,&full_commands,&full_text)==KSN_OK);
    ksn_view_host fast_host,full_host;
    ksn_view_host_init(&fast_host,&fast_core,&fast_cache,17);
    ksn_view_host_init(&full_host,&full_core,&full_cache,17);
    ksn_view *fast=ksn_view_host_endpoint(&fast_host,KSN_APP);
    ksn_view *full=ksn_view_host_endpoint(&full_host,KSN_APP);
    ksn_rect viewport={0,0,240,135};
    ksn_presenter_values values={.playing=true,.phase=18};
    CHECK(ksn_presenter_copy_text(values.text[0],&values.bytes[0],"TRACK",5)==KSN_OK);
    CHECK(ksn_presenter_copy_text(values.text[1],&values.bytes[1],"PLAYING  0s",11)==KSN_OK);
    ksn_presenter_plan plan;
    CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&values,240,135,&plan)==KSN_OK);
    plan.patchable=true;
    ksn_ref fast_refs[KSN_PRESENTER_ITEMS]={0},full_refs[KSN_PRESENTER_ITEMS]={0};
    uint8_t indices[3];ksn_tx tx;ksn_render_stats stats;
    light_indices(&plan,indices);
    CHECK(ksn_presenter_submit(fast,viewport,(ksn_resource){0},&plan,fast_refs,&tx)==KSN_OK);
    CHECK(ksn_presenter_submit(full,viewport,(ksn_resource){0},&plan,full_refs,&tx)==KSN_OK);
    ksn_display_port fast_port=port(&fast_display),full_port=port(&full_display);
    CHECK(ksn_view_host_present(&fast_host,&fast_port,&stats)==KSN_OK);
    CHECK(ksn_view_host_present(&full_host,&full_port,&stats)==KSN_OK);
    CHECK(memcmp(fast_display.panel,full_display.panel,sizeof(fast_display.panel))==0);
    uint64_t old_key=ksn_presenter_music_light_key(18,216);
    unsigned patches=0,replaces=0,repairs=0;
    for(unsigned phase=19;phase<288;phase++){
        values.phase=phase;
        CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&values,240,135,&plan)==KSN_OK);
        plan.patchable=true;
        CHECK(ksn_presenter_submit(full,viewport,(ksn_resource){0},&plan,full_refs,&tx)==KSN_OK);
        CHECK(ksn_view_host_present(&full_host,&full_port,&stats)==KSN_OK);
        uint64_t key=ksn_presenter_music_light_key(phase,216);
        ksn_result result=KSN_UNSUPPORTED;
        if(key!=old_key)
            result=ksn_presenter_music_light_patch(fast,viewport,fast_refs,indices,
                                                    (uint8_t)(plan.count),old_key,key,&tx);
        if(result==KSN_UNSUPPORTED){
            CHECK(ksn_presenter_submit(fast,viewport,(ksn_resource){0},
                                       &plan,fast_refs,&tx)==KSN_OK);
            light_indices(&plan,indices);replaces++;
        }else CHECK(result==KSN_OK);
        if(result==KSN_OK)patches++;
        if(phase==19&&result==KSN_OK){
            memcpy(committed,fast_display.panel,sizeof(committed));
            fast_display.fail_y=104;
            CHECK(ksn_view_host_present(&fast_host,&fast_port,&stats)==KSN_IO);
            CHECK(ksn_view_cancel(fast,tx)==KSN_OK);
            fast_display.fail_y=-1;
            CHECK(ksn_view_host_present(&fast_host,&fast_port,&stats)==KSN_OK);
            CHECK(memcmp(fast_display.panel,committed,sizeof(committed))==0);
            CHECK(ksn_presenter_music_light_patch(fast,viewport,fast_refs,indices,
                                                  (uint8_t)plan.count,old_key,key,&tx)==KSN_OK);
            repairs++;
        }
        CHECK(ksn_view_host_present(&fast_host,&fast_port,&stats)==KSN_OK);
        CHECK(memcmp(fast_display.panel,full_display.panel,sizeof(fast_display.panel))==0);
        old_key=key;
    }
    CHECK(patches>100&&replaces>0&&repairs==1);
    /* The product presenter uses the generic PATCH for stable topology, not
     * only the diagnostic three-rectangle light shortcut above. Exercise
     * shorter/longer text on both sides of the same full-plan comparison. */
    static const char *titles[]={"日本語の曲名", "TRACK"};
    static const char *messages[]={"PAUSED  2s", "PLAYING  123s / 240s"};
    for(unsigned i=0;i<2;i++){
        CHECK(ksn_presenter_copy_text(values.text[0],&values.bytes[0],
                                      titles[i],strlen(titles[i]))==KSN_OK);
        CHECK(ksn_presenter_copy_text(values.text[1],&values.bytes[1],
                                      messages[i],strlen(messages[i]))==KSN_OK);
        CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&values,240,135,&plan)==KSN_OK);
        plan.patchable=true;
        CHECK(ksn_presenter_patch(fast,viewport,&plan,fast_refs,&tx)==KSN_OK);
        CHECK(ksn_presenter_submit(full,viewport,(ksn_resource){0},
                                   &plan,full_refs,&tx)==KSN_OK);
        CHECK(ksn_view_host_present(&fast_host,&fast_port,&stats)==KSN_OK);
        CHECK(ksn_view_host_present(&full_host,&full_port,&stats)==KSN_OK);
        CHECK(memcmp(fast_display.panel,full_display.panel,
                     sizeof(fast_display.panel))==0);
    }
    /* Production keeps other items' narrow clips and does not enable the
     * expensive all-item PATCH. Only movable light rectangles get a track
     * clip, so their bounds can change without a full-plan resubmission. */
    values.phase=18;
    CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&values,240,135,&plan)==KSN_OK);
    CHECK(!plan.patchable);
    light_indices(&plan,indices);
    CHECK(ksn_presenter_submit(fast,viewport,(ksn_resource){0},&plan,fast_refs,&tx)==KSN_OK);
    CHECK(ksn_presenter_submit(full,viewport,(ksn_resource){0},&plan,full_refs,&tx)==KSN_OK);
    CHECK(ksn_view_host_present(&fast_host,&fast_port,&stats)==KSN_OK);
    CHECK(ksn_view_host_present(&full_host,&full_port,&stats)==KSN_OK);
    CHECK(memcmp(fast_display.panel,full_display.panel,sizeof(fast_display.panel))==0);
    old_key=ksn_presenter_music_light_key(18,216);
    unsigned product_patches=0,product_replaces=0,product_repairs=0;
    for(unsigned phase=19;phase<101;phase++){
        values.phase=phase;
        CHECK(ksn_presenter_make(KSN_PRESENTER_MUSIC,&values,240,135,&plan)==KSN_OK);
        CHECK(!plan.patchable);
        CHECK(ksn_presenter_submit(full,viewport,(ksn_resource){0},
                                   &plan,full_refs,&tx)==KSN_OK);
        CHECK(ksn_view_host_present(&full_host,&full_port,&stats)==KSN_OK);
        uint64_t key=ksn_presenter_music_light_key(phase,216);
        if(key!=old_key){
            ksn_result result=ksn_presenter_music_light_patch(fast,viewport,
                fast_refs,indices,(uint8_t)plan.count,old_key,key,&tx);
            if(result==KSN_UNSUPPORTED){
                CHECK(ksn_presenter_submit(fast,viewport,(ksn_resource){0},
                                           &plan,fast_refs,&tx)==KSN_OK);
                light_indices(&plan,indices);product_replaces++;
            }else{CHECK(result==KSN_OK);product_patches++;}
            if(!product_repairs&&result==KSN_OK){
                memcpy(committed,fast_display.panel,sizeof(committed));
                fast_display.fail_y=104;
                CHECK(ksn_view_host_present(&fast_host,&fast_port,&stats)==KSN_IO);
                CHECK(ksn_view_cancel(fast,tx)==KSN_OK);
                fast_display.fail_y=-1;
                CHECK(ksn_view_host_present(&fast_host,&fast_port,&stats)==KSN_OK);
                CHECK(memcmp(fast_display.panel,committed,sizeof(committed))==0);
                CHECK(ksn_presenter_music_light_patch(fast,viewport,fast_refs,
                    indices,(uint8_t)plan.count,old_key,key,&tx)==KSN_OK);
                product_repairs++;
            }
            CHECK(ksn_view_host_present(&fast_host,&fast_port,&stats)==KSN_OK);
        }
        CHECK(memcmp(fast_display.panel,full_display.panel,
                     sizeof(fast_display.panel))==0);
        old_key=key;
    }
    CHECK(product_patches>30&&product_replaces>0&&product_repairs==1);
    printf("music light fast: PASS (geometry 216 tracks x 270 phases; "
           "%u PATCH, %u REPLACE, %u repair; 2 text PATCH; "
           "product %u PATCH/%u REPLACE/%u repair; 32400 pixels/frame)\n",
           patches,replaces,repairs,product_patches,product_replaces,product_repairs);
    return 0;
}
