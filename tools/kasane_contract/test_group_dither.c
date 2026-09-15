#include "ksn_render.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"group dither line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
static ksn_core core;
static uint16_t strip[240*8],panel[240*135],patch_pixels[240*135];
static ksn_draw draws[12];
static bool visible[12];
static ksn_ref refs[12];
static unsigned draw_count,dither_differences;
static const uint32_t background=0x315d7bff;

static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result transfer(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));return KSN_OK;
}
static const ksn_display_port display={NULL,buffer,transfer,240,135,8};

/* Scalar reference reads only the input scene, never native commands or
 * renderer helpers. Round coverage uses pixel-center distances; quantization
 * uses a single rational expression and derives Bayer from interleaved bits. */
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
static uint16_t reference(unsigned opacity,int x,int y,bool enable_dither){
    unsigned p[4]={0,0,0,0};bool marked=false;
    for(unsigned i=0;i<draw_count;i++){
        if(!visible[i]||!covered(&draws[i],x,y))continue;
        unsigned rgba[4];sample_rgba(&draws[i],x,y,rgba);
        unsigned a=product(rgba[3],draws[i].opacity);
        if(a==255)marked=false;
        if(a&&draws[i].kind==KSN_GRADIENT&&draws[i].data.gradient.dither)marked=true;
        for(unsigned c=0;c<3;c++)p[c]=limited(product(rgba[c],a)+product(p[c],255-a));
        p[3]=limited(a+product(p[3],255-a));
    }
    unsigned bg[3]={component(background,0)>>3,component(background,1)>>2,component(background,2)>>3};
    uint16_t original=(uint16_t)(bg[0]<<11|bg[1]<<5|bg[2]);
    unsigned alpha=product(p[3],opacity);
    if(!alpha)return original;
    bg[0]=bg[0]*8+bg[0]/4;bg[1]=bg[1]*4+bg[1]/16;bg[2]=bg[2]*8+bg[2]/4;
    for(unsigned c=0;c<3;c++)p[c]=limited(product(p[c],opacity)+product(bg[c],255-alpha));
    if(marked&&enable_dither){
        unsigned t=threshold((unsigned)x,(unsigned)y);
        return (uint16_t)(quantized(p[0],31,t)<<11|quantized(p[1],63,t)<<5|quantized(p[2],31,t));
    }
    return (uint16_t)((p[0]/8)<<11|(p[1]/4)<<5|(p[2]/8));
}
static int matches(unsigned opacity){
    for(int y=0;y<135;y++)for(int x=0;x<240;x++){
        uint16_t expected=reference(opacity,x,y,true);
        if(panel[y*240+x]!=expected){
            fprintf(stderr,"group dither pixel %d,%d opacity %u: got %04x expected %04x\n",
                    x,y,opacity,panel[y*240+x],expected);return 1;
        }
        if(expected!=reference(opacity,x,y,false))dither_differences++;
    }
    return 0;
}
static int replace(unsigned opacity){
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx tx;ksn_render_stats stats;
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,background)==KSN_OK);
    for(unsigned i=0;i<draw_count;i++){
        CHECK(app.ops->add(app.ctx,tx,&draws[i],&refs[i])==KSN_OK);
        if(!visible[i]){
            ksn_change c={.property=KSN_SET_VISIBLE,.value.visible=false};
            CHECK(app.ops->change(app.ctx,tx,refs[i],&c)==KSN_OK);
        }
    }
    CHECK(ksn_core_group(&core,KSN_APP,tx,refs[0],(uint16_t)draw_count,(uint8_t)opacity)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK&&stats.transferred_bytes==64800);
    return matches(opacity);
}
static int patch_and_full(unsigned opacity){
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx tx;ksn_render_stats stats;
    CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    for(unsigned i=0;i<draw_count;i++){
        ksn_change c={.property=KSN_SET_RECT,.value.rect=draws[i].bounds};
        CHECK(app.ops->change(app.ctx,tx,refs[i],&c)==KSN_OK);
        c=(ksn_change){.property=KSN_SET_CLIP,.value.rect=draws[i].clip};
        CHECK(app.ops->change(app.ctx,tx,refs[i],&c)==KSN_OK);
        c=(ksn_change){.property=KSN_SET_VISIBLE,.value.visible=visible[i]};
        CHECK(app.ops->change(app.ctx,tx,refs[i],&c)==KSN_OK);
        if(draws[i].kind==KSN_RECT){
            c=(ksn_change){.property=KSN_SET_COLOR,.value.color=draws[i].data.shape.color};
            CHECK(app.ops->change(app.ctx,tx,refs[i],&c)==KSN_OK);
        }
    }
    CHECK(ksn_core_group(&core,KSN_APP,tx,refs[0],(uint16_t)draw_count,(uint8_t)opacity)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK&&stats.transferred_bytes<64800);
    CHECK(matches(opacity)==0);memcpy(patch_pixels,panel,sizeof(panel));
    ksn_core_invalidate(&core);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK&&stats.transferred_bytes==64800);
    CHECK(memcmp(patch_pixels,panel,sizeof(panel))==0);
    return 0;
}
static void translate(int dx,int dy){
    for(unsigned i=0;i<draw_count;i++){
        draws[i].bounds.x0+=(int16_t)dx;draws[i].bounds.x1+=(int16_t)dx;
        draws[i].bounds.y0+=(int16_t)dy;draws[i].bounds.y1+=(int16_t)dy;
        draws[i].clip.x0+=(int16_t)dx;draws[i].clip.x1+=(int16_t)dx;
        draws[i].clip.y0+=(int16_t)dy;draws[i].clip.y1+=(int16_t)dy;
    }
}
int main(void){
    ksn_core_init(&core);
    const ksn_draw mixed[]={
        {.kind=KSN_RECT,.bounds={3,3,147,47},.clip={0,0,240,135},.opacity=201,.data.shape={0xd9867243,0,0}},
        {.kind=KSN_GRADIENT,.bounds={9,5,138,40},.clip={11,7,130,34},.opacity=219,
         .data.gradient={0x25384900,0xe0a972d2,0,5,true}},
        {.kind=KSN_RECT,.bounds={30,12,42,27},.clip={0,0,240,135},.opacity=255,.data.shape={0x708ca4ff,0,0}},
        {.kind=KSN_ROUND_RECT,.bounds={38,10,90,36},.clip={0,0,240,135},.opacity=177,.data.shape={0x357ecbb5,6,0}},
        {.kind=KSN_GRADIENT,.bounds={80,6,145,44},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0xa9b67aff,0x789bc4ff,1,4,false}},
        {.kind=KSN_GRADIENT,.bounds={3,3,145,45},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0x90909000,0xffffff00,1,0,true}},
        {.kind=KSN_GRADIENT,.bounds={3,3,147,47},.clip={0,0,240,135},.opacity=0,
         .data.gradient={0x123456ff,0xffff80ff,0,0,true}},
        {.kind=KSN_GRADIENT,.bounds={0,0,178,50},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0x123456ff,0xffff80ff,0,0,true}},
        {.kind=KSN_RECT,.bounds={150,5,176,20},.clip={0,0,240,135},.opacity=211,.data.shape={0x435769ff,0,0}}
    };
    draw_count=COUNT(mixed);memcpy(draws,mixed,sizeof(mixed));
    for(unsigned i=0;i<draw_count;i++)visible[i]=i!=7;
    CHECK(replace(255)==0);
    /* Exhaust group alpha, retaining exact PATCH/full equivalence throughout. */
    for(unsigned opacity=0;opacity<256;opacity++)CHECK(patch_and_full(opacity)==0);
    /* Each translated origin reaches every absolute Bayer phase, across the
     * 32-bit provenance words, 64-pixel tiles, clips and 8-row display bands. */
    const unsigned opacity_cases[]={1,2,127,128,254,255};
    for(int y=0;y<4;y++)for(int x=0;x<4;x++){
        memcpy(draws,mixed,sizeof(mixed));translate(x,y);
        for(unsigned i=0;i<COUNT(opacity_cases);i++)CHECK(patch_and_full(opacity_cases[i])==0);
    }
    memcpy(draws,mixed,sizeof(mixed));translate(-20,-12);CHECK(patch_and_full(193)==0);
    /* Hiding the sole contributing gradient removes provenance; restoring it
     * then replacing its foreground alpha exercises opaque-bit clearing. */
    memcpy(draws,mixed,sizeof(mixed));visible[1]=false;CHECK(patch_and_full(255)==0);
    visible[1]=true;draws[2].data.shape.color=0x708ca4fe;CHECK(patch_and_full(255)==0);
    draws[2].data.shape.color=0x708ca4ff;CHECK(patch_and_full(255)==0);
    CHECK(dither_differences>1000);

    /* Fully transparent and rounded-to-zero group alpha preserve the exact
     * RGB565 background. No expand/requantize is allowed, even with a mark. */
    draw_count=1;visible[0]=true;
    draws[0]=(ksn_draw){.kind=KSN_GRADIENT,.bounds={1,1,145,37},.clip={0,0,240,135},.opacity=255,
                       .data.gradient={0x7799bb00,0xbbddff00,0,8,true}};
    CHECK(replace(255)==0);
    draws[0].data.gradient.from|=1;draws[0].data.gradient.to|=1;CHECK(replace(1)==0);
    CHECK(patch_and_full(127)==0);CHECK(patch_and_full(128)==0);CHECK(patch_and_full(255)==0);
    /* Provenance deliberately survives partial attenuation even if integer
     * compositing has rounded the tiny gradient color contribution away. */
    draw_count=2;visible[1]=true;
    draws[1]=(ksn_draw){.kind=KSN_RECT,.bounds={1,1,145,37},.clip={0,0,240,135},.opacity=255,
                       .data.shape={0x637f9bfe,0,0}};
    CHECK(replace(255)==0);
    draws[1].data.shape.color=0x637f9bff;CHECK(patch_and_full(255)==0);
    printf("group dither: PASS (independent scalar, all group alpha/Bayer phases, mixed children, transparent pixels, PATCH/full; %u dither differences)\n",dither_differences);
    return 0;
}
