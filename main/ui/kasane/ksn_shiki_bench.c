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
static int rnd2(uint32_t *st){return (int)(next(st)&1u);}

/* Regions are field planes (spec v1.4 sec.5.1): x of every record, then y,
 * each plane S = records rounded up to 8 long. pair 0 is affine128, 1 the
 * lines; both cases of a pair draw the same segments. */
enum { CASES=4 };
unsigned ksn_shiki_bench_count(void){return CASES;}

/* Records of k fields, given record by record in src, into planes of
 * stride S at dst (the padding is left 0). */
static void to_planes(int16_t *dst,const int16_t *src,unsigned k,unsigned n){
    const unsigned S=ksn_shiki_stride(n);
    memset(dst,0,k*S*sizeof *dst);
    for(unsigned j=0;j<n;j++)for(unsigned c=0;c<k;c++)dst[c*S+j]=src[j*k+c];
}

bool ksn_shiki_bench_build(unsigned i,int16_t work[KSN_SHIKI_BENCH_WORK],ksn_shiki_bench_case *c){
    if(i>=CASES)return false;
    memset(c,0,sizeof *c);
    uint32_t st=12345;
    c->pair=i/2;
    if(i<2){
        int16_t *pts=work,*out=work+2*POINTS,*tmp=work+4*POINTS;
        for(unsigned k=0;k<2*POINTS;k++)tmp[k]=(int16_t)((int32_t)(next(&st)%201)-100);
        to_planes(pts,tmp,2,POINTS);
        memset(out,0,2*POINTS*sizeof *out);
        const uint8_t decl[2][4]={{KSN_SHIKI_I16,0,2,0},{KSN_SHIKI_I16,0,2,1}};
        c->region[0]=(ksn_shiki_region){pts,POINTS,KSN_SHIKI_I16,0,2};
        c->region[1]=(ksn_shiki_region){out,POINTS,KSN_SHIKI_I16,0,2};
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
    /* The lines: LINES records of (x0, y0, x1, y1), planes of 104. */
    int16_t *ln=work,*tmp=work+4*ksn_shiki_stride(LINES);
    for(unsigned k=0;k<LINES;k++){
        const int16_t x=(int16_t)(next(&st)%240),y=(int16_t)(next(&st)%135);
        tmp[4*k]=x;tmp[4*k+1]=y;
        tmp[4*k+2]=(int16_t)(x+(int32_t)(next(&st)%17)-8);
        tmp[4*k+3]=(int16_t)(y+(int32_t)(next(&st)%17)-8);
    }
    to_planes(ln,tmp,4,LINES);
    const uint8_t decl[1][4]={{KSN_SHIKI_I16,0,4,0}};
    c->region[0]=(ksn_shiki_region){ln,LINES,KSN_SHIKI_I16,0,4};
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

/* ---- the PIE checks */
enum { CHECKS=10 };
unsigned ksn_shiki_bench_check_count(void){return CHECKS;}
bool ksn_shiki_bench_check_build(unsigned i,int16_t work[KSN_SHIKI_BENCH_WORK],ksn_shiki_bench_case *c){
    if(i>=CHECKS)return false;
    if(i==CHECKS-1){                       /* the timed vector form, checked too */
        const bool ok=ksn_shiki_bench_build(1,work,c);
        c->name="check affine128";
        return ok;
    }
    memset(c,0,sizeof *c);
    uint32_t st=777+i;
    for(unsigned k=0;k<KSN_SHIKI_BENCH_WORK;k++)work[k]=(int16_t)next(&st);
    int16_t *a=work,*o=work+512;
    const uint8_t one[2][4]={{KSN_SHIKI_I16,0,1,0},{KSN_SHIKI_I16,0,1,1}};
    c->region[0]=(ksn_shiki_region){a,128,KSN_SHIKI_I16,0,1};
    c->region[1]=(ksn_shiki_region){o,128,KSN_SHIKI_I16,0,1};
    c->regions=2;
    asm_t s;
    switch(i){
    case 0:
        c->name="check tail";
        s=begin(c,one,2);
        op(&s,KSN_SHIKI_SET,0,0,0,13,0);
        op(&s,KSN_SHIKI_SET,1,0,0,30000,0);
        op(&s,KSN_SHIKI_SET,2,0,0,-70000,0);         /* VSPLAT saturates */
        op(&s,KSN_SHIKI_VEACH,0,0,0,0,1);
        op(&s,KSN_SHIKI_VLOAD,0,0,0,0,0);
        op(&s,KSN_SHIKI_VSPLAT,1,1,0,0,0);
        op(&s,KSN_SHIKI_VSPLAT,2,2,0,0,0);
        op(&s,KSN_SHIKI_VADD,3,0,1,0,0);
        op(&s,KSN_SHIKI_VSUB,4,0,1,0,0);
        op(&s,KSN_SHIKI_VMIN,5,3,4,0,0);
        op(&s,KSN_SHIKI_VMAX,6,0,2,0,0);
        op(&s,KSN_SHIKI_VIOTA,7,0,0,0,0);
        op(&s,KSN_SHIKI_VSTORE,3,1,0,0,0);
        op(&s,KSN_SHIKI_END,0,0,0,0,0);
        finish(c,&s,2);
        return true;
    case 1: {
        /* 61 records (planes of 64): both fields on PIE, the last block
         * through the lane loops, the padding untouched. */
        c->name="check fields2";
        const uint8_t two[2][4]={{KSN_SHIKI_I16,0,2,0},{KSN_SHIKI_I16,0,2,1}};
        c->region[0]=(ksn_shiki_region){a,61,KSN_SHIKI_I16,0,2};
        c->region[1]=(ksn_shiki_region){o,61,KSN_SHIKI_I16,0,2};
        s=begin(c,two,2);
        op(&s,KSN_SHIKI_VEACH,0,0,0,0,0);
        op(&s,KSN_SHIKI_VLOAD,0,0,0,0,0);
        op(&s,KSN_SHIKI_VLOAD,1,0,1,0,0);
        op(&s,KSN_SHIKI_VSUB,2,1,0,0,0);
        op(&s,KSN_SHIKI_VSTORE,2,1,1,0,0);
        op(&s,KSN_SHIKI_END,0,0,0,0,0);
        finish(c,&s,2);
        return true;
    }
    case 2: {
        c->name="check record3";
        const uint8_t three[2][4]={{KSN_SHIKI_I16,0,3,0},{KSN_SHIKI_I16,0,1,1}};
        c->region[0]=(ksn_shiki_region){a,42,KSN_SHIKI_I16,0,3};    /* planes of 48 */
        c->region[1]=(ksn_shiki_region){o,42,KSN_SHIKI_I16,0,1};
        s=begin(c,three,2);
        op(&s,KSN_SHIKI_VEACH,0,0,0,0,0);
        op(&s,KSN_SHIKI_VLOAD,0,0,2,0,0);
        op(&s,KSN_SHIKI_VSTORE,0,1,0,0,0);
        op(&s,KSN_SHIKI_END,0,0,0,0,0);
        finish(c,&s,2);
        return true;
    }
    case 3: case 4:
        /* 16 blocks x 40 MACs of +-32767 * 32767 pass 2^39 either way. */
        c->name=i==3?"check sat40 up":"check sat40 down";
        for(unsigned k=0;k<128;k++)a[k]=i==3?32767:-32768;
        s=begin(c,one,2);
        op(&s,KSN_SHIKI_SET,0,0,0,32767,0);
        op(&s,KSN_SHIKI_SET,1,0,0,8,0);
        op(&s,KSN_SHIKI_VEACH,0,0,0,0,0);
        op(&s,KSN_SHIKI_VLOAD,0,0,0,0,0);
        op(&s,KSN_SHIKI_VSPLAT,1,0,0,0,0);
        for(unsigned k=0;k<20;k++){
            op(&s,KSN_SHIKI_VMACK,0,0,0,0,0);
            op(&s,KSN_SHIKI_VMAC,0,0,1,0,0);
        }
        op(&s,KSN_SHIKI_END,0,0,0,0,0);
        op(&s,KSN_SHIKI_VEACH,0,0,1,0,1);
        op(&s,KSN_SHIKI_VOUT,2,0,0,0,24);
        op(&s,KSN_SHIKI_VOUT,3,0,0,0,0);
        op(&s,KSN_SHIKI_END,0,0,0,0,0);
        finish(c,&s,2);
        return true;
    case 5:
        /* What SRCMB writes back: 500000 (the shifted value) or 32767 (its
         * sat16)? The second VOUT reads 31250 or 2047. */
        c->name="check writeback";
        s=begin(c,one,2);
        op(&s,KSN_SHIKI_SET,0,0,0,1000000,0);
        op(&s,KSN_SHIKI_SET,1,0,0,8,0);
        op(&s,KSN_SHIKI_VEACH,0,0,1,0,1);
        op(&s,KSN_SHIKI_VACCSET,0,0,0,0,0);
        op(&s,KSN_SHIKI_VOUT,0,0,0,0,1);
        op(&s,KSN_SHIKI_VOUT,1,0,0,0,4);
        op(&s,KSN_SHIKI_END,0,0,0,0,0);
        finish(c,&s,2);
        return true;
    case 6: {
        c->name="check accset";
        static const int32_t t[4]={123456789,INT32_MIN,-5,INT32_MAX};
        static const uint16_t sh[4]={12,16,1,16};
        s=begin(c,one,2);
        for(unsigned k=0;k<4;k++)op(&s,KSN_SHIKI_SET,(uint8_t)k,0,0,t[k],0);
        op(&s,KSN_SHIKI_SET,4,0,0,8,0);
        op(&s,KSN_SHIKI_VEACH,0,0,4,0,1);
        for(unsigned k=0;k<4;k++){
            op(&s,KSN_SHIKI_VACCSET,0,(uint8_t)k,0,0,0);
            op(&s,KSN_SHIKI_VOUT,(uint8_t)k,0,0,0,sh[k]);
        }
        op(&s,KSN_SHIKI_END,0,0,0,0,0);
        finish(c,&s,2);
        return true;
    }
    case 7:
        /* Large shifts: 40 * 32767^2 is about 2^35.3. The first device run
         * (f3ec673) also had 40 and 63 here, which PIE did not match. */
        c->name="check shift32";
        s=begin(c,one,2);
        op(&s,KSN_SHIKI_SET,0,0,0,32767,0);
        op(&s,KSN_SHIKI_SET,1,0,0,8,0);
        op(&s,KSN_SHIKI_SET,2,0,0,-1000,0);
        op(&s,KSN_SHIKI_VEACH,0,0,1,0,1);
        op(&s,KSN_SHIKI_VZACC,0,0,0,0,0);
        op(&s,KSN_SHIKI_VSPLAT,0,0,0,0,0);
        for(unsigned k=0;k<40;k++)op(&s,KSN_SHIKI_VMACK,0,0,0,0,0);
        op(&s,KSN_SHIKI_VOUT,1,0,0,0,32);
        op(&s,KSN_SHIKI_VOUT,2,0,0,0,0);
        op(&s,KSN_SHIKI_VACCSET,0,2,0,0,0);
        op(&s,KSN_SHIKI_VOUT,3,0,0,0,32);            /* the largest shift allowed */
        op(&s,KSN_SHIKI_VACCSET,0,2,0,0,0);
        op(&s,KSN_SHIKI_VOUT,4,0,0,0,31);
        op(&s,KSN_SHIKI_END,0,0,0,0,0);
        finish(c,&s,2);
        return true;
    default:
        /* Regions off the 16-B grid: the C model's lane loops. */
        c->name="check unaligned";
        c->region[0]=(ksn_shiki_region){a+1,64,KSN_SHIKI_I16,0,1};
        c->region[1]=(ksn_shiki_region){o+3,64,KSN_SHIKI_I16,0,1};   /* off the 16 B grid */
        s=begin(c,one,2);
        op(&s,KSN_SHIKI_VEACH,0,0,0,0,0);
        op(&s,KSN_SHIKI_VLOAD,0,0,0,0,0);
        op(&s,KSN_SHIKI_VADD,0,0,0,0,0);
        op(&s,KSN_SHIKI_VSTORE,0,1,0,0,0);
        op(&s,KSN_SHIKI_END,0,0,0,0,0);
        finish(c,&s,2);
        return true;
    }
}

/* ---- random programs for the scalar JIT (ksn_shiki_bench.h) --------------- */
static void put_inst(uint8_t *b,uint8_t op,uint8_t dst,uint8_t a,uint8_t bb,int32_t imm,uint16_t aux){
    b[0]=op;b[1]=dst;b[2]=a;b[3]=bb;
    for(unsigned k=0;k<4;k++)b[4+k]=(uint8_t)((uint32_t)imm>>(8*k));
    b[8]=(uint8_t)aux;b[9]=(uint8_t)(aux>>8);b[10]=b[11]=0;
}
static int32_t value(uint32_t *st){
    switch(next(st)%12){
    case 10: case 11: {                         /* +-2^k, +-(2^k - 1): the exact boundaries */
        const unsigned k=next(st)%32;
        const int64_t m=((int64_t)1<<k)-(int64_t)(next(st)&1);
        return (int32_t)(next(st)&1?m:-m);
    }
    case 8: case 9: {                           /* log-uniform: products near every boundary */
        const uint32_t m=(next(st)<<8|next(st))>>(next(st)%32);
        return next(st)&1?(int32_t)m:-(int32_t)m;
    }
    case 0: return (int32_t)(next(st)%4001)-2000;
    case 1: return INT32_MAX-(int32_t)(next(st)%3);
    case 2: return INT32_MIN+(int32_t)(next(st)%3);
    case 3: return (int32_t)(next(st)<<8^next(st));
    case 4: return (int32_t)(next(st)%2048)-1024+((next(st)&1)?65536:0);
    case 5: return (int32_t)(next(st)%601)-300;
    default: return (int32_t)(next(st)%40)-20;
    }
}
/* A well-nested program of up to 64 instructions; registration has the last
 * word (a refused one is drawn again). */
static unsigned program(uint32_t *st,uint8_t *body,unsigned *nin){
    unsigned n=0,open[KSN_SHIKI_LOOP_DEPTH],depth=0;
    const unsigned want=4+next(st)%60;
    const int big=next(st)%4==0;                   /* long loops: the step limit */
    *nin=next(st)%(KSN_SHIKI_INPUTS+1);
    /* Half the programs give most registers a value first, so that the
     * arithmetic meets the boundaries rather than zeros. */
    if(next(st)&1)
        for(unsigned k=0;k<KSN_SHIKI_REGS&&n+4<want;k++)if(next(st)%4)put_inst(body+12*n++,KSN_SHIKI_SET,(uint8_t)k,0,0,value(st),0);
    while(n<want){
        const unsigned left=want-n;
        if(depth&&(left<=depth||next(st)%6==0)){put_inst(body+12*n++,KSN_SHIKI_END,0,0,0,0,0);depth--;continue;}
        const uint8_t d=(uint8_t)(next(st)%KSN_SHIKI_REGS),ra=(uint8_t)(next(st)%KSN_SHIKI_REGS),rb=(uint8_t)(next(st)%KSN_SHIKI_REGS);
        uint8_t *p=body+12*n;
        switch(next(st)%24){
        case 0: case 1: put_inst(p,KSN_SHIKI_SET,d,0,0,value(st),0); break;
        case 2: if(*nin){put_inst(p,KSN_SHIKI_INPUT,d,(uint8_t)(next(st)%*nin),0,0,0);break;}
                put_inst(p,KSN_SHIKI_SET,d,0,0,value(st),0); break;
        case 3: case 4: case 5: put_inst(p,KSN_SHIKI_ADD,d,ra,rb,0,0); break;
        case 6: put_inst(p,KSN_SHIKI_SUB,d,ra,rb,0,0); break;
        case 7: put_inst(p,KSN_SHIKI_NEG,d,ra,0,0,0); break;
        case 8: case 9: case 10: {
            static const uint16_t s[]={0,1,8,15,16,24,31,32,33,40,62};
            put_inst(p,KSN_SHIKI_MUL,d,ra,rb,0,next(st)%3?s[next(st)%11]:(uint16_t)(next(st)%63));
            break;
        }
        case 11: put_inst(p,KSN_SHIKI_SHIFT,d,ra,0,(int32_t)(next(st)%63)-31,0); break;
        case 12: put_inst(p,KSN_SHIKI_SIN,d,ra,0,0,0); break;
        case 13: put_inst(p,KSN_SHIKI_ADD_H,d,ra,rb,0,0); break;
        case 14: case 15:
            if(depth<4&&left>depth+2){
                put_inst(p,KSN_SHIKI_REPEAT,0,0,0,big?(int32_t)(1+next(st)%255):(int32_t)(1+next(st)%4),0);
                open[depth++]=n;
            }else put_inst(p,KSN_SHIKI_ADD,d,ra,rb,0,0);
            break;
        case 16:
            if(depth<4&&left>depth+2){put_inst(p,KSN_SHIKI_REPEAT_REG,0,ra,0,0,0);open[depth++]=n;}
            else put_inst(p,KSN_SHIKI_SUB,d,ra,rb,0,0);
            break;
        case 17: if(depth){put_inst(p,KSN_SHIKI_BREAK_IF_GT,0,ra,rb,0,0);break;}
                 put_inst(p,KSN_SHIKI_MOVE,0,ra,rb,0,0); break;
        case 18: put_inst(p,KSN_SHIKI_MOVE,0,ra,rb,0,0); break;
        case 19: put_inst(p,KSN_SHIKI_LINE,0,ra,rb,0,(uint16_t)next(st)); break;
        case 20: put_inst(p,KSN_SHIKI_PLOT,0,ra,rb,0,(uint16_t)next(st)); break;
        case 21: put_inst(p,rnd2(st)?KSN_SHIKI_LINE_C:KSN_SHIKI_PLOT_C,d,ra,rb,0,0); break;
        case 22: put_inst(p,KSN_SHIKI_SIN,d,ra,0,0,0); break;
        default: put_inst(p,KSN_SHIKI_DRAW_INDEX,d,0,0,0,0); break;
        }
        (void)open;
        n++;
    }
    while(depth){put_inst(body+12*n++,KSN_SHIKI_END,0,0,0,0,0);depth--;}
    return n;
}

size_t ksn_shiki_bench_random(uint32_t *seed,ksn_shiki_bench_case *c,int32_t in[KSN_SHIKI_INPUTS],
                              unsigned *nin,unsigned *index){
    uint32_t *st=seed;
    memset(c,0,sizeof *c);
    c->name="random";
    uint8_t body[12*(KSN_SHIKI_CODE+KSN_SHIKI_LOOP_DEPTH)];
    unsigned n;
    do n=program(st,body,nin); while(n>KSN_SHIKI_CODE);
    const uint8_t head[12]={'S','K','I','1',1,0,(uint8_t)n,(uint8_t)*nin,0,0,0,0};
    memcpy(c->code,head,12);
    const size_t at=(12u+*nin+3u)&~(size_t)3u;     /* the inputs' Q bytes (0), padded */
    memcpy(c->code+at,body,12u*n);
    c->len=at+12u*n;
    for(unsigned k=0;k<KSN_SHIKI_INPUTS;k++)in[k]=k<*nin?value(st):0;
    *index=next(st)%5u;
    return c->len;
}
