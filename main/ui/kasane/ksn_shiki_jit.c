#include "ksn_shiki_jit.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
/* As ksn_shiki.c: only the real ESP32-S3 compiler reaches the device path. */
#if defined(__xtensa__) && defined(__XTENSA__) && defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
#define KSN_JIT_DEVICE 1
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_cpu.h"
#else
#define KSN_JIT_DEVICE 0
#endif

#include "ksn_shiki_jit_enc.h"

void ksn_jit_emit(ksn_jit_buf *b,unsigned form,int o0,int o1,int o2){
    if(form>=KSN_JIT_FORMS)return;
    const ksn_jit_form *f=&KSN_JIT_FORM[form];
    if(b->overflow||b->len+f->len>b->cap){b->overflow=true;return;}
    const int o[3]={o0,o1,o2};
    uint32_t w=f->base;
    for(unsigned k=0;k<f->nbits;k++){
        const ksn_jit_bit *t=&f->bits[k];
        if((((unsigned)o[t->operand]^(unsigned)f->base_op[t->operand])>>t->bit)&1u)w^=1u<<t->at;
    }
    for(unsigned k=0;k<f->len;k++)b->p[b->len++]=(uint8_t)(w>>(8*k));
}

/* ---- Stage 2: VEACH sections as PIE code (shiki-jit.md sec.3) --------------
 * A section is a VEACH whose body (up to its END) is vector instructions
 * only. Its code runs the body over the full blocks in a zero-overhead loop
 * with Shiki's vN in qN and the accumulator in QACC; the interpreter's
 * memory image of both (vregs, the QACC image, the same bytes the
 * instruction-by-instruction PIE path keeps) is read before the loop and
 * written after it, so the interpreter runs the tail block, and everything
 * after the section, from where the code left off.
 *
 * Address registers: a2 the context (below), a3 the block count, a4 a
 * scratch, a5 the constants' address, a6..a15 one per (region, field) that
 * the body loads or stores and one per distinct VOUT shift. The pointers
 * advance by 16 at their last use in the body (EE.VLD.128.IP's increment),
 * so each moves one block per iteration however often the body uses it. */
#define JIT_AR_FIRST 6
#define JIT_ARS 10
#define JIT_CONSTS 8
#define JIT_SHIFTS 4
#define JIT_IMAGES 2

/* What the code reads, laid out for 32-bit pointers on both sides (the host
 * builds the code to check it, never runs it). */
typedef struct {
    uint32_t ptr[JIT_ARS];        /* 0: the planes' addresses at the first block */
    int32_t blocks;               /* 40 */
    uint32_t vregs,acc;           /* 44, 48: v0..v7 and the QACC image */
    int32_t shift[JIT_SHIFTS];    /* 52 */
    int32_t pad[3];
    int16_t konst[JIT_CONSTS];    /* 80: sat16 of the registers the body reads */
    uint32_t accimg[JIT_IMAGES];  /* 96: QACC images of VACCSET's values */
} ksn_jit_ctx;
_Static_assert(offsetof(ksn_jit_ctx,blocks)==40&&offsetof(ksn_jit_ctx,vregs)==44&&
               offsetof(ksn_jit_ctx,acc)==48&&offsetof(ksn_jit_ctx,shift)==52&&
               offsetof(ksn_jit_ctx,konst)==80&&offsetof(ksn_jit_ctx,accimg)==96,
               "the generated code's offsets");

static int find_or_add(uint8_t *list,uint8_t *n,unsigned max,uint8_t v){
    for(unsigned k=0;k<*n;k++)if(list[k]==v)return (int)k;
    if(*n>=max)return -1;
    list[*n]=v;
    return (int)(*n)++;
}
static void pad_to16(ksn_jit_buf *b,unsigned after){
    /* nop.n (2) and nop (3) until (len + after) is a multiple of 16. */
    unsigned need=(16u-((unsigned)(b->len+after)&15u))&15u;
    while(need&&!b->overflow){
        if(need%3u){ksn_jit_emit(b,KSN_JIT_NOPN,0,0,0);need=(need-2u)&15u;}   /* 1 wraps to 15 */
        else {ksn_jit_emit(b,KSN_JIT_NOP,0,0,0);need-=3u;}
    }
}

/* Compiles the VEACH at pc into b (16-aligned at b->len on entry); false,
 * with b->len unchanged, when the section is not one the JIT takes. */
bool ksn_shiki_jit_section(const ksn_shiki_plan *plan,unsigned pc,ksn_jit_section *s,ksn_jit_buf *b){
    const ksn_shiki_inst *v=&plan->code[pc];
    if(v->op!=KSN_SHIKI_VEACH||v->end<=pc+1u||v->end>=plan->count)return false;
    memset(s,0,sizeof *s);
    s->pc=(uint8_t)pc;
    s->steps=(uint8_t)(v->end-pc);              /* the body and END, per block */
    uint8_t ptr_key[JIT_ARS],nptr=0,last[JIT_ARS];
    bool reads_v7=false,mack=false;
    for(unsigned k=pc+1u;k<v->end;k++){
        const ksn_shiki_inst *i=&plan->code[k];
        switch(i->op){
        case KSN_SHIKI_VLOAD: case KSN_SHIKI_VSTORE: {
            const int at=find_or_add(ptr_key,&nptr,JIT_ARS,(uint8_t)(i->a<<4|i->b));
            if(at<0)return false;
            last[at]=(uint8_t)k;
            reads_v7|=i->dst==7;
            break;
        }
        case KSN_SHIKI_VSPLAT:
            if(find_or_add(s->const_reg,&s->nconst,JIT_CONSTS,i->a)<0)return false;
            reads_v7|=i->dst==7;
            break;
        case KSN_SHIKI_VADD: case KSN_SHIKI_VSUB: case KSN_SHIKI_VMIN: case KSN_SHIKI_VMAX:
            reads_v7|=i->dst==7||i->a==7||i->b==7;
            break;
        case KSN_SHIKI_VZACC: s->acc=true; break;
        case KSN_SHIKI_VMAC: s->acc=true; reads_v7|=i->a==7||i->b==7; break;
        case KSN_SHIKI_VMACK:
            s->acc=mack=true; reads_v7|=i->a==7;
            if(find_or_add(s->const_reg,&s->nconst,JIT_CONSTS,i->b)<0)return false;
            break;
        case KSN_SHIKI_VOUT:
            s->acc=true; reads_v7|=i->dst==7;
            if(find_or_add(s->shift,&s->nshift,JIT_SHIFTS,(uint8_t)i->aux)<0)return false;
            break;
        /* Every lane = r[a]: the QACC image of that value is made once per
         * call (the run-time below) and loaded here, 4 instructions. */
        case KSN_SHIKI_VACCSET:
            s->acc=true;
            if(find_or_add(s->img_reg,&s->nimg,JIT_IMAGES,i->a)<0)return false;
            break;
        default: return false;                  /* VIOTA, scalar, EMIT, loops */
        }
    }
    /* VMACK multiplies by one lane of q7, which holds the constants. */
    if(mack&&reads_v7)return false;
    s->q7=mack;
    if(nptr+s->nshift>JIT_ARS)return false;
    s->nptr=nptr;
    for(unsigned k=0;k<nptr;k++){s->ptr_region[k]=ptr_key[k]>>4;s->ptr_field[k]=ptr_key[k]&15u;}
    const unsigned qs=s->q7?7u:8u;              /* the vector registers moved through memory */

    const size_t start=b->len;
    s->off=(uint16_t)start;
    ksn_jit_emit(b,KSN_JIT_ENTRY32,0,0,0);
    ksn_jit_emit(b,KSN_JIT_L32I,3,2,40/4);
    for(unsigned k=0;k<nptr;k++)ksn_jit_emit(b,KSN_JIT_L32I,JIT_AR_FIRST+(int)k,2,(int)k);
    for(unsigned k=0;k<s->nshift;k++)ksn_jit_emit(b,KSN_JIT_L32I,JIT_AR_FIRST+(int)(nptr+k),2,(52+4*(int)k)/4);
    if(s->nconst)ksn_jit_emit(b,KSN_JIT_ADDI,5,2,80);
    if(s->q7)ksn_jit_emit(b,KSN_JIT_VLD128IP,7,5,0);
    ksn_jit_emit(b,KSN_JIT_L32I,4,2,44/4);
    for(unsigned q=0;q<qs;q++)ksn_jit_emit(b,KSN_JIT_VLD128IP,(int)q,4,1);
    if(s->acc){
        ksn_jit_emit(b,KSN_JIT_L32I,4,2,48/4);
        ksn_jit_emit(b,KSN_JIT_LDQACCLL,4,0,0);ksn_jit_emit(b,KSN_JIT_LDQACCLH,4,0,0);
        ksn_jit_emit(b,KSN_JIT_LDQACCHL,4,0,0);ksn_jit_emit(b,KSN_JIT_LDQACCHH,4,0,0);
    }
    pad_to16(b,3);                               /* the body starts 16-aligned */
    const size_t loop_at=b->len;
    ksn_jit_emit(b,KSN_JIT_LOOPGTZ,3,0,0);       /* the offset is patched below */
    const size_t body=b->len;
    for(unsigned k=pc+1u;k<v->end;k++){
        const ksn_shiki_inst *i=&plan->code[k];
        switch(i->op){
        case KSN_SHIKI_VLOAD: case KSN_SHIKI_VSTORE: {
            const int at=find_or_add(ptr_key,&nptr,JIT_ARS,(uint8_t)(i->a<<4|i->b));
            ksn_jit_emit(b,i->op==KSN_SHIKI_VLOAD?KSN_JIT_VLD128IP:KSN_JIT_VST128IP,
                         i->dst,JIT_AR_FIRST+at,last[at]==k?1:0);
            break;
        }
        case KSN_SHIKI_VSPLAT: {
            const int c=find_or_add(s->const_reg,&s->nconst,JIT_CONSTS,i->a);
            ksn_jit_emit(b,KSN_JIT_ADDI,4,5,2*c);
            ksn_jit_emit(b,KSN_JIT_VLDBC16,i->dst,4,0);
            break;
        }
        case KSN_SHIKI_VADD: ksn_jit_emit(b,KSN_JIT_VADDS16,i->dst,i->a,i->b); break;
        case KSN_SHIKI_VSUB: ksn_jit_emit(b,KSN_JIT_VSUBS16,i->dst,i->a,i->b); break;
        case KSN_SHIKI_VMIN: ksn_jit_emit(b,KSN_JIT_VMIN16,i->dst,i->a,i->b); break;
        case KSN_SHIKI_VMAX: ksn_jit_emit(b,KSN_JIT_VMAX16,i->dst,i->a,i->b); break;
        case KSN_SHIKI_VZACC: ksn_jit_emit(b,KSN_JIT_ZEROQACC,0,0,0); break;
        case KSN_SHIKI_VMAC: ksn_jit_emit(b,KSN_JIT_VMULAS16,i->a,i->b,0); break;
        case KSN_SHIKI_VMACK: {
            const int c=find_or_add(s->const_reg,&s->nconst,JIT_CONSTS,i->b);
            ksn_jit_emit(b,KSN_JIT_VSMULAS16,i->a,7,c);
            break;
        }
        case KSN_SHIKI_VOUT: {
            const int j=find_or_add(s->shift,&s->nshift,JIT_SHIFTS,(uint8_t)i->aux);
            ksn_jit_emit(b,KSN_JIT_SRCMB16,i->dst,JIT_AR_FIRST+(int)nptr+j,0);
            break;
        }
        case KSN_SHIKI_VACCSET: {
            const int j=find_or_add(s->img_reg,&s->nimg,JIT_IMAGES,i->a);
            ksn_jit_emit(b,KSN_JIT_L32I,4,2,(96+4*j)/4);
            ksn_jit_emit(b,KSN_JIT_LDQACCLL,4,0,0);ksn_jit_emit(b,KSN_JIT_LDQACCLH,4,0,0);
            ksn_jit_emit(b,KSN_JIT_LDQACCHL,4,0,0);ksn_jit_emit(b,KSN_JIT_LDQACCHH,4,0,0);
            break;
        }
        default: break;
        }
    }
    const size_t end=b->len;
    /* loopgtz: the loop ends at its own address + 4 + the offset. */
    if(b->overflow||end==body||end-loop_at-4u>255u){b->len=start;b->overflow=false;return false;}
    b->p[loop_at+2]=(uint8_t)(end-loop_at-4u);
    ksn_jit_emit(b,KSN_JIT_L32I,4,2,44/4);
    for(unsigned q=0;q<qs;q++)ksn_jit_emit(b,KSN_JIT_VST128IP,(int)q,4,1);
    if(s->acc){
        ksn_jit_emit(b,KSN_JIT_L32I,4,2,48/4);
        ksn_jit_emit(b,KSN_JIT_STQACCLL,4,0,0);ksn_jit_emit(b,KSN_JIT_STQACCLH,4,0,0);
        ksn_jit_emit(b,KSN_JIT_STQACCHL,4,0,0);ksn_jit_emit(b,KSN_JIT_STQACCHH,4,0,0);
    }
    ksn_jit_emit(b,KSN_JIT_RETWN,0,0,0);
    if(b->overflow){b->len=start;b->overflow=false;return false;}
    s->len=(uint16_t)(b->len-start);
    return true;
}

/* Every section of the plan, one after another, each 16-aligned. */
unsigned ksn_shiki_jit_plan(const ksn_shiki_plan *plan,ksn_jit_section *s,unsigned max,ksn_jit_buf *b){
    unsigned n=0;
    for(unsigned pc=0;pc<plan->count&&n<max;pc++){
        if(plan->code[pc].op!=KSN_SHIKI_VEACH)continue;
        while(b->len&15u){if(b->len<b->cap)b->p[b->len++]=0;else return n;}
        if(ksn_shiki_jit_section(plan,pc,&s[n],b))n++;
    }
    return n;
}

/* ---- Stage 2 at run time (device) ------------------------------------------ */
bool ksn_shiki_jit_enabled=true;

#if KSN_JIT_DEVICE
typedef struct {
    uint8_t *raw,*exec;           /* the allocation, and the code in it (16-aligned) */
    unsigned n;
    ksn_jit_section s[KSN_JIT_SECTIONS];
} jit_plan;
#define JIT_SCRATCH 2048u        /* 4 sections of at most ~400 B each */

void ksn_shiki_jit_attach(ksn_shiki_plan *plan){
    plan->jit=NULL;
    bool any=false;
    for(unsigned pc=0;pc<plan->count;pc++)any|=plan->code[pc].op==KSN_SHIKI_VEACH;
    if(!any)return;
    uint8_t *tmp=malloc(JIT_SCRATCH);
    jit_plan *j=calloc(1,sizeof *j);
    if(!tmp||!j){free(tmp);free(j);return;}
    ksn_jit_buf b={tmp,0,JIT_SCRATCH,false};
    j->n=ksn_shiki_jit_plan(plan,j->s,KSN_JIT_SECTIONS,&b);
    if(!j->n){free(tmp);free(j);return;}
    /* Rounded up to whole words: the instruction bus takes only word stores. */
    const size_t len=(b.len+3u)&~(size_t)3u;
    j->raw=heap_caps_malloc(len+16u,MALLOC_CAP_EXEC);
    if(!j->raw){free(tmp);free(j);return;}       /* no code: the interpreter runs it */
    j->exec=(uint8_t *)(((uintptr_t)j->raw+15u)&~(uintptr_t)15u);
    volatile uint32_t *w=(volatile uint32_t *)(esp_ptr_in_diram_iram(j->exec)?
                                               esp_ptr_diram_iram_to_dram(j->exec):(void *)j->exec);
    memset(tmp+b.len,0,len-b.len);
    for(size_t k=0;k<len/4;k++)
        w[k]=(uint32_t)tmp[4*k]|(uint32_t)tmp[4*k+1]<<8|(uint32_t)tmp[4*k+2]<<16|(uint32_t)tmp[4*k+3]<<24;
    __asm__ volatile("memw\n" "isync\n":::"memory");
    free(tmp);
    plan->jit=j;
}
void ksn_shiki_jit_detach(ksn_shiki_plan *plan){
    jit_plan *j=plan?plan->jit:NULL;
    if(!j)return;
    heap_caps_free(j->raw);
    free(j);
    plan->jit=NULL;
}
static int16_t jit_sat16(int32_t v){return v>32767?32767:v<-32768?-32768:(int16_t)v;}

uint32_t ksn_shiki_jit_veach(const ksn_shiki_plan *plan,unsigned pc,uint32_t start,uint32_t count,
                             ksn_shiki_vm *vm,void *vregs,void *acc){
    const jit_plan *j=plan->jit;
    if(!j||!ksn_shiki_jit_enabled||!vm->pie)return 0;
    const ksn_jit_section *s=NULL;
    for(unsigned k=0;k<j->n&&!s;k++)if(j->s[k].pc==pc)s=&j->s[k];
    const uint32_t full=count/8u;
    if(!s||!full||(start&7u))return 0;
    /* The limit is never reached inside the code: the interpreter stops at
     * the exact step, so a section that would reach it is left to it. */
    if((uint32_t)vm->steps+full*s->steps>=KSN_SHIKI_STEPS)return 0;
    ksn_jit_ctx ctx __attribute__((aligned(16)));
    for(unsigned k=0;k<s->nptr;k++){
        const ksn_shiki_bound *g=&plan->region[s->ptr_region[k]];
        const int16_t *d=(const int16_t *)g->data+(uint32_t)s->ptr_field[k]*g->stride+start;
        if((uintptr_t)d&15u)return 0;            /* as the PIE path: unaligned is the C model's */
        ctx.ptr[k]=(uint32_t)(uintptr_t)d;
    }
    for(unsigned k=0;k<s->nshift;k++)ctx.shift[k]=s->shift[k];
    for(unsigned k=0;k<s->nconst;k++)ctx.konst[k]=jit_sat16(vm->reg[s->const_reg[k]]);
    /* The same images the interpreter's PIE path makes for VACCSET. */
    uint8_t img[JIT_IMAGES][64] __attribute__((aligned(16)));
    for(unsigned k=0;k<s->nimg;k++){
        ksn_shiki_pie_accset_image(img[k],vm->reg[s->img_reg[k]]);
        ctx.accimg[k]=(uint32_t)(uintptr_t)img[k];
    }
    ctx.blocks=(int32_t)full;
    ctx.vregs=(uint32_t)(uintptr_t)vregs;
    ctx.acc=(uint32_t)(uintptr_t)acc;
    ((void (*)(ksn_jit_ctx *))(void *)(j->exec+s->off))(&ctx);
    vm->steps=(uint16_t)(vm->steps+full*s->steps);
    return full;
}
#endif

#if KSN_JIT_DEVICE
/* Two functions, assembled once with the toolchain that builds this firmware
 * (xtensa-esp32s3-elf-as, esp-15.2.0) and kept as bytes; stage 0 checks
 * that such bytes run from RAM, not how they are made (that is the encoder's
 * job, shiki-jit.md sec.4). Windowed ABI: entry a1, 32 ... retw.n; a2.. are
 * the arguments, a2 the result.
 *
 * one(a, b, d): d = a +sat b over 8 lanes (one 128-bit block); returns 42.
 *   entry a1,32; ee.vld.128.ip q0,a2,0; ee.vld.128.ip q1,a3,0;
 *   ee.vadds.s16 q2,q0,q1; ee.vst.128.ip q2,a4,0; movi.n a2,42; retw.n */
static const uint8_t ONE[]={0x36,0x41,0x00,0x24,0x00,0x83,0x34,0x80,0x83,0x64,0x08,0x9e,
                            0x44,0x00,0x9a,0x2c,0xa2,0x1d,0xf0};
/* loop(a, b, d, blocks): the same over `blocks` blocks in a zero-overhead
 * loop, pointers post-incremented by 16; returns 43. The assembler put a
 * nop.n before loopgtz so that the body starts aligned; the bytes keep that
 * only if the function starts 16-aligned, as it did when assembled.
 *   entry a1,32; nop.n; loopgtz a5,end; ee.vld.128.ip q0,a2,16;
 *   ee.vld.128.ip q1,a3,16; ee.vadds.s16 q2,q0,q1; ee.vst.128.ip q2,a4,16;
 *   end: movi.n a2,43; retw.n */
static const uint8_t LOOP[]={0x36,0x41,0x00,0x3d,0xf0,0x76,0xa5,0x0b,0x24,0x01,0x83,0x34,
                             0x81,0x83,0x64,0x08,0x9e,0x44,0x01,0x9a,0x2c,0xb2,0x1d,0xf0};
#define LOOP_AT 32u                       /* offset of LOOP in the buffer, 16-aligned */
#define CODE_BYTES 64u
#define BLOCKS 16u

typedef int (*one_fn)(const int16_t *,const int16_t *,int16_t *);
typedef int (*loop_fn)(const int16_t *,const int16_t *,int16_t *,int);

static int16_t sat16(int32_t v){return v>32767?32767:v<-32768?-32768:(int16_t)v;}

/* Copy bytes into executable memory as 32-bit words: the instruction bus
 * alias takes only word stores, and the data bus alias (when there is one)
 * is written the same way so one path serves both. */
static void put_code(volatile uint32_t *w,const uint8_t *code,size_t len,size_t at){
    uint8_t buf[CODE_BYTES];
    memset(buf,0,sizeof buf);
    memcpy(buf+at,code,len);
    for(size_t k=at/4;k<(at+len+3)/4;k++)
        w[k]=(uint32_t)buf[4*k]|(uint32_t)buf[4*k+1]<<8|(uint32_t)buf[4*k+2]<<16|(uint32_t)buf[4*k+3]<<24;
}

bool ksn_shiki_jit_probe(char *out,size_t n){
#if defined(CONFIG_ESP_SYSTEM_MEMPROT) && CONFIG_ESP_SYSTEM_MEMPROT
    const int memprot=1;
#else
    const int memprot=0;
#endif
    /* 16 bytes extra so that the code can start 16-aligned. */
    uint8_t *raw=heap_caps_malloc(CODE_BYTES+16u,MALLOC_CAP_EXEC);
    if(!raw){snprintf(out,n,"CB jit memprot=%d alloc=NULL",memprot);return true;}
    uint8_t *x=(uint8_t *)(((uintptr_t)raw+15u)&~(uintptr_t)15u);
    const int exec=esp_ptr_executable(x)?1:0;
    const int diram=esp_ptr_in_diram_iram(x)?1:0;
    volatile uint32_t *w=(volatile uint32_t *)(diram?esp_ptr_diram_iram_to_dram(x):(void *)x);
    if(!exec){
        snprintf(out,n,"CB jit memprot=%d alloc=%p exec=0 (not run)",memprot,(void *)x);
        heap_caps_free(raw);
        return true;
    }
    put_code(w,ONE,sizeof ONE,0);
    put_code(w,LOOP,sizeof LOOP,LOOP_AT);
    __asm__ volatile("memw\n" "isync\n":::"memory");

    /* Per call, not .bss: every build would carry 1 KB for one bench line. */
    int16_t *mem=heap_caps_aligned_alloc(16,4u*BLOCKS*8u*sizeof(int16_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!mem){snprintf(out,n,"CB jit NO MEMORY");heap_caps_free(raw);return true;}
    int16_t *a=mem,*b=a+BLOCKS*8,*d=b+BLOCKS*8,*want=d+BLOCKS*8;
    const size_t bytes=BLOCKS*8*sizeof(int16_t);
    for(unsigned k=0;k<BLOCKS*8;k++){
        a[k]=(int16_t)(k*977-30000);
        b[k]=(int16_t)(k&1?32000-(int)k*13:-(int)k*251);
        want[k]=sat16((int32_t)a[k]+b[k]);
    }
    memset(d,0,bytes);
    const int r1=((one_fn)(void *)x)(a,b,d);
    const int ok1=r1==42&&!memcmp(d,want,16)&&d[8]==0;

    memset(d,0,bytes);
    const int r2=((loop_fn)(void *)(x+LOOP_AT))(a,b,d,BLOCKS);
    const int ok2=r2==43&&!memcmp(d,want,bytes);

    /* 7-run minimum each: the generated loop, and the same sum in C. */
    uint32_t jit=UINT32_MAX,c=UINT32_MAX;
    for(unsigned r=0;r<7;r++){
        const uint32_t t0=esp_cpu_get_cycle_count();
        ((loop_fn)(void *)(x+LOOP_AT))(a,b,d,BLOCKS);
        const uint32_t dt=esp_cpu_get_cycle_count()-t0;
        if(dt<jit)jit=dt;
    }
    for(unsigned r=0;r<7;r++){
        const uint32_t t0=esp_cpu_get_cycle_count();
        for(unsigned k=0;k<BLOCKS*8;k++)d[k]=sat16((int32_t)a[k]+b[k]);
        __asm__ volatile("":::"memory");
        const uint32_t dt=esp_cpu_get_cycle_count()-t0;
        if(dt<c)c=dt;
    }
    snprintf(out,n,"CB jit memprot=%d alloc=%p diram=%d exec=1 one=%s/%d loop=%s/%d blocks=%u cy_jit=%lu cy_c=%lu",
             memprot,(void *)x,diram,ok1?"same":"DIFF",r1,ok2?"same":"DIFF",r2,BLOCKS,
             (unsigned long)jit,(unsigned long)c);
    heap_caps_free(mem);
    heap_caps_free(raw);
    return true;
}
#else
bool ksn_shiki_jit_probe(char *out,size_t n){
    snprintf(out,n,"CB jit device only");
    return true;
}
#endif
