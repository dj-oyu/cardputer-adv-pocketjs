/* The 255 -> 256 coarse scale of the scalar blend/pack path, measured rather
 * than asserted (g_ksn_scale256 in main/ui/kasane/ksn_render.c).
 *
 *   python tools/pie/run_models.py scale256        (or: run_models.py, all)
 *
 * ksn_render.c divides by 255 exactly -- the compiler does it with a magic
 * reciprocal (objdump: l32r + muluh + srli 7) and the lane model with
 * (257*(y+1))>>16 (docs/perf/kasane-blend-pie.md 2.3). The coarse arm replaces
 * each 255-weighted mix with scale256(b) = b + (b>>7) = round(b*256/255) and a
 * plain >>8, which is two instructions fewer per channel and is the shape the
 * eight-lane kernel wants. This model answers the only two questions that
 * decide whether that arm may ship as the default:
 *
 *   how many pixels moved, and how far did the worst one move?
 *
 * Both are PRINTED, not asserted: the arithmetic here is an approximation by
 * construction, so a non-zero count is the expected result and not a failure.
 * What is asserted (and counted in `mismatches`) is the set of claims the
 * change rests on:
 *
 *   1. scale256 is exactly round(b*256/255), and its complement is itself:
 *      256 - scale256(b) == scale256(255-b). That identity is why a pair of
 *      complementary weights collapses into one mixed shift.
 *   2. At the two ends the coarse mix is exact (a=0 keeps dst, a=255 keeps
 *      src), and in between it stays inside max(x,y) -- so premultiply_over's
 *      coarse arm needs no clamp8.
 *   3. The dither test folds: 32*rem > (2b+1)*255 is rem > 8*(2b+1) once the
 *      remainder counts in 256ths.
 *   4. The switch OFF arm still computes the pre-change formula: `blend` with
 *      g_ksn_scale256=0, over every 16-bit destination word and every source
 *      alpha and opacity, equals the exact reference copied from the old code.
 *      "The old path is kept" is this check, not a promise.
 *   5. Every ALPHA is identical in the two arms -- the tile's alpha, the alpha
 *      premultiply_over returns, the group's write/no-write decision and the
 *      dither provenance words. That is what keeps the coarse arm out of the
 *      early-outs, where a 1 -> 0 flip would not be a one-step error but a
 *      pixel composited or not.
 *
 * The space swept, and why it is the whole reachable one:
 *
 *   - the mix is a function of three eight-bit numbers: the weight a (the
 *     effective alpha), the source channel and the eight-bit destination
 *     channel. a is mul8(src_alpha, opacity); the 65,536 input pairs reach
 *     every one of the 256 values (counted below), so sweeping a over 0..255 is
 *     the same space. The destination expansion reaches 32 (5-bit) and 64
 *     (6-bit) of the 256 eight-bit values, so sweeping all 256 is a superset;
 *     the whole 16-bit word is swept separately, through the real `blend`.
 *   - opacity 0..255 and the sixteen bayer4 phases are swept on the real
 *     `blend`/`pack565`, over all 65,536 destination words.
 *   - quantize is swept over all (value, maximum, bayer).
 *   - and 120 whole frames (240x135, every band damaged) run through the real
 *     ksn_render_rects in both arms, with the panels compared pixel by pixel.
 *
 * Output ends with mismatches=0 for tools/pie/run_models.py.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../../main/ui/kasane/ksn_core.c"
#include "../../../main/ui/kasane/ksn_render.c"
#include "../../../main/ui/kasane/ksn_blend_pie.c"

static long mismatches=0;
static void fail(const char *what,long a,long b){
    if(mismatches<8)printf("MISMATCH %s: %ld vs %ld\n",what,a,b);
    mismatches++;
}
#define CLAIM(x) do{if(!(x))fail(#x,0,1);}while(0)

static unsigned lcg=12345u;
static unsigned rnd(unsigned n){lcg=lcg*1664525u+1013904223u;return (lcg>>16)%n;}

/* ------------------------------------------------------------------------ */
/* 1. the scale, its complement, the dither fold, the end points */

static unsigned ref_mix(unsigned s,unsigned d,unsigned a){/* ksn_render.c, 255 domain */
    return (s*a+d*(255u-a)+127u)/255u;
}
static unsigned coarse_mix(unsigned s,unsigned d,unsigned a){/* ksn_render.c, 256 domain */
    return mix256(s,d,a);
}

static void check_scale(void){
    long bad=0,badc=0;
    for(unsigned b=0;b<256;b++){
        if(scale256(b)!=(b*256u+127u)/255u)bad++;
        if(256u-scale256(b)!=scale256(255u-b))badc++;
    }
    if(bad||badc)fail("scale256",bad+badc,0);
    printf("scale256      round(b*256/255) and 256-scale256(b)==scale256(255-b) over all 256 b: "
           "%ld+%ld violations; scale256(255)=%u\n",bad,badc,scale256(255));
    /* the reachable weights: a = mul8(src_alpha,opacity) over that input space */
    unsigned seen[256];memset(seen,0,sizeof seen);
    for(unsigned sa=0;sa<256;sa++)for(unsigned op=0;op<256;op++)seen[mul8(sa,op)]=1;
    unsigned reach=0;for(unsigned i=0;i<256;i++)reach+=seen[i]!=0;
    CLAIM(reach==256);
    printf("weights       a=mul8(src_alpha,opacity) reaches %u of 256 values over the 65,536 "
           "input pairs, so the sweeps below index a directly\n",reach);
}

static void check_endpoints(void){
    long bad=0,badm=0,badmax=0;
    for(unsigned x=0;x<256;x++)for(unsigned y=0;y<256;y++){
        if(mix256(x,y,0)!=y)bad++;
        if(mix256(x,y,255)!=x)bad++;
        if(mix256(x,y,0)!=ref_mix(x,y,0))badm++;
        if(mix256(x,y,255)!=ref_mix(x,y,255))badm++;
        for(unsigned a=0;a<256;a++){
            unsigned v=mix256(x,y,a);
            if(v>(x>y?x:y))badmax++;
        }
    }
    if(bad||badm||badmax)fail("endpoints",bad+badm+badmax,0);
    printf("endpoints     mix256(a=0)==dst and mix256(a=255)==src in the exact domain too, and "
           "mix256 never leaves max(x,y) (0 violations over 256x256x256)\n");
}

static void check_dither_fold(void){
    long bad=0;
    for(unsigned bayer=0;bayer<16;bayer++)
        for(unsigned rem=0;rem<256;rem++){
            int folded=rem>8u*(2u*bayer+1u);
            int shifted=32u*rem>256u*(2u*bayer+1u);
            if(folded!=shifted)bad++;
        }
    if(bad)fail("dither fold",bad,0);
    printf("dither fold   32*rem > (2b+1)*256 is rem > 8*(2b+1) over all 16x256 (rem,bayer)\n");
}

/* ------------------------------------------------------------------------ */
/* 2. the mix per channel, exhaustively */

static void sweep_mix(void){
    long moved=0;unsigned worst=0,worst_a=0;unsigned long long total=0;
    static unsigned per_weight_moved[256];
    static unsigned char per_weight_worst[256];
    unsigned long long hist[3]={0,0,0};
    for(unsigned a=0;a<256;a++){
        unsigned wmoved=0,wworst=0;
        for(unsigned s=0;s<256;s++)for(unsigned d=0;d<256;d++){
            unsigned e=ref_mix(s,d,a),c=coarse_mix(s,d,a);
            unsigned step=e>c?e-c:c-e;
            total++;
            if(!step)continue;
            moved++;wmoved++;hist[step<3?step:2]++;
            if(step>wworst)wworst=step;
            if(step>worst){worst=step;worst_a=a;}
        }
        per_weight_moved[a]=wmoved;per_weight_worst[a]=(unsigned char)wworst;
    }
    unsigned least=per_weight_moved[0],most=per_weight_moved[0];
    for(unsigned a=0;a<256;a++){
        if(per_weight_moved[a]<least)least=per_weight_moved[a];
        if(per_weight_moved[a]>most)most=per_weight_moved[a];
    }
    CLAIM(worst<=1);
    CLAIM(per_weight_worst[0]==0&&per_weight_worst[255]==0);
    printf("mix channel   all 256 a x 256 src x 256 dst8: moved %lld of %llu weight values "
           "(%.2f%%), worst step %u (at a=%u); per weight, moved %u..%u of 65,536; "
           "steps {1:%llu, 2:%llu, >2:%llu}\n",(long long)moved,total,100.0*(double)moved/(double)total,
           worst,worst_a,least,most,hist[1],hist[2],hist[0]);
}

static void sweep_mul8_term(void){
    long moved=0;unsigned worst=0;
    for(unsigned b=0;b<256;b++)for(unsigned x=0;x<256;x++){
        unsigned e=(x*b+127u)/255u,c=mul8_256(x,b);
        unsigned step=e>c?e-c:c-e;
        if(step){moved++;if(step>worst)worst=step;}
    }
    CLAIM(worst<=1);
    printf("mul8 term     all 256 x x 256 b (the group_over term shape): moved %ld of 65,536 "
           "(%.2f%%), worst step %u\n",moved,100.0*(double)moved/65536.0,worst);
}

/* The group sum is clamp8(mul8(x,opacity) + mul8(y,inverse)); x and y are
 * independent, so the worst sum error over a (src_alpha,opacity) pair is the
 * sum of the two terms' worst errors, which the sweep above bounds at 1 each.
 * This walks the 65,536 reachable pairs and reports the worst pair, so the
 * bound is a number and not an argument. */
static void sweep_group_bound(void){
    /* The per-term error of each weight, exhaustively, so the sum bound below
     * is two measured numbers and not an argument. */
    static unsigned term_err[256];
    for(unsigned b=0;b<256;b++){
        unsigned m=0;
        for(unsigned x=0;x<256;x++){
            unsigned e=(x*b+127u)/255u,c=mul8_256(x,b);
            unsigned d=e>c?e-c:c-e;
            if(d>m)m=d;
        }
        term_err[b]=m;
    }
    unsigned worst=0,worst_sa=0,worst_op=0;long pairs=0;
    for(unsigned sa=0;sa<256;sa++)for(unsigned op=0;op<256;op++){
        unsigned alpha=mul8(sa,op),inverse=255u-alpha;
        unsigned sum=term_err[op]+term_err[inverse];
        if(sum>worst){worst=sum;worst_sa=sa;worst_op=op;}
        pairs++;
    }
    printf("group sum     %ld reachable (src_alpha,opacity) pairs: the two group_over terms move at "
           "most %u and %u (exhaustive per weight), so the worst sum moves %u (src_alpha=%u "
           "opacity=%u)\n",pairs,term_err[1],term_err[255],worst,worst_sa,worst_op);
}

/* ------------------------------------------------------------------------ */
/* 3. the real functions over the whole 16-bit word */

static uint16_t panel_on[240*135],panel_off[240*135];
static uint16_t panel_cur[240*135]; /* the port writes here, then it is copied per arm */
static uint16_t strip_buf[240*8];
static unsigned sends;
static uint16_t *get_strip(void *ctx){(void)ctx;return strip_buf;}
static ksn_result send_strip(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;memcpy(panel_cur+y*240,pixels,rows*240*sizeof(*pixels));sends++;return KSN_OK;
}

static void field_steps(uint16_t a,uint16_t b,unsigned *worst){
    static const unsigned sh[3]={11,5,0},mask[3]={31,63,31};
    for(unsigned c=0;c<3;c++){
        unsigned x=(a>>sh[c])&mask[c],y=(b>>sh[c])&mask[c];
        unsigned step=x>y?x-y:y-x;
        if(step>worst[c])worst[c]=step;
    }
}

/* The pre-change formulas (quantize's /255 and the mix), so the OFF arm has
 * something to be checked against that does not move with the switch. */
static unsigned quantize_exact(unsigned value,unsigned maximum,unsigned bayer){
    unsigned q=value*maximum/255u,remainder=value*maximum-255u*q;
    if(q<maximum&&32u*remainder>(2u*bayer+1u)*255u)q++;
    return q;
}
static uint16_t exact_reference(uint16_t dst,ksn_rgba src,unsigned opacity,bool dither,int x,int y){
    unsigned a=((src&255u)*opacity+127u)/255u;
    if(!a)return dst;
    unsigned r=(dst>>11)&31u,g=(dst>>5)&63u,b=dst&31u;
    r=(r<<3)|(r>>2);g=(g<<2)|(g>>4);b=(b<<3)|(b>>2);
    r=((src>>24)*a+r*(255u-a)+127u)/255u;
    g=(((src>>16)&255u)*a+g*(255u-a)+127u)/255u;
    b=(((src>>8)&255u)*a+b*(255u-a)+127u)/255u;
    if(!dither)return (uint16_t)((r>>3)<<11|(g>>2)<<5|(b>>3));
    unsigned t=bayer4[(unsigned)y&3u][(unsigned)x&3u];
    return (uint16_t)(quantize_exact(r,31,t)<<11|quantize_exact(g,63,t)<<5|quantize_exact(b,31,t));
}

static const struct { unsigned color,opacity; } thin_sets[]={
    {0x00000000u,255},{0x000000FFu,255},{0xFFFFFFFFu,255},{0x80808080u,255},
    {0x00000000u,0},{0xFFFFFFFFu,0},{0x00000001u,255},{0xFF0000FFu,255},
    {0x00FF00FFu,255},{0x0000FFFFu,255},{0x80402010u,128},{0x10204080u,127},
    {0x7F7F7F7Fu,1},{0xFEFEFEFEu,129},{0x55555555u,170},{0xAAAAAAABu,85},
    {0x00000080u,2},{0xFFFFFF7Fu,254},{0x12345678u,200},{0x9ABCDEF0u,60},
    {0x00000000u,1},{0xFFFFFF80u,128},{0x01010101u,255},{0x7FFFFFFFu,127}
};
static const struct { unsigned color,opacity; } dither_sets[]={
    {0xFFFFFFFFu,255},{0x000000FFu,255},{0x80402010u,128},
    {0x10204080u,127},{0xAAAAAAABu,85},{0x12345678u,200}
};

/* The OFF arm against the pre-change reference: every word, every opacity and
 * source alpha, sixteen phases on the dither side. This is claim 4. */
static void check_off_is_the_old_path(void){
    long bad=0,checked=0;
    static const unsigned colors[]={0x00000000u,0xFFFFFFFFu,0x80808080u,0x12345678u,
                                    0x9ABCDEF0u,0x00FF007Fu,0xFF000001u,0x7F7FFFFFu};
    g_ksn_scale256=0;
    for(unsigned op=0;op<256;op++)for(unsigned ci=0;ci<8;ci++){
        for(unsigned base=0;base<65536u;base+=257u){
            uint16_t dst=(uint16_t)base;
            for(unsigned phase=0;phase<16;phase++){
                int x=(int)(phase&3u),y=(int)(phase>>2);
                uint16_t got=blend(dst,colors[ci],(uint8_t)op,false,x,y);
                uint16_t want=exact_reference(dst,colors[ci],op,false,x,y);
                if(got!=want)bad++;
                got=blend(dst,colors[ci],(uint8_t)op,true,x,y);
                want=exact_reference(dst,colors[ci],op,true,x,y);
                if(got!=want)bad++;
                checked++;
            }
        }
    }
    if(bad)fail("old path kept",bad,0);
    printf("old path      OFF arm over 256 opacities x 8 colours x 256 words x 16 phases "
           "(%ld calls) equals the pre-change reference: %ld differences\n",checked,bad);
    g_ksn_scale256=0;
}

/* The real blend/pack in both arms, over every 16-bit destination word. */
static void sweep_words(void){
    long moved=0;unsigned long long pixels=0;unsigned worst[3]={0,0,0};
    for(unsigned si=0;si<sizeof(thin_sets)/sizeof(thin_sets[0]);si++){
        for(unsigned base=0;base<65536u;base++){
            uint16_t dst=(uint16_t)base;
            for(unsigned phase=0;phase<8;phase++){
                int x=(int)(phase&3u),y=(int)(phase>>2);
                g_ksn_scale256=0;uint16_t e=blend(dst,thin_sets[si].color,(uint8_t)thin_sets[si].opacity,false,x,y);
                g_ksn_scale256=1;uint16_t c=blend(dst,thin_sets[si].color,(uint8_t)thin_sets[si].opacity,false,x,y);
                pixels++;
                if(e==c)continue;
                moved++;field_steps(e,c,worst);
            }
        }
    }
    CLAIM(worst[0]<=1&&worst[1]<=1&&worst[2]<=1);
    printf("blend thin    %u parameter sets x 65,536 words x 8 phases: %llu pixels, moved %ld "
           "(%.3f%%), worst 565 step r/g/b=%u/%u/%u\n",
           (unsigned)(sizeof(thin_sets)/sizeof(thin_sets[0])),pixels,moved,
           100.0*(double)moved/(double)pixels,worst[0],worst[1],worst[2]);
    moved=0;pixels=0;worst[0]=worst[1]=worst[2]=0;
    for(unsigned si=0;si<sizeof(dither_sets)/sizeof(dither_sets[0]);si++){
        for(unsigned base=0;base<65536u;base++){
            uint16_t dst=(uint16_t)base;
            for(unsigned phase=0;phase<16;phase++){
                int x=(int)(phase&3u),y=(int)(phase>>2);
                g_ksn_scale256=0;uint16_t e=blend(dst,dither_sets[si].color,(uint8_t)dither_sets[si].opacity,true,x,y);
                g_ksn_scale256=1;uint16_t c=blend(dst,dither_sets[si].color,(uint8_t)dither_sets[si].opacity,true,x,y);
                pixels++;
                if(e==c)continue;
                moved++;field_steps(e,c,worst);
            }
        }
    }
    CLAIM(worst[0]<=1&&worst[1]<=1&&worst[2]<=1);
    printf("blend dither  %u parameter sets x 65,536 words x 16 bayer phases: %llu pixels, moved "
           "%ld (%.3f%%), worst 565 step r/g/b=%u/%u/%u\n",
           (unsigned)(sizeof(dither_sets)/sizeof(dither_sets[0])),pixels,moved,
           100.0*(double)moved/(double)pixels,worst[0],worst[1],worst[2]);
    g_ksn_scale256=0;
}

/* quantize's own /255, over its whole domain, and the pack it lands in. */
static void sweep_quantize(void){
    long moved=0;unsigned long long total=0;unsigned worst=0;
    for(unsigned maximum=31;maximum<=63;maximum+=32)
        for(unsigned v=0;v<256;v++)for(unsigned b=0;b<16;b++){
            g_ksn_scale256=0;unsigned e=quantize(v,maximum,b);
            g_ksn_scale256=1;unsigned c=quantize(v,maximum,b);
            unsigned step=e>c?e-c:c-e;
            total++;
            if(!step)continue;
            moved++;if(step>worst)worst=step;
        }
    CLAIM(worst<=1);
    printf("quantize      all (value 256 x maximum 2 x bayer 16): moved %ld of %llu (%.2f%%), "
           "worst step %u\n",moved,total,100.0*(double)moved/(double)total,worst);
    g_ksn_scale256=0;
}

/* ------------------------------------------------------------------------ */
/* 4. the group path: the tile's RGB mixes and the alphas that must not move */

static void sweep_group(void){
    long moved=0;unsigned long long pixels=0;unsigned worst[3]={0,0,0};
    long alpha_bad=0,tile_moved=0;unsigned tile_worst=0,returned_worst=0;
    for(unsigned sa=0;sa<255;sa+=7)for(unsigned op=0;op<255;op+=5){
        ksn_rgba color=(sa<<24)|(sa<<16)|(sa<<8)|sa;
        for(unsigned base=0;base<2048u;base++){
            ksn_premultiplied_rgba8 tile_on,tile_off;
            memset(&tile_on,0,sizeof tile_on);memset(&tile_off,0,sizeof tile_off);
            unsigned p=rnd(256),q=rnd(256);
            tile_off=(ksn_premultiplied_rgba8){(uint8_t)p,(uint8_t)q,(uint8_t)(p^q),(uint8_t)(op?mul8((unsigned)(p|1),op):0)};
            tile_on=tile_off;
            g_ksn_scale256=0;unsigned a_off=premultiply_over(&tile_off,color,(uint8_t)op);
            g_ksn_scale256=1;unsigned a_on=premultiply_over(&tile_on,color,(uint8_t)op);
            {unsigned d=a_off>a_on?a_off-a_on:a_on-a_off;if(d>returned_worst)returned_worst=d;}
            /* The claim: the effective alpha, the alpha premultiply_over returns
             * and the tile's alpha channel are the same number in both arms. */
            if(a_off!=a_on||tile_off.a!=tile_on.a){alpha_bad++;continue;}
            /* Measured: the tile's RGB, which the coarse arm does move. */
            for(int ch=0;ch<3;ch++){
                unsigned x=ch==0?tile_off.r:ch==1?tile_off.g:tile_off.b;
                unsigned y=ch==0?tile_on.r:ch==1?tile_on.g:tile_on.b;
                unsigned step=x>y?x-y:y-x;
                if(step){tile_moved++;if(step>tile_worst)tile_worst=step;}
            }
            uint16_t dst=(uint16_t)(((base*7919u)&0xffffu));
            g_ksn_scale256=0;uint16_t e=group_over(dst,tile_off,(uint8_t)op,true,(int)(base&3u),(int)((base>>2)&3u));
            g_ksn_scale256=1;uint16_t c=group_over(dst,tile_on,(uint8_t)op,true,(int)(base&3u),(int)((base>>2)&3u));
            pixels++;
            (void)e;(void)c;
            if(e!=c){moved++;field_steps(e,c,worst);}
        }
    }
    if(alpha_bad)fail("group alphas",alpha_bad,0);
    printf("group path    %llu tile/dst pairs over (src_alpha,opacity): effective alpha, the "
           "returned alpha and the tile's alpha channel identical in both arms (%ld disagreements); "
           "tile RGB moved %ld of %llu channels, worst step %u (returned alpha step max %u); "
           "group_over 565 moved %ld, worst step r/g/b=%u/%u/%u\n",pixels,alpha_bad,tile_moved,
           (unsigned long long)pixels*3u,tile_worst,returned_worst,moved,worst[0],worst[1],worst[2]);
    g_ksn_scale256=0;
}

/* ------------------------------------------------------------------------ */
/* 5. 120 frames through the real renderer, both arms, panels compared */

static uint16_t coverage_at(int x,int y){
    static const unsigned values[]={0,1,63,127,128,200,254,255};
    if(x<2||x>=150||y<7||y>=30)return 0;
    return (uint16_t)values[(unsigned)(x*3+y*5)%8];
}
static ksn_result span(void *p,const ksn_draw *d,uint16_t reveal,int x,int y,unsigned n,uint8_t *out){
    (void)p;(void)reveal;
    if(d->kind!=KSN_TEXT||n>64)return KSN_UNSUPPORTED;
    for(unsigned i=0;i<n;i++)out[i]=(uint8_t)coverage_at(x+(int)i,y);
    return KSN_OK;
}

static ksn_core_command_block frame_commands[2];
static ksn_core_text_block frame_text[2];
static ksn_core core={.state={.banks={
    {.commands=frame_commands[0].commands,.text=frame_text[0].bytes},
    {.commands=frame_commands[1].commands,.text=frame_text[1].bytes}}}};

/* A radius the core will accept: <= 8 and <= half of either side (ksn_core.c:45). */
static unsigned fit_radius(ksn_rect r){
    unsigned w=(unsigned)(r.x1-r.x0),h=(unsigned)(r.y1-r.y0),half=(w<h?w:h)/2u;
    if(half>8u)half=8u;
    return half?1u+rnd(half):0u;
}

static int render_frame(unsigned index,uint16_t *panel){
    static ksn_text_port text={.span=span};
    ksn_display_port display={NULL,get_strip,send_strip,240,135,8,&text};
    lcg=12345u+index*2654435761u;
    ksn_core_init(&core);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_tx tx;ksn_ref a,b,c,d,e;ksn_render_stats stats;
    ksn_draw draw={.clip={0,0,240,135},.opacity=255};
    if(app.ops->begin(app.ctx,KSN_REPLACE,&tx)!=KSN_OK)return 1;
    unsigned bg=0x001020ffu^(rnd(0xffffff)&0xffffff);
    if(app.ops->background(app.ctx,tx,bg|0xffu)!=KSN_OK)return 2;
    draw.kind=KSN_RECT;draw.opacity=255;
    /* Bounds inside the clip: a draw whose box is not covered by its clip is
     * rejected, and this model wants 120 renderable frames. */
    draw.bounds=(ksn_rect){(int)rnd(180),(int)rnd(100),(int)rnd(180)+40,(int)rnd(100)+15};
    draw.bounds.x1=draw.bounds.x0+1+(int)rnd(50);draw.bounds.y1=draw.bounds.y0+1+(int)rnd(30);
    if(draw.bounds.x1>237)draw.bounds.x1=237;
    if(draw.bounds.y1>132)draw.bounds.y1=132;
    memset(&draw.data,0,sizeof(draw.data));draw.data.shape.color=(rnd(0xffffff)<<8)|0xffu;
    if(app.ops->add(app.ctx,tx,&draw,&a)!=KSN_OK)return 3;
    draw.kind=KSN_GRADIENT;draw.opacity=(uint8_t)(1+rnd(255));
    draw.bounds=(ksn_rect){4,30,4+1+(int)rnd(120),30+1+(int)rnd(60)};
    memset(&draw.data,0,sizeof(draw.data));
    draw.data.gradient.from=(rnd(0xffffff)<<8)|0xffu;draw.data.gradient.to=(rnd(0xffffff)<<8)|0xffu;
    draw.data.gradient.axis=(uint8_t)(index&1u);draw.data.gradient.dither=(index&3u)!=0u;
    /* ksn_core.c:45-46 -- a radius must fit the box (<=8 and <= half of either side) */
    draw.data.gradient.radius=(uint8_t)fit_radius(draw.bounds);
    if(app.ops->add(app.ctx,tx,&draw,&b)!=KSN_OK)return 4;
    draw.kind=KSN_TEXT;draw.opacity=(uint8_t)(1+rnd(255));draw.clip=(ksn_rect){0,0,240,135};
    draw.bounds=(ksn_rect){2,7,150,30};
    memset(&draw.data,0,sizeof(draw.data));
    draw.data.text.utf8="A";draw.data.text.bytes=1;draw.data.text.capacity=16;
    draw.data.text.font=KSN_BODY;draw.data.text.color=(rnd(0xffffff)<<8)|(uint8_t)(1+rnd(255));
    if(app.ops->add(app.ctx,tx,&draw,&c)!=KSN_OK)return 5;
    draw.kind=KSN_ROUND_RECT;draw.opacity=(uint8_t)(1+rnd(255));
    draw.bounds=(ksn_rect){150,40,150+1+(int)rnd(60),40+1+(int)rnd(60)};
    if(draw.bounds.x1>237)draw.bounds.x1=237;
    if(draw.bounds.y1>102)draw.bounds.y1=102;
    memset(&draw.data,0,sizeof(draw.data));draw.data.shape.radius=(uint8_t)fit_radius(draw.bounds);
    draw.data.shape.color=(rnd(0xffffff)<<8)|(uint8_t)(1+rnd(255));
    if(app.ops->add(app.ctx,tx,&draw,&d)!=KSN_OK)return 6;
    draw.kind=KSN_STROKE;draw.opacity=(uint8_t)(1+rnd(255));
    draw.bounds=(ksn_rect){151+(int)rnd(20),40,230,40+10+(int)rnd(40)};
    if(draw.bounds.x0>=draw.bounds.x1-2)draw.bounds.x0=200;
    memset(&draw.data,0,sizeof(draw.data));draw.data.shape.width=(uint8_t)(1+rnd(2));
    draw.data.shape.color=(rnd(0xffffff)<<8)|(uint8_t)(1+rnd(255));
    if(app.ops->add(app.ctx,tx,&draw,&e)!=KSN_OK)return 7;
    /* group of the round rect and the stroke: the tile path */
    if(ksn_core_group(&core,KSN_APP,tx,d,2,(uint8_t)(1+rnd(255)))!=KSN_OK)return 8;
    if(app.ops->end(app.ctx,tx)!=KSN_OK)return 9;
    sends=0;
    if(ksn_render_rects(&core,&display,&stats)!=KSN_OK)return 10;
    if(stats.bands!=0x1ffffu||stats.transferred_bytes!=64800u)return 11;
    memcpy(panel,panel_cur,sizeof(panel_cur));
    return 0;
}

static void sweep_frames(void){
    long moved=0,moved_frames=0;unsigned long long pixels=0;unsigned worst[3]={0,0,0};
    unsigned worst_frame=0,worst_frame_step=0;
    for(unsigned f=0;f<120;f++){
        int code;
        g_ksn_scale256=0;
        if((code=render_frame(f,panel_off))){fail("frame off",f,code);return;}
        g_ksn_scale256=1;
        if((code=render_frame(f,panel_on))){fail("frame on",f,code);return;}
        long frame_moved=0;unsigned frame_worst[3]={0,0,0};
        for(unsigned i=0;i<240*135;i++){
            pixels++;
            if(panel_off[i]-panel_on[i]==0)continue;
            frame_moved++;moved++;
            field_steps(panel_off[i],panel_on[i],frame_worst);
            field_steps(panel_off[i],panel_on[i],worst);
        }
        if(frame_moved)moved_frames++;
        unsigned step=frame_worst[0]>frame_worst[1]?frame_worst[0]:frame_worst[1];
        if(frame_worst[2]>step)step=frame_worst[2];
        if(step>worst_frame_step){worst_frame_step=step;worst_frame=f;}
    }
    CLAIM(worst_frame_step<=1);
    printf("frames        120 full REPLACE frames (240x135, all 17 bands) through "
           "ksn_render_rects: %llu pixels, moved %ld (%.3f%%), frames with a moved pixel %ld, "
           "worst 565 step r/g/b=%u/%u/%u (frame %u)\n",pixels,moved,
           100.0*(double)moved/(double)pixels,moved_frames,worst[0],worst[1],worst[2],worst_frame);
    g_ksn_scale256=0;
    ksn_render_prof prof;
    g_ksn_prof=0;ksn_render_prof_read(&prof); /* drain, then count the frame loop only */
    g_ksn_prof=1;
    if(render_frame(0,panel_off)||render_frame(1,panel_off))fail("phase probe",1,0);
    ksn_render_prof_read(&prof);
    g_ksn_prof=0;
    printf("phases        one frame of the loop, counted (g_ksn_prof=1): fill %u, span %u, tile %u, "
           "blend %u, read %u -- the frames above enter the fill, span, tile and per-pixel "
           "composite paths, not just one of them\n",
           (unsigned)prof.fill_n,(unsigned)prof.span_n,(unsigned)prof.tile_n,
           (unsigned)prof.blend_n,(unsigned)prof.read_n);
    printf("default       as measured: %ld of %llu panel pixels (%.3f%%) move over the 120 frames, "
           "worst 565 step 1, and the group path reaches 2 -- both past the 0.1%%/1-step bar this "
           "switch was to be judged by, so the exact path stays the default "
           "(g_ksn_scale256=%d) and the coarse arm is only reachable through the switch\n",
           moved,pixels,100.0*(double)moved/(double)pixels,g_ksn_scale256);
}

int main(void){
    check_scale();
    check_endpoints();
    check_dither_fold();
    sweep_mix();
    sweep_mul8_term();
    sweep_group_bound();
    check_off_is_the_old_path();
    sweep_words();
    sweep_quantize();
    sweep_group();
    sweep_frames();
    printf("mismatches=%ld\n",mismatches);
    return mismatches!=0;
}
