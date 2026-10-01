/* LINE_PATTERN (docs/kasane/crowd-primitives-design.md): the pattern line of
 * the procedural VM and band renderer. Host only. Checks every pixel against
 * a formula written here, not against the renderer: the pattern's cell per
 * pixel, the same picture from any band split (1..9 rows, bands drawn last
 * first), the two-entry encoding as code that walks a frame sees it, the
 * plain lines of a frame with pattern lines, and the limits and rejections.
 * Built by run.sh with ASan/UBSan and with -O2. */
#include "ksn_proc_analysis.h"
#include "ksn_proc_plan.h"
#include "ksn_procedural_surface.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W KSN_PROC_W
#define H KSN_PROC_H
#define BACK 0x0841u
static unsigned failures;
#define CHECK(c) do{ if(!(c)){ failures++; printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); } }while(0)

static ksn_proc_frame frame,other;
static ksn_proc_vm vm;
static uint16_t full[W*H],split[W*H];

static const float ZERO[KSN_PROC_INPUTS];
static ksn_proc_status run(const ksn_proc_inst *code,unsigned n,ksn_proc_frame *f){
    const ksn_proc_program p={code,(uint8_t)n};
    ksn_proc_status s=ksn_proc_begin(&vm,&p,ZERO,f);
    return s==KSN_PROC_RUNNING?ksn_proc_run(&vm):s;
}
static void clear(uint16_t *px){ for(unsigned i=0;i<W*H;i++)px[i]=BACK; }
static void render_full(const ksn_proc_frame *f,uint16_t *px){
    clear(px);
    CHECK(ksn_proc_render_band(f,px,0,H));
}
/* Bands of `rows` rows, last first: a band must not depend on another. */
static void render_split(const ksn_proc_frame *f,uint16_t *px,int rows){
    clear(px);
    int bands=(H+rows-1)/rows;
    for(int b=bands-1;b>=0;b--){
        int y=b*rows,h=y+rows>H?H-y:rows;
        CHECK(ksn_proc_render_band(f,px+y*W,y,h));
    }
}
static void same_in_bands(const ksn_proc_frame *f){
    render_full(f,full);
    for(int rows=1;rows<=9;rows++){
        render_split(f,split,rows);
        CHECK(!memcmp(full,split,sizeof full));
    }
}
#define SET(r,v) {KSN_PROC_SET,(r),0,0,(float)(v),0}
#define MOVE(a,b) {KSN_PROC_MOVE,0,(a),(b),0,0}
#define LINE(a,b,c) {KSN_PROC_LINE,0,(a),(b),0,(c)}
#define PATTERN(base,a,b,n,c) {KSN_PROC_LINE_PATTERN,(base),(a),(b),(float)(n),(c)}
/* Colour A is the instruction's; the arrays keep a SET where it was set
 * before (r15, unused), so their indices still name the same values. */

static void pattern_horizontal(void){
    /* Period 5, bits 10110b, cells 2..52 (one a pixel), from x = 10 to 60 on
     * row 20; colour A where the bit is set, B elsewhere. */
    const ksn_proc_inst code[]={
        SET(0,10),SET(1,20),SET(2,60),
        SET(4,0x16),SET(15,0),SET(5,0x07e0),SET(6,2),SET(7,52),
        MOVE(0,1),PATTERN(4,2,1,5,0xf800),
    };
    CHECK(run(code,10,&frame)==KSN_PROC_DONE);
    CHECK(frame.count==2&&frame.ext==1&&frame.raster_steps==51);
    /* The encoding: geometry with the bias, parameters with negative rows. */
    const ksn_proc_segment *g=&frame.segments[0],*q=g+1;
    CHECK(g->x0==10+KSN_PROC_EXT_PATTERN&&g->y0==20&&g->x1==60&&g->y1==20&&g->color==0xf800);
    CHECK((uint16_t)q->x0==0x16&&(uint16_t)q->y0==(0x8000u|5u<<8|1u<<13));
    CHECK((uint16_t)q->x1==512&&(uint16_t)q->y1==(0x8000u|256u)&&q->color==0x07e0);
    CHECK(q->y0<0&&q->y1<0);
    render_full(&frame,full);
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        uint16_t want=BACK;
        if(y==20&&x>=10&&x<=60)want=(0x16u>>((2+(x-10))%5)&1u)?0xf800:0x07e0;
        CHECK(full[y*W+x]==want);
    }
    same_in_bands(&frame);

    /* No colour B (-1): clear bits leave the backdrop. Drawn right to left:
     * pixel 0 is the pen's end, so the pattern runs from x = 60 down. */
    const ksn_proc_inst back[]={
        SET(0,10),SET(1,20),SET(2,60),
        SET(4,0x16),SET(15,0),SET(5,-1),SET(6,2),SET(7,52),
        MOVE(2,1),PATTERN(4,0,1,5,0xf800),
    };
    CHECK(run(back,10,&frame)==KSN_PROC_DONE);
    CHECK(!((uint16_t)frame.segments[1].y0>>13&1u)&&frame.segments[1].color==0);
    render_full(&frame,full);
    for(int x=0;x<W;x++){
        uint16_t want=BACK;
        if(x>=10&&x<=60&&(0x16u>>((2+(60-x))%5)&1u))want=0xf800;
        CHECK(full[20*W+x]==want);
    }
    same_in_bands(&frame);

    /* du = .375 cells a pixel (96/256), u0 = 1.5, period 24, all 24 bits,
     * clipped on both sides of the panel (x = -30..300). */
    const ksn_proc_inst wide[]={
        SET(0,-30),SET(1,134),SET(2,300),
        SET(4,0xa5c3e1),SET(15,0),SET(5,0x001f),SET(6,1.5f),SET(7,125.25f),
        MOVE(0,1),PATTERN(4,2,1,24,0xffff),
    };
    CHECK(run(wide,10,&frame)==KSN_PROC_DONE);
    render_full(&frame,full);
    for(int x=0;x<W;x++){
        unsigned u=(384u+(unsigned)(x+30)*96u)%(24u*256u);
        CHECK(full[134*W+x]==((0xa5c3e1u>>(u>>8)&1u)?0xffff:0x001f));
    }
    same_in_bands(&frame);
    /* A negative start is taken modulo the period: u0 = -1.25 is 22.75. */
    ksn_proc_inst neg[10];
    memcpy(neg,wide,sizeof neg);
    neg[6].value=-1.25f;neg[7].value=-0.25f;neg[1].value=7;neg[0].value=0;neg[2].value=9;
    CHECK(run(neg,10,&frame)==KSN_PROC_DONE);
    CHECK((uint16_t)frame.segments[1].x1==(uint16_t)(22.75*256)&&
          ((uint16_t)frame.segments[1].y1&0x7fffu)==(uint16_t)(256/9.0+.5));
    same_in_bands(&frame);
}
static void pattern_slanted(void){
    /* A pattern of all ones draws exactly a plain LINE's pixels, at every
     * slope and direction, through any band split. */
    static const int ends[][4]={{5,3,200,120},{200,120,5,3},{30,130,90,2},{90,2,30,130},
                                {-40,60,260,75},{120,-30,130,170},{0,0,239,134},{239,0,0,134},
                                {-60,40,300,52},{300,52,-60,40},{17,90,23,20},{100,7,101,128}};
    for(unsigned e=0;e<sizeof ends/sizeof ends[0];e++){
        const ksn_proc_inst pat[]={
            SET(0,ends[e][0]),SET(1,ends[e][1]),SET(2,ends[e][2]),SET(3,ends[e][3]),
            SET(4,0xffffff),SET(15,0),SET(5,-1),SET(6,0),SET(7,0),
            MOVE(0,1),PATTERN(4,2,3,24,0x1234),
        };
        const int steps=abs(ends[e][2]-ends[e][0])>abs(ends[e][3]-ends[e][1])?
                        abs(ends[e][2]-ends[e][0]):abs(ends[e][3]-ends[e][1]);
        const ksn_proc_inst plain[]={
            SET(0,ends[e][0]),SET(1,ends[e][1]),SET(2,ends[e][2]),SET(3,ends[e][3]),
            MOVE(0,1),LINE(2,3,0x1234),
        };
        CHECK(run(pat,11,&frame)==KSN_PROC_DONE);
        CHECK(run(plain,6,&other)==KSN_PROC_DONE);
        CHECK(other.ext==0&&other.count==1&&frame.raster_steps==other.raster_steps);
        render_full(&other,split);
        render_full(&frame,full);
        CHECK(!memcmp(full,split,sizeof full));
        same_in_bands(&frame);
        /* Period 3, bits 001b: every third pixel of the walk, counted from
         * the pen (the walk's major axis moves one a pixel). */
        ksn_proc_inst third[11];
        memcpy(third,pat,sizeof third);
        third[4].value=1;third[8].value=(float)steps;third[10].value=3;
        CHECK(run(third,11,&frame)==KSN_PROC_DONE);
        render_full(&other,split);
        render_full(&frame,full);
        const bool xmajor=abs(ends[e][2]-ends[e][0])>=abs(ends[e][3]-ends[e][1]);
        unsigned drawn=0,expected=0;
        for(int y=0;y<H;y++)for(int x=0;x<W;x++){
            const bool on=split[y*W+x]==0x1234;
            const int k=xmajor?abs(x-ends[e][0]):abs(y-ends[e][1]);
            if(full[y*W+x]==0x1234){drawn++;CHECK(on&&k%3==0);}
            if(on&&k%3==0)expected++;
        }
        CHECK(drawn==expected&&drawn>0);
        same_in_bands(&frame);
    }
}
/* Random pattern lines against the formula: pixel k of the plain line's walk
 * reads cell ((u0 + k du) mod (N*256)) >> 8, u0 and du from the parameter
 * entry (whose values are checked against floats elsewhere). */
static uint32_t rng=12345u;
static uint32_t next(void){ rng=rng*1664525u+1013904223u; return rng>>8; }
static void pattern_random(void){
    for(unsigned t=0;t<400;t++){
        const int x0=(int)(next()%500)-130,y0=(int)(next()%300)-80;
        const int x1=(int)(next()%500)-130,y1=t%5==0?y0:(int)(next()%300)-80;
        const unsigned n=1+next()%24,bits=next()&0xffffffu;
        const bool has_b=next()&1u;
        const float u0=(float)((int)(next()%20000)-10000)/64.0f;
        const float u1=u0+(float)((int)(next()%4000)-2000)/16.0f;
        const ksn_proc_inst code[]={
            SET(0,x0),SET(1,y0),SET(2,x1),SET(3,y1),
            SET(4,bits),SET(15,0),SET(5,has_b?0x0bad:-1),SET(6,u0),SET(7,u1),
            MOVE(0,1),PATTERN(4,2,3,n,0x4321),
        };
        const ksn_proc_inst plain[]={SET(0,x0),SET(1,y0),SET(2,x1),SET(3,y1),MOVE(0,1),LINE(2,3,0xffff)};
        const ksn_proc_status s=run(code,11,&frame);
        const int dx=abs(x1-x0),dy=abs(y1-y0),steps=dx>dy?dx:dy;
        if(steps&&fabsf((u1-u0)/(float)steps)>=256.0f){CHECK(s==KSN_PROC_INVALID);continue;}
        CHECK(s==KSN_PROC_DONE&&frame.count==2);
        CHECK(run(plain,6,&other)==KSN_PROC_DONE);
        const ksn_proc_segment *q=&frame.segments[1];
        const uint32_t wrap=n*256u,uq=(uint16_t)q->x1,duq=(uint16_t)q->y1&0x7fffu;
        CHECK(uq<wrap&&duq<wrap);
        render_full(&other,split);
        render_full(&frame,full);
        for(int y=0;y<H;y++)for(int x=0;x<W;x++){
            uint16_t want=BACK;
            if(split[y*W+x]==0xffff){
                const uint32_t k=(uint32_t)(dx>=dy?abs(x-x0):abs(y-y0));
                const uint32_t cell=(uq+k*duq)%wrap>>8;
                want=(bits>>cell&1u)?0x4321:has_b?0x0bad:BACK;
            }
            if(full[y*W+x]!=want){CHECK(full[y*W+x]==want);t=400;y=H;break;}
        }
        same_in_bands(&frame);
    }
}
static void pattern_depth(void){
    /* Weights 1 and 4 (the far end four times nearer): 8 chords, the cut at
     * t = j/8 on the line and at u = 100 * 4t / ((1 - t) + 4t) in the
     * pattern, where an unweighted line would be at 100 t. */
    ksn_proc_inst code[]={
        SET(0,0),SET(1,10),SET(2,200),SET(3,50),
        SET(4,0xffffff),SET(15,0),SET(5,-1),SET(6,0),SET(7,100),SET(8,1),SET(9,4),
        MOVE(0,1),PATTERN(4,2,3,24,0x1234),
    };
    CHECK(run(code,13,&frame)==KSN_PROC_DONE);
    CHECK(frame.count==16&&frame.ext==1&&frame.raster_steps==200+8);
    for(unsigned j=0;j<8;j++){
        const float t=(float)j/8.0f,u=100.0f*4.0f*t/((1.0f-t)+4.0f*t);
        const ksn_proc_segment *g=&frame.segments[2*j],*q=g+1;
        CHECK(g->x0-KSN_PROC_EXT_PATTERN==25*(int)j&&g->y0==10+5*(int)j);
        CHECK(g->x1==25*((int)j+1)&&g->y1==10+5*((int)j+1));
        CHECK(abs((int)(uint16_t)q->x1-(int)lroundf(fmodf(u,24.0f)*256.0f))<=1);
    }
    /* The chords are the plain line's pixels (here the cuts fall on it). */
    render_full(&frame,full);
    const ksn_proc_inst plain[]={SET(0,0),SET(1,10),SET(2,200),SET(3,50),MOVE(0,1),LINE(2,3,0x1234)};
    CHECK(run(plain,6,&other)==KSN_PROC_DONE);
    render_full(&other,split);
    CHECK(!memcmp(full,split,sizeof full));
    same_in_bands(&frame);
    /* Equal weights, a zero, or unlike signs: one chord. Ratios at the
     * thresholds: 1.02 -> 2, 1.2 -> 4, 1.6 -> 8. */
    static const float flat[][3]={{3,3,1},{0,4,1},{1,0,1},{-1,4,1},{0,0,1},{1,1.01f,1},{1,1.02f,2},
                                  {1,1.19f,2},{1,1.2f,4},{1,1.59f,4},{1,1.6f,8},{5,1,8}};
    for(unsigned k=0;k<sizeof flat/sizeof flat[0];k++){
        code[9].value=flat[k][0];code[10].value=flat[k][1];
        CHECK(run(code,13,&frame)==KSN_PROC_DONE&&frame.count==2*(unsigned)flat[k][2]);
    }
    /* Negative weights of one sign count as their sizes (a row's -2.4/Z). */
    code[9].value=-1;code[10].value=-1.3f;
    CHECK(run(code,13,&frame)==KSN_PROC_DONE&&frame.count==8);
    same_in_bands(&frame);
}
static void walkers_and_plain(void){
    /* A plain line, a pattern line over it, a plain line over that: order. */
    const ksn_proc_inst code[]={
        SET(0,10),SET(1,10),SET(2,30),SET(3,20),
        SET(9,0xffffff),SET(15,0),SET(10,-1),SET(11,0),SET(12,0),SET(13,0),SET(14,0),
        MOVE(0,1),LINE(2,1,0xaaaa),    /* row 10, x 10..30 */
        MOVE(0,3),LINE(0,1,0xbbbb),    /* column 10 */
        MOVE(0,1),PATTERN(9,2,1,24,0xcccc),   /* row 10 again, over the first */
        MOVE(2,3),LINE(2,1,0xdddd),    /* column 30 over the pattern's end */
    };
    CHECK(run(code,19,&frame)==KSN_PROC_DONE);
    CHECK(frame.count==5&&frame.ext==1);
    render_full(&frame,full);
    for(int x=11;x<=29;x++)CHECK(full[10*W+x]==0xcccc);
    CHECK(full[10*W+30]==0xdddd&&full[10*W+10]==0xcccc&&full[15*W+10]==0xbbbb);
    same_in_bands(&frame);

    /* A walker sees four geometries with their own costs, never the
     * parameter entry. */
    unsigned i=0,cost,n=0,total=0;
    ksn_proc_segment g;
    bool pattern;
    static const bool kinds[]={false,false,true,false};
    while(ksn_proc_frame_next(&frame,&i,&g,&pattern,&cost)){
        CHECK(n<4&&pattern==kinds[n]);
        CHECK(g.x0>=10&&g.x0<=30&&g.x1>=10&&g.x1<=30&&g.y0>=10&&g.y1<=20);
        total+=cost;n++;
    }
    CHECK(n==4&&i==frame.count&&total==frame.raster_steps&&total==21+11+21+11);

    /* The host surface takes the frame (its validation walks geometry) and
     * its damage is the geometry's bounds: bands 1 and 2, x 10..30. */
    static ksn_proc_surface surface;
    ksn_proc_surface_init(&surface);
    CHECK(ksn_proc_surface_stage(&surface,&frame)!=0);
    const ksn_proc_damage *d=ksn_proc_surface_pending_damage(&surface);
    CHECK(d&&d->bands==0x6u&&d->x0[1]==10&&d->x1[1]==31&&d->x0[2]==10&&d->x1[2]==31);
    CHECK(ksn_proc_surface_pending_frame(&surface)->ext==1);
    /* A frame cut inside a pattern line, or whose parameter entry was
     * overwritten, is not a frame. */
    other=frame;other.count=3;
    ksn_proc_surface_init(&surface);
    CHECK(ksn_proc_surface_stage(&surface,&other)==0);
    other=frame;other.segments[3].y0=5;
    ksn_proc_surface_init(&surface);
    CHECK(ksn_proc_surface_stage(&surface,&other)==0);
    other=frame;other.segments[3].y0=(int16_t)(0x8000u|25u<<8);
    ksn_proc_surface_init(&surface);
    CHECK(ksn_proc_surface_stage(&surface,&other)==0);

    /* A frame without pattern lines says so, and a later plain run on a
     * frame that held some clears the mark. */
    const ksn_proc_inst plain[]={SET(0,1),SET(1,2),SET(2,9),MOVE(0,1),LINE(2,1,7)};
    CHECK(run(plain,5,&frame)==KSN_PROC_DONE&&frame.ext==0&&frame.count==1);

    /* Plain lines in a frame with a pattern line draw the pixels they draw
     * without one: 300 random lines, then one pattern dot added. */
    static ksn_proc_frame plain_only,mixed;
    memset(&plain_only,0,sizeof plain_only);
    for(unsigned k=0;k<300;k++){
        const int16_t a=(int16_t)((int)(next()%800)-280),b=(int16_t)((int)(next()%500)-180);
        const int16_t c=(int16_t)((int)(next()%800)-280),e=(int16_t)((int)(next()%500)-180);
        plain_only.segments[plain_only.count++]=(ksn_proc_segment){a,b,c,e,(uint16_t)(k*97u)};
    }
    plain_only.ready=true;
    mixed=plain_only;
    const ksn_proc_inst dot[]={SET(0,500),SET(1,500),SET(4,1),SET(15,0),SET(5,-1),SET(6,0),SET(7,0),
                               MOVE(0,1),PATTERN(4,0,1,1,1)};
    CHECK(run(dot,9,&other)==KSN_PROC_DONE&&other.count==2);
    mixed.segments[mixed.count++]=other.segments[0];
    mixed.segments[mixed.count++]=other.segments[1];
    mixed.ext=1;
    render_full(&plain_only,split);
    render_full(&mixed,full);
    CHECK(!memcmp(full,split,sizeof full));
    same_in_bands(&mixed);
}
static void limits_and_rejections(void){
    /* Two entries each: 512 pattern lines fill a frame, 513 do not. */
    ksn_proc_inst many[]={
        SET(0,0),SET(1,0),SET(2,3),SET(3,1),
        SET(4,1),SET(15,0),SET(5,-1),SET(6,0),SET(7,1),
        {KSN_PROC_REPEAT,0,2,0,0,0},
          {KSN_PROC_REPEAT,0,255,0,0,0},
            MOVE(0,1),PATTERN(4,2,1,1,1),{KSN_PROC_ADD,1,1,3,0,0},
          {KSN_PROC_END,0,0,0,0,0},
          SET(1,0),
        {KSN_PROC_END,0,0,0,0,0},
        MOVE(0,1),PATTERN(4,2,1,1,1),
        MOVE(0,1),PATTERN(4,2,1,1,1),
        MOVE(0,1),PATTERN(4,2,1,1,1),
    };
    CHECK(run(many,21,&frame)==KSN_PROC_DONE&&frame.count==1024&&frame.raster_steps==512*4);
    CHECK(run(many,23,&frame)==KSN_PROC_LIMIT&&!frame.ready);
    /* The raster cap counts a pattern line as a line: 8 x 1,001 steps fit,
     * a ninth does not. */
    ksn_proc_inst longest[]={
        SET(0,-280),SET(1,0),SET(2,720),SET(3,-280),
        SET(4,1),SET(15,0),SET(5,-1),SET(6,0),SET(7,1),
        {KSN_PROC_REPEAT,0,8,0,0,0},MOVE(0,1),PATTERN(4,2,1,1,1),{KSN_PROC_END,0,0,0,0,0},
        MOVE(0,1),PATTERN(4,2,1,1,1),
    };
    CHECK(run(longest,13,&frame)==KSN_PROC_DONE&&frame.raster_steps==8*1001);
    CHECK(run(longest,15,&frame)==KSN_PROC_LIMIT);

    /* Rejections at begin: period 0, over 24 or not whole, the parameter
     * block past the registers. A line of 10 steps. */
    ksn_proc_inst bad[]={SET(0,0),SET(4,1),SET(15,0),SET(5,-1),SET(6,0),SET(7,1),SET(3,10),MOVE(0,0),PATTERN(4,3,0,25,1)};
    CHECK(run(bad,9,&frame)==KSN_PROC_INVALID);
    bad[8].value=0;CHECK(run(bad,9,&frame)==KSN_PROC_INVALID);
    bad[8].value=2.5f;CHECK(run(bad,9,&frame)==KSN_PROC_INVALID);
    bad[8].value=NAN;CHECK(run(bad,9,&frame)==KSN_PROC_INVALID);
    bad[8].value=24;CHECK(run(bad,9,&frame)==KSN_PROC_DONE&&frame.count==2);
    bad[8].dst=11;CHECK(run(bad,9,&frame)==KSN_PROC_INVALID);
    bad[8].dst=9;CHECK(run(bad,9,&frame)==KSN_PROC_DONE);
    bad[8].dst=4;
    bad[8].a=16;CHECK(run(bad,9,&frame)==KSN_PROC_INVALID);
    bad[8].a=3;
    /* Rejections at the step: each parameter out of range or not whole
     * (r4 pattern, r5 B, r6 u0, r7 u1: 2,560 cells over 10 steps is 256 a
     * pixel), a coordinate off the VM's range. */
    static const struct { unsigned pc; float v; } pattern_bad[]={
        {1,16777216.0f},{1,-1},{1,1.5f},{3,-2},{3,65536},{3,.5f},
        {4,2000000},{5,2560},{5,-2560},{5,1048577},{6,1000}};
    for(unsigned k=0;k<sizeof pattern_bad/sizeof pattern_bad[0];k++){
        ksn_proc_inst c[9];
        memcpy(c,bad,sizeof c);
        c[pattern_bad[k].pc].value=pattern_bad[k].v;
        CHECK(run(c,9,&frame)==KSN_PROC_INVALID&&!frame.ready);
    }
    /* Backwards is legal: 255.9 cells a pixel either way. */
    bad[5].value=-2559;CHECK(run(bad,9,&frame)==KSN_PROC_DONE);
    bad[5].value=2559;CHECK(run(bad,9,&frame)==KSN_PROC_DONE);
    bad[5].value=1;
    /* No pen: nothing drawn, the pen set (a second one draws). */
    const ksn_proc_inst nopen[]={SET(0,5),SET(4,1),SET(15,0),SET(5,-1),SET(6,0),SET(7,1),
                                 PATTERN(4,0,0,1,1),PATTERN(4,0,0,1,1)};
    CHECK(run(nopen,7,&frame)==KSN_PROC_DONE&&frame.count==0&&frame.ext==0);
    CHECK(run(nopen,8,&frame)==KSN_PROC_DONE&&frame.count==2);
    /* A depth weight that is not finite stops the draw (an input can carry
     * one only through arithmetic, which already fails; this is the block). */
    ksn_proc_inst huge[]={SET(0,0),SET(4,1),SET(15,0),SET(5,-1),SET(6,0),SET(7,1),SET(3,10),
                          SET(8,3e38f),{KSN_PROC_MUL,9,8,8,0,0},MOVE(0,0),PATTERN(4,3,0,1,1)};
    CHECK(run(huge,11,&frame)==KSN_PROC_INVALID);
}
static void analysis_and_plan(void){
    const ksn_proc_inst code[]={
        SET(0,10),SET(1,10),SET(2,30),SET(3,20),
        SET(9,0xffffff),SET(15,0),SET(10,-1),SET(11,0),SET(12,0),SET(13,1),SET(14,2),
        MOVE(0,1),LINE(2,3,7),
        {KSN_PROC_ADD,1,1,3,0,0},{KSN_PROC_MUL,4,1,1,0,0},
        PATTERN(9,0,1,24,0xcccc),
    };
    const ksn_proc_program p={code,16};
    static ksn_proc_analysis a;
    CHECK(ksn_proc_analyze(&p,&a)&&a.valid);
    CHECK(a.inst[15].reads==((1u<<0)|(1u<<1)|(0x3fu<<9)));
    CHECK(a.inst[15].effects==(KSN_PA_EFFECT_PEN|KSN_PA_EFFECT_DRAW));
    CHECK((a.inst[15].failure&KSN_PA_FAIL_RASTER)&&(a.inst[15].failure&KSN_PA_FAIL_COLOR));
    CHECK(!a.inst[15].relocatable);
    /* The registered plan (fused pair at 13, 14) draws the reference's frame. */
    static ksn_proc_plan plan;
    CHECK(ksn_proc_plan_prepare(&plan,&p)&&plan.fused_count==1);
    CHECK(run(code,16,&frame)==KSN_PROC_DONE);
    CHECK(ksn_proc_plan_begin(&vm,&plan,ZERO,&other)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_plan_run(&vm,&plan,false)==KSN_PROC_DONE);
    CHECK(other.count==frame.count&&other.ext==frame.ext&&other.raster_steps==frame.raster_steps&&
          !memcmp(other.segments,frame.segments,frame.count*sizeof frame.segments[0]));
    ksn_proc_inst bad[16];
    memcpy(bad,code,sizeof bad);
    bad[15].value=30;
    const ksn_proc_program q={bad,16};
    CHECK(!ksn_proc_analyze(&q,&a));
    CHECK(!ksn_proc_plan_prepare(&plan,&q));
    bad[15].value=24;bad[15].dst=11;
    CHECK(!ksn_proc_analyze(&q,&a));
    /* A flash plan's period may be an argument (the VALUE patch); the bound
     * program is validated after the patch. */
    static const ksn_proc_patch patch[]={{15,KSN_PROC_FIELD_VALUE,0}};
    float arg=12;
    ksn_proc_binding binding={patch,&arg,1,1};
    CHECK(ksn_proc_begin_bound(&vm,&p,&binding,ZERO,&other)==KSN_PROC_RUNNING&&ksn_proc_run(&vm)==KSN_PROC_DONE);
    CHECK(((uint16_t)other.segments[2].y0>>8&31u)==12);
    arg=25;
    CHECK(ksn_proc_begin_bound(&vm,&p,&binding,ZERO,&other)==KSN_PROC_INVALID);
}
int main(void){
    pattern_horizontal();
    pattern_slanted();
    pattern_random();
    pattern_depth();
    walkers_and_plain();
    limits_and_rejections();
    analysis_and_plan();
    printf(failures?"PATTERN LINE FAIL (%u)\n":"PATTERN LINE PASS\n",failures);
    return failures?1:0;
}
