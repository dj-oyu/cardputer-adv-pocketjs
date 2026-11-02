/* The PIE prototype's vector and batch instructions (docs/kasane/shiki-pie.md
 * sec.3.1, sec.3.2; not in the spec yet), host only:
 *   1. every bench pair (main/ui/kasane/ksn_shiki_bench.c) draws the same
 *      segments in both forms, and affine128 equals a direct computation;
 *   2. lane semantics: the masked tail, sat16 lanes, sat40 accumulator,
 *      VOUT's floor and write-back, VIOTA, VMIN/VMAX;
 *   3. step counts (one per vector instruction per block);
 *   4. EMIT rounding, counts and INVALID;
 *   5. what registration refuses;
 *   6. the start record (v2.0 draft, docs/kasane/shiki-v2.md sec.4.1) of
 *      VEACH (aux bit 1, r[imm]) and EMIT_* (dst bit 1, r[b]; the count
 *      then in r[b + 1]): lanes, VIOTA's numbers, the edges and INVALID.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Imain/ui/kasane main/ui/kasane/ksn_shiki.c \
 *      main/ui/kasane/ksn_shiki_bench.c tools/kasane_contract/test_shiki_vec.c -lm
 */
#include "ksn_shiki.h"
#include "ksn_shiki_bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned fails,checks;
#define CHECK(c,...) do{checks++;if(!(c)){fails++;printf("FAIL %s:%d ",__FILE__,__LINE__);printf(__VA_ARGS__);printf("\n");}}while(0)

/* A tiny assembler: regions {type, q, k, write}, then instructions. */
typedef struct { uint8_t b[12+16+12*64]; unsigned nd,n; } prog;
static void p_begin(prog *p,unsigned nd,const uint8_t (*decl)[4]){
    memset(p,0,sizeof *p);
    const uint8_t h[12]={'S','K','I','1',1,0,0,0,0,(uint8_t)nd,0,0};
    memcpy(p->b,h,12);
    for(unsigned k=0;k<nd;k++)memcpy(p->b+12+4*k,decl[k],4);
    p->nd=nd;
}
static void I(prog *p,uint8_t o,uint8_t d,uint8_t a,uint8_t b,int32_t imm,uint16_t aux){
    uint8_t *q=p->b+12+4*p->nd+12*p->n++;
    const uint32_t u=(uint32_t)imm;
    const uint8_t w[12]={o,d,a,b,(uint8_t)u,(uint8_t)(u>>8),(uint8_t)(u>>16),(uint8_t)(u>>24),
                         (uint8_t)aux,(uint8_t)(aux>>8),0,0};
    memcpy(q,w,12);
    p->b[6]=(uint8_t)p->n;
}
static size_t p_len(const prog *p){return 12+4*p->nd+12*(size_t)p->n;}

static ksn_shiki_plan *plan;
static ksn_shiki_vm vm;
static ksn_proc_segment seg[KSN_SHIKI_SEGMENTS];
static bool reg(const prog *p,ksn_shiki_region *r,unsigned nr){
    return ksn_shiki_register(p->b,p_len(p),NULL,0,r,nr,plan);
}
static ksn_shiki_status run(void){return ksn_shiki_draw(plan,NULL,0,&vm,seg);}
static const int16_t *V(unsigned k){return (const int16_t *)(((uintptr_t)vm.vraw+15u)&~(uintptr_t)15u)+8*k;}

static void pairs(void){
    static int16_t work[KSN_SHIKI_BENCH_WORK];
    static ksn_proc_segment first[2][KSN_SHIKI_SEGMENTS];
    unsigned count[2]={0},seen[2]={0};
    for(unsigned i=0;i<ksn_shiki_bench_count();i++){
        ksn_shiki_bench_case c;
        CHECK(ksn_shiki_bench_build(i,work,&c),"build %u",i);
        const bool ok=ksn_shiki_register(c.code,c.len,NULL,0,c.region,c.regions,plan);
        CHECK(ok,"%s registers",c.name);
        if(!ok)continue;
        const ksn_shiki_status s=run();
        CHECK(s==KSN_SHIKI_DONE,"%s DONE (got %d)",c.name,s);
        printf("  %-22s %2u instr, %5u steps, %4u segments\n",c.name,(unsigned)plan->count,vm.steps,vm.count);
        if(!seen[c.pair]++){count[c.pair]=vm.count;memcpy(first[c.pair],seg,vm.count*sizeof *seg);}
        else CHECK(count[c.pair]==vm.count&&!memcmp(first[c.pair],seg,vm.count*sizeof *seg),
                   "%s draws what the first of its pair draws (%u vs %u)",c.name,vm.count,count[c.pair]);
        if(i==1){
            /* Direct: x' = floor((cos x - sin y + 120 * 2^14) / 2^14). */
            const int16_t *pts=work;                       /* planes: x then y */
            bool same=vm.count==128;
            for(unsigned k=0;same&&k<128;k++){
                const int32_t x=pts[k],y=pts[128+k];
                const int32_t X=(int32_t)((14189*x-8192*y+(120<<14))>>14);
                const int32_t Y=(int32_t)((8192*x+14189*y+(67<<14))>>14);
                same=seg[k].x0==X&&seg[k].y0==Y&&seg[k].x1==X&&seg[k].y1==Y&&seg[k].color==2016;
            }
            CHECK(same,"affine128 equals the direct computation");
        }
    }
}

/* ksn_shiki_put into a NUL-terminated buffer (ctx, 256 B). */
static void put_line(void *ctx,const char *text,size_t len){
    char *b=ctx;
    const size_t n=strlen(b);
    if(n+len<256){memcpy(b+n,text,len);b[n+len]=0;}
}

/* PUT and FETCH on planes (v1.4 sec.5.4.6): field c of record j at c * S + j,
 * S = records rounded up to 8. 6 records: planes of 8, padding 6..7 and
 * 14..15 never touched. */
static void planes(void){
    int16_t a[16];
    for(int k=0;k<16;k++)a[k]=(int16_t)(100+k);
    const uint8_t decl[1][4]={{KSN_SHIKI_I16,0,2,1}};
    ksn_shiki_region r[1]={{a,6,KSN_SHIKI_I16,0,2}};
    prog p;p_begin(&p,1,decl);
    I(&p,KSN_SHIKI_EACH,0,0,0,0,0);
    I(&p,KSN_SHIKI_FETCH,0,0,0,0,2);                 /* r0 = x, r1 = y */
    I(&p,KSN_SHIKI_ADD,0,0,1,0,0);
    I(&p,KSN_SHIKI_PUT,0,0,1,0,1);                   /* y = x + y */
    I(&p,KSN_SHIKI_END,0,0,0,0,0);
    CHECK(reg(&p,r,1)&&run()==KSN_SHIKI_DONE,"planes: put runs");
    bool ok=true;
    for(int j=0;j<6;j++)ok=ok&&a[j]==100+j&&a[8+j]==(100+j)+(108+j);
    for(int j=6;j<8;j++)ok=ok&&a[j]==100+j&&a[8+j]==108+j;
    CHECK(ok,"planes: put writes the y plane, padding untouched (%d %d %d)",a[8],a[13],a[14]);
    /* LOOKUP clamps to record R - 1, not to the padding. */
    int16_t t[8]={11,22,33,44,55,-1,-1,-1};
    const uint8_t d1[1][4]={{KSN_SHIKI_I16,0,1,0}};
    ksn_shiki_region rt[1]={{t,5,KSN_SHIKI_I16,0,1}};
    p_begin(&p,1,d1);
    I(&p,KSN_SHIKI_SET,0,0,0,7,0);
    I(&p,KSN_SHIKI_LOOKUP,1,0,0,0,0);
    I(&p,KSN_SHIKI_SET,0,0,0,-3,0);
    I(&p,KSN_SHIKI_LOOKUP,2,0,0,0,0);
    CHECK(reg(&p,rt,1)&&run()==KSN_SHIKI_DONE&&vm.reg[1]==55&&vm.reg[2]==11,
          "planes: LOOKUP clamps to 0..R-1 (%d %d)",vm.reg[1],vm.reg[2]);
    /* The region table's flag bits other than write are 0 (v1.4 sec.3). */
    const uint8_t bad[1][4]={{KSN_SHIKI_I16,0,2,2}};
    p_begin(&p,1,bad);
    I(&p,KSN_SHIKI_EACH,0,0,0,0,0);I(&p,KSN_SHIKI_END,0,0,0,0,0);
    CHECK(!reg(&p,r,1),"planes: refuses flag bit 1");
    /* The result line lists k_rec * R values field by field, no padding. */
    char line[256]="";
    ksn_shiki_region rr[1]={{a,6,KSN_SHIKI_I16,0,2}};
    ksn_shiki_result_regions(rr,1,put_line,line);
    CHECK(!strcmp(line," D 1 6 100 101 102 103 104 105 208 210 212 214 216 218"),"planes: result line (%s)",line);
}

static void lanes(void){
    /* 13 records of one field: lanes 13..15 of block 2 read 0 and are not written. */
    int16_t a[16],o[16];
    for(int k=0;k<16;k++){a[k]=(int16_t)(k*1000-3000);o[k]=-7;}
    const uint8_t decl[2][4]={{KSN_SHIKI_I16,0,1,0},{KSN_SHIKI_I16,0,1,1}};
    ksn_shiki_region r[2]={{a,16,KSN_SHIKI_I16,0,1},{o,16,KSN_SHIKI_I16,0,1}};
    prog p;p_begin(&p,2,decl);
    I(&p,KSN_SHIKI_SET,0,0,0,13,0);
    I(&p,KSN_SHIKI_SET,1,0,0,30000,0);
    I(&p,KSN_SHIKI_VEACH,0,0,0,0,1);                 /* N = r0 = 13 */
    I(&p,KSN_SHIKI_VLOAD,0,0,0,0,0);
    I(&p,KSN_SHIKI_VSPLAT,1,1,0,0,0);
    I(&p,KSN_SHIKI_VADD,2,0,1,0,0);                  /* sat16 */
    I(&p,KSN_SHIKI_VSTORE,2,1,0,0,0);
    I(&p,KSN_SHIKI_END,0,0,0,0,0);
    CHECK(reg(&p,r,2),"tail registers");
    CHECK(run()==KSN_SHIKI_DONE,"tail runs");
    bool ok=true;
    for(int k=0;k<16;k++){
        const int32_t want=k<13?(a[k]+30000>32767?32767:a[k]+30000):-7;
        if(o[k]!=want){ok=false;printf("  o[%d]=%d want %d\n",k,o[k],want);}
    }
    CHECK(ok,"masked tail and sat16");
    /* 2 SETs, VEACH, 2 blocks x (4 + END). */
    CHECK(vm.steps==2+1+2*5,"steps %u",vm.steps);
    const int16_t *v0=V(0);
    CHECK(v0[4]==12*1000-3000&&v0[5]==0&&v0[7]==0,"last block's dead lanes load 0 (%d %d)",v0[5],v0[7]);

    /* sat40: 16 blocks x 40 MACs of 32767^2 = 6.9e11 > 2^39 (the accumulator
     * carries across blocks and loops: nothing resets it but VZACC/VACCSET). */
    static int16_t h[128];
    ksn_shiki_region rh[2]={{h,128,KSN_SHIKI_I16,0,1},{h,128,KSN_SHIKI_I16,0,1}};
    for(int k=0;k<128;k++)h[k]=32767;
    p_begin(&p,2,decl);
    I(&p,KSN_SHIKI_SET,0,0,0,32767,0);
    I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);
    I(&p,KSN_SHIKI_VLOAD,0,0,0,0,0);
    for(int k=0;k<40;k++)I(&p,KSN_SHIKI_VMACK,0,0,0,0,0);
    I(&p,KSN_SHIKI_END,0,0,0,0,0);
    I(&p,KSN_SHIKI_SET,1,0,0,8,0);
    I(&p,KSN_SHIKI_VEACH,0,0,1,0,1);                 /* one block */
    I(&p,KSN_SHIKI_VOUT,2,0,0,0,24);                 /* (2^39-1) >> 24 = 32767 */
    I(&p,KSN_SHIKI_END,0,0,0,0,0);
    CHECK(reg(&p,rh,2),"sat40 registers");
    CHECK(run()==KSN_SHIKI_DONE,"sat40 runs");
    CHECK(V(2)[0]==32767&&V(2)[7]==32767&&ksn_shiki_vm_acc(&vm)[5]==32767,"sat40, then VOUT writes back (%d %lld)",
          V(2)[0],(long long)ksn_shiki_vm_acc(&vm)[5]);
    CHECK(vm.steps==1+1+16*42+1+1+2,"steps %u",vm.steps);
    for(int k=0;k<128;k++)h[k]=-32768;
    p_begin(&p,2,decl);
    I(&p,KSN_SHIKI_SET,0,0,0,32767,0);
    I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);
    I(&p,KSN_SHIKI_VLOAD,0,0,0,0,0);
    I(&p,KSN_SHIKI_VSPLAT,1,0,0,0,0);
    for(int k=0;k<40;k++)I(&p,KSN_SHIKI_VMAC,0,0,1,0,0);
    I(&p,KSN_SHIKI_END,0,0,0,0,0);
    CHECK(reg(&p,rh,2),"vmac registers");
    CHECK(run()==KSN_SHIKI_DONE&&ksn_shiki_vm_acc(&vm)[3]==-((int64_t)1<<39),"sat40 below (%lld)",(long long)ksn_shiki_vm_acc(&vm)[3]);
    /* VACCSET, VOUT's floor, VIOTA, VMIN/VMAX over 16 records. */
    p_begin(&p,2,decl);
    I(&p,KSN_SHIKI_SET,1,0,0,-5,0);
    I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);
    I(&p,KSN_SHIKI_VACCSET,0,1,0,0,0);
    I(&p,KSN_SHIKI_VOUT,3,0,0,0,1);                  /* floor(-5/2) = -3 */
    I(&p,KSN_SHIKI_VIOTA,4,0,0,0,0);
    I(&p,KSN_SHIKI_VMIN,5,4,3,0,0);
    I(&p,KSN_SHIKI_VMAX,6,4,3,0,0);
    I(&p,KSN_SHIKI_END,0,0,0,0,0);
    CHECK(reg(&p,r,2),"acc registers");
    CHECK(run()==KSN_SHIKI_DONE,"acc runs");
    CHECK(ksn_shiki_vm_acc(&vm)[0]==-3&&ksn_shiki_vm_acc(&vm)[7]==-3,"VOUT writes the shifted value back (%lld)",(long long)ksn_shiki_vm_acc(&vm)[0]);
    CHECK(V(3)[3]==-3,"VOUT floors (%d)",V(3)[3]);
    CHECK(V(4)[0]==8&&V(4)[7]==15,"VIOTA numbers the second block's records (%d %d)",V(4)[0],V(4)[7]);
    CHECK(V(5)[0]==-3&&V(6)[0]==8,"VMIN/VMAX");
    CHECK(vm.steps==1+1+2*6,"steps %u",vm.steps);
    /* N out of range: r0 > records, and N above int16. */
    p_begin(&p,2,decl);
    I(&p,KSN_SHIKI_SET,0,0,0,17,0);
    I(&p,KSN_SHIKI_VEACH,0,0,0,0,1);
    I(&p,KSN_SHIKI_VIOTA,0,0,0,0,0);
    I(&p,KSN_SHIKI_END,0,0,0,0,0);
    CHECK(reg(&p,r,2)&&run()==KSN_SHIKI_INVALID,"N above the records is INVALID");
    static int16_t big[40000];
    ksn_shiki_region rb[2]={{big,40000,KSN_SHIKI_I16,0,1},{big,40000,KSN_SHIKI_I16,0,1}};
    p_begin(&p,2,decl);
    I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);
    I(&p,KSN_SHIKI_VIOTA,0,0,0,0,0);
    I(&p,KSN_SHIKI_END,0,0,0,0,0);
    CHECK(reg(&p,rb,2)&&run()==KSN_SHIKI_INVALID,"N above 32767 is INVALID");
}

static void emits(void){
    /* Q4 points: halves up, -480..720. */
    /* 3 records as planes of 8: x0 x1 x2 . . . . . | y0 y1 y2 . . . . . */
    int16_t pt[16]={(int16_t)(10*16+8),(int16_t)(5*16+7),(int16_t)(720*16+8),0,0,0,0,0,
                    (int16_t)(-3*16-8),0,0,0,0,0,0,0};
    const uint8_t decl[1][4]={{KSN_SHIKI_I16,4,2,0}};
    ksn_shiki_region r[1]={{pt,3,KSN_SHIKI_I16,4,2}};
    prog p;p_begin(&p,1,decl);
    I(&p,KSN_SHIKI_SET,0,0,0,2,0);
    I(&p,KSN_SHIKI_EMIT_POINTS,1,0,0,4,31);          /* count = r0 = 2 */
    CHECK(reg(&p,r,1),"emit registers");
    CHECK(run()==KSN_SHIKI_DONE&&vm.count==2,"emit count from r[b]");
    CHECK(seg[0].x0==11&&seg[0].y0==-3&&seg[1].x0==5&&seg[1].y0==0&&seg[0].color==31,
          "halves up (%d %d %d)",seg[0].x0,seg[0].y0,seg[1].x0);
    CHECK(!vm.pen_valid,"the pen is untouched");
    p_begin(&p,1,decl);
    I(&p,KSN_SHIKI_EMIT_POINTS,0,0,0,4,31);          /* all 3: the third is 721 */
    CHECK(reg(&p,r,1)&&run()==KSN_SHIKI_INVALID&&vm.count==2,"off the range is INVALID after the earlier ones");
    p_begin(&p,1,decl);
    I(&p,KSN_SHIKI_SET,0,0,0,4,0);
    I(&p,KSN_SHIKI_EMIT_POINTS,1,0,0,4,31);
    CHECK(reg(&p,r,1)&&run()==KSN_SHIKI_INVALID,"count above the records is INVALID");
}

static void starts(void){
    /* VEACH from record 3 for 6 records into a second region: lanes 3..8,
     * VIOTA gives the record numbers; nothing else is written. */
    static int16_t src[16] __attribute__((aligned(16))),dst[16] __attribute__((aligned(16)));
    for(int k=0;k<16;k++){src[k]=(int16_t)(10*k);dst[k]=-1;}
    const uint8_t decl[2][4]={{KSN_SHIKI_I16,0,1,0},{KSN_SHIKI_I16,0,1,1}};
    ksn_shiki_region r[2]={{src,12,KSN_SHIKI_I16,0,1},{dst,12,KSN_SHIKI_I16,0,1}};
    prog p;p_begin(&p,2,decl);
    I(&p,KSN_SHIKI_SET,4,0,0,3,0);                   /* start */
    I(&p,KSN_SHIKI_SET,5,0,0,6,0);                   /* count */
    I(&p,KSN_SHIKI_VEACH,0,0,5,4,3);
    I(&p,KSN_SHIKI_VLOAD,0,0,0,0,0);
    I(&p,KSN_SHIKI_VIOTA,1,0,0,0,0);
    I(&p,KSN_SHIKI_VADD,0,0,1,0,0);
    I(&p,KSN_SHIKI_VSTORE,0,1,0,0,0);
    I(&p,KSN_SHIKI_END,0,0,0,0,0);
    CHECK(reg(&p,r,2),"VEACH with a start registers");
    CHECK(run()==KSN_SHIKI_DONE,"VEACH with a start runs");
    bool ok=true;
    for(int k=0;k<12;k++)ok=ok&&dst[k]==(k>=3&&k<9?11*k:-1);
    CHECK(ok,"VEACH lanes 3..8 only (%d %d %d %d)",dst[2],dst[3],dst[8],dst[9]);
    /* 2 SETs, VEACH, one block x (4 + END): one block of 6 lanes. */
    CHECK(vm.steps==2+1+5,"VEACH with a start: one block (%u steps)",vm.steps);
    /* Without a count: start .. R. Start R: nothing. Start R + 1, -1, or a
     * count past R - start: INVALID. */
    for(int k=0;k<16;k++)dst[k]=-1;
    p_begin(&p,2,decl);
    I(&p,KSN_SHIKI_SET,4,0,0,10,0);
    I(&p,KSN_SHIKI_VEACH,0,0,0,4,2);
    I(&p,KSN_SHIKI_VLOAD,0,0,0,0,0);I(&p,KSN_SHIKI_VSTORE,0,1,0,0,0);
    I(&p,KSN_SHIKI_END,0,0,0,0,0);
    CHECK(reg(&p,r,2)&&run()==KSN_SHIKI_DONE&&dst[9]==-1&&dst[10]==100&&dst[11]==110&&dst[12]==-1,
          "VEACH from 10 to R");
    const int32_t edge[][3]={{12,0,KSN_SHIKI_DONE},{13,0,KSN_SHIKI_INVALID},{-1,0,KSN_SHIKI_INVALID},
                             {5,8,KSN_SHIKI_INVALID},{5,7,KSN_SHIKI_DONE}};
    for(unsigned e=0;e<sizeof edge/sizeof edge[0];e++){
        p_begin(&p,2,decl);
        I(&p,KSN_SHIKI_SET,4,0,0,edge[e][0],0);
        I(&p,KSN_SHIKI_SET,5,0,0,edge[e][1],0);
        I(&p,KSN_SHIKI_VEACH,0,0,edge[e][1]?5:0,4,edge[e][1]?3:2);
        I(&p,KSN_SHIKI_VIOTA,0,0,0,0,0);
        I(&p,KSN_SHIKI_END,0,0,0,0,0);
        CHECK(reg(&p,r,2)&&run()==(ksn_shiki_status)edge[e][2],"VEACH start %d count %d",edge[e][0],edge[e][1]);
    }
    /* EMIT_POINTS from record 1 (dst 2: start r[b]), and from 1 for 1
     * (dst 3: count r[b + 1]). */
    int16_t pt[16]={(int16_t)(10*16),(int16_t)(20*16),(int16_t)(30*16),0,0,0,0,0,
                    (int16_t)(1*16),(int16_t)(2*16),(int16_t)(3*16),0,0,0,0,0};
    const uint8_t dp[1][4]={{KSN_SHIKI_I16,4,2,0}};
    ksn_shiki_region rp[1]={{pt,3,KSN_SHIKI_I16,4,2}};
    p_begin(&p,1,dp);
    I(&p,KSN_SHIKI_SET,6,0,0,1,0);
    I(&p,KSN_SHIKI_EMIT_POINTS,2,0,6,4,31);
    CHECK(reg(&p,rp,1)&&run()==KSN_SHIKI_DONE&&vm.count==2&&seg[0].x0==20&&seg[0].y0==2&&seg[1].x0==30,
          "EMIT from record 1 (%u: %d %d)",vm.count,seg[0].x0,seg[0].y0);
    p_begin(&p,1,dp);
    I(&p,KSN_SHIKI_SET,6,0,0,1,0);I(&p,KSN_SHIKI_SET,7,0,0,1,0);
    I(&p,KSN_SHIKI_EMIT_POINTS,3,0,6,4,31);
    CHECK(reg(&p,rp,1)&&run()==KSN_SHIKI_DONE&&vm.count==1&&seg[0].x0==20,"EMIT from 1 for 1");
    p_begin(&p,1,dp);
    I(&p,KSN_SHIKI_SET,6,0,0,2,0);I(&p,KSN_SHIKI_SET,7,0,0,2,0);
    I(&p,KSN_SHIKI_EMIT_POINTS,3,0,6,4,31);
    CHECK(reg(&p,rp,1)&&run()==KSN_SHIKI_INVALID&&vm.count==0,"EMIT count past R - start is INVALID");
    p_begin(&p,1,dp);
    I(&p,KSN_SHIKI_SET,6,0,0,4,0);
    I(&p,KSN_SHIKI_EMIT_POINTS,2,0,6,4,31);
    CHECK(reg(&p,rp,1)&&run()==KSN_SHIKI_INVALID,"EMIT start past R is INVALID");
    p_begin(&p,1,dp);
    I(&p,KSN_SHIKI_EMIT_POINTS,3,0,15,4,31);
    CHECK(!reg(&p,rp,1),"EMIT start and count need r[b + 1]");
    p_begin(&p,1,dp);
    I(&p,KSN_SHIKI_EMIT_POINTS,4,0,0,4,31);
    CHECK(!reg(&p,rp,1),"EMIT dst above 3 is refused");
}

static void refusals(void){
    static int16_t a[64];   /* 32 records of 2: two planes of 32 */
    const uint8_t decl[3][4]={{KSN_SHIKI_I16,0,2,0},{KSN_SHIKI_I32,0,2,1},{KSN_SHIKI_I16,0,2,0}};
    static int32_t w[64];
    ksn_shiki_region r[3]={{a,32,KSN_SHIKI_I16,0,2},{w,32,KSN_SHIKI_I32,0,2},{a,16,KSN_SHIKI_I16,0,2}};
    prog p;
#define REFUSE(what,...) do{p_begin(&p,3,decl);__VA_ARGS__;CHECK(!reg(&p,r,3),"refuses %s",what);}while(0)
    REFUSE("a vector op outside VEACH",I(&p,KSN_SHIKI_VIOTA,0,0,0,0,0));
    REFUSE("a scalar op in VEACH",I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);I(&p,KSN_SHIKI_SET,0,0,0,1,0);I(&p,KSN_SHIKI_END,0,0,0,0,0));
    REFUSE("VEACH in VEACH",I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);
           I(&p,KSN_SHIKI_END,0,0,0,0,0);I(&p,KSN_SHIKI_END,0,0,0,0,0));
    REFUSE("REPEAT in VEACH",I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);I(&p,KSN_SHIKI_REPEAT,0,0,0,2,0);
           I(&p,KSN_SHIKI_END,0,0,0,0,0);I(&p,KSN_SHIKI_END,0,0,0,0,0));
    REFUSE("EMIT in VEACH",I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);I(&p,KSN_SHIKI_EMIT_POINTS,0,0,0,0,1);
           I(&p,KSN_SHIKI_END,0,0,0,0,0));
    REFUSE("VLOAD of an i32 region",I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);I(&p,KSN_SHIKI_VLOAD,0,1,0,0,0);
           I(&p,KSN_SHIKI_END,0,0,0,0,0));
    REFUSE("VSTORE to a read-only region",I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);I(&p,KSN_SHIKI_VSTORE,0,0,0,0,0);
           I(&p,KSN_SHIKI_END,0,0,0,0,0));
    REFUSE("VLOAD of a shorter region",I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);I(&p,KSN_SHIKI_VLOAD,0,2,0,0,0);
           I(&p,KSN_SHIKI_END,0,0,0,0,0));
    REFUSE("a field past the record",I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);I(&p,KSN_SHIKI_VLOAD,0,0,2,0,0);
           I(&p,KSN_SHIKI_END,0,0,0,0,0));
    REFUSE("vector register 8",I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);I(&p,KSN_SHIKI_VIOTA,8,0,0,0,0);
           I(&p,KSN_SHIKI_END,0,0,0,0,0));
    REFUSE("VOUT shift 33",I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);I(&p,KSN_SHIKI_VOUT,0,0,0,0,33);
           I(&p,KSN_SHIKI_END,0,0,0,0,0));
    REFUSE("EMIT_LINES on a 2-element record",I(&p,KSN_SHIKI_EMIT_LINES,0,0,0,0,1));
    REFUSE("EMIT q 16",I(&p,KSN_SHIKI_EMIT_POINTS,0,0,0,16,1));
    REFUSE("EMIT b without the count flag",I(&p,KSN_SHIKI_EMIT_POINTS,0,0,3,0,1));
    REFUSE("FETCH in VEACH",I(&p,KSN_SHIKI_VEACH,0,0,0,0,0);I(&p,KSN_SHIKI_FETCH,0,0,0,0,1);
           I(&p,KSN_SHIKI_END,0,0,0,0,0));
    /* And a well-formed one passes the same decl (the harness is not refusing everything). */
    p_begin(&p,3,decl);
    I(&p,KSN_SHIKI_VEACH,0,2,0,0,0);I(&p,KSN_SHIKI_VLOAD,0,0,1,0,0);I(&p,KSN_SHIKI_END,0,0,0,0,0);
    CHECK(reg(&p,r,3),"accepts VLOAD of a longer region");
}

/* The device's PIE checks must at least run to DONE on the C model. */
static void checks_run(void){
    static int16_t work[KSN_SHIKI_BENCH_WORK] __attribute__((aligned(16)));
    for(unsigned i=0;i<ksn_shiki_bench_check_count();i++){
        ksn_shiki_bench_case c;
        CHECK(ksn_shiki_bench_check_build(i,work,&c),"check %u builds",i);
        const bool ok=ksn_shiki_register(c.code,c.len,NULL,0,c.region,c.regions,plan);
        CHECK(ok,"%s registers",c.name);
        if(ok)CHECK(run()==KSN_SHIKI_DONE,"%s runs",c.name);
    }
}

int main(void){
    plan=malloc(KSN_SHIKI_PLAN_MAX);
    pairs();
    planes();
    lanes();
    starts();
    emits();
    refusals();
    checks_run();
    free(plan);
    printf("%s (%u checks, %u failed)\n",fails?"SHIKI VEC FAIL":"SHIKI VEC PASS",checks,fails);
    return fails?1:0;
}
