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
        case KSN_PROC_CUBIC:
            if(i->dst!=0||i->a<1||i->a>64)return false;
            break;
        case KSN_PROC_LINE_PATTERN:
            if(!reg_ok(i->a)||!reg_ok(i->b)||i->dst>KSN_PROC_REGS-7||
               !(i->value>=0.0f&&i->value<=(float)KSN_PROC_PATTERN_BITS)||
               i->value!=(float)(unsigned)i->value)return false;
            break;
        case KSN_PROC_TILE:
            if(!reg_ok(i->a)||!reg_ok(i->b)||i->dst>KSN_PROC_REGS-5)return false;
            break;
        default:return false;
        }
    }
    return depth==0;
}
void ksn_proc_state_reset(ksn_proc_state *state){
    if(state)memset(state,0,sizeof *state);
}
bool ksn_proc_apply_binding(ksn_proc_inst *code,uint8_t count,const ksn_proc_binding *binding){
    if(!code||!binding||(binding->patches&&(!binding->patch||!binding->arg)))return false;
    for(unsigned k=0;k<binding->patches;k++){
        const ksn_proc_patch *p=&binding->patch[k];
        if(p->pc>=count||p->param>=binding->args||(k&&p->pc<binding->patch[k-1].pc))
            return false;
        const float v=binding->arg[p->param];
        ksn_proc_inst *i=&code[p->pc];
        switch(p->field){
        /* An integer field takes only what the registration's number check
         * would have taken (pocket_proc.c entry()): no truncation here. */
        case KSN_PROC_FIELD_A:
            if(!(v>=0.0f&&v<=255.0f)||v!=(float)(uint8_t)v)return false;
            i->a=(uint8_t)v;break;
        case KSN_PROC_FIELD_COLOR:
            if(!(v>=0.0f&&v<=65535.0f)||v!=(float)(uint16_t)v)return false;
            i->color=(uint16_t)v;break;
        case KSN_PROC_FIELD_VALUE:
            if(!isfinite(v))return false;
            i->value=v;break;
        default:return false;
        }
    }
    return true;
}
static ksn_proc_status begin_core(ksn_proc_vm *vm,const ksn_proc_program *program,
                                  const ksn_proc_binding *binding,
                                  const float input[KSN_PROC_INPUTS],
                                  const ksn_proc_state *state,ksn_proc_frame *frame){
    if(!vm||!frame)return KSN_PROC_INVALID;
    memset(vm,0,sizeof *vm);
    frame->ready=false;frame->count=0;frame->raster_steps=0;frame->ext=0;
    vm->status=KSN_PROC_INVALID;
    if(!input||!program||!program->code||!program->count||
       program->count>KSN_PROC_CODE)return vm->status;
    /* Copy before validation, so validation and execution see the same bytes. */
    memcpy(vm->owned_code,program->code,program->count*sizeof vm->owned_code[0]);
    vm->owned_program=(ksn_proc_program){vm->owned_code,program->count};
    /* Arguments go into the VM's copy, before validation sees it. */
    if(binding&&!ksn_proc_apply_binding(vm->owned_code,program->count,binding))
        return vm->status;
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
ksn_proc_status ksn_proc_begin_state(ksn_proc_vm *vm,const ksn_proc_program *program,
                                    const float input[KSN_PROC_INPUTS],
                                    const ksn_proc_state *state,ksn_proc_frame *frame){
    return begin_core(vm,program,NULL,input,state,frame);
}
ksn_proc_status ksn_proc_begin_bound(ksn_proc_vm *vm,const ksn_proc_program *program,
                                     const ksn_proc_binding *binding,
                                     const float input[KSN_PROC_INPUTS],ksn_proc_frame *frame){
    /* NULL would silently run the placeholders: a bound begin needs one. */
    if(!binding){
        if(vm&&frame){memset(vm,0,sizeof *vm);vm->status=KSN_PROC_INVALID;
            frame->ready=false;frame->count=0;frame->raster_steps=0;frame->ext=0;}
        return KSN_PROC_INVALID;
    }
    return begin_core(vm,program,binding,input,NULL,frame);
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
static const ksn_proc_tile *tile_table;
static unsigned tile_count;
void ksn_proc_set_tiles(const ksn_proc_tile *tiles,unsigned count){
    tile_table=tiles;tile_count=tiles?count:0;
}
#ifdef KSN_PROC_STATS
ksn_proc_stats g_ksn_proc_stats;
#define STAT(field,n) (g_ksn_proc_stats.field+=(unsigned long)(n))
#else
#define STAT(field,n) ((void)0)
#endif
/* Geometry and parameters as two entries, or neither. */
static bool emit_ext(ksn_proc_vm *vm,int bias,int16_t x0,int16_t y0,int16_t x1,int16_t y1,
                     uint16_t color,unsigned cost,const uint16_t parameter[5]){
    ksn_proc_frame *f=vm->frame;
    if(f->count>KSN_PROC_SEGMENTS-2)return false;
    if(f->raster_steps+cost>KSN_PROC_RASTER_STEPS)return false;
    f->raster_steps=(uint16_t)(f->raster_steps+cost);
    f->segments[f->count++]=(ksn_proc_segment){(int16_t)(x0+bias),y0,x1,y1,color};
    f->segments[f->count++]=(ksn_proc_segment){(int16_t)parameter[0],(int16_t)parameter[1],
        (int16_t)parameter[2],(int16_t)parameter[3],parameter[4]};
    f->ext=1;
    return true;
}
/* A register as an integer in low..high, exactly. */
static bool whole(float n,float low,float high,int32_t *out){
    if(!(n>=low&&n<=high)||n!=(float)(int32_t)n)return false;
    *out=(int32_t)n;return true;
}
/* A texture coordinate running from u0 to u1 over `steps` pixels, as a start
 * and a step in 8.8 fixed point inside one period (cells or texels). The
 * arithmetic is modular, so a step backwards is the period minus it. */
static bool phase(float u0,float u1,unsigned steps,unsigned period,uint16_t *u0q,uint16_t *duq){
    if(!(u0>=-1048576.0f&&u0<=1048576.0f)||!(u1>=-1048576.0f&&u1<=1048576.0f))return false;
    const float du=steps?(u1-u0)/(float)steps:0.0f;
    if(!(du>-256.0f&&du<256.0f))return false;
    const int32_t wrap=(int32_t)(period*256u);
    float m=fmodf(u0,(float)period);
    if(m<0)m+=(float)period;
    *u0q=(uint16_t)((int32_t)lroundf(m*256.0f)%wrap);
    *duq=(uint16_t)(((int32_t)lroundf(du*256.0f)%wrap+wrap)%wrap);
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
    /* Only arithmetic can produce a non-finite register: SET values and inputs
     * are checked at validation/begin, and every other op writes no register.
     * Checking the one dst keeps a step O(1) in KSN_PROC_REGS; the former
     * sweep of all registers doubled when the file grew from 8 to 16. */
    case KSN_PROC_ADD:vm->reg[i->dst]=vm->reg[i->a]+vm->reg[i->b];goto finite;
    case KSN_PROC_MUL:vm->reg[i->dst]=vm->reg[i->a]*vm->reg[i->b];goto finite;
    case KSN_PROC_SIN:vm->reg[i->dst]=sinf(vm->reg[i->a]);
    finite:
        if(!isfinite(vm->reg[i->dst]))return vm->status=KSN_PROC_INVALID;
        break;
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
    case KSN_PROC_CUBIC: {
        float px[4],py[4];
        for(unsigned p=0;p<4;p++){
            px[p]=vm->reg[i->dst+2*p];
            py[p]=vm->reg[i->dst+2*p+1];
            if(!coordinate(px[p],&x)||!coordinate(py[p],&y))
                return vm->status=KSN_PROC_INVALID;
        }
        int16_t previous_x,previous_y;
        if(!coordinate(px[0],&previous_x)||!coordinate(py[0],&previous_y))
            return vm->status=KSN_PROC_INVALID;
        for(unsigned step=1;step<=i->a;step++){
            float t=(float)step/(float)i->a,u=1.0f-t;
            float bx=u*u*u*px[0]+3.0f*u*u*t*px[1]+
                     3.0f*u*t*t*px[2]+t*t*t*px[3];
            float by=u*u*u*py[0]+3.0f*u*u*t*py[1]+
                     3.0f*u*t*t*py[2]+t*t*t*py[3];
            if(!coordinate(bx,&x)||!coordinate(by,&y))
                return vm->status=KSN_PROC_INVALID;
            if(!emit(vm,previous_x,previous_y,x,y,i->color))
                return vm->status=KSN_PROC_LIMIT;
            previous_x=x;previous_y=y;
        }
        vm->pen_x=previous_x;vm->pen_y=previous_y;vm->pen_valid=true;
        break;
    }
    case KSN_PROC_LINE_PATTERN: {
        const float *r=&vm->reg[i->dst];
        int32_t bits,first,other;
        const unsigned n=(unsigned)i->value;
        const unsigned period=n?n:KSN_PROC_NOISE_CELLS;
        uint16_t p[5];
        if(!whole(r[0],0.0f,16777215.0f,&bits)||!whole(r[1],0.0f,65535.0f,&first)||
           !whole(r[2],-1.0f,65535.0f,&other)||
           !coordinate(vm->reg[i->a],&x)||!coordinate(vm->reg[i->b],&y))
            return vm->status=KSN_PROC_INVALID;
        if(vm->pen_valid){
            /* Depth: with weights (1/Z) of one sign and unlike size at the
             * two ends, the pattern is laid as on a line receding in space:
             * the line is cut into chords here, natively, each linear, with
             * the perspective-correct coordinate at every cut. */
            const float w0=fabsf(r[5]),w1=fabsf(r[6]);
            unsigned chords=1;
            if(r[5]*r[6]>0.0f){
                const float ratio=w0>w1?w0/w1:w1/w0;
                chords=ratio<1.02f?1u:ratio<1.2f?2u:ratio<1.6f?4u:8u;
            }
            p[0]=(uint16_t)bits;
            p[1]=(uint16_t)((uint32_t)bits>>16|n<<8|(other>=0?1u<<13:0u));
            p[4]=(uint16_t)(other>=0?other:0);
            int16_t ax=vm->pen_x,ay=vm->pen_y;
            float au=r[3];
            for(unsigned j=1;j<=chords;j++){
                int16_t bx=x,by=y;
                float bu=r[4];
                if(j<chords){
                    const float t=(float)j/(float)chords,near=w0*(1.0f-t),far=w1*t;
                    bu=(r[3]*near+r[4]*far)/(near+far);
                    if(!coordinate((float)vm->pen_x+(float)(x-vm->pen_x)*t,&bx)||
                       !coordinate((float)vm->pen_y+(float)(y-vm->pen_y)*t,&by))
                        return vm->status=KSN_PROC_INVALID;
                }
                unsigned dx=(unsigned)abs(bx-ax),dy=(unsigned)abs(by-ay);
                if(!phase(au,bu,dx>dy?dx:dy,period,&p[2],&p[3]))
                    return vm->status=KSN_PROC_INVALID;
                if(!emit_ext(vm,KSN_PROC_EXT_PATTERN,ax,ay,bx,by,(uint16_t)first,
                             (dx>dy?dx:dy)+1,p))return vm->status=KSN_PROC_LIMIT;
                ax=bx;ay=by;au=bu;
            }
        }
        vm->pen_x=x;vm->pen_y=y;vm->pen_valid=true;break;
    }
    case KSN_PROC_TILE: {
        const float *r=&vm->reg[i->dst];
        uint16_t p[5];
        if(i->color>=tile_count||i->color>255||!(r[4]>=-1048576.0f&&r[4]<=1048576.0f)||
           !coordinate(vm->reg[i->a],&x)||!coordinate(vm->reg[i->b],&y))
            return vm->status=KSN_PROC_INVALID;
        if(vm->pen_valid){
            /* Stored left top to right bottom, with the texel of that corner:
             * (u0,v0) is the pen's, (u1,v1) the other's, whichever is left. */
            const bool pen_left=vm->pen_x<=x,pen_top=vm->pen_y<=y;
            int16_t l=pen_left?vm->pen_x:x,r1=pen_left?x:vm->pen_x;
            int16_t t=pen_top?vm->pen_y:y,b=pen_top?y:vm->pen_y;
            if(!phase(pen_left?r[0]:r[2],pen_left?r[2]:r[0],(unsigned)(r1-l+1),
                      tile_table[i->color].w,&p[1],&p[3])||
               !phase(pen_top?r[1]:r[3],pen_top?r[3]:r[1],(unsigned)(b-t+1),
                      tile_table[i->color].h,&p[2],&p[4]))
                return vm->status=KSN_PROC_INVALID;
            /* The frame is the register's floor, wrapped: a fraction is a
             * phase the caller may keep there (or a second value, scaled). */
            const int32_t frames=tile_table[i->color].frames;
            const int32_t shot=(((int32_t)floorf(r[4]))%frames+frames)%frames;
            p[0]=(uint16_t)(i->color|(uint32_t)shot<<8);
            if(!emit_ext(vm,KSN_PROC_EXT_TILE,l,t,r1,b,0,
                         KSN_PROC_TILE_COST((unsigned)(r1-l+1)*(unsigned)(b-t+1)),p))
                return vm->status=KSN_PROC_LIMIT;
        }
        vm->pen_x=x;vm->pen_y=y;vm->pen_valid=true;break;
    }
    default:return vm->status=KSN_PROC_INVALID;
    }
#ifdef KASANE_PROC_LIMITS_PROBE
    /* The instruction cache moves one kernel by ~15% between builds, so the
     * old sweep is re-created here behind a runtime switch instead of being
     * compared across two images. */
    for(unsigned j=0;j<g_ksn_proc_sweep_regs;j++)
        if(!isfinite(vm->reg[j]))return vm->status=KSN_PROC_INVALID;
#endif
    if(vm->pc==vm->program->count){vm->frame->ready=true;return vm->status=KSN_PROC_DONE;}
    return vm->status;
}
#ifdef KASANE_PROC_LIMITS_PROBE
uint8_t g_ksn_proc_sweep_regs;
#endif
ksn_proc_status ksn_proc_run(ksn_proc_vm *vm){
    ksn_proc_status s;
    do{s=ksn_proc_step(vm);}while(s==KSN_PROC_RUNNING);
    return s;
}
/* One plain line's pixels inside rows y..y+height-1. */
static inline __attribute__((always_inline)) void line_band(const ksn_proc_segment *s,uint16_t *pixels,int y,int height){
    int x=s->x0,yy=s->y0,dx=abs(s->x1-s->x0),dy=abs(s->y1-s->y0);
    int sx=x<s->x1?1:-1,sy=yy<s->y1?1:-1,err=dx-dy;
    if(dy){
        /* A band can be reached without replaying the line's prefix. The
         * original Bresenham phase is reconstructed from its major-axis
         * step count; clipping endpoints and restarting would change ties. */
        int row=sy>0?y-s->y0:s->y0-(y+height-1);
        if(row>0){
            int major,minor,x_steps,y_steps;
            if(dx>=dy){
                int64_t numerator=(int64_t)row*dx-(dx-1)/2;
                major=numerator>0?(int)((numerator+dy-1)/dy):0;
                minor=(int)(((int64_t)major*dy+(dx-1)/2)/dx);
                x_steps=major;y_steps=minor;
            }else{
                major=row;
                minor=(int)(((int64_t)major*dx+(dy-1)/2)/dy);
                x_steps=minor;y_steps=major;
            }
            x+=sx*x_steps;yy+=sy*y_steps;
            err=(int)((int64_t)dx-dy-(int64_t)x_steps*dy+(int64_t)y_steps*dx);
        }
    }
    for(;;){
        if((sy>0&&yy>=y+height)||(sy<0&&yy<y))break;
        if(x>=0&&x<KSN_PROC_W&&yy>=y&&yy<y+height){
            pixels[(yy-y)*KSN_PROC_W+x]=s->color;STAT(line_px,1);
        }
        if(x==s->x1&&yy==s->y1)break;
        int twice=2*err;
        if(twice>-dy){err-=dy;x+=sx;}
        if(twice<dx){err+=dx;yy+=sy;}
    }
}
/* The colour of one pattern cell, or false for a cell that draws nothing. */
static inline bool pattern_cell(uint32_t bits,unsigned n,bool has_b,unsigned cell,
                                uint16_t a,uint16_t b,uint16_t *out){
    if(n){
        if(bits>>cell&1u){*out=a;return true;}
        *out=b;return has_b;
    }
    /* The seed is mixed in by its own multiply, so seed + 1 is another
     * sequence, not this one moved by a cell. */
    uint32_t h=cell*0x9e3779b1u^(bits&0xffffu)*0x85ebca6bu;
    h^=h>>15;h*=0x85ebca77u;h^=h>>13;
    if((h&0xffu)>=(bits>>16&0xffu))return false;
    *out=has_b&&(h>>8&1u)?b:a;
    return true;
}
/* A pattern line: line_band's walk with the pixel index k carried along. A
 * horizontal line (a crowd's row) skips the walk. */
static void pattern_band(const ksn_proc_segment *g,const ksn_proc_segment *q,
                         uint16_t *pixels,int y,int height){
    const int x0=g->x0-KSN_PROC_EXT_PATTERN;
    const uint32_t bits=(uint16_t)q->x0|(uint32_t)((uint16_t)q->y0&0xffu)<<16;
    const unsigned n=(uint16_t)q->y0>>8&31u;
    const bool has_b=(uint16_t)q->y0>>13&1u;
    const uint32_t wrap=(n?n:KSN_PROC_NOISE_CELLS)*256u,u0=(uint16_t)q->x1,du=(uint16_t)q->y1;
    const uint16_t a=g->color,b=q->color;
    uint16_t c;
    if(g->y0==g->y1){
        int left=x0<g->x1?x0:g->x1,right=x0<g->x1?g->x1:x0;
        if(left<0)left=0;
        if(right>=KSN_PROC_W)right=KSN_PROC_W-1;
        if(left>right)return;
        /* k counts from the pen, whichever side that is. */
        const bool forward=x0<=g->x1;
        const uint32_t k=(uint32_t)(forward?left-x0:x0-left);
        /* Left to right whatever the pen's side: backwards is wrap - du. */
        const uint32_t step=forward?du:(wrap-du)%wrap;
        /* 32 bits hold it: u0 and du are under 2^13, k at most 1,200. */
        uint32_t u=(u0+k*du)%wrap;
        uint16_t *row=pixels+(g->y0-y)*KSN_PROC_W;
        for(int x=left;x<=right;x++){
            STAT(pattern_px,1);
            if(pattern_cell(bits,n,has_b,u>>8,a,b,&c)){row[x]=c;STAT(pattern_written,1);}
            u+=step;if(u>=wrap)u-=wrap;
        }
        return;
    }
    int x=x0,yy=g->y0,dx=abs(g->x1-x0),dy=abs(g->y1-g->y0);
    int sx=x<g->x1?1:-1,sy=yy<g->y1?1:-1,err=dx-dy;
    uint32_t k=0;
    int row=sy>0?y-g->y0:g->y0-(y+height-1);
    if(row>0){
        int major,minor,x_steps,y_steps;
        if(dx>=dy){
            int64_t numerator=(int64_t)row*dx-(dx-1)/2;
            major=numerator>0?(int)((numerator+dy-1)/dy):0;
            minor=(int)(((int64_t)major*dy+(dx-1)/2)/dx);
            x_steps=major;y_steps=minor;
        }else{
            major=row;
            minor=(int)(((int64_t)major*dx+(dy-1)/2)/dy);
            x_steps=minor;y_steps=major;
        }
        x+=sx*x_steps;yy+=sy*y_steps;k=(uint32_t)major;
        err=(int)((int64_t)dx-dy-(int64_t)x_steps*dy+(int64_t)y_steps*dx);
    }
    uint32_t u=(u0+k*du)%wrap;
    for(;;){
        if((sy>0&&yy>=y+height)||(sy<0&&yy<y))break;
        if(x>=0&&x<KSN_PROC_W&&yy>=y&&yy<y+height){
            STAT(pattern_px,1);STAT(pattern_walk_px,1);
            if(pattern_cell(bits,n,has_b,u>>8,a,b,&c)){
                pixels[(yy-y)*KSN_PROC_W+x]=c;STAT(pattern_written,1);
            }
        }
        if(x==g->x1&&yy==g->y1)break;
        int twice=2*err;
        /* Each step moves the major axis by one, so k is the pixel index. */
        if(twice>-dy){err-=dy;x+=sx;}
        if(twice<dx){err+=dx;yy+=sy;}
        u+=du;if(u>=wrap)u-=wrap;
    }
}
static void tile_band(const ksn_proc_segment *g,const ksn_proc_segment *q,
                      uint16_t *pixels,int y,int height){
    const unsigned id=(uint16_t)q->x0&0xffu,shot=(uint16_t)q->x0>>8;
    if(id>=tile_count||shot>=tile_table[id].frames)return;
    const ksn_proc_tile *t=&tile_table[id];
    const int x0=g->x0-KSN_PROC_EXT_TILE;
    int left=x0<0?0:x0,right=g->x1>=KSN_PROC_W?KSN_PROC_W-1:g->x1;
    int top=g->y0<y?y:g->y0,bottom=g->y1>=y+height?y+height-1:g->y1;
    if(left>right||top>bottom)return;
    const uint32_t uwrap=t->w*256u,vwrap=t->h*256u;
    const uint32_t du=(uint16_t)q->y1,dv=q->color;
    /* 32 bits hold these: u0, v0, du, dv are under 2^16, the offsets at most 1,200. */
    const uint32_t ustart=((uint16_t)q->y0+(uint32_t)(left-x0)*du)%uwrap;
    const uint16_t *image=t->px+(size_t)shot*t->w*t->h,key=t->key;
    for(int yy=top;yy<=bottom;yy++){
        const uint32_t v=((uint16_t)q->x1+(uint32_t)(yy-g->y0)*dv)%vwrap;
        const uint16_t *texel=image+(v>>8)*t->w;
        uint16_t *row=pixels+(yy-y)*KSN_PROC_W;
        uint32_t u=ustart;
        STAT(tile_rows,1);
        for(int x=left;x<=right;x++){
            const uint16_t c=texel[u>>8];
            STAT(tile_px,1);
            if(c!=key){row[x]=c;STAT(tile_written,1);}
            u+=du;if(u>=uwrap)u-=uwrap;
        }
    }
}
/* A frame with extended entries: the same order, the same lines. */
static void render_band_ext(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height){
    for(unsigned j=0;j<frame->count;j++){
        const ksn_proc_segment *s=&frame->segments[j];
        const bool ext=s->x0>=KSN_PROC_EXT_MIN;
        if(ext&&j+1>=frame->count)return;
        int ymin=s->y0<s->y1?s->y0:s->y1;
        int ymax=s->y0>s->y1?s->y0:s->y1;
        STAT(scans,1);
        if(ext)j++;
        if(ymax<y||ymin>=y+height)continue;
        STAT(entries,1);
        if(!ext)line_band(s,pixels,y,height);
        else if(s->x0>=KSN_PROC_EXT_TILE-KSN_PROC_EXT_MIN)tile_band(s,s+1,pixels,y,height);
        else pattern_band(s,s+1,pixels,y,height);
    }
}
bool ksn_proc_render_band(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height){
    if(!frame||!frame->ready||!pixels||y<0||height<0||y>KSN_PROC_H||height>KSN_PROC_H-y)return false;
    if(frame->ext){render_band_ext(frame,pixels,y,height);return true;}
    for(unsigned j=0;j<frame->count;j++){
        const ksn_proc_segment *s=&frame->segments[j];
        int ymin=s->y0<s->y1?s->y0:s->y1;
        int ymax=s->y0>s->y1?s->y0:s->y1;
        STAT(scans,1);
        if(ymax<y||ymin>=y+height)continue;
        STAT(entries,1);
        line_band(s,pixels,y,height);
    }
    return true;
}
