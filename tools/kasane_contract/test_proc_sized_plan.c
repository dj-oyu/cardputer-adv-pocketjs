/* The sized plan (allocated for its own instruction count) against the
 * full-size plan: same prepare verdict, same fusion, same VM state and frame
 * for every program, at capacities 1, 2, 63, 64 and random ones. Each sized
 * plan lives in a malloc of exactly ksn_proc_sized_plan_bytes(), so under
 * ASan any read or write past the count is a heap-buffer-overflow. */
#include "ksn_proc_plan.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define I(op,dst,a,b,v,c) {op,dst,a,b,v,c}
static uint32_t seed=0x2545F491u;
static uint32_t next(void){seed=seed*1664525u+1013904223u;return seed>>8;}

static void equal_vm(const ksn_proc_vm *a,const ksn_proc_vm *b){
    assert(a->status==b->status && a->pc==b->pc && a->last_pc==b->last_pc);
    assert(a->steps==b->steps && a->depth==b->depth);
    assert(memcmp(a->reg,b->reg,sizeof a->reg)==0);
    assert(memcmp(a->loop,b->loop,sizeof a->loop)==0);
    assert(a->pen_x==b->pen_x && a->pen_y==b->pen_y && a->pen_valid==b->pen_valid);
    assert(a->frame->ready==b->frame->ready && a->frame->count==b->frame->count);
    assert(a->frame->raster_steps==b->frame->raster_steps);
    assert(memcmp(a->frame->segments,b->frame->segments,
                  a->frame->count*sizeof a->frame->segments[0])==0);
}

/* Mostly valid programs: registers 0..15, small SET values, balanced loops
 * most of the time, occasional garbage so the rejecting paths run too. */
static void random_program(ksn_proc_inst *code,unsigned n){
    unsigned depth=0;
    for(unsigned i=0;i<n;i++){
        ksn_proc_inst in={0};
        unsigned left=n-i;
        unsigned pick=next()%16;
        if(depth && left<=depth){in.op=KSN_PROC_END;depth--;code[i]=in;continue;}
        switch(pick){
        case 0:case 1:in.op=KSN_PROC_SET;in.dst=(uint8_t)(next()%16);
            in.value=(float)((int)(next()%200)-100)/8.0f;break;
        case 2:in.op=KSN_PROC_INPUT;in.dst=(uint8_t)(next()%16);in.a=(uint8_t)(next()%8);break;
        case 3:case 4:in.op=KSN_PROC_ADD;in.dst=(uint8_t)(next()%16);
            in.a=(uint8_t)(next()%16);in.b=(uint8_t)(next()%16);break;
        case 5:case 6:in.op=KSN_PROC_MUL;in.dst=(uint8_t)(next()%16);
            in.a=(uint8_t)(next()%16);in.b=(uint8_t)(next()%16);break;
        case 7:in.op=KSN_PROC_SIN;in.dst=(uint8_t)(next()%16);in.a=(uint8_t)(next()%16);break;
        case 8:in.op=KSN_PROC_MOVE;in.a=(uint8_t)(next()%16);in.b=(uint8_t)(next()%16);break;
        case 9:in.op=KSN_PROC_LINE;in.a=(uint8_t)(next()%16);in.b=(uint8_t)(next()%16);
            in.color=(uint16_t)next();break;
        case 10:in.op=KSN_PROC_PLOT;in.a=(uint8_t)(next()%16);in.b=(uint8_t)(next()%16);
            in.color=(uint16_t)next();break;
        case 11:if(depth<KSN_PROC_LOOP_DEPTH&&left>depth+2){
                in.op=KSN_PROC_REPEAT;in.a=(uint8_t)(1+next()%6);depth++;
            }else{in.op=KSN_PROC_ADD;in.dst=1;in.a=1;in.b=2;}break;
        case 12:if(depth){in.op=KSN_PROC_END;depth--;}
            else{in.op=KSN_PROC_MUL;in.dst=3;in.a=3;in.b=4;}break;
        case 13:in.op=KSN_PROC_BREAK_IF_GT;in.a=(uint8_t)(next()%16);in.b=(uint8_t)(next()%16);break;
        case 14:in.op=KSN_PROC_CUBIC;in.a=(uint8_t)(1+next()%8);in.color=(uint16_t)next();break;
        default:/* garbage: bad op or register */
            in.op=(uint8_t)(next()%20);in.dst=(uint8_t)(next()%20);
            in.a=(uint8_t)(next()%20);in.b=(uint8_t)(next()%20);break;
        }
        code[i]=in;
    }
}

static ksn_proc_sized_plan *sized(unsigned capacity){
    size_t bytes=ksn_proc_sized_plan_bytes(capacity);
    assert(bytes==offsetof(ksn_proc_sized_plan,code)+capacity*sizeof(ksn_proc_inst));
    ksn_proc_sized_plan *p=malloc(bytes);
    assert(p);memset(p,0xA5,bytes);
    return p;
}

static unsigned checked_valid;
static void compare(const ksn_proc_inst *code,unsigned count,unsigned capacity){
    const ksn_proc_program program={code,(uint8_t)count};
    static ksn_proc_plan full;
    ksn_proc_sized_plan *s=sized(capacity);
    bool a=ksn_proc_plan_prepare(&full,&program);
    bool b=ksn_proc_sized_plan_prepare(s,capacity,&program);
    assert(a==b && full.valid==s->valid && s->capacity==capacity);
    if(b){
        assert(s->count==count && full.fused_count==s->fused_count);
        assert(!memcmp(full.fused,s->fused,sizeof s->fused));
        assert(!memcmp(full.code,s->code,count*sizeof code[0]));
    }
    float input[KSN_PROC_INPUTS];
    for(unsigned j=0;j<KSN_PROC_INPUTS;j++)input[j]=(float)((int)(next()%64)-32)/4.0f;
    static ksn_proc_vm va,vb;static ksn_proc_frame fa,fb;
    ksn_proc_status ra=ksn_proc_plan_begin(&va,&full,input,&fa);
    ksn_proc_status rb=ksn_proc_sized_plan_begin(&vb,s,input,&fb);
    assert(ra==rb);
    for(int debug=0;debug<2;debug++){
        if(debug){ra=ksn_proc_plan_begin(&va,&full,input,&fa);
                  rb=ksn_proc_sized_plan_begin(&vb,s,input,&fb);}
        if(ra==KSN_PROC_RUNNING){
            assert(ksn_proc_plan_run(&va,&full,debug)==ksn_proc_sized_plan_run(&vb,s,debug));
            equal_vm(&va,&vb);
            if(va.status==KSN_PROC_DONE)checked_valid++;
        }
    }
    free(s);
}

static void boundaries(void){
    static ksn_proc_inst code[KSN_PROC_CODE];
    const unsigned caps[]={1,2,63,64};
    for(unsigned c=0;c<4;c++)
        for(unsigned r=0;r<400;r++){random_program(code,caps[c]);compare(code,caps[c],caps[c]);}
    for(unsigned r=0;r<4000;r++){
        unsigned n=1+next()%KSN_PROC_CODE;
        random_program(code,n);compare(code,n,n);
        /* A plan may be larger than its program; the tail is never read. */
        unsigned cap=n+next()%(KSN_PROC_CODE-n+1);
        compare(code,n,cap);
    }
    /* A 64-instruction plan that draws (fusion right at pc 62..63). */
    ksn_proc_inst big[KSN_PROC_CODE];
    for(unsigned i=0;i<KSN_PROC_CODE;i++)big[i]=(ksn_proc_inst)I(KSN_PROC_ADD,1,1,0,0,0);
    big[0]=(ksn_proc_inst)I(KSN_PROC_SET,0,0,0,1.5f,0);
    big[KSN_PROC_CODE-3]=(ksn_proc_inst)I(KSN_PROC_SET,4,0,0,2,0);
    big[KSN_PROC_CODE-2]=(ksn_proc_inst)I(KSN_PROC_ADD,2,1,0,0,0);
    big[KSN_PROC_CODE-1]=(ksn_proc_inst)I(KSN_PROC_MUL,3,2,2,0,0);
    compare(big,KSN_PROC_CODE,KSN_PROC_CODE);
    ksn_proc_sized_plan *s=sized(KSN_PROC_CODE);
    const ksn_proc_program p64={big,KSN_PROC_CODE};
    assert(ksn_proc_sized_plan_prepare(s,KSN_PROC_CODE,&p64));
    assert(s->fused[1]&(1u<<30)); /* pc 62 */
    free(s);
}

static void rejects(void){
    ksn_proc_inst code[KSN_PROC_CODE+1];
    for(unsigned i=0;i<=KSN_PROC_CODE;i++)code[i]=(ksn_proc_inst)I(KSN_PROC_SET,0,0,0,1,0);
    assert(ksn_proc_sized_plan_bytes(0)==0);
    assert(ksn_proc_sized_plan_bytes(KSN_PROC_CODE+1)==0);
    assert(ksn_proc_sized_plan_bytes(KSN_PROC_CODE)<sizeof(ksn_proc_plan));
    /* count > capacity: refused without writing past the allocation. */
    for(unsigned cap=1;cap<KSN_PROC_CODE;cap++){
        ksn_proc_sized_plan *s=sized(cap);
        const ksn_proc_program p={code,(uint8_t)(cap+1)};
        assert(!ksn_proc_sized_plan_prepare(s,cap,&p));
        assert(!s->valid && s->capacity==cap);
        /* An invalid plan takes the reference path: begin refuses. */
        ksn_proc_vm vm;ksn_proc_frame f;const float in[KSN_PROC_INPUTS]={0};
        assert(ksn_proc_sized_plan_begin(&vm,s,in,&f)==KSN_PROC_INVALID);
        assert(ksn_proc_sized_plan_run(&vm,s,false)==KSN_PROC_INVALID);
        free(s);
    }
    ksn_proc_sized_plan *s=sized(4);
    const ksn_proc_program zero={code,0},null={NULL,1},p65={code,KSN_PROC_CODE+1};
    assert(!ksn_proc_sized_plan_prepare(s,4,&zero));
    assert(!ksn_proc_sized_plan_prepare(s,4,&null));
    assert(!ksn_proc_sized_plan_prepare(s,4,NULL));
    assert(!ksn_proc_sized_plan_prepare(s,4,&p65));
    assert(!ksn_proc_sized_plan_prepare(s,0,&zero));
    assert(!ksn_proc_sized_plan_prepare(NULL,4,&zero));
    /* Prepared, then re-prepared from a subrange of its own code. */
    const ksn_proc_inst two[]={I(KSN_PROC_ADD,0,0,1,0,0),I(KSN_PROC_MUL,2,0,1,0,0),
                               I(KSN_PROC_SET,3,0,0,2,0),I(KSN_PROC_ADD,4,3,3,0,0)};
    const ksn_proc_program p4={two,4};
    assert(ksn_proc_sized_plan_prepare(s,4,&p4));
    const ksn_proc_program own={s->code+2,2};
    assert(ksn_proc_sized_plan_prepare(s,4,&own));
    assert(s->count==2 && s->code[0].op==KSN_PROC_SET && s->fused_count==0);
    /* Points registration: only on a valid plan; re-prepare clears it. */
    const KsnProcAffineQ14 coeff={16384,0,0,16384,0,0};
    assert(ksn_proc_sized_plan_register_points_affine(s,&coeff,NULL));
    int16_t x[3]={1,2,3},y[3]={4,5,6},ox[3],oy[3];
    KsnProcPointsDecision d;
    assert(ksn_proc_sized_plan_run_points_affine(s,(KsnProcPointDst){ox,oy},
                                                 (KsnProcPointSrc){x,y},3,&d));
    assert(ox[2]==3 && oy[0]==4 && d.backend==KSN_PROC_POINTS_SCALAR);
    assert(ksn_proc_sized_plan_prepare(s,4,&p4));
    assert(!ksn_proc_sized_plan_run_points_affine(s,(KsnProcPointDst){ox,oy},
                                                  (KsnProcPointSrc){x,y},3,&d));
    assert(!ksn_proc_sized_plan_prepare(s,4,&zero));
    assert(!ksn_proc_sized_plan_register_points_affine(s,&coeff,NULL));
    free(s);
}

int main(void){
    rejects();
    boundaries();
    assert(checked_valid>1000);
    printf("sized plan: parity with the full-size plan (%u completed runs); bytes 1/8/16/32/64 = %zu/%zu/%zu/%zu/%zu, full %zu\n",
           checked_valid,ksn_proc_sized_plan_bytes(1),ksn_proc_sized_plan_bytes(8),
           ksn_proc_sized_plan_bytes(16),ksn_proc_sized_plan_bytes(32),
           ksn_proc_sized_plan_bytes(64),sizeof(ksn_proc_plan));
    return 0;
}
