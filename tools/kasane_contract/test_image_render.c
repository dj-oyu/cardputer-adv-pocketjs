#include "core_fixture.h"
#include "ksn_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t panel[240*135],strip[240*8],saved[240*135];
static unsigned calls,sends;static int failure_y=-1;
static uint16_t *buffer(void *p){(void)p;return strip;}
static ksn_result send(void *p,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)p;sends++;memcpy(panel+y*240,pixels,rows*240*2);return KSN_OK;
}
static uint16_t color(unsigned x,unsigned y,unsigned frame){return (uint16_t)(x*313+y*937+frame*3001);}
static uint8_t coverage(unsigned x,unsigned y){const uint8_t a[]={0,1,127,128,254,255};return a[(x+y*3)%6];}
static ksn_result source(void *p,uint16_t variant,uint16_t frame,uint16_t y,uint16_t x,uint16_t n,uint16_t *rgb,uint8_t *alpha){
    (void)p;assert(variant==0&&frame<2&&x+n<=70&&y<40&&n<=32);calls++;
    if(y==failure_y)return KSN_IO;
    for(unsigned j=0;j<n;j++){rgb[j]=color(x+j,y,frame);alpha[j]=coverage(x+j,y);}return KSN_OK;
}
static unsigned mul(unsigned a,unsigned b){return (a*b+127)/255;}
static uint16_t expected(const ksn_draw *d,int x,int y,bool group){
    if(x<d->bounds.x0||x>=d->bounds.x1||y<d->bounds.y0||y>=d->bounds.y1||
       x<d->clip.x0||x>=d->clip.x1||y<d->clip.y0||y>=d->clip.y1)return 0x19ec;
    // Independent pixel-center nearest-neighbor mapping, not span indexing.
    unsigned dx=(unsigned)(x-d->bounds.x0),dy=(unsigned)(y-d->bounds.y0);
    unsigned denominator=d->data.image.scale==KSN_IMAGE_2X?4:2;
    unsigned numerator=d->data.image.scale==KSN_IMAGE_HALF?2:1;
    unsigned sx=3+(2*dx+1)*numerator/denominator,sy=2+(2*dy+1)*numerator/denominator;
    uint16_t rgb=color(sx,sy,d->data.image.frame);
    unsigned src[]={((rgb>>11)&31)*255/31,((rgb>>5)&63)*255/63,(rgb&31)*255/31};
    // RGB565 expansion uses bit replication, not normalized multiply/divide.
    src[0]=((rgb>>11)<<3)|(rgb>>13);src[1]=(((rgb>>5)&63)<<2)|((rgb>>9)&3);src[2]=((rgb&31)<<3)|((rgb&31)>>2);
    const unsigned bg[]={24,60,99};unsigned out[3],a=mul(coverage(sx,sy),d->opacity);
    if(!a||(group&&!mul(a,137)))return 0x19ec;
    for(unsigned i=0;i<3;i++){
        out[i]=group?mul(mul(src[i],a),137)+mul(bg[i],255-mul(a,137)):
                       (src[i]*a+bg[i]*(255-a)+127)/255;
        if(out[i]>255)out[i]=255;
    }
    return (uint16_t)((out[0]>>3)<<11|(out[1]>>2)<<5|(out[2]>>3));
}
static void verify(const ksn_draw *d,bool grouped){
    for(int y=0;y<135;y++)for(int x=0;x<240;x++){
        uint16_t want=expected(d,x,y,grouped);
        if(panel[y*240+x]!=want){fprintf(stderr,"image mismatch scale=%d group=%d alpha=%u at %d,%d: %x != %x\n",
            d->data.image.scale,grouped,d->opacity,x,y,panel[y*240+x],want);assert(false);}
    }
}
int main(void){
    KSN_TEST_CORE(core,);ksn_core_init(&core);ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_image_port port={NULL,70,40,1,2,source};ksn_resource image;
    assert(ksn_core_register_image(&core,KSN_APP,&port,&image)==KSN_OK);
    ksn_display_port display={NULL,buffer,send,240,135,8,NULL};ksn_render_stats stats;ksn_tx tx;ksn_ref ref;
    ksn_draw d={.kind=KSN_IMAGE,.clip={2,7,110,65},.data.image={.source_x=3,.source_y=2}};
    d.data.image.resource=image;
    for(unsigned scale=0;scale<3;scale++)for(unsigned group=0;group<2;group++)for(unsigned opacity=0;opacity<256;opacity++){
        d.data.image.scale=(ksn_image_scale)scale;d.opacity=(uint8_t)opacity;
        int width=scale==1?128:scale==2?32:64,height=width/2;
        d.bounds=(ksn_rect){-3,5,(int16_t)(-3+width),(int16_t)(5+height)};
        assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
        assert(app.ops->background(app.ctx,tx,0x183c60ff)==KSN_OK);
        assert(app.ops->add(app.ctx,tx,&d,&ref)==KSN_OK);
        if(group)assert(ksn_core_group(&core,KSN_APP,tx,ref,1,137)==KSN_OK);
        assert(app.ops->end(app.ctx,tx)==KSN_OK);
        assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);verify(&d,group!=0);
    }
    // Frame changes retain crop/scale. Provider failure after one LCD band must
    // keep the pending generation immutable and repair to the same pixels.
    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    ksn_change change={.property=KSN_SET_IMAGE_FRAME,.value.image={0,1}};
    assert(app.ops->change(app.ctx,tx,ref,&change)==KSN_OK);assert(app.ops->end(app.ctx,tx)==KSN_OK);
    d.data.image.frame=1;failure_y=9;sends=0;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_IO&&sends==1);
    failure_y=-1;assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);verify(&d,true);
    memcpy(saved,panel,sizeof(panel));ksn_core_invalidate(&core);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK&&!memcmp(saved,panel,sizeof(panel)));
    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    change=(ksn_change){.property=KSN_SET_RECT,.value.rect={-32768,0,32767,10}};
    assert(app.ops->change(app.ctx,tx,ref,&change)==KSN_INVALID);app.ops->abort(app.ctx,tx);
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    d.data.image.scale=KSN_IMAGE_2X;d.bounds.x1=28;
    assert(app.ops->add(app.ctx,tx,&d,&ref)==KSN_INVALID);app.ops->abort(app.ctx,tx);
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    d.bounds.x1=29;d.data.image.source_x=65535;
    assert(app.ops->add(app.ctx,tx,&d,&ref)==KSN_INVALID);app.ops->abort(app.ctx,tx);
    puts("image render PASS: independent 1x/2x/half crop, alpha/group, negative clip, frames, provider retry, overflow");
}
