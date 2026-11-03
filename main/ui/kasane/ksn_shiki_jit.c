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
#include "esp_log.h"
#include "shiki_heap.h"
#else
#define KSN_JIT_DEVICE 0
#endif

#include "ksn_shiki_jit_enc.h"

/* One byte at b->len (ksn_jit_buf: word_only memory gets it by
 * read-modify-write of the aligned word). */
static inline void jit_put(ksn_jit_buf *b,uint8_t v){
    if(b->word_only){
        volatile uint32_t *w=(volatile uint32_t *)(void *)(b->p+(b->len&~(size_t)3u));
        const unsigned sh=(unsigned)(b->len&3u)*8u;
        *w=(*w&~(0xFFu<<sh))|((uint32_t)v<<sh);
    }else b->p[b->len]=v;
    b->len++;
}
void ksn_jit_emit(ksn_jit_buf *b,unsigned form,int o0,int o1,int o2){
    if(form>=KSN_JIT_FORMS)return;
    const ksn_jit_form *f=&KSN_JIT_FORM[form];
    if(b->overflow||b->len+f->len>b->cap){b->overflow=true;return;}
    if(!b->p){b->len+=f->len;return;}           /* a sizing pass: the length is enough */
    const int o[3]={o0,o1,o2};
    /* Each run of operand bits that differ from the base is XORed in with one
     * shift and mask (sec.17: the bit-by-bit loop was 91% of a compile). */
    uint32_t w=f->base;
    for(unsigned k=0;k<f->nruns;k++){
        const ksn_jit_field *r=&f->runs[k];
        const uint32_t d=((uint32_t)o[r->operand]^(uint32_t)f->base_op[r->operand])>>r->src;
        w^=(d&((1u<<r->len)-1u))<<r->at;
    }
    if(b->word_only){for(unsigned k=0;k<f->len;k++)jit_put(b,(uint8_t)(w>>(8*k)));return;}
    uint8_t *q=b->p+b->len;
    q[0]=(uint8_t)w;q[1]=(uint8_t)(w>>8);
    if(f->len>2)q[2]=(uint8_t)(w>>16);
    if(f->len>3)q[3]=(uint8_t)(w>>24);
    b->len+=f->len;
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

/* ---- Stage 3: the whole plan as scalar code (shiki-jit.md sec.11) ----------
 * Address registers: a2 the run context, a3 the vm (its registers at 0, so
 * r[k] is l32i.n at 4k), a4 the step limit, a5 the inputs, a6 INT32_MAX
 * (the saturation's fix-up), a7 the helper; a8..a15 are scratch within one
 * instruction (callx8 gives them to the helper). The vm stays in memory:
 * the code reads and writes the same registers, loop frames, depth and
 * step count the interpreter does, so it can hand any block over to it.
 *
 * Steps are counted per block: a block runs from the instruction after a
 * loop opener, END or BREAK_IF_GT (or pc 0) to the next of them, and its
 * header adds its length to vm->steps, or returns its pc to the interpreter
 * when that would pass the limit (the interpreter then stops at the exact
 * step). Every jump target is a block start, so every path passes a header.
 * Loop depth is lexical (registration matched each END to its opener), so
 * each frame's address is known when compiling. */
#define VM_REM(f) (offsetof(ksn_shiki_vm,loop)+(f)*sizeof(((ksn_shiki_vm *)0)->loop[0])+offsetof(__typeof__(((ksn_shiki_vm *)0)->loop[0]),remaining))
#define VM_LPC(f) (offsetof(ksn_shiki_vm,loop)+(f)*sizeof(((ksn_shiki_vm *)0)->loop[0])+offsetof(__typeof__(((ksn_shiki_vm *)0)->loop[0]),pc))
_Static_assert(offsetof(ksn_shiki_vm,reg)==0,"r[k] at 4k");
_Static_assert(VM_REM(KSN_SHIKI_LOOP_DEPTH-1)<=510&&offsetof(ksn_shiki_vm,steps)<=510&&
               offsetof(ksn_shiki_vm,depth)<=255&&VM_LPC(KSN_SHIKI_LOOP_DEPTH-1)<=255,
               "the vm's fields within l16ui/s8i reach");
_Static_assert(KSN_SHIKI_LOOP_DEPTH<=8&&KSN_SHIKI_CODE<=64,"pcs and depths fit a movi");

static bool opens(uint8_t op){
    return op==KSN_SHIKI_REPEAT||op==KSN_SHIKI_REPEAT_REG||op==KSN_SHIKI_EACH||op==KSN_SHIKI_VEACH;
}
static bool ends_block(uint8_t op){
    return opens(op)||op==KSN_SHIKI_END||op==KSN_SHIKI_BREAK_IF_GT;
}
/* Re-encodes the instruction at `at` (a branch whose offset is known now). */
static void patch(ksn_jit_buf *b,size_t at,unsigned form,int o0,int o1,int o2){
    if(b->overflow)return;
    const size_t len=b->len;
    b->len=at;
    ksn_jit_emit(b,form,o0,o1,o2);
    b->len=len;
}
/* l32r: not in the generated table (its target is pc-relative to the word
 * below, ((pc + 3) & ~3) + 4 * (imm16 - 65536)); test_scalar.py checks the
 * bytes with objdump. */
static void emit_l32r(ksn_jit_buf *b,unsigned t,size_t literal){
    if(b->overflow||b->len+3u>b->cap){b->overflow=true;return;}
    const long words=((long)literal-(long)((b->len+3u)&~(size_t)3u))/4;
    if(words< -65536||words>=0){b->overflow=true;return;}
    const uint32_t imm=(uint32_t)words&0xFFFFu;
    if(!b->p){b->len+=3u;return;}
    jit_put(b,(uint8_t)(0x01u|t<<4));
    jit_put(b,(uint8_t)imm);
    jit_put(b,(uint8_t)(imm>>8));
}
/* dst = r[a] (+ or -) r[b] saturated: overflow when the operands' signs
 * say so (add: both differ from the sum's; sub: a's differs from b's and
 * from the difference's); the fix-up is INT32_MAX or MIN by a's sign. */
/* sec.18: the site of a shared check. The checker (ksn_jit_chk_add/_sub,
 * IRAM) takes a8, a9 and the wrapped a10, saturates a10, ORs |a10| into the
 * 4-byte slot after the call0 (its bit 31, set here, says "never run") and
 * returns past the slot. The slot must be a word: the loads and the add
 * take their wide forms (or a nop leads) until the call0 is at 1 mod 4.
 * Patched out (ksn_shiki_jit_run's decision), the call0 becomes j over the
 * slot. */
#define JIT_SLOT_NEVER 0x80000000u
static void emit_checked(ksn_jit_buf *b,const ksn_shiki_inst *i,bool sub){
    const unsigned least=sub?7u:6u;              /* l32i.n, l32i.n, add.n or sub */
    unsigned wide=(1u-(unsigned)(b->org+b->len+least))&3u;
    if(sub&&wide==3u){ksn_jit_emit(b,KSN_JIT_NOP,0,0,0);wide=0;}
    ksn_jit_emit(b,wide>=1u?KSN_JIT_L32I:KSN_JIT_L32IN,8,3,i->a);
    ksn_jit_emit(b,wide>=2u?KSN_JIT_L32I:KSN_JIT_L32IN,9,3,i->b);
    if(sub)ksn_jit_emit(b,KSN_JIT_SUB,10,8,9);
    else ksn_jit_emit(b,wide==3u?KSN_JIT_ADD:KSN_JIT_ADDN,10,8,9);
    if(b->sites&&b->nsites<KSN_SHIKI_CODE)b->sites[b->nsites++]=(uint16_t)b->len;
    long words=0;
    if(b->p){
        const uint32_t pc=b->org+(uint32_t)b->len,to=sub?b->chk_sub:b->chk_add;
        words=((long)to-(long)(pc&~3u)-4)/4;
        if((pc&3u)!=1u||words< -131072||words>131071){b->overflow=true;return;}
    }
    ksn_jit_emit(b,KSN_JIT_CALL0,(int)words,0,0);
    if(b->overflow||b->len+4u>b->cap){b->overflow=true;return;}
    for(unsigned k=0;k<4;k++){
        if(b->p)jit_put(b,(uint8_t)(JIT_SLOT_NEVER>>(8*k)));
        else b->len++;
    }
    ksn_jit_emit(b,KSN_JIT_S32IN,10,3,i->dst);
}
static void emit_addsub(ksn_jit_buf *b,const ksn_shiki_inst *i,bool sub,unsigned site){
    if(site<64u&&(b->plain>>site&1u)){
        /* Decided (sec.18): the logs showed room, the check is gone. */
        ksn_jit_emit(b,KSN_JIT_L32IN,8,3,i->a);
        ksn_jit_emit(b,KSN_JIT_L32IN,9,3,i->b);
        if(sub)ksn_jit_emit(b,KSN_JIT_SUB,8,8,9);
        else ksn_jit_emit(b,KSN_JIT_ADDN,8,8,9);
        ksn_jit_emit(b,KSN_JIT_S32IN,8,3,i->dst);
        return;
    }
    if(b->chk_add){emit_checked(b,i,sub);return;}
    ksn_jit_emit(b,KSN_JIT_L32IN,8,3,i->a);
    ksn_jit_emit(b,KSN_JIT_L32IN,9,3,i->b);
    if(sub){
        ksn_jit_emit(b,KSN_JIT_SUB,10,8,9);
        ksn_jit_emit(b,KSN_JIT_XOR,11,8,9);
        ksn_jit_emit(b,KSN_JIT_XOR,12,8,10);
    }else{
        ksn_jit_emit(b,KSN_JIT_ADDN,10,8,9);
        ksn_jit_emit(b,KSN_JIT_XOR,11,8,10);
        ksn_jit_emit(b,KSN_JIT_XOR,12,9,10);
    }
    ksn_jit_emit(b,KSN_JIT_AND,11,11,12);
    const size_t br=b->len;
    ksn_jit_emit(b,KSN_JIT_BGEZ,11,0,0);
    ksn_jit_emit(b,KSN_JIT_SRAI,10,8,31);
    ksn_jit_emit(b,KSN_JIT_XOR,10,10,6);
    patch(b,br,KSN_JIT_BGEZ,11,(int)(b->len-br-4u),0);
    ksn_jit_emit(b,KSN_JIT_S32IN,10,3,i->dst);
}
/* a10 holds the result and a11 the 64-bit value's high word (its sign is
 * the true result's); hi_ok and lo_sign are the registers whose equality
 * says the result fits. */
static void emit_fix(ksn_jit_buf *b,unsigned hi_ok,unsigned lo_sign,unsigned sign_of){
    const size_t br=b->len;
    ksn_jit_emit(b,KSN_JIT_BEQ,(int)hi_ok,(int)lo_sign,0);
    ksn_jit_emit(b,KSN_JIT_SRAI,10,(int)sign_of,31);
    ksn_jit_emit(b,KSN_JIT_XOR,10,10,6);
    patch(b,br,KSN_JIT_BEQ,(int)hi_ok,(int)lo_sign,(int)(b->len-br-4u));
}
static void emit_mul(ksn_jit_buf *b,const ksn_shiki_inst *i){
    const unsigned s=i->aux;                         /* 0..62 (registration) */
    ksn_jit_emit(b,KSN_JIT_L32IN,8,3,i->a);
    ksn_jit_emit(b,KSN_JIT_L32IN,9,3,i->b);
    ksn_jit_emit(b,KSN_JIT_MULL,10,8,9);
    ksn_jit_emit(b,KSN_JIT_MULSH,11,8,9);
    if(s==0){
        ksn_jit_emit(b,KSN_JIT_SRAI,12,10,31);
        emit_fix(b,11,12,11);
    }else if(s<32){
        /* floor(p / 2^s) is p >> s (arithmetic); it fits when the high
         * word shifted alone is the low word's sign. */
        ksn_jit_emit(b,KSN_JIT_SSAI,(int)s,0,0);
        ksn_jit_emit(b,KSN_JIT_SRC,10,11,10);
        ksn_jit_emit(b,KSN_JIT_SRAI,12,11,(int)s);
        ksn_jit_emit(b,KSN_JIT_SRAI,13,10,31);
        emit_fix(b,12,13,11);
    }else{
        /* |p| <= 2^62, so p >> s (s >= 32) always fits. */
        ksn_jit_emit(b,KSN_JIT_SRAI,10,11,(int)(s-32u));
    }
    ksn_jit_emit(b,KSN_JIT_S32IN,10,3,i->dst);
}
static void emit_shift(ksn_jit_buf *b,const ksn_shiki_inst *i){
    const int t=i->imm;                              /* -31..31 (registration) */
    ksn_jit_emit(b,KSN_JIT_L32IN,8,3,i->a);
    if(t==0){
        ksn_jit_emit(b,KSN_JIT_S32IN,8,3,i->dst);
        return;
    }
    if(t<0){
        ksn_jit_emit(b,KSN_JIT_SRAI,10,8,-t);
    }else{
        /* sll shifts by 32 - SAR; src gives the high word of (sign:a) << t. */
        ksn_jit_emit(b,KSN_JIT_SSAI,32-t,0,0);
        ksn_jit_emit(b,KSN_JIT_SLL,10,8,0);
        ksn_jit_emit(b,KSN_JIT_SRAI,9,8,31);
        ksn_jit_emit(b,KSN_JIT_SRC,11,9,8);
        ksn_jit_emit(b,KSN_JIT_SRAI,12,10,31);
        emit_fix(b,11,12,9);
    }
    ksn_jit_emit(b,KSN_JIT_S32IN,10,3,i->dst);
}

/* SIN (sec.4.4): p = r & 0xFFFF, quadrant p >> 14, w = p & 0x3FFF mirrored
 * in odd quadrants; T[i] + floor((T[i+1] - T[i]) * t / 64) with i = w >> 6,
 * t = w & 63 (T[i+1] is read only when t > 0, so w = 0x4000 reads T[256]
 * alone); negated in quadrants 2 and 3. The table's address is ctx->sin. */
static void emit_sin(ksn_jit_buf *b,const ksn_shiki_inst *i){
    ksn_jit_emit(b,KSN_JIT_L32IN,8,3,i->a);
    ksn_jit_emit(b,KSN_JIT_EXTUI14,9,8,0);           /* w */
    ksn_jit_emit(b,KSN_JIT_EXTUI1,10,8,14);          /* odd quadrant */
    size_t br=b->len;
    ksn_jit_emit(b,KSN_JIT_BEQZN,10,0,0);
    ksn_jit_emit(b,KSN_JIT_NEG,9,9,0);
    ksn_jit_emit(b,KSN_JIT_ADDMI,9,9,0x4000/256);    /* 0x4000 - w */
    patch(b,br,KSN_JIT_BEQZN,10,(int)(b->len-br-4u),0);
    ksn_jit_emit(b,KSN_JIT_EXTUI9,11,9,6);           /* i, 0..256 */
    ksn_jit_emit(b,KSN_JIT_EXTUI6,12,9,0);           /* t */
    ksn_jit_emit(b,KSN_JIT_L32IN,13,2,5);            /* ctx->sin */
    ksn_jit_emit(b,KSN_JIT_ADDX4,11,11,13);
    ksn_jit_emit(b,KSN_JIT_L32IN,14,11,0);
    br=b->len;
    ksn_jit_emit(b,KSN_JIT_BEQZN,12,0,0);
    ksn_jit_emit(b,KSN_JIT_L32IN,15,11,1);
    ksn_jit_emit(b,KSN_JIT_SUB,15,15,14);
    ksn_jit_emit(b,KSN_JIT_MULL,15,15,12);
    ksn_jit_emit(b,KSN_JIT_SRAI,15,15,6);
    ksn_jit_emit(b,KSN_JIT_ADDN,14,14,15);
    patch(b,br,KSN_JIT_BEQZN,12,(int)(b->len-br-4u),0);
    ksn_jit_emit(b,KSN_JIT_EXTUI1,10,8,15);          /* quadrants 2, 3 */
    br=b->len;
    ksn_jit_emit(b,KSN_JIT_BEQZN,10,0,0);
    ksn_jit_emit(b,KSN_JIT_NEG,14,14,0);
    patch(b,br,KSN_JIT_BEQZN,10,(int)(b->len-br-4u),0);
    ksn_jit_emit(b,KSN_JIT_S32IN,14,3,i->dst);
}

#define JIT_FIXUPS (2u*KSN_SHIKI_CODE)
bool ksn_shiki_jit_scalar(const ksn_shiki_plan *plan,ksn_jit_buf *b,size_t *entry){
    const unsigned n=plan->count;
    /* The literal pool: SET's values that movi cannot hold. */
    int32_t pool[KSN_SHIKI_CODE];
    unsigned npool=0;
    uint8_t lit[KSN_SHIKI_CODE];
    int32_t wrow[KSN_SHIKI_CODE];                    /* CUBIC: its first row in ctx->weights */
    int32_t rows=0;
    for(unsigned pc=0;pc<n;pc++){
        const ksn_shiki_inst *i=&plan->code[pc];
        int32_t v;
        if(i->op==KSN_SHIKI_SET)v=i->imm;
        else if(i->op==KSN_SHIKI_LINE||i->op==KSN_SHIKI_PLOT)v=i->aux;  /* the colour */
        else if(i->op==KSN_SHIKI_CUBIC){v=wrow[pc]=rows;rows+=4*i->imm;}  /* ksn_shiki_cubic_weights' order */
        else continue;
        if(v>=-2048&&v<=2047)continue;
        unsigned k=0;
        while(k<npool&&pool[k]!=v)k++;
        if(k==npool)pool[npool++]=v;
        lit[pc]=(uint8_t)k;
    }
    const size_t base=b->len;                        /* 4-aligned: the pool's words */
    for(unsigned k=0;k<npool;k++)
        for(unsigned j=0;j<4;j++){
            if(b->len>=b->cap){b->overflow=true;return false;}
            if(b->p)jit_put(b,(uint8_t)((uint32_t)pool[k]>>(8*j)));
            else b->len++;
        }
    *entry=b->len;
    ksn_jit_emit(b,KSN_JIT_ENTRY48,0,0,0);
    ksn_jit_emit(b,KSN_JIT_L32IN,3,2,0);
    ksn_jit_emit(b,KSN_JIT_L32IN,5,2,1);
    ksn_jit_emit(b,KSN_JIT_L32IN,4,2,2);
    ksn_jit_emit(b,KSN_JIT_MOVI,6,-1,0);
    ksn_jit_emit(b,KSN_JIT_SRLI,6,6,1);
    /* sec.18: call0 overwrites a0, the code's own return; the checker
     * reloads it from ctx->ra before it returns. */
    unsigned site=0;
    if(b->chk_add)
        for(unsigned pc=0;pc<n;pc++){
            const uint8_t op=plan->code[pc].op;
            if(op!=KSN_SHIKI_ADD&&op!=KSN_SHIKI_SUB)continue;
            if(site>=64u||!(b->plain>>site&1u)){ksn_jit_emit(b,KSN_JIT_S32I,0,2,7);break;}
            site++;
        }
    site=0;
    b->nsites=0;

    size_t at[KSN_SHIKI_CODE+1];
    struct { uint16_t at; uint8_t target; } fix[JIT_FIXUPS];
    /* sec.13: an innermost REPEAT / REPEAT_REG whose body has no BREAK_IF_GT
     * counts its steps once on entry, count * (body + END), and keeps the
     * count in a7 (callx8 leaves a0..a7 alone); its END is addi + branch. */
    bool hoist[KSN_SHIKI_CODE];
    for(unsigned pc=0;pc<n;pc++){
        const ksn_shiki_inst *i=&plan->code[pc];
        hoist[pc]=false;
        if(i->op!=KSN_SHIKI_REPEAT&&i->op!=KSN_SHIKI_REPEAT_REG)continue;
        bool ok=true;
        for(unsigned k=pc+1u;k<i->end&&ok;k++)ok=!ends_block(plan->code[k].op);
        hoist[pc]=ok;
    }
    size_t body_at[KSN_SHIKI_LOOP_DEPTH];            /* a hoisted loop's body, or SIZE_MAX */
    unsigned nfix=0;
    uint8_t stack[KSN_SHIKI_LOOP_DEPTH];             /* the openers, innermost last */
    unsigned depth=0;
    bool block=true;
    /* A j to the block at pc_, its offset patched at the end. */
#define JUMP(pc_) do{ \
        if(nfix<JIT_FIXUPS){fix[nfix].at=(uint16_t)b->len;fix[nfix++].target=(uint8_t)(pc_);} \
        else{b->overflow=true;} \
        ksn_jit_emit(b,KSN_JIT_J,0,0,0); \
    }while(0)
    for(unsigned pc=0;pc<n;pc++){
        const ksn_shiki_inst *i=&plan->code[pc];
        at[pc]=b->len;
        if(block&&pc>0&&hoist[pc-1u]&&depth&&stack[depth-1u]==pc-1u){
            /* The hoisted loop's entry (its opener pushed the frame). */
            const unsigned f=depth-1u,len=plan->code[pc-1u].end-(pc-1u);   /* body + END */
            ksn_jit_emit(b,KSN_JIT_L16UI,7,3,(int)(VM_REM(f)/2));
            ksn_jit_emit(b,KSN_JIT_L16UI,8,3,(int)(offsetof(ksn_shiki_vm,steps)/2));
            ksn_jit_emit(b,KSN_JIT_MOVI,9,(int)len,0);
            ksn_jit_emit(b,KSN_JIT_MULL,9,9,7);
            ksn_jit_emit(b,KSN_JIT_ADDN,8,8,9);
            const size_t br=b->len;
            ksn_jit_emit(b,KSN_JIT_BGE,4,8,0);
            ksn_jit_emit(b,KSN_JIT_MOVI,2,(int)pc,0);
            ksn_jit_emit(b,KSN_JIT_RETWN,0,0,0);
            patch(b,br,KSN_JIT_BGE,4,8,(int)(b->len-br-4u));
            ksn_jit_emit(b,KSN_JIT_S16I,8,3,(int)(offsetof(ksn_shiki_vm,steps)/2));
            body_at[f]=b->len;
            block=false;
        }
        if(block){
            unsigned k=pc;
            while(k<n&&!ends_block(plan->code[k].op))k++;
            const unsigned len=(k<n?k+1u:n)-pc;      /* through the terminator */
            ksn_jit_emit(b,KSN_JIT_L16UI,8,3,(int)(offsetof(ksn_shiki_vm,steps)/2));
            ksn_jit_emit(b,KSN_JIT_ADDI,8,8,(int)len);
            const size_t br=b->len;
            ksn_jit_emit(b,KSN_JIT_BGE,4,8,0);
            ksn_jit_emit(b,KSN_JIT_MOVI,2,(int)pc,0);
            ksn_jit_emit(b,KSN_JIT_RETWN,0,0,0);
            patch(b,br,KSN_JIT_BGE,4,8,(int)(b->len-br-4u));
            ksn_jit_emit(b,KSN_JIT_S16I,8,3,(int)(offsetof(ksn_shiki_vm,steps)/2));
            block=false;
        }
        switch(i->op){
        case KSN_SHIKI_SET:
            if(i->imm>=-2048&&i->imm<=2047)ksn_jit_emit(b,KSN_JIT_MOVI,8,i->imm,0);
            else emit_l32r(b,8,base+4u*lit[pc]);
            ksn_jit_emit(b,KSN_JIT_S32IN,8,3,i->dst);
            break;
        case KSN_SHIKI_INPUT:
            ksn_jit_emit(b,KSN_JIT_L32IN,8,5,i->a);
            ksn_jit_emit(b,KSN_JIT_S32IN,8,3,i->dst);
            break;
        case KSN_SHIKI_ADD: emit_addsub(b,i,false,site++); break;
        case KSN_SHIKI_SUB: emit_addsub(b,i,true,site++); break;
        case KSN_SHIKI_NEG: {
            /* -INT32_MIN is the only overflow: then a & -a is negative. */
            ksn_jit_emit(b,KSN_JIT_L32IN,8,3,i->a);
            ksn_jit_emit(b,KSN_JIT_NEG,10,8,0);
            ksn_jit_emit(b,KSN_JIT_AND,11,8,10);
            const size_t br=b->len;
            ksn_jit_emit(b,KSN_JIT_BGEZ,11,0,0);
            ksn_jit_emit(b,KSN_JIT_MOVN,10,6,0);
            patch(b,br,KSN_JIT_BGEZ,11,(int)(b->len-br-4u),0);
            ksn_jit_emit(b,KSN_JIT_S32IN,10,3,i->dst);
            break;
        }
        case KSN_SHIKI_MUL: emit_mul(b,i); break;
        case KSN_SHIKI_SHIFT: emit_shift(b,i); break;
        case KSN_SHIKI_SIN: emit_sin(b,i); break;
        case KSN_SHIKI_CUBIC: case KSN_SHIKI_LINE_PATTERN: {
            /* ksn_shiki_jit_curve(ctx, r[a], r[b], pc); for CUBIC x is its
             * first row in the weights table instead. */
            ksn_jit_emit(b,KSN_JIT_MOVN,10,2,0);
            if(i->op==KSN_SHIKI_LINE_PATTERN){
                ksn_jit_emit(b,KSN_JIT_L32IN,11,3,i->a);
                ksn_jit_emit(b,KSN_JIT_L32IN,12,3,i->b);
            }else if(wrow[pc]<=2047)ksn_jit_emit(b,KSN_JIT_MOVI,11,wrow[pc],0);  /* x: the row (sec.13) */
            else emit_l32r(b,11,base+4u*lit[pc]);
            ksn_jit_emit(b,KSN_JIT_MOVI,13,(int)pc,0);
            ksn_jit_emit(b,KSN_JIT_L32IN,9,2,6);         /* ctx->curve */
            ksn_jit_emit(b,KSN_JIT_CALLX8,9,0,0);
            const size_t go=b->len;
            ksn_jit_emit(b,KSN_JIT_BEQZN,10,0,0);
            ksn_jit_emit(b,KSN_JIT_MOVN,2,10,0);
            ksn_jit_emit(b,KSN_JIT_RETWN,0,0,0);
            patch(b,go,KSN_JIT_BEQZN,10,(int)(b->len-go-4u),0);
            break;
        }
        case KSN_SHIKI_MOVE: case KSN_SHIKI_LINE: case KSN_SHIKI_PLOT:
        case KSN_SHIKI_LINE_C: case KSN_SHIKI_PLOT_C: {
            /* ksn_shiki_jit_draw(ctx, r[a], r[b], colour, kind). */
            static const uint8_t kind[]={[KSN_SHIKI_MOVE]=KSN_JIT_DRAW_MOVE,[KSN_SHIKI_LINE]=KSN_JIT_DRAW_LINE,
                                         [KSN_SHIKI_PLOT]=KSN_JIT_DRAW_PLOT,[KSN_SHIKI_LINE_C]=KSN_JIT_DRAW_LINE_C,
                                         [KSN_SHIKI_PLOT_C]=KSN_JIT_DRAW_PLOT_C};
            ksn_jit_emit(b,KSN_JIT_MOVN,10,2,0);
            ksn_jit_emit(b,KSN_JIT_L32IN,11,3,i->a);
            ksn_jit_emit(b,KSN_JIT_L32IN,12,3,i->b);
            if(i->op==KSN_SHIKI_LINE_C||i->op==KSN_SHIKI_PLOT_C)ksn_jit_emit(b,KSN_JIT_L32IN,13,3,i->dst);
            else if(i->op==KSN_SHIKI_MOVE)ksn_jit_emit(b,KSN_JIT_MOVI,13,0,0);
            else if(i->aux<=2047)ksn_jit_emit(b,KSN_JIT_MOVI,13,i->aux,0);
            else emit_l32r(b,13,base+4u*lit[pc]);
            ksn_jit_emit(b,KSN_JIT_MOVI,14,kind[i->op],0);
            ksn_jit_emit(b,KSN_JIT_L32IN,9,2,4);         /* ctx->draw */
            ksn_jit_emit(b,KSN_JIT_CALLX8,9,0,0);
            const size_t go=b->len;
            ksn_jit_emit(b,KSN_JIT_BEQZN,10,0,0);
            ksn_jit_emit(b,KSN_JIT_MOVN,2,10,0);
            ksn_jit_emit(b,KSN_JIT_RETWN,0,0,0);
            patch(b,go,KSN_JIT_BEQZN,10,(int)(b->len-go-4u),0);
            break;
        }
        case KSN_SHIKI_REPEAT:
            if(depth>=KSN_SHIKI_LOOP_DEPTH)return false;
            ksn_jit_emit(b,KSN_JIT_MOVI,8,(int)pc,0);
            ksn_jit_emit(b,KSN_JIT_S8I,8,3,(int)VM_LPC(depth));
            ksn_jit_emit(b,KSN_JIT_MOVI,8,i->imm,0);     /* 1..255 */
            ksn_jit_emit(b,KSN_JIT_S16I,8,3,(int)(VM_REM(depth)/2));
            ksn_jit_emit(b,KSN_JIT_MOVI,8,(int)depth+1,0);
            ksn_jit_emit(b,KSN_JIT_S8I,8,3,(int)offsetof(ksn_shiki_vm,depth));
            body_at[depth]=SIZE_MAX;
            stack[depth++]=(uint8_t)pc;
            block=true;
            break;
        case KSN_SHIKI_END: {
            if(!depth)return false;
            const unsigned f=depth-1u,o=stack[f];
            if(body_at[f]!=SIZE_MAX){
                /* A hoisted loop: the count is in a7, the steps are counted. */
                ksn_jit_emit(b,KSN_JIT_ADDI,7,7,-1);
                const size_t br=b->len;
                ksn_jit_emit(b,KSN_JIT_BEQZN,7,0,0);
                ksn_jit_emit(b,KSN_JIT_J,(int)((long)body_at[f]-(long)b->len-4),0,0);
                patch(b,br,KSN_JIT_BEQZN,7,(int)(b->len-br-4u),0);
                ksn_jit_emit(b,KSN_JIT_MOVI,8,(int)f,0);
                ksn_jit_emit(b,KSN_JIT_S8I,8,3,(int)offsetof(ksn_shiki_vm,depth));
                depth--;
                block=true;
                break;
            }
            ksn_jit_emit(b,KSN_JIT_L16UI,8,3,(int)(VM_REM(f)/2));
            ksn_jit_emit(b,KSN_JIT_ADDI,8,8,-1);
            ksn_jit_emit(b,KSN_JIT_S16I,8,3,(int)(VM_REM(f)/2));
            const size_t br=b->len;
            ksn_jit_emit(b,KSN_JIT_BEQZN,8,0,0);
            JUMP(o+1u);
            patch(b,br,KSN_JIT_BEQZN,8,(int)(b->len-br-4u),0);
            ksn_jit_emit(b,KSN_JIT_MOVI,8,(int)f,0);
            ksn_jit_emit(b,KSN_JIT_S8I,8,3,(int)offsetof(ksn_shiki_vm,depth));
            depth--;
            block=true;
            break;
        }
        case KSN_SHIKI_BREAK_IF_GT: {
            if(!depth)return false;
            const unsigned f=depth-1u,o=stack[f];
            ksn_jit_emit(b,KSN_JIT_L32IN,8,3,i->a);
            ksn_jit_emit(b,KSN_JIT_L32IN,9,3,i->b);
            const size_t br=b->len;
            ksn_jit_emit(b,KSN_JIT_BGE,9,8,0);
            ksn_jit_emit(b,KSN_JIT_MOVI,8,(int)f,0);
            ksn_jit_emit(b,KSN_JIT_S8I,8,3,(int)offsetof(ksn_shiki_vm,depth));
            JUMP(plan->code[o].end+1u);
            patch(b,br,KSN_JIT_BGE,9,8,(int)(b->len-br-4u));
            block=true;
            break;
        }
        default: {
            /* The helper: ksn_shiki_jit_leaf(ctx, pc). */
            const bool skips=opens(i->op);
            if(skips&&depth>=KSN_SHIKI_LOOP_DEPTH)return false;
            ksn_jit_emit(b,KSN_JIT_MOVN,10,2,0);
            ksn_jit_emit(b,KSN_JIT_MOVI,11,(int)pc,0);
            ksn_jit_emit(b,KSN_JIT_L32IN,9,2,3);         /* ctx->leaf */
            ksn_jit_emit(b,KSN_JIT_CALLX8,9,0,0);
            const size_t go=b->len;
            ksn_jit_emit(b,KSN_JIT_BEQZN,10,0,0);
            if(skips){
                ksn_jit_emit(b,KSN_JIT_ADDI,9,10,-1);
                const size_t br=b->len;
                ksn_jit_emit(b,KSN_JIT_BNEZN,9,0,0);
                JUMP(i->end+1u);
                patch(b,br,KSN_JIT_BNEZN,9,(int)(b->len-br-4u),0);
            }
            ksn_jit_emit(b,KSN_JIT_MOVN,2,10,0);
            ksn_jit_emit(b,KSN_JIT_RETWN,0,0,0);
            patch(b,go,KSN_JIT_BEQZN,10,(int)(b->len-go-4u),0);
            if(skips){body_at[depth]=SIZE_MAX;stack[depth++]=(uint8_t)pc;block=true;}
            break;
        }
        }
        if(b->overflow)return false;
    }
#undef JUMP
    if(depth)return false;
    at[n]=b->len;                                    /* the program's end: DONE */
    ksn_jit_emit(b,KSN_JIT_MOVI,2,(int)KSN_SHIKI_DONE<<8,0);
    ksn_jit_emit(b,KSN_JIT_RETWN,0,0,0);
    for(unsigned k=0;k<nfix;k++){
        const long off=(long)at[fix[k].target]-(long)fix[k].at-4;
        patch(b,fix[k].at,KSN_JIT_J,(int)off,0,0);
    }
    return !b->overflow;
}

/* ---- Stage 2 at run time (device) ------------------------------------------ */
bool ksn_shiki_jit_enabled=true;
/* Stage 3's switch and memory (sec.11); defined everywhere so that the
 * session's log can read them, used on the device only. */
bool ksn_shiki_jit_scalar_on=true;
uint32_t ksn_shiki_jit_fail[3];
/* Plans that gave their region blocks back to the Shiki region (sec.15). */
uint32_t ksn_shiki_jit_evicted;
/* Plans whose code was dropped for not being drawn lately (sec.16). */
uint32_t ksn_shiki_jit_cold;
/* Compiles attempted and their cycles (sec.17), for the session's log. */
uint32_t ksn_shiki_jit_compiles,ksn_shiki_jit_compile_cy;
uint32_t ksn_shiki_jit_sat[3];
uint32_t ksn_shiki_jit_budget=16384u,ksn_shiki_jit_bytes;

#if KSN_JIT_DEVICE
/* sec.18: the shared saturation checker, reached by call0 from each ADD/SUB
 * site (emit_checked): a8, a9 the operands, a10 the wrapped result, a2 the
 * context, a6 INT32_MAX, a0 the slot's instruction-bus address. Saturates
 * a10 (as the inline check), ORs |a10| into the slot through the data bus
 * (ctx->alias) and clears its never-run bit, reloads the code's a0 from
 * ctx->ra and returns past the slot. Uses a11..a14, which the code treats
 * as scratch. In IRAM: within call0's reach of the Shiki region. */
__asm__(
"    .pushsection .iram1.ksn_jit_chk,\"ax\",@progbits\n"
"    .align 4\n"
"    .global ksn_jit_chk_sub\n"
"ksn_jit_chk_sub:\n"
"    xor a11, a8, a9\n"
"    xor a12, a8, a10\n"
"    j 1f\n"
"    .align 4\n"
"    .global ksn_jit_chk_add\n"
"ksn_jit_chk_add:\n"
"    xor a11, a8, a10\n"
"    xor a12, a9, a10\n"
/* The loads are issued ahead of their uses (a load's result costs a cycle
 * when the next instruction reads it). */
"1:  l32i a13, a2, 32\n"
"    and a11, a11, a12\n"
"    add a13, a13, a0\n"
"    bgez a11, 2f\n"
"    srai a10, a8, 31\n"
"    xor a10, a10, a6\n"
"2:  l32i a12, a13, 0\n"
"    srai a14, a10, 31\n"
"    xor a14, a14, a10\n"
"    and a12, a12, a6\n"
"    or a12, a12, a14\n"
"    s32i a12, a13, 0\n"
"    addi a13, a0, 4\n"
"    l32i a0, a2, 28\n"
"    jx a13\n"
/* The fast entries: the same saturation, no log. A site whose check stays
 * is pointed here once its plan has decided (sat_pass). */
"    .align 4\n"
"    .global ksn_jit_chk_sub_fast\n"
"ksn_jit_chk_sub_fast:\n"
"    xor a11, a8, a9\n"
"    xor a12, a8, a10\n"
"    j 3f\n"
"    .align 4\n"
"    .global ksn_jit_chk_add_fast\n"
"ksn_jit_chk_add_fast:\n"
"    xor a11, a8, a10\n"
"    xor a12, a9, a10\n"
"3:  and a11, a11, a12\n"
"    bgez a11, 4f\n"
"    srai a10, a8, 31\n"
"    xor a10, a10, a6\n"
"4:  addi a12, a0, 4\n"
"    l32i a0, a2, 28\n"
"    jx a12\n"
"    .popsection\n");
extern const uint8_t ksn_jit_chk_add[],ksn_jit_chk_sub[],ksn_jit_chk_add_fast[],ksn_jit_chk_sub_fast[];
_Static_assert(offsetof(ksn_jit_run,ra)==28&&offsetof(ksn_jit_run,alias)==32,"the checker's offsets");

/* Measurement build (shiki-jit.md sec.15): -DPOCKET_SHIKI_JIT_EXECMAP logs
 * where executable memory goes (EXECMAP lines). */
#ifdef POCKET_SHIKI_JIT_EXECMAP
#include "esp_log.h"
#include "esp_timer.h"
/* Where the JIT's code landed: [0] DIRAM from the system heap, [1]
 * elsewhere (RTC fast memory), [2] the Shiki region. */
static uint32_t execmap_where[3];
typedef struct {
    intptr_t start,end;
    uint32_t free,largest,nfree,used,nused;
    void *top[4];
    uint32_t topn[4];
    bool exec;
} execmap_heap;
typedef struct { execmap_heap h[12]; unsigned n; bool mark; } execmap_t;
static execmap_heap *execmap_find(execmap_t *m,intptr_t start,intptr_t end){
    for(unsigned k=0;k<m->n;k++)if(m->h[k].start==start)return &m->h[k];
    if(m->n==sizeof m->h/sizeof m->h[0])return NULL;
    execmap_heap *h=&m->h[m->n++];
    memset(h,0,sizeof *h);
    h->start=start;h->end=end;
    return h;
}
/* heap_caps_walk holds the heap lock while it calls this: count only. */
static bool execmap_walk(walker_heap_into_t heap,walker_block_info_t block,void *ud){
    execmap_t *m=ud;
    execmap_heap *h=execmap_find(m,heap.start,heap.end);
    if(!h)return true;
    if(m->mark){h->exec=true;return true;}
    const uint32_t n=(uint32_t)block.size;
    if(block.used){
        h->used+=n;h->nused++;
        for(unsigned k=0;k<4;k++)if(n>h->topn[k]){
            for(unsigned q=3;q>k;q--){h->topn[q]=h->topn[q-1];h->top[q]=h->top[q-1];}
            h->topn[k]=n;h->top[k]=block.ptr;break;
        }
    }else{
        h->free+=n;h->nfree++;
        if(n>h->largest)h->largest=n;
    }
    return true;
}
static const char *execmap_kind(intptr_t a){
    if(a>=0x3FC88000&&a<0x3FCF0000)return "diram";
    if(a>=0x3FCF0000&&a<0x3FD00000)return "dram";
    if(a>=0x40370000&&a<0x403E0000)return "iram";
    if(a>=0x600FE000&&a<0x60100000)return "rtc";
    return "other";
}
static void execmap_report(const char *why){
    static int64_t last;
    const int64_t now=esp_timer_get_time();
    if(last&&now-last<3000000)return;
    last=now;
    static execmap_t m;                 /* 600 B: not on the UI task's stack */
    memset(&m,0,sizeof m);
    m.mark=true;heap_caps_walk(MALLOC_CAP_EXEC,execmap_walk,&m);
    m.mark=false;heap_caps_walk_all(execmap_walk,&m);
    const void *sr;size_t sn;
    shiki_heap_region(&sr,&sn);
    ESP_LOGI("jit","EXECMAP why=%s exec_free=%u exec_largest=%u int_free=%u int_largest=%u shiki=%p/%u jit=%u got=%u/%u/%u evict=%u fail=%u/%u/%u",
             why,(unsigned)heap_caps_get_free_size(MALLOC_CAP_EXEC),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_EXEC),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             sr,(unsigned)sn,(unsigned)ksn_shiki_jit_bytes,
             (unsigned)execmap_where[0],(unsigned)execmap_where[1],(unsigned)execmap_where[2],
             (unsigned)ksn_shiki_jit_evicted,
             (unsigned)ksn_shiki_jit_fail[0],(unsigned)ksn_shiki_jit_fail[1],(unsigned)ksn_shiki_jit_fail[2]);
    for(unsigned k=0;k<m.n;k++){
        const execmap_heap *h=&m.h[k];
        ESP_LOGI("jit","EXECMAP heap=%p-%p %s exec=%d size=%u free=%u/%u largest=%u used=%u/%u top=%p/%u,%p/%u,%p/%u,%p/%u",
                 (void *)h->start,(void *)h->end,execmap_kind(h->start),h->exec?1:0,
                 (unsigned)(h->end-h->start),(unsigned)h->free,(unsigned)h->nfree,(unsigned)h->largest,
                 (unsigned)h->used,(unsigned)h->nused,
                 h->top[0],(unsigned)h->topn[0],h->top[1],(unsigned)h->topn[1],
                 h->top[2],(unsigned)h->topn[2],h->top[3],(unsigned)h->topn[3]);
    }
}
#define EXECMAP_REPORT(why) execmap_report(why)
#define EXECMAP_GOT(x,region) (execmap_where[(region)?2:esp_ptr_in_diram_iram(x)?0:1]++)
#else
#define EXECMAP_REPORT(why) ((void)0)
#define EXECMAP_GOT(x,region) ((void)0)
#endif
typedef struct {
    uint8_t *raw,*exec;           /* the allocation, and the code in it (16-aligned) */
    unsigned n;
    ksn_jit_section s[KSN_JIT_SECTIONS];
    uint8_t *sraw;                /* stage 3: the plan's scalar code (allocation, entry) */
    uint32_t (*scalar)(ksn_jit_run *);
    uint32_t sbytes;
    int32_t *weights;             /* sec.13: CUBIC's weights, or NULL */
    uint32_t wbytes;
    /* sec.15: sraw and weights are blocks of the Shiki region (data-bus
     * addresses; the code runs at the instruction-bus alias), given back
     * when the region's own users need the room. Such plans are listed. */
    bool sregion,wregion;
    ksn_shiki_plan *plan;
    void *next;
    uint32_t last;                /* jit_clock at its last draw (sec.16) */
    /* sec.18: JIT_SAT_LOG, the code calls the shared checker and its slots
     * log; JIT_SAT_DECIDED, every site is settled. plain: sites that lost
     * their check (patched to j over the slot); fast: sites that keep it,
     * pointed at the checker's entry without the log. A later compile
     * (after a drop) of a decided plan emits plain sites plain and the rest
     * inline. draws: draws since the code was compiled. */
    uint8_t sat;
    uint16_t draws;
    int32_t alias;                /* the code's data-bus address minus its instruction-bus one */
    uint8_t *sdata;               /* the code's start on the data bus */
    uint64_t plain,fast;
} jit_plan;
enum { JIT_SAT_NONE, JIT_SAT_LOG, JIT_SAT_DECIDED };
static unsigned count_sites(const ksn_shiki_plan *plan){
    unsigned n=0;
    for(unsigned pc=0;pc<plan->count;pc++)n+=plan->code[pc].op==KSN_SHIKI_ADD||plan->code[pc].op==KSN_SHIKI_SUB;
    return n;
}
#define JIT_WEIGHTS_MAX 512u     /* int32s: 2 KB, e.g. 8 CUBICs of 16 segments */
/* Copies len bytes of code into a new executable allocation, as 32-bit
 * words (the instruction bus takes only word stores); *raw is what to free.
 * The code starts 16-aligned (the VEACH loops count on it). */
static uint8_t *to_exec(uint8_t *tmp,size_t n,uint8_t **raw,size_t *bytes){
    const size_t len=(n+3u)&~(size_t)3u;
    *raw=heap_caps_malloc(len+16u,MALLOC_CAP_EXEC);
    if(!*raw)return NULL;
    *bytes=len+16u;
    uint8_t *x=(uint8_t *)(((uintptr_t)*raw+15u)&~(uintptr_t)15u);
    volatile uint32_t *w=(volatile uint32_t *)(esp_ptr_in_diram_iram(x)?esp_ptr_diram_iram_to_dram(x):(void *)x);
    memset(tmp+n,0,len-n);
    for(size_t k=0;k<len/4;k++)
        w[k]=(uint32_t)tmp[4*k]|(uint32_t)tmp[4*k+1]<<8|(uint32_t)tmp[4*k+2]<<16|(uint32_t)tmp[4*k+3]<<24;
    __asm__ volatile("memw\n" "isync\n":::"memory");
    return x;
}
#define JIT_SCRATCH 2048u        /* 4 sections of at most ~400 B each */

void ksn_shiki_jit_attach(ksn_shiki_plan *plan){
    plan->jit=NULL;
    bool any=false;
    for(unsigned pc=0;pc<plan->count;pc++)any|=plan->code[pc].op==KSN_SHIKI_VEACH;
    if(!any)return;
    uint8_t *tmp=malloc(JIT_SCRATCH);
    jit_plan *j=calloc(1,sizeof *j);
    if(!tmp||!j){free(tmp);free(j);return;}
    ksn_jit_buf b={tmp,0,JIT_SCRATCH,false,false,0,0,0,0,NULL,0};
    j->n=ksn_shiki_jit_plan(plan,j->s,KSN_JIT_SECTIONS,&b);
    if(!j->n){free(tmp);free(j);return;}
    size_t bytes;
    j->exec=to_exec(tmp,b.len,&j->raw,&bytes);
    free(tmp);
    if(!j->exec){free(j);return;}                /* no code: the interpreter runs it */
    plan->jit=j;
}
/* Plans with scalar code (sec.15, sec.16), wherever it lives. */
static jit_plan *jit_plans;
static void unlist(jit_plan *j){
    for(jit_plan **q=&jit_plans;*q;q=(jit_plan **)&(*q)->next)
        if(*q==j){*q=j->next;j->next=NULL;return;}
}
/* Gives back the plan's scalar code and weights (the interpreter runs it
 * until it is compiled again). */
static void drop_scalar(jit_plan *j){
    if(j->sraw){
        if(j->sregion)shiki_heap_free(j->sraw);else heap_caps_free(j->sraw);
        ksn_shiki_jit_bytes-=j->sbytes;
    }
    if(j->weights){
        if(j->wregion)shiki_heap_free(j->weights);else free(j->weights);
        ksn_shiki_jit_bytes-=j->wbytes;
    }
    j->sraw=NULL;j->scalar=NULL;j->sbytes=0;j->weights=NULL;j->wbytes=0;
    j->sregion=j->wregion=false;
    unlist(j);
}
void ksn_shiki_jit_detach(ksn_shiki_plan *plan){
    jit_plan *j=plan?plan->jit:NULL;
    if(!j)return;
    if(j->raw)heap_caps_free(j->raw);
    drop_scalar(j);
    free(j);
    plan->jit=NULL;
}

/* ---- Stage 3 at run time ----------------------------------------------------
 * A plan is compiled on its JIT_HEAT-th draw (a plan drawn once, a title's,
 * never costs memory). A plan the compiler refuses stays with the
 * interpreter; one that met a full budget or heap is tried again later
 * (JIT_RETRY). The budget bounds the
 * executable memory taken from the heap the app shares (shiki-jit.md
 * sec.11). 4, not 32: MEGADEMO registers a scene's plans while it plays
 * (TWIST's negative set from frame 60, drawn from 122 for 74 frames), and
 * at one draw a frame 32 left half of that scene interpreted (sec.14). */
#define JIT_HEAT 4u
#define JIT_SCALAR_SCRATCH 4096u

/* Why a compile did not happen (ksn_shiki_jit_fail[]): the compiler refused
 * the plan (never retried), the budget was full, or the heap was (both
 * retried after JIT_RETRY more draws: a scene's plans come and go). */
enum { JIT_DONE=-1, JIT_REFUSED, JIT_BUDGET, JIT_NOMEM };
#define JIT_RETRY 64
/* The code a plan of n instructions takes at most, for the budget check
 * that spares a compile when it would not fit (sec.11: ~22-25 B each). */
#define JIT_GUESS(n) ((uint32_t)(n)*32u+64u)

/* sec.15: the region's free room the JIT leaves to the region's own users,
 * so that a registration rarely has to take the JIT's blocks back. */
#define JIT_REGION_KEEP 2048u
static int jit_running;           /* inside the JIT's code: nothing is dropped */
static uint32_t region_largest(void){
    uint32_t bytes,used,largest;
    shiki_heap_info(&bytes,&used,&largest);
    return largest;
}
/* The Shiki region's reclaim hook: one of its own allocations failed, so
 * every plan with region blocks gives them back and waits JIT_RETRY draws. */
static void jit_reclaim(void){
    if(jit_running)return;
    for(jit_plan *j=jit_plans,*n;j;j=n){
        n=j->next;
        if(!j->sregion&&!j->wregion)continue;
        ksn_shiki_plan *p=j->plan;
        drop_scalar(j);
        if(p){p->heat=(int8_t)((int)JIT_HEAT-JIT_RETRY);p->need64=0;}
        ksn_shiki_jit_evicted++;
    }
}

/* sec.16: code of plans not drawn lately makes room for a plan being drawn.
 * DERBY registers every camera's plans for the race, and the memory filled
 * up with the code of cameras already cut away from, while the later ones
 * ran interpreted. jit_clock counts draws of compiled plans; a plan not
 * drawn in the last JIT_COLD_DRAWS (about 1 s of DERBY's race, 5 s of a
 * MEGADEMO scene) is cold. */
#define JIT_COLD_DRAWS 1024u
static uint32_t jit_clock;
/* need bytes of code fit the budget and the region (keeping its share)
 * or the system heap's executable memory. */
static bool fits(size_t need){
    if(ksn_shiki_jit_bytes+need+20u>ksn_shiki_jit_budget)return false;
    return region_largest()>=need+JIT_REGION_KEEP||
           heap_caps_get_largest_free_block(MALLOC_CAP_EXEC)>=need+16u;
}
/* Drops cold plans' code, coldest first, until need fits. A dropped plan
 * starts again from heat 0: drawn again, it is compiled on its 4th draw. */
static bool make_room(size_t need){
    if(jit_running)return false;
    while(!fits(need)){
        jit_plan *cold=NULL;
        for(jit_plan *j=jit_plans;j;j=j->next)
            if(jit_clock-j->last>JIT_COLD_DRAWS&&(!cold||(int32_t)(j->last-cold->last)<0))cold=j;
        if(!cold)return false;
        ksn_shiki_plan *p=cold->plan;
        drop_scalar(cold);
        if(p){p->heat=0;p->need64=0;}
        ksn_shiki_jit_cold++;
    }
    return true;
}

/* CUBIC's weights (sec.13); without them the code still works (the helper
 * computes them), so a refusal here is not a failure. code: the code's bytes,
 * for the budget. */
static void attach_weights(jit_plan *j,const ksn_shiki_plan *plan,size_t code){
    size_t rows=0;
    for(unsigned pc=0;pc<plan->count;pc++)if(plan->code[pc].op==KSN_SHIKI_CUBIC)rows+=4u*(size_t)plan->code[pc].imm;
    if(!rows||rows>JIT_WEIGHTS_MAX||ksn_shiki_jit_bytes+code+20u+4u*rows>ksn_shiki_jit_budget)return;
    /* The region first (sec.15), then the system heap. */
    j->weights=shiki_heap_spare(4u*rows,JIT_REGION_KEEP);
    j->wregion=j->weights!=NULL;
    if(!j->weights)j->weights=malloc(4u*rows);
    if(!j->weights)return;
    ksn_shiki_cubic_weights(plan,j->weights,rows);
    j->wbytes=(uint32_t)(4u*rows);
    ksn_shiki_jit_bytes+=j->wbytes;             /* drop_scalar takes it back */
}
/* The plan's code is at x (instruction bus), bytes long, entry in: the plan
 * runs it from its next draw. */
static int compiled(ksn_shiki_plan *plan,jit_plan *j,uint8_t *x,size_t entry,size_t bytes,
                    uint8_t *data,bool checker){
    __asm__ volatile("memw\n" "isync\n":::"memory");
    j->sdata=data;
    j->alias=(int32_t)((intptr_t)data-(intptr_t)x);
    j->draws=0;
    if(j->sat!=JIT_SAT_DECIDED){j->plain=j->fast=0;}
    if(j->sat!=JIT_SAT_DECIDED)j->sat=checker&&count_sites(plan)?JIT_SAT_LOG:JIT_SAT_NONE;
    j->scalar=(uint32_t (*)(ksn_jit_run *))(void *)(x+entry);
    j->sbytes=(uint32_t)bytes;
    ksn_shiki_jit_bytes+=(uint32_t)bytes;
    j->plan=plan;
    j->last=jit_clock;
    j->next=jit_plans;jit_plans=j;
    shiki_heap_set_reclaim(jit_reclaim);
    plan->jit=j;
    return JIT_DONE;
}
static bool in_diram(const uint8_t *p,size_t n){
    return esp_ptr_in_diram_dram(p)&&esp_ptr_in_diram_dram(p+n-1);
}

/* sec.18: how a plan's ADD/SUB sites are emitted. Decided: the sites the
 * logs cleared plain, the rest inline (no logging any more). Otherwise the
 * shared checker, when the code will run on DIRAM (org: its instruction-bus
 * address; elsewhere call0 does not reach the checker). */
static void sat_mode(const jit_plan *j,ksn_jit_buf *b,uint32_t org,bool checker){
    if(j&&j->sat==JIT_SAT_DECIDED){b->plain=j->plain;return;}
    if(!checker)return;
    b->org=org;
    b->chk_add=(uint32_t)(uintptr_t)ksn_jit_chk_add;
    b->chk_sub=(uint32_t)(uintptr_t)ksn_jit_chk_sub;
}
/* sec.17: in the Shiki region, one pass. A block as long as the guess
 * (JIT_GUESS), the code written once through the data bus, the end given
 * back. Returns JIT_DONE, JIT_REFUSED, or -2 when the region has no room
 * or the guess was short (the two-pass path below then runs). */
static int compile_in_region(ksn_shiki_plan *plan){
    const size_t guess=JIT_GUESS(plan->count);
    uint8_t *d=shiki_heap_spare(guess,JIT_REGION_KEEP);
    if(!d)return -2;
    if(!in_diram(d,guess)){shiki_heap_free(d);return -2;}
    jit_plan *j=plan->jit;
    const bool fresh=!j;
    if(fresh&&!(j=calloc(1,sizeof *j))){shiki_heap_free(d);return -2;}
    ksn_jit_buf e={d,0,guess,false,false,0,0,0,0,NULL,0};
    sat_mode(j,&e,(uint32_t)(uintptr_t)esp_ptr_diram_dram_to_iram(d),true);
    size_t entry=0;
    if(!ksn_shiki_jit_scalar(plan,&e,&entry)){
        shiki_heap_free(d);
        if(fresh)free(j);
        return e.overflow?-2:JIT_REFUSED;
    }
    const size_t len=(e.len+3u)&~(size_t)3u;
    uint8_t *q=shiki_heap_shrink(d,len);
    /* TLSF trims in place; if the block ever moved, the code moved with it
     * (it addresses only relative to itself and through the context). */
    if(!q||!in_diram(q,len)){
        if(q)shiki_heap_free(q);
        if(fresh)free(j);
        return -2;
    }
    j->sraw=q;j->sregion=true;
    attach_weights(j,plan,len);
    EXECMAP_GOT(q,true);
    return compiled(plan,j,(uint8_t *)esp_ptr_diram_dram_to_iram(q),entry,len,q,e.chk_add!=0);
}

/* The two-pass path: a sizing pass (no buffer) gives the length, the
 * allocation is that long (the region, or MALLOC_CAP_EXEC), the second pass
 * writes. No scratch buffer: MEGADEMO compiles TWIST's negative set at its
 * tightest heap, where a 4 KB scratch did not fit (sec.14). */
static int compile_scalar(ksn_shiki_plan *plan){
    if(ksn_shiki_jit_bytes+JIT_GUESS(plan->count)>ksn_shiki_jit_budget&&!make_room(JIT_GUESS(plan->count)))
        return JIT_BUDGET;
    /* A plan whose allocation failed before is not sized again while the
     * heap still has no block that long: the sizing pass costs as much as
     * the compile, and DERBY's WIDE retried a dozen plans a second at its
     * tightest heap, which made the JIT slower than the interpreter (sec.14). */
    if(plan->need64&&!fits(64u*plan->need64)&&!make_room(64u*plan->need64))return JIT_NOMEM;
    const int r=compile_in_region(plan);
    if(r!=-2)return r;
    ksn_jit_buf b={NULL,0,JIT_SCALAR_SCRATCH,false,false,0,0,0,0,NULL,0};
    sat_mode(plan->jit,&b,0,true);                /* sized for DIRAM: the shared checker */
    size_t entry=0;
    if(!ksn_shiki_jit_scalar(plan,&b,&entry))return JIT_REFUSED;
    size_t need=b.len;
    if(!fits(need)&&!make_room(need)){
        if(ksn_shiki_jit_bytes+need+20u>ksn_shiki_jit_budget)return JIT_BUDGET;
        const size_t est=need+16u;
        plan->need64=(uint8_t)(est/64u>255u?255u:est/64u);
        return JIT_NOMEM;
    }
    jit_plan *j=plan->jit;
    const bool fresh=!j;
    if(fresh&&!(j=calloc(1,sizeof *j)))return JIT_NOMEM;
    attach_weights(j,plan,need);
    size_t len=(need+3u)&~(size_t)3u;
    /* The code: in the Shiki region when it has room (sec.15: DIRAM, so
     * its blocks run at their instruction-bus alias; the region is taken
     * at the app's start, before the guest scatters its blocks over DIRAM),
     * else MALLOC_CAP_EXEC from the system heap. */
    size_t bytes=len;
    uint8_t *x=NULL,*w=NULL;
    j->sraw=shiki_heap_spare(len,JIT_REGION_KEEP);
    if(j->sraw&&!in_diram(j->sraw,len)){shiki_heap_free(j->sraw);j->sraw=NULL;}
    if(j->sraw){
        j->sregion=true;
        w=j->sraw;
        x=(uint8_t *)esp_ptr_diram_dram_to_iram(w);
    }else{
        bytes=len+16u;
        j->sraw=heap_caps_malloc(bytes,MALLOC_CAP_EXEC);
        x=j->sraw?(uint8_t *)(((uintptr_t)j->sraw+15u)&~(uintptr_t)15u):NULL;
    }
    if(!x){
        drop_scalar(j);if(fresh)free(j);
        plan->need64=(uint8_t)(bytes/64u>255u?255u:bytes/64u);
        return JIT_NOMEM;
    }
    /* DIRAM: write bytes through the data alias. Otherwise (RTC fast memory,
     * where a tight heap puts MALLOC_CAP_EXEC: sec.15) write whole words. */
    bool diram=j->sregion||esp_ptr_in_diram_iram(x);
    if(!diram&&b.chk_add){
        /* sec.18: RTC fast memory is beyond call0's reach of the checker, so
         * the code there has the inline checks, which are longer: the block
         * is taken again at that length. */
        ksn_jit_buf c={NULL,0,JIT_SCALAR_SCRATCH,false,false,0,0,0,0,NULL,0};
        sat_mode(j,&c,0,false);
        if(!ksn_shiki_jit_scalar(plan,&c,&entry)){drop_scalar(j);if(fresh)free(j);return JIT_REFUSED;}
        need=c.len;
        if(need>len){
            heap_caps_free(j->sraw);
            len=(need+3u)&~(size_t)3u;
            bytes=len+16u;
            j->sraw=heap_caps_malloc(bytes,MALLOC_CAP_EXEC);
            x=j->sraw?(uint8_t *)(((uintptr_t)j->sraw+15u)&~(uintptr_t)15u):NULL;
            if(!x){
                drop_scalar(j);if(fresh)free(j);
                plan->need64=(uint8_t)(bytes/64u>255u?255u:bytes/64u);
                return JIT_NOMEM;
            }
            diram=esp_ptr_in_diram_iram(x);
        }
        b.chk_add=0;                          /* inline, wherever it landed now */
    }
    EXECMAP_GOT(x,j->sregion);
    if(!w)w=diram?(uint8_t *)esp_ptr_diram_iram_to_dram(x):x;
    for(size_t k=0;k<len/4;k++)((volatile uint32_t *)(void *)w)[k]=0;
    ksn_jit_buf e={w,0,need,false,!diram,0,0,0,0,NULL,0};
    sat_mode(j,&e,(uint32_t)(uintptr_t)x,diram&&b.chk_add);
    size_t entry2=0;
    if(!ksn_shiki_jit_scalar(plan,&e,&entry2)||e.len!=need||entry2!=entry){
        drop_scalar(j);if(fresh)free(j);return JIT_REFUSED;
    }
    return compiled(plan,j,x,entry,bytes,w,e.chk_add!=0);
}
/* sec.18: the logs are read twice, early (the checker with its log costs
 * ~4x the inline check, so the sooner a site leaves it the better):
 * - at JIT_SAT_EARLY draws a site whose results used at most
 *   31 - JIT_SAT_ROOM_EARLY bits loses its check (its call0 becomes j over
 *   the slot), and one that came within JIT_SAT_NEAR bits of INT32_MAX (or
 *   saturated) keeps it, without the log (call0 to the fast entry);
 * - at JIT_SAT_LATE draws the rest are settled with JIT_SAT_ROOM bits of
 *   room; a site that never ran keeps its check, without the log.
 * Values are risk, not proof: a site cleared here wraps if a later draw
 * overflows it (docs/kasane/shiki-jit.md sec.18). */
#define JIT_SAT_EARLY 16u
#define JIT_SAT_LATE 64u
#define JIT_SAT_ROOM_EARLY 12u
#define JIT_SAT_ROOM 8u
#define JIT_SAT_NEAR 3u
/* Rewrites the 3 bytes at the site: j over the slot, or call0 to `to`. */
static void sat_patch(jit_plan *j,unsigned at,const uint8_t *to){
    ksn_jit_buf q={j->sdata,at,at+3u,false,false,0,0,0,0,NULL,0};
    if(!to){ksn_jit_emit(&q,KSN_JIT_J,3,0,0);return;}
    const uint32_t pc=(uint32_t)(uintptr_t)j->sdata-(uint32_t)j->alias+at;
    ksn_jit_emit(&q,KSN_JIT_CALL0,(int)(((int32_t)(uintptr_t)to-(int32_t)(pc&~3u)-4)/4),0,0);
}
static void sat_pass(const ksn_shiki_plan *plan,jit_plan *j,bool late){
    uint16_t sites[KSN_SHIKI_CODE];
    ksn_jit_buf b={NULL,0,JIT_SCALAR_SCRATCH,false,false,0,0,0,0,NULL,0};
    sat_mode(j,&b,0,true);
    b.sites=sites;
    size_t entry=0;
    if(!ksn_shiki_jit_scalar(plan,&b,&entry)){j->sat=JIT_SAT_DECIDED;return;}   /* cannot happen: it compiled */
    /* The SHIKI_SAT line: per site, in program order, the bits its results
     * used (0-9, a-v for 10-31; '-' never ran); fnv names the plan (FNV-1a
     * of its instructions, as a host tool can compute from the ROM). */
    char bits[65];
    unsigned out=0,kept=0,never=0,open=0,n=0;
    uint64_t subs=0;                                   /* which sites are SUB */
    for(unsigned pc=0;pc<plan->count&&n<64u;pc++){
        const uint8_t op=plan->code[pc].op;
        if(op==KSN_SHIKI_SUB)subs|=1ull<<n;
        n+=op==KSN_SHIKI_ADD||op==KSN_SHIKI_SUB;
    }
    for(unsigned k=0;k<b.nsites&&k<64u;k++){
        const uint32_t w=*(volatile const uint32_t *)(const void *)(j->sdata+sites[k]+3u);
        const unsigned used=w?32u-(unsigned)__builtin_clz(w):0u;
        bits[k]=(w&JIT_SLOT_NEVER)?'-':(char)(used<10u?'0'+used:'a'+used-10u);
        bits[k+1]=0;
        if((j->plain|j->fast)>>k&1u)continue;          /* settled early */
        bool clear,keep;
        if(w&JIT_SLOT_NEVER){clear=false;keep=late;if(late){ksn_shiki_jit_sat[2]++;never++;}}
        else if(late){clear=used<=31u-JIT_SAT_ROOM;keep=!clear;}
        else{clear=used<=31u-JIT_SAT_ROOM_EARLY;keep=used>=31u-JIT_SAT_NEAR;}
        if(clear){
            sat_patch(j,sites[k],NULL);
            j->plain|=1ull<<k;ksn_shiki_jit_sat[0]++;out++;
        }else if(keep){
            sat_patch(j,sites[k],subs>>k&1u?ksn_jit_chk_sub_fast:ksn_jit_chk_add_fast);
            j->fast|=1ull<<k;
            if(!(w&JIT_SLOT_NEVER)){ksn_shiki_jit_sat[1]++;kept++;}
        }else open++;
    }
    __asm__ volatile("memw\n" "isync\n":::"memory");
    if(late||!open)j->sat=JIT_SAT_DECIDED;
    uint32_t fnv=2166136261u;
    const uint8_t *q=(const uint8_t *)plan->code;
    for(size_t k=0;k<plan->count*sizeof plan->code[0];k++)fnv=(fnv^q[k])*16777619u;
    ESP_LOGI("kasane","SHIKI_SAT fnv=%08x n=%u at=%u out=%u kept=%u never=%u open=%u bits=%s",
             (unsigned)fnv,(unsigned)plan->count,(unsigned)j->draws,out,kept,never,open,b.nsites?bits:"");
}
bool ksn_shiki_jit_compile_now(ksn_shiki_plan *plan){
    const jit_plan *j=plan->jit;
    if(j&&j->scalar)return true;
    plan->heat=INT8_MIN;
    return compile_scalar(plan)==JIT_DONE;
}
uint32_t ksn_shiki_jit_run(const ksn_shiki_plan *plan,const int32_t *in,unsigned index,
                           ksn_shiki_vm *vm,ksn_proc_segment *out){
    if(!ksn_shiki_jit_scalar_on)return 0;
    /* The plan's memory is the caller's own copy (never flash), so the JIT
     * may keep its heat and code pointer in it. */
    ksn_shiki_plan *p=(ksn_shiki_plan *)plan;
    const jit_plan *j=p->jit;
    if(!j||!j->scalar){
        /* heat: draws so far; INT8_MIN never again; negative: waiting
         * to retry after a full budget or heap. */
        if(p->heat==INT8_MIN||++p->heat<(int8_t)JIT_HEAT)return 0;
#ifdef POCKET_SHIKI_JIT_EXECMAP
        static bool first=true;
        if(first){first=false;EXECMAP_REPORT("first");}
#endif
        const uint32_t t0=esp_cpu_get_cycle_count();
        const int r=compile_scalar(p);
        ksn_shiki_jit_compile_cy+=esp_cpu_get_cycle_count()-t0;
        ksn_shiki_jit_compiles++;
        if(r!=JIT_DONE){
            ksn_shiki_jit_fail[r]++;
            if(r==JIT_NOMEM)EXECMAP_REPORT("nomem");
            p->heat=r==JIT_REFUSED?INT8_MIN:(int8_t)((int)JIT_HEAT-JIT_RETRY);
            return 0;
        }
        p->heat=INT8_MIN;
        j=p->jit;
    }
    ksn_jit_run c={vm,in,KSN_SHIKI_STEPS,(uint32_t (*)(const void *,uint32_t))ksn_shiki_jit_leaf,
                   (uint32_t (*)(const void *,int32_t,int32_t,int32_t,uint32_t))ksn_shiki_jit_draw,
                   ksn_shiki_sin_table,
                   (uint32_t (*)(const void *,int32_t,int32_t,uint32_t))ksn_shiki_jit_curve,
                   0,j->alias,j->weights,plan,out,index};
    ((jit_plan *)j)->last=++jit_clock;
    jit_running++;
    const uint32_t r=j->scalar(&c);
    jit_running--;
    if(j->sat==JIT_SAT_LOG){
        const uint16_t d=++((jit_plan *)j)->draws;
        if(d==JIT_SAT_EARLY||d==JIT_SAT_LATE)sat_pass(p,(jit_plan *)j,d==JIT_SAT_LATE);
    }
    return r;
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
