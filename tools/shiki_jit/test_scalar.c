/* The scalar JIT (docs/kasane/shiki-jit.md sec.11) on the host: random
 * programs compiled by ksn_shiki_jit_scalar and run on a small emulator of
 * the Xtensa instructions the compiler emits, against the interpreter.
 *
 * The emulator decodes with the generated table (ksn_shiki_jit_enc.h, which
 * test_encoder.py checks against the assembler) and gives each instruction
 * the ISA's meaning; it is the half of this test that is written from the
 * manual, so a wrong reading there and the same one in the compiler would
 * agree. The device check (pocket_shiki.c's jit lines) runs the real CPU.
 *
 * What must match: the status, and for DONE the registers, the segments,
 * the pen and the step count; for any other status the registers and the
 * segments so far (an error inside a block leaves vm->steps counted to the
 * block's end: sec.11).
 *
 * The programs are ksn_shiki_bench_random()'s, which the device check
 * (pocket_shiki.c, "CB jit scalar") runs as well.
 *
 *   cc -std=gnu11 -O1 -Imain/ui/kasane main/ui/kasane/ksn_shiki.c main/ui/kasane/ksn_shiki_bench.c \
 *      main/ui/kasane/ksn_shiki_jit.c tools/shiki_jit/test_scalar.c -o t && ./t [TRIALS]
 */
#include "ksn_shiki_jit.h"
#include "ksn_shiki_jit_enc.h"
#include "ksn_shiki_bench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t st=7;
static uint32_t rnd(void){st=st*1664525u+1013904223u;return st>>8;}

/* ---- the emulator ---------------------------------------------------------- */
enum { A_CTX=0x10000000u, A_VM=0x20000000u, A_IN=0x30000000u, A_CODE=0x40000000u, A_LEAF=0x50000000u,
       A_DRAW=0x60000000u, A_SIN=0x70000000u, A_CURVE=0x58000000u,
       /* sec.18: the shared checker's entries (within call0's reach of the
        * code) and the code's own return address. */
       A_CHK_ADD=A_CODE+0x20000u, A_CHK_SUB=A_CODE+0x20010u, A_CHK_ADD_F=A_CODE+0x20020u,
       A_CHK_SUB_F=A_CODE+0x20030u, A_RA=0x48000000u };
typedef struct {
    uint32_t ctx[9];
    ksn_shiki_vm *vm;
    int32_t *in;
    uint8_t *code; size_t code_len;
    const ksn_jit_run *run;
    unsigned steps,checks;
} emu;
static uint8_t *addr(emu *e,uint32_t a,unsigned n){
    struct { uint32_t base; void *p; size_t len; } m[]={
        {A_CTX,e->ctx,sizeof e->ctx},{A_VM,e->vm,sizeof *e->vm},
        {A_IN,e->in,4u*KSN_SHIKI_INPUTS},{A_CODE,e->code,e->code_len},
        {A_SIN,(void *)ksn_shiki_sin_table,sizeof ksn_shiki_sin_table}};
    for(unsigned k=0;k<5;k++)
        if(a>=m[k].base&&a-m[k].base+n<=m[k].len){
            if((a-m[k].base)%n){fprintf(stderr,"EMU unaligned %08x\n",a);exit(2);}
            return (uint8_t *)m[k].p+(a-m[k].base);
        }
    fprintf(stderr,"EMU bad address %08x\n",a);exit(2);
}
static uint32_t ld(emu *e,uint32_t a,unsigned n){uint32_t v=0;memcpy(&v,addr(e,a,n),n);return v;}
static void st_(emu *e,uint32_t a,uint32_t v,unsigned n){
    if((a>=A_CODE&&a<A_LEAF)||a>=A_SIN){fprintf(stderr,"EMU store to code or table\n");exit(2);}
    memcpy(addr(e,a,n),&v,n);
}
static int32_t sext(uint32_t v,unsigned bits){return (int32_t)(v<<(32-bits))>>(32-bits);}

/* The form at p, and its operands as encoded (immediates in field units). */
static int decode(const uint8_t *p,size_t left,int o[3]){
    int found=-1;
    for(unsigned f=0;f<KSN_JIT_FORMS;f++){
        const ksn_jit_form *F=&KSN_JIT_FORM[f];
        if(F->len>left)continue;
        uint32_t w=0;
        for(unsigned k=0;k<F->len;k++)w|=(uint32_t)p[k]<<(8*k);
        uint32_t field=0;
        for(unsigned k=0;k<F->nruns;k++)field|=((1u<<F->runs[k].len)-1u)<<F->runs[k].at;
        const uint32_t all=F->len==4?0xFFFFFFFFu:((1u<<(8*F->len))-1u);
        if((w&all&~field)!=(F->base&all&~field))continue;
        if(found>=0){fprintf(stderr,"EMU ambiguous %d/%u\n",found,f);exit(2);}
        found=(int)f;
        for(unsigned j=0;j<3;j++)o[j]=0;
        unsigned width[3]={0,0,0};
        for(unsigned k=0;k<F->nruns;k++){
            const ksn_jit_field *t=&F->runs[k];
            const unsigned bits=((w^F->base)>>t->at)&((1u<<t->len)-1u);
            o[t->operand]|=(int)(bits<<t->src);
            if(t->src+t->len>width[t->operand])width[t->operand]=t->src+t->len;
        }
        for(unsigned j=0;j<3;j++)o[j]^=F->base_op[j]&((1<<width[j])-1);
    }
    return found;
}

/* Runs the code at entry; returns its a2. */
static uint32_t emulate(emu *e,size_t entry){
    uint32_t a[16]={0},sar=0;
    a[2]=A_CTX;
    a[0]=A_RA;
    size_t pc=entry;
    for(unsigned n=0;;n++){
        if(n>2000000u){fprintf(stderr,"EMU runaway\n");exit(2);}
        if(pc>=e->code_len){fprintf(stderr,"EMU pc out %zu\n",pc);exit(2);}
        const uint8_t *p=e->code+pc;
        e->steps++;
        if((p[0]&15u)==1u){                       /* l32r */
            const uint32_t imm=(uint32_t)p[1]|(uint32_t)p[2]<<8;
            const uint32_t at=(uint32_t)((pc+3u)&~(size_t)3u)+4u*imm-262144u;
            a[p[0]>>4]=ld(e,A_CODE+at,4);
            pc+=3;
            continue;
        }
        int o[3];
        const int f=decode(p,e->code_len-pc,o);
        if(f<0){fprintf(stderr,"EMU undecodable at %zu: %02x %02x %02x\n",pc,p[0],p[1],p[2]);exit(2);}
        size_t next=pc+KSN_JIT_FORM[f].len;
        const uint32_t x=a[o[1]&15],y=a[o[2]&15];
        switch(f){
        case KSN_JIT_ENTRY48: break;
        case KSN_JIT_NOP: case KSN_JIT_NOPN: break;
        case KSN_JIT_RETWN:
            if(a[0]!=A_RA){fprintf(stderr,"EMU a0 lost: %08x\n",a[0]);exit(2);}
            return a[2];
        case KSN_JIT_L32I: a[o[0]]=ld(e,x+4u*(unsigned)o[2],4); break;
        case KSN_JIT_ADD: a[o[0]]=x+y; break;
        case KSN_JIT_CALL0: {
            /* The checker (ksn_shiki_jit.c's ksn_jit_chk_add/_sub) from its
             * comment: saturate a10, OR |a10| into the slot after the call
             * (clearing the never-run bit), reload a0, return past the slot. */
            const uint32_t at=A_CODE+(uint32_t)pc,to=(at&~3u)+4u+4u*(uint32_t)sext((uint32_t)o[0],18);
            if(to!=A_CHK_ADD&&to!=A_CHK_SUB&&to!=A_CHK_ADD_F&&to!=A_CHK_SUB_F){fprintf(stderr,"EMU call0 to %08x\n",to);exit(2);}
            const uint32_t ret=at+3u,va=a[8],vb=a[9],d=a[10];
            const bool add=to==A_CHK_ADD||to==A_CHK_ADD_F;
            const uint32_t ov=add?((va^d)&(vb^d)):((va^vb)&(va^d));
            uint32_t v=(int32_t)ov<0?(uint32_t)((int32_t)va>>31)^a[6]:d;
            a[10]=v;
            uint8_t *q=addr(e,ret+e->ctx[8],4);       /* the slot: in the code, word-aligned */
            if(to==A_CHK_ADD||to==A_CHK_SUB){         /* the fast entries do not log */
                uint32_t w;memcpy(&w,q,4);
                w=(w&a[6])|(v^(uint32_t)((int32_t)v>>31));
                memcpy(q,&w,4);
            }
            e->checks++;
            for(unsigned k=11;k<15;k++)a[k]=0xDEADBEEFu;
            a[0]=e->ctx[7];
            next=(size_t)(ret+4u-A_CODE);
            break;
        }
        case KSN_JIT_L32IN: a[o[0]]=ld(e,x+4u*(unsigned)o[2],4); break;
        case KSN_JIT_S32IN: st_(e,x+4u*(unsigned)o[2],a[o[0]],4); break;
        case KSN_JIT_S32I: st_(e,x+4u*(unsigned)o[2],a[o[0]],4); break;
        case KSN_JIT_L16UI: a[o[0]]=ld(e,x+2u*(unsigned)o[2],2); break;
        case KSN_JIT_S16I: st_(e,x+2u*(unsigned)o[2],a[o[0]]&0xFFFFu,2); break;
        case KSN_JIT_S8I: st_(e,x+(unsigned)o[2],a[o[0]]&0xFFu,1); break;
        case KSN_JIT_MOVI: a[o[0]]=(uint32_t)sext((uint32_t)o[1],12); break;
        case KSN_JIT_MOVN: a[o[0]]=x; break;
        case KSN_JIT_ADDN: a[o[0]]=x+y; break;
        case KSN_JIT_ADDI: a[o[0]]=x+(uint32_t)sext((uint32_t)o[2],8); break;
        case KSN_JIT_SUB: a[o[0]]=x-y; break;
        case KSN_JIT_NEG: a[o[0]]=0u-x; break;
        case KSN_JIT_XOR: a[o[0]]=x^y; break;
        case KSN_JIT_AND: a[o[0]]=x&y; break;
        case KSN_JIT_SRAI: a[o[0]]=(uint32_t)((int32_t)x>>o[2]); break;
        case KSN_JIT_SRLI: a[o[0]]=x>>o[2]; break;
        case KSN_JIT_SSAI: sar=(uint32_t)o[0]; break;
        case KSN_JIT_SRC: a[o[0]]=(uint32_t)((((uint64_t)x<<32)|y)>>sar); break;
        case KSN_JIT_SLL: a[o[0]]=(uint32_t)(((uint64_t)x)<<(32u-sar)); break;
        case KSN_JIT_EXTUI1: a[o[0]]=(x>>o[2])&1u; break;
        case KSN_JIT_EXTUI6: a[o[0]]=(x>>o[2])&63u; break;
        case KSN_JIT_EXTUI9: a[o[0]]=(x>>o[2])&511u; break;
        case KSN_JIT_EXTUI14: a[o[0]]=(x>>o[2])&0x3FFFu; break;
        case KSN_JIT_ADDMI: a[o[0]]=x+(uint32_t)sext((uint32_t)o[2],8)*256u; break;
        case KSN_JIT_ADDX4: a[o[0]]=(x<<2)+y; break;
        case KSN_JIT_MULL: a[o[0]]=x*y; break;
        case KSN_JIT_MULSH: a[o[0]]=(uint32_t)((uint64_t)((int64_t)(int32_t)x*(int32_t)y)>>32); break;
        case KSN_JIT_BEQ: if(a[o[0]]==x)next=pc+4u+(size_t)sext((uint32_t)o[2],8); break;
        case KSN_JIT_BGE: if((int32_t)a[o[0]]>=(int32_t)x)next=pc+4u+(size_t)sext((uint32_t)o[2],8); break;
        case KSN_JIT_BGEZ: if((int32_t)a[o[0]]>=0)next=pc+4u+(size_t)sext((uint32_t)o[1],12); break;
        case KSN_JIT_BEQZN: if(!a[o[0]])next=pc+4u+(size_t)o[1]; break;
        case KSN_JIT_BNEZN: if(a[o[0]])next=pc+4u+(size_t)o[1]; break;
        case KSN_JIT_J: next=pc+4u+(size_t)sext((uint32_t)o[0],18); break;
        case KSN_JIT_CALLX8:
            if((a[o[0]]!=A_LEAF&&a[o[0]]!=A_DRAW&&a[o[0]]!=A_CURVE)||a[10]!=A_CTX){fprintf(stderr,"EMU bad call\n");exit(2);}
            if(a[o[0]]==A_LEAF)a[10]=ksn_shiki_jit_leaf(e->run,a[11]);
            else if(a[o[0]]==A_CURVE)a[10]=ksn_shiki_jit_curve(e->run,(int32_t)a[11],(int32_t)a[12],a[13]);
            else a[10]=ksn_shiki_jit_draw(e->run,(int32_t)a[11],(int32_t)a[12],(int32_t)a[13],a[14]);
            for(unsigned k=8;k<16;k++)if(k!=10)a[k]=0xDEADBEEFu;   /* the callee's */
            sar=rnd();                                               /* not kept either */
            break;
        default: fprintf(stderr,"EMU form %d not emulated\n",f);exit(2);
        }
        pc=next;
    }
}

static ksn_proc_segment seg_i[KSN_SHIKI_SEGMENTS],seg_j[KSN_SHIKI_SEGMENTS];
static ksn_shiki_vm vi,vj;
/* CUBIC's weights (sec.13): odd trials give the code the table, even
 * ones none (the helper computes them), so both paths are compared. */
static int32_t weights[4*KSN_SHIKI_CODE*255];
static unsigned bailed,checks;

/* Compiles plan into buf (org A_CODE, the checker's entries when chk) and
 * checks that a sizing pass and the word-only writer agree; 0 on failure. */
static size_t compile(const ksn_shiki_plan *plan,uint8_t *buf,size_t cap,bool chk,uint64_t plain,
                      size_t *entry,uint16_t *sites,unsigned *nsites,unsigned t){
    ksn_jit_buf b={buf,0,cap,false,false,A_CODE,chk?A_CHK_ADD:0,chk?A_CHK_SUB:0,plain,sites,0};
    if(!ksn_shiki_jit_scalar(plan,&b,entry)){printf("COMPILE FAILED trial %u (n=%u)\n",t,plan->count);return 0;}
    if(nsites)*nsites=b.nsites;
    /* The device sizes first (no buffer, org 0), then emits into memory of
     * exactly that length (sec.14): both passes must agree. */
    ksn_jit_buf z={NULL,0,cap,false,false,0,b.chk_add,b.chk_sub,plain,NULL,0};
    size_t ez=0;
    if(!ksn_shiki_jit_scalar(plan,&z,&ez)||z.len!=b.len||ez!=*entry){
        printf("SIZING DIFF trial %u: %zu/%zu entry %zu/%zu\n",t,z.len,b.len,ez,*entry);return 0;
    }
    /* And the word-only writer (IRAM without a data alias) gives the
     * same bytes. */
    static uint32_t wbuf[8192/4];
    memset(wbuf,0xA5,sizeof wbuf);
    ksn_jit_buf w={(uint8_t *)wbuf,0,b.len,false,true,A_CODE,b.chk_add,b.chk_sub,plain,NULL,0};
    if(!ksn_shiki_jit_scalar(plan,&w,&ez)||w.len!=b.len||memcmp(wbuf,buf,b.len)){
        printf("WORD WRITER DIFF trial %u\n",t);return 0;
    }
    return b.len;
}
/* Runs the code once from a cleared vm, as ksn_shiki_draw_index would; true
 * when it agrees with the interpreter's run (vi, seg_i, want). */
static bool agrees(const ksn_shiki_plan *plan,uint8_t *code,size_t len,size_t entry,const int32_t *in,
                   unsigned nin,unsigned index,unsigned t,ksn_shiki_status want,ksn_shiki_status *gotp){
    memset(&vj,0,sizeof vj);
    int32_t inj[KSN_SHIKI_INPUTS]={0};
    for(unsigned k=0;k<plan->inputs&&k<nin;k++)inj[k]=in[k];
    const ksn_jit_run run={&vj,inj,KSN_SHIKI_STEPS,(uint32_t (*)(const void *,uint32_t))ksn_shiki_jit_leaf,
                           (uint32_t (*)(const void *,int32_t,int32_t,int32_t,uint32_t))ksn_shiki_jit_draw,
                           ksn_shiki_sin_table,
                           (uint32_t (*)(const void *,int32_t,int32_t,uint32_t))ksn_shiki_jit_curve,
                           0,0,
                           (t&1)&&ksn_shiki_cubic_weights(plan,weights,sizeof weights/4)?weights:NULL,
                           plan,seg_j,index};
    emu e={{A_VM,A_IN,KSN_SHIKI_STEPS,A_LEAF,A_DRAW,A_SIN,A_CURVE,0,0},&vj,inj,code,len,&run,0,0};
    const uint32_t r=emulate(&e,entry);
    checks+=e.checks;
    ksn_shiki_status got;
    if(r>=256u)got=(ksn_shiki_status)(r>>8);
    else{bailed++;got=ksn_shiki_resume(plan,inj,index,&vj,seg_j,r);}
    *gotp=got;
    bool bad=got!=want||memcmp(vi.reg,vj.reg,sizeof vi.reg)||vi.count!=vj.count||
             memcmp(seg_i,seg_j,vi.count*sizeof *seg_i);
    if(want==KSN_SHIKI_DONE)
        bad|=vi.steps!=vj.steps||vi.pen_x!=vj.pen_x||vi.pen_y!=vj.pen_y||vi.pen_valid!=vj.pen_valid||
             vi.raster!=vj.raster||vi.depth!=vj.depth;
    return !bad;
}

/* sec.18.3: the rest schedule, on a few hand-made samples. */
static unsigned sched_fails(void){
    unsigned bad=0;
#define EXPECT(c) do{if(!(c)){printf("SCHEDULE FAIL line %d: %s\n",__LINE__,#c);bad++;}}while(0)
    const uint32_t N=0x80000000u;
    EXPECT(ksn_shiki_jit_sat_first(N,16)==KSN_JIT_SAT_LOG);                 /* never ran */
    EXPECT(ksn_shiki_jit_sat_first(0x0FFFFFFFu,16)==KSN_JIT_SAT_FAST);       /* 28 bits: within 3 of the top */
    EXPECT(ksn_shiki_jit_sat_first(0x7FFFFFFFu,16)==KSN_JIT_SAT_FAST);       /* saturated */
    const uint32_t r=ksn_shiki_jit_sat_first(0x00FFFFFFu,16);                /* 24 bits: rest 16 */
    EXPECT((r&0xFFFFu)==32u&&(r>>16&31u)==24u&&(r>>21&1023u)==16u);
    const uint32_t r2=ksn_shiki_jit_sat_next(r,0x00800000u,32);              /* no growth: twice */
    EXPECT((r2&0xFFFFu)==64u&&(r2>>16&31u)==24u&&(r2>>21&1023u)==32u);
    const uint32_t r3=ksn_shiki_jit_sat_next(r2,0x01FFFFFFu,64);             /* +1 bit: 32*(31-3-25)/2 = 48 */
    EXPECT((r3>>16&31u)==25u&&(r3>>21&1023u)==48u&&(r3&0xFFFFu)==112u);
    EXPECT(ksn_shiki_jit_sat_next(r3,0x07FFFFFFu,112)==KSN_JIT_SAT_FAST);  /* +2 bits: 48*(31-3-27)/4 = 12 < 16 */
    EXPECT(ksn_shiki_jit_sat_next(r3,0x0FFFFFFFu,112)==KSN_JIT_SAT_FAST);  /* 28 bits */
    const uint32_t r4=ksn_shiki_jit_sat_next(r3,N,112);                      /* not reached: soon again */
    EXPECT((r4&0xFFFFu)==116u&&(r4>>21&1023u)==48u);
    uint32_t w=r;                                                             /* doubling stops at the cap */
    for(unsigned k=0;k<12;k++)w=ksn_shiki_jit_sat_next(w,0x00800000u,1000u+k);
    EXPECT((w>>21&1023u)==1023u);
    EXPECT(((ksn_shiki_jit_sat_next(r,0x00800000u,0xFFF0u)&0xFFFFu)==((0xFFF0u+32u)&0xFFFFu)));  /* wraps in 16 bits */
#undef EXPECT
    return bad;
}

int main(int argc,char **argv){
    static uint8_t buf[8192],cbuf[8192],pbuf[8192];
    ksn_shiki_plan *plan=calloc(1,KSN_SHIKI_PLAN_MAX);
    unsigned fails=0,by[5]={0},tried=0,maxlen=0,sites_all=0,sites_plain=0,sites_never=0,sites_fast=0;
    unsigned long total_bytes=0,total_inst=0,chk_bytes=0,plain_bytes=0;
    const unsigned trials=argc>1?(unsigned)atoi(argv[1]):20000;
    uint32_t gen=1;
    static const char *const MODE[]={"inline","checker","patched","plain"};
    for(unsigned t=0;t<trials;t++){
        static ksn_shiki_bench_case c;
        int32_t in[KSN_SHIKI_INPUTS];
        unsigned nin,index;
        ksn_shiki_bench_random(&gen,&c,in,&nin,&index);
        if(!ksn_shiki_register(c.code,c.len,NULL,0,NULL,0,plan)){t--;continue;}
        const unsigned n=plan->count;
        tried++;
        const ksn_shiki_status want=ksn_shiki_draw_index(plan,in,nin,index,&vi,seg_i);
        by[want]++;
        size_t entry=0,centry=0,pentry=0;
        uint16_t sites[KSN_SHIKI_CODE];
        unsigned nsites=0;
        /* inline: the checks in the code. checker (sec.18): the shared
         * checker, whose slots then say where the check can go; patched: that
         * code with those call0s turned into j over the slot; plain: compiled
         * again without those checks. The same draw has the same values, so
         * all four must agree with the interpreter. */
        const size_t len=compile(plan,buf,sizeof buf,false,0,&entry,NULL,NULL,t);
        const size_t clen=len?compile(plan,cbuf,sizeof cbuf,true,0,&centry,sites,&nsites,t):0;
        if(!len||!clen){fails++;continue;}
        total_bytes+=len;total_inst+=n;chk_bytes+=clen;
        if(len>maxlen)maxlen=(unsigned)len;
        ksn_shiki_status got;
        int bad=-1;
        if(!agrees(plan,buf,len,entry,in,nin,index,t,want,&got))bad=0;
        else if(!agrees(plan,cbuf,clen,centry,in,nin,index,t,want,&got))bad=1;
        else{
            uint64_t plain=0;
            memcpy(pbuf,cbuf,clen);
            for(unsigned k=0;k<nsites;k++){
                uint32_t w;memcpy(&w,cbuf+sites[k]+3u,4);
                ksn_jit_buf q={pbuf,sites[k],sites[k]+3u,false,false,0,0,0,0,NULL,0};
                if(w&0x80000000u){sites_never++;continue;}
                if(w>0x3FFFFFFFu){
                    /* Saturated (or as good as): kept, pointed at the fast
                     * entry of the same kind (ksn_shiki_jit.c's sat_patch). */
                    int o[3];
                    decode(pbuf+sites[k],3,o);
                    const uint32_t at=A_CODE+sites[k],old=(at&~3u)+4u+4u*(uint32_t)sext((uint32_t)o[0],18);
                    const uint32_t to=old==A_CHK_ADD?A_CHK_ADD_F:A_CHK_SUB_F;
                    ksn_jit_emit(&q,KSN_JIT_CALL0,(int)(((int32_t)to-(int32_t)(at&~3u)-4)/4),0,0);
                    sites_fast++;
                    continue;
                }
                plain|=1ull<<k;
                sites_plain++;
                ksn_jit_emit(&q,KSN_JIT_J,3,0,0);        /* over the slot */
            }
            sites_all+=nsites;
            if(!agrees(plan,pbuf,clen,centry,in,nin,index,t,want,&got))bad=2;
            else{
                const size_t plen=compile(plan,pbuf,sizeof pbuf,true,plain,&pentry,NULL,NULL,t);
                if(!plen){fails++;continue;}
                plain_bytes+=plen;
                if(!agrees(plan,pbuf,plen,pentry,in,nin,index,t,want,&got))bad=3;
            }
        }
        if(bad>=0&&fails++<10)
            printf("DIFF %s trial %u: status %d/%d steps %u/%u count %u/%u (n=%u)\n",MODE[bad],t,(int)got,(int)want,
                   vj.steps,vi.steps,vj.count,vi.count,n);
    }
    printf("scalar: %u programs (DONE %u INVALID %u LIMIT %u), %u handed back at the limit; "
           "code %.1f B per instruction, largest %u B\n",tried,by[KSN_SHIKI_DONE],by[KSN_SHIKI_INVALID],
           by[KSN_SHIKI_LIMIT],bailed,(double)total_bytes/(double)total_inst,maxlen);
    printf("scalar sec.18: checker %.1f B per instruction (inline %.1f), %u checker calls; of %u sites "
           "%u patched out, %u to the fast entry, %u never run; with them plain %.1f B per instruction\n",
           (double)chk_bytes/(double)total_inst,(double)total_bytes/(double)total_inst,checks,sites_all,
           sites_plain,sites_fast,sites_never,(double)plain_bytes/(double)total_inst);
    /* Both paths count CUBIC / LINE_PATTERN: half of these ran in the code. */
    printf("scalar: CUBIC %u calls, LINE_PATTERN %u calls (interpreter and code together)\n",
           (unsigned)ksn_shiki_curve_n[0],(unsigned)ksn_shiki_curve_n[1]);
    free(plan);
    fails+=sched_fails();
    if(fails){printf("SHIKI JIT SCALAR FAIL (%u)\n",fails);return 1;}
    printf("SHIKI JIT SCALAR PASS\n");
    return 0;
}
