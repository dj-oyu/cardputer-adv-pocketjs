/* Actual analytic shader intersected with an independent halfplane mask. */
#include "ksn_procedural_surface.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static ksn_proc_frame frame,old_frame;
static uint16_t full[240*135],bands[240*135],reference[240*135],expected[240*135];
static ksn_proc_surface surface;
static const uint16_t background=0x2137;
static ksn_proc_ellipsoid sample(void){
    return (ksn_proc_ellipsoid){.screen={35,7,0,95,0,24,5,61},.normal={1,0,0,0,1,0,0,0,1},
        .light={-.5f,-.4f,.8f,0,0,1,1,.9f,.8f},.material={.3f,.7f,.2f,16,.2f,1,.5f,.3f},
        .texture={1,0,0,0,1,0,.08f,0,1},.surface={0xffff,0xf800,179,128,0}};
}
static bool inside(const ksn_proc_ellipsoid_clip *p,int x,int y){
    if(!p->count)return false;
    int sign=0;
    for(unsigned i=0;i<p->count;i++){
        unsigned j=(i+1)%p->count;
        double a=(double)p->xy[2*j]-p->xy[2*i],b=(double)p->xy[2*j+1]-p->xy[2*i+1];
        double cross=a*(y-(double)p->xy[2*i+1])-b*(x-(double)p->xy[2*i]);
        int side=(cross>0)-(cross<0);if(side&&sign&&side!=sign)return false;if(side)sign=side;
    }
    return true;
}
static void put_old(const ksn_proc_ellipsoid *p){
    int l,t,r,b;CHECK(ksn_proc_ellipsoid_bounds(p,&l,&t,&r,&b));
    old_frame=(ksn_proc_frame){.count=KSN_PROC_ELLIPSOID_ENTRIES,.raster_steps=(uint16_t)((r-l+1)*(b-t+1)),.ext=1,.ready=true};
    old_frame.segments[0]=(ksn_proc_segment){(int16_t)(l+KSN_PROC_EXT_ELLIPSOID),(int16_t)t,(int16_t)r,(int16_t)b,0};
    memcpy(old_frame.segments+1,p,sizeof *p);
}
static void put_clip(const ksn_proc_clipped_ellipsoid *p){
    int l,t,r,b;CHECK(ksn_proc_clipped_ellipsoid_bounds(p,&l,&t,&r,&b));
    frame=(ksn_proc_frame){.ready=true};if(l>r||t>b)return;
    frame.count=KSN_PROC_CLIPPED_ENTRIES;frame.raster_steps=(uint16_t)((r-l+1)*(b-t+1));frame.ext=1;
    frame.segments[0]=(ksn_proc_segment){(int16_t)(l+KSN_PROC_EXT_CLIPPED_ELLIPSOID),(int16_t)t,(int16_t)r,(int16_t)b,0};
    memcpy(frame.segments+1,p,sizeof *p);
}
static void check_pixels(const ksn_proc_clipped_ellipsoid *p){
    put_old(&p->patch);put_clip(p);
    for(unsigned i=0;i<240*135;i++)reference[i]=full[i]=bands[i]=background;
    CHECK(ksn_proc_render_band(&old_frame,reference,0,135));CHECK(ksn_proc_render_band(&frame,full,0,135));
    for(int y=0;y<135;y+=8){int h=135-y;if(h>8)h=8;CHECK(ksn_proc_render_band(&frame,bands+y*240,y,h));}
    CHECK(!memcmp(full,bands,sizeof full));
    for(int y=0;y<135;y++)for(int x=0;x<240;x++)CHECK(full[y*240+x]==(inside(&p->clip,x,y)?reference[y*240+x]:background));
}
int main(void){
    ksn_proc_clipped_ellipsoid p={.patch=sample(),.clip={.count=4,.xy={60,35,130,35,130,90,60,90}}};
    check_pixels(&p);memcpy(expected,full,sizeof full);
    unsigned i=0,cost;ksn_proc_segment geometry;bool ext;
    CHECK(ksn_proc_frame_next(&frame,&i,&geometry,&ext,&cost)&&i==KSN_PROC_CLIPPED_ENTRIES&&ext&&cost==frame.raster_steps);
    ksn_proc_clipped_ellipsoid decoded,sentinel;memset(&sentinel,0xa5,sizeof sentinel);
    for(unsigned words=0;words<KSN_PROC_CLIPPED_WORDS;words++){decoded=sentinel;CHECK(!ksn_proc_clipped_ellipsoid_read(frame.segments+1,words,&decoded));CHECK(!memcmp(&decoded,&sentinel,sizeof decoded));}
    CHECK(ksn_proc_clipped_ellipsoid_read(frame.segments+1,KSN_PROC_CLIPPED_WORDS,&decoded)&&!memcmp(&decoded,&p,sizeof p));
    ((uint8_t *)(frame.segments+1))[sizeof p]=1;decoded=sentinel;
    CHECK(!ksn_proc_clipped_ellipsoid_read(frame.segments+1,KSN_PROC_CLIPPED_WORDS,&decoded)&&!memcmp(&decoded,&sentinel,sizeof decoded));
    ((uint8_t *)(frame.segments+1))[sizeof p]=0;
    ksn_proc_frame saved=frame;frame.count--;i=0;CHECK(!ksn_proc_frame_next(&frame,&i,&geometry,&ext,&cost));frame=saved;
    frame.segments[0].x0++;i=0;CHECK(!ksn_proc_frame_next(&frame,&i,&geometry,&ext,&cost));frame=saved;
    ksn_proc_surface_init(&surface);uint32_t generation=ksn_proc_surface_stage(&surface,&frame);CHECK(generation);
    memset(frame.segments+1,0,sizeof p);CHECK(!ksn_proc_surface_stage(&surface,&frame));
    for(unsigned k=0;k<240*135;k++)full[k]=background;
    CHECK(ksn_proc_render_band(ksn_proc_surface_pending_frame(&surface),full,0,135)&&!memcmp(full,expected,sizeof full));
    CHECK(ksn_proc_surface_finish(&surface,generation,true));
    put_clip(&p);generation=ksn_proc_surface_stage(&surface,&frame);CHECK(generation);CHECK(ksn_proc_surface_finish(&surface,generation,false));
    CHECK(ksn_proc_surface_needs_repair(&surface));CHECK(!ksn_proc_surface_stage(&surface,&frame));ksn_proc_surface_repair_done(&surface);
    int l,t,r,b;
    p.clip.count=0;memset(p.clip.xy,0,sizeof p.clip.xy);CHECK(ksn_proc_clipped_ellipsoid_bounds(&p,&l,&t,&r,&b)&&l>r);check_pixels(&p);
    generation=ksn_proc_surface_stage(&surface,&frame);CHECK(generation&&ksn_proc_surface_pending_damage(&surface)->bands);CHECK(ksn_proc_surface_finish(&surface,generation,true));
    p.patch.normal[8]=0;CHECK(!ksn_proc_clipped_ellipsoid_bounds(&p,&l,&t,&r,&b));p.patch=sample();
    p.clip=(ksn_proc_ellipsoid_clip){.count=4,.xy={0,0,2,2,0,2,2,0}};CHECK(!ksn_proc_clipped_ellipsoid_bounds(&p,&l,&t,&r,&b));
    p.clip=(ksn_proc_ellipsoid_clip){.count=3,.xy={0,0,1,1,2,2}};CHECK(!ksn_proc_clipped_ellipsoid_bounds(&p,&l,&t,&r,&b));
    p.clip=(ksn_proc_ellipsoid_clip){.count=3,.xy={0,0,20,0,0,20}};p.clip.xy[6]=1;CHECK(!ksn_proc_clipped_ellipsoid_bounds(&p,&l,&t,&r,&b));
    p.clip=(ksn_proc_ellipsoid_clip){.count=3,.xy={0,0,20,0,0,20}};p.clip.xy[0]=NAN;CHECK(!ksn_proc_clipped_ellipsoid_bounds(&p,&l,&t,&r,&b));
    p.clip.xy[0]=4097;CHECK(!ksn_proc_clipped_ellipsoid_bounds(&p,&l,&t,&r,&b));
    p.clip=(ksn_proc_ellipsoid_clip){.count=2};CHECK(!ksn_proc_clipped_ellipsoid_bounds(&p,&l,&t,&r,&b));
    p.clip.count=9;CHECK(!ksn_proc_clipped_ellipsoid_bounds(&p,&l,&t,&r,&b));
    p.clip=(ksn_proc_ellipsoid_clip){.count=3,.reserved={1,0,0},.xy={0,0,20,0,0,20}};CHECK(!ksn_proc_clipped_ellipsoid_bounds(&p,&l,&t,&r,&b));
    p.clip=(ksn_proc_ellipsoid_clip){.count=5};
    for(unsigned v=0;v<5;v++){double angle=6.283185307179586*((v*2)%5)/5;p.clip.xy[2*v]=(float)cos(angle);p.clip.xy[2*v+1]=(float)sin(angle);}
    CHECK(!ksn_proc_clipped_ellipsoid_bounds(&p,&l,&t,&r,&b));
    for(unsigned k=0;k<120;k++){
        p.patch=sample();if(k&2)p.patch.surface[4]=3;
        if(k&4){p.patch.surface[4]=4;const float region[9]={0,1,0,.1f,.1f,0,.08f,0,1};memcpy(p.patch.texture,region,sizeof region);}
        p.clip=(ksn_proc_ellipsoid_clip){.count=(uint8_t)(3+k%6)};
        float cx=(float)(k*43%300)-30,cy=(float)(k*29%190)-25,rx=3+(float)(k*17%90),ry=2+(float)(k*13%70);
        for(unsigned v=0;v<p.clip.count;v++){
            double angle=6.283185307179586*(k&1?-(double)v:(double)v)/p.clip.count+k*.137;
            p.clip.xy[2*v]=cx+rx*(float)cos(angle);p.clip.xy[2*v+1]=cy+ry*(float)sin(angle);
        }
        check_pixels(&p);
    }
    puts("PASS clipped analytic ellipsoid: independent inclusion mask, 120 moving polygons, alpha/UV, EMPTY, full/bands, packet truncation, snapshot/repair/damage");return 0;
}
