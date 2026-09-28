/* Host-only throughput probe, not a device performance gate.
 * Build from repository root with MSYS2 UCRT64 GCC on PATH:
 * gcc -std=c11 -O2 -Wall -Wextra -Werror -Imain/ui/kasane \
 *   main/ui/kasane/ksn_procedural.c main/ui/kasane/ksn_proc_analysis.c \
 *   main/ui/kasane/ksn_proc_plan.c tools/kasane_contract/probe_proc_plan_cost.c \
 *   -lm -o probe_proc_plan_cost.exe
 */
#include "ksn_proc_plan.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define I(op,d,a,b,v,c) {op,d,a,b,v,c}
#define COUNT(a) ((uint8_t)(sizeof(a)/sizeof((a)[0])))
#define FRAMES 500u
enum { BATCHES=9, REGISTRATIONS=200 };

static volatile uint64_t sink;
static ksn_proc_vm vm;
static ksn_proc_frame frame;
static LARGE_INTEGER frequency;

static const ksn_proc_inst straight[]={
    I(KSN_PROC_INPUT,0,0,0,0,0),I(KSN_PROC_SET,1,0,0,1.001f,0),
    I(KSN_PROC_ADD,2,0,1,0,0),I(KSN_PROC_MUL,3,2,1,0,0),
    I(KSN_PROC_ADD,4,3,2,0,0),I(KSN_PROC_MUL,5,4,1,0,0),
    I(KSN_PROC_ADD,2,5,0,0,0),I(KSN_PROC_MUL,3,2,1,0,0),
    I(KSN_PROC_ADD,4,3,0,0,0),I(KSN_PROC_MUL,5,4,1,0,0),
    I(KSN_PROC_ADD,2,5,0,0,0),I(KSN_PROC_MUL,3,2,1,0,0),
    I(KSN_PROC_ADD,4,3,0,0,0),I(KSN_PROC_MUL,5,4,1,0,0)
};
static const ksn_proc_inst loop_math[]={
    I(KSN_PROC_INPUT,0,0,0,0,0),I(KSN_PROC_SET,1,0,0,1.0001f,0),
    I(KSN_PROC_REPEAT,0,80,0,0,0),
    I(KSN_PROC_ADD,2,0,1,0,0),I(KSN_PROC_MUL,3,2,1,0,0),
    I(KSN_PROC_ADD,4,3,0,0,0),I(KSN_PROC_MUL,0,4,1,0,0),
    I(KSN_PROC_END,0,0,0,0,0)
};
static const ksn_proc_inst loop_draw[]={
    I(KSN_PROC_INPUT,0,0,0,0,0),I(KSN_PROC_SET,1,0,0,1,0),
    I(KSN_PROC_SET,2,0,0,20,0),I(KSN_PROC_REPEAT,0,40,0,0,0),
    I(KSN_PROC_ADD,0,0,1,0,0),I(KSN_PROC_MUL,3,0,1,0,0),
    I(KSN_PROC_PLOT,0,3,2,0,0xffff),I(KSN_PROC_END,0,0,0,0,0)
};
typedef struct { const char *name; ksn_proc_program program; } workload;
static const workload workloads[]={
    {"straight-arithmetic",{straight,COUNT(straight)}},
    {"loop-arithmetic",{loop_math,COUNT(loop_math)}},
    {"loop-draw",{loop_draw,COUNT(loop_draw)}}
};

static uint64_t ticks(void){ LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
static double us(uint64_t t,unsigned iterations){ return (double)t*1000000.0/((double)frequency.QuadPart*iterations); }
static int double_cmp(const void *a,const void *b){
    const double x=*(const double *)a,y=*(const double *)b;
    return (x>y)-(x<y);
}
static void print_distribution(const char *label,double values[BATCHES]){
    qsort(values,BATCHES,sizeof values[0],double_cmp);
    printf("  %-18s median %.3f us  range %.3f..%.3f us\n",
           label,values[BATCHES/2],values[0],values[BATCHES-1]);
}
static uint64_t execute(const ksn_proc_program *p,const ksn_proc_plan *plan,
                        bool prepared,unsigned seed){
    float input[KSN_PROC_INPUTS]={(float)(seed%17u)*0.03125f,0,0,0};
    ksn_proc_status begin=prepared ? ksn_proc_plan_begin(&vm,plan,input,&frame)
                                   : ksn_proc_begin(&vm,p,input,&frame);
    if(begin!=KSN_PROC_RUNNING){fprintf(stderr,"begin status %d\n",begin);exit(2);}
    ksn_proc_status end=prepared ? ksn_proc_plan_run(&vm,plan,false) : ksn_proc_run(&vm);
    if(end!=KSN_PROC_DONE){fprintf(stderr,"run status %d\n",end);exit(2);}
    union { float f; uint32_t u; } reg={.f=vm.reg[0]};
    return (uint64_t)reg.u+vm.steps+frame.count+
           (frame.count ? (uint16_t)frame.segments[frame.count-1].x0 : 0u);
}
static double time_frames(const workload *w,const ksn_proc_plan *plan,
                          bool prepared,unsigned batch){
    uint64_t digest=0,start=ticks();
    for(unsigned n=0;n<FRAMES;n++)digest+=execute(&w->program,plan,prepared,n+batch*FRAMES);
    uint64_t elapsed=ticks()-start;
    sink+=digest;
    return us(elapsed,FRAMES);
}
static double time_registration(const workload *w){
    ksn_proc_plan plan;
    uint64_t start=ticks();
    for(unsigned n=0;n<REGISTRATIONS;n++){
        if(!ksn_proc_plan_prepare(&plan,&w->program)){fprintf(stderr,"prepare failed\n");exit(2);}
        sink+=plan.fused_count;
    }
    return us(ticks()-start,REGISTRATIONS);
}
int main(void){
    if(!QueryPerformanceFrequency(&frequency)){fprintf(stderr,"QPC unavailable\n");return 2;}
    printf("host-only O2 probe; %u frames/batch, %d batches, %d registrations/batch\n",
           FRAMES,BATCHES,REGISTRATIONS);
    puts("frame cost includes begin validation/copy and run on both paths; registration is plan_prepare only");
    for(size_t j=0;j<sizeof workloads/sizeof workloads[0];j++){
        const workload *w=&workloads[j];ksn_proc_plan plan;
        if(!ksn_proc_plan_prepare(&plan,&w->program)){fprintf(stderr,"prepare failed\n");return 2;}
        uint64_t a=execute(&w->program,&plan,false,0),b=execute(&w->program,&plan,true,0);
        if(a!=b){fprintf(stderr,"result differs for %s\n",w->name);return 2;}
        printf("%s: code=%u bytes=%zu fused_pairs=%u steps=%" PRIu32 " segments=%u\n",
               w->name,w->program.count,(size_t)w->program.count*sizeof(ksn_proc_inst),
               plan.fused_count,vm.steps,frame.count);
        double registration[BATCHES],baseline[BATCHES],prepared[BATCHES];
        for(unsigned batch=0;batch<BATCHES;batch++){
            registration[batch]=time_registration(w);
            if(batch&1u){prepared[batch]=time_frames(w,&plan,true,batch);
                         baseline[batch]=time_frames(w,&plan,false,batch);}
            else {baseline[batch]=time_frames(w,&plan,false,batch);
                  prepared[batch]=time_frames(w,&plan,true,batch);}
        }
        print_distribution("plan registration",registration);
        print_distribution("baseline begin+run",baseline);
        print_distribution("prepared begin+run",prepared);
        printf("  median prepared/baseline %.3fx\n",prepared[BATCHES/2]/baseline[BATCHES/2]);
    }
    printf("checksum=%" PRIu64 "\n",sink);
    return 0;
}
