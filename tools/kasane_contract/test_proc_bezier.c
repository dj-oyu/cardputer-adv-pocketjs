#include "ksn_procedural.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t whole[KSN_PROC_W*KSN_PROC_H];
static uint16_t bands[KSN_PROC_W*KSN_PROC_H];

int main(void){
    ksn_proc_state control;
    ksn_proc_state_reset(&control);
    float points[]={10,10,35,95,75,95,100,10};
    memcpy(control.reg,points,sizeof points);
    const ksn_proc_inst code[]={
        {KSN_PROC_INPUT,3,0,0,0,0},
        {KSN_PROC_CUBIC,0,16,0,0,0x07e0},
        {KSN_PROC_SET,0,0,0,120,0},
        {KSN_PROC_SET,1,0,0,20,0},
        {KSN_PROC_LINE,0,0,1,0,0xf800}
    };
    ksn_proc_program program={code,5};
    ksn_proc_vm vm;
    ksn_proc_frame frame;
    float input[KSN_PROC_INPUTS]={95,0,0,0};
    assert(ksn_proc_begin_state(&vm,&program,input,&control,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_step(&vm)==KSN_PROC_RUNNING);
    assert(ksn_proc_step(&vm)==KSN_PROC_RUNNING);
    assert(vm.last_pc==1&&frame.count==16&&vm.pen_x==100&&vm.pen_y==10);
    assert(frame.segments[0].x0==10&&frame.segments[0].y0==10);
    assert(frame.segments[15].x1==100&&frame.segments[15].y1==10);
    assert(frame.segments[7].y1>50);
    assert(ksn_proc_step(&vm)==KSN_PROC_RUNNING);
    assert(ksn_proc_step(&vm)==KSN_PROC_RUNNING);
    assert(ksn_proc_step(&vm)==KSN_PROC_DONE);
    assert(frame.count==17&&frame.segments[16].x0==100&&
           frame.segments[16].x1==120&&vm.pen_x==120&&vm.pen_y==20);
    assert(ksn_proc_render_band(&frame,whole,0,KSN_PROC_H));
    for(int bottom=KSN_PROC_H;bottom>0;){
        int rows=bottom<8?bottom:8;bottom-=rows;
        assert(ksn_proc_render_band(&frame,bands+bottom*KSN_PROC_W,bottom,rows));
    }
    assert(!memcmp(whole,bands,sizeof whole));
    input[0]=25;
    assert(ksn_proc_begin_state(&vm,&program,input,&control,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_DONE);
    assert(frame.segments[7].y1<50);

    ksn_proc_inst invalid=code[1];
    ksn_proc_program one={&invalid,1};
    invalid.a=0;
    assert(ksn_proc_begin(&vm,&one,input,&frame)==KSN_PROC_INVALID);
    invalid.a=65;
    assert(ksn_proc_begin(&vm,&one,input,&frame)==KSN_PROC_INVALID);
    invalid.a=16;invalid.dst=9;
    assert(ksn_proc_begin(&vm,&one,input,&frame)==KSN_PROC_INVALID);
    invalid.dst=0;
    control.reg[0]=721;
    assert(ksn_proc_begin_state(&vm,&one,input,&control,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_INVALID&&!frame.ready);
    control.reg[0]=10;
    ksn_proc_inst many[]={
        {KSN_PROC_REPEAT,0,17,0,0,0},
        {KSN_PROC_CUBIC,0,64,0,0,0x07e0},
        {KSN_PROC_END,0,0,0,0,0}
    };
    ksn_proc_program capped={many,3};
    assert(ksn_proc_begin_state(&vm,&capped,input,&control,&frame)==KSN_PROC_RUNNING);
    assert(ksn_proc_run(&vm)==KSN_PROC_LIMIT&&!frame.ready);
    puts("procedural cubic: native ordered segments, band replay, dynamic control, validation and cap passed");
    return 0;
}
