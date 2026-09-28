#include "../../main/ui/kasane/ksn_proc_analysis.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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
    /* Every register is observable at exit; r0..r3 are defined first. */
    const unsigned all=(1u<<KSN_PROC_REGS)-1u;
    assert(a.block[0].live_out==all && a.block[0].live_in==(all&~15u));
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
    const ksn_proc_inst bad_reg[]={I(KSN_PROC_SET,KSN_PROC_REGS,0,0)};
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
    /* D nested one-trip loops around one SET: legal at D=KSN_PROC_LOOP_DEPTH,
     * rejected by both the analyzer and the VM at D+1. */
    const unsigned d=KSN_PROC_LOOP_DEPTH,n=2*d+1;
    const float input[KSN_PROC_INPUTS]={0};
    ksn_proc_vm vm;ksn_proc_frame frame;
    for(unsigned j=0;j<d;j++)
        code[j]=(ksn_proc_inst)I(KSN_PROC_REPEAT,0,1,0);
    code[d]=(ksn_proc_inst)I(KSN_PROC_SET,KSN_PROC_REGS-1,0,0);
    for(unsigned j=0;j<d;j++)
        code[d+1+j]=(ksn_proc_inst)I(KSN_PROC_END,0,0,0);
    a=analyze(code,n);
    assert(a.loop_count==d && a.max_steps==n);
    ksn_proc_program p={code,(uint8_t)n};
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE && vm.steps==n);
    ksn_proc_inst deeper[KSN_PROC_CODE];
    for(unsigned j=0;j<=d;j++)deeper[j]=(ksn_proc_inst)I(KSN_PROC_REPEAT,0,1,0);
    deeper[d+1]=(ksn_proc_inst)I(KSN_PROC_SET,0,0,0);
    for(unsigned j=0;j<=d;j++)deeper[d+2+j]=(ksn_proc_inst)I(KSN_PROC_END,0,0,0);
    p=(ksn_proc_program){deeper,(uint8_t)(n+2)};
    assert(!ksn_proc_analyze(&p,&a));
    assert(a.program_reasons & KSN_PA_INVALID);
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_INVALID);
    code[d]=(ksn_proc_inst)I(KSN_PROC_REPEAT,0,1,0);
    p=(ksn_proc_program){code,(uint8_t)n};
    assert(!ksn_proc_analyze(&p,&a));
}
/* Registers above r7 are tracked, not truncated: an 8-bit mask would lose
 * them silently. Every claim is checked against the VM by perturbation: a
 * register outside an instruction's read set cannot change what it does, and
 * one outside live_in at entry cannot change the finished state or frame. */
static bool same_run(const ksn_proc_program *p,const ksn_proc_state *s0,
                     const ksn_proc_state *s1,const float input[KSN_PROC_INPUTS],
                     bool compare_regs){
    static ksn_proc_frame f0,f1;
    ksn_proc_vm v0,v1;ksn_proc_state o0,o1;
    assert(ksn_proc_begin_state(&v0,p,input,s0,&f0)==KSN_PROC_RUNNING);
    assert(ksn_proc_begin_state(&v1,p,input,s1,&f1)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&v0)==KSN_PROC_DONE && ksn_proc_run(&v1)==KSN_PROC_DONE);
    assert(ksn_proc_capture_state(&v0,&o0) && ksn_proc_capture_state(&v1,&o1));
    if((compare_regs&&memcmp(o0.reg,o1.reg,sizeof o0.reg))||o0.pen_x!=o1.pen_x||o0.pen_y!=o1.pen_y)
        return false;
    return f0.count==f1.count &&
           !memcmp(f0.segments,f1.segments,f0.count*sizeof f0.segments[0]);
}
static void high_registers(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_INPUT,8,7,0),          /* r8 = input[7] */
        I(KSN_PROC_ADD,9,8,15),           /* r9 = r8 + r15 (r15 live-in) */
        I(KSN_PROC_MUL,14,9,12),          /* r14 = r9 * r12 (r12 live-in) */
        I(KSN_PROC_REPEAT,0,3,0),
        I(KSN_PROC_ADD,10,10,11),         /* r10 carried, r11 live-in */
        I(KSN_PROC_END,0,0,0),
        I(KSN_PROC_MOVE,0,14,13),         /* pen reads r14, r13 */
        I(KSN_PROC_CUBIC,0,4,0)           /* reads exactly r0..r7 */
    };
    ksn_proc_analysis a=analyze(code,sizeof code/sizeof code[0]);
    assert(a.inst[0].writes==(1u<<8) && a.inst[0].reads==0);
    assert(a.inst[1].reads==((1u<<8)|(1u<<15)) && a.inst[1].writes==(1u<<9));
    assert(a.inst[2].reads==((1u<<9)|(1u<<12)) && a.inst[2].writes==(1u<<14));
    assert(a.inst[6].reads==((1u<<14)|(1u<<13)));
    assert(a.inst[7].reads==0xffu);
    assert(a.loop[0].carried==(1u<<10) && (a.loop[0].reasons & KSN_PA_LOOP_CARRIED));
    const unsigned live_entry=0xffu|(1u<<10)|(1u<<11)|(1u<<12)|(1u<<13)|(1u<<15);
    assert(a.block[0].live_in==live_entry);
    /* r9 and r14 are written before any read: dead at entry. */
    assert(!(a.block[0].live_in & (1u<<9)) && !(a.block[0].live_in & (1u<<14)));

    ksn_proc_program p={code,sizeof code/sizeof code[0]};
    float input[KSN_PROC_INPUTS]={0};
    input[7]=4;
    ksn_proc_state base;ksn_proc_state_reset(&base);
    for(unsigned r=0;r<KSN_PROC_REGS;r++)base.reg[r]=(float)(10+r*3);
    base.reg[12]=2; /* keeps the MOVE coordinate r14=(4+r15)*r12 on -480..720 */
    for(unsigned r=0;r<KSN_PROC_REGS;r++){
        ksn_proc_state s=base;s.reg[r]+=1.0f;
        bool same=same_run(&p,&base,&s,input,true);
        /* Dead at entry must be invisible. Every live register in this
         * program does reach the output, so liveness is exact here. */
        assert(same==!(live_entry & (1u<<r)));
    }
    /* Reads of CUBIC alone: r8..r15 never influence the emitted curve or pen
     * (they are carried through unchanged, so final registers differ). */
    const ksn_proc_inst curve[]={I(KSN_PROC_CUBIC,0,4,0)};
    ksn_proc_program q={curve,1};
    for(unsigned r=8;r<KSN_PROC_REGS;r++){
        ksn_proc_state s=base;s.reg[r]+=100.0f;
        assert(same_run(&q,&base,&s,input,false));
    }
    /* Read sets per instruction: perturbing a non-read register just before
     * the step leaves its destination and the pen unchanged. */
    for(unsigned pc=0;pc<p.count;pc++){
        if(code[pc].op==KSN_PROC_REPEAT||code[pc].op==KSN_PROC_END)continue;
        for(unsigned r=0;r<KSN_PROC_REGS;r++){
            if(a.inst[pc].reads & (1u<<r))continue;
            if(a.inst[pc].writes & (1u<<r))continue;
            static ksn_proc_frame f0,f1;
            ksn_proc_vm v0,v1;
            const ksn_proc_program one={&code[pc],1};
            ksn_proc_state s=base;s.reg[r]+=1.0f;
            assert(ksn_proc_begin_state(&v0,&one,input,&base,&f0)==KSN_PROC_RUNNING);
            assert(ksn_proc_begin_state(&v1,&one,input,&s,&f1)==KSN_PROC_RUNNING);
            assert(ksn_proc_run(&v0)==KSN_PROC_DONE && ksn_proc_run(&v1)==KSN_PROC_DONE);
            for(unsigned w=0;w<KSN_PROC_REGS;w++)
                if(a.inst[pc].writes & (1u<<w))assert(v0.reg[w]==v1.reg[w]);
            assert(v0.pen_x==v1.pen_x && v0.pen_y==v1.pen_y && f0.count==f1.count &&
                   !memcmp(f0.segments,f1.segments,f0.count*sizeof f0.segments[0]));
        }
    }
}
static void validation_parity(void){
    /* Differential validation only: the independent semantic oracle remains
     * responsible for execution behavior. Put every opcode in a legal loop
     * context and vary all register/immediate fields across their bounds. */
    /* 7/8 straddle the input count, 15/16 the register count. */
    const uint8_t values[]={0,1,3,4,7,8,15,16,255};
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
    validation_parity();high_registers();
    puts("proc analysis: ok");
    return 0;
}
