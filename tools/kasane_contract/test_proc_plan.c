#include "ksn_proc_plan.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <string.h>

#define I(op,dst,a,b,v,c) {op,dst,a,b,v,c}
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
static void compare(const ksn_proc_plan *plan,const float input[KSN_PROC_INPUTS],
                    const ksn_proc_state *state,bool debug){
    ksn_proc_vm a,b;
    ksn_proc_frame fa,fb;
    assert(ksn_proc_begin_state(&a,&plan->program,input,state,&fa)==KSN_PROC_RUNNING);
    assert(ksn_proc_plan_begin_state(&b,plan,input,state,&fb)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&a)==ksn_proc_plan_run(&b,plan,debug));
    equal_vm(&a,&b);
    if(a.status==KSN_PROC_DONE){
        ksn_proc_state sa,sb;
        assert(ksn_proc_capture_state(&a,&sa) && ksn_proc_capture_state(&b,&sb));
        assert(memcmp(sa.reg,sb.reg,sizeof sa.reg)==0);
        assert(sa.pen_x==sb.pen_x && sa.pen_y==sb.pen_y &&
               sa.pen_valid==sb.pen_valid);
    }
}
static void program_change_and_input(void){
    ksn_proc_inst code[]={
        I(KSN_PROC_INPUT,0,0,0,0,0), I(KSN_PROC_SET,1,0,0,2,0),
        I(KSN_PROC_ADD,2,0,1,0,0), I(KSN_PROC_MUL,3,2,1,0,0),
        I(KSN_PROC_SIN,4,3,0,0,0), I(KSN_PROC_ADD,5,4,2,0,0),
        I(KSN_PROC_MOVE,0,2,3,0,0), I(KSN_PROC_PLOT,0,2,5,0,12)
    };
    ksn_proc_program p={code,(uint8_t)(sizeof code/sizeof code[0])};
    ksn_proc_plan plan;
    assert(ksn_proc_plan_prepare(&plan,&p));assert(plan.fused_count>=2);
    code[1].value=80;code[3].op=KSN_PROC_ADD;
    for(unsigned n=0;n<9;n++){
        float input[KSN_PROC_INPUTS]={(float)n,0,0,0};
        compare(&plan,input,NULL,false);compare(&plan,input,NULL,true);
    }
    /* A different VM source must take the reference path. */
    {
        float input[KSN_PROC_INPUTS]={1,0,0,0};
        ksn_proc_vm a,b;ksn_proc_frame fa,fb;
        assert(ksn_proc_begin(&a,&p,input,&fa)==KSN_PROC_RUNNING);
        assert(ksn_proc_begin(&b,&p,input,&fb)==KSN_PROC_RUNNING);
        assert(ksn_proc_run(&a)==ksn_proc_plan_run(&b,&plan,false));equal_vm(&a,&b);
    }
}
static void loops_and_failure(void){
    ksn_proc_inst code[]={
        I(KSN_PROC_SET,0,0,0,1,0), I(KSN_PROC_SET,1,0,0,1,0),
        I(KSN_PROC_REPEAT_REG,0,2,0,0,0),
        I(KSN_PROC_ADD,0,0,1,0,0), I(KSN_PROC_MUL,3,0,1,0,0),
        I(KSN_PROC_PLOT,0,0,3,0,3),I(KSN_PROC_END,0,0,0,0,0)
    };
    ksn_proc_program p={code,(uint8_t)(sizeof code/sizeof code[0])};
    ksn_proc_plan plan;assert(ksn_proc_plan_prepare(&plan,&p));
    assert(plan.fused_count==1);
    for(unsigned n=0;n<=255;n+=17){
        float input[KSN_PROC_INPUTS]={0};ksn_proc_state state;
        ksn_proc_state_reset(&state);state.reg[2]=(float)n;
        compare(&plan,input,&state,false);
    }
    /* Intermediate overflow must fail before the second fused instruction. */
    {
        ksn_proc_inst overflow[]={I(KSN_PROC_MUL,0,0,1,0,0),
                                  I(KSN_PROC_ADD,2,0,1,0,0)};
        ksn_proc_program op={overflow,2};
        ksn_proc_state state;float input[KSN_PROC_INPUTS]={0};
        ksn_proc_state_reset(&state);state.reg[0]=FLT_MAX;state.reg[1]=2;
        assert(ksn_proc_plan_prepare(&plan,&op));compare(&plan,input,&state,false);
    }
}
static void limits(void){
    ksn_proc_inst code[]={
        I(KSN_PROC_SET,0,0,0,0,0),I(KSN_PROC_SET,1,0,0,1,0),
        I(KSN_PROC_REPEAT,0,255,0,0,0),
        I(KSN_PROC_REPEAT,0,255,0,0,0),
        I(KSN_PROC_ADD,0,0,1,0,0),I(KSN_PROC_MUL,2,0,1,0,0),
        I(KSN_PROC_END,0,0,0,0,0),I(KSN_PROC_END,0,0,0,0,0)
    };
    ksn_proc_program p={code,(uint8_t)(sizeof code/sizeof code[0])};
    ksn_proc_plan plan;float input[KSN_PROC_INPUTS]={0};
    assert(ksn_proc_plan_prepare(&plan,&p));assert(plan.fused_count==1);
    compare(&plan,input,NULL,false);
    /* Segment and raster caps are reached by ordinary VM drawing steps. */
    ksn_proc_inst draw[]={
        I(KSN_PROC_SET,0,0,0,-480,0),I(KSN_PROC_SET,1,0,0,720,0),
        I(KSN_PROC_ADD,2,0,1,0,0),I(KSN_PROC_MUL,3,2,1,0,0),
        I(KSN_PROC_MOVE,0,0,0,0,0),I(KSN_PROC_REPEAT,0,100,0,0,0),
        I(KSN_PROC_LINE,0,1,1,0,1),I(KSN_PROC_LINE,0,0,0,0,2),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    p=(ksn_proc_program){draw,(uint8_t)(sizeof draw/sizeof draw[0])};
    assert(ksn_proc_plan_prepare(&plan,&p));compare(&plan,input,NULL,false);
    ksn_proc_inst segments[]={
        I(KSN_PROC_SET,0,0,0,0,0),I(KSN_PROC_SET,1,0,0,1,0),
        I(KSN_PROC_ADD,2,0,1,0,0),I(KSN_PROC_MUL,3,2,1,0,0),
        I(KSN_PROC_REPEAT,0,5,0,0,0),I(KSN_PROC_REPEAT,0,255,0,0,0),
        I(KSN_PROC_PLOT,0,0,0,0,7),I(KSN_PROC_END,0,0,0,0,0),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    p=(ksn_proc_program){segments,(uint8_t)(sizeof segments/sizeof segments[0])};
    assert(ksn_proc_plan_prepare(&plan,&p));compare(&plan,input,NULL,false);
}
static void lifecycle_and_resume(void){
    const ksn_proc_inst code[]={I(KSN_PROC_ADD,0,0,1,0,0),
                                I(KSN_PROC_MUL,2,0,1,0,0)};
    const ksn_proc_program p={code,2};
    const float input[KSN_PROC_INPUTS]={0};
    ksn_proc_plan original,copy;
    assert(ksn_proc_plan_prepare(&original,&p));
    assert(ksn_proc_plan_prepare(&original,&original.program));
    assert(original.fused_count==1 && !memcmp(original.code,code,sizeof code));
    copy=original;
    original.code[0].op=KSN_PROC_SET;original.code[0].value=99;
    ksn_proc_state state;
    ksn_proc_state_reset(&state);state.reg[0]=3;state.reg[1]=2;
    ksn_proc_vm a,b;ksn_proc_frame fa,fb;
    assert(ksn_proc_begin_state(&a,&p,input,&state,&fa)==KSN_PROC_RUNNING);
    assert(ksn_proc_plan_begin_state(&b,&copy,input,&state,&fb)==KSN_PROC_RUNNING);
    /* An ordinary debugger step may stop inside the selected pair. */
    assert(ksn_proc_step(&a)==KSN_PROC_RUNNING);
    assert(ksn_proc_step(&b)==KSN_PROC_RUNNING);equal_vm(&a,&b);
    assert(b.reg[0]==5 && b.reg[2]==0 && b.steps==1 && b.last_pc==0);
    assert(ksn_proc_run(&a)==ksn_proc_plan_run(&b,&copy,false));equal_vm(&a,&b);
    assert(b.reg[2]==10 && b.steps==2 && b.last_pc==1);
    assert(ksn_proc_plan_begin(&b,NULL,input,&fb)==KSN_PROC_INVALID);
    assert(b.status==KSN_PROC_INVALID && !fb.ready && fb.count==0);
    assert(!ksn_proc_capture_state(&b,&state));
    /* Exhaustion on either side of a pair preserves the failure snapshot. */
    for(unsigned left=0;left<=2;left++){
        assert(ksn_proc_begin(&a,&p,input,&fa)==KSN_PROC_RUNNING);
        assert(ksn_proc_plan_begin(&b,&copy,input,&fb)==KSN_PROC_RUNNING);
        a.steps=b.steps=KSN_PROC_STEPS-left;
        assert(ksn_proc_run(&a)==ksn_proc_plan_run(&b,&copy,false));equal_vm(&a,&b);
        assert(b.status==(left==2?KSN_PROC_DONE:KSN_PROC_LIMIT));
        assert(b.pc==left && b.steps==KSN_PROC_STEPS);
    }
    /* The second arithmetic result can overflow independently of the first. */
    ksn_proc_state_reset(&state);state.reg[0]=FLT_MAX/2;state.reg[1]=2;
    assert(ksn_proc_begin_state(&a,&p,input,&state,&fa)==KSN_PROC_RUNNING);
    assert(ksn_proc_plan_begin_state(&b,&copy,input,&state,&fb)==KSN_PROC_RUNNING);
    /* FLT_MAX/2 times two is finite; choose three to overflow at MUL only. */
    a.reg[1]=b.reg[1]=3;
    assert(ksn_proc_run(&a)==ksn_proc_plan_run(&b,&copy,false));equal_vm(&a,&b);
    assert(b.status==KSN_PROC_INVALID && b.steps==2 && b.last_pc==1 && !fb.ready);
}
static void nested_break_and_replacement(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_REPEAT,0,3,0,0,0),I(KSN_PROC_REPEAT_REG,0,7,0,0,0),
        I(KSN_PROC_ADD,0,0,1,0,0),I(KSN_PROC_SIN,2,0,0,0,0),
        I(KSN_PROC_BREAK_IF_GT,0,0,3,0,0),I(KSN_PROC_END,0,0,0,0,0),
        I(KSN_PROC_MUL,4,2,1,0,0),I(KSN_PROC_ADD,5,4,1,0,0),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    const ksn_proc_program p={code,9};
    ksn_proc_plan plan;assert(ksn_proc_plan_prepare(&plan,&p));
    assert(plan.fused_count==2);
    const float input[KSN_PROC_INPUTS]={0};
    const float counts[]={0,1,5,255,-1,0.5f,256};
    ksn_proc_state state;
    for(unsigned n=0;n<sizeof counts/sizeof counts[0];n++){
        ksn_proc_state_reset(&state);state.reg[1]=1;state.reg[3]=2;
        state.reg[7]=counts[n];
        compare(&plan,input,&state,false);compare(&plan,input,&state,true);
    }
    /* A newly registered plan must not replace an already begun VM's code. */
    ksn_proc_vm a,b;ksn_proc_frame fa,fb;
    assert(ksn_proc_begin_state(&a,&p,input,&state,&fa)==KSN_PROC_RUNNING);
    assert(ksn_proc_plan_begin_state(&b,&plan,input,&state,&fb)==KSN_PROC_RUNNING);
    const ksn_proc_inst replacement[]={I(KSN_PROC_SET,0,0,0,123,0)};
    const ksn_proc_program other={replacement,1};
    assert(ksn_proc_plan_prepare(&plan,&other) && !plan.fused_count);
    assert(ksn_proc_run(&a)==ksn_proc_plan_run(&b,&plan,false));equal_vm(&a,&b);
    compare(&plan,input,NULL,false);
    /* Signed zero and subnormal arithmetic must survive the scalar pair. */
    const ksn_proc_inst tiny[]={I(KSN_PROC_MUL,0,0,1,0,0),
                                I(KSN_PROC_ADD,2,0,0,0,0)};
    const ksn_proc_program t={tiny,2};
    assert(ksn_proc_plan_prepare(&plan,&t));
    ksn_proc_state_reset(&state);state.reg[0]=-0.0f;state.reg[1]=1;
    compare(&plan,input,&state,false);
    state.reg[0]=FLT_MIN;state.reg[1]=0.5f;
    compare(&plan,input,&state,false);
}
int main(void){
    program_change_and_input();loops_and_failure();limits();lifecycle_and_resume();
    nested_break_and_replacement();
    return 0;
}
