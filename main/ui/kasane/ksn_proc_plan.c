#include "ksn_proc_plan.h"
#include <math.h>
#include <string.h>

static bool arithmetic(uint8_t op){
    return op==KSN_PROC_ADD||op==KSN_PROC_MUL||op==KSN_PROC_SIN;
}
bool ksn_proc_plan_prepare(ksn_proc_plan *plan,const ksn_proc_program *program){
    if(!plan)return false;
    if(!program||!program->code||!program->count||program->count>KSN_PROC_CODE){
        memset(plan,0,sizeof *plan);
        return false;
    }
    /* The source may be this plan's own program, including a subrange. */
    uint8_t count=program->count;
    memmove(plan->code,program->code,count*sizeof plan->code[0]);
    memset(plan->fused_at,0,sizeof plan->fused_at);
    plan->fused_count=0;plan->valid=false;
    /* A typed batch belongs to this exact registration. A replacement IR
     * must explicitly register its own descriptor. */
    plan->points_registered=false;
    plan->program=(ksn_proc_program){plan->code,count};
    ksn_proc_analysis analysis;
    if(!ksn_proc_analyze(&plan->program,&analysis))return false;
    for(uint8_t pc=0;pc+1<count;pc++){
        const ksn_pa_inst *a=&analysis.inst[pc],*b=&analysis.inst[pc+1];
        if(arithmetic(plan->code[pc].op)&&arithmetic(plan->code[pc+1].op)&&
           a->block==b->block && (a->writes & b->reads) &&
           a->effects==KSN_PA_EFFECT_REG && b->effects==KSN_PA_EFFECT_REG &&
           !(a->failure & ~(KSN_PA_FAIL_FINITE|KSN_PA_FAIL_STEPS)) &&
           !(b->failure & ~(KSN_PA_FAIL_FINITE|KSN_PA_FAIL_STEPS))){
            plan->fused_at[pc]=1;plan->fused_count++;pc++;
        }
    }
    plan->valid=true;
    return true;
}
ksn_proc_status ksn_proc_plan_begin_state(ksn_proc_vm *vm,const ksn_proc_plan *plan,
                                          const float input[KSN_PROC_INPUTS],
                                          const ksn_proc_state *state,ksn_proc_frame *frame){
    /* Bind the owned array here: a struct copy must not execute the original
     * plan's storage. Failed begins also invalidate a previous ready result. */
    if(!plan||!plan->valid)
        return ksn_proc_begin_state(vm,NULL,input,state,frame);
    const ksn_proc_program program={plan->code,plan->program.count};
    return ksn_proc_begin_state(vm,&program,input,state,frame);
}
ksn_proc_status ksn_proc_plan_begin(ksn_proc_vm *vm,const ksn_proc_plan *plan,
                                    const float input[KSN_PROC_INPUTS],ksn_proc_frame *frame){
    return ksn_proc_plan_begin_state(vm,plan,input,NULL,frame);
}
static ksn_proc_status scalar_step(ksn_proc_vm *vm,const ksn_proc_inst *i){
    if(vm->steps==KSN_PROC_STEPS)return vm->status=KSN_PROC_LIMIT;
    vm->last_pc=vm->pc;vm->pc++;vm->steps++;
    switch(i->op){
    case KSN_PROC_ADD:vm->reg[i->dst]=vm->reg[i->a]+vm->reg[i->b];break;
    case KSN_PROC_MUL:vm->reg[i->dst]=vm->reg[i->a]*vm->reg[i->b];break;
    case KSN_PROC_SIN:vm->reg[i->dst]=sinf(vm->reg[i->a]);break;
    default:return vm->status=KSN_PROC_INVALID;
    }
    /* Only dst can change; begin and prior steps already checked every reg. */
    if(!isfinite(vm->reg[i->dst]))return vm->status=KSN_PROC_INVALID;
    if(vm->pc==vm->program->count){vm->frame->ready=true;return vm->status=KSN_PROC_DONE;}
    return vm->status;
}
ksn_proc_status ksn_proc_plan_run(ksn_proc_vm *vm,const ksn_proc_plan *plan,bool debug_step){
    if(!vm)return KSN_PROC_INVALID;
    if(vm->status!=KSN_PROC_RUNNING)return vm->status;
    /* A VM from another registration takes the reference path. */
    if(!plan||!plan->valid||!vm->program||vm->program!=&vm->owned_program||
       vm->program->count!=plan->program.count||
       memcmp(vm->owned_code,plan->code,plan->program.count*sizeof plan->code[0]))
        return ksn_proc_run(vm);
    while(vm->status==KSN_PROC_RUNNING){
        uint8_t pc=vm->pc;
        if(!debug_step && plan->fused_at[pc]){
            scalar_step(vm,&plan->code[pc]);
            if(vm->status==KSN_PROC_RUNNING)
                scalar_step(vm,&plan->code[pc+1]);
        }else ksn_proc_step(vm);
    }
    return vm->status;
}
