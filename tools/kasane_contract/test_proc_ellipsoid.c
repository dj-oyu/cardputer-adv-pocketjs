/* Analytic patches use the real band renderer, never a JS approximation. */
#include "ksn_procedural_surface.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static ksn_proc_frame frame;
static uint16_t full[KSN_PROC_W*KSN_PROC_H],strips[KSN_PROC_W*KSN_PROC_H];
static ksn_proc_surface surface;
static ksn_proc_ellipsoid sample(void){
    ksn_proc_ellipsoid p={.screen={20,0,0,40,0,20,0,30},
        .normal={1,0,0,0,1,0,0,0,1},.light={0,0,1,0,0,1,1,1,1},
        .material={1,0,0,16,0,1,0.5f,0.25f},
        .texture={1,0,0,0,1,0,0,0,1},.surface={0xffff,0xf800,255,255,0}};
    return p;
}
static void put(const ksn_proc_ellipsoid *p){
    int l,t,r,b;CHECK(ksn_proc_ellipsoid_bounds(p,&l,&t,&r,&b));CHECK(l<=r&&t<=b);
    frame=(ksn_proc_frame){.count=KSN_PROC_ELLIPSOID_ENTRIES,.raster_steps=(uint16_t)((r-l+1)*(b-t+1)),.ext=1,.ready=true};
    frame.segments[0]=(ksn_proc_segment){(int16_t)(l+KSN_PROC_EXT_ELLIPSOID),(int16_t)t,(int16_t)r,(int16_t)b,0};
    memcpy(frame.segments+1,p,sizeof *p);
}
static void draw(uint16_t bg){
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)full[i]=strips[i]=bg;
    CHECK(ksn_proc_render_band(&frame,full,0,KSN_PROC_H));
    for(int y=0;y<KSN_PROC_H;y+=8){int h=KSN_PROC_H-y;if(h>8)h=8;CHECK(ksn_proc_render_band(&frame,strips+y*KSN_PROC_W,y,h));}
    CHECK(memcmp(full,strips,sizeof full)==0);
}
int main(void){
    ksn_proc_ellipsoid p=sample();int l,t,r,b;
    CHECK(ksn_proc_ellipsoid_bounds(&p,&l,&t,&r,&b));CHECK(l==20&&t==10&&r==60&&b==50);
    put(&p);draw(0x001f);
    CHECK(full[30*240+40]==0xffff);CHECK(full[30*240+60]==0x001f);
    CHECK(full[30*240+59]!=0xffff&&full[30*240+59]!=0x001f);
    unsigned i=0,cost;ksn_proc_segment g;bool ext;
    CHECK(ksn_proc_frame_next(&frame,&i,&g,&ext,&cost));CHECK(ext&&i==KSN_PROC_ELLIPSOID_ENTRIES&&cost==1681);
    ksn_proc_surface_init(&surface);CHECK(ksn_proc_surface_stage(&surface,&frame)!=0);
    frame.count=1;frame.raster_steps=0;ksn_proc_surface_init(&surface);CHECK(!ksn_proc_surface_stage(&surface,&frame));
    p.screen[4]=20;p.screen[5]=1e-8f;CHECK(!ksn_proc_ellipsoid_bounds(&p,&l,&t,&r,&b));
    p=sample();p.normal[8]=0;CHECK(!ksn_proc_ellipsoid_bounds(&p,&l,&t,&r,&b));
    p=sample();p.material[0]=NAN;CHECK(!ksn_proc_ellipsoid_bounds(&p,&l,&t,&r,&b));
    p=sample();p.surface[4]=4;CHECK(!ksn_proc_ellipsoid_bounds(&p,&l,&t,&r,&b));
    p=sample();p.screen[3]=-1000;CHECK(ksn_proc_ellipsoid_bounds(&p,&l,&t,&r,&b)&&l>r);
    p=sample();p.surface[2]=0;CHECK(ksn_proc_ellipsoid_bounds(&p,&l,&t,&r,&b)&&l>r);
    p=sample();p.surface[2]=128;put(&p);draw(0);CHECK(full[30*240+40]==0x8410);
    p=sample();p.material[0]=0;p.material[1]=1;put(&p);draw(0);
    uint16_t lit=full[30*240+40];CHECK(lit==0xffff);
    p.light[0]=1;p.light[2]=0;put(&p);draw(0);CHECK(full[30*240+40]==0);
    p=sample();p.material[0]=0;p.material[2]=1;p.material[3]=4;put(&p);draw(0);CHECK(full[30*240+40]==0xffff);
    p.light[3]=1;p.light[5]=0;put(&p);draw(0);CHECK(full[30*240+40]!=0xffff&&full[30*240+40]!=0);
    p=sample();p.material[0]=0;p.material[4]=1;p.light[2]=-1;put(&p);draw(0);
    CHECK(full[30*240+40]==0);CHECK(full[30*240+53]!=0);
    p=sample();p.surface[4]=2;p.texture[7]=0.6f;p.texture[8]=0.9f;put(&p);draw(0);
    CHECK(full[30*240+40]==0xffff);CHECK(full[40*240+40]==0xf800);
    p.texture[5]=-0.5f;put(&p);draw(0);CHECK(full[40*240+40]==0xffff);
    // Reflection must keep the front surface facing the camera.
    p=sample();p.screen[0]=-20;p.normal[0]=-1;p.material[0]=0;p.material[1]=1;
    put(&p);draw(0);CHECK(full[30*240+40]==0xffff);
    // Subsequent plain draws retain ordering in frames containing patches.
    frame.segments[frame.count++]=(ksn_proc_segment){40,30,40,30,0x07e0};frame.raster_steps++;
    draw(0);CHECK(full[30*240+40]==0x07e0);
    // An off-band pattern's bits can resemble an ellipsoid packet marker.
    // Consume its parameter entry before walking a later visible primitive.
    frame.segments[frame.count++]=(ksn_proc_segment){KSN_PROC_EXT_PATTERN+10,2,15,2,0xffff};
    frame.segments[frame.count++]=(ksn_proc_segment){32767,(int16_t)0x8100,0,(int16_t)0x8000,0};
    frame.segments[frame.count++]=(ksn_proc_segment){40,80,40,80,0x07e0};
    frame.raster_steps+=7;
    draw(0);CHECK(full[80*240+40]==0x07e0);
    puts("PASS analytic ellipsoid continuous light/alpha/UV/limits/band oracle");return 0;
}
