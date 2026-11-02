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
enum { A_CTX=0x10000000u, A_VM=0x20000000u, A_IN=0x30000000u, A_CODE=0x40000000u, A_LEAF=0x50000000u };
typedef struct {
    uint32_t ctx[4];
    ksn_shiki_vm *vm;
    int32_t *in;
    const uint8_t *code; size_t code_len;
    const ksn_jit_run *run;
    unsigned steps;
} emu;
static uint8_t *addr(emu *e,uint32_t a,unsigned n){
    struct { uint32_t base; void *p; size_t len; } m[]={
        {A_CTX,e->ctx,sizeof e->ctx},{A_VM,e->vm,sizeof *e->vm},
        {A_IN,e->in,4u*KSN_SHIKI_INPUTS},{A_CODE,(void *)e->code,e->code_len}};
    for(unsigned k=0;k<4;k++)
        if(a>=m[k].base&&a-m[k].base+n<=m[k].len){
            if((a-m[k].base)%n){fprintf(stderr,"EMU unaligned %08x\n",a);exit(2);}
            return (uint8_t *)m[k].p+(a-m[k].base);
        }
    fprintf(stderr,"EMU bad address %08x\n",a);exit(2);
}
static uint32_t ld(emu *e,uint32_t a,unsigned n){uint32_t v=0;memcpy(&v,addr(e,a,n),n);return v;}
static void st_(emu *e,uint32_t a,uint32_t v,unsigned n){
    if(a>=A_CODE&&a<A_LEAF){fprintf(stderr,"EMU store to code\n");exit(2);}
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
        for(unsigned k=0;k<F->nbits;k++)field|=1u<<F->bits[k].at;
        const uint32_t all=F->len==4?0xFFFFFFFFu:((1u<<(8*F->len))-1u);
        if((w&all&~field)!=(F->base&all&~field))continue;
        if(found>=0){fprintf(stderr,"EMU ambiguous %d/%u\n",found,f);exit(2);}
        found=(int)f;
        for(unsigned j=0;j<3;j++)o[j]=0;
        unsigned width[3]={0,0,0};
        for(unsigned k=0;k<F->nbits;k++){
            const ksn_jit_bit *t=&F->bits[k];
            const unsigned bit=((w^F->base)>>t->at)&1u;
            o[t->operand]|=(int)(bit<<t->bit);
            if(t->bit+1u>width[t->operand])width[t->operand]=t->bit+1u;
        }
        for(unsigned j=0;j<3;j++)o[j]^=F->base_op[j]&((1<<width[j])-1);
    }
    return found;
}

/* Runs the code at entry; returns its a2. */
static uint32_t emulate(emu *e,size_t entry){
    uint32_t a[16]={0},sar=0;
    a[2]=A_CTX;
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
        case KSN_JIT_RETWN: return a[2];
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
        case KSN_JIT_MULL: a[o[0]]=x*y; break;
        case KSN_JIT_MULSH: a[o[0]]=(uint32_t)((uint64_t)((int64_t)(int32_t)x*(int32_t)y)>>32); break;
        case KSN_JIT_BEQ: if(a[o[0]]==x)next=pc+4u+(size_t)sext((uint32_t)o[2],8); break;
        case KSN_JIT_BGE: if((int32_t)a[o[0]]>=(int32_t)x)next=pc+4u+(size_t)sext((uint32_t)o[2],8); break;
        case KSN_JIT_BGEZ: if((int32_t)a[o[0]]>=0)next=pc+4u+(size_t)sext((uint32_t)o[1],12); break;
        case KSN_JIT_BEQZN: if(!a[o[0]])next=pc+4u+(size_t)o[1]; break;
        case KSN_JIT_BNEZN: if(a[o[0]])next=pc+4u+(size_t)o[1]; break;
        case KSN_JIT_J: next=pc+4u+(size_t)sext((uint32_t)o[0],18); break;
        case KSN_JIT_CALLX8:
            if(a[o[0]]!=A_LEAF||a[10]!=A_CTX){fprintf(stderr,"EMU bad call\n");exit(2);}
            a[10]=ksn_shiki_jit_leaf(e->run,a[11]);
            for(unsigned k=8;k<16;k++)if(k!=10)a[k]=0xDEADBEEFu;   /* the callee's */
            sar=rnd();                                               /* not kept either */
            break;
        default: fprintf(stderr,"EMU form %d not emulated\n",f);exit(2);
        }
        pc=next;
    }
}

int main(int argc,char **argv){
    static ksn_proc_segment seg_i[KSN_SHIKI_SEGMENTS],seg_j[KSN_SHIKI_SEGMENTS];
    static uint8_t buf[8192];
    ksn_shiki_plan *plan=calloc(1,KSN_SHIKI_PLAN_MAX);
    static ksn_shiki_vm vi,vj;
    unsigned fails=0,by[5]={0},tried=0,bailed=0,maxlen=0;
    unsigned long total_bytes=0,total_inst=0;
    const unsigned trials=argc>1?(unsigned)atoi(argv[1]):20000;
    uint32_t gen=1;
    for(unsigned t=0;t<trials;t++){
        static ksn_shiki_bench_case c;
        int32_t in[KSN_SHIKI_INPUTS];
        unsigned nin,index;
        ksn_shiki_bench_random(&gen,&c,in,&nin,&index);
        if(!ksn_shiki_register(c.code,c.len,NULL,0,NULL,0,plan)){t--;continue;}
        const unsigned n=plan->count;
        tried++;
        const ksn_shiki_status want=ksn_shiki_draw_index(plan,in,nin,index,&vi,seg_i);
        ksn_jit_buf b={buf,0,sizeof buf,false};
        size_t entry=0;
        if(!ksn_shiki_jit_scalar(plan,&b,&entry)){printf("COMPILE FAILED trial %u (n=%u)\n",t,n);fails++;continue;}
        total_bytes+=b.len;total_inst+=n;
        if(b.len>maxlen)maxlen=(unsigned)b.len;
        /* As ksn_shiki_draw_index: a cleared vm, the inputs copied. */
        memset(&vj,0,sizeof vj);
        int32_t inj[KSN_SHIKI_INPUTS]={0};
        for(unsigned k=0;k<plan->inputs&&k<nin;k++)inj[k]=in[k];
        const ksn_jit_run run={&vj,inj,KSN_SHIKI_STEPS,(uint32_t (*)(const void *,uint32_t))ksn_shiki_jit_leaf,plan,seg_j,index};
        emu e={{A_VM,A_IN,KSN_SHIKI_STEPS,A_LEAF},&vj,inj,buf,b.len,&run,0};
        const uint32_t r=emulate(&e,entry);
        ksn_shiki_status got;
        if(r>=256u)got=(ksn_shiki_status)(r>>8);
        else{bailed++;got=ksn_shiki_resume(plan,inj,index,&vj,seg_j,r);}
        by[want]++;
        int bad=got!=want||memcmp(vi.reg,vj.reg,sizeof vi.reg)||vi.count!=vj.count||
                memcmp(seg_i,seg_j,vi.count*sizeof *seg_i);
        if(want==KSN_SHIKI_DONE)
            bad|=vi.steps!=vj.steps||vi.pen_x!=vj.pen_x||vi.pen_y!=vj.pen_y||vi.pen_valid!=vj.pen_valid||
                 vi.raster!=vj.raster||vi.depth!=vj.depth;
        if(bad&&fails++<10)
            printf("DIFF trial %u: status %d/%d steps %u/%u count %u/%u (n=%u)\n",t,(int)got,(int)want,
                   vj.steps,vi.steps,vj.count,vi.count,n);
    }
    printf("scalar: %u programs (DONE %u INVALID %u LIMIT %u), %u handed back at the limit; "
           "code %.1f B per instruction, largest %u B\n",tried,by[KSN_SHIKI_DONE],by[KSN_SHIKI_INVALID],
           by[KSN_SHIKI_LIMIT],bailed,(double)total_bytes/(double)total_inst,maxlen);
    free(plan);
    if(fails){printf("SHIKI JIT SCALAR FAIL (%u)\n",fails);return 1;}
    printf("SHIKI JIT SCALAR PASS\n");
    return 0;
}
