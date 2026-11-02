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
    case KSN_PROC_SIN:vm->reg[i->dst]=ksn_proc_sinf(vm->reg[i->a]);break;
    default:return vm->status=KSN_PROC_INVALID;
    }
    /* Only dst can change; begin and prior steps already checked every reg. */
    if(!isfinite(vm->reg[i->dst]))return vm->status=KSN_PROC_INVALID;
    if(vm->pc==vm->program->count){vm->frame->ready=true;return vm->status=KSN_PROC_DONE;}
    return vm->status;
}
static ksn_proc_status run_loop(ksn_proc_vm *vm,const ksn_proc_inst *code,
                                const uint32_t fused[KSN_PROC_FUSED_WORDS],bool debug_step);
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
    return run_loop(vm,code,fused,debug_step);
}
/* code is the VM's own program, already matched against the plan. */
static ksn_proc_status run_loop(ksn_proc_vm *vm,const ksn_proc_inst *code,
                                const uint32_t fused[KSN_PROC_FUSED_WORDS],bool debug_step){
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
/* ---- draw: begin and run as one call (pocket_proc.c draw_run) ----
 * begin + run redid on every draw work whose answer was fixed when the plan
 * was prepared: a memset of the whole VM (about 1 KB), the copy of the rows,
 * valid_program() over them, then a memcmp (rom_matches() for a built-in
 * plan) proving the VM still held this plan's rows. Here begin and run are
 * one call with nothing in between, so "the VM holds this registration's
 * rows" is true by construction, not checked; and no VM state outlives the
 * call that a later registration in the same slot or at the same address
 * could be taken for (the next draw starts over), so no generation number
 * is needed.
 *
 * Validation may be skipped because plan->valid is set only after
 * mark_fused()'s ksn_proc_analyze() accepted these exact rows, and analyze
 * rejects every row valid_program() rejects (the same per-op field checks,
 * loop depth and nesting; tools/kasane_contract/test_proc_plan_draw.c checks
 * that the two agree). A sized plan's rows are its own heap copy, written only
 * by prepare; a built-in plan's are const and its args are written only
 * before prepare. Neither can change inside a draw: no guest code runs there.
 *
 * Kept per draw: what depends on the draw (inputs, registers, pen, counters,
 * the frame header); loop_end, one pass over REPEAT/END (keeping it in the
 * plan would cost 64 B of DRAM per plan, up to 32 plans); and for a built-in
 * plan the copy out of flash with its arguments applied, since the rows the
 * VM runs must hold them and the table is const. A sized plan runs from its
 * own rows without a copy. VM fields the run never reads before writing
 * (loop[], owned_code past count, last_pc) are left as they were. */
static ksn_proc_status draw_begin(ksn_proc_vm *vm,const ksn_proc_inst *code,uint8_t count,
                                  const float input[KSN_PROC_INPUTS],ksn_proc_frame *frame){
    frame->ready=false;frame->count=0;frame->raster_steps=0;frame->ext=0;
    vm->status=KSN_PROC_INVALID;
    for(unsigned j=0;j<KSN_PROC_INPUTS;j++)if(!isfinite(input[j]))return vm->status;
    /* Prepare proved the nesting; the bounds are kept so a broken invariant
     * fails the draw instead of writing outside loop_end/open. */
    uint8_t open[KSN_PROC_LOOP_DEPTH];
    unsigned depth=0;
    for(unsigned pc=0;pc<count;pc++){
        const uint8_t op=code[pc].op;
        if(op==KSN_PROC_REPEAT||op==KSN_PROC_REPEAT_REG){
            if(depth==KSN_PROC_LOOP_DEPTH)return vm->status;
            open[depth++]=(uint8_t)pc;
        }else if(op==KSN_PROC_END){
            if(!depth)return vm->status;
            vm->loop_end[open[--depth]]=(uint8_t)pc;
        }
    }
    if(depth)return vm->status;
    memset(vm->reg,0,sizeof vm->reg);
    memcpy(vm->input,input,sizeof vm->input);
    vm->pc=0;vm->last_pc=0;vm->depth=0;vm->steps=0;
    vm->pen_x=0;vm->pen_y=0;vm->pen_valid=false;
    vm->owned_program=(ksn_proc_program){code,count};
    vm->program=&vm->owned_program;vm->frame=frame;
    return vm->status=KSN_PROC_RUNNING;
}
ksn_proc_status ksn_proc_sized_plan_draw(ksn_proc_vm *vm,const ksn_proc_sized_plan *plan,
                                         const float input[KSN_PROC_INPUTS],
                                         ksn_proc_frame *frame){
    if(!vm||!frame||!input||!plan||!plan->valid||!plan->count||plan->count>KSN_PROC_CODE)
        return ksn_proc_begin_state(vm,NULL,input,NULL,frame);
    if(draw_begin(vm,plan->code,plan->count,input,frame)!=KSN_PROC_RUNNING)return vm->status;
    return run_loop(vm,plan->code,plan->fused,false);
}
ksn_proc_status ksn_proc_rom_plan_draw(ksn_proc_vm *vm,const ksn_proc_rom_plan *plan,
                                       const float input[KSN_PROC_INPUTS],
                                       ksn_proc_frame *frame){
    if(!vm||!frame||!input||!plan||!plan->valid||!plan->rom||!plan->rom->count||
       plan->rom->count>KSN_PROC_CODE)
        return ksn_proc_begin_state(vm,NULL,input,NULL,frame);
    const ksn_proc_rom_entry *rom=plan->rom;
    memcpy(vm->owned_code,rom->code,rom->count*sizeof vm->owned_code[0]);
    const ksn_proc_binding binding={rom->patch,plan->args,rom->patches,plan->params};
    /* Succeeded at prepare with the same arguments; a failure here would mean
     * the plan changed after prepare, so the draw fails rather than run it. */
    if(!ksn_proc_apply_binding(vm->owned_code,rom->count,&binding)){
        frame->ready=false;frame->count=0;frame->raster_steps=0;frame->ext=0;
        return vm->status=KSN_PROC_INVALID;
    }
    if(draw_begin(vm,vm->owned_code,rom->count,input,frame)!=KSN_PROC_RUNNING)return vm->status;
    return run_loop(vm,vm->owned_code,plan->fused,false);
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

/* ---- built-in (flash) plans: docs/kasane/flash-plan.md */
size_t ksn_proc_rom_plan_bytes(unsigned params){
    if(params>KSN_PROC_ROM_PARAMS)return 0;
    const size_t bytes=offsetof(ksn_proc_rom_plan,args)+params*sizeof(float);
    return bytes<sizeof(ksn_proc_rom_plan)?sizeof(ksn_proc_rom_plan):bytes;
}
bool ksn_proc_rom_entry_valid(const ksn_proc_rom_entry *rom){
    if(!rom||!rom->code||!rom->count||rom->count>KSN_PROC_CODE||
       rom->params>KSN_PROC_ROM_PARAMS||(rom->patches&&!rom->patch))return false;
    for(unsigned k=0;k<rom->patches;k++){
        const ksn_proc_patch *p=&rom->patch[k];
        if(p->pc>=rom->count||p->param>=rom->params||p->field>KSN_PROC_FIELD_COLOR||
           (k&&p->pc<rom->patch[k-1].pc))return false;
    }
    return true;
}
bool ksn_proc_rom_plan_prepare(ksn_proc_rom_plan *plan,const ksn_proc_rom_entry *rom){
    if(!plan)return false;
    plan->valid=false;plan->points_registered=false;
    memset(plan->fused,0,sizeof plan->fused);plan->fused_count=0;
    plan->rom=rom;
    if(!ksn_proc_rom_entry_valid(rom)||plan->params!=rom->params)return false;
    /* The rows the array path would have registered: the analysis and the
     * fused marks see exactly them (the arguments decide REPEAT counts, which
     * the analysis bounds and validates). 768 B of stack, as register() has
     * for the rows it reads. */
    ksn_proc_inst code[KSN_PROC_CODE];
    memcpy(code,rom->code,rom->count*sizeof code[0]);
    const ksn_proc_binding binding={rom->patch,plan->args,rom->patches,plan->params};
    if(!ksn_proc_apply_binding(code,rom->count,&binding))return false;
    if(!mark_fused(code,rom->count,plan->fused,&plan->fused_count))return false;
    plan->valid=true;
    return true;
}
ksn_proc_status ksn_proc_rom_plan_begin(ksn_proc_vm *vm,const ksn_proc_rom_plan *plan,
                                        const float input[KSN_PROC_INPUTS],
                                        ksn_proc_frame *frame){
    if(!plan||!plan->valid)return ksn_proc_begin_state(vm,NULL,input,NULL,frame);
    const ksn_proc_program program={plan->rom->code,plan->rom->count};
    const ksn_proc_binding binding={plan->rom->patch,plan->args,plan->rom->patches,plan->params};
    return ksn_proc_begin_bound(vm,&program,&binding,input,frame);
}
/* The fused path's guard, as run_core's memcmp: the VM must hold this plan's
 * program with this registration's arguments. A patched field is compared
 * with its argument, then put back to the entry's placeholder so the rest of
 * the instruction (padding included) compares as bytes. */
static bool rom_matches(const ksn_proc_vm *vm,const ksn_proc_rom_plan *plan){
    const ksn_proc_rom_entry *rom=plan->rom;
    if(!vm->program||vm->program!=&vm->owned_program||vm->program->count!=rom->count)
        return false;
    unsigned k=0;
    for(unsigned pc=0;pc<rom->count;pc++){
        if(k==rom->patches||rom->patch[k].pc!=pc){
            if(memcmp(&vm->owned_code[pc],&rom->code[pc],sizeof rom->code[0]))return false;
            continue;
        }
        ksn_proc_inst got;
        memcpy(&got,&vm->owned_code[pc],sizeof got);
        for(;k<rom->patches&&rom->patch[k].pc==pc;k++){
            const ksn_proc_patch *p=&rom->patch[k];
            const float want=plan->args[p->param];
            switch(p->field){
            case KSN_PROC_FIELD_A:
                if((float)got.a!=want)return false;
                got.a=rom->code[pc].a;break;
            case KSN_PROC_FIELD_COLOR:
                if((float)got.color!=want)return false;
                got.color=rom->code[pc].color;break;
            default:
                if(memcmp(&got.value,&want,sizeof want))return false;
                got.value=rom->code[pc].value;break;
            }
        }
        if(memcmp(&got,&rom->code[pc],sizeof got))return false;
    }
    return true;
}
ksn_proc_status ksn_proc_rom_plan_run(ksn_proc_vm *vm,const ksn_proc_rom_plan *plan,
                                      bool debug_step){
    if(!vm)return KSN_PROC_INVALID;
    if(vm->status!=KSN_PROC_RUNNING)return vm->status;
    if(!plan||!plan->valid||!rom_matches(vm,plan))return ksn_proc_run(vm);
    return run_loop(vm,vm->owned_code,plan->fused,debug_step);
}
