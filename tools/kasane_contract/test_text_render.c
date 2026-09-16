#include "core_fixture.h"
#include "ksn_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t panel[240*135],strip[240*8],reference[240*135];
static unsigned sends;static int fault_y=-1;static bool unavailable;
static uint16_t *buffer(void *p){(void)p;return strip;}
static ksn_result send(void *p,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)p;sends++;memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));return KSN_OK;
}
static unsigned coverage(int x,int y,unsigned reveal){
    static const unsigned values[]={0,1,127,128,254,255};
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
    ksn_text_port text={.span=span};ksn_display_port display={NULL,buffer,send,240,135,8,&text};
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
    puts("text render PASS: coverage/alpha/group, clipping, UTF-8 reveal, bands, provider failure/repair");
}
