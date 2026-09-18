/* The narrow arm against the full-width one, pixel for pixel.
 *
 * ksn_core_damage now carries a column range per band, and a port that offers
 * present_rect gets only the damaged columns composited and only those
 * transferred. That is a change to WHICH pixels the renderer writes, so the
 * thing to prove is that the pixels the PANEL ends up with do not move: the
 * columns the narrow arm skips have to be exactly the columns whose value did
 * not change, or the display is quietly wrong in a way no timing number shows
 * (docs/perf/kasane-text-damage.md, and design-system.md 11's "部分描画: 同じ
 * 状態の全面スカラー描画と全画素一致").
 *
 * Two cores run the same script in lockstep. One is given a port without
 * present_rect, so every band is widened to 240 and composited the way it
 * always was; the other is given present_rect and narrows. After every frame
 * the two panels must be identical -- and the narrow one must actually have
 * narrowed, or the test would pass by doing nothing, which is why the bytes are
 * compared too.
 *
 * The strip is poisoned before each present so a column the narrow arm fails to
 * write shows up as poison rather than as whatever the previous band left, and
 * the panel starts poisoned too: an untransferred column keeps its previous
 * value, which is the whole point, and a first frame that skipped a column
 * would leave poison on the panel where the wide arm has a pixel. */
#include "core_fixture.h"
#include "ksn_render.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"narrow line %d: %s\n",__LINE__,#x);return 1;}}while(0)

#define POISON 0xa55a

typedef struct {
    uint16_t panel[240*135];
    uint16_t strip[240*8];
    uint32_t bytes;
    unsigned transfers,rect_transfers;
} arm_t;
static arm_t wide,narrow;

static uint16_t *wide_strip(void *ctx){(void)ctx;return wide.strip;}
static uint16_t *narrow_strip(void *ctx){(void)ctx;return narrow.strip;}
static ksn_result wide_send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;
    memcpy(wide.panel+y*240,pixels,(size_t)rows*240*sizeof(uint16_t));
    wide.bytes+=(uint32_t)rows*240*2;wide.transfers++;return KSN_OK;
}
static ksn_result narrow_send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;
    memcpy(narrow.panel+y*240,pixels,(size_t)rows*240*sizeof(uint16_t));
    narrow.bytes+=(uint32_t)rows*240*2;narrow.transfers++;return KSN_OK;
}
/* The panel takes exactly the window, the way board_present_rect's CASET does.
 * Nothing outside it is read, so a renderer that left a column unwritten cannot
 * hide behind the strip still holding the right value from an earlier band. */
static ksn_result narrow_send_rect(void *ctx,uint16_t x,uint16_t y,uint16_t cols,
                                   uint16_t rows,const uint16_t *pixels){
    (void)ctx;
    for(unsigned r=0;r<rows;r++)
        memcpy(narrow.panel+(y+r)*240+x,pixels+(size_t)r*240+x,(size_t)cols*sizeof(uint16_t));
    narrow.bytes+=(uint32_t)rows*cols*2;narrow.transfers++;narrow.rect_transfers++;return KSN_OK;
}

KSN_TEST_CORE(wide_core,static);
KSN_TEST_CORE(narrow_core,static);

/* One script, run against both cores. Each step is a PATCH whose damage is
 * deliberately small and off-centre, plus the REPLACE that sets the scene up. */
static ksn_result build(ksn_core *core,ksn_client app,unsigned step,ksn_ref *refs){
    ksn_tx tx;ksn_result r;
    if(step==0){
        ksn_draw d[5]={
            {.kind=KSN_RECT,.bounds={0,0,240,135},.clip={0,0,240,135},.opacity=255,
             .data.shape={0x1b2f44ff,0,0}},
            {.kind=KSN_ROUND_RECT,.bounds={16,62,224,100},.clip={0,0,240,135},.opacity=255,
             .data.shape={0x12334aff,6,0}},
            /* The counter: the case the whole change exists for -- a few columns
             * inside a wide band, low on the panel. */
            {.kind=KSN_RECT,.bounds={28,77,40,89},.clip={0,0,240,135},.opacity=255,
             .data.shape={0x8ef0c4ff,0,0}},
            {.kind=KSN_GRADIENT,.bounds={16,12,226,34},.clip={0,0,240,135},.opacity=200,
             .data.gradient={0x69cdeeff,0x0d2b3cff,1,0,true}},
            {.kind=KSN_STROKE,.bounds={120,100,200,130},.clip={0,0,240,135},.opacity=180,
             .data.shape={0xf5bb69ff,0,2}},
        };
        if((r=app.ops->begin(app.ctx,KSN_REPLACE,&tx))!=KSN_OK)return r;
        if((r=app.ops->background(app.ctx,tx,0x071425ff))!=KSN_OK)return r;
        for(unsigned i=0;i<5;i++)
            if((r=app.ops->add(app.ctx,tx,&d[i],&refs[i]))!=KSN_OK)return r;
        /* The last two are one isolated group, so the group arms narrow too. */
        if((r=ksn_core_group(core,KSN_APP,tx,refs[3],2,168))!=KSN_OK)return r;
        return app.ops->end(app.ctx,tx);
    }
    if((r=app.ops->begin(app.ctx,KSN_PATCH,&tx))!=KSN_OK)return r;
    switch(step%5){
    case 1:{ /* The counter grows a column: six pixels of damage. */
        ksn_change c={.property=KSN_SET_RECT,
                      .value.rect={28,77,(int16_t)(34+(int16_t)(step%7)),89}};
        if((r=app.ops->change(app.ctx,tx,refs[2],&c))!=KSN_OK)return r;
        break;}
    case 2:{ /* And changes colour without moving. */
        ksn_change c={.property=KSN_SET_COLOR,
                      .value.color=(step&8)?0xf07878ff:0x8ef0c4ff};
        if((r=app.ops->change(app.ctx,tx,refs[2],&c))!=KSN_OK)return r;
        break;}
    case 3:{ /* A clip that cuts the stroke, inside the group. */
        ksn_change c={.property=KSN_SET_CLIP,
                      .value.rect={(int16_t)(120+(int16_t)(step%11)),100,200,130}};
        if((r=app.ops->change(app.ctx,tx,refs[4],&c))!=KSN_OK)return r;
        break;}
    case 4:{ /* Hide and show the round rect: damage is its whole box. */
        ksn_change c={.property=KSN_SET_VISIBLE,.value.visible=(step%10)<5};
        if((r=app.ops->change(app.ctx,tx,refs[1],&c))!=KSN_OK)return r;
        break;}
    default:{ /* A move across a band boundary: old box and new box both count. */
        int16_t y0=(int16_t)(60+(int16_t)(step%9));
        ksn_change c={.property=KSN_SET_RECT,.value.rect={16,y0,224,(int16_t)(y0+38)}};
        if((r=app.ops->change(app.ctx,tx,refs[1],&c))!=KSN_OK)return r;
        break;}
    }
    return app.ops->end(app.ctx,tx);
}

int main(void){
    ksn_core_init(&wide_core);ksn_core_init(&narrow_core);
    ksn_client wide_app=ksn_core_client(&wide_core,KSN_APP);
    ksn_client narrow_app=ksn_core_client(&narrow_core,KSN_APP);
    ksn_display_port wide_port={NULL,wide_strip,wide_send,240,135,8,NULL,NULL};
    ksn_display_port narrow_port={NULL,narrow_strip,narrow_send,240,135,8,NULL,narrow_send_rect};
    ksn_render_stats wide_stats,narrow_stats;
    ksn_ref wide_refs[5],narrow_refs[5];
    for(unsigned i=0;i<240*135;i++){wide.panel[i]=POISON;narrow.panel[i]=POISON;}
    unsigned narrowed_frames=0;
    for(unsigned step=0;step<120;step++){
        CHECK(build(&wide_core,wide_app,step,wide_refs)==KSN_OK);
        CHECK(build(&narrow_core,narrow_app,step,narrow_refs)==KSN_OK);
        for(unsigned i=0;i<240*8;i++){wide.strip[i]=POISON;narrow.strip[i]=POISON;}
        wide.bytes=narrow.bytes=0;
        CHECK(ksn_render_rects(&wide_core,&wide_port,&wide_stats)==KSN_OK);
        CHECK(ksn_render_rects(&narrow_core,&narrow_port,&narrow_stats)==KSN_OK);
        CHECK(wide_stats.bands==narrow_stats.bands);
        CHECK(narrow.bytes<=wide.bytes);
        if(narrow.bytes<wide.bytes)narrowed_frames++;
        for(unsigned i=0;i<240*135;i++){
            if(wide.panel[i]!=narrow.panel[i]){
                fprintf(stderr,"step %u pixel %u,%u: wide %04x narrow %04x\n",
                        step,i%240,i/240,wide.panel[i],narrow.panel[i]);
                return 1;
            }
        }
        /* The first frame is a REPLACE: every pixel must have been written, so
         * no poison may survive on either panel. */
        if(step==0)for(unsigned i=0;i<240*135;i++)CHECK(narrow.panel[i]!=POISON);
    }
    /* If nothing ever narrowed, the comparison above proved nothing. */
    CHECK(narrowed_frames>=60);
    CHECK(narrow.rect_transfers>0&&narrow.bytes<=wide.bytes);
    printf("narrow damage PASS: 120 frames identical to the full-width arm, "
           "%u of them narrowed (%u windowed transfers)\n",
           narrowed_frames,narrow.rect_transfers);
    return 0;
}
