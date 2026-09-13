#ifndef DS_STRESS_SCENE_H
#define DS_STRESS_SCENE_H
#include <stdint.h>
/* Deterministic diagnostic workload; t is elapsed animation milliseconds. */
static inline unsigned stress_triangle(unsigned t,unsigned span){
    unsigned v=t%(2*span);return v<span?v:2*span-v;
}
static inline uint16_t stress_rgb(unsigned r,unsigned g,unsigned b){return (uint16_t)((r>>3)<<11|(g>>2)<<5|(b>>3));}
static inline uint16_t stress_over(uint16_t p,unsigned r,unsigned g,unsigned b,unsigned a){
    unsigned dr=p>>11,dg=(p>>5)&63,db=p&31;
    dr=(dr<<3)|(dr>>2);dg=(dg<<2)|(dg>>4);db=(db<<3)|(db>>2);
    return stress_rgb((r*a+dr*(255-a)+127)/255,(g*a+dg*(255-a)+127)/255,(b*a+db*(255-a)+127)/255);
}
static inline uint16_t stress_source(unsigned x,unsigned y,unsigned t){
    unsigned dx=t/13,dy=t/19;
    uint16_t p=((x+dx)/18+(y+dy)/18)%2?stress_rgb(30,160,220):stress_rgb(235,128,48);
    unsigned a=stress_triangle(t/7,255),left=stress_triangle(t/9,170),top=stress_triangle(t/17,85);
    if(x>=left&&x<left+70&&y>=top&&y<top+50)p=stress_over(p,230,40,120,a);
    left=170-left;top=85-top;
    if(x>=left&&x<left+70&&y>=top&&y<top+50)p=stress_over(p,30,240,130,255-a);
    return p;
}
/* Frame-invariant geometry and eight possible composite colors. The source
 * scene is unchanged; only repeated time arithmetic and blending are hoisted. */
typedef struct { unsigned dx,dy,left,top;uint16_t color[8]; } stress_frame;
static inline stress_frame stress_frame_prepare(unsigned t){
    stress_frame f={.dx=t/13,.dy=t/19,.left=stress_triangle(t/9,170),.top=stress_triangle(t/17,85)};
    unsigned a=stress_triangle(t/7,255);
    for(unsigned mask=0;mask<4;mask++)for(unsigned checker=0;checker<2;checker++){
        uint16_t p=checker?stress_rgb(30,160,220):stress_rgb(235,128,48);
        if(mask&1)p=stress_over(p,230,40,120,a);
        if(mask&2)p=stress_over(p,30,240,130,255-a);
        f.color[mask*2+checker]=p;
    }
    return f;
}
static inline void stress_source_row(const stress_frame *f,unsigned y,uint16_t *out){
    unsigned checker=(f->dx/18+(y+f->dy)/18)%2,remaining=18-f->dx%18;
    unsigned other_left=170-f->left,other_top=85-f->top;
    unsigned first=y>=f->top&&y<f->top+50,second=y>=other_top&&y<other_top+50;
    for(unsigned x=0;x<240;x++){
        unsigned mask=(first&&x>=f->left&&x<f->left+70?1u:0u)|
                      (second&&x>=other_left&&x<other_left+70?2u:0u);
        out[x]=f->color[mask*2+checker];
        if(!--remaining){remaining=18;checker^=1;}
    }
}
static inline void stress_panel(unsigned t,unsigned *x,unsigned *y,unsigned *alpha){
    *x=40+stress_triangle(t/23,70);*y=20+stress_triangle(t/31,20);
    *alpha=48+stress_triangle(t/11,144);
}
#endif
