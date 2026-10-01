/* Prototype primitives of docs/kasane/crowd-primitives-design.md: the pattern
 * line (KSN_PROC_LINE_PATTERN) and the tile rectangle (KSN_PROC_TILE). Host
 * only. Checks every pixel against a formula written here, not against the
 * renderer: the pattern's cell per pixel, the tile's texel per pixel, the
 * same picture from any band split, the two-entry encoding seen by code that
 * walks a frame, and the limits and rejections. Build with KSN_PROC_STATS. */
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
    for(int rows=1;rows<=9;rows+=(rows==1?6:1)){ /* 1, 7, 8, 9 */
        render_split(f,split,rows);
        CHECK(!memcmp(full,split,sizeof full));
    }
}
#define SET(r,v) {KSN_PROC_SET,(r),0,0,(v),0}
#define MOVE(a,b) {KSN_PROC_MOVE,0,(a),(b),0,0}
#define LINE(a,b,c) {KSN_PROC_LINE,0,(a),(b),0,(c)}
#define PATTERN(base,a,b,n) {KSN_PROC_LINE_PATTERN,(base),(a),(b),(float)(n),0}
#define TILE(base,a,b,id) {KSN_PROC_TILE,(base),(a),(b),0,(id)}

/* The noise hash, written again: the test owns the definition. */
static bool noise_on(unsigned cell,unsigned seed,unsigned density,bool *second){
    uint32_t h=cell*0x9e3779b1u^seed*0x85ebca6bu;
    h^=h>>15;h*=0x85ebca77u;h^=h>>13;
    *second=h>>8&1u;
    return (h&0xffu)<density;
}

static void pattern_horizontal(void){
    /* Period 5, bits 10110b, cells 2..52 (one a pixel), from x = 10 to 60 on
     * row 20; colour A where the bit is set, B elsewhere. */
    const ksn_proc_inst code[]={
        SET(0,10),SET(1,20),SET(2,60),
        SET(4,0x16),SET(5,0xf800),SET(6,0x07e0),SET(7,2),SET(8,52),
        MOVE(0,1),PATTERN(4,2,1,5),
    };
    CHECK(run(code,10,&frame)==KSN_PROC_DONE);
    CHECK(frame.count==2&&frame.ext==1&&frame.raster_steps==51);
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
        SET(4,0x16),SET(5,0xf800),SET(6,-1),SET(7,2),SET(8,52),
        MOVE(2,1),PATTERN(4,0,1,5),
    };
    CHECK(run(back,10,&frame)==KSN_PROC_DONE);
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
        SET(4,0xa5c3e1),SET(5,0xffff),SET(6,0x001f),SET(7,1.5f),SET(8,125.25f),
        MOVE(0,1),PATTERN(4,2,1,24),
    };
    CHECK(run(wide,10,&frame)==KSN_PROC_DONE);
    render_full(&frame,full);
    for(int x=0;x<W;x++){
        unsigned u=(384u+(unsigned)(x+30)*96u)%(24u*256u);
        CHECK(full[134*W+x]==((0xa5c3e1u>>(u>>8)&1u)?0xffff:0x001f));
    }
    same_in_bands(&frame);
}
static void pattern_slanted(void){
    /* A pattern of all ones draws exactly a plain LINE's pixels, at every
     * slope and direction, through any band split. */
    static const int ends[][4]={{5,3,200,120},{200,120,5,3},{30,130,90,2},{90,2,30,130},
                                {-40,60,260,75},{120,-30,130,170},{0,0,239,134},{239,0,0,134}};
    for(unsigned e=0;e<sizeof ends/sizeof ends[0];e++){
        const ksn_proc_inst pat[]={
            SET(0,(float)ends[e][0]),SET(1,(float)ends[e][1]),SET(2,(float)ends[e][2]),SET(3,(float)ends[e][3]),
            SET(4,0xffffff),SET(5,0x1234),SET(6,-1),SET(7,0),SET(8,0),
            MOVE(0,1),PATTERN(4,2,3,24),
        };
        const int steps=abs(ends[e][2]-ends[e][0])>abs(ends[e][3]-ends[e][1])?
                        abs(ends[e][2]-ends[e][0]):abs(ends[e][3]-ends[e][1]);
        const ksn_proc_inst plain[]={
            SET(0,(float)ends[e][0]),SET(1,(float)ends[e][1]),SET(2,(float)ends[e][2]),SET(3,(float)ends[e][3]),
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
         * the pen. The walk's major axis moves one a pixel. */
        ksn_proc_inst third[11];
        memcpy(third,pat,sizeof third);
        third[4].value=1;third[8].value=(float)steps;third[10].value=3;
        CHECK(run(third,11,&frame)==KSN_PROC_DONE);
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
static void pattern_depth(void){
    /* Weights 1 and 4 (the far end four times nearer): 8 chords, the cut at
     * t = j/8 on the line and at u = 100 * 4t / ((1 - t) + 4t) in the
     * pattern, where an unweighted line would be at 100 t. */
    ksn_proc_inst code[]={
        SET(0,0),SET(1,10),SET(2,200),SET(3,50),
        SET(4,0xffffff),SET(5,0x1234),SET(6,-1),SET(7,0),SET(8,100),SET(9,1),SET(10,4),
        MOVE(0,1),PATTERN(4,2,3,24),
    };
    CHECK(run(code,13,&frame)==KSN_PROC_DONE);
    CHECK(frame.count==16&&frame.ext==1&&frame.raster_steps==200+8);
    for(unsigned j=0;j<8;j++){
        const float t=(float)j/8.0f,u=100.0f*4.0f*t/((1.0f-t)+4.0f*t);
        const ksn_proc_segment *g=&frame.segments[2*j],*q=g+1;
        CHECK(g->x0-KSN_PROC_EXT_PATTERN==25*(int)j&&g->y0==10+5*(int)j);
        CHECK(g->x1==25*((int)j+1)&&g->y1==10+5*((int)j+1));
        CHECK((uint16_t)q->x1==(uint16_t)lroundf(fmodf(u,24.0f)*256.0f));
    }
    /* The chords are the plain line's pixels (here the cuts fall on it). */
    render_full(&frame,full);
    const ksn_proc_inst plain[]={SET(0,0),SET(1,10),SET(2,200),SET(3,50),MOVE(0,1),LINE(2,3,0x1234)};
    CHECK(run(plain,6,&other)==KSN_PROC_DONE);
    render_full(&other,split);
    CHECK(!memcmp(full,split,sizeof full));
    same_in_bands(&frame);
    /* Equal weights, a zero, or unlike signs: one chord. */
    static const float flat[][2]={{3,3},{0,4},{1,0},{-1,4},{0,0}};
    for(unsigned k=0;k<5;k++){
        code[9].value=flat[k][0];code[10].value=flat[k][1];
        CHECK(run(code,13,&frame)==KSN_PROC_DONE&&frame.count==2);
    }
    /* Negative weights of one sign count as their sizes (a row's -2.4/Z). */
    code[9].value=-1;code[10].value=-1.3f;
    CHECK(run(code,13,&frame)==KSN_PROC_DONE&&frame.count==8);
}
static void pattern_noise(void){
    /* N = 0: density << 16 | seed. Each pixel is its cell's hash. */
    const unsigned seed=0x3c5a,density=140;
    const ksn_proc_inst code[]={
        SET(0,0),SET(1,50),SET(2,239),
        SET(4,(float)(density<<16|seed)),SET(5,0xf81f),SET(6,0x07ff),SET(7,250.25f),SET(8,369.75f),
        MOVE(0,1),PATTERN(4,2,1,0),
    };
    CHECK(run(code,10,&frame)==KSN_PROC_DONE);
    render_full(&frame,full);
    unsigned on=0,b=0;
    for(int x=0;x<W;x++){
        const unsigned u=(unsigned)(250.25f*256)+(unsigned)x*128u;
        bool second;
        uint16_t want=BACK;
        if(noise_on(u>>8&255u,seed,density,&second)){want=second?0x07ff:0xf81f;on++;b+=second;}
        CHECK(full[50*W+x]==want);
    }
    CHECK(on>80&&on<200&&b>20&&b<on-20); /* some of each, not all */
    same_in_bands(&frame);
    /* Density 0 draws nothing. */
    ksn_proc_inst none[10];
    memcpy(none,code,sizeof none);
    none[3].value=(float)seed;
    CHECK(run(none,10,&frame)==KSN_PROC_DONE);
    render_full(&frame,full);
    for(unsigned i=0;i<W*H;i++)CHECK(full[i]==BACK);
}

/* 5 x 3 texels, 2 frames; 0xdead is the key. */
#define K 0xdeadu
static const uint16_t TEXELS[]={
    0x0001,0x0002,K,0x0004,0x0005,
    0x0011,K,0x0013,0x0014,0x0015,
    0x0021,0x0022,0x0023,K,0x0025,
    0x0101,0x0102,0x0103,0x0104,K,
    K,0x0112,0x0113,0x0114,0x0115,
    0x0121,0x0122,K,0x0124,0x0125,
};
static const ksn_proc_tile TILES[]={{TEXELS,5,3,2,K}};
static uint16_t texel(int shot,unsigned u,unsigned v){ return TEXELS[(shot*3+(int)(v%3))*5+(int)(u%5)]; }

static void tile_rect(void){
    ksn_proc_set_tiles(TILES,1);
    /* One texel a pixel, from the pen (40,30) to (75,52): texels 3..39
     * across 36 columns, 1..24 down 23 rows. */
    const ksn_proc_inst code[]={
        SET(0,40),SET(1,30),SET(2,75),SET(3,52),
        SET(4,3),SET(5,1),SET(6,39),SET(7,24),SET(8,0),
        MOVE(0,1),TILE(4,2,3,0),
    };
    CHECK(run(code,11,&frame)==KSN_PROC_DONE);
    CHECK(frame.count==2&&frame.ext==1&&frame.raster_steps==KSN_PROC_TILE_COST(36*23));
    render_full(&frame,full);
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        uint16_t want=BACK;
        if(x>=40&&x<=75&&y>=30&&y<=52){
            uint16_t t=texel(0,3u+(unsigned)(x-40),1u+(unsigned)(y-30));
            if(t!=K)want=t;
        }
        CHECK(full[y*W+x]==want);
    }
    same_in_bands(&frame);

    /* The pen at the right bottom: its texel is the far edge's. Frame 1;
     * 2.5 pixels a texel across (121 columns for 48.4 texels: du = .4 ->
     * 102/256), 2 down (51 rows for 25.5: dv = .5); clipped left and top. */
    const ksn_proc_inst scaled[]={
        SET(0,100),SET(1,40),SET(2,-20),SET(3,-10),
        SET(4,46.4f),SET(5,33),SET(6,-2),SET(7,7.5f),SET(8,1),
        MOVE(0,1),TILE(4,2,3,0),
    };
    CHECK(run(scaled,11,&frame)==KSN_PROC_DONE);
    render_full(&frame,full);
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        uint16_t want=BACK;
        if(x<=100&&y<=40){
            /* u0 = -2 mod 5 = 3; v0 = 7.5 mod 3 = 1.5. */
            unsigned u=(3u*256u+(unsigned)(x+20)*102u)%(5u*256u);
            unsigned v=(384u+(unsigned)(y+10)*128u)%(3u*256u);
            uint16_t t=texel(1,u>>8,v>>8);
            if(t!=K)want=t;
        }
        CHECK(full[y*W+x]==want);
    }
    same_in_bands(&frame);
    /* The frame is the register's floor, wrapped to the tile's two frames:
     * 4097.25 and -0.5 are both frame 1, 2.75 is frame 0. */
    static const float shots[][2]={{4097.25f,1},{-0.5f,1},{2.75f,0},{0,0},{1,1}};
    for(unsigned k=0;k<5;k++){
        ksn_proc_inst c[11];
        memcpy(c,code,sizeof c);
        c[8].value=shots[k][0];
        CHECK(run(c,11,&frame)==KSN_PROC_DONE);
        render_full(&frame,full);
        CHECK(full[30*W+40]==texel((int)shots[k][1],3,1));
    }
}
static void order_and_walkers(void){
    ksn_proc_set_tiles(TILES,1);
    /* line, tile over it, line over the tile, pattern line over that. */
    const ksn_proc_inst code[]={
        SET(0,10),SET(1,10),SET(2,30),SET(3,20),
        SET(4,0),SET(5,0),SET(6,21),SET(7,11),SET(8,0),
        SET(9,0xffffff),SET(10,0xcccc),SET(11,-1),SET(12,0),SET(13,0),
        MOVE(0,1),LINE(2,1,0xaaaa),   /* row 10, x 10..30 */
        MOVE(0,1),TILE(4,2,3,0),      /* covers it, but for key texels */
        MOVE(0,3),LINE(2,3,0xbbbb),   /* row 20 over the tile */
        MOVE(0,1),PATTERN(9,0,3,24), /* column 10 over all */
    };
    CHECK(run(code,22,&frame)==KSN_PROC_DONE);
    CHECK(frame.count==6&&frame.ext==1);
    render_full(&frame,full);
    for(int x=11;x<=30;x++){
        uint16_t t=texel(0,(unsigned)(x-10),0);
        CHECK(full[10*W+x]==(t==K?0xaaaa:t));
        CHECK(full[20*W+x]==0xbbbb);
    }
    for(int y=10;y<=20;y++)CHECK(full[y*W+10]==0xcccc);
    same_in_bands(&frame);

    /* A walker sees four geometries with their own costs, never the
     * parameter entries. */
    unsigned i=0,cost,n=0,total=0;
    ksn_proc_segment g;
    ksn_proc_seg_kind kind;
    static const ksn_proc_seg_kind kinds[]={KSN_PROC_SEG_PLAIN,KSN_PROC_SEG_TILE,KSN_PROC_SEG_PLAIN,KSN_PROC_SEG_PATTERN};
    while(ksn_proc_frame_next(&frame,&i,&g,&kind,&cost)){
        CHECK(n<4&&kind==kinds[n]);
        CHECK(g.x0>=10&&g.x0<=30&&g.x1>=10&&g.x1<=30&&g.y0>=10&&g.y1<=20);
        total+=cost;n++;
    }
    CHECK(n==4&&i==frame.count&&total==frame.raster_steps);
    CHECK(total==21+KSN_PROC_TILE_COST(21*11)+21+11);

    /* The host surface takes the frame (its validation walks geometry) and
     * its damage is the geometry's bounds: bands 1 and 2, x 10..30. */
    static ksn_proc_surface surface;
    ksn_proc_surface_init(&surface);
    uint32_t ticket=ksn_proc_surface_stage(&surface,&frame);
    CHECK(ticket!=0);
    const ksn_proc_damage *d=ksn_proc_surface_pending_damage(&surface);
    CHECK(d&&d->bands==0x6u&&d->x0[1]==10&&d->x1[1]==31&&d->x0[2]==10&&d->x1[2]==31);
    CHECK(ksn_proc_surface_pending_frame(&surface)->ext==1);
    /* A frame cut inside an extended entry is not a frame. */
    other=frame;other.count=5;
    ksn_proc_surface_init(&surface);
    CHECK(ksn_proc_surface_stage(&surface,&other)==0);

    /* A frame without extended entries says so, and a later plain run on a
     * frame that held some clears the mark. */
    const ksn_proc_inst plain[]={SET(0,1),SET(1,2),SET(2,9),MOVE(0,1),LINE(2,1,7)};
    CHECK(run(plain,5,&frame)==KSN_PROC_DONE&&frame.ext==0&&frame.count==1);
}
static void limits_and_rejections(void){
    ksn_proc_set_tiles(TILES,1);
    /* Two entries each: 512 pattern lines fill a frame, 513 do not. */
    ksn_proc_inst many[]={
        SET(0,0),SET(1,0),SET(2,3),SET(3,1),
        SET(4,1),SET(5,1),SET(6,-1),SET(7,0),SET(8,1),
        {KSN_PROC_REPEAT,0,2,0,0,0},
          {KSN_PROC_REPEAT,0,255,0,0,0},
            MOVE(0,1),PATTERN(4,2,1,1),{KSN_PROC_ADD,1,1,3,0,0},
          {KSN_PROC_END,0,0,0,0,0},
          SET(1,0),
        {KSN_PROC_END,0,0,0,0,0},
        MOVE(0,1),PATTERN(4,2,1,1),
        MOVE(0,1),PATTERN(4,2,1,1),
        MOVE(0,1),PATTERN(4,2,1,1),
    };
    CHECK(run(many,21,&frame)==KSN_PROC_DONE&&frame.count==1024&&frame.raster_steps==512*4);
    CHECK(run(many,23,&frame)==KSN_PROC_LIMIT&&!frame.ready);
    /* A tile's cost is a quarter of its area: 182 x 180 = 32,760 pixels is
     * 8,190 and fits, 183 x 180 does not. */
    ksn_proc_inst area[]={
        SET(0,0),SET(1,0),SET(2,181),SET(3,179),
        SET(4,0),SET(5,0),SET(6,1),SET(7,1),SET(8,0),
        MOVE(0,1),TILE(4,2,3,0),
    };
    CHECK(run(area,11,&frame)==KSN_PROC_DONE&&frame.raster_steps==8190);
    area[2].value=182;
    CHECK(run(area,11,&frame)==KSN_PROC_LIMIT);
    area[2].value=20;area[3].value=89;

    /* Rejections at begin: period over 24 or not whole, parameter block
     * past the registers. */
    /* (a line of 10 steps, so u1 = 2,560 would be 256 cells a pixel) */
    ksn_proc_inst bad[]={SET(0,0),SET(4,1),SET(5,1),SET(6,-1),SET(7,0),SET(8,1),SET(3,10),MOVE(0,0),PATTERN(4,3,0,25)};
    CHECK(run(bad,9,&frame)==KSN_PROC_INVALID);
    bad[8].value=2.5f;CHECK(run(bad,9,&frame)==KSN_PROC_INVALID);
    bad[8].value=24;CHECK(run(bad,9,&frame)==KSN_PROC_DONE&&frame.count==2);
    bad[8].dst=10;CHECK(run(bad,9,&frame)==KSN_PROC_INVALID);
    bad[8].dst=4;
    ksn_proc_inst block[11];
    memcpy(block,area,sizeof block);
    block[10].dst=12;CHECK(run(block,11,&frame)==KSN_PROC_INVALID);
    /* Rejections at the step: each parameter out of range. */
    static const struct { unsigned pc; float v; } pattern_bad[]={
        {1,16777216.0f},{1,-1},{1,1.5f},{2,-1},{2,65536},{3,-2},{3,65536},{3,.5f},
        {4,2000000},{5,2560},{5,-2560}};
    for(unsigned k=0;k<sizeof pattern_bad/sizeof pattern_bad[0];k++){
        ksn_proc_inst c[9];
        memcpy(c,bad,sizeof c);
        c[pattern_bad[k].pc].value=pattern_bad[k].v;
        CHECK(run(c,9,&frame)==KSN_PROC_INVALID&&!frame.ready);
    }
    /* Backwards is legal: 255.9 cells a pixel either way. */
    bad[5].value=-2559;CHECK(run(bad,9,&frame)==KSN_PROC_DONE);
    bad[5].value=1;
    static const struct { unsigned pc; float v; } tile_bad[]={
        {8,3000000},{6,5376},{7,-23040},{4,3000000}}; /* 21 columns, 90 rows */
    for(unsigned k=0;k<sizeof tile_bad/sizeof tile_bad[0];k++){
        ksn_proc_inst c[11];
        memcpy(c,area,sizeof c);
        c[tile_bad[k].pc].value=tile_bad[k].v;
        CHECK(run(c,11,&frame)==KSN_PROC_INVALID);
    }
    area[10].color=1; /* no such tile */
    CHECK(run(area,11,&frame)==KSN_PROC_INVALID);
    area[10].color=0;
    ksn_proc_set_tiles(NULL,0);
    CHECK(run(area,11,&frame)==KSN_PROC_INVALID);
    /* A frame made with a table renders nothing for a tile no longer there. */
    ksn_proc_set_tiles(TILES,1);
    CHECK(run(area,11,&frame)==KSN_PROC_DONE);
    ksn_proc_set_tiles(NULL,0);
    render_full(&frame,full);
    for(unsigned i=0;i<W*H;i++)CHECK(full[i]==BACK);
    ksn_proc_set_tiles(TILES,1);
    /* No pen: nothing drawn, the pen set. */
    const ksn_proc_inst nopen[]={SET(0,5),SET(4,1),SET(5,1),SET(6,-1),SET(7,0),SET(8,1),PATTERN(4,0,0,1),TILE(4,0,0,0)};
    /* (the tile reads r4..r8 as u0 = 1, v0 = 1, u1 = -1, v1 = 0, frame 1 -> 1 mod 2) */
    CHECK(run(nopen,7,&frame)==KSN_PROC_DONE&&frame.count==0&&frame.ext==0);
    CHECK(run(nopen,8,&frame)==KSN_PROC_DONE&&frame.count==2); /* a 1 x 1 tile at the pen */
}
static void analysis_and_plan(void){
    ksn_proc_set_tiles(TILES,1);
    const ksn_proc_inst code[]={
        SET(0,10),SET(1,10),SET(2,30),SET(3,20),
        SET(4,0),SET(5,0),SET(6,21),SET(7,11),SET(8,0),
        SET(9,0xffffff),SET(10,0xcccc),SET(11,-1),SET(12,0),SET(13,0),
        MOVE(0,1),TILE(4,2,3,0),
        {KSN_PROC_ADD,1,1,3,0,0},{KSN_PROC_MUL,14,1,1,0,0},
        PATTERN(9,0,1,24),
    };
    const ksn_proc_program p={code,19};
    static ksn_proc_analysis a;
    CHECK(ksn_proc_analyze(&p,&a)&&a.valid);
    CHECK(a.inst[15].reads==((1u<<2)|(1u<<3)|(0x1fu<<4)));
    CHECK(a.inst[18].reads==((1u<<0)|(1u<<1)|(0x7fu<<9)));
    CHECK(a.inst[15].effects==(KSN_PA_EFFECT_PEN|KSN_PA_EFFECT_DRAW));
    CHECK(a.inst[18].failure&KSN_PA_FAIL_RASTER);
    CHECK(!a.inst[15].relocatable&&!a.inst[18].relocatable);
    /* The registered plan (fused pair at 16, 17) draws the reference's frame. */
    static ksn_proc_plan plan;
    CHECK(ksn_proc_plan_prepare(&plan,&p)&&plan.fused_count==1);
    CHECK(run(code,19,&frame)==KSN_PROC_DONE);
    CHECK(ksn_proc_plan_begin(&vm,&plan,ZERO,&other)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_plan_run(&vm,&plan,false)==KSN_PROC_DONE);
    CHECK(other.count==frame.count&&other.ext==frame.ext&&other.raster_steps==frame.raster_steps&&
          !memcmp(other.segments,frame.segments,frame.count*sizeof frame.segments[0]));
    ksn_proc_inst bad[19];
    memcpy(bad,code,sizeof bad);
    bad[18].value=30;
    const ksn_proc_program q={bad,19};
    CHECK(!ksn_proc_analyze(&q,&a));
    CHECK(!ksn_proc_plan_prepare(&plan,&q));
}
int main(void){
    pattern_horizontal();
    pattern_slanted();
    pattern_noise();
    pattern_depth();
    tile_rect();
    order_and_walkers();
    limits_and_rejections();
    analysis_and_plan();
#ifdef KSN_PROC_STATS
    printf("renderer counted: %lu scans, %lu pattern px (%lu written), %lu tile px (%lu written)\n",
           g_ksn_proc_stats.scans,g_ksn_proc_stats.pattern_px,g_ksn_proc_stats.pattern_written,
           g_ksn_proc_stats.tile_px,g_ksn_proc_stats.tile_written);
#endif
    printf(failures?"CROWD PRIM FAIL (%u)\n":"CROWD PRIM PASS\n",failures);
    return failures?1:0;
}
