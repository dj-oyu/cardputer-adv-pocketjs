#include "ksn_proc_plan.h"
#include <math.h>
#include <string.h>

static bool arithmetic(uint8_t op){
    return op==KSN_PROC_ADD||op==KSN_PROC_MUL||op==KSN_PROC_SIN;
}
static bool fused_at(const uint32_t fused[KSN_PROC_FUSED_WORDS],unsigned pc){
    return (fused[pc>>5]>>(pc&31u))&1u;
}
/* Shared by both plan forms: code already holds the plan's own copy of count
 * instructions. Marks fused pairs; false when the analysis rejects the IR. */
static bool mark_fused(const ksn_proc_inst *code,uint8_t count,
                       uint32_t fused[KSN_PROC_FUSED_WORDS],uint8_t *fused_count){
    memset(fused,0,KSN_PROC_FUSED_WORDS*sizeof fused[0]);
    *fused_count=0;
    const ksn_proc_program program={code,count};
    ksn_proc_analysis analysis;
    if(!ksn_proc_analyze(&program,&analysis))return false;
    for(uint8_t pc=0;pc+1<count;pc++){
        const ksn_pa_inst *a=&analysis.inst[pc],*b=&analysis.inst[pc+1];
        if(arithmetic(code[pc].op)&&arithmetic(code[pc+1].op)&&
           a->block==b->block && (a->writes & b->reads) &&
           a->effects==KSN_PA_EFFECT_REG && b->effects==KSN_PA_EFFECT_REG &&
           !(a->failure & ~(KSN_PA_FAIL_FINITE|KSN_PA_FAIL_STEPS)) &&
           !(b->failure & ~(KSN_PA_FAIL_FINITE|KSN_PA_FAIL_STEPS))){
            fused[pc>>5]|=1u<<(pc&31u);(*fused_count)++;pc++;
        }
    }
    return true;
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
    plan->valid=false;
    /* A typed batch belongs to this exact registration. A replacement IR
     * must explicitly register its own descriptor. */
    plan->points_registered=false;
    plan->program=(ksn_proc_program){plan->code,count};
    if(!mark_fused(plan->code,count,plan->fused,&plan->fused_count))return false;
    plan->valid=true;
    return true;
}
size_t ksn_proc_sized_plan_bytes(unsigned capacity){
    if(!capacity||capacity>KSN_PROC_CODE)return 0;
    return offsetof(ksn_proc_sized_plan,code)+capacity*sizeof(ksn_proc_inst);
}
bool ksn_proc_sized_plan_prepare(ksn_proc_sized_plan *plan,unsigned capacity,
                                 const ksn_proc_program *program){
    if(!plan||!capacity||capacity>KSN_PROC_CODE)return false;
    if(!program||!program->code||!program->count||program->count>capacity){
        /* Only the bytes this plan owns: capacity bounds the allocation. */
        memset(plan,0,ksn_proc_sized_plan_bytes(capacity));
        plan->capacity=(uint8_t)capacity;
        return false;
    }
    uint8_t count=program->count;
    memmove(plan->code,program->code,count*sizeof plan->code[0]);
    plan->capacity=(uint8_t)capacity;plan->count=count;
    plan->valid=false;plan->points_registered=false;
    if(!mark_fused(plan->code,count,plan->fused,&plan->fused_count))return false;
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
ksn_proc_status ksn_proc_sized_plan_begin(ksn_proc_vm *vm,const ksn_proc_sized_plan *plan,
                                          const float input[KSN_PROC_INPUTS],
                                          ksn_proc_frame *frame){
    if(!plan||!plan->valid)return ksn_proc_begin_state(vm,NULL,input,NULL,frame);
    const ksn_proc_program program={plan->code,plan->count};
    return ksn_proc_begin_state(vm,&program,input,NULL,frame);
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
/* valid is the caller's plan->valid; code/count/fused are its own storage. */
static ksn_proc_status run_core(ksn_proc_vm *vm,bool valid,const ksn_proc_inst *code,
                                uint8_t count,const uint32_t fused[KSN_PROC_FUSED_WORDS],
                                bool debug_step){
    if(!vm)return KSN_PROC_INVALID;
    if(vm->status!=KSN_PROC_RUNNING)return vm->status;
    /* A VM from another registration takes the reference path. */
    if(!valid||!vm->program||vm->program!=&vm->owned_program||
       vm->program->count!=count||
       memcmp(vm->owned_code,code,count*sizeof code[0]))
        return ksn_proc_run(vm);
    while(vm->status==KSN_PROC_RUNNING){
        uint8_t pc=vm->pc;
        if(!debug_step && fused_at(fused,pc)){
            scalar_step(vm,&code[pc]);
            if(vm->status==KSN_PROC_RUNNING)
                scalar_step(vm,&code[pc+1]);
        }else ksn_proc_step(vm);
    }
    return vm->status;
}
ksn_proc_status ksn_proc_plan_run(ksn_proc_vm *vm,const ksn_proc_plan *plan,bool debug_step){
    if(!plan)return run_core(vm,false,NULL,0,NULL,debug_step);
    return run_core(vm,plan->valid,plan->code,plan->program.count,plan->fused,debug_step);
}
ksn_proc_status ksn_proc_sized_plan_run(ksn_proc_vm *vm,const ksn_proc_sized_plan *plan,
                                        bool debug_step){
    if(!plan)return run_core(vm,false,NULL,0,NULL,debug_step);
    return run_core(vm,plan->valid,plan->code,plan->count,plan->fused,debug_step);
}
