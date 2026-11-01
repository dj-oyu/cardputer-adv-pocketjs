#include "ksn_shiki_bench.h"
#include <string.h>

/* The pairs (shiki-pie.md sec.6 items 1 and 2):
 *   affine128: 128 points (i16, x and y in -100..100) rotated by 30 degrees
 *     in Q14 and moved to (120, 67), drawn as points. Scalar: EACH + FETCH +
 *     MUL/ADD/SHIFT + PLOT per record. Vector: VEACH over blocks of 8 into a
 *     second region, then one EMIT_POINTS.
 *   lines100: 100 short lines from an i16 region (x0, y0, x1, y1). Scalar:
 *     EACH + FETCH + MOVE + LINE. Batch: one EMIT_LINES.
 * The arithmetic is chosen so both forms are exact: products fit int32,
 * results fit int16 and the screen, so the vector path's sat16 never acts
 * and both round the same way (floor by 2^14, then whole pixels). */

enum { A_COS=14189, A_SIN=8192, TX=120, TY=67, COLOUR=2016, POINTS=128, LINES=100 };

typedef struct { uint8_t *code; unsigned n; } asm_t;
static void op(asm_t *s,uint8_t o,uint8_t dst,uint8_t a,uint8_t b,int32_t imm,uint16_t aux){
    uint8_t *p=s->code+12*s->n++;
    const uint32_t u=(uint32_t)imm;
    const uint8_t w[12]={o,dst,a,b,(uint8_t)u,(uint8_t)(u>>8),(uint8_t)(u>>16),(uint8_t)(u>>24),
                         (uint8_t)aux,(uint8_t)(aux>>8),0,0};
    memcpy(p,w,sizeof w);
}
/* Header and region table for ndata regions {type, q, k, write}; returns
 * the assembler positioned at the body. */
static asm_t begin(ksn_shiki_bench_case *c,const uint8_t (*decl)[4],unsigned ndata){
    memset(c->code,0,sizeof c->code);
    const uint8_t head[12]={'S','K','I','1',1,0,0,0,0,(uint8_t)ndata,0,0};
    memcpy(c->code,head,12);
    for(unsigned k=0;k<ndata;k++)memcpy(c->code+12+4*k,decl[k],4);
    return (asm_t){c->code+12+4*ndata,0};
}
static void finish(ksn_shiki_bench_case *c,asm_t *s,unsigned ndata){
    c->code[6]=(uint8_t)s->n;
    c->len=12+4*ndata+12*(size_t)s->n;
}
/* The constants both affine forms set first: r10 = cos, r11 = sin,
 * r12 = -sin, r13 = TX << 14, r14 = TY << 14 (the accumulator's origin). */
static void affine_consts(asm_t *s){
    op(s,KSN_SHIKI_SET,10,0,0,A_COS,0);
    op(s,KSN_SHIKI_SET,11,0,0,A_SIN,0);
    op(s,KSN_SHIKI_SET,12,0,0,-A_SIN,0);
    op(s,KSN_SHIKI_SET,13,0,0,TX<<14,0);
    op(s,KSN_SHIKI_SET,14,0,0,TY<<14,0);
}
static uint32_t next(uint32_t *st){*st=*st*1664525u+1013904223u;return *st>>8;}

unsigned ksn_shiki_bench_count(void){return 4;}

bool ksn_shiki_bench_build(unsigned i,int16_t work[KSN_SHIKI_BENCH_WORK],ksn_shiki_bench_case *c){
    if(i>=4)return false;
    memset(c,0,sizeof *c);
    uint32_t st=12345;
    c->pair=i/2;
    if(i<2){
        int16_t *pts=work,*out=work+2*POINTS;
        for(unsigned k=0;k<2*POINTS;k++)pts[k]=(int16_t)((int32_t)(next(&st)%201)-100);
        memset(out,0,2*POINTS*sizeof *out);
        const uint8_t decl[2][4]={{KSN_SHIKI_I16,0,2,0},{KSN_SHIKI_I16,0,2,1}};
        c->region[0]=(ksn_shiki_region){pts,2*POINTS,KSN_SHIKI_I16,0,2};
        c->region[1]=(ksn_shiki_region){out,2*POINTS,KSN_SHIKI_I16,0,2};
        if(i==0){
            c->name="affine128 scalar";
            c->regions=1;
            asm_t s=begin(c,decl,1);
            affine_consts(&s);
            op(&s,KSN_SHIKI_EACH,0,0,0,0,0);
            op(&s,KSN_SHIKI_FETCH,0,0,0,0,2);             /* r0 = x, r1 = y */
            op(&s,KSN_SHIKI_MUL,2,0,10,0,0);
            op(&s,KSN_SHIKI_MUL,3,1,12,0,0);
            op(&s,KSN_SHIKI_ADD,2,2,3,0,0);
            op(&s,KSN_SHIKI_ADD,2,2,13,0,0);
            op(&s,KSN_SHIKI_SHIFT,2,2,0,-14,0);
            op(&s,KSN_SHIKI_SHIFT,2,2,0,16,0);
            op(&s,KSN_SHIKI_MUL,4,0,11,0,0);
            op(&s,KSN_SHIKI_MUL,5,1,10,0,0);
            op(&s,KSN_SHIKI_ADD,4,4,5,0,0);
            op(&s,KSN_SHIKI_ADD,4,4,14,0,0);
            op(&s,KSN_SHIKI_SHIFT,4,4,0,-14,0);
            op(&s,KSN_SHIKI_SHIFT,4,4,0,16,0);
            op(&s,KSN_SHIKI_PLOT,0,2,4,0,COLOUR);
            op(&s,KSN_SHIKI_END,0,0,0,0,0);
            finish(c,&s,1);
        }else{
            c->name="affine128 vector";
            c->regions=2;
            asm_t s=begin(c,decl,2);
            affine_consts(&s);
            op(&s,KSN_SHIKI_VEACH,0,0,0,0,0);
            op(&s,KSN_SHIKI_VLOAD,0,0,0,0,0);             /* v0 = x */
            op(&s,KSN_SHIKI_VLOAD,1,0,1,0,0);             /* v1 = y */
            op(&s,KSN_SHIKI_VACCSET,0,13,0,0,0);
            op(&s,KSN_SHIKI_VMACK,0,0,10,0,0);
            op(&s,KSN_SHIKI_VMACK,0,1,12,0,0);
            op(&s,KSN_SHIKI_VOUT,2,0,0,0,14);
            op(&s,KSN_SHIKI_VSTORE,2,1,0,0,0);
            op(&s,KSN_SHIKI_VACCSET,0,14,0,0,0);
            op(&s,KSN_SHIKI_VMACK,0,0,11,0,0);
            op(&s,KSN_SHIKI_VMACK,0,1,10,0,0);
            op(&s,KSN_SHIKI_VOUT,3,0,0,0,14);
            op(&s,KSN_SHIKI_VSTORE,3,1,1,0,0);
            op(&s,KSN_SHIKI_END,0,0,0,0,0);
            op(&s,KSN_SHIKI_EMIT_POINTS,0,1,0,0,COLOUR);
            finish(c,&s,2);
        }
        return true;
    }
    int16_t *ln=work;
    for(unsigned k=0;k<LINES;k++){
        const int16_t x=(int16_t)(next(&st)%240),y=(int16_t)(next(&st)%135);
        ln[4*k]=x;ln[4*k+1]=y;
        ln[4*k+2]=(int16_t)(x+(int32_t)(next(&st)%17)-8);
        ln[4*k+3]=(int16_t)(y+(int32_t)(next(&st)%17)-8);
    }
    const uint8_t decl[1][4]={{KSN_SHIKI_I16,0,4,0}};
    c->region[0]=(ksn_shiki_region){ln,4*LINES,KSN_SHIKI_I16,0,4};
    c->regions=1;
    asm_t s=begin(c,decl,1);
    if(i==2){
        c->name="lines100 scalar";
        op(&s,KSN_SHIKI_EACH,0,0,0,0,0);
        op(&s,KSN_SHIKI_FETCH,0,0,0,16,4);                /* r0..r3 in Q16 */
        op(&s,KSN_SHIKI_MOVE,0,0,1,0,0);
        op(&s,KSN_SHIKI_LINE,0,2,3,0,COLOUR);
        op(&s,KSN_SHIKI_END,0,0,0,0,0);
    }else{
        c->name="lines100 emit";
        op(&s,KSN_SHIKI_EMIT_LINES,0,0,0,0,COLOUR);
    }
    finish(c,&s,1);
    return true;
}
