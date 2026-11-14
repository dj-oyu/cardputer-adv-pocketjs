#include "core_fixture.h"
#include "ksn_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* The renderer's two text paths inside one binary: g_ksn_text_runs 0 (span's
 * coverage bytes) and 1 (the port's ink runs) must paint the same panel. The
 * port is binary and synthetic -- its ink is a fixed function of the pixel,
 * the command's origin and the reveal, served both ways -- so this checks the
 * renderer's half of the contract: every inked pixel takes the coverage loop's
 * operation for a 255 byte. Colour alpha x opacity reach effective alpha 0, 1,
 * 254 and 255 (solid), with the alpha table and the coarse colour arm on and
 * off, 8- and 4-row strips, negative origins and clipped boxes. The font's
 * half (runs' ink equals span's) is test_span_count.c and test_font.c. */
static uint16_t panel[240*135],reference[240*135];
static uint16_t strip[240*8] __attribute__((aligned(16)));
static uint16_t strip_rows=8;
static unsigned half;
static uint16_t *buffer(void *p){(void)p;return strip_rows==4?strip+(half++&1u)*4u*240u:strip;}
static ksn_result send(void *p,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)p;memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));return KSN_OK;
}
static bool ink(const ksn_draw *d,unsigned reveal,int x,int y){
    const int gx=x-d->bounds.x0,gy=y-d->bounds.y0;
    if(gx<0||gy<0||gy>=11||gx>=(int)reveal*7)return false;
    return ((gx*5+gy*3+(gx>>3))%7)<3||(gx%7)==0;
}
static ksn_result span(void *p,const ksn_draw *d,uint16_t reveal,int x,int y,unsigned n,uint8_t *out){
    (void)p;assert(d->kind==KSN_TEXT&&n<=64);
    for(unsigned i=0;i<n;i++)out[i]=ink(d,reveal,x+(int)i,y)?255:0;
    return KSN_OK;
}
static unsigned runs_called;
static void runs(void *p,const ksn_draw *d,uint16_t reveal,int x0,int x1,int y0,int y1,
                 ksn_ink_run run,void *user){
    (void)p;runs_called++;
    for(int y=y0;y<y1;y++){
        int start=-1;
        for(int x=x0;x<x1;x++){
            if(ink(d,reveal,x,y)){if(start<0)start=x;}
            else if(start>=0){run(user,start,y,(unsigned)(x-start));start=-1;}
        }
        if(start>=0)run(user,start,y,(unsigned)(x1-start));
    }
}
static const unsigned alphas[]={0,1,2,128,254,255};
static const unsigned opacities[]={0,1,128,200,254,255};
int main(void){
    KSN_TEST_CORE(core,);ksn_core_init(&core);ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_text_port text={.span=span,.runs=runs,.binary_coverage=true};
    ksn_display_port display={NULL,buffer,send,240,135,8,&text,NULL};
    ksn_tx tx;ksn_ref ref;ksn_render_stats stats;
    unsigned scenes=0;
    for(unsigned rows=0;rows<2;rows++)for(unsigned lut=0;lut<2;lut++)for(unsigned coarse=0;coarse<2;coarse++)
    for(unsigned ai=0;ai<sizeof alphas/sizeof alphas[0];ai++)
    for(unsigned oi=0;oi<sizeof opacities/sizeof opacities[0];oi++){
        strip_rows=rows?4:8;display.strip_rows=strip_rows;
        g_ksn_blend_lut_alpha=(int)lut;g_ksn_scale256=(int)coarse;
        const ksn_rgba color=0xa15f3700u|alphas[ai];
        ksn_draw a={.kind=KSN_TEXT,.bounds={-5,3,150,20},.clip={0,0,240,135},.opacity=(uint8_t)opacities[oi],
            .data.text={.utf8="ABCDEFGHIJKL",.bytes=12,.capacity=16,.font=KSN_BODY,.color=color}};
        ksn_draw b=a;b.bounds=(ksn_rect){30,9,239,40};b.clip=(ksn_rect){33,10,201,37};
        b.data.text.color=0x20e0f000u|(255u-alphas[ai]);
        ksn_draw c=a;c.bounds=(ksn_rect){100,60,240,135};c.opacity=255;
        c.data.text.color=0xffffff00u|alphas[ai];
        ksn_draw box={.kind=KSN_RECT,.bounds={20,50,180,90},.clip={0,0,240,135},.opacity=170,
            .data.shape={.color=0x3070a0ffu}};
        assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
        assert(app.ops->background(app.ctx,tx,0x183c60ff)==KSN_OK);
        assert(app.ops->add(app.ctx,tx,&box,&ref)==KSN_OK);
        assert(app.ops->add(app.ctx,tx,&a,&ref)==KSN_OK);
        assert(app.ops->add(app.ctx,tx,&b,&ref)==KSN_OK);
        assert(app.ops->add(app.ctx,tx,&c,&ref)==KSN_OK);
        assert(app.ops->end(app.ctx,tx)==KSN_OK);
        g_ksn_text_runs=0;
        assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        memcpy(reference,panel,sizeof panel);
        memset(panel,0,sizeof panel);
        ksn_core_invalidate(&core);
        g_ksn_text_runs=1;runs_called=0;
        assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        if(memcmp(reference,panel,sizeof panel)){
            for(int i=0;i<240*135;i++)if(reference[i]!=panel[i]){
                fprintf(stderr,"MISMATCH rows=%u lut=%u coarse=%u alpha=%u opacity=%u at %d,%d: %04x vs %04x\n",
                        strip_rows,lut,coarse,alphas[ai],opacities[oi],i%240,i/240,reference[i],panel[i]);
                return 1;
            }
        }
        /* The runs arm was taken, unless every command's alpha is zero. */
        assert(runs_called||!alphas[ai]||!opacities[oi]||lut);
        scenes++;
    }
    g_ksn_blend_lut_alpha=0;g_ksn_scale256=0;g_ksn_text_runs=1;
    printf("text runs PASS: %u scenes, runs and coverage arms paint the same panel\n",scenes);
    return 0;
}
