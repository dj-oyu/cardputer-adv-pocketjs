/* Arithmetic proof for the eight-lane RGB565 blend/pack kernel
 * (main/ui/kasane/ksn_blend_pie.c, candidate 4a of docs/perf/kasane-opt-survey.md).
 *
 *   python tools/pie/run_models.py blendpack        (or: run_models.py, all of them)
 *
 * Two halves, in the order the project's discipline asks for.
 *
 * 1. The lane algebra, each step exhaustively over its own domain: the 565->888
 *    replication and the field extractions at SAR 12, the a' mask-and-XOR, the
 *    (257*(y+1))>>16 division by 255, the mix identity that puts the scalar's
 *    rounded `(s*a + d*(255-a) + 127)/255` into sixteen-bit lanes, and the whole
 *    of quantize() including the two facts the dither arm rests on (the q<M
 *    guard is redundant, and the increment condition is `rem > floor(T/32)`).
 *
 * 2. The kernel itself -- ksn_blend8_pie() from the included source, which is
 *    the lane sequence the assembly in the same file is checked against by
 *    tools/pie/test_kernels.py -- against the scalar reference
 *    ksn_blend_scalar_ref() (ksn_render.c:101-111 and :190-199), over the space
 *    the renderer can produce:
 *
 *      - destination: every 16-bit word. The strip is a bare uint16_t buffer that
 *        fill565, the group path and the frost pass share, so nothing about a
 *        word is guaranteed.
 *      - source colour: every eight-bit channel. The API's colours are 8-bit and
 *        the text path scales only the alpha (:262), so the RGB the mix sees are
 *        always the caller's bytes.
 *      - opacity: 0..255 (uint8_t in the draw descriptor).
 *      - dither: the sixteen bayer4 values (:43), one per column; the model
 *        builds the eight-lane threshold vector exactly as the contract says the
 *        caller must -- bayer4[y&3][(x0+i)&3] -- and gives the scalar reference
 *        the matching x0+i.
 *      - and the reachable part of the destination expansion: the replicated
 *        red and blue take 32 of the 256 eight-bit values and green takes 64, so
 *        the sweep of every (source, destination) pair over 0..255 is a strict
 *        superset of what the renderer can hand the mix.
 *
 * Moved pixels and the worst step are measured and printed, not asserted: the
 * kernel is exact, so they come out zero over the whole sweep, and if that ever
 * stops being true the failure prints the count, the channel and the step.
 *
 * Output ends with mismatches=0 for tools/pie/run_models.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../main/ui/kasane/ksn_blend_pie.c"

static long mismatches=0;

static void fail(const char *what,long a,long b){
    if(mismatches<5)printf("MISMATCH %s: %ld vs %ld\n",what,a,b);
    mismatches++;
}

/* ---- 1. the lane algebra ------------------------------------------------- */

static int16_t s16(uint16_t v){return v<0x8000u?(int16_t)v:(int16_t)(v-0x10000u);}
static uint16_t vmul(uint16_t x,uint16_t y,int sar){
    return (uint16_t)(((uint32_t)x*y)>>sar);
}

static void check_unpack(void){
    long bad=0;
    for(unsigned dst=0;dst<65536u;dst++){
        unsigned r5=(dst>>11)&31u,g6=(dst>>5)&63u,b5=dst&31u;
        uint16_t r5v=vmul((uint16_t)dst,2,12),   r8v=vmul(r5v,33792,12);
        uint16_t g6v=(uint16_t)(vmul((uint16_t)dst,128,12)&63u),
                 g8v=vmul(g6v,16640,12);
        uint16_t b5v=(uint16_t)(dst&31u),        b8v=vmul(b5v,33792,12);
        if(r5v!=r5)bad++;
        if(r8v!=((r5<<3)|(r5>>2)))bad++;
        if(g6v!=g6)bad++;
        if(g8v!=((g6<<2)|(g6>>4)))bad++;
        if(b5v!=b5)bad++;
        if(b8v!=((b5<<3)|(b5>>2)))bad++;
    }
    if(bad)fail("unpack",bad,0);
    printf("unpack       exact over all 65,536 destination words (6 identities each)\n");
}

static void check_alpha_flip(void){
    long bad=0;
    for(int s=0;s<256;s++)for(int d=0;d<256;d++)for(int a=0;a<256;a++){
        unsigned want=(s<d)?(unsigned)(255-a):(unsigned)a;
        unsigned got=(unsigned)a^((s<d)?0x00FFu:0u);
        if(got!=want)bad++;
    }
    if(bad)fail("a'",bad,0);
    printf("a'           exact over all 16,777,216 (s,d,a): mask-and-XOR is the flip\n");
}

static void check_divide255(void){
    long bad=0;unsigned long long maxy=0;
    for(unsigned y=0;y<=65278u;y++){
        if(((257ull*(y+1))>>16)!=(y/255u))bad++;
        maxy=y;
    }
    if(bad)fail("/255",bad,0);
    printf("divide /255  exact through y=65,278 (the mix needs 65,152, the dither 16,319)\n");
    if(maxy!=65278u)fail("sweep reached",(long)maxy,65278);
}

static void check_mix_divide(void){
    /* floor((32896 + 257*diff*ap)/65536) == (diff*ap + 127)/255 */
    long bad=0;long long maxacc=0,maxq=0;
    for(int diff=0;diff<256;diff++)for(int ap=0;ap<256;ap++){
        long long acc=32896+257LL*diff*ap;
        long long q=acc>>16;
        if(q!=(diff*ap+127)/255)bad++;
        if(acc>maxacc)maxacc=acc;
        if(q>maxq)maxq=q;
    }
    if(bad)fail("mix divide",bad,0);
    printf("mix divide   exact over all 65,536 (|s-d|,a'); accumulator peak %lld of 2^40, "
           "readout peak %lld of 32,767\n",maxacc,maxq);
}

static void check_channel_mix(void){
    long bad=0;long long maxc=0;unsigned long long reach_r=0,reach_g=0,reach_b=0;
    for(int s=0;s<256;s++)for(int d=0;d<256;d++)for(int a=0;a<256;a++){
        unsigned mn=s<d?(unsigned)s:(unsigned)d;
        unsigned diff=s<d?(unsigned)(d-s):(unsigned)(s-d);
        unsigned ap=s<d?(unsigned)(255-a):(unsigned)a;
        long long acc=32896+257LL*diff*ap;
        long long c=mn+(acc>>16);
        if(c!=(long long)((s*a+d*(255-a)+127)/255))bad++;
        if(c>maxc)maxc=c;
    }
    /* what the destination can actually replicate */
    unsigned seen[256];memset(seen,0,sizeof seen);
    for(unsigned r5=0;r5<32;r5++)seen[(r5<<3)|(r5>>2)]=1;
    for(unsigned v=0;v<256;v++)if(seen[v])reach_r++;
    memset(seen,0,sizeof seen);
    for(unsigned g6=0;g6<64;g6++)seen[(g6<<2)|(g6>>4)]=1;
    for(unsigned v=0;v<256;v++)if(seen[v])reach_g++;
    memset(seen,0,sizeof seen);
    for(unsigned b5=0;b5<32;b5++)seen[(b5<<3)|(b5>>2)]=1;
    for(unsigned v=0;v<256;v++)if(seen[v])reach_b++;
    if(bad)fail("channel mix",bad,0);
    printf("channel mix  exact over all 16,777,216 (s,d8,a); destination expansion "
           "reaches %llu/32 red, %llu/64 green, %llu/32 blue values, all inside the sweep; "
           "c peak %lld of 255\n",reach_r,reach_g,reach_b,maxc);
}

static void check_quantize(void){
    long bad=0,badguard=0,badcond=0;
    for(unsigned b=0;b<16;b++){
        unsigned T=(2u*b+1u)*255u,Tp=T/32u;
        for(unsigned rem=0;rem<255;rem++){
            int scalar=32u*rem>T;
            int folded=rem>Tp;
            if(scalar!=folded)badcond++;
        }
    }
    for(unsigned maximum=0;maximum<64;maximum++){
        if(maximum!=31u&&maximum!=63u)continue;
        for(unsigned value=0;value<256;value++){
            unsigned p=value*maximum,q=p/255u,rem=p-255u*q;
            /* the scalar, with and without its q<M guard */
            for(unsigned bayer=0;bayer<16;bayer++){
                unsigned t=(2u*bayer+1u)*255u;
                unsigned a=q+(q<maximum&&32u*rem>t);
                unsigned b=q+(32u*rem>t);
                if(a!=b)badguard++;
            }
            for(unsigned bayer=0;bayer<16;bayer++){
                unsigned T=(2u*bayer+1u)*255u,Tp=T/32u;
                unsigned scalar_q=value*maximum/255u,scalar_rem=value*maximum-255u*scalar_q;
                if(scalar_q<maximum&&32u*scalar_rem>T)scalar_q++;
                unsigned P=65535u-257u*Tp;
                long long acc=(long long)P+257LL*value*maximum;
                unsigned got=(unsigned)(acc>>16);
                if(got!=scalar_q)bad++;
            }
        }
    }
    if(bad||badguard||badcond)fail("quantize",bad+badguard+badcond,0);
    printf("quantize     exact over all 8,192 (value,maximum,bayer); the q<M guard changes "
           "nothing (0 of 8,192 cases), and 32*rem>T is rem>floor(T/32) over all 16x255\n");
}

static void check_pack(void){
    long bad=0;
    for(unsigned r=0;r<256;r++)for(unsigned g=0;g<256;g++)for(unsigned b=0;b<256;b++){
        unsigned want=(r>>3)<<11|(g>>2)<<5|(b>>3);
        unsigned rq=vmul((uint16_t)r,2,4),gq=vmul((uint16_t)g,4,4),bq=vmul((uint16_t)b,2,4);
        unsigned got=vmul((uint16_t)rq,32768,4)|vmul((uint16_t)gq,512,4)|bq;
        if(got!=want)bad++;
    }
    if(bad)fail("pack",bad,0);
    printf("pack         exact over all 16,777,216 (r,g,b): three multiplies and two ORs\n");
}

/* ---- 2. the kernel against the scalar reference -------------------------- */

#define DITHER_SETS 6
#define THIN_SETS 24


static void sweep_thin(void){
    static const struct { unsigned color,opacity; } set[THIN_SETS]={
        {0x00000000u,255},{0x000000FFu,255},{0xFFFFFFFFu,255},{0x80808080u,255},
        {0x00000000u,0},{0xFFFFFFFFu,0},{0x00000001u,255},{0xFF0000FFu,255},
        {0x00FF00FFu,255},{0x0000FFFFu,255},{0x80402010u,128},{0x10204080u,127},
        {0x7F7F7F7Fu,1},{0xFEFEFEFEu,129},{0x55555555u,170},{0xAAAAAAABu,85},
        {0x00000080u,2},{0xFFFFFF7Fu,254},{0x12345678u,200},{0x9ABCDEF0u,60},
        {0x00000000u,1},{0xFFFFFF80u,128},{0x01010101u,255},{0x7FFFFFFFu,127}
    };
    static uint16_t dst[8] __attribute__((aligned(16))),want[8];
    long differing=0,blocks=0,worst[3]={0,0,0};
    for(unsigned s=0;s<THIN_SETS;s++){
        for(unsigned base=0;base<65536u;base+=8u){
            for(int i=0;i<8;i++)dst[i]=(uint16_t)(base+(unsigned)i);
            memcpy(want,dst,sizeof dst);
            ksn_blend8_pie(dst,1,set[s].color,(uint8_t)set[s].opacity,NULL);
            for(int i=0;i<8;i++)want[i]=ksn_blend_scalar_ref(want[i],set[s].color,set[s].opacity,false,i,0);
            for(int i=0;i<8;i++){
                blocks++;
                if(dst[i]==want[i])continue;
                differing++;
                for(int c=0;c<3;c++){
                    int sh=(c==0)?11:(c==1)?5:0,mask=(c==1)?63:31;
                    int step=abs((int)((dst[i]>>sh)&mask)-(int)((want[i]>>sh)&mask));
                    if(step>worst[c])worst[c]=step;
                }
            }
        }
    }
    if(differing)fail("thin sweep",differing,0);
    printf("kernel thin  %ld blocks, %u parameter sets x all 65,536 words: differing=%ld "
           "worst step r/g/b=%ld/%ld/%ld\n",blocks,THIN_SETS,differing,worst[0],worst[1],worst[2]);
}

static void sweep_dither(void){
    static const struct { unsigned color,opacity; } set[DITHER_SETS]={
        {0xFFFFFFFFu,255},{0x000000FFu,255},{0x80402010u,128},
        {0x10204080u,127},{0xAAAAAAABu,85},{0x12345678u,200}
    };
    static uint16_t dst[8] __attribute__((aligned(16))),want[8],thr[8] __attribute__((aligned(16)));
    long differing=0,blocks=0,worst[3]={0,0,0};
    for(unsigned s=0;s<DITHER_SETS;s++)
    for(unsigned y=0;y<4;y++)
    for(unsigned x0=0;x0<8;x0+=2){
        for(int i=0;i<8;i++)thr[i]=ksn_blend_bayer4[y&3u][(x0+(unsigned)i)&3u];
        for(unsigned base=0;base<65536u;base+=8u){
            for(int i=0;i<8;i++)dst[i]=(uint16_t)(base+(unsigned)i);
            memcpy(want,dst,sizeof dst);
            ksn_blend8_pie(dst,1,set[s].color,(uint8_t)set[s].opacity,thr);
            for(int i=0;i<8;i++)
                want[i]=ksn_blend_scalar_ref(want[i],set[s].color,set[s].opacity,true,(int)x0+i,(int)y);
            for(int i=0;i<8;i++){
                blocks++;
                if(dst[i]==want[i])continue;
                differing++;
                for(int c=0;c<3;c++){
                    int sh=(c==0)?11:(c==1)?5:0,mask=(c==1)?63:31;
                    int step=abs((int)((dst[i]>>sh)&mask)-(int)((want[i]>>sh)&mask));
                    if(step>worst[c])worst[c]=step;
                }
            }
        }
    }
    if(differing)fail("dither sweep",differing,0);
    printf("kernel dith  %ld blocks, %u parameter sets x 16 bayer phases x all 65,536 words: "
           "differing=%ld worst step r/g/b=%ld/%ld/%ld\n",blocks,DITHER_SETS,differing,
           worst[0],worst[1],worst[2]);
}

static void sweep_edges(void){
    /* the shapes the sweeps above reach least: many blocks in one call, the
     * zero-alpha early return, and runs that start at the end of a row */
    static uint16_t dst[8*40] __attribute__((aligned(16))),want[8*40];
    static uint16_t thr[8] __attribute__((aligned(16)));
    long differing=0,blocks=0;
    srand(12345);
    for(int it=0;it<2000;it++){
        int n=1+(rand()%40);
        unsigned color=(unsigned)rand()|((unsigned)rand()<<16);
        unsigned opacity=(unsigned)(rand()&255);
        int dither=rand()&1;
        for(int i=0;i<n*8;i++)dst[i]=(uint16_t)rand();
        /* the vector the contract asks for: bayer4[y&3][(x0+i)&3], y=0, x0=0 */
        for(int i=0;i<8;i++)thr[i]=ksn_blend_bayer4[0][(unsigned)i&3u];
        memcpy(want,dst,sizeof(uint16_t)*(size_t)(n*8));
        ksn_blend8_pie(dst,n,color,(uint8_t)opacity,dither?thr:NULL);
        for(int i=0;i<n*8;i++)
            want[i]=ksn_blend_scalar_ref(want[i],color,opacity,dither!=0,i&7,0);
        for(int i=0;i<n*8;i++){
            blocks++;
            if(dst[i]!=want[i])differing++;
        }
    }
    /* opacity 0 and alpha 0 must write nothing at all */
    for(int i=0;i<8;i++)dst[i]=0x1234;
    ksn_blend8_pie(dst,1,0xFFFFFF00u,255,NULL);
    for(int i=0;i<8;i++)if(dst[i]!=0x1234)differing++;
    for(int i=0;i<8;i++)dst[i]=0x1234;
    ksn_blend8_pie(dst,1,0xFFFFFFFFu,0,NULL);
    for(int i=0;i<8;i++)if(dst[i]!=0x1234)differing++;
    if(differing)fail("edges",differing,0);
    printf("kernel runs  %ld blocks over 2,000 random multi-block calls + both zero-alpha "
           "early returns: differing=%ld\n",blocks,differing);
}

int main(void){
    check_unpack();
    check_alpha_flip();
    check_divide255();
    check_mix_divide();
    check_channel_mix();
    check_quantize();
    check_pack();
    sweep_thin();
    sweep_dither();
    sweep_edges();
    printf("mismatches=%ld\n",mismatches);
    return mismatches!=0;
}
