#define _POSIX_C_SOURCE 200809L
#include "ksn_proc_plan.h"
#include <assert.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define I(op,d,a,b,v,c) {op,d,a,b,v,c}
#define N(a) ((uint8_t)(sizeof(a)/sizeof((a)[0])))
static unsigned comparisons;
static volatile uint64_t append_sink;
static uint64_t now_ns(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
static void equal(const ksn_proc_vm *a,const ksn_proc_execution *b){
    assert(a->status==b->status&&a->pc==b->pc&&a->last_pc==b->last_pc);
    assert(a->steps==b->steps&&a->depth==b->depth);
    assert(!memcmp(a->reg,b->reg,sizeof a->reg)&&!memcmp(a->input,b->input,sizeof a->input));
    assert(!memcmp(a->loop,b->loop,sizeof a->loop));
    assert(a->pen_x==b->pen_x&&a->pen_y==b->pen_y&&a->pen_valid==b->pen_valid);
    assert(a->frame->ready==b->frame->ready&&a->frame->count==b->frame->count);
    assert(a->frame->raster_steps==b->frame->raster_steps&&a->frame->ext==b->frame->ext);
    assert(!memcmp(a->frame->segments,b->frame->segments,a->frame->count*sizeof a->frame->segments[0]));
    ++comparisons;
}
static void compare(ksn_proc_program program,const float input[KSN_PROC_INPUTS],const ksn_proc_state *state){
    ksn_proc_validated_plan *plan=ksn_proc_validated_create(program.count);assert(plan);
    assert(ksn_proc_validated_prepare(plan,&program,NULL));
    for(unsigned mode=0;mode<3;mode++){
        ksn_proc_vm a;ksn_proc_execution b={0};ksn_proc_frame fa,fb;
        assert(ksn_proc_begin_state(&a,&program,input,state,&fa)==KSN_PROC_RUNNING);
        assert(ksn_proc_validated_begin(&b,plan,input,state,&fb)==KSN_PROC_RUNNING);equal(&a,&b);
        if(mode==0){
            /* Pauses at every instruction, including loop and failure edges. */
            while(a.status==KSN_PROC_RUNNING){assert(ksn_proc_step(&a)==ksn_proc_validated_step(&b));equal(&a,&b);}
        }else{assert(ksn_proc_run(&a)==ksn_proc_validated_run(&b,mode==1));equal(&a,&b);}
        ksn_proc_state sa,sb;memset(&sa,0xa5,sizeof sa);memset(&sb,0xa5,sizeof sb);
        assert(ksn_proc_capture_state(&a,&sa)==ksn_proc_validated_capture(&b,&sb));
        assert(!memcmp(&sa,&sb,sizeof sa));
        ksn_proc_validated_end(&b);ksn_proc_validated_end(&b);
        assert(ksn_proc_validated_step(&b)==KSN_PROC_INVALID);
    }
    assert(ksn_proc_validated_destroy(plan));
}
static void semantics(void){
    float input[KSN_PROC_INPUTS]={0};ksn_proc_state state={0};
    ksn_proc_inst code[]={I(KSN_PROC_INPUT,0,0,0,0,0),I(KSN_PROC_SET,1,0,0,1,0),
        I(KSN_PROC_REPEAT_REG,0,2,0,0,0),I(KSN_PROC_ADD,0,0,1,0,0),I(KSN_PROC_MUL,3,0,1,0,0),
        I(KSN_PROC_PLOT,0,0,3,0,7),I(KSN_PROC_BREAK_IF_GT,0,0,4,0,0),I(KSN_PROC_END,0,0,0,0,0)};
    for(unsigned n=0;n<=255;n+=17){state.reg[2]=(float)n;state.reg[4]=n/2;compare((ksn_proc_program){code,N(code)},input,&state);}
    state.reg[2]=NAN;
    ksn_proc_validated_plan *bad=ksn_proc_validated_create(N(code));assert(bad&&ksn_proc_validated_prepare(bad,&(ksn_proc_program){code,N(code)},NULL));
    for(unsigned kind=0;kind<3;kind++){
        ksn_proc_vm a;ksn_proc_execution b={0};ksn_proc_frame fa={.ready=true},fb={.ready=true};
        input[0]=kind==0?INFINITY:0;state.reg[2]=kind==1?NAN:0;state.pen_valid=kind==2;state.pen_x=721;
        assert(ksn_proc_begin_state(&a,&(ksn_proc_program){code,N(code)},input,&state,&fa)==KSN_PROC_INVALID);
        assert(ksn_proc_validated_begin(&b,bad,input,&state,&fb)==KSN_PROC_INVALID);
        assert(!b.lease&&!fb.ready&&!fb.count&&!memcmp(a.reg,b.reg,sizeof a.reg)&&!memcmp(a.input,b.input,sizeof a.input));
    }
    assert(ksn_proc_validated_destroy(bad));input[0]=0;
    ksn_proc_inst overflow[]={I(KSN_PROC_SET,0,0,0,FLT_MAX,0),I(KSN_PROC_SET,1,0,0,2,0),I(KSN_PROC_MUL,2,0,1,0,0),I(KSN_PROC_ADD,3,2,1,0,0)};
    compare((ksn_proc_program){overflow,N(overflow)},input,NULL);
    ksn_proc_inst zero[]={I(KSN_PROC_ADD,0,0,15,0,0),I(KSN_PROC_PLOT,0,0,15,0,9)};
    compare((ksn_proc_program){zero,N(zero)},input,NULL);
    ksn_proc_inst limit[]={I(KSN_PROC_REPEAT,0,255,0,0,0),I(KSN_PROC_REPEAT,0,255,0,0,0),I(KSN_PROC_SET,0,0,0,0,0),I(KSN_PROC_END,0,0,0,0,0),I(KSN_PROC_END,0,0,0,0,0)};
    compare((ksn_proc_program){limit,N(limit)},input,NULL);
    limit[2]=(ksn_proc_inst)I(KSN_PROC_PLOT,0,0,0,0,4);compare((ksn_proc_program){limit,N(limit)},input,NULL);
    ksn_proc_inst raster[]={I(KSN_PROC_SET,0,0,0,-480,0),I(KSN_PROC_SET,1,0,0,720,0),I(KSN_PROC_REPEAT,0,20,0,0,0),I(KSN_PROC_MOVE,0,0,0,0,0),I(KSN_PROC_LINE,0,1,1,0,8),I(KSN_PROC_END,0,0,0,0,0)};
    compare((ksn_proc_program){raster,N(raster)},input,NULL);
}
static void lifetime(void){
    ksn_proc_inst *code=malloc(2*sizeof *code);assert(code);
    code[0]=(ksn_proc_inst)I(KSN_PROC_SET,0,0,0,0,0);code[1]=(ksn_proc_inst)I(KSN_PROC_PLOT,0,0,0,0,3);
    ksn_proc_patch patch={0,KSN_PROC_FIELD_VALUE,0};float args[]={12},input[KSN_PROC_INPUTS]={0};
    ksn_proc_binding binding={&patch,args,1,1};ksn_proc_program program={code,2};
    ksn_proc_validated_plan *plan=ksn_proc_validated_create(2);assert(plan&&ksn_proc_validated_prepare(plan,&program,&binding));
    uint64_t generation=ksn_proc_validated_generation(plan);
    ksn_proc_vm a;ksn_proc_execution b={0};ksn_proc_frame fa,fb;
    memset(&a,0xa5,sizeof a);
    assert(ksn_proc_begin_bound(&a,&program,NULL,input,&fa)==KSN_PROC_INVALID);
    assert(a.loop_end==a.owned_loop_end);
    for(unsigned i=0;i<KSN_PROC_CODE;i++)assert(a.loop_end[i]==0);
    assert(ksn_proc_begin_bound(&a,&program,&binding,input,&fa)==KSN_PROC_RUNNING);
    assert(ksn_proc_validated_begin(&b,plan,input,NULL,&fb)==KSN_PROC_RUNNING);
    code[0].op=255;args[0]=NAN;patch.pc=255;free(code);
    assert(!ksn_proc_validated_prepare(plan,NULL,NULL)&&!ksn_proc_validated_destroy(plan));
    assert(ksn_proc_validated_generation(plan)==generation);
    assert(ksn_proc_validated_begin(&b,plan,input,NULL,&fb)==KSN_PROC_INVALID);equal(&a,&b);
    assert(ksn_proc_step(&a)==ksn_proc_validated_step(&b));equal(&a,&b);
    assert(ksn_proc_run(&a)==ksn_proc_validated_run(&b,false));equal(&a,&b);
    ksn_proc_validated_end(&b);assert(!b.lease&&!b.program&&!b.loop_end);
    assert(!ksn_proc_validated_prepare(plan,NULL,NULL));assert(ksn_proc_validated_generation(plan)==generation+1);
    assert(ksn_proc_validated_begin(&b,plan,input,NULL,&fb)==KSN_PROC_INVALID&&!b.lease&&!fb.ready);
    const ksn_proc_inst replacement[]={I(KSN_PROC_PLOT,0,0,0,0,11)};
    assert(ksn_proc_validated_prepare(plan,&(ksn_proc_program){replacement,1},NULL));
    assert(ksn_proc_validated_begin(&b,plan,input,NULL,&fb)==KSN_PROC_RUNNING);
    ++b.generation;assert(ksn_proc_validated_step(&b)==KSN_PROC_INVALID);--b.generation;
    ksn_proc_validated_end(&b);assert(b.status==KSN_PROC_INVALID);
    assert(ksn_proc_validated_destroy(plan));assert(ksn_proc_validated_step(&b)==KSN_PROC_INVALID);
}
static void costs(unsigned count){
    ksn_proc_inst code[KSN_PROC_CODE]={0};
    for(unsigned i=0;i<count;i++)code[i]=(ksn_proc_inst)I(KSN_PROC_ADD,0,0,1,0,0);
    code[count-1]=(ksn_proc_inst)I(KSN_PROC_PLOT,0,0,0,0,1);
    ksn_proc_program program={code,(uint8_t)count};float input[KSN_PROC_INPUTS]={0};
    uint64_t began=now_ns();
    ksn_proc_sized_plan *reference_plan=calloc(1,ksn_proc_sized_plan_bytes(count));
    assert(reference_plan&&ksn_proc_sized_plan_prepare(reference_plan,count,&program));
    uint64_t reference_preparation=now_ns()-began;began=now_ns();
    ksn_proc_validated_plan *plan=ksn_proc_validated_create(count);
    assert(plan&&ksn_proc_validated_prepare(plan,&program,NULL));uint64_t preparation=now_ns()-began;
    for(unsigned mode=0;mode<2;mode++){
        uint64_t begin=0,execute=0,append=0,release=0;ksn_proc_frame frame,candidate={0};
        ksn_proc_vm reference;ksn_proc_execution execution={0};
        for(unsigned rep=0;rep<20000;rep++){
            uint64_t t=now_ns();
            assert((mode?ksn_proc_validated_begin(&execution,plan,input,NULL,&frame):ksn_proc_sized_plan_begin(&reference,reference_plan,input,&frame))==KSN_PROC_RUNNING);
            begin+=now_ns()-t;t=now_ns();
            assert((mode?ksn_proc_validated_run(&execution,false):ksn_proc_sized_plan_run(&reference,reference_plan,false))==KSN_PROC_DONE);
            execute+=now_ns()-t;t=now_ns();
            memcpy(candidate.segments,frame.segments,frame.count*sizeof frame.segments[0]);candidate.count=frame.count;
            __asm__ __volatile__("" : : "r"(&candidate) : "memory");
            append+=now_ns()-t;
            const volatile unsigned char *bytes=(const volatile unsigned char *)candidate.segments;
            uint64_t hash=1469598103934665603ull;
            for(size_t i=0;i<candidate.count*sizeof candidate.segments[0];i++){hash^=bytes[i];hash*=1099511628211ull;}
            append_sink+=hash^rep;
            if(mode){t=now_ns();ksn_proc_validated_end(&execution);release+=now_ns()-t;}
        }
        printf("PROC_SETUP_COST {\"host_only\":true,\"commands\":%u,\"mode\":\"%s\",\"iterations\":20000,\"prepare_ns\":%"PRIu64",\"begin_ns\":%"PRIu64",\"execute_ns\":%"PRIu64",\"append_ns\":%"PRIu64",\"release_ns\":%"PRIu64",\"plan_bytes\":%zu,\"execution_bytes\":%zu,\"snapshot_vm_bytes\":%zu,\"last_segments\":%u}\n",count,mode?"validated":"reference",mode?preparation:reference_preparation,begin,execute,append,release,mode?ksn_proc_validated_allocation_bytes(plan):ksn_proc_sized_plan_bytes(count),sizeof execution,sizeof reference,candidate.count);
    }
    assert(ksn_proc_validated_destroy(plan));free(reference_plan);
    printf("PROC_APPEND_SINK %"PRIu64"\n",append_sink);
}
int main(void){semantics();lifetime();if(!getenv("PROC_SKIP_COSTS")){costs(1);costs(KSN_PROC_CODE);}printf("PROC_VALIDATED_PASS comparisons=%u mutation resume generation failure_snapshot lifetime caps zero\n",comparisons);return 0;}
