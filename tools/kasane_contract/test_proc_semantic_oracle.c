/* Frozen observable contract for the pre-optimization procedural VM.
 * Build with ksn_procedural.c, -std=c11 -Wall -Wextra -Werror -lm.
 * Expected values below are written from the programs' semantics, not obtained
 * by running another VM or by comparing two implementations with each other.
 */
#include "ksn_procedural.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define I(OP,D,A,B,V,C) {OP,D,A,B,V,C}
#define LEN(A) (sizeof(A)/sizeof((A)[0]))
#define CHECK(X) do { if (!(X)) { \
    fprintf(stderr,"semantic oracle: %s:%d: %s\n",__FILE__,__LINE__,#X); \
    exit(1); } } while (0)

typedef struct { int16_t x0,y0,x1,y1; uint16_t color; } expected_segment;

static void segments_are(const ksn_proc_frame *f,const expected_segment *want,
                         size_t count,uint16_t raster_steps,bool ready){
    CHECK(f->ready==ready);
    CHECK(f->count==count);
    CHECK(f->raster_steps==raster_steps);
    for(size_t j=0;j<count;j++){
        const ksn_proc_segment *s=&f->segments[j];
        CHECK(s->x0==want[j].x0&&s->y0==want[j].y0);
        CHECK(s->x1==want[j].x1&&s->y1==want[j].y1);
        CHECK(s->color==want[j].color);
    }
}

static void state_is(const ksn_proc_vm *vm,const float reg[KSN_PROC_REGS],
                     int16_t pen_x,int16_t pen_y,bool pen_valid){
    ksn_proc_state got;
    memset(&got,0xa5,sizeof got);
    CHECK(ksn_proc_capture_state(vm,&got));
    for(unsigned j=0;j<KSN_PROC_REGS;j++){
        if(got.reg[j]!=reg[j])fprintf(stderr,"reg[%u]: got %.9g, want %.9g\n",
                                    j,(double)got.reg[j],(double)reg[j]);
        CHECK(got.reg[j]==reg[j]);
    }
    CHECK(got.pen_x==pen_x&&got.pen_y==pen_y&&got.pen_valid==pen_valid);
}

static void no_state(const ksn_proc_vm *vm){
    ksn_proc_state state,before;
    memset(&state,0x5a,sizeof state);
    before=state;
    CHECK(!ksn_proc_capture_state(vm,&state));
    CHECK(!memcmp(&state,&before,sizeof state));
}

/* ADD and MUL carry two recurrences across unrelated SET and PLOT commands.
 * The complete PC/depth trace also fixes END's implicit back edge and the
 * fact that a debugger step is one logical instruction, including END. */
static void recurrence_and_trace(void){
    static const ksn_proc_inst code[]={
        I(KSN_PROC_SET,0,0,0,1,0), I(KSN_PROC_SET,1,0,0,2,0),
        I(KSN_PROC_SET,2,0,0,3,0), I(KSN_PROC_SET,3,0,0,20,0),
        I(KSN_PROC_SET,4,0,0,30,0), I(KSN_PROC_REPEAT,0,3,0,0,0),
        I(KSN_PROC_ADD,0,0,1,0,0), I(KSN_PROC_SET,7,0,0,99,0),
        I(KSN_PROC_MUL,1,1,2,0,0), I(KSN_PROC_PLOT,0,0,3,0,0xf800),
        I(KSN_PROC_END,0,0,0,0,0), I(KSN_PROC_MOVE,0,4,3,0,0),
        I(KSN_PROC_LINE,0,0,3,0,0x001f)
    };
    static const uint8_t last_pc[]={0,1,2,3,4,5,6,7,8,9,10,
                                      6,7,8,9,10,6,7,8,9,10,11,12};
    static const uint8_t next_pc[]={1,2,3,4,5,6,7,8,9,10,6,
                                      7,8,9,10,6,7,8,9,10,11,12,13};
    static const expected_segment want[]={
        {3,20,3,20,0xf800},{9,20,9,20,0xf800},
        {27,20,27,20,0xf800},{30,20,27,20,0x001f}
    };
    const float input[KSN_PROC_INPUTS]={0};
    const float regs[KSN_PROC_REGS]={27,54,3,20,30,0,0,99};
    const ksn_proc_program p={code,(uint8_t)LEN(code)};
    ksn_proc_vm vm;
    ksn_proc_frame frame;
    CHECK(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    for(size_t j=0;j<LEN(last_pc);j++){
        CHECK(ksn_proc_step(&vm)==(j+1==LEN(last_pc)?KSN_PROC_DONE:KSN_PROC_RUNNING));
        CHECK(vm.steps==j+1&&vm.last_pc==last_pc[j]&&vm.pc==next_pc[j]);
        CHECK(vm.depth==(j>=5&&j<20?1:0));
        if(j==10)CHECK(vm.loop[0].remaining==2);
        if(j==15)CHECK(vm.loop[0].remaining==1);
        if(j==20)CHECK(frame.count==3&&vm.reg[0]==27&&vm.reg[1]==54);
    }
    CHECK(ksn_proc_step(&vm)==KSN_PROC_DONE&&vm.steps==LEN(last_pc));
    segments_are(&frame,want,LEN(want),7,true);
    state_is(&vm,regs,27,20,true);
}

/* A dynamic outer count and a conditional inner exit. BREAK skips that
 * inner END but outer END still executes. Zero skips the entire outer body. */
static void dynamic_break(void){
    static const ksn_proc_inst code[]={
        I(KSN_PROC_INPUT,0,0,0,0,0), I(KSN_PROC_SET,1,0,0,0,0),
        I(KSN_PROC_SET,2,0,0,1,0), I(KSN_PROC_SET,3,0,0,10,0),
        I(KSN_PROC_SET,4,0,0,7,0), I(KSN_PROC_REPEAT_REG,0,0,0,0,0),
        I(KSN_PROC_REPEAT,0,4,0,0,0), I(KSN_PROC_ADD,1,1,2,0,0),
        I(KSN_PROC_BREAK_IF_GT,0,1,0,0,0),
        I(KSN_PROC_PLOT,0,3,4,0,0x07e0), I(KSN_PROC_END,0,0,0,0,0),
        I(KSN_PROC_ADD,3,3,2,0,0), I(KSN_PROC_END,0,0,0,0,0)
    };
    static const expected_segment want[]={
        {10,7,10,7,0x07e0},{10,7,10,7,0x07e0}
    };
    const ksn_proc_program p={code,(uint8_t)LEN(code)};
    ksn_proc_vm vm;
    ksn_proc_frame frame;
    float input[KSN_PROC_INPUTS]={2,0,0,0};
    const float regs[KSN_PROC_REGS]={2,4,1,12,7,0,0,0};
    CHECK(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_run(&vm)==KSN_PROC_DONE);
    CHECK(vm.steps==24&&vm.pc==LEN(code)&&vm.depth==0);
    segments_are(&frame,want,LEN(want),2,true);
    state_is(&vm,regs,10,7,true);
    input[0]=0;
    CHECK(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_run(&vm)==KSN_PROC_DONE&&vm.steps==6);
    segments_are(&frame,want,0,0,true);
    {
        const float zero_regs[KSN_PROC_REGS]={0,0,1,10,7,0,0,0};
        state_is(&vm,zero_regs,0,0,false);
    }
    input[0]=2.5f;
    CHECK(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_run(&vm)==KSN_PROC_INVALID&&vm.steps==6);
    segments_are(&frame,want,0,0,false);
    no_state(&vm);
    input[0]=256;
    CHECK(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_run(&vm)==KSN_PROC_INVALID&&vm.steps==6);
}

/* Each source-order arithmetic operation is rounded as float. Reassociation
 * would change r3 or r5. An overflow is invalid even if a later SET would
 * overwrite the dead value before any drawing. */
static void arithmetic_barriers(void){
    static const ksn_proc_inst rounding[]={
        I(KSN_PROC_SET,0,0,0,33554432.0f,0),
        I(KSN_PROC_SET,1,0,0,-33554432.0f,0),
        I(KSN_PROC_SET,2,0,0,1,0), I(KSN_PROC_ADD,3,0,1,0,0),
        I(KSN_PROC_ADD,3,3,2,0,0), I(KSN_PROC_ADD,4,1,2,0,0),
        I(KSN_PROC_ADD,5,0,4,0,0)
    };
    static const ksn_proc_inst overflow[]={
        I(KSN_PROC_SET,0,0,0,FLT_MAX,0), I(KSN_PROC_SET,1,0,0,2,0),
        I(KSN_PROC_MUL,2,0,1,0,0), I(KSN_PROC_SET,2,0,0,0,0)
    };
    const float input[KSN_PROC_INPUTS]={0};
    const float regs[KSN_PROC_REGS]={33554432.0f,-33554432.0f,1,1,
                                     -33554432.0f,0,0,0};
    ksn_proc_vm vm;
    ksn_proc_frame frame;
    const ksn_proc_program a={rounding,(uint8_t)LEN(rounding)};
    const ksn_proc_program b={overflow,(uint8_t)LEN(overflow)};
    CHECK(ksn_proc_begin(&vm,&a,input,&frame)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_run(&vm)==KSN_PROC_DONE&&vm.steps==7);
    segments_are(&frame,NULL,0,0,true);
    state_is(&vm,regs,0,0,false);
    CHECK(ksn_proc_begin(&vm,&b,input,&frame)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_run(&vm)==KSN_PROC_INVALID);
    CHECK(vm.steps==3&&vm.last_pc==2&&vm.pc==3);
    CHECK(!isfinite(vm.reg[2]));
    segments_are(&frame,NULL,0,0,false);
    no_state(&vm);
}

/* Ordered overdraw, a line whose start is the prior pen, and all eight
 * carried registers after an explicit stateful begin. */
static void draw_order_and_state(void){
    static const ksn_proc_inst code[]={
        I(KSN_PROC_SET,0,0,0,2,0), I(KSN_PROC_SET,1,0,0,3,0),
        I(KSN_PROC_PLOT,0,0,1,0,0xf800),
        I(KSN_PROC_PLOT,0,0,1,0,0x07e0),
        I(KSN_PROC_SET,0,0,0,5,0),
        I(KSN_PROC_LINE,0,0,1,0,0x001f)
    };
    static const expected_segment want[]={
        {2,3,2,3,0xf800},{2,3,2,3,0x07e0},{2,3,5,3,0x001f}
    };
    const float input[KSN_PROC_INPUTS]={0};
    /* r8..r15 are untouched and carry their initial 10+j through. */
    const float regs[KSN_PROC_REGS]={5,3,12,13,14,15,16,17,
                                     18,19,20,21,22,23,24,25};
    ksn_proc_state initial,captured;
    ksn_proc_vm vm;
    ksn_proc_frame frame;
    uint16_t pixels[KSN_PROC_W]={0};
    const ksn_proc_program p={code,(uint8_t)LEN(code)};
    ksn_proc_state_reset(&initial);
    for(unsigned j=0;j<KSN_PROC_REGS;j++)initial.reg[j]=(float)(10+j);
    initial.pen_x=8;initial.pen_y=9;initial.pen_valid=true;
    CHECK(ksn_proc_begin_state(&vm,&p,input,&initial,&frame)==KSN_PROC_RUNNING);
    no_state(&vm);
    CHECK(ksn_proc_run(&vm)==KSN_PROC_DONE&&vm.steps==6);
    segments_are(&frame,want,LEN(want),6,true);
    state_is(&vm,regs,5,3,true);
    CHECK(initial.reg[0]==10&&initial.pen_x==8);
    CHECK(ksn_proc_render_band(&frame,pixels,3,1));
    CHECK(pixels[2]==0x001f&&pixels[3]==0x001f&&
          pixels[4]==0x001f&&pixels[5]==0x001f);
    CHECK(ksn_proc_capture_state(&vm,&captured));
    CHECK(captured.reg[7]==17);
}

static void rejection_and_caps(void){
    static const ksn_proc_inst bad_code[]={I(KSN_PROC_END,0,0,0,0,0)};
    static const ksn_proc_inst long_code[]={
        I(KSN_PROC_REPEAT,0,255,0,0,0),
        I(KSN_PROC_REPEAT,0,255,0,0,0),
        I(KSN_PROC_SET,0,0,0,1,0), I(KSN_PROC_END,0,0,0,0,0),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    static const ksn_proc_inst too_many_segments[]={
        I(KSN_PROC_SET,0,0,0,2,0), I(KSN_PROC_SET,1,0,0,3,0),
        I(KSN_PROC_REPEAT,0,255,0,0,0),
        I(KSN_PROC_REPEAT,0,5,0,0,0),
        I(KSN_PROC_PLOT,0,0,1,0,0xffff),
        I(KSN_PROC_END,0,0,0,0,0),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    static const ksn_proc_inst too_much_raster[]={
        I(KSN_PROC_SET,0,0,0,-480,0), I(KSN_PROC_SET,1,0,0,0,0),
        I(KSN_PROC_SET,2,0,0,720,0), I(KSN_PROC_REPEAT,0,8,0,0,0),
        I(KSN_PROC_MOVE,0,0,1,0,0),
        I(KSN_PROC_LINE,0,2,1,0,0xffff),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    const ksn_proc_program bad={bad_code,(uint8_t)LEN(bad_code)};
    const ksn_proc_program long_p={long_code,(uint8_t)LEN(long_code)};
    const ksn_proc_program segments={too_many_segments,(uint8_t)LEN(too_many_segments)};
    const ksn_proc_program raster={too_much_raster,(uint8_t)LEN(too_much_raster)};
    const float input[KSN_PROC_INPUTS]={0};
    ksn_proc_vm vm;
    ksn_proc_frame frame;
    CHECK(ksn_proc_begin(&vm,&bad,input,&frame)==KSN_PROC_INVALID);
    CHECK(vm.steps==0&&!frame.ready&&frame.count==0);
    no_state(&vm);
    CHECK(ksn_proc_begin(&vm,&long_p,input,&frame)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_run(&vm)==KSN_PROC_LIMIT&&vm.steps==KSN_PROC_STEPS);
    CHECK(!frame.ready&&frame.count==0&&frame.raster_steps==0);
    no_state(&vm);
    CHECK(ksn_proc_begin(&vm,&segments,input,&frame)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_run(&vm)==KSN_PROC_LIMIT&&vm.steps<KSN_PROC_STEPS);
    CHECK(!frame.ready&&frame.count==KSN_PROC_SEGMENTS);
    CHECK(frame.raster_steps==KSN_PROC_SEGMENTS);
    no_state(&vm);
    CHECK(ksn_proc_begin(&vm,&raster,input,&frame)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_run(&vm)==KSN_PROC_LIMIT&&vm.steps==24);
    CHECK(!frame.ready&&frame.count==6&&frame.raster_steps==7206);
    no_state(&vm);
    {
        float invalid_input[KSN_PROC_INPUTS]={NAN,0,0,0};
        CHECK(ksn_proc_begin(&vm,&segments,invalid_input,&frame)==KSN_PROC_INVALID);
        CHECK(vm.steps==0&&!frame.ready&&frame.count==0);
    }
}

/* Fractional coordinates round away from zero at a tie. Color-register
 * validation precedes emission, and LINE without a pen only establishes it. */
static void coordinates_and_color(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_INPUT,0,0,0,0,0), I(KSN_PROC_INPUT,1,1,0,0,0),
        I(KSN_PROC_INPUT,2,2,0,0,0),
        I(KSN_PROC_LINE_COLOR_REG,2,0,1,0,0),
        I(KSN_PROC_PLOT_COLOR_REG,2,0,1,0,0)
    };
    const ksn_proc_program p={code,(uint8_t)LEN(code)};
    const expected_segment want[]={ {-1,1,-1,1,65535} };
    float input[KSN_PROC_INPUTS]={-0.5f,0.5f,65535,0};
    const float regs[KSN_PROC_REGS]={-0.5f,0.5f,65535,0,0,0,0,0};
    ksn_proc_vm vm;
    ksn_proc_frame frame;
    CHECK(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
    CHECK(ksn_proc_run(&vm)==KSN_PROC_DONE&&vm.steps==5);
    segments_are(&frame,want,1,1,true);
    state_is(&vm,regs,-1,1,true);
    const float bad_colors[]={-1,0.5f,65536};
    for(size_t j=0;j<LEN(bad_colors);j++){
        input[2]=bad_colors[j];
        CHECK(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
        CHECK(ksn_proc_run(&vm)==KSN_PROC_INVALID&&vm.steps==4);
        CHECK(vm.last_pc==3&&vm.pc==4&&!vm.pen_valid);
        segments_are(&frame,NULL,0,0,false);
        no_state(&vm);
    }
    input[2]=0;
    /* These would round into the permitted range; validation uses the
     * original float coordinate, before conversion to an integer. */
    const float bad_coords[]={-480.25f,720.25f};
    for(size_t j=0;j<LEN(bad_coords);j++){
        input[0]=bad_coords[j];
        CHECK(ksn_proc_begin(&vm,&p,input,&frame)==KSN_PROC_RUNNING);
        CHECK(ksn_proc_run(&vm)==KSN_PROC_INVALID&&vm.steps==4);
        segments_are(&frame,NULL,0,0,false);
        no_state(&vm);
    }
}

int main(void){
    recurrence_and_trace();
    dynamic_break();
    arithmetic_barriers();
    draw_order_and_state();
    rejection_and_caps();
    coordinates_and_color();
    puts("PROC_SEMANTIC_ORACLE_OK");
    return 0;
}
