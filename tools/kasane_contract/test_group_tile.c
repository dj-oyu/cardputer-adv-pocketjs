/* Tile-level work on the group's compositing tile: docs/perf/kasane-opt-survey.md
 * boundary 3, candidates 3a (blocks the group's bbox cannot reach), 3b (a 16-pixel
 * tile instead of 64) and the smooth-layer shortcut the survey leaves for last
 * ("evaluate the layer once per block, write the block from a constant plus a
 * per-pixel increment"). The switches and the numbers are in
 * docs/perf/kasane-tile.md.
 *
 * Three proofs, all host-only:
 *   1. the two exact arms (reach off/on, tile 64/16) are compared against each
 *      other over the parameter space AND against the independent scalar
 *      reference test_group_dither.c already uses, so "the arms agree" cannot
 *      mean "both are wrong";
 *   2. the approximate arm (smooth) is measured, not asserted: how many pixels
 *      move, the worst 5/6/5 level step, the worst expanded 8-bit channel step,
 *      and which pixels -- classified by whether a radius-0 gradient covers them
 *      and along which axis, because the claim is "exact along y, approximate
 *      along x";
 *   3. the chord's arithmetic is swept exhaustively (every 8-bit endpoint pair x
 *      every block shape a tile can produce) and has to stay inside the two
 *      endpoints' closed interval -- that is what lets the block write without a
 *      clamp, and what bounds the deviation to one 8-bit level.
 *
 * Built with -DKSN_TILE_COUNT it also reports the tile's waste rate (tile pixels
 * visited vs tile pixels a child wrote into) and how often each arm was taken;
 * a shortcut that is never entered passes a two-arm comparison by comparing the
 * old path with itself. */
#include "core_fixture.h"
#include "ksn_render.h"
#include <stdio.h>
#include <string.h>
#ifdef KSN_TILE_COUNT
#include <inttypes.h>
extern uint32_t ksn_tile_visited,ksn_tile_covered,ksn_tile_blocks,ksn_tile_skipped,
                ksn_tile_smooth_blocks,ksn_tile_smooth_pixels,ksn_tile_child_pixels;
#endif

#define CHECK(x) do{if(!(x)){fprintf(stderr,"group tile line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
KSN_TEST_CORE(core,static);
static uint16_t strip[240*8],panel[240*135],arm_a[240*135],arm_b[240*135];
static ksn_draw draws[16];
static bool visible[16];
static unsigned draw_count,dither_differences;
static uint32_t background=0x315d7bff;
static const char title_text[]="Kasane";

static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result transfer(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));return KSN_OK;
}
static unsigned coverage(int x,int y){
    static const unsigned values[]={0,1,127,128,254,255};
    if(x<2||x>=77||y<7||y>=22)return 0;
    return values[(x+y)%6];
}
static ksn_result span(void *port,const ksn_draw *d,uint16_t reveal,int x,int y,unsigned count,uint8_t *out){
    (void)port;(void)reveal;(void)d;
    for(unsigned i=0;i<count;i++)out[i]=(uint8_t)coverage(x+(int)i,y);
    return KSN_OK;
}
static const ksn_text_port text_port={.span=span};
static const ksn_display_port display={NULL,buffer,transfer,240,135,8,&text_port,NULL};

/* ---- independent scalar reference (same shape as test_group_dither.c) ----- */
/* Reads the scene, never the renderer's helpers, and reproduces the documented
 * chain: premultiplied `over` into a tile whose alpha starts at 0, then the
 * group's opacity against the expanded 565 background. */
static bool covered(const ksn_draw *d,int x,int y){
    if(x<d->clip.x0||x>=d->clip.x1||y<d->clip.y0||y>=d->clip.y1||
       x<d->bounds.x0||x>=d->bounds.x1||y<d->bounds.y0||y>=d->bounds.y1)return false;
    if(d->kind==KSN_STROKE){
        int w=d->data.shape.width;
        return x-d->bounds.x0<w||d->bounds.x1-x<=w||y-d->bounds.y0<w||d->bounds.y1-y<=w;
    }
    unsigned radius=d->kind==KSN_GRADIENT?d->data.gradient.radius:d->data.shape.radius;
    double dx=radius-(x-d->bounds.x0+0.5),far_x=radius-(d->bounds.x1-x-0.5);
    double dy=radius-(y-d->bounds.y0+0.5),far_y=radius-(d->bounds.y1-y-0.5);
    if(dx<far_x)dx=far_x;
    if(dy<far_y)dy=far_y;
    if(dx<0)dx=0;
    if(dy<0)dy=0;
    return dx*dx+dy*dy<=radius*radius;
}
static unsigned product(unsigned a,unsigned b){return (a*b+127)/255;}
static unsigned limited(unsigned value){return value<256?value:255;}
static unsigned component(uint32_t color,unsigned c){return (color>>(24-c*8))&255;}
static void sample_rgba(const ksn_draw *d,int x,int y,unsigned out[4]){
    if(d->kind!=KSN_GRADIENT){
        for(unsigned c=0;c<4;c++)out[c]=component(d->data.shape.color,c);
        return;
    }
    int n=d->data.gradient.axis?d->bounds.y1-d->bounds.y0:d->bounds.x1-d->bounds.x0;
    int at=d->data.gradient.axis?y-d->bounds.y0:x-d->bounds.x0;
    for(unsigned c=0;c<4;c++){
        unsigned first=component(d->data.gradient.from,c),last=component(d->data.gradient.to,c);
        out[c]=n<=1?first:((unsigned)(n-1-at)*first+(unsigned)at*last+(unsigned)(n-1)/2)/(unsigned)(n-1);
    }
}
static unsigned threshold(unsigned x,unsigned y){
    return 4*(2*((x^y)&1)+(y&1))+2*(((x>>1)^(y>>1))&1)+((y>>1)&1);
}
static unsigned quantized(unsigned channel_value,unsigned maximum,unsigned t){
    return (32*channel_value*maximum+(31-2*t)*255-1)/(32*255);
}
static uint16_t reference(unsigned opacity,int x,int y,bool enable_dither){
    unsigned p[4]={0,0,0,0};bool marked=false;
    for(unsigned i=0;i<draw_count;i++){
        if(!visible[i]||!covered(&draws[i],x,y))continue;
        unsigned rgba[4];sample_rgba(&draws[i],x,y,rgba);
        unsigned a=product(rgba[3],draws[i].opacity);
        if(a==255)marked=false;
        if(a&&draws[i].kind==KSN_GRADIENT&&draws[i].data.gradient.dither)marked=true;
        for(unsigned c=0;c<3;c++)p[c]=limited(product(rgba[c],a)+product(p[c],255-a));
        p[3]=limited(a+product(p[3],255-a));
    }
    unsigned bg[3]={component(background,0)>>3,component(background,1)>>2,component(background,2)>>3};
    uint16_t original=(uint16_t)(bg[0]<<11|bg[1]<<5|bg[2]);
    unsigned alpha=product(p[3],opacity);
    if(!alpha)return original;
    bg[0]=bg[0]*8+bg[0]/4;bg[1]=bg[1]*4+bg[1]/16;bg[2]=bg[2]*8+bg[2]/4;
    for(unsigned c=0;c<3;c++)p[c]=limited(product(p[c],opacity)+product(bg[c],255-alpha));
    if(marked&&enable_dither){
        unsigned t=threshold((unsigned)x,(unsigned)y);
        return (uint16_t)(quantized(p[0],31,t)<<11|quantized(p[1],63,t)<<5|quantized(p[2],31,t));
    }
    return (uint16_t)((p[0]/8)<<11|(p[1]/4)<<5|(p[2]/8));
}
static int against_reference(const uint16_t *got,const char *what,unsigned opacity,unsigned *mismatches){
    for(int y=0;y<135;y++)for(int x=0;x<240;x++){
        uint16_t expected=reference(opacity,x,y,true);
        if(expected!=reference(opacity,x,y,false))dither_differences++;
        if(got[y*240+x]!=expected){
            if(*mismatches<4)
                fprintf(stderr,"%s: reference pixel %d,%d got %04x expected %04x\n",
                        what,x,y,got[y*240+x],expected);
            (*mismatches)++;
        }
    }
    return 0;
}

/* ---- one frame under one arm -------------------------------------------- */
/* One REPLACE frame with the whole command list as one group, the three tile
 * switches set explicitly, so both arms are the same binary. */
static int render_frame(const ksn_draw *d,const bool *vis,unsigned count,uint8_t opacity,
                        uint32_t back,int pixels,int reach,int smooth,uint16_t *out){
    g_ksn_tile_pixels=pixels;g_ksn_tile_reach=reach;g_ksn_tile_smooth=smooth;
    background=back;
    ksn_core_init(&core);
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx tx;ksn_ref refs[16];
    ksn_render_stats stats;
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,back)==KSN_OK);
    for(unsigned i=0;i<count;i++){
        CHECK(app.ops->add(app.ctx,tx,&d[i],&refs[i])==KSN_OK);
        if(!vis[i]){
            ksn_change c={.property=KSN_SET_VISIBLE,.value.visible=false};
            CHECK(app.ops->change(app.ctx,tx,refs[i],&c)==KSN_OK);
        }
    }
    CHECK(ksn_core_group(&core,KSN_APP,tx,refs[0],(uint16_t)count,opacity)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(stats.transferred_bytes==64800);
    memcpy(out,panel,sizeof(panel));
    return 0;
}

/* ---- the exact arms have to agree pixel for pixel ------------------------ */
static unsigned exact_pixels;
static int exact_arms(const ksn_draw *d,const bool *vis,unsigned count,uint8_t opacity,
                      const char *what){
    CHECK(render_frame(d,vis,count,opacity,0x315d7bff,64,0,0,arm_a)==0);
    CHECK(render_frame(d,vis,count,opacity,0x315d7bff,64,1,0,arm_b)==0);
    if(memcmp(arm_a,arm_b,sizeof(panel))!=0){
        for(int i=0;i<240*135;i++)if(arm_a[i]!=arm_b[i]){
            fprintf(stderr,"%s: reach moved %d,%d: %04x vs %04x\n",what,i%240,i/240,arm_a[i],arm_b[i]);
            break;
        }
        return 1;
    }
    /* ... the 16-pixel tile against the 64-pixel one, both with reach on ... */
    CHECK(render_frame(d,vis,count,opacity,0x315d7bff,16,1,0,arm_b)==0);
    CHECK(memcmp(arm_a,arm_b,sizeof(panel))==0);
    /* ... and the exact increment arm, which claims bit-identity. */
    CHECK(render_frame(d,vis,count,opacity,0x315d7bff,64,1,1,arm_b)==0);
    CHECK(memcmp(arm_a,arm_b,sizeof(panel))==0);
    CHECK(render_frame(d,vis,count,opacity,0x315d7bff,16,1,1,arm_b)==0);
    CHECK(memcmp(arm_a,arm_b,sizeof(panel))==0);
    exact_pixels+=5u*240u*135u;
    return 0;
}

/* ---- what the approximate arm moves ------------------------------------- */
static unsigned moved_pixels,compared_pixels;
static unsigned worst565,worst8;
static unsigned moved_x,moved_x_pixels,moved_y,moved_y_pixels,moved_none;
/* Which radius-0 gradients cover this pixel: 1 = one that varies along x (the
 * approximate direction), 2 = one that varies along y only, 0 = none. */
static unsigned smooth_class(int x,int y){
    unsigned cls=0;
    for(unsigned i=0;i<draw_count;i++){
        if(!visible[i]||!draws[i].opacity)continue;
        if(draws[i].kind!=KSN_GRADIENT||draws[i].data.gradient.radius)continue;
        if(!covered(&draws[i],x,y))continue;
        cls|=draws[i].data.gradient.axis?2u:1u;
    }
    return cls;
}
static void unpack565(uint16_t value,unsigned out[3]){
    unsigned r=value>>11,g=(value>>5)&63,b=value&31;
    out[0]=(r<<3)|(r>>2);out[1]=(g<<2)|(g>>4);out[2]=(b<<3)|(b>>2);
}
static void measure_move(const uint16_t *exact,const uint16_t *approx,const char *what){
    static unsigned reported;
    for(int y=0;y<135;y++)for(int x=0;x<240;x++){
        unsigned cls=smooth_class(x,y);
        if(cls&1)moved_x_pixels++;
        else if(cls&2)moved_y_pixels++;
        uint16_t a=exact[y*240+x],b=approx[y*240+x];
        if(a==b)continue;
        moved_pixels++;
        if(cls&1)moved_x++;
        else if(cls&2)moved_y++;
        else{
            moved_none++;
            if(reported<4){
                reported++;
                fprintf(stderr,"%s: moved at %d,%d with no radius-0 gradient covering: %04x vs %04x\n",
                        what,x,y,a,b);
            }
        }
        unsigned pa[3],pb[3];unpack565(a,pa);unpack565(b,pb);
        unsigned step=0;
        for(unsigned c=0;c<3;c++){
            unsigned diff=pa[c]>pb[c]?pa[c]-pb[c]:pb[c]-pa[c];
            if(diff>worst8)worst8=diff;
        }
        unsigned sa=a>>11,sb=b>>11,ga=(a>>5)&63,gb=(b>>5)&63,ba=a&31,bb=b&31;
        step=sa>sb?sa-sb:sb-sa;
        if((ga>gb?ga-gb:gb-ga)>step)step=ga>gb?ga-gb:gb-ga;
        if((ba>bb?ba-bb:bb-ba)>step)step=ba>bb?ba-bb:bb-ba;
        if(step>worst565)worst565=step;
    }
    compared_pixels+=240u*135u;
}
static int approximate_arm(const ksn_draw *d,const bool *vis,unsigned count,uint8_t opacity,
                           int pixels,const char *what){
    CHECK(render_frame(d,vis,count,opacity,0x315d7bff,pixels,1,0,arm_a)==0);
    CHECK(render_frame(d,vis,count,opacity,0x315d7bff,pixels,1,2,arm_b)==0);
    measure_move(arm_a,arm_b,what);
    return 0;
}

/* ---- 1: the two increments, exhaustively ---------------------------------- */
/* Both of smooth_block's ways, against the chain's own interpolate. Two claims:
 * the exact remainder stepping is bit-identical (deviation 0), and the 8.16
 * chord never leaves the interval its two ends span (so no clamp is owed) and
 * deviates by at most one 8-bit level. The doc records the one-off full sweep
 * (every endpoint pair x every length 2..240 x every run start, 1.9e9 cases)
 * that set that bound; this is the bounded version that runs with the suite. */
static unsigned arithmetic_blocks;
static int smooth_arithmetic(void){
    const unsigned lengths[]={2,3,4,5,8,16,17,64,65,240};
    const unsigned runs[]={2,4,8,16,64};
    unsigned worst_chord=0,worst_exact=0,worst5=0;
    for(unsigned from=0;from<256;from++)for(unsigned to=0;to<256;to++)
        for(unsigned l=0;l<COUNT(lengths);l++){
            unsigned length=lengths[l],last=length-1;
            const unsigned starts[4]={0,1,length/2,length-1};
            for(unsigned s=0;s<4;s++){
                unsigned i0=starts[s];
                if(i0>=length)continue;
                unsigned longest=length-i0;if(longest>64)longest=64;
                for(unsigned r=0;r<COUNT(runs);r++){
                    unsigned n=runs[r];
                    if(n>longest)continue;
                    arithmetic_blocks++;
                    /* The chain's own numerator: from*(last-i) + to*i + last/2. */
                    unsigned base=(from*(last-i0)+to*i0+last/2)/last;
                    unsigned end=(from*(last-i0-(n-1))+to*(i0+n-1)+last/2)/last;
                    int32_t span=(int32_t)end-(int32_t)base;
                    int32_t step=n>1?(int32_t)(((int64_t)span<<16)/(int32_t)(n-1)):0;
                    int32_t offset=0;
                    unsigned lo=base<end?base:end,hi=base<end?end:base;
                    /* the exact remainder stepping, over the same run */
                    int32_t full_span=(int32_t)to-(int32_t)from;
                    int32_t q=full_span/(int32_t)last,rs=full_span-q*(int32_t)last;
                    if(rs<0){q--;rs+=(int32_t)last;}
                    int32_t rem=(int32_t)from*(int32_t)last+(int32_t)i0*full_span+(int32_t)(last/2)
                                -(int32_t)base*(int32_t)last;
                    int32_t exact_off=0;
                    for(unsigned j=0;j<n;j++){
                        unsigned chord=(unsigned)((int)base+((offset+32768)>>16));
                        offset+=step;
                        if(chord<lo||chord>hi){
                            fprintf(stderr,"arithmetic: chord %u..%u length %u i0 %u run %u j %u "
                                    "left the interval [%u,%u]: %u\n",
                                    from,to,length,i0,n,j,lo,hi,chord);
                            return 1;
                        }
                        unsigned exact=(from*(last-i0-j)+to*(i0+j)+last/2)/last;
                        unsigned exact_step=(unsigned)((int)base+exact_off);
                        rem+=rs;exact_off+=q;
                        if(rem>=(int32_t)last){rem-=(int32_t)last;exact_off++;}
                        if(exact_step!=exact){
                            fprintf(stderr,"arithmetic: exact stepping %u..%u length %u i0 %u "
                                    "run %u j %u gave %u, chain %u\n",
                                    from,to,length,i0,n,j,exact_step,exact);
                            return 1;
                        }
                        unsigned dev=chord>exact?chord-exact:exact-chord;
                        if(dev>worst_chord)worst_chord=dev;
                        unsigned d5=chord>>3,ex5=exact>>3;
                        if((d5>ex5?d5-ex5:ex5-d5)>worst5)worst5=d5>ex5?d5-ex5:ex5-d5;
                    }
                }
            }
        }
    printf("arithmetic: %u blocks swept; chord worst 8-bit deviation %u (worst 5-bit level %u), "
           "exact stepping deviation %u\n",arithmetic_blocks,worst_chord,worst5,worst_exact);
    CHECK(worst_exact==0);      /* the exact arm is bit-identical to the chain */
    CHECK(worst_chord<=1);      /* the approximate arm is at most one 8-bit level out */
    CHECK(worst5<=1);
    return 0;
}

/* ---- 2: the parameter space --------------------------------------------- */
/* Origin 0..3 reaches every Bayer phase and every tile and band edge; the two
 * extra origins land a box edge on a 64-pixel boundary and off the panel. */
static void translate(int dx,int dy){
    for(unsigned i=0;i<draw_count;i++){
        draws[i].bounds.x0+=(int16_t)dx;draws[i].bounds.x1+=(int16_t)dx;
        draws[i].bounds.y0+=(int16_t)dy;draws[i].bounds.y1+=(int16_t)dy;
        draws[i].clip.x0+=(int16_t)dx;draws[i].clip.x1+=(int16_t)dx;
        draws[i].clip.y0+=(int16_t)dy;draws[i].clip.y1+=(int16_t)dy;
    }
}
static int parameter_space(void){
    /* The mixed scene test_group_dither.c already uses: both gradient axes, with
     * and without dither, a rounded gradient (which must stay on the per-pixel
     * path), a hidden child, a zero-opacity child, images out of the tile. */
    const ksn_draw mixed[]={
        {.kind=KSN_RECT,.bounds={3,3,147,47},.clip={0,0,240,135},.opacity=201,.data.shape={0xd9867243,0,0}},
        {.kind=KSN_GRADIENT,.bounds={9,5,138,40},.clip={11,7,130,34},.opacity=219,
         .data.gradient={0x25384900,0xe0a972d2,0,0,true}},
        {.kind=KSN_RECT,.bounds={30,12,42,27},.clip={0,0,240,135},.opacity=255,.data.shape={0x708ca4ff,0,0}},
        {.kind=KSN_ROUND_RECT,.bounds={38,10,90,36},.clip={0,0,240,135},.opacity=177,.data.shape={0x357ecbb5,6,0}},
        {.kind=KSN_GRADIENT,.bounds={80,6,145,44},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0xa9b67aff,0x789bc4ff,1,4,false}},
        {.kind=KSN_GRADIENT,.bounds={3,3,145,45},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0x90909000,0xffffff00,1,0,true}},
        {.kind=KSN_GRADIENT,.bounds={3,3,147,47},.clip={0,0,240,135},.opacity=0,
         .data.gradient={0x123456ff,0xffff80ff,0,0,true}},
        {.kind=KSN_GRADIENT,.bounds={0,0,178,50},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0x123456ff,0xffff80ff,0,0,true}},
        {.kind=KSN_RECT,.bounds={150,5,176,20},.clip={0,0,240,135},.opacity=211,.data.shape={0x435769ff,0,0}},
        {.kind=KSN_GRADIENT,.bounds={20,60,236,120},.clip={0,0,240,135},.opacity=233,
         .data.gradient={0x000000ff,0x080808ff,0,0,false}}
    };
    /* A block whose run is shorter than the tile, at every run length a block can
     * have, both axes -- the smooth arm's anchor and increment change with the
     * run, not with the gradient's length. */
    const ksn_draw runs[]={
        {.kind=KSN_GRADIENT,.bounds={4,4,68,44},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0x102030ff,0xf0e0d0ff,0,0,false}},
        {.kind=KSN_GRADIENT,.bounds={4,4,68,44},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0x102030ff,0xf0e0d0ff,1,0,false}},
        {.kind=KSN_RECT,.bounds={4,4,68,44},.clip={0,0,240,135},.opacity=190,
         .data.shape={0x1a2b3cff,0,0}},
        {.kind=KSN_GRADIENT,.bounds={4,4,68,44},.clip={8,4,60,44},.opacity=255,
         .data.gradient={0x000000ff,0xffffffff,0,0,true}}
    };
    const unsigned opacities[]={255,254,1};
    const int origins[][2]={{0,0},{1,2},{3,3},{-20,-12},{35,17}};
    for(unsigned o=0;o<COUNT(origins);o++)for(unsigned al=0;al<COUNT(opacities);al++){
        memcpy(draws,mixed,sizeof(mixed));draw_count=COUNT(mixed);
        for(unsigned i=0;i<draw_count;i++)visible[i]=i!=7;
        translate(origins[o][0],origins[o][1]);
        CHECK(exact_arms(draws,visible,draw_count,(uint8_t)opacities[al],"mixed")==0);
        CHECK(approximate_arm(draws,visible,draw_count,(uint8_t)opacities[al],64,"mixed")==0);
        CHECK(approximate_arm(draws,visible,draw_count,(uint8_t)opacities[al],16,"mixed-16")==0);
        /* The independent reference is exact only where no TEXT child is in the
         * group (it has no ink model) and only for the exact arm; the mixed scene
         * has neither a TEXT child nor a smoothed arm in this comparison. */
        CHECK(render_frame(draws,visible,draw_count,(uint8_t)opacities[al],0x315d7bff,64,1,0,arm_a)==0);
        unsigned mismatches=0;
        CHECK(against_reference(arm_a,"mixed/exact arm",opacities[al],&mismatches)==0);
        CHECK(mismatches==0);
    }
    for(unsigned o=0;o<COUNT(origins);o++){
        memcpy(draws,runs,sizeof(runs));draw_count=COUNT(runs);
        for(unsigned i=0;i<draw_count;i++)visible[i]=true;
        translate(origins[o][0],origins[o][1]);
        CHECK(exact_arms(draws,visible,draw_count,255,"runs")==0);
        CHECK(approximate_arm(draws,visible,draw_count,255,64,"runs")==0);
        CHECK(approximate_arm(draws,visible,draw_count,255,16,"runs-16")==0);
    }
    CHECK(moved_none==0);        /* only a radius-0 gradient may move a pixel */
    CHECK(moved_y==0);           /* a child that does not vary along x is exact */
    CHECK(moved_x>1000);         /* ... and the other direction really is exercised */
    return 0;
}

/* ---- the 42-pixel instance the survey measured the waste on -------------- */
/* Two children of a group whose union is far wider than either: every block the
 * union spans is partly outside both. The counters below are the tile's own
 * pixels -- visited means the block loop entered them, covered means a child
 * actually wrote alpha there -- so the waste rate is 1 - covered/visited. */
#ifdef KSN_TILE_COUNT
static void reset_tile_counters(void){
    ksn_tile_visited=ksn_tile_covered=ksn_tile_blocks=ksn_tile_skipped=0;
    ksn_tile_smooth_blocks=ksn_tile_smooth_pixels=ksn_tile_child_pixels=0;
}
static void print_tile(const char *what){
    printf("tile %-18s block pixels %6" PRIu32 " covered %6" PRIu32 " untaken %5.2f%% | "
           "child loop %7" PRIu32 " waste %5.2f%% | blocks %5" PRIu32 " skipped %5" PRIu32
           " | smooth %" PRIu32 "/%" PRIu32 "\n",
           what,ksn_tile_visited,ksn_tile_covered,
           ksn_tile_visited?100.0*(double)(ksn_tile_visited-ksn_tile_covered)/(double)ksn_tile_visited:0.0,
           ksn_tile_child_pixels,
           ksn_tile_child_pixels?100.0*(double)(ksn_tile_child_pixels-ksn_tile_covered)/(double)ksn_tile_child_pixels:0.0,
           ksn_tile_blocks,ksn_tile_skipped,ksn_tile_smooth_blocks,ksn_tile_smooth_pixels);
}
#else
#define reset_tile_counters() do{}while(0)
#define print_tile(w) do{(void)(w);}while(0)
#endif
static int waste_scene(void){
    const ksn_draw instance[]={
        {.kind=KSN_RECT,.bounds={3,3,45,27},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x185071ff,0,0}},
        {.kind=KSN_ROUND_RECT,.bounds={5,5,43,25},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x63d7bcff,4,0}},
        {.kind=KSN_RECT,.bounds={80,6,145,44},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x3c2a18ff,0,0}},
        {.kind=KSN_GRADIENT,.bounds={131,6,145,44},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0xffffff44,0x000000ff,0,0,true}}
    };
    memcpy(draws,instance,sizeof(instance));draw_count=COUNT(instance);
    for(unsigned i=0;i<draw_count;i++)visible[i]=true;
    CHECK(exact_arms(draws,visible,draw_count,254,"waste")==0);
    const int arms[][3]={{64,0,0},{64,1,0},{16,0,0},{16,1,0},{16,0,2},{64,1,1},{16,1,1},{16,1,2}};
    const char *names[]={"tile64 reach-off","tile64 reach-on","tile16 reach-off",
                         "tile16 reach-on","tile16 smooth=2","tile64 smooth=1",
                         "tile16 smooth=1","tile16 reach+smooth2"};
    for(unsigned a=0;a<COUNT(arms);a++){
        reset_tile_counters();
        CHECK(render_frame(draws,visible,draw_count,254,0x315d7bff,arms[a][0],arms[a][1],
                           arms[a][2],arm_a)==0);
        print_tile(names[a]);
    }
    return 0;
}

/* A TEXT child in the same group: the reach window applies to it too, and the
 * smooth arm must leave it on the per-pixel path. The scalar reference cannot
 * answer for ink, so here the arms are compared with each other. */
static int text_scene(void){
    const ksn_draw group[]={
        {.kind=KSN_GRADIENT,.bounds={4,4,180,60},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0xd98672ff,0x357ecbff,0,0,true}},
        {.kind=KSN_TEXT,.bounds={-3,5,145,29},.clip={2,7,77,22},.opacity=255,
         .data.text={.utf8=title_text,.bytes=sizeof(title_text)-1,.capacity=64,
                     .font=KSN_BODY,.color=0xa15f3780}},
        {.kind=KSN_RECT,.bounds={40,20,200,90},.clip={0,0,240,135},.opacity=200,
         .data.shape={0x123456ff,0,0}}
    };
    for(unsigned o=0;o<4;o++){
        memcpy(draws,group,sizeof(group));draw_count=COUNT(group);
        for(unsigned i=0;i<draw_count;i++)visible[i]=true;
        translate((int)(o%2),(int)(o/2));
        CHECK(exact_arms(draws,visible,draw_count,255,"text")==0);
        CHECK(approximate_arm(draws,visible,draw_count,255,64,"text")==0);
        CHECK(approximate_arm(draws,visible,draw_count,255,16,"text")==0);
    }
    return 0;
}
/* ---- 3: 120 frames of a demo-shaped scene -------------------------------- */
static unsigned moved_frames,worst_frame_moved,exact_frames_pixels,frames_moved_total;
static uint32_t panel_hash(uint32_t state){
    const unsigned char *bytes=(const unsigned char *)panel;
    for(unsigned i=0;i<sizeof(panel);i++)state=(state^bytes[i])*16777619u;
    return state;
}
static int frames_120(void){
    const ksn_draw scene[]={
        {.kind=KSN_GRADIENT,.bounds={0,0,240,18},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0x0d2940ff,0x498781ff,0,0,true}},
        {.kind=KSN_RECT,.bounds={12,28,112,56},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x164c70ff,0,0}},
        {.kind=KSN_ROUND_RECT,.bounds={34,36,134,64},.clip={0,0,240,135},.opacity=255,
         .data.shape={0x65d7bcff,7,0}},
        {.kind=KSN_GRADIENT,.bounds={16,76,120,96},.clip={0,0,240,135},.opacity=210,
         .data.gradient={0xf5bd4fff,0x1d3b8aff,0,0,false}},
        {.kind=KSN_STROKE,.bounds={10,117,232,129},.clip={0,0,240,135},.opacity=170,
         .data.shape={0x80b5cfaa,0,1}},
        {.kind=KSN_GRADIENT,.bounds={130,60,236,124},.clip={0,0,240,135},.opacity=255,
         .data.gradient={0x000000ff,0xffffff44,0,0,true}},
        {.kind=KSN_GRADIENT,.bounds={4,4,20,132},.clip={0,0,240,135},.opacity=240,
         .data.gradient={0x223344ff,0xddeeff00,1,0,true}},
        {.kind=KSN_RECT,.bounds={150,20,200,50},.clip={0,0,240,135},.opacity=211,
         .data.shape={0x435769ff,0,0}}
    };
    uint32_t hashes[2]={2166136261u,2166136261u};
    ksn_client app=ksn_core_client(&core,KSN_APP);ksn_tx tx;ksn_ref refs[16];
    ksn_render_stats stats;
    g_ksn_tile_pixels=64;g_ksn_tile_reach=1;
    ksn_core_init(&core);
    memcpy(draws,scene,sizeof(scene));draw_count=COUNT(scene);
    for(unsigned i=0;i<draw_count;i++)visible[i]=true;
    background=0x0b1727ff;
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,background)==KSN_OK);
    for(unsigned i=0;i<draw_count;i++)CHECK(app.ops->add(app.ctx,tx,&draws[i],&refs[i])==KSN_OK);
    CHECK(ksn_core_group(&core,KSN_APP,tx,refs[0],(uint16_t)draw_count,255)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    /* The core commits on a render: without one, the first PATCH has no
     * committed bank to read and refuses to begin. */
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    unsigned moved_start=moved_pixels;
    for(unsigned frame=0;frame<120;frame++){
        /* A demo.js-shaped tick: a rect moves, a colour and an opacity change, a
         * child is hidden and shown, and every 17th frame the whole panel is
         * repainted instead of patched, so both damage paths are exercised. */
        draws[1].bounds.x0=(int16_t)(12+(int)(frame%40));
        draws[1].bounds.x1=(int16_t)(112+(int)(frame%40));
        draws[3].bounds.y0=(int16_t)(76+(int)(frame%5));
        draws[3].bounds.y1=(int16_t)(96+(int)(frame%5));
        draws[7].data.shape.color=(frame%3)?0x435769ff:0x8a5c22ff;
        visible[5]=(frame%7)!=0;
        if(frame%17==0)ksn_core_invalidate(&core);
        uint8_t op=frame%11?(uint8_t)(200+(frame%55)):128;
        CHECK(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
        ksn_change c={.property=KSN_SET_RECT,.value.rect=draws[1].bounds};
        CHECK(app.ops->change(app.ctx,tx,refs[1],&c)==KSN_OK);
        c=(ksn_change){.property=KSN_SET_RECT,.value.rect=draws[3].bounds};
        CHECK(app.ops->change(app.ctx,tx,refs[3],&c)==KSN_OK);
        c=(ksn_change){.property=KSN_SET_COLOR,.value.color=draws[7].data.shape.color};
        CHECK(app.ops->change(app.ctx,tx,refs[7],&c)==KSN_OK);
        c=(ksn_change){.property=KSN_SET_VISIBLE,.value.visible=visible[5]};
        CHECK(app.ops->change(app.ctx,tx,refs[5],&c)==KSN_OK);
        CHECK(ksn_core_group(&core,KSN_APP,tx,refs[0],(uint16_t)draw_count,op)==KSN_OK);
        CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
        g_ksn_tile_smooth=0;
        CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        memcpy(arm_a,panel,sizeof(panel));
        hashes[0]=panel_hash(hashes[0]);
        /* The old arm has to be the same picture as a whole-panel repaint of the
         * same state, or a moved pixel below could be the damage path ... */
        ksn_core_invalidate(&core);
        CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK&&stats.transferred_bytes==64800);
        if(memcmp(arm_a,panel,sizeof(panel))!=0){
            fprintf(stderr,"120 frames: frame %u patched and full repaint differ\n",frame);
            return 1;
        }
        /* ... and the exact increment arm has to reproduce it exactly, on the
         * patch/full mix the frame loop produces, not only on REPLACE frames. */
        ksn_core_invalidate(&core);
        g_ksn_tile_smooth=1;
        CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        if(memcmp(arm_a,panel,sizeof(panel))!=0){
            fprintf(stderr,"120 frames: frame %u exact increment arm moved a pixel\n",frame);
            return 1;
        }
        exact_frames_pixels+=240u*135u;
        ksn_core_invalidate(&core);
        g_ksn_tile_smooth=2;
        CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
        memcpy(arm_b,panel,sizeof(panel));
        hashes[1]=panel_hash(hashes[1]);
        unsigned before=moved_pixels,before_none=moved_none;
        measure_move(arm_a,arm_b,"120 frames");
        if(moved_none!=before_none){
            fprintf(stderr,"120 frames: frame %u moved a pixel no radius-0 gradient covers\n",frame);
            return 1;
        }
        if(moved_pixels>before){
            moved_frames++;
            if(moved_pixels-before>worst_frame_moved)worst_frame_moved=moved_pixels-before;
        }
    }
    frames_moved_total=moved_pixels-moved_start;
    printf("120 frames: %u of 120 frames moved a pixel, %u of %u pixels (%.2f%%), worst frame "
           "%u of 32400 (%.2f%%); exact arm %u pixels identical; old/approx hashes %08x / %08x\n",
           moved_frames,frames_moved_total,120u*240u*135u,
           100.0*(double)frames_moved_total/(120.0*240.0*135.0),worst_frame_moved,
           100.0*(double)worst_frame_moved/32400.0,exact_frames_pixels,hashes[0],hashes[1]);
    /* The two arms have to differ somewhere, or the comparison above is empty. */
    CHECK(moved_frames>0);
    return 0;
}
int main(void){
    CHECK(smooth_arithmetic()==0);
    CHECK(parameter_space()==0);
    CHECK(text_scene()==0);
    CHECK(waste_scene()==0);
    printf("parameter space: %u pixels compared, %u moved (%.2f%%): along x %u of %u, "
           "along y %u of %u, unexplained %u\n",
           compared_pixels,moved_pixels,100.0*(double)moved_pixels/(double)compared_pixels,
           moved_x,moved_x_pixels,moved_y,moved_y_pixels,moved_none);
    printf("approximate arm: worst step %u 5/6/5 level(s), %u expanded 8-bit level(s); "
           "exact arm: 0 (asserted above)\n",worst565,worst8);
    printf("exact arms: %u pixels identical (reach on/off, tile 64/16, smooth=1)\n",exact_pixels);
    CHECK(dither_differences>1000);
    CHECK(frames_120()==0);
#ifdef KSN_TILE_COUNT
    printf("tile counters: visited %" PRIu32 " covered %" PRIu32 " waste %.2f%%, blocks %" PRIu32
           " skipped %" PRIu32 ", smooth blocks %" PRIu32 " pixels %" PRIu32 "\n",
           ksn_tile_visited,ksn_tile_covered,
           ksn_tile_visited?100.0*(double)(ksn_tile_visited-ksn_tile_covered)/(double)ksn_tile_visited:0.0,
           ksn_tile_blocks,ksn_tile_skipped,ksn_tile_smooth_blocks,ksn_tile_smooth_pixels);
#endif
    printf("group tile: PASS (arithmetic swept, exact arms identical, approximation "
           "measured and named, 120 frames compared)\n");
    return 0;
}
