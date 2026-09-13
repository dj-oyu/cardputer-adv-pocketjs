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
static inline void stress_panel(unsigned t,unsigned *x,unsigned *y,unsigned *alpha){
    *x=40+stress_triangle(t/23,70);*y=20+stress_triangle(t/31,20);
    *alpha=48+stress_triangle(t/11,144);
}
#endif
