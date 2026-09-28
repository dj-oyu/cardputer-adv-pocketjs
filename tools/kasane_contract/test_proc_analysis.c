#include "../../main/ui/kasane/ksn_proc_analysis.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

#define I(op,d,a,b) {op,d,a,b,0.0f,0}

static ksn_proc_analysis analyze(const ksn_proc_inst *code,unsigned count){
    ksn_proc_analysis a;
    ksn_proc_program p={code,(uint8_t)count};
    assert(ksn_proc_analyze(&p,&a));
    assert(a.valid && a.count==count);
    return a;
}
static void basic_flow(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_INPUT,0,0,0),I(KSN_PROC_SET,1,0,0),
        I(KSN_PROC_ADD,2,0,1),I(KSN_PROC_SET,3,0,0)
    };
    ksn_proc_analysis a=analyze(code,4);
    assert(a.block_count==1 && a.max_steps==4);
    assert(a.block[0].use==0 && a.block[0].def==15);
    assert(a.block[0].live_out==255 && a.block[0].live_in==240);
    assert(a.inst[0].relocatable && a.inst[1].relocatable);
    assert(!a.inst[2].relocatable);
    assert(a.inst[2].failure & KSN_PA_FAIL_FINITE);
    assert(a.inst[3].relocation_reasons & KSN_PA_REG_DEP);
    assert(a.program_reasons & KSN_PA_OBSERVABLE_REG);
}
static void loops_and_break(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_REPEAT_REG,0,7,0),
        I(KSN_PROC_SET,0,0,0),
        I(KSN_PROC_BREAK_IF_GT,0,0,1),
        I(KSN_PROC_ADD,2,2,0),
        I(KSN_PROC_END,0,0,0),
        I(KSN_PROC_SET,3,0,0)
    };
    ksn_proc_analysis a=analyze(code,6);
    assert(a.loop_count==1 && a.loop[0].begin==0 && a.loop[0].end==4);
    assert(a.loop[0].carried & (1u<<2));
    assert(a.loop[0].reasons & KSN_PA_CONTROL);
    assert(a.inst[0].successor_count==2 && a.inst[0].successor[1]==5);
    assert(a.inst[2].successor_count==2 && a.inst[2].successor[0]==5);
    assert(a.inst[4].successor_count==2 && a.inst[4].successor[0]==1);
    assert(a.block_count>=5);
    assert(!a.inst[3].loop_independent);
    assert(a.inst[0].failure & KSN_PA_FAIL_COUNT);
}
static void independent_loop(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_REPEAT,0,3,0),I(KSN_PROC_SET,0,0,0),
        I(KSN_PROC_INPUT,1,0,0),I(KSN_PROC_END,0,0,0)
    };
    ksn_proc_analysis a=analyze(code,4);
    assert(a.max_steps==10);
    assert(a.loop[0].carried==0 && a.loop[0].independent);
    assert(a.inst[1].loop_independent && a.inst[2].loop_independent);
    assert(a.inst[1].relocatable && a.inst[2].relocatable);
}
static void dynamic_header_failure(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_REPEAT_REG,0,7,0),I(KSN_PROC_SET,0,0,0),
        I(KSN_PROC_END,0,0,0)
    };
    ksn_proc_analysis a=analyze(code,3);
    assert(a.max_steps==511);
    assert(a.loop[0].reasons & KSN_PA_MAY_FAIL);
    assert(!a.loop[0].independent && !a.inst[1].loop_independent);
}
static void step_boundary(void){
    /* 1 + 100*(1 + 49*2 + 1) == 10001; reducing the outer
     * count by one proves 9901. Include the deepest product saturation. */
    ksn_proc_inst code[]={
        I(KSN_PROC_REPEAT,0,100,0),I(KSN_PROC_REPEAT,0,49,0),
        I(KSN_PROC_SET,0,0,0),I(KSN_PROC_END,0,0,0),
        I(KSN_PROC_END,0,0,0)
    };
    ksn_proc_analysis a=analyze(code,5);
    assert(!a.max_steps && (a.program_reasons & KSN_PA_STEP_LIMIT));
    code[0].a=99;
    a=analyze(code,5);
    assert(a.max_steps==9901 && !(a.program_reasons & KSN_PA_STEP_LIMIT));
    ksn_proc_inst deep[]={
        I(KSN_PROC_REPEAT,0,255,0),I(KSN_PROC_REPEAT,0,255,0),
        I(KSN_PROC_REPEAT,0,255,0),I(KSN_PROC_REPEAT,0,255,0),
        I(KSN_PROC_END,0,0,0),I(KSN_PROC_END,0,0,0),
        I(KSN_PROC_END,0,0,0),I(KSN_PROC_END,0,0,0)
    };
    a=analyze(deep,8);
    assert(!a.max_steps && (a.program_reasons & KSN_PA_STEP_LIMIT));
}
static void drawing_and_limit(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_REPEAT,0,255,0),
        I(KSN_PROC_REPEAT,0,255,0),
        I(KSN_PROC_MOVE,0,0,1),
        I(KSN_PROC_LINE_COLOR_REG,2,0,1),
        I(KSN_PROC_END,0,0,0),I(KSN_PROC_END,0,0,0)
    };
    ksn_proc_analysis a=analyze(code,6);
    assert(a.max_steps==0);
    assert(a.program_reasons & KSN_PA_STEP_LIMIT);
    assert(a.inst[2].effects & KSN_PA_EFFECT_PEN);
    assert(a.inst[3].effects & KSN_PA_EFFECT_DRAW);
    assert((a.inst[3].failure & (KSN_PA_FAIL_COORD|KSN_PA_FAIL_COLOR|
           KSN_PA_FAIL_SEGMENTS|KSN_PA_FAIL_RASTER|KSN_PA_FAIL_STEPS))==
           (KSN_PA_FAIL_COORD|KSN_PA_FAIL_COLOR|KSN_PA_FAIL_SEGMENTS|
            KSN_PA_FAIL_RASTER|KSN_PA_FAIL_STEPS));
    assert(a.loop[0].reasons & KSN_PA_PEN);
    assert(a.loop[1].reasons & KSN_PA_DRAW_ORDER);
    assert(!a.inst[2].relocatable && !a.inst[3].loop_independent);
}
static void invalid_input(void){
    ksn_proc_analysis a;
    const ksn_proc_inst broken[]={I(KSN_PROC_BREAK_IF_GT,0,0,1)};
    ksn_proc_program p={broken,1};
    assert(!ksn_proc_analyze(&p,&a));
    assert(!a.valid && (a.program_reasons & KSN_PA_INVALID));
    const ksn_proc_inst bad_reg[]={I(KSN_PROC_SET,8,0,0)};
    p.code=bad_reg;
    assert(!ksn_proc_analyze(&p,&a));
    assert(!a.valid);
    assert(!ksn_proc_analyze(NULL,&a));
    assert(!ksn_proc_analyze(&p,NULL));
}
static void bounds(void){
    ksn_proc_inst code[KSN_PROC_CODE];
    for(unsigned j=0;j<KSN_PROC_CODE;j++)
        code[j]=(ksn_proc_inst)I(KSN_PROC_SET,0,0,0);
    ksn_proc_analysis a=analyze(code,KSN_PROC_CODE);
    assert(a.max_steps==KSN_PROC_CODE && a.block_count==1);
    for(unsigned j=0;j<KSN_PROC_LOOP_DEPTH;j++)
        code[j]=(ksn_proc_inst)I(KSN_PROC_REPEAT,0,1,0);
    code[4]=(ksn_proc_inst)I(KSN_PROC_SET,7,0,0);
    for(unsigned j=0;j<KSN_PROC_LOOP_DEPTH;j++)
        code[5+j]=(ksn_proc_inst)I(KSN_PROC_END,0,0,0);
    a=analyze(code,9);
    assert(a.loop_count==KSN_PROC_LOOP_DEPTH && a.max_steps==9);
    code[4]=(ksn_proc_inst)I(KSN_PROC_REPEAT,0,1,0);
    ksn_proc_program p={code,9};
    assert(!ksn_proc_analyze(&p,&a));
    assert(a.program_reasons & KSN_PA_INVALID);
}
static void validation_parity(void){
    /* Differential validation only: the independent semantic oracle remains
     * responsible for execution behavior. Put every opcode in a legal loop
     * context and vary all register/immediate fields across their bounds. */
    const uint8_t values[]={0,1,3,4,7,8,255};
    const float input[KSN_PROC_INPUTS]={0};
    for(unsigned op=0;op<=KSN_PROC_LINE_COLOR_REG+1;op++){
        for(unsigned field=0;field<3;field++){
            for(unsigned k=0;k<sizeof values/sizeof values[0];k++){
                ksn_proc_inst code[]={
                    I(KSN_PROC_REPEAT,0,1,0),I(KSN_PROC_SET,0,0,0),
                    I(KSN_PROC_END,0,0,0),I(KSN_PROC_END,0,0,0)
                };
                code[1].op=(uint8_t)op;
                code[1].a=1;
                if(field==0)code[1].dst=values[k];
                if(field==1)code[1].a=values[k];
                if(field==2)code[1].b=values[k];
                uint8_t count=(op==KSN_PROC_REPEAT||op==KSN_PROC_REPEAT_REG)?4:3;
                ksn_proc_program p={code,count};
                ksn_proc_analysis a;
                ksn_proc_vm vm;
                ksn_proc_frame frame;
                bool valid=ksn_proc_analyze(&p,&a);
                assert(valid==(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING));
            }
        }
    }
    ksn_proc_inst code=I(KSN_PROC_SET,0,0,0);
    const float bad[]={INFINITY,-INFINITY,NAN};
    for(unsigned j=0;j<sizeof bad/sizeof bad[0];j++){
        code.value=bad[j];
        ksn_proc_program p={&code,1};
        ksn_proc_analysis a;
        ksn_proc_vm vm;
        ksn_proc_frame frame;
        assert(!ksn_proc_analyze(&p,&a));
        assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_INVALID);
    }
}
int main(void){
    basic_flow();loops_and_break();independent_loop();drawing_and_limit();
    invalid_input();bounds();dynamic_header_failure();step_boundary();
    validation_parity();
    puts("proc analysis: ok");
    return 0;
}
