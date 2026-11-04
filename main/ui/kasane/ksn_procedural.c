#include "ksn_procedural.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int g_ksn_proc_hspan=1;
/* Columns a..b (inclusive, already on the panel) of one row in colour c, two
 * pixels a 32-bit store once the address is 4-aligned. may_alias: the rest of
 * the renderer reads and writes these pixels as uint16_t
 * (docs/vm/aliasing-types.md). */
static inline __attribute__((always_inline)) void fill_row(uint16_t *row,int a,int b,uint16_t c){
    typedef uint32_t pixel_pair __attribute__((may_alias));
    uint16_t *p=row+a;
    unsigned n=(unsigned)(b-a+1);
    if(((uintptr_t)p&2u)){*p++=c;n--;}
    pixel_pair *w=(pixel_pair *)(void *)p;
    const uint32_t v=(uint32_t)c|(uint32_t)c<<16;
    for(unsigned i=n/2;i;i--)*w++=v;
    if(n&1u)p[n-1]=c;
}
/* A horizontal line in the band (the caller's range test put its row there):
 * the Bresenham walk of dy = 0 writes exactly the row's columns between the
 * ends, so clip them to the panel and fill. */
static inline __attribute__((always_inline)) void hspan(const ksn_proc_segment *s,int x0,
                                                        uint16_t *pixels,int y){
    int a=x0<s->x1?x0:s->x1,b=x0<s->x1?s->x1:x0;
    if(a<0)a=0;
    if(b>=KSN_PROC_W)b=KSN_PROC_W-1;
    if(a<=b)fill_row(pixels+(s->y0-y)*KSN_PROC_W,a,b,s->color);
}
/* The Bresenham phase where line s (pen end x0) reaches band y..y+height-1
 * mid-way: x, yy, err moved there; returns the major-axis steps taken (the
 * pixel index), 0 when the line starts in the band. */
static inline __attribute__((always_inline)) int band_entry(const ksn_proc_segment *s,int x0,int y,int height,
                                                            int *x,int *yy,int *err){
    const int dx=abs(s->x1-x0),dy=abs(s->y1-s->y0),sx=x0<s->x1?1:-1,sy=s->y0<s->y1?1:-1;
    const int row=sy>0?y-s->y0:s->y0-(y+height-1);
    if(!dy||row<=0)return 0;
    int major,minor,x_steps,y_steps;
    /* int16 endpoints and a panel band bound row by 32768 + KSN_PROC_H.
     * The positive division numerators fit uint32_t, including rounding. */
    if(dx>=dy){
        uint32_t numerator=(uint32_t)row*dx-(dx-1)/2;
        major=numerator>0?(int)((numerator+dy-1)/(unsigned)dy):0;
        minor=(int)(((uint32_t)major*dy+(dx-1)/2)/(unsigned)dx);
        x_steps=major;y_steps=minor;
    }else{
        major=row;
        minor=(int)(((uint32_t)major*dx+(dy-1)/2)/(unsigned)dy);
        x_steps=minor;y_steps=major;
    }
    *x+=sx*x_steps;*yy+=sy*y_steps;
    *err=(int)((int64_t)dx-dy-(int64_t)x_steps*dy+(int64_t)y_steps*dx);
    return major;
}
/* A plain line in a frame with pattern lines: ksn_proc_render_band's walk. */
static inline __attribute__((always_inline)) void plain_band(const ksn_proc_segment *s,uint16_t *pixels,int y,int height){
    if(!height)return;
    if(g_ksn_proc_hspan&&s->y0==s->y1){hspan(s,s->x0,pixels,y);return;}
    int x=s->x0,yy=s->y0,dx=abs(s->x1-s->x0),dy=abs(s->y1-s->y0);
    int sx=x<s->x1?1:-1,sy=yy<s->y1?1:-1,err=dx-dy;
    band_entry(s,s->x0,y,height,&x,&yy,&err);
    for(;;){
        if(x>=0&&x<KSN_PROC_W)pixels[(yy-y)*KSN_PROC_W+x]=s->color;
        if(x==s->x1&&yy==s->y1)break;
        int twice=2*err;
        if(twice>-dy){err-=dy;x+=sx;}
        if(twice<dx){err+=dx;yy+=sy;
            if((sy>0&&yy>=y+height)||(sy<0&&yy<y))break;
        }
    }
}
/* One pixel of a pattern line: cell u >> 8 of the pattern. */
#define PATTERN_PIXEL(at) do{ if(bits>>(u>>8)&1u)(at)=ca; else if(has_b)(at)=cb; }while(0)
/* A pattern line's pixels inside the band: g its geometry, q its parameters.
 * The walk is the plain line's, with the pixel index k carried as u. A
 * horizontal line (a stand's row from the side) clips and runs left to
 * right; a slanted one keeps its row's address and tests only the major
 * axis's end (that axis moves every step). */
static void pattern_band(const ksn_proc_segment *g,const ksn_proc_segment *q,
                         uint16_t *pixels,int y,int height){
    const unsigned n=(uint16_t)q->y0>>8&31u;
    if(n<1||n>KSN_PROC_PATTERN_BITS)return;
    const uint32_t wrap=n*256u,u0=(uint16_t)q->x1%wrap,du=((uint16_t)q->y1&0x7fffu)%wrap;
    const uint32_t bits=(uint16_t)q->x0|(uint32_t)((uint16_t)q->y0&0xffu)<<16;
    const bool has_b=(uint16_t)q->y0>>13&1u;
    const uint16_t ca=g->color,cb=q->color;
    const int x0=g->x0-KSN_PROC_EXT_PATTERN;
    if(g->y0==g->y1){
        const bool forward=x0<=g->x1;
        int left=forward?x0:g->x1,right=forward?g->x1:x0;
        if(left<0)left=0;
        if(right>=KSN_PROC_W)right=KSN_PROC_W-1;
        if(left>right)return;
        /* k counts from the pen, whichever side that is; left to right is
         * backwards (wrap - du) when the pen is on the right. */
        const uint32_t k=(uint32_t)(forward?left-x0:x0-left);
        const uint32_t step=forward?du:(wrap-du)%wrap;
        /* 32 bits hold it: u0 and du are under 2^13, k at most 1,200. */
        uint32_t u=(u0+k*du)%wrap;
        uint16_t *row=pixels+(g->y0-y)*KSN_PROC_W;
        for(int x=left;x<=right;x++){
            PATTERN_PIXEL(row[x]);
            u+=step;if(u>=wrap)u-=wrap;
        }
        return;
    }
    const int dx=abs(g->x1-x0),dy=abs(g->y1-g->y0),sx=x0<g->x1?1:-1,sy=g->y0<g->y1?1:-1;
    int x=x0,yy=g->y0,err=dx-dy;
    const uint32_t k=(uint32_t)band_entry(g,x0,y,height,&x,&yy,&err);
    /* The range test let the line in, so its entry point is in the band. */
    uint32_t u=(u0+k*du)%wrap;
    uint16_t *row=pixels+(yy-y)*KSN_PROC_W;
    const int stride=sy*KSN_PROC_W;
    if(dx>=dy){
        for(;;){
            if((unsigned)x<(unsigned)KSN_PROC_W)PATTERN_PIXEL(row[x]);
            if(x==g->x1)break;
            const int twice=2*err;
            if(twice>-dy){err-=dy;x+=sx;}
            if(twice<dx){err+=dx;yy+=sy;if(yy<y||yy>=y+height)break;row+=stride;}
            u+=du;if(u>=wrap)u-=wrap;
        }
    }else{
        for(;;){
            if((unsigned)x<(unsigned)KSN_PROC_W)PATTERN_PIXEL(row[x]);
            if(yy==g->y1)break;
            const int twice=2*err;
            if(twice>-dy){err-=dy;x+=sx;}
            if(twice<dx){err+=dx;yy+=sy;}
            if(yy<y||yy>=y+height)break;
            row+=stride;
            u+=du;if(u>=wrap)u-=wrap;
        }
    }
}
#undef PATTERN_PIXEL
/* A frame with pattern lines: the same order, the same plain lines. A
 * parameter entry's negative rows fail the range test; the bias is looked at
 * only for an entry that reached the band. Out of line, so a frame without
 * pattern lines runs the loop below as it always did. */
static __attribute__((noinline)) void render_band_ext(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height,
                                                      unsigned first,unsigned end){
    for(unsigned j=first;j<end;j++){
        const ksn_proc_segment *s=&frame->segments[j];
        int ymin=s->y0<s->y1?s->y0:s->y1;
        int ymax=s->y0>s->y1?s->y0:s->y1;
        if(ymax<y||ymin>=y+height)continue;
        if(s->x0>=KSN_PROC_EXT_MIN){
            if(j+1<frame->count)pattern_band(s,s+1,pixels,y,height);
            j++;
        }else plain_band(s,pixels,y,height);
    }
}
bool ksn_proc_render_band(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height){
    return ksn_proc_render_band_range(frame,pixels,y,height,0,frame?frame->count:0);
}
bool ksn_proc_render_band_range(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height,
                                unsigned first,unsigned end){
    if(!frame||!frame->ready||!pixels||y<0||height<0||y>KSN_PROC_H||height>KSN_PROC_H-y||
       first>end||end>frame->count)return false;
    if(frame->ext){render_band_ext(frame,pixels,y,height,first,end);return true;}
    for(unsigned j=first;j<end;j++){
        const ksn_proc_segment *s=&frame->segments[j];
        int ymin=s->y0<s->y1?s->y0:s->y1;
        int ymax=s->y0>s->y1?s->y0:s->y1;
        if(ymax<y||ymin>=y+height)continue;
        if(g_ksn_proc_hspan&&s->y0==s->y1){hspan(s,s->x0,pixels,y);continue;}
        int x=s->x0,yy=s->y0,dx=abs(s->x1-s->x0),dy=abs(s->y1-s->y0);
        int sx=x<s->x1?1:-1,sy=yy<s->y1?1:-1,err=dx-dy;
        /* A band can be reached without replaying the line's prefix: the
         * Bresenham phase is rebuilt from its major-axis step count (clipping
         * the endpoints and restarting would change ties). band_entry does it
         * in 32 bits; the 64-bit form this replaced was a __divdi3 call per
         * line and piece, twice as many with 4-row pieces (lcd-dma-driver.md
         * 9.9). */
        band_entry(s,s->x0,y,height,&x,&yy,&err);
        for(;;){
            if((sy>0&&yy>=y+height)||(sy<0&&yy<y))break;
            if(x>=0&&x<KSN_PROC_W&&yy>=y&&yy<y+height)pixels[(yy-y)*KSN_PROC_W+x]=s->color;
            if(x==s->x1&&yy==s->y1)break;
            int twice=2*err;
            if(twice>-dy){err-=dy;x+=sx;}
            if(twice<dx){err+=dx;yy+=sy;}
        }
    }
    return true;
}
