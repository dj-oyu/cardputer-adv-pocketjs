/* Shared surfaces exercise the real bounded band renderer and ownership path. */
#include "ksn_procedural_surface.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static ksn_proc_frame frame;
static uint16_t full[KSN_PROC_W*KSN_PROC_H],strips[KSN_PROC_W*KSN_PROC_H];
static ksn_proc_surface surface;
static ksn_proc_blend_ellipsoids sample(void){
    ksn_proc_blend_ellipsoids p={.header={.light={0,0,1,0,0,1,1,1,1},
        .material={0,1,0,16,0,1,1,1,1,1,1},.controls={8,.08f},.surface={0xffff,255,255},.count=2}};
    for(unsigned i=0;i<2;i++)p.lobes[i]=(ksn_proc_blend_lobe){
        .screen={12,0,0,33+14*i,0,12,0,30},.normal={1.0f/12,0,0,0,1.0f/12,0,0,0,.1f},.depth={0,0,10,0}};
    return p;
}
static void put(const ksn_proc_blend_ellipsoids *p){
    int l,t,r,b;CHECK(ksn_proc_blend_bounds(p,&l,&t,&r,&b));CHECK(l<=r&&t<=b);
    const unsigned words=(unsigned)KSN_PROC_BLEND_WORDS(p->header.count);
    frame=(ksn_proc_frame){.count=(uint16_t)(words+1),.raster_steps=(uint16_t)((r-l+1)*(b-t+1)*p->header.count),.ext=1,.ready=true};
    frame.segments[0]=(ksn_proc_segment){(int16_t)(l+KSN_PROC_EXT_BLEND_ELLIPSOIDS),(int16_t)t,(int16_t)r,(int16_t)b,0};
    memcpy(frame.segments+1,p,KSN_PROC_BLEND_BYTES(p->header.count));
}
static void draw(uint16_t bg){
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)full[i]=strips[i]=bg;
    CHECK(ksn_proc_render_band(&frame,full,0,KSN_PROC_H));
    for(int y=0;y<KSN_PROC_H;y+=8){int h=KSN_PROC_H-y;if(h>8)h=8;CHECK(ksn_proc_render_band(&frame,strips+y*KSN_PROC_W,y,h));}
    CHECK(memcmp(full,strips,sizeof full)==0);
}
int main(void){
    ksn_proc_blend_ellipsoids p=sample();int l,t,r,b;
    CHECK(ksn_proc_blend_bounds(&p,&l,&t,&r,&b));CHECK(l==21&&r==59&&t==18&&b==42);
    p.header.controls[0]=0;put(&p);draw(0);const uint16_t hard=full[30*240+40];
    p.header.controls[0]=8;put(&p);draw(0);CHECK(full[30*240+40]>hard);CHECK(full[30*240+40]==0xffff);
    CHECK(full[30*240+21]==0&&full[30*240+59]==0);
    unsigned i=0,cost;ksn_proc_segment g;bool ext;
    CHECK(ksn_proc_frame_next(&frame,&i,&g,&ext,&cost));CHECK(ext&&i==frame.count&&cost==1950);
    // One opacity application, including coincident lobes.
    p.lobes[1]=p.lobes[0];p.header.surface[1]=128;put(&p);draw(0);CHECK(full[30*240+33]==0x8410);
    p.header.surface[1]=255;p.header.material[0]=1;p.header.material[1]=0;p.header.material[8]=.25f;p.header.material[9]=.5f;
    put(&p);draw(0);CHECK(full[30*240+33]==0x441f);
    // Depth chooses the visible cap independently of input painter order.
    p=sample();p.header.controls[0]=0;p.header.light[0]=1;p.header.light[2]=0;
    p.lobes[1].depth[3]=100;put(&p);draw(0);const uint16_t front=full[30*240+40];
    ksn_proc_blend_lobe swap=p.lobes[0];p.lobes[0]=p.lobes[1];p.lobes[1]=swap;
    put(&p);draw(0);CHECK(full[30*240+40]==front);
    p.lobes[1].depth[3]=200;put(&p);draw(0);CHECK(full[30*240+40]>front);
    // Input and candidate are not retained by a completed staged snapshot.
    p=sample();put(&p);ksn_proc_surface_init(&surface);
    uint32_t generation=ksn_proc_surface_stage(&surface,&frame);CHECK(generation);
    memset(frame.segments,0,sizeof frame.segments);
    CHECK(ksn_proc_surface_finish(&surface,generation,true));
    CHECK(ksn_proc_surface_committed_frame(&surface)->segments[0].x0==KSN_PROC_EXT_BLEND_ELLIPSOIDS+21);
    put(&p);generation=ksn_proc_surface_stage(&surface,&frame);CHECK(generation);
    CHECK(ksn_proc_surface_finish(&surface,generation,false));CHECK(ksn_proc_surface_needs_repair(&surface));
    CHECK(!ksn_proc_surface_stage(&surface,&frame));ksn_proc_surface_repair_done(&surface);
    CHECK(ksn_proc_surface_stage(&surface,&frame));
    // Truncation and count corruption must not walk into later draw entries.
    put(&p);frame.count--;i=0;CHECK(!ksn_proc_frame_next(&frame,&i,&g,&ext,&cost));CHECK(i>frame.count);
    put(&p);((uint8_t *)(frame.segments+1))[94]=9;i=0;CHECK(!ksn_proc_frame_next(&frame,&i,&g,&ext,&cost));
    p=sample();p.header.count=0;CHECK(!ksn_proc_blend_bounds(&p,&l,&t,&r,&b));
    p=sample();p.lobes[1].depth[2]=0;CHECK(!ksn_proc_blend_bounds(&p,&l,&t,&r,&b));
    p=sample();p.lobes[1].normal[8]=0;CHECK(!ksn_proc_blend_bounds(&p,&l,&t,&r,&b));
    p=sample();p.lobes[1].screen[0]=NAN;CHECK(!ksn_proc_blend_bounds(&p,&l,&t,&r,&b));
    p=sample();p.header.material[10]=1.01f;CHECK(!ksn_proc_blend_bounds(&p,&l,&t,&r,&b));
    p=sample();p.header.controls[0]=-1;CHECK(!ksn_proc_blend_bounds(&p,&l,&t,&r,&b));
    p=sample();p.header.controls[1]=1.01f;CHECK(!ksn_proc_blend_bounds(&p,&l,&t,&r,&b));
    p=sample();p.header.surface[1]=0;CHECK(ksn_proc_blend_bounds(&p,&l,&t,&r,&b)&&l>r);
    // Reflection preserves front lighting; later plain geometry still wins.
    p=sample();p.header.count=1;p.lobes[0].screen[0]=-12;p.lobes[0].normal[0]=-1.0f/12;
    put(&p);draw(0);CHECK(full[30*240+33]==0xffff);
    frame.segments[frame.count++]=(ksn_proc_segment){33,30,33,30,0x07e0};frame.raster_steps++;
    draw(0);CHECK(full[30*240+33]==0x07e0);
    puts("PASS blended ellipsoid seam/ambient/depth/alpha/snapshot/repair/limits/band oracle");return 0;
}
