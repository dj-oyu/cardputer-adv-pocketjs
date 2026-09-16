#include "core_fixture.h"
#include "ksn_render.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"primitives line %d: %s\n",__LINE__,#x);return 1;}} while(0)

static uint16_t panel[240*135],strip[240*8];
static uint16_t *get_strip(void *ctx){(void)ctx;return strip;}
static ksn_result present(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));return KSN_OK;
}
static unsigned channel(uint32_t color,unsigned shift){return (color>>shift)&255u;}
static uint32_t interpolate(uint32_t from,uint32_t to,unsigned i,unsigned length){
    if(length<=1)return from;
    const unsigned shifts[4]={24,16,8,0};unsigned last=length-1,result=0;
    for(unsigned n=0;n<4;n++)
        result|=((channel(from,shifts[n])*(last-i)+channel(to,shifts[n])*i+last/2)/last)<<shifts[n];
    return result;
}
static int rounded(ksn_rect bounds,unsigned radius,int x,int y){
    if(!radius)return 1;
    int cx=x<bounds.x0+(int)radius?bounds.x0+(int)radius:
           x>=bounds.x1-(int)radius?bounds.x1-(int)radius:x;
    int cy=y<bounds.y0+(int)radius?bounds.y0+(int)radius:
           y>=bounds.y1-(int)radius?bounds.y1-(int)radius:y;
    int dx=2*x+1-2*cx,dy=2*y+1-2*cy,r=2*(int)radius;
    return dx*dx+dy*dy<=r*r;
}
static int covered(const ksn_draw *draw,int x,int y){
    if(x<draw->bounds.x0||x>=draw->bounds.x1||y<draw->bounds.y0||y>=draw->bounds.y1||
       x<draw->clip.x0||x>=draw->clip.x1||y<draw->clip.y0||y>=draw->clip.y1)return 0;
    if(draw->kind==KSN_STROKE){
        int width=draw->data.shape.width;
        return x<draw->bounds.x0+width||x>=draw->bounds.x1-width||
               y<draw->bounds.y0+width||y>=draw->bounds.y1-width;
    }
    return rounded(draw->bounds,draw->kind==KSN_GRADIENT?draw->data.gradient.radius:
                                           draw->data.shape.radius,x,y);
}
static unsigned quantize(unsigned value,unsigned maximum,unsigned threshold){
    unsigned q=value*maximum/255u,remainder=value*maximum-255u*q;
    if(q<maximum&&32u*remainder>(2u*threshold+1u)*255u)q++;
    return q;
}
static uint16_t pack(unsigned r,unsigned g,unsigned b,int dither,int x,int y){
    static const unsigned bayer[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    if(!dither)return (uint16_t)((r>>3)<<11|(g>>2)<<5|(b>>3));
    unsigned t=bayer[y&3][x&3];
    return (uint16_t)(quantize(r,31,t)<<11|quantize(g,63,t)<<5|quantize(b,31,t));
}
static uint16_t over(uint16_t dst,uint32_t source,unsigned opacity,int dither,int x,int y){
    unsigned alpha=((source&255u)*opacity+127u)/255u;
    unsigned r=(dst>>11)&31u,g=(dst>>5)&63u,b=dst&31u;
    r=(r<<3)|(r>>2);g=(g<<2)|(g>>4);b=(b<<3)|(b>>2);
    r=(channel(source,24)*alpha+r*(255-alpha)+127)/255;
    g=(channel(source,16)*alpha+g*(255-alpha)+127)/255;
    b=(channel(source,8)*alpha+b*(255-alpha)+127)/255;
    return pack(r,g,b,dither,x,y);
}
static uint32_t source(const ksn_draw *draw,int x,int y){
    if(draw->kind!=KSN_GRADIENT)return draw->data.shape.color;
    unsigned length=(unsigned)(draw->data.gradient.axis?draw->bounds.y1-draw->bounds.y0:
                                                        draw->bounds.x1-draw->bounds.x0);
    unsigned i=(unsigned)(draw->data.gradient.axis?y-draw->bounds.y0:x-draw->bounds.x0);
    return interpolate(draw->data.gradient.from,draw->data.gradient.to,i,length);
}

int main(void){
    KSN_TEST_CORE(core,);ksn_core_init(&core);ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_draw draws[]={
        {.kind=KSN_ROUND_RECT,.bounds={3,3,17,15},.clip={4,2,17,14},.opacity=201,
         .data.shape={0xf07878d7,4,0}},
        {.kind=KSN_STROKE,.bounds={25,4,43,18},.clip={0,0,240,135},.opacity=223,
         .data.shape={0x67dfc7b5,0,2}},
        {.kind=KSN_GRADIENT,.bounds={50,3,67,15},.clip={53,2,65,16},.opacity=217,
         .data.gradient={0x102030ff,0xe0c08080,0,3,false}},
        {.kind=KSN_GRADIENT,.bounds={75,2,89,19},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0x91a6baff,0xf5bb69ff,1,4,true}}
    };
    ksn_tx tx;ksn_ref ref;
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x102030ff)==KSN_OK);
    for(unsigned i=0;i<sizeof(draws)/sizeof(draws[0]);i++)CHECK(app.ops->add(app.ctx,tx,&draws[i],&ref)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_display_port display={NULL,get_strip,present,240,135,8,NULL};ksn_render_stats stats;
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK&&stats.transferred_bytes==64800);
    for(int y=0;y<135;y++)for(int x=0;x<240;x++){
        uint16_t expected=pack(0x10,0x20,0x30,0,x,y);
        for(unsigned i=0;i<sizeof(draws)/sizeof(draws[0]);i++)if(covered(&draws[i],x,y))
            expected=over(expected,source(&draws[i],x,y),draws[i].opacity,
                          draws[i].kind==KSN_GRADIENT&&draws[i].data.gradient.dither,x,y);
        CHECK(panel[y*240+x]==expected);
    }
    CHECK(panel[3*240+3]==pack(0x10,0x20,0x30,0,3,3));
    CHECK(panel[4*240+27]!=panel[7*240+29]);
    CHECK(panel[5*240+53]!=panel[5*240+64]);
    CHECK(panel[2*240+75]==pack(0x10,0x20,0x30,0,75,2));
    puts("round rect, stroke and two-color gradients: PASS");return 0;
}
