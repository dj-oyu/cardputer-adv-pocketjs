// Host: compile ksn_procedural.c and this file with -std=c11 -O2 -Wall
// -Wextra -Werror -Imain/ui/kasane -lm, then run with --trace to see each step.
#include "ksn_procedural.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define I(OP,D,A,B,V,C) {OP,D,A,B,V,C}
static const ksn_proc_inst graph_code[]={
    I(KSN_PROC_SET,0,0,0,10,0),       // x = 10
    I(KSN_PROC_SET,1,0,0,1,0),        // dx = 1
    I(KSN_PROC_INPUT,2,0,0,0,0),      // baseline, frozen at begin
    I(KSN_PROC_SET,3,0,0,12,0),       // amplitude
    I(KSN_PROC_SET,4,0,0,.1f,0),      // frequency
    I(KSN_PROC_REPEAT,0,100,0,0,0),  // 100 samples, native loop
    I(KSN_PROC_MUL,5,0,4,0,0),
    I(KSN_PROC_SIN,6,5,0,0,0),
    I(KSN_PROC_MUL,6,6,3,0,0),
    I(KSN_PROC_ADD,7,6,2,0,0),
    I(KSN_PROC_LINE,0,0,7,0,0xffff),
    I(KSN_PROC_ADD,0,0,1,0,0),
    I(KSN_PROC_END,0,0,0,0,0)
};
static const ksn_proc_program graph={graph_code,sizeof graph_code/sizeof graph_code[0]};
static ksn_proc_frame frozen,second;
static uint16_t whole[KSN_PROC_W*KSN_PROC_H],joined[KSN_PROC_W*KSN_PROC_H],changed[KSN_PROC_W*KSN_PROC_H];
static ksn_proc_frame state_frame_a,state_frame_b;

static void test_owned_program(void){
    ksn_proc_inst code[]={
        I(KSN_PROC_SET,0,0,0,15,0),I(KSN_PROC_SET,1,0,0,25,0),
        I(KSN_PROC_PLOT,0,0,1,0,0xf800)
    };
    ksn_proc_program program={code,3};
    float input[KSN_PROC_INPUTS]={0};
    ksn_proc_vm vm;
    assert(ksn_proc_begin(&vm,&program,input,&state_frame_a)==KSN_PROC_RUNNING);
    assert(ksn_proc_step(&vm)==KSN_PROC_RUNNING);
    code[1].op=255;
    code[2].color=0x001f;
    program.count=1;
    program.code=NULL;
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
    assert(state_frame_a.count==1&&state_frame_a.segments[0].color==0xf800);
    assert(state_frame_a.segments[0].x0==15&&state_frame_a.segments[0].y0==25);
}

static void test_state_and_color(void){
    const ksn_proc_inst code[]={
        I(KSN_PROC_SET,1,0,0,1,0),I(KSN_PROC_SET,2,0,0,30,0),
        I(KSN_PROC_SET,4,0,0,31,0),I(KSN_PROC_REPEAT,0,3,0,0,0),
          I(KSN_PROC_ADD,0,0,1,0,0),I(KSN_PROC_ADD,3,0,4,0,0),
          I(KSN_PROC_PLOT_COLOR_REG,3,0,2,0,0),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    ksn_proc_program program={code,8};
    float input[KSN_PROC_INPUTS]={0};
    ksn_proc_state state;
    ksn_proc_state_reset(&state);
    state.reg[0]=10;
    ksn_proc_vm vm;
    assert(ksn_proc_begin_state(&vm,&program,input,&state,&state_frame_a)==KSN_PROC_RUNNING);
    assert(!ksn_proc_capture_state(&vm,&state));
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
    assert(state_frame_a.count==3&&state_frame_a.segments[0].color==42);
    assert(state.reg[0]==10); /* The state changes only after an explicit commit. */
    assert(ksn_proc_capture_state(&vm,&state)&&state.reg[0]==13);
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)whole[i]=joined[i]=0x001f;
    assert(ksn_proc_render_band(&state_frame_a,whole,0,KSN_PROC_H));
    assert(ksn_proc_begin_state(&vm,&program,input,&state,&state_frame_b)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
    assert(ksn_proc_capture_state(&vm,&state)&&state.reg[0]==16);
    assert(state_frame_b.count==3&&state_frame_b.segments[0].x0==14);
    assert(state_frame_b.segments[0].color==45&&state_frame_b.segments[2].color==47);
    assert(ksn_proc_render_band(&state_frame_a,joined,0,KSN_PROC_H));
    assert(!memcmp(whole,joined,sizeof whole));

    const ksn_proc_inst line_code[]={
        I(KSN_PROC_SET,0,0,0,2,0),I(KSN_PROC_SET,1,0,0,3,0),
        I(KSN_PROC_SET,2,0,0,2016,0),I(KSN_PROC_MOVE,0,0,1,0,0),
        I(KSN_PROC_SET,0,0,0,6,0),I(KSN_PROC_LINE_COLOR_REG,2,0,1,0,0)
    };
    ksn_proc_program line={line_code,6};
    assert(ksn_proc_begin(&vm,&line,input,&state_frame_b)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&state_frame_b.count==1);
    assert(state_frame_b.segments[0].color==0x07e0);
    assert(state_frame_b.segments[0].x0==2&&state_frame_b.segments[0].x1==6);

    const ksn_proc_inst bad_color[]={
        I(KSN_PROC_SET,0,0,0,2,0),I(KSN_PROC_SET,1,0,0,3,0),
        I(KSN_PROC_INPUT,2,0,0,0,0),I(KSN_PROC_PLOT_COLOR_REG,2,0,1,0,0)
    };
    ksn_proc_program bad={bad_color,4};
    const float invalid_colors[]={-1,65536,1.5f};
    for(unsigned n=0;n<sizeof invalid_colors/sizeof invalid_colors[0];n++){
        input[0]=invalid_colors[n];
        assert(ksn_proc_begin(&vm,&bad,input,&state_frame_b)==KSN_PROC_RUNNING);
        assert(ksn_proc_run(&vm)==KSN_PROC_INVALID&&!state_frame_b.ready);
        ksn_proc_state unchanged=state;
        assert(!ksn_proc_capture_state(&vm,&state));
        assert(!memcmp(&state,&unchanged,sizeof state));
    }
}

int main(int argc,char **argv){
    test_owned_program();
    test_state_and_color();
    bool trace=argc>1&&!strcmp(argv[1],"--trace");
    float input[KSN_PROC_INPUTS]={60,0,0,0};
    ksn_proc_vm vm;
    assert(ksn_proc_begin(&vm,&graph,input,&frozen)==KSN_PROC_RUNNING);
    input[0]=100; // input is copied at begin; JS need not remain on the render path.
    for(unsigned n=0;n<20;n++){
        assert(ksn_proc_step(&vm)==KSN_PROC_RUNNING);
        if(trace)printf("TRACE step=%u pc=%u op=%u x=%.2f y=%.2f depth=%u emitted=%u\n",
                        vm.steps,vm.last_pc,graph.code[vm.last_pc].op,vm.reg[0],vm.reg[7],vm.depth,frozen.count);
    }
    assert(vm.depth==1&&vm.reg[2]==60&&frozen.count==1);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
    assert(frozen.ready&&frozen.count==99&&vm.reg[0]==110);
    assert(vm.steps==5+1+100*7); // each sample: 6 body instructions + END
    assert(ksn_proc_step(&vm)==KSN_PROC_DONE);
    for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)whole[i]=joined[i]=changed[i]=0x001f;
    assert(ksn_proc_render_band(&frozen,whole,0,KSN_PROC_H));
    for(int band=(KSN_PROC_H-1)/8;band>=0;band--){
        int y=band*8,h=KSN_PROC_H-y<8?KSN_PROC_H-y:8;
        assert(ksn_proc_render_band(&frozen,joined+y*KSN_PROC_W,y,h));
    }
    assert(!memcmp(whole,joined,sizeof whole));
    assert(ksn_proc_begin(&vm,&graph,input,&second)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&second.count==99);
    assert(ksn_proc_render_band(&second,changed,0,KSN_PROC_H));
    assert(memcmp(whole,changed,sizeof whole));
    assert(ksn_proc_render_band(&frozen,joined,0,KSN_PROC_H));
    assert(!memcmp(whole,joined,sizeof whole));

    // Later operations cover earlier ones; MOVE starts a new path.
    const ksn_proc_inst order_code[]={
        I(KSN_PROC_SET,0,0,0,20,0),I(KSN_PROC_SET,1,0,0,30,0),
        I(KSN_PROC_PLOT,0,0,1,0,0xf800),I(KSN_PROC_PLOT,0,0,1,0,0x07e0)
    };
    ksn_proc_program order={order_code,4};
    ksn_proc_frame over;
    assert(ksn_proc_begin(&vm,&order,input,&over)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&over.count==2);
    assert(ksn_proc_render_band(&over,joined,0,KSN_PROC_H));
    assert(joined[30*KSN_PROC_W+20]==0x07e0);

    const ksn_proc_inst bad_code[]={I(KSN_PROC_END,0,0,0,0,0)};
    ksn_proc_program bad={bad_code,1};
    assert(ksn_proc_begin(&vm,&bad,input,&over)==KSN_PROC_INVALID);
    assert(!over.ready&&!ksn_proc_render_band(&over,joined,0,KSN_PROC_H));
    const ksn_proc_inst many_code[]={
        I(KSN_PROC_SET,0,0,0,20,0),I(KSN_PROC_SET,1,0,0,30,0),
        I(KSN_PROC_REPEAT,0,255,0,0,0),I(KSN_PROC_REPEAT,0,5,0,0,0),
        I(KSN_PROC_PLOT,0,0,1,0,0xffff),I(KSN_PROC_END,0,0,0,0,0),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    ksn_proc_program many={many_code,7};
    assert(ksn_proc_begin(&vm,&many,input,&over)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_LIMIT&&over.count==KSN_PROC_SEGMENTS);
    assert(!over.ready&&!ksn_proc_render_band(&over,joined,0,KSN_PROC_H));
    const ksn_proc_inst long_code[]={
        I(KSN_PROC_SET,0,0,0,-480,0),I(KSN_PROC_SET,1,0,0,0,0),
        I(KSN_PROC_SET,2,0,0,720,0),I(KSN_PROC_REPEAT,0,8,0,0,0),
        I(KSN_PROC_MOVE,0,0,1,0,0),I(KSN_PROC_LINE,0,2,1,0,0xffff),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    ksn_proc_program long_lines={long_code,7};
    assert(ksn_proc_begin(&vm,&long_lines,input,&over)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_LIMIT&&over.count==6);
    assert(!over.ready&&over.raster_steps<=KSN_PROC_RASTER_STEPS);
    const ksn_proc_inst endless_code[]={
        I(KSN_PROC_REPEAT,0,255,0,0,0),I(KSN_PROC_REPEAT,0,255,0,0,0),
        I(KSN_PROC_SET,0,0,0,1,0),I(KSN_PROC_END,0,0,0,0,0),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    ksn_proc_program endless={endless_code,5};
    assert(ksn_proc_begin(&vm,&endless,input,&over)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_LIMIT&&vm.steps==KSN_PROC_STEPS);
    assert(!over.ready);
    // A frame input can vary the bounded loop count without rebuilding code.
    const ksn_proc_inst variable_code[]={
        I(KSN_PROC_SET,0,0,0,10,0),I(KSN_PROC_SET,1,0,0,20,0),
        I(KSN_PROC_SET,2,0,0,1,0),I(KSN_PROC_INPUT,3,0,0,0,0),
        I(KSN_PROC_REPEAT_REG,0,3,0,0,0),
          I(KSN_PROC_PLOT,0,0,1,0,0xffff),I(KSN_PROC_ADD,0,0,2,0,0),
        I(KSN_PROC_END,0,0,0,0,0)
    };
    ksn_proc_program variable={variable_code,8};
    for(int count=0;count<=5;count++){
        float variable_input[KSN_PROC_INPUTS]={(float)count,0,0,0};
        assert(ksn_proc_begin(&vm,&variable,variable_input,&over)==KSN_PROC_RUNNING);
        assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&over.count==count);
        assert(vm.reg[0]==10+count);
    }
    const float fractional[KSN_PROC_INPUTS]={2.5f,0,0,0};
    assert(ksn_proc_begin(&vm,&variable,fractional,&over)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_INVALID&&!over.ready);
    const float too_many[KSN_PROC_INPUTS]={256,0,0,0};
    assert(ksn_proc_begin(&vm,&variable,too_many,&over)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_INVALID&&!over.ready);
    const ksn_proc_inst orphan_break[]={I(KSN_PROC_BREAK_IF_GT,0,0,1,0,0)};
    ksn_proc_program orphan={orphan_break,1};
    assert(ksn_proc_begin(&vm,&orphan,input,&over)==KSN_PROC_INVALID);
    // BREAK exits the innermost loop; the outer loop still repeats.
    const ksn_proc_inst nested_break[]={
        I(KSN_PROC_SET,0,0,0,10,0),I(KSN_PROC_SET,1,0,0,20,0),
        I(KSN_PROC_SET,2,0,0,1,0),I(KSN_PROC_SET,3,0,0,0,0),
        I(KSN_PROC_REPEAT,0,2,0,0,0),I(KSN_PROC_REPEAT,0,3,0,0,0),
        I(KSN_PROC_PLOT,0,0,1,0,0xffff),
        I(KSN_PROC_BREAK_IF_GT,0,0,3,0,0),I(KSN_PROC_END,0,0,0,0,0),
        I(KSN_PROC_ADD,0,0,2,0,0),I(KSN_PROC_END,0,0,0,0,0)
    };
    ksn_proc_program nested={nested_break,11};
    assert(ksn_proc_begin(&vm,&nested,input,&over)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE&&over.count==2);
    assert(over.segments[0].x0==10&&over.segments[1].x0==11);
    input[0]=NAN;
    assert(ksn_proc_begin(&vm,&graph,input,&over)==KSN_PROC_INVALID);
    printf("PROCEDURAL_OK: 100 native loop samples, %u segments, %u VM steps; frame capacity %zu B, VM %zu B; step debug, owned program, native state continuation, computed RGB565, frozen reverse-band replay, bounded variable repeat/break, ordered overdraw, invalid/limit rejection\n",
           frozen.count,5+1+100*7,sizeof frozen,sizeof vm);
}
