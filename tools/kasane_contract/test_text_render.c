#include "core_fixture.h"
#include "ksn_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t panel[240*135],reference[240*135];
static uint16_t strip[240*8+8] __attribute__((aligned(16)));
static unsigned sends;static int fault_y=-1;static bool unavailable;
static bool binary_mode,unaligned_mode;
#ifdef KSN_TEXT_PIE_COUNT
extern uint32_t ksn_text_pie_blocks;
extern uint32_t ksn_text_pie_mixed_blocks;
#endif
static uint16_t *buffer(void *p){(void)p;return strip+(unaligned_mode?1:0);}
static ksn_result send(void *p,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)p;sends++;memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));return KSN_OK;
}
static unsigned coverage(int x,int y,unsigned reveal){
    static const unsigned values[]={0,1,127,128,254,255};
    if(binary_mode){
        if(x<8||x>=93||y<7||y>=22)return 0;
        switch((unsigned)x&31u){
        case 0:case 1:case 2:case 3:case 4:case 5:case 6:case 7:return 0;
        case 8:case 9:case 10:case 11:case 12:case 13:case 14:case 15:return 255;
        case 16:case 17:case 18:case 19:case 20:case 21:case 22:case 23:
            return ((x+y)&1)?255:0;
        default:return ((x*13+y*7)&4)?255:0;
        }
    }
    if(x<2||x>=77||y<7||y>=22||x>=-3+(int)reveal*40)return 0;
    return values[(x+y)%6];
}
static ksn_result span(void *p,const ksn_draw *d,uint16_t reveal,int x,int y,unsigned n,uint8_t *out){
    (void)p;assert(d->kind==KSN_TEXT&&n<=64);
    if(unavailable)return KSN_UNSUPPORTED;
    if(n&&y==fault_y)return KSN_IO;
    for(unsigned i=0;i<n;i++)out[i]=(uint8_t)coverage(x+(int)i,y,reveal);
    return KSN_OK;
}
static unsigned mul(unsigned a,unsigned b){return (a*b+127)/255;}
static uint16_t expected(int x,int y,unsigned opacity,bool grouped,unsigned reveal){
    unsigned a=mul(mul(183,coverage(x,y,reveal)),opacity),r,g,b;
    // Background 0x183c60 expands to 24,60,99 from RGB565.
    if(grouped){unsigned final=mul(a,137);if(!final)return 0x19ec;
        r=mul(mul(161,a),137)+mul(24,255-final);
        g=mul(mul(95,a),137)+mul(60,255-final);
        b=mul(mul(55,a),137)+mul(99,255-final);
    }else{
        r=(161*a+24*(255-a)+127)/255;
        g=(95*a+60*(255-a)+127)/255;
        b=(55*a+99*(255-a)+127)/255;
    }
    return (uint16_t)((r>>3)<<11|(g>>2)<<5|(b>>3));
}
int main(void){
    KSN_TEST_CORE(core,);ksn_core_init(&core);ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_text_port text={.span=span};ksn_display_port display={NULL,buffer,send,240,135,8,&text,NULL};
    ksn_tx tx;ksn_ref ref;ksn_render_stats stats;
    ksn_draw d={.kind=KSN_TEXT,.bounds={-3,5,145,29},.clip={2,7,77,22},.opacity=255,
        .data.text={.utf8="\xe3\x81\x82" "A",.bytes=4,.capacity=16,.font=KSN_BODY,.color=0xa15f37b7}};
    for(unsigned group=0;group<2;group++)for(unsigned opacity=0;opacity<256;opacity++){
        d.opacity=(uint8_t)opacity;
        assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
        assert(app.ops->background(app.ctx,tx,0x183c60ff)==KSN_OK);
        assert(app.ops->add(app.ctx,tx,&d,&ref)==KSN_OK);
        if(group)assert(ksn_core_group(&core,KSN_APP,tx,ref,1,137)==KSN_OK);
        assert(app.ops->end(app.ctx,tx)==KSN_OK);
        assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        for(int y=0;y<135;y++)for(int x=0;x<240;x++)
            assert(panel[y*240+x]==expected(x,y,opacity,group!=0,2));
    }
    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change change={.property=KSN_SET_REVEAL,.value.reveal=1};
    assert(app.ops->change(app.ctx,tx,ref,&change)==KSN_OK);assert(app.ops->end(app.ctx,tx)==KSN_OK);
    unavailable=true;sends=0;assert(ksn_render_rects(&core,&display,&stats)==KSN_UNSUPPORTED&&!sends);
    unavailable=false;fault_y=8;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_IO&&sends==1);
    fault_y=-1;assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    for(int y=0;y<135;y++)for(int x=0;x<240;x++)assert(panel[y*240+x]==expected(x,y,255,true,1));
    memcpy(reference,panel,sizeof(panel));ksn_core_invalidate(&core);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK&&!memcmp(reference,panel,sizeof(panel)));
    /* Separate ungrouped text scene: zero, solid, and mixed binary-mask blocks
     * plus a clipped tail. Repaint the same core with
     * the switch flipped so the byte comparison includes the whole panel. */
    {
        KSN_TEST_CORE(pie_core,);ksn_core_init(&pie_core);
        ksn_client pie_app=ksn_core_client(&pie_core,KSN_APP);
        ksn_draw label={.kind=KSN_TEXT,.bounds={8,5,101,24},.clip={8,7,93,22},
            .opacity=193,.data.text={.utf8="PIE",.bytes=3,.capacity=8,
                                     .font=KSN_BODY,.color=0xa15f37b7}};
        binary_mode=true;text.binary_coverage=true;
        assert(pie_app.ops->begin(pie_app.ctx,KSN_REPLACE,&tx)==KSN_OK);
        assert(pie_app.ops->background(pie_app.ctx,tx,0x183c60ff)==KSN_OK);
        assert(pie_app.ops->add(pie_app.ctx,tx,&label,&ref)==KSN_OK);
        assert(pie_app.ops->end(pie_app.ctx,tx)==KSN_OK);
        g_ksn_text_pie=0;
        assert(ksn_render_rects(&pie_core,&display,&stats)==KSN_OK);
        memcpy(reference,panel,sizeof(panel));
        ksn_core_invalidate(&pie_core);
        g_ksn_text_pie=1;
#ifdef KSN_TEXT_PIE_COUNT
        ksn_text_pie_blocks=0;
        ksn_text_pie_mixed_blocks=0;
#endif
        assert(ksn_render_rects(&pie_core,&display,&stats)==KSN_OK);
        assert(!memcmp(reference,panel,sizeof(panel)));
#ifdef KSN_TEXT_PIE_COUNT
        assert(ksn_text_pie_blocks>0);
        assert(ksn_text_pie_mixed_blocks>0);
#endif
        ksn_core_invalidate(&pie_core);fault_y=8;
        assert(ksn_render_rects(&pie_core,&display,&stats)==KSN_IO);
        fault_y=-1;
        assert(ksn_render_rects(&pie_core,&display,&stats)==KSN_OK);
        assert(!memcmp(reference,panel,sizeof(panel)));
        text.binary_coverage=false;ksn_core_invalidate(&pie_core);
#ifdef KSN_TEXT_PIE_COUNT
        ksn_text_pie_blocks=0;
#endif
        assert(ksn_render_rects(&pie_core,&display,&stats)==KSN_OK);
        assert(!memcmp(reference,panel,sizeof(panel)));
#ifdef KSN_TEXT_PIE_COUNT
        assert(ksn_text_pie_blocks==0);
#endif
        text.binary_coverage=true;
        /* PIE loads/stores require 16-byte alignment: an unaligned borrowed
         * strip must use the scalar path and preserve the same panel. */
        unaligned_mode=true;ksn_core_invalidate(&pie_core);
#ifdef KSN_TEXT_PIE_COUNT
        ksn_text_pie_blocks=0;
#endif
        assert(ksn_render_rects(&pie_core,&display,&stats)==KSN_OK);
        assert(!memcmp(reference,panel,sizeof(panel)));
#ifdef KSN_TEXT_PIE_COUNT
        assert(ksn_text_pie_blocks==0);
#endif
        unaligned_mode=false;
        /* The coarse scalar colour arm is approximate; the exact PIE arm must
         * not silently override it when that independent switch is selected. */
        g_ksn_scale256=1;
        g_ksn_text_pie=0;ksn_core_invalidate(&pie_core);
        assert(ksn_render_rects(&pie_core,&display,&stats)==KSN_OK);
        memcpy(reference,panel,sizeof(panel));
        g_ksn_text_pie=1;ksn_core_invalidate(&pie_core);
#ifdef KSN_TEXT_PIE_COUNT
        ksn_text_pie_blocks=0;
#endif
        assert(ksn_render_rects(&pie_core,&display,&stats)==KSN_OK);
        assert(!memcmp(reference,panel,sizeof(panel)));
#ifdef KSN_TEXT_PIE_COUNT
        assert(ksn_text_pie_blocks==0);
#endif
        /* A command whose span starts one pixel off the eight-byte boundary
         * cannot pair the aligned panel blocks with aligned mask bytes. */
        g_ksn_scale256=0;
        label.bounds.x0=9;label.clip.x0=9;
        assert(pie_app.ops->begin(pie_app.ctx,KSN_REPLACE,&tx)==KSN_OK);
        assert(pie_app.ops->background(pie_app.ctx,tx,0x183c60ff)==KSN_OK);
        assert(pie_app.ops->add(pie_app.ctx,tx,&label,&ref)==KSN_OK);
        assert(pie_app.ops->end(pie_app.ctx,tx)==KSN_OK);
        g_ksn_text_pie=0;
        assert(ksn_render_rects(&pie_core,&display,&stats)==KSN_OK);
        memcpy(reference,panel,sizeof(panel));
        g_ksn_text_pie=1;ksn_core_invalidate(&pie_core);
#ifdef KSN_TEXT_PIE_COUNT
        ksn_text_pie_blocks=0;
#endif
        assert(ksn_render_rects(&pie_core,&display,&stats)==KSN_OK);
        assert(!memcmp(reference,panel,sizeof(panel)));
#ifdef KSN_TEXT_PIE_COUNT
        assert(ksn_text_pie_blocks==0);
#endif
        g_ksn_scale256=0;g_ksn_text_pie=0;binary_mode=false;text.binary_coverage=false;
    }
    puts("text render PASS: coverage/alpha/group, clipping, UTF-8 reveal, bands, provider failure/repair");
}
