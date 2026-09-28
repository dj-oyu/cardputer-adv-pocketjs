/* Structural probes for the host-only procedural IR. These are small semantic
 * examples, not throughput tests. Compile with ksn_procedural.c and -lm. */
#include "ksn_procedural.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define I(OP,D,A,B,V,C) {OP,D,A,B,V,C}
static ksn_proc_frame frame;
static ksn_proc_vm vm;
static uint16_t pixels[KSN_PROC_W*KSN_PROC_H];

static void grid(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_SET,0,0,0,10,0),I(KSN_PROC_SET,1,0,0,20,0),
        I(KSN_PROC_SET,2,0,0,2,0),I(KSN_PROC_SET,3,0,0,3,0),
        I(KSN_PROC_SET,4,0,0,10,0),I(KSN_PROC_SET,5,0,0,0,0),
        I(KSN_PROC_REPEAT,0,3,0,0,0),
          I(KSN_PROC_ADD,0,4,5,0,0),
          I(KSN_PROC_REPEAT,0,4,0,0,0),
            I(KSN_PROC_PLOT,0,0,1,0,0xffff),I(KSN_PROC_ADD,0,0,2,0,0),
          I(KSN_PROC_END,0,0,0,0,0),
          I(KSN_PROC_ADD,1,1,3,0,0),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    ksn_proc_program p={code,sizeof code/sizeof code[0]};
    const float input[KSN_PROC_INPUTS]={0};
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    unsigned deepest=0;
    while(vm.status==KSN_PROC_RUNNING){
        ksn_proc_step(&vm);
        if(vm.depth>deepest)deepest=vm.depth;
    }
    assert(vm.status==KSN_PROC_DONE&&deepest==2&&frame.count==12);
    for(unsigned n=0;n<12;n++){
        const ksn_proc_segment *s=&frame.segments[n];
        assert(s->x0==10+(int)(n%4)*2&&s->y0==20+(int)(n/4)*3);
    }
    printf("GRID_OK: nested loops depth=%u, points=%u, steps=%u\n",deepest,frame.count,vm.steps);
}
static void feedback(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_SET,0,0,0,20,0),I(KSN_PROC_SET,1,0,0,20,0),
        I(KSN_PROC_SET,2,0,0,2,0),I(KSN_PROC_SET,3,0,0,.05f,0),
        I(KSN_PROC_REPEAT,0,8,0,0,0),
          I(KSN_PROC_MUL,4,0,3,0,0),I(KSN_PROC_ADD,1,1,4,0,0),
          I(KSN_PROC_LINE,0,0,1,0,0x07e0),I(KSN_PROC_ADD,0,0,2,0,0),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    ksn_proc_program p={code,sizeof code/sizeof code[0]};
    const float input[KSN_PROC_INPUTS]={0};
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&frame.count==7);
    assert(frame.segments[0].x0==20&&frame.segments[6].x1==34);
    assert(frame.segments[0].y0<frame.segments[6].y1);
    printf("FEEDBACK_OK: state carried across iterations, segments=%u\n",frame.count);
}
static void path_break(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_SET,0,0,0,10,0),I(KSN_PROC_SET,1,0,0,10,0),
        I(KSN_PROC_MOVE,0,0,1,0,0),
        I(KSN_PROC_SET,0,0,0,20,0),I(KSN_PROC_LINE,0,0,1,0,0xffff),
        I(KSN_PROC_SET,1,0,0,30,0),I(KSN_PROC_MOVE,0,0,1,0,0),
        I(KSN_PROC_SET,0,0,0,30,0),I(KSN_PROC_LINE,0,0,1,0,0xffff)
    };
    ksn_proc_program p={code,sizeof code/sizeof code[0]};
    const float input[KSN_PROC_INPUTS]={0};
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&frame.count==2);
    memset(pixels,0,sizeof pixels);
    assert(ksn_proc_render_band(&frame,pixels,0,KSN_PROC_H));
    assert(pixels[10*KSN_PROC_W+15]&&pixels[30*KSN_PROC_W+25]);
    assert(!pixels[20*KSN_PROC_W+20]);
    printf("PATH_OK: MOVE splits paths without a connecting segment\n");
}
static void escape_time(void){
    /* A minimal escape-time orbit: z <- z*z + c. A real renderer would stop an
     * individual orbit as soon as it escapes and use that iteration as colour. */
    const ksn_proc_inst code[]={
        I(KSN_PROC_SET,0,0,0,0,0),I(KSN_PROC_INPUT,1,0,0,0,0),
        I(KSN_PROC_SET,2,0,0,1,0),I(KSN_PROC_SET,3,0,0,1,0),
        I(KSN_PROC_PLOT,0,2,3,0,0xffff),
        I(KSN_PROC_REPEAT,0,16,0,0,0),
          I(KSN_PROC_MUL,0,0,0,0,0),I(KSN_PROC_ADD,0,0,1,0,0),
        I(KSN_PROC_END,0,0,0,0,0),
        I(KSN_PROC_PLOT,0,2,3,0,0xf800)
    };
    ksn_proc_program p={code,sizeof code/sizeof code[0]};
    float reference_z=0;unsigned reference_escape=0;
    for(unsigned n=1;n<=16;n++){
        reference_z=reference_z*reference_z+2;
        if(reference_z>2){reference_escape=n;break;}
    }
    assert(reference_escape==2);
    float input[KSN_PROC_INPUTS]={-1,0,0,0};
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&frame.ready&&frame.count==2);
    input[0]=2;
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_INVALID);
    assert(!frame.ready&&frame.count==1&&vm.last_pc==6);
    printf("ESCAPE_GAP: c=2 should bail at iteration %u; fixed repeat overflows at pc=%u after %u steps and invalidates the frame\n",
           reference_escape,vm.last_pc,vm.steps);

    const ksn_proc_inst bounded[]={
        I(KSN_PROC_SET,0,0,0,0,0),I(KSN_PROC_INPUT,1,0,0,0,0),
        I(KSN_PROC_SET,2,0,0,2,0),I(KSN_PROC_SET,3,0,0,0,0),
        I(KSN_PROC_SET,4,0,0,1,0),I(KSN_PROC_INPUT,5,1,0,0,0),
        I(KSN_PROC_SET,6,0,0,20,0),
        I(KSN_PROC_REPEAT_REG,0,5,0,0,0),
          I(KSN_PROC_MUL,0,0,0,0,0),I(KSN_PROC_ADD,0,0,1,0,0),
          I(KSN_PROC_ADD,3,3,4,0,0),I(KSN_PROC_BREAK_IF_GT,0,0,2,0,0),
        I(KSN_PROC_END,0,0,0,0,0),
        I(KSN_PROC_PLOT_COLOR_REG,3,3,6,0,0)
    };
    ksn_proc_program bp={bounded,sizeof bounded/sizeof bounded[0]};
    input[0]=2;input[1]=16;
    assert(ksn_proc_begin(&vm,&bp,input,&frame)==KSN_PROC_RUNNING);
    bool saw_break=false;
    while(vm.status==KSN_PROC_RUNNING){
        ksn_proc_step(&vm);
        if(vm.last_pc==11&&vm.pc==13&&vm.depth==0)saw_break=true;
    }
    assert(saw_break&&vm.status==KSN_PROC_DONE&&vm.reg[3]==2);
    assert(frame.ready&&frame.count==1&&frame.segments[0].x0==2);
    assert(frame.segments[0].color==2);
    input[0]=-1;
    assert(ksn_proc_begin(&vm,&bp,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&vm.reg[3]==16);
    assert(frame.count==1&&frame.segments[0].x0==16);
    assert(frame.segments[0].color==16);
    printf("ESCAPE_FIXED: c=2 exits after 2 iterations; c=-1 uses all 16; RGB565 attribute reads iteration register\n");
}
static void frame_state(void){
    /* A particle's position is integrated during one frame. The next begin
     * clears the registers, so continuing it requires an external readback. */
    const ksn_proc_inst code[]={
        I(KSN_PROC_INPUT,0,0,0,0,0),I(KSN_PROC_INPUT,1,1,0,0,0),
        I(KSN_PROC_SET,2,0,0,20,0),I(KSN_PROC_ADD,0,0,1,0,0),
        I(KSN_PROC_PLOT,0,0,2,0,0xffff)
    };
    ksn_proc_program p={code,sizeof code/sizeof code[0]};
    float input[KSN_PROC_INPUTS]={10,2,0,0};
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&frame.segments[0].x0==12);
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&frame.segments[0].x0==12);
    input[0]=vm.reg[0];
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&frame.segments[0].x0==14);
    printf("STATE_GAP: repeated frame input stays at x=12; x=14 requires external register readback\n");

    const ksn_proc_inst native_code[]={
        I(KSN_PROC_SET,1,0,0,2,0),I(KSN_PROC_SET,2,0,0,20,0),
        I(KSN_PROC_ADD,0,0,1,0,0),I(KSN_PROC_PLOT,0,0,2,0,0xffff)
    };
    ksn_proc_program native={native_code,4};
    ksn_proc_state state;
    ksn_proc_state_reset(&state);state.reg[0]=10;
    const float no_input[KSN_PROC_INPUTS]={0};
    assert(ksn_proc_begin_state(&vm,&native,no_input,&state,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&frame.segments[0].x0==12);
    assert(ksn_proc_capture_state(&vm,&state)&&state.reg[0]==12);
    assert(ksn_proc_begin_state(&vm,&native,no_input,&state,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&frame.segments[0].x0==14);
    assert(ksn_proc_capture_state(&vm,&state)&&state.reg[0]==14);
    printf("STATE_FIXED: native state carries x=12 to x=14 across two prepared frames\n");
}
static void mutable_program(void){
    /* Validation at begin is not a registration copy. A paused debugger can
     * observe changed instructions if the producer still owns the memory. */
    ksn_proc_inst code[]={
        I(KSN_PROC_SET,0,0,0,10,0),I(KSN_PROC_SET,1,0,0,20,0),
        I(KSN_PROC_PLOT,0,0,1,0,0xffff)
    };
    ksn_proc_program p={code,sizeof code/sizeof code[0]};
    const float input[KSN_PROC_INPUTS]={0};
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_step(&vm)==KSN_PROC_RUNNING);
    code[1].value=30;
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
    assert(frame.segments[0].x0==10&&frame.segments[0].y0==20);
    printf("PROGRAM_LIFETIME_FIXED: producer changes instruction during pause; owned copy still renders y=20\n");
}
static void dynamic_count(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_SET,0,0,0,10,0),I(KSN_PROC_SET,1,0,0,20,0),
        I(KSN_PROC_SET,2,0,0,1,0),I(KSN_PROC_INPUT,3,0,0,0,0),
        I(KSN_PROC_REPEAT,0,3,0,0,0),
          I(KSN_PROC_PLOT,0,0,1,0,0xffff),I(KSN_PROC_ADD,0,0,2,0,0),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    ksn_proc_program p={code,sizeof code/sizeof code[0]};
    float input[KSN_PROC_INPUTS]={3,0,0,0};
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&frame.count==3);
    input[0]=5;
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&vm.reg[3]==5&&frame.count==3);
    ksn_proc_inst variable_code[sizeof code/sizeof code[0]];
    memcpy(variable_code,code,sizeof code);
    variable_code[4].op=KSN_PROC_REPEAT_REG;
    ksn_proc_program variable={variable_code,sizeof variable_code/sizeof variable_code[0]};
    assert(ksn_proc_begin(&vm,&variable,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&frame.count==5);
    printf("COUNT_FIXED: input=5 yields 3 points with immediate REPEAT and 5 with REPEAT_REG\n");
}
static void candidate_failure(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_SET,0,0,0,10,0),I(KSN_PROC_SET,1,0,0,10,0),
        I(KSN_PROC_PLOT,0,0,1,0,0xf800)
    };
    ksn_proc_program p={code,sizeof code/sizeof code[0]};
    float input[KSN_PROC_INPUTS]={0};
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&frame.ready&&frame.count==1);
    input[0]=NAN;
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_INVALID);
    assert(!frame.ready&&frame.count==0);
    printf("CANDIDATE_GAP: failed prepare in the same frame storage destroys the prior replayable frame\n");
}
static void moving_damage(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_INPUT,0,0,0,0,0),I(KSN_PROC_SET,1,0,0,10,0),
        I(KSN_PROC_PLOT,0,0,1,0,0xf800)
    };
    ksn_proc_program p={code,sizeof code/sizeof code[0]};
    float input[KSN_PROC_INPUTS]={10,0,0,0};
    memset(pixels,0,sizeof pixels);
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
    assert(ksn_proc_render_band(&frame,pixels,0,KSN_PROC_H));
    input[0]=20;
    assert(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
    assert(ksn_proc_render_band(&frame,pixels,0,KSN_PROC_H));
    assert(pixels[10*KSN_PROC_W+10]==0xf800&&pixels[10*KSN_PROC_W+20]==0xf800);
    printf("DAMAGE_GAP: replaying the new moving frame over old pixels leaves the old x=10 pixel visible\n");
}
static void depth_order(void){
    /* Red is the imagined near line, green the far line. There is no depth in
     * the segment record, so reversing submission reverses the intersection. */
    const ksn_proc_inst red_then_green[]={
        I(KSN_PROC_SET,0,0,0,10,0),I(KSN_PROC_SET,1,0,0,50,0),
        I(KSN_PROC_MOVE,0,0,1,0,0),I(KSN_PROC_SET,0,0,0,30,0),
        I(KSN_PROC_LINE,0,0,1,0,0xf800),
        I(KSN_PROC_SET,0,0,0,20,0),I(KSN_PROC_SET,1,0,0,40,0),
        I(KSN_PROC_MOVE,0,0,1,0,0),I(KSN_PROC_SET,1,0,0,60,0),
        I(KSN_PROC_LINE,0,0,1,0,0x07e0)
    };
    const ksn_proc_inst green_then_red[]={
        I(KSN_PROC_SET,0,0,0,20,0),I(KSN_PROC_SET,1,0,0,40,0),
        I(KSN_PROC_MOVE,0,0,1,0,0),I(KSN_PROC_SET,1,0,0,60,0),
        I(KSN_PROC_LINE,0,0,1,0,0x07e0),
        I(KSN_PROC_SET,0,0,0,10,0),I(KSN_PROC_SET,1,0,0,50,0),
        I(KSN_PROC_MOVE,0,0,1,0,0),I(KSN_PROC_SET,0,0,0,30,0),
        I(KSN_PROC_LINE,0,0,1,0,0xf800)
    };
    const float input[KSN_PROC_INPUTS]={0};
    ksn_proc_program far_last={red_then_green,10},near_last={green_then_red,10};
    assert(ksn_proc_begin(&vm,&far_last,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
    memset(pixels,0,sizeof pixels);
    assert(ksn_proc_render_band(&frame,pixels,0,KSN_PROC_H));
    assert(pixels[50*KSN_PROC_W+20]==0x07e0);
    assert(ksn_proc_begin(&vm,&near_last,input,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
    memset(pixels,0,sizeof pixels);
    assert(ksn_proc_render_band(&frame,pixels,0,KSN_PROC_H));
    assert(pixels[50*KSN_PROC_W+20]==0xf800);
    printf("DEPTH_GAP: same crossing changes from green to red when instruction order reverses; no depth test exists\n");
}
int main(void){
    grid();feedback();path_break();escape_time();
    frame_state();mutable_program();dynamic_count();
    candidate_failure();moving_damage();depth_order();
}
