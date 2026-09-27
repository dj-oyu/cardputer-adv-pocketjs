#include "ksn_procedural.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static bool reg_ok(uint8_t r){return r<KSN_PROC_REGS;}
static bool valid_program(const ksn_proc_program *p,uint8_t loop_end[KSN_PROC_CODE]){
    if(!p||!p->code||!p->count||p->count>KSN_PROC_CODE)return false;
    uint8_t open[KSN_PROC_LOOP_DEPTH];
    unsigned depth=0;
    for(unsigned pc=0;pc<p->count;pc++){
        const ksn_proc_inst *i=&p->code[pc];
        switch(i->op){
        case KSN_PROC_SET: if(!reg_ok(i->dst)||!isfinite(i->value))return false;break;
        case KSN_PROC_INPUT: if(!reg_ok(i->dst)||i->a>=KSN_PROC_INPUTS)return false;break;
        case KSN_PROC_ADD: case KSN_PROC_MUL:
            if(!reg_ok(i->dst)||!reg_ok(i->a)||!reg_ok(i->b))return false;
            break;
        case KSN_PROC_SIN: if(!reg_ok(i->dst)||!reg_ok(i->a))return false;break;
        case KSN_PROC_REPEAT: case KSN_PROC_REPEAT_REG:
            if((i->op==KSN_PROC_REPEAT&&!i->a)||
               (i->op==KSN_PROC_REPEAT_REG&&!reg_ok(i->a))||
               depth==KSN_PROC_LOOP_DEPTH)return false;
            open[depth++]=(uint8_t)pc;break;
        case KSN_PROC_END:
            if(!depth)return false;
            loop_end[open[--depth]]=(uint8_t)pc;
            break;
        case KSN_PROC_BREAK_IF_GT:
            if(!depth||!reg_ok(i->a)||!reg_ok(i->b))return false;
            break;
        case KSN_PROC_PLOT_COLOR_REG: case KSN_PROC_LINE_COLOR_REG:
            if(!reg_ok(i->dst))return false;
            if(!reg_ok(i->a)||!reg_ok(i->b))return false;
            break;
        case KSN_PROC_MOVE: case KSN_PROC_PLOT: case KSN_PROC_LINE:
            if(!reg_ok(i->a)||!reg_ok(i->b))return false;
            break;
        default:return false;
        }
    }
    return depth==0;
}
void ksn_proc_state_reset(ksn_proc_state *state){
    if(state)memset(state,0,sizeof *state);
}
ksn_proc_status ksn_proc_begin_state(ksn_proc_vm *vm,const ksn_proc_program *program,
                                    const float input[KSN_PROC_INPUTS],
                                    const ksn_proc_state *state,ksn_proc_frame *frame){
    if(!vm||!frame)return KSN_PROC_INVALID;
    memset(vm,0,sizeof *vm);
    frame->ready=false;frame->count=0;frame->raster_steps=0;
    vm->status=KSN_PROC_INVALID;
    if(!input||!program||!program->code||!program->count||
       program->count>KSN_PROC_CODE)return vm->status;
    /* Copy before validation, so validation and execution see the same bytes. */
    memcpy(vm->owned_code,program->code,program->count*sizeof vm->owned_code[0]);
    vm->owned_program=(ksn_proc_program){vm->owned_code,program->count};
    if(!valid_program(&vm->owned_program,vm->loop_end))return vm->status;
    for(unsigned j=0;j<KSN_PROC_INPUTS;j++)if(!isfinite(input[j]))return vm->status;
    if(state){
        for(unsigned j=0;j<KSN_PROC_REGS;j++)if(!isfinite(state->reg[j]))return vm->status;
        if(state->pen_valid&&(state->pen_x < -480||state->pen_x > 720||
                              state->pen_y < -480||state->pen_y > 720))return vm->status;
        memcpy(vm->reg,state->reg,sizeof vm->reg);
        vm->pen_x=state->pen_x;vm->pen_y=state->pen_y;
        vm->pen_valid=state->pen_valid;
    }
    memcpy(vm->input,input,sizeof vm->input);
    vm->program=&vm->owned_program;vm->frame=frame;vm->status=KSN_PROC_RUNNING;
    return vm->status;
}
ksn_proc_status ksn_proc_begin(ksn_proc_vm *vm,const ksn_proc_program *program,
                               const float input[KSN_PROC_INPUTS],ksn_proc_frame *frame){
    return ksn_proc_begin_state(vm,program,input,NULL,frame);
}
bool ksn_proc_capture_state(const ksn_proc_vm *vm,ksn_proc_state *state){
    if(!vm||!state||vm->status!=KSN_PROC_DONE||!vm->frame||!vm->frame->ready)return false;
    memcpy(state->reg,vm->reg,sizeof state->reg);
    state->pen_x=vm->pen_x;state->pen_y=vm->pen_y;
    state->pen_valid=vm->pen_valid;
    return true;
}
static bool coordinate(float f,int16_t *out){
    /* Keep line traversal bounded even for malformed or adversarial input. */
    if(!isfinite(f)||f < -480.0f||f > 720.0f)return false;
    *out=(int16_t)lroundf(f);return true;
}
static bool emit(ksn_proc_vm *vm,int16_t x0,int16_t y0,int16_t x1,int16_t y1,uint16_t color){
    if(vm->frame->count==KSN_PROC_SEGMENTS)return false;
    unsigned dx=(unsigned)abs(x1-x0),dy=(unsigned)abs(y1-y0);
    unsigned cost=(dx>dy?dx:dy)+1;
    if(vm->frame->raster_steps+cost>KSN_PROC_RASTER_STEPS)return false;
    vm->frame->raster_steps=(uint16_t)(vm->frame->raster_steps+cost);
    vm->frame->segments[vm->frame->count++]=(ksn_proc_segment){x0,y0,x1,y1,color};
    return true;
}
ksn_proc_status ksn_proc_step(ksn_proc_vm *vm){
    if(!vm)return KSN_PROC_INVALID;
    if(vm->status!=KSN_PROC_RUNNING)return vm->status;
    if(vm->steps==KSN_PROC_STEPS)return vm->status=KSN_PROC_LIMIT;
    const ksn_proc_inst *i=&vm->program->code[vm->pc];
    vm->last_pc=vm->pc;vm->pc++;vm->steps++;
    int16_t x=0,y=0;
    switch(i->op){
    case KSN_PROC_SET:vm->reg[i->dst]=i->value;break;
    case KSN_PROC_INPUT:vm->reg[i->dst]=vm->input[i->a];break;
    case KSN_PROC_ADD:vm->reg[i->dst]=vm->reg[i->a]+vm->reg[i->b];break;
    case KSN_PROC_MUL:vm->reg[i->dst]=vm->reg[i->a]*vm->reg[i->b];break;
    case KSN_PROC_SIN:vm->reg[i->dst]=sinf(vm->reg[i->a]);break;
    case KSN_PROC_REPEAT: case KSN_PROC_REPEAT_REG: {
        unsigned count=i->a;
        if(i->op==KSN_PROC_REPEAT_REG){
            float n=vm->reg[i->a];
            if(!isfinite(n)||n<0||n>255||n!=(float)(unsigned)n)
                return vm->status=KSN_PROC_INVALID;
            count=(unsigned)n;
        }
        if(!count){vm->pc=vm->loop_end[vm->last_pc]+1;break;}
        vm->loop[vm->depth].pc=vm->last_pc;
        vm->loop[vm->depth].remaining=(uint16_t)count;
        vm->depth++;break;
    }
    case KSN_PROC_END:
        if(!vm->depth)return vm->status=KSN_PROC_INVALID;
        if(--vm->loop[vm->depth-1].remaining)vm->pc=vm->loop[vm->depth-1].pc+1;
        else vm->depth--;
        break;
    case KSN_PROC_BREAK_IF_GT:
        if(!vm->depth)return vm->status=KSN_PROC_INVALID;
        if(vm->reg[i->a]>vm->reg[i->b]){
            vm->pc=vm->loop_end[vm->loop[vm->depth-1].pc]+1;
            vm->depth--;
        }
        break;
    case KSN_PROC_MOVE: case KSN_PROC_PLOT: case KSN_PROC_LINE:
    case KSN_PROC_PLOT_COLOR_REG: case KSN_PROC_LINE_COLOR_REG: {
        uint16_t color=i->color;
        if(i->op==KSN_PROC_PLOT_COLOR_REG||i->op==KSN_PROC_LINE_COLOR_REG){
            float n=vm->reg[i->dst];
            if(!isfinite(n)||n<0||n>65535||n!=(float)(unsigned)n)
                return vm->status=KSN_PROC_INVALID;
            color=(uint16_t)n;
        }
        if(!coordinate(vm->reg[i->a],&x)||!coordinate(vm->reg[i->b],&y))
            return vm->status=KSN_PROC_INVALID;
        if(i->op==KSN_PROC_PLOT||i->op==KSN_PROC_PLOT_COLOR_REG){
            if(!emit(vm,x,y,x,y,color))return vm->status=KSN_PROC_LIMIT;
        }
        if((i->op==KSN_PROC_LINE||i->op==KSN_PROC_LINE_COLOR_REG)&&vm->pen_valid){
            if(!emit(vm,vm->pen_x,vm->pen_y,x,y,color))return vm->status=KSN_PROC_LIMIT;
        }
        vm->pen_x=x;vm->pen_y=y;vm->pen_valid=true;break;
    }
    default:return vm->status=KSN_PROC_INVALID;
    }
    for(unsigned j=0;j<KSN_PROC_REGS;j++)if(!isfinite(vm->reg[j]))return vm->status=KSN_PROC_INVALID;
    if(vm->pc==vm->program->count){vm->frame->ready=true;return vm->status=KSN_PROC_DONE;}
    return vm->status;
}
ksn_proc_status ksn_proc_run(ksn_proc_vm *vm){
    ksn_proc_status s;
    do{s=ksn_proc_step(vm);}while(s==KSN_PROC_RUNNING);
    return s;
}
bool ksn_proc_render_band(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height){
    if(!frame||!frame->ready||!pixels||y<0||height<0||y>KSN_PROC_H||height>KSN_PROC_H-y)return false;
    for(unsigned j=0;j<frame->count;j++){
        const ksn_proc_segment *s=&frame->segments[j];
        int ymin=s->y0<s->y1?s->y0:s->y1;
        int ymax=s->y0>s->y1?s->y0:s->y1;
        if(ymax<y||ymin>=y+height)continue;
        int x=s->x0,yy=s->y0,dx=abs(s->x1-s->x0),dy=abs(s->y1-s->y0);
        int sx=x<s->x1?1:-1,sy=yy<s->y1?1:-1,err=dx-dy;
        for(;;){
            if(x>=0&&x<KSN_PROC_W&&yy>=y&&yy<y+height)pixels[(yy-y)*KSN_PROC_W+x]=s->color;
            if(x==s->x1&&yy==s->y1)break;
            int twice=2*err;
            if(twice>-dy){err-=dy;x+=sx;}
            if(twice<dx){err+=dx;yy+=sy;}
        }
    }
    return true;
}
