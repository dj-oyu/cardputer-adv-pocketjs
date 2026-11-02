/* ksn_proc_*_plan_draw against begin + run (docs/kasane/flash-plan.md sec.13).
 *
 * 1. The draw skips valid_program() because prepare's ksn_proc_analyze()
 *    accepted the rows. That holds only if analyze rejects whatever
 *    valid_program() rejects: random rows, with and without loop structure,
 *    must get the same accept/reject from both (begin is the only way to
 *    reach valid_program()).
 * 2. For every accepted program, the draw on one long-lived VM (never
 *    cleared between plans, as pocket_proc.c's is) and begin + run on a
 *    fresh one give the same status, frame, registers, pen, steps, pc and
 *    loop depth -- as a sized plan, and as a built-in plan whose patched
 *    fields hold placeholders in the table. */
#include "ksn_proc_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t seed=0x2545F491u;
static uint32_t rnd(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static unsigned pick(unsigned n){return rnd()%n;}
static float value(void){
    static const float pool[]={0.0f,1.0f,2.0f,3.0f,-1.0f,0.5f,7.0f,24.0f,25.0f,
                               100.0f,-300.0f,500.0f,1e30f,0.25f,12.0f};
    unsigned k=pick(18);
    if(k==15)return NAN;
    if(k==16)return INFINITY;
    if(k==17)return (float)(int)pick(240)-20.0f;
    return pool[k];
}
/* Mostly well-formed rows (loops balanced, registers in range) so most
 * programs run; `wild` lets any field take any small value. */
static uint8_t make(ksn_proc_inst *code,bool wild){
    uint8_t n=(uint8_t)(1+pick(KSN_PROC_CODE));
    unsigned depth=0;
    for(uint8_t pc=0;pc<n;pc++){
        ksn_proc_inst *i=&code[pc];
        memset(i,0,sizeof *i);
        const unsigned reg=wild?18u:KSN_PROC_REGS;
        i->op=(uint8_t)(wild?pick(17):pick(16));
        i->dst=(uint8_t)pick(reg);i->a=(uint8_t)pick(reg);i->b=(uint8_t)pick(reg);
        i->value=value();i->color=(uint16_t)rnd();
        if(!wild){
            if(i->op==KSN_PROC_INPUT)i->a=(uint8_t)pick(KSN_PROC_INPUTS);
            if(i->op==KSN_PROC_REPEAT)i->a=(uint8_t)(1+pick(6));
            if(i->op==KSN_PROC_CUBIC){i->dst=0;i->a=(uint8_t)(1+pick(8));}
            if(i->op==KSN_PROC_LINE_PATTERN){
                i->dst=(uint8_t)pick(KSN_PROC_REGS-KSN_PROC_PATTERN_BLOCK+1);
                i->value=(float)(1+pick(KSN_PROC_PATTERN_BITS));
            }
            if(i->op==KSN_PROC_SET&&!isfinite(i->value))i->value=1.0f;
            if((i->op==KSN_PROC_REPEAT||i->op==KSN_PROC_REPEAT_REG)&&
               (depth==KSN_PROC_LOOP_DEPTH||n-pc<2+(int)depth))i->op=KSN_PROC_SET;
            if(i->op==KSN_PROC_END&&!depth)i->op=KSN_PROC_ADD;
            if(i->op==KSN_PROC_BREAK_IF_GT&&!depth)i->op=KSN_PROC_MUL;
            if(depth&&(unsigned)(n-pc)<=depth)i->op=KSN_PROC_END;
            if(i->op==KSN_PROC_REPEAT||i->op==KSN_PROC_REPEAT_REG)depth++;
            if(i->op==KSN_PROC_END)depth--;
        }
    }
    return n;
}
static void inputs(float in[KSN_PROC_INPUTS]){
    for(unsigned j=0;j<KSN_PROC_INPUTS;j++)in[j]=(float)(int)pick(400)-80.0f+(float)pick(4)*0.25f;
}
static ksn_proc_frame ref_frame,draw_frame;
static ksn_proc_vm ref_vm,draw_vm;
static unsigned failures,compared,done,fused_plans;
static void same(const char *what,unsigned trial,ksn_proc_status a,ksn_proc_status b){
    bool ok=a==b&&ref_frame.ready==draw_frame.ready;
    if(ok&&a==KSN_PROC_DONE){
        ok=ref_frame.count==draw_frame.count&&ref_frame.raster_steps==draw_frame.raster_steps&&
           ref_frame.ext==draw_frame.ext&&
           !memcmp(ref_frame.segments,draw_frame.segments,ref_frame.count*sizeof ref_frame.segments[0]);
    }
    /* Registers, pen and counters as the run left them, whatever the status. */
    ok=ok&&!memcmp(ref_vm.reg,draw_vm.reg,sizeof ref_vm.reg)&&ref_vm.steps==draw_vm.steps&&
       ref_vm.pc==draw_vm.pc&&ref_vm.depth==draw_vm.depth&&ref_vm.status==draw_vm.status&&
       ref_vm.pen_valid==draw_vm.pen_valid&&
       (!ref_vm.pen_valid||(ref_vm.pen_x==draw_vm.pen_x&&ref_vm.pen_y==draw_vm.pen_y));
    if(a!=KSN_PROC_INVALID||b!=KSN_PROC_INVALID)compared++;
    if(a==KSN_PROC_DONE)done++;
    if(!ok){failures++;
        if(failures<10)printf("MISMATCH %s trial %u: status %d/%d count %u/%u steps %u/%u\n",what,trial,
                              a,b,ref_frame.count,draw_frame.count,
                              (unsigned)ref_vm.steps,(unsigned)draw_vm.steps);}
}

int main(void){
    static ksn_proc_inst code[KSN_PROC_CODE],table[KSN_PROC_CODE];
    unsigned agree=0,accepted=0;
    union { ksn_proc_sized_plan plan; unsigned char bytes[sizeof(ksn_proc_sized_plan)+
                                         KSN_PROC_CODE*sizeof(ksn_proc_inst)]; } sized;
    union { ksn_proc_rom_plan plan; unsigned char bytes[sizeof(ksn_proc_rom_plan)+
                                      KSN_PROC_ROM_PARAMS*sizeof(float)]; } rom;
    /* The draw VM is filled with garbage once: nothing it relies on may come
     * from a whole-VM clear. */
    memset(&draw_vm,0xA5,sizeof draw_vm);
    for(unsigned trial=0;trial<60000;trial++){
        const bool wild=trial%4==3;
        const uint8_t n=make(code,wild);
        const ksn_proc_program program={code,n};
        float in[KSN_PROC_INPUTS];
        for(unsigned j=0;j<KSN_PROC_INPUTS;j++)in[j]=0.0f;
        /* 1. analyze and valid_program agree. */
        ksn_proc_analysis analysis;
        const bool analyzed=ksn_proc_analyze(&program,&analysis);
        const bool validated=ksn_proc_begin(&ref_vm,&program,in,&ref_frame)==KSN_PROC_RUNNING;
        if(analyzed==validated)agree++;
        else{failures++;printf("DISAGREE trial %u: analyze %d valid_program %d\n",trial,analyzed,validated);}
        if(!analyzed)continue;
        accepted++;
        /* 2a. Sized plan: draw vs begin + run. */
        if(!ksn_proc_sized_plan_prepare(&sized.plan,KSN_PROC_CODE,&program)){
            failures++;printf("PREPARE trial %u\n",trial);continue;
        }
        if(sized.plan.fused_count)fused_plans++;
        for(unsigned r=0;r<3;r++){
            inputs(in);
            ksn_proc_status a=ksn_proc_sized_plan_begin(&ref_vm,&sized.plan,in,&ref_frame);
            if(a==KSN_PROC_RUNNING)a=ksn_proc_sized_plan_run(&ref_vm,&sized.plan,false);
            ksn_proc_status b=ksn_proc_sized_plan_draw(&draw_vm,&sized.plan,in,&draw_frame);
            same("sized",trial,a,b);
        }
        /* 2b. The same rows as a built-in plan: up to eight fields become
         * arguments, the table holding 0 there. */
        memcpy(table,code,n*sizeof code[0]);
        ksn_proc_patch patch[KSN_PROC_ROM_PARAMS];
        uint8_t patches=0;
        float arg[KSN_PROC_ROM_PARAMS];
        for(uint8_t pc=0;pc<n&&patches<KSN_PROC_ROM_PARAMS;pc++){
            if(pick(3))continue;
            ksn_proc_inst *t=&table[pc];
            uint8_t field;
            if(t->op==KSN_PROC_SET){field=KSN_PROC_FIELD_VALUE;arg[patches]=t->value;t->value=0;}
            else if(t->op==KSN_PROC_LINE||t->op==KSN_PROC_PLOT||t->op==KSN_PROC_CUBIC||
                    t->op==KSN_PROC_LINE_PATTERN){
                field=KSN_PROC_FIELD_COLOR;arg[patches]=(float)t->color;t->color=0;
            }else if(t->op==KSN_PROC_REPEAT){field=KSN_PROC_FIELD_A;arg[patches]=(float)t->a;t->a=0;}
            else continue;
            patch[patches]=(ksn_proc_patch){pc,field,patches};patches++;
        }
        const ksn_proc_rom_entry entry={"t",table,patch,n,patches,patches};
        memset(&rom,0,sizeof rom);
        rom.plan.params=patches;
        memcpy(rom.plan.args,arg,patches*sizeof arg[0]);
        if(!ksn_proc_rom_plan_prepare(&rom.plan,&entry)){
            failures++;printf("ROM PREPARE trial %u\n",trial);continue;
        }
        for(unsigned r=0;r<3;r++){
            inputs(in);
            ksn_proc_status a=ksn_proc_rom_plan_begin(&ref_vm,&rom.plan,in,&ref_frame);
            if(a==KSN_PROC_RUNNING)a=ksn_proc_rom_plan_run(&ref_vm,&rom.plan,false);
            ksn_proc_status b=ksn_proc_rom_plan_draw(&draw_vm,&rom.plan,in,&draw_frame);
            same("rom",trial,a,b);
            /* And the sized draw of the original rows gives the same again. */
            ksn_proc_status c=ksn_proc_sized_plan_draw(&draw_vm,&sized.plan,in,&draw_frame);
            same("rom-vs-sized",trial,a,c);
        }
    }
    /* An invalid plan fails as its begin does, and a non-finite input too. */
    {
        float in[KSN_PROC_INPUTS]={0};
        sized.plan.valid=false;
        draw_frame.ready=true;
        if(ksn_proc_sized_plan_draw(&draw_vm,&sized.plan,in,&draw_frame)!=KSN_PROC_INVALID||
           draw_frame.ready){failures++;printf("INVALID PLAN\n");}
        const ksn_proc_inst one[]={{KSN_PROC_INPUT,0,0,0,0,0}};
        const ksn_proc_program p={one,1};
        ksn_proc_sized_plan_prepare(&sized.plan,KSN_PROC_CODE,&p);
        in[3]=NAN;
        if(ksn_proc_sized_plan_draw(&draw_vm,&sized.plan,in,&draw_frame)!=KSN_PROC_INVALID){
            failures++;printf("NAN INPUT\n");}
    }
    printf("PLAN DRAW %s: %u/%u agree (analyze = valid_program), %u accepted, %u compared runs "
           "(%u done), %u plans with fused pairs\n",failures?"FAIL":"PASS",agree,60000u,accepted,
           compared,done,fused_plans);
    return failures?1:0;
}
