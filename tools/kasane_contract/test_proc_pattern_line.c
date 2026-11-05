/* LINE_PATTERN (docs/kasane/crowd-primitives-design.md): the pattern line of
 * the band renderer. Host only. Checks every pixel against a formula written
 * here, not against the renderer: the pattern's cell per pixel, the same
 * picture from any band split (1..9 rows, bands drawn last first), the
 * two-entry encoding as code that walks a frame sees it, the plain lines of a
 * frame with pattern lines, and the surface's rejection of a broken entry.
 * Built by run.sh with ASan/UBSan and with -O2.
 *
 * The frames are encoded here, in the two-entry form ksn_shiki_frame.h
 * documents, with u0 and du already in 8.8 cells: the float VM that used to
 * write them (and its float -> cell conversion, depth chords and step-time
 * rejections) went in Shiki v2.0, whose own LINE_PATTERN writes the same
 * entries and is checked by its own tests. */
#include "ksn_shiki_surface.h"
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
static uint16_t full[W*H],split[W*H];

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
static unsigned steps_of(int x0,int y0,int x1,int y1){
    const int dx=abs(x1-x0),dy=abs(y1-y0);
    return (unsigned)(dx>dy?dx:dy)+1u;
}
static void reset(ksn_proc_frame *f){ memset(f,0,sizeof *f); f->ready=true; }
static void line(ksn_proc_frame *f,int x0,int y0,int x1,int y1,uint16_t c){
    f->segments[f->count++]=(ksn_proc_segment){(int16_t)x0,(int16_t)y0,(int16_t)x1,(int16_t)y1,c};
    f->raster_steps=(uint16_t)(f->raster_steps+steps_of(x0,y0,x1,y1));
}
/* The two-entry pattern line: b < 0 is "no colour B", u0 and du are 8.8
 * cells under n * 256. */
static void pat(ksn_proc_frame *f,int x0,int y0,int x1,int y1,uint32_t bits,unsigned n,
                    uint16_t a,int b,unsigned u0,unsigned du){
    f->segments[f->count++]=(ksn_proc_segment){(int16_t)(x0+KSN_PROC_EXT_PATTERN),(int16_t)y0,(int16_t)x1,(int16_t)y1,a};
    f->segments[f->count++]=(ksn_proc_segment){
        (int16_t)(bits&0xffffu),
        (int16_t)(0x8000u|(bits>>16&0xffu)|n<<8|(b>=0?1u:0u)<<13),
        (int16_t)u0,(int16_t)(0x8000u|du),(uint16_t)(b>=0?b:0)};
    f->raster_steps=(uint16_t)(f->raster_steps+steps_of(x0,y0,x1,y1));
    f->ext=1;
}

static void pattern_horizontal(void){
    /* Period 5, bits 10110b, cells 2..52 (one a pixel), from x = 10 to 60 on
     * row 20; colour A where the bit is set, B elsewhere. */
    reset(&frame);
    pat(&frame,10,20,60,20,0x16,5,0xf800,0x07e0,512,256);
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

    /* No colour B: clear bits leave the backdrop. Drawn right to left:
     * pixel 0 is the pen's end, so the pattern runs from x = 60 down. */
    reset(&frame);
    pat(&frame,60,20,10,20,0x16,5,0xf800,-1,512,256);
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
    reset(&frame);
    pat(&frame,-30,134,300,134,0xa5c3e1,24,0xffff,0x001f,384,96);
    render_full(&frame,full);
    for(int x=0;x<W;x++){
        unsigned u=(384u+(unsigned)(x+30)*96u)%(24u*256u);
        CHECK(full[134*W+x]==((0xa5c3e1u>>(u>>8)&1u)?0xffff:0x001f));
    }
    same_in_bands(&frame);
}
static void pattern_slanted(void){
    /* A pattern of all ones draws exactly a plain line's pixels, at every
     * slope and direction, through any band split. */
    static const int ends[][4]={{5,3,200,120},{200,120,5,3},{30,130,90,2},{90,2,30,130},
                                {-40,60,260,75},{120,-30,130,170},{0,0,239,134},{239,0,0,134},
                                {-60,40,300,52},{300,52,-60,40},{17,90,23,20},{100,7,101,128}};
    for(unsigned e=0;e<sizeof ends/sizeof ends[0];e++){
        const int x0=ends[e][0],y0=ends[e][1],x1=ends[e][2],y1=ends[e][3];
        reset(&frame);
        pat(&frame,x0,y0,x1,y1,0xffffff,24,0x1234,-1,0,0);
        reset(&other);
        line(&other,x0,y0,x1,y1,0x1234);
        CHECK(other.ext==0&&other.count==1&&frame.raster_steps==other.raster_steps);
        render_full(&other,split);
        render_full(&frame,full);
        CHECK(!memcmp(full,split,sizeof full));
        same_in_bands(&frame);
        /* Period 3, bits 001b, one cell a pixel: every third pixel of the
         * walk, counted from the pen (the major axis moves one a pixel). */
        reset(&frame);
        pat(&frame,x0,y0,x1,y1,1,3,0x1234,-1,0,256);
        render_full(&other,split);
        render_full(&frame,full);
        const bool xmajor=abs(x1-x0)>=abs(y1-y0);
        unsigned drawn=0,expected=0;
        for(int y=0;y<H;y++)for(int x=0;x<W;x++){
            const bool on=split[y*W+x]==0x1234;
            const int k=xmajor?abs(x-x0):abs(y-y0);
            if(full[y*W+x]==0x1234){drawn++;CHECK(on&&k%3==0);}
            if(on&&k%3==0)expected++;
        }
        CHECK(drawn==expected&&drawn>0);
        same_in_bands(&frame);
    }
}
/* Random pattern lines against the formula: pixel k of the plain line's walk
 * reads cell ((u0 + k du) mod (N*256)) >> 8. */
static uint32_t rng=12345u;
static uint32_t next(void){ rng=rng*1664525u+1013904223u; return rng>>8; }
static void pattern_random(void){
    for(unsigned t=0;t<400;t++){
        const int x0=(int)(next()%500)-130,y0=(int)(next()%300)-80;
        const int x1=(int)(next()%500)-130,y1=t%5==0?y0:(int)(next()%300)-80;
        const unsigned n=1+next()%24,bits=next()&0xffffffu;
        const bool has_b=next()&1u;
        const uint32_t wrap=n*256u,uq=next()%wrap,duq=next()%wrap;
        const int dx=abs(x1-x0),dy=abs(y1-y0);
        reset(&frame);
        pat(&frame,x0,y0,x1,y1,bits,n,0x4321,has_b?0x0bad:-1,uq,duq);
        reset(&other);
        line(&other,x0,y0,x1,y1,0xffff);
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
static void walkers_and_plain(void){
    /* A plain line, a pattern line over it, a plain line over that: order. */
    reset(&frame);
    line(&frame,10,10,30,10,0xaaaa);    /* row 10, x 10..30 */
    line(&frame,10,20,10,10,0xbbbb);    /* column 10 */
    pat(&frame,10,10,30,10,0xffffff,24,0xcccc,-1,0,0); /* row 10 again, over the first */
    line(&frame,30,20,30,10,0xdddd);    /* column 30 over the pattern's end */
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

    /* A parameter entry with a period of 0 or over 24, or a start cell past
     * its period, is not a pattern line either. */
    other=frame;other.segments[3].y0=(int16_t)(0x8000u|0u<<8);
    ksn_proc_surface_init(&surface);
    CHECK(ksn_proc_surface_stage(&surface,&other)==0);
    other=frame;other.segments[3].x1=24*256;
    ksn_proc_surface_init(&surface);
    CHECK(ksn_proc_surface_stage(&surface,&other)==0);

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
    reset(&other);
    pat(&other,500,500,500,500,1,1,1,-1,0,0);
    mixed.segments[mixed.count++]=other.segments[0];
    mixed.segments[mixed.count++]=other.segments[1];
    mixed.ext=1;
    render_full(&plain_only,split);
    render_full(&mixed,full);
    CHECK(!memcmp(full,split,sizeof full));
    same_in_bands(&mixed);
}
int main(void){
    pattern_horizontal();
    pattern_slanted();
    pattern_random();
    walkers_and_plain();
    printf(failures?"PATTERN LINE FAIL (%u)\n":"PATTERN LINE PASS\n",failures);
    return failures?1:0;
}
