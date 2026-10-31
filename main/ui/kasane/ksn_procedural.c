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
            if(!reg_ok(i->a)||!reg_ok(i->b)||i->dst>KSN_PROC_REGS-KSN_PROC_PATTERN_BLOCK||
               !(i->value>=1.0f&&i->value<=(float)KSN_PROC_PATTERN_BITS)||
               i->value!=(float)(unsigned)i->value)return false;
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
/* A register as an integer in low..high, exactly. */
static bool whole(float n,float low,float high,int32_t *out){
    if(!(n>=low&&n<=high)||n!=(float)(int32_t)n)return false;
    *out=(int32_t)n;return true;
}
/* Stores and reloads a float: the device's compiler contracts a*b+c into one
 * madd.s (-ffp-contract=fast is GCC's default), the host's does not, and a
 * chord's ends must round alike on both. */
static float rounded(float v){volatile float r=v;return r;}
/* The pattern coordinate from u0 to u1 over `steps` pixels as a start and a
 * step in 8.8 fixed point inside one period. The arithmetic is modular, so a
 * step backwards is the period minus it. */
static bool phase(float u0,float u1,unsigned steps,unsigned period,uint16_t *u0q,uint16_t *duq){
    const float du=steps?(u1-u0)/(float)steps:0.0f;
    if(!(du>-256.0f&&du<256.0f))return false;
    const int32_t wrap=(int32_t)(period*256u);
    float m=fmodf(u0,(float)period);
    if(m<0)m+=(float)period;
    *u0q=(uint16_t)((int32_t)lroundf(m*256.0f)%wrap);
    *duq=(uint16_t)(((int32_t)lroundf(du*256.0f)%wrap+wrap)%wrap);
    return true;
}
/* LINE_PATTERN from the pen to (x, y): its chords, two entries each. The
 * status to stop with, or RUNNING. */
static ksn_proc_status pattern_line(ksn_proc_vm *vm,const ksn_proc_inst *i,int16_t x,int16_t y){
    const float *r=&vm->reg[i->dst];
    int32_t bits,other;
    const unsigned n=(unsigned)i->value;
    if(!whole(r[0],0.0f,16777215.0f,&bits)||!whole(r[1],-1.0f,65535.0f,&other)||
       !(fabsf(r[2])<=1048576.0f)||!(fabsf(r[3])<=1048576.0f)||!isfinite(r[4])||!isfinite(r[5]))
        return KSN_PROC_INVALID;
    /* Depth: weights (1/Z) of one sign and unlike size lay the pattern as on
     * a line receding in space. */
    const float w0=fabsf(r[4]),w1=fabsf(r[5]);
    unsigned chords=1;
    if(r[4]*r[5]>0.0f){
        const float ratio=w0>w1?w0/w1:w1/w0;
        chords=ratio<1.02f?1u:ratio<1.2f?2u:ratio<1.6f?4u:8u;
    }
    const int16_t px=vm->pen_x,py=vm->pen_y;
    const uint16_t head=(uint16_t)(0x8000u|(uint32_t)bits>>16|n<<8|(other>=0?1u<<13:0u));
    int16_t ax=px,ay=py;
    float au=r[2];
    for(unsigned j=1;j<=chords;j++){
        int16_t bx=x,by=y;
        float bu=r[3];
        if(j<chords){
            const float t=(float)j/(float)chords,near=rounded(w0*(1.0f-t)),far=rounded(w1*t);
            bu=(rounded(r[2]*near)+rounded(r[3]*far))/(near+far);
            if(!coordinate((float)px+rounded((float)(x-px)*t),&bx)||
               !coordinate((float)py+rounded((float)(y-py)*t),&by))
                return KSN_PROC_INVALID;
        }
        const unsigned dx=(unsigned)abs(bx-ax),dy=(unsigned)abs(by-ay),steps=dx>dy?dx:dy;
        uint16_t u0q,duq;
        if(!phase(au,bu,steps,n,&u0q,&duq))return KSN_PROC_INVALID;
        ksn_proc_frame *f=vm->frame;
        if(f->count>KSN_PROC_SEGMENTS-2||f->raster_steps+steps+1>KSN_PROC_RASTER_STEPS)
            return KSN_PROC_LIMIT;
        f->raster_steps=(uint16_t)(f->raster_steps+steps+1);
        f->segments[f->count++]=(ksn_proc_segment){(int16_t)(ax+KSN_PROC_EXT_PATTERN),ay,bx,by,i->color};
        f->segments[f->count++]=(ksn_proc_segment){(int16_t)(uint16_t)bits,(int16_t)head,(int16_t)u0q,
            (int16_t)(0x8000u|duq),(uint16_t)(other>=0?other:0)};
        f->ext=1;
        ax=bx;ay=by;au=bu;
    }
    return KSN_PROC_RUNNING;
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
    case KSN_PROC_SIN:vm->reg[i->dst]=ksn_proc_sinf(vm->reg[i->a]);
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
    case KSN_PROC_LINE_PATTERN:
        if(!coordinate(vm->reg[i->a],&x)||!coordinate(vm->reg[i->b],&y))
            return vm->status=KSN_PROC_INVALID;
        if(vm->pen_valid){
            const ksn_proc_status s=pattern_line(vm,i,x,y);
            if(s!=KSN_PROC_RUNNING)return vm->status=s;
        }
        vm->pen_x=x;vm->pen_y=y;vm->pen_valid=true;break;
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
/* The Bresenham phase where line s (pen end x0) reaches band y..y+height-1
 * mid-way: x, yy, err moved there; returns the major-axis steps taken (the
 * pixel index), 0 when the line starts in the band. */
static inline __attribute__((always_inline)) int band_entry(const ksn_proc_segment *s,int x0,int y,int height,
                                                            int *x,int *yy,int *err){
    const int dx=abs(s->x1-x0),dy=abs(s->y1-s->y0),sx=x0<s->x1?1:-1,sy=s->y0<s->y1?1:-1;
    const int row=sy>0?y-s->y0:s->y0-(y+height-1);
    if(!dy||row<=0)return 0;
    int major,minor,x_steps,y_steps;
    /* int16 endpoints and a panel band bound row by 32768 + KSN_PROC_H.
     * The positive division numerators fit uint32_t, including rounding. */
    if(dx>=dy){
        uint32_t numerator=(uint32_t)row*dx-(dx-1)/2;
        major=numerator>0?(int)((numerator+dy-1)/(unsigned)dy):0;
        minor=(int)(((uint32_t)major*dy+(dx-1)/2)/(unsigned)dx);
        x_steps=major;y_steps=minor;
    }else{
        major=row;
        minor=(int)(((uint32_t)major*dx+(dy-1)/2)/(unsigned)dy);
        x_steps=minor;y_steps=major;
    }
    *x+=sx*x_steps;*yy+=sy*y_steps;
    *err=(int)((int64_t)dx-dy-(int64_t)x_steps*dy+(int64_t)y_steps*dx);
    return major;
}
/* A plain line in a frame with pattern lines: ksn_proc_render_band's walk. */
static inline __attribute__((always_inline)) void plain_band(const ksn_proc_segment *s,uint16_t *pixels,int y,int height){
    if(!height)return;
    int x=s->x0,yy=s->y0,dx=abs(s->x1-s->x0),dy=abs(s->y1-s->y0);
    int sx=x<s->x1?1:-1,sy=yy<s->y1?1:-1,err=dx-dy;
    band_entry(s,s->x0,y,height,&x,&yy,&err);
    for(;;){
        if(x>=0&&x<KSN_PROC_W)pixels[(yy-y)*KSN_PROC_W+x]=s->color;
        if(x==s->x1&&yy==s->y1)break;
        int twice=2*err;
        if(twice>-dy){err-=dy;x+=sx;}
        if(twice<dx){err+=dx;yy+=sy;
            if((sy>0&&yy>=y+height)||(sy<0&&yy<y))break;
        }
    }
}
/* One pixel of a pattern line: cell u >> 8 of the pattern. */
#define PATTERN_PIXEL(at) do{ if(bits>>(u>>8)&1u)(at)=ca; else if(has_b)(at)=cb; }while(0)
/* A pattern line's pixels inside the band: g its geometry, q its parameters.
 * The walk is the plain line's, with the pixel index k carried as u. A
 * horizontal line (a stand's row from the side) clips and runs left to
 * right; a slanted one keeps its row's address and tests only the major
 * axis's end (that axis moves every step). */
static void pattern_band(const ksn_proc_segment *g,const ksn_proc_segment *q,
                         uint16_t *pixels,int y,int height){
    const unsigned n=(uint16_t)q->y0>>8&31u;
    if(n<1||n>KSN_PROC_PATTERN_BITS)return;
    const uint32_t wrap=n*256u,u0=(uint16_t)q->x1%wrap,du=((uint16_t)q->y1&0x7fffu)%wrap;
    const uint32_t bits=(uint16_t)q->x0|(uint32_t)((uint16_t)q->y0&0xffu)<<16;
    const bool has_b=(uint16_t)q->y0>>13&1u;
    const uint16_t ca=g->color,cb=q->color;
    const int x0=g->x0-KSN_PROC_EXT_PATTERN;
    if(g->y0==g->y1){
        const bool forward=x0<=g->x1;
        int left=forward?x0:g->x1,right=forward?g->x1:x0;
        if(left<0)left=0;
        if(right>=KSN_PROC_W)right=KSN_PROC_W-1;
        if(left>right)return;
        /* k counts from the pen, whichever side that is; left to right is
         * backwards (wrap - du) when the pen is on the right. */
        const uint32_t k=(uint32_t)(forward?left-x0:x0-left);
        const uint32_t step=forward?du:(wrap-du)%wrap;
        /* 32 bits hold it: u0 and du are under 2^13, k at most 1,200. */
        uint32_t u=(u0+k*du)%wrap;
        uint16_t *row=pixels+(g->y0-y)*KSN_PROC_W;
        for(int x=left;x<=right;x++){
            PATTERN_PIXEL(row[x]);
            u+=step;if(u>=wrap)u-=wrap;
        }
        return;
    }
    const int dx=abs(g->x1-x0),dy=abs(g->y1-g->y0),sx=x0<g->x1?1:-1,sy=g->y0<g->y1?1:-1;
    int x=x0,yy=g->y0,err=dx-dy;
    const uint32_t k=(uint32_t)band_entry(g,x0,y,height,&x,&yy,&err);
    /* The range test let the line in, so its entry point is in the band. */
    uint32_t u=(u0+k*du)%wrap;
    uint16_t *row=pixels+(yy-y)*KSN_PROC_W;
    const int stride=sy*KSN_PROC_W;
    if(dx>=dy){
        for(;;){
            if((unsigned)x<(unsigned)KSN_PROC_W)PATTERN_PIXEL(row[x]);
            if(x==g->x1)break;
            const int twice=2*err;
            if(twice>-dy){err-=dy;x+=sx;}
            if(twice<dx){err+=dx;yy+=sy;if(yy<y||yy>=y+height)break;row+=stride;}
            u+=du;if(u>=wrap)u-=wrap;
        }
    }else{
        for(;;){
            if((unsigned)x<(unsigned)KSN_PROC_W)PATTERN_PIXEL(row[x]);
            if(yy==g->y1)break;
            const int twice=2*err;
            if(twice>-dy){err-=dy;x+=sx;}
            if(twice<dx){err+=dx;yy+=sy;}
            if(yy<y||yy>=y+height)break;
            row+=stride;
            u+=du;if(u>=wrap)u-=wrap;
        }
    }
}
#undef PATTERN_PIXEL
static bool ell_finite(const float *p,unsigned n,float limit){
    for(unsigned i=0;i<n;i++)if(!isfinite(p[i])||fabsf(p[i])>limit)return false;
    return true;
}
static bool ell_unit(float v[3]){
    const float s=fmaxf(fabsf(v[0]),fmaxf(fabsf(v[1]),fabsf(v[2])));
    if(!(s>0))return false;
    v[0]/=s;v[1]/=s;v[2]/=s;
    const float d=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    for(unsigned i=0;i<3;i++)v[i]/=d;
    return true;
}
bool ksn_proc_ellipsoid_bounds(const ksn_proc_ellipsoid *p,int *left,int *top,int *right,int *bottom){
    if(!p||!ell_finite(p->screen,8,4096)||!ell_finite(p->normal,9,1e6f)||
       !ell_finite(p->light,9,1)||!ell_finite(p->material,8,128)||
       !ell_finite(p->texture,9,4096)||p->surface[2]>255||p->surface[3]>255||p->surface[4]>4||p->reserved)return false;
    if(p->surface[4]==4){
        const float *t=p->texture;
        const double n2=(double)t[0]*t[0]+(double)t[1]*t[1]+(double)t[2]*t[2];
        if(fabs(n2-1)>1e-4||fabsf(t[3])>2||t[4]<0||t[4]>2||
           (t[4]>0&&t[4]<1e-4f)||t[5]!=0||t[7]!=0||t[8]!=1)return false;
    }
    const float *a=p->screen,*b=p->screen+4,*n=p->normal;
    const float aa=a[0]*a[0]+a[1]*a[1]+a[2]*a[2],bb=b[0]*b[0]+b[1]*b[1]+b[2]*b[2],
        ab=a[0]*b[0]+a[1]*b[1]+a[2]*b[2],det=aa*bb-ab*ab;
    const float nd=n[0]*(n[4]*n[8]-n[5]*n[7])-n[1]*(n[3]*n[8]-n[5]*n[6])+n[2]*(n[3]*n[7]-n[4]*n[6]);
    if(!(aa>0&&bb>0&&det>aa*bb*1e-6f)||!isfinite(nd)||nd==0)return false;
    float l[3]={p->light[0],p->light[1],p->light[2]},v[3]={p->light[3],p->light[4],p->light[5]};
    if(!ell_unit(l)||!ell_unit(v))return false;
    for(unsigned i=6;i<9;i++)if(p->light[i]<0)return false;
    for(unsigned i=0;i<8;i++)if(i==3?(p->material[i]<1||p->material[i]>128):
        (p->material[i]<0||p->material[i]>1))return false;
    if(p->texture[6]<0||p->texture[6]>1||p->texture[7]<0||p->texture[8]>1||p->texture[7]>p->texture[8])return false;
    const float rx=sqrtf(aa),ry=sqrtf(bb);
    *left=(int)ceilf(a[3]-rx);*right=(int)floorf(a[3]+rx);
    *top=(int)ceilf(b[3]-ry);*bottom=(int)floorf(b[3]+ry);
    if(*left<0)*left=0;
    if(*top<0)*top=0;
    if(*right>=KSN_PROC_W)*right=KSN_PROC_W-1;
    if(*bottom>=KSN_PROC_H)*bottom=KSN_PROC_H-1;
    if(!p->surface[2]){*left=1;*right=0;}
    return true;
}
static bool clip_prepare(const ksn_proc_ellipsoid_clip *p,int *winding){
    if(p->reserved[0]||p->reserved[1]||p->reserved[2]||
       (p->count!=0&&(p->count<3||p->count>KSN_PROC_CLIP_VERTICES)))return false;
    for(unsigned k=0;k<2u*p->count;k++)if(!isfinite(p->xy[k])||fabsf(p->xy[k])>4096)return false;
    for(unsigned k=2u*p->count;k<16;k++){uint32_t bits;memcpy(&bits,p->xy+k,4);if(bits)return false;}
    if(!p->count){*winding=0;return true;}
    int orientation=0;
    for(unsigned i=0;i<p->count;i++){
        const unsigned next=(i+1u)%p->count;
        const double ax=p->xy[2*i],ay=p->xy[2*i+1],dx=(double)p->xy[2*next]-ax,dy=(double)p->xy[2*next+1]-ay;
        if(dx==0&&dy==0)return false;
        /* Checking all vertices, not just adjacent turns, rejects stars. */
        for(unsigned j=0;j<p->count;j++)if(j!=i&&j!=next){
            const double cross=dx*((double)p->xy[2*j+1]-ay)-dy*((double)p->xy[2*j]-ax);
            const int sign=(cross>0)-(cross<0);
            if(!sign||(orientation&&sign!=orientation))return false;
            orientation=sign;
        }
    }
    *winding=orientation;return true;
}
static bool clip_contains(const ksn_proc_ellipsoid_clip *p,int winding,int x,int y){
    for(unsigned i=0;i<p->count;i++){
        const unsigned next=(i+1u)%p->count;
        const double ax=p->xy[2*i],ay=p->xy[2*i+1],dx=(double)p->xy[2*next]-ax,dy=(double)p->xy[2*next+1]-ay;
        if(winding*(dx*((double)y-ay)-dy*((double)x-ax))<0)return false;
    }
    return p->count!=0;
}
/* Closed integer pixel centres. Double intersection work is per row, not
 * per shaded pixel; endpoint halfplanes repair division rounding at ties. */
static bool clip_span(const ksn_proc_ellipsoid_clip *p,int winding,int y,int *left,int *right){
    if(!p->count)return false;
    double lo=*left,hi=*right;
    for(unsigned i=0;i<p->count;i++){
        const unsigned next=(i+1u)%p->count;
        const double ax=p->xy[2*i],ay=p->xy[2*i+1],dx=(double)p->xy[2*next]-ax,dy=(double)p->xy[2*next+1]-ay;
        if(dy==0){if(winding*dx*((double)y-ay)<0)return false;continue;}
        const double intersection=ax+((double)y-ay)*dx/dy;
        if(winding*dy>0)hi=fmin(hi,intersection);else lo=fmax(lo,intersection);
    }
    const double first=fmax(*left,ceil(lo)-1),last=fmin(*right,floor(hi)+1);
    if(first>last||first>*right||last<*left)return false;
    int a=(int)first,b=(int)last;
    while(a<=b&&!clip_contains(p,winding,a,y))++a;
    while(b>=a&&!clip_contains(p,winding,b,y))--b;
    if(a>b)return false;
    *left=a;*right=b;return true;
}
bool ksn_proc_clipped_ellipsoid_read(const ksn_proc_segment *data,unsigned words,ksn_proc_clipped_ellipsoid *out){
    if(!data||!out||words<KSN_PROC_CLIPPED_WORDS)return false;
    const uint8_t *raw=(const uint8_t *)data;
    for(size_t k=sizeof *out;k<KSN_PROC_CLIPPED_WORDS*sizeof *data;k++)if(raw[k])return false;
    memcpy(out,data,sizeof *out);return true;
}
bool ksn_proc_clipped_ellipsoid_bounds(const ksn_proc_clipped_ellipsoid *p,int *left,int *top,int *right,int *bottom){
    int l,t,r,b,winding;
    /* EMPTY never hides invalid analytic geometry. */
    if(!p||!ksn_proc_ellipsoid_bounds(&p->patch,&l,&t,&r,&b)||!clip_prepare(&p->clip,&winding))return false;
    if(!p->clip.count){l=1;r=0;}
    else{
        float x0=p->clip.xy[0],x1=x0,y0=p->clip.xy[1],y1=y0;
        for(unsigned k=1;k<p->clip.count;k++){
            x0=fminf(x0,p->clip.xy[2*k]);x1=fmaxf(x1,p->clip.xy[2*k]);
            y0=fminf(y0,p->clip.xy[2*k+1]);y1=fmaxf(y1,p->clip.xy[2*k+1]);
        }
        if(ceilf(x0)>l)l=(int)ceilf(x0);
        if(floorf(x1)<r)r=(int)floorf(x1);
        if(ceilf(y0)>t)t=(int)ceilf(y0);
        if(floorf(y1)<b)b=(int)floorf(y1);
    }
    *left=l;*top=t;*right=r;*bottom=b;return true;
}
bool ksn_proc_blend_read(const ksn_proc_segment *data,unsigned words,ksn_proc_blend_ellipsoids *p,unsigned *used){
    if(!data||!p||!used||words*sizeof *data<sizeof p->header)return false;
    memcpy(&p->header,data,sizeof p->header);
    if(!p->header.count||p->header.count>KSN_PROC_BLEND_LOBES)return false;
    *used=(unsigned)KSN_PROC_BLEND_WORDS(p->header.count);
    if(*used>words)return false;
    memcpy(p,data,KSN_PROC_BLEND_BYTES(p->header.count));
    return true;
}
bool ksn_proc_blend_region_read(const ksn_proc_segment *data,unsigned words,
        ksn_proc_blend_ellipsoids *p,ksn_proc_blend_region_mask *mask,unsigned *used){
    if(!data||!p||!mask||!used||words*sizeof *data<sizeof p->header)return false;
    memcpy(&p->header,data,sizeof p->header);
    if(!p->header.count||p->header.count>KSN_PROC_BLEND_LOBES)return false;
    const unsigned bytes=(unsigned)KSN_PROC_BLEND_BYTES(p->header.count),
        needed=(unsigned)KSN_PROC_BLEND_REGION_WORDS(p->header.count);
    if(needed>words)return false;
    memcpy(p,data,bytes);memcpy(mask,(const uint8_t *)data+bytes,sizeof *mask);
    *used=needed;return true;
}
bool ksn_proc_blend_region_bounds(const ksn_proc_blend_ellipsoids *p,const ksn_proc_blend_region_mask *mask,
        int *left,int *top,int *right,int *bottom){
    if(!mask||mask->reserved||!ell_finite(mask->plane,4,1e6f)||
       !(mask->plane[0]!=0||mask->plane[1]!=0||mask->plane[2]!=0)||!isfinite(mask->feather)||
       !(mask->feather==0||(mask->feather>=1e-4f&&mask->feather<=4096)))return false;
    return ksn_proc_blend_bounds(p,left,top,right,bottom);
}
bool ksn_proc_blend_bounds(const ksn_proc_blend_ellipsoids *p,int *left,int *top,int *right,int *bottom){
    if(!p||!p->header.count||p->header.count>KSN_PROC_BLEND_LOBES||p->header.reserved||
       p->header.surface[1]>255||p->header.surface[2]>255||
       !ell_finite(p->header.material,11,128)||!ell_finite(p->header.controls,2,128)||
       p->header.controls[0]<0||p->header.controls[1]<0||p->header.controls[1]>1)return false;
    for(unsigned i=8;i<11;i++)if(p->header.material[i]<0||p->header.material[i]>1)return false;
    ksn_proc_ellipsoid q={.texture={1,0,0,0,1,0,0,0,1},.surface={0,0,255,255,0}};
    memcpy(q.light,p->header.light,sizeof q.light);memcpy(q.material,p->header.material,sizeof q.material);
    *left=KSN_PROC_W;*top=KSN_PROC_H;*right=-1;*bottom=-1;
    for(unsigned i=0;i<p->header.count;i++){
        const ksn_proc_blend_lobe *l=&p->lobes[i];
        if(!ell_finite(l->depth,4,4096))return false;
        memcpy(q.screen,l->screen,sizeof q.screen);memcpy(q.normal,l->normal,sizeof q.normal);
        int x0,y0,x1,y1;if(!ksn_proc_ellipsoid_bounds(&q,&x0,&y0,&x1,&y1))return false;
        const float *a=l->screen,*b=a+4,*d=l->depth;
        const float aa=a[0]*a[0]+a[1]*a[1]+a[2]*a[2],bb=b[0]*b[0]+b[1]*b[1]+b[2]*b[2],
            ab=a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
        if(!isfinite(1/(aa*bb-ab*ab)))return false;
        const float volume=(a[1]*b[2]-a[2]*b[1])*d[0]+(a[2]*b[0]-a[0]*b[2])*d[1]+(a[0]*b[1]-a[1]*b[0])*d[2];
        if(!isfinite(volume)||volume==0)return false;
        if(x0>x1||y0>y1)continue;
        if(x0<*left)*left=x0;
        if(y0<*top)*top=y0;
        if(x1>*right)*right=x1;
        if(y1>*bottom)*bottom=y1;
    }
    if(!p->header.surface[1]){*left=1;*right=0;}
    return true;
}
static unsigned ell_colour(float base,float light,float tint,float front,float spec,float back,float thin,
                           const float *m,unsigned max){
    float c=base*(m[0]+m[1]*front*light)+light*(m[2]*spec+m[4]*thin*back*tint);
    c=fmaxf(0,fminf(1,c));
    return (unsigned)lroundf(c*max);
}
/* A unit sphere projected by two affine rows. Reconstruct its front surface
 * analytically, so geometry storage does not grow with pixel resolution. */
static void ellipsoid_band(const ksn_proc_ellipsoid *p,const ksn_proc_ellipsoid_clip *clip,int winding,
                           uint16_t *pixels,int y,int height){
    int left,top,right,bottom;
    if(!ksn_proc_ellipsoid_bounds(p,&left,&top,&right,&bottom))return;
    if(top<y)top=y;
    if(bottom>=y+height)bottom=y+height-1;
    if(left>right||top>bottom)return;
    const float *a=p->screen,*b=p->screen+4,*nm=p->normal,*m=p->material,*uv=p->texture;
    const float aa=a[0]*a[0]+a[1]*a[1]+a[2]*a[2],bb=b[0]*b[0]+b[1]*b[1]+b[2]*b[2],
        ab=a[0]*b[0]+a[1]*b[1]+a[2]*b[2],inv=1/(aa*bb-ab*ab);
    float axis[3]={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
    (void)ell_unit(axis);
    float l[3]={p->light[0],p->light[1],p->light[2]},v[3]={p->light[3],p->light[4],p->light[5]};
    (void)ell_unit(l);(void)ell_unit(v);
    float sign=0;
    for(unsigned k=0;k<3;k++)sign+=(nm[3*k]*axis[0]+nm[3*k+1]*axis[1]+nm[3*k+2]*axis[2])*v[k];
    if(sign<0)for(unsigned k=0;k<3;k++)axis[k]=-axis[k];
    float half[3]={l[0]+v[0],l[1]+v[1],l[2]+v[2]};const bool has_half=ell_unit(half);
    const float edge=fmaxf(uv[6],2/fminf(sqrtf(aa),sqrtf(bb)));
    const float region_inverse=p->surface[4]==4&&uv[4]>0?1/uv[4]:0;
    for(int yy=top;yy<=bottom;yy++){
      int row_left=left,row_right=right;
      if(clip&&!clip_span(clip,winding,yy,&row_left,&row_right))continue;
      for(int x=row_left;x<=row_right;x++){
        const float dx=x-a[3],dy=yy-b[3],u=(bb*dx-ab*dy)*inv,w=(aa*dy-ab*dx)*inv;
        float q[3]={a[0]*u+b[0]*w,a[1]*u+b[1]*w,a[2]*u+b[2]*w};
        const float residual=1-q[0]*q[0]-q[1]*q[1]-q[2]*q[2];
        if(!(residual>0))continue;
        const float chord=sqrtf(residual);
        for(unsigned k=0;k<3;k++)q[k]+=axis[k]*chord;
        float n[3];
        for(unsigned k=0;k<3;k++)n[k]=nm[3*k]*q[0]+nm[3*k+1]*q[1]+nm[3*k+2]*q[2];
        if(!ell_unit(n))continue;
        uint16_t base=p->surface[0];
        float region_mix=0;
        if(p->surface[4]==4){
            /* Body-local albedo only: do not derive markings from lighting or normals. */
            const float distance=uv[0]*q[0]+uv[1]*q[1]+uv[2]*q[2]-uv[3];
            region_mix=uv[4]>0?fmaxf(0,fminf(1,.5f+distance*region_inverse)):(distance>=0?1:0);
            if(region_mix>=1)base=p->surface[1];
        }else if(p->surface[4]){
            const float tu=atan2f(q[0],q[2])*(1.0f/6.283185307179586f)+0.5f,tv=q[1]*0.5f+0.5f;
            const float U=uv[0]*tu+uv[1]*tv+uv[2],V=uv[3]*tu+uv[4]*tv+uv[5];
            bool second;
            if(p->surface[4]==3)second=((int)floorf(U)+(int)floorf(V))&1;
            else{const float t=p->surface[4]==1?U:V,f=t-floorf(t);second=f>=uv[7]&&f<=uv[8];}
            if(second)base=p->surface[1];
        }
        const float dot=n[0]*l[0]+n[1]*l[1]+n[2]*l[2],front=fmaxf(0,dot),back=fmaxf(0,-dot),
            nh=has_half?fmaxf(0,n[0]*half[0]+n[1]*half[1]+n[2]*half[2]):0,
            spec=front>0&&nh>0?powf(nh,m[3]):0,thin=1-p->surface[3]*(1.0f/255)*chord;
        float red=((base>>11)&31)*(1.0f/31),green=((base>>5)&63)*(1.0f/63),blue_base=(base&31)*(1.0f/31);
        if(p->surface[4]==4&&region_mix>0&&region_mix<1){
            const uint16_t second=p->surface[1];
            red+=((((second>>11)&31)*(1.0f/31))-red)*region_mix;
            green+=((((second>>5)&63)*(1.0f/63))-green)*region_mix;
            blue_base+=(((second&31)*(1.0f/31))-blue_base)*region_mix;
        }
        unsigned r=ell_colour(red,p->light[6],m[5],front,spec,back,thin,m,31),
            gg=ell_colour(green,p->light[7],m[6],front,spec,back,thin,m,63),
            blue=ell_colour(blue_base,p->light[8],m[7],front,spec,back,thin,m,31);
        const unsigned alpha=(unsigned)lroundf(p->surface[2]*fminf(1,residual/edge)),ia=255-alpha;
        uint16_t *dest=pixels+(yy-y)*KSN_PROC_W+x;
        r=(r*alpha+((*dest>>11)&31)*ia+127)/255;
        gg=(gg*alpha+((*dest>>5)&63)*ia+127)/255;
        blue=(blue*alpha+(*dest&31)*ia+127)/255;
        *dest=(uint16_t)((r<<11)|(gg<<5)|blue);
      }
    }
}
typedef struct {
    float aa,bb,ab,inv,axis[3],edge;
    int left,top,right,bottom;
} blend_projection;
/* Blend analytic front heights, not independently composited sphere colours.
 * This has bounded work and no depth buffer. The projected silhouette remains
 * the union of the lobes. Support-regularized shading normals suppress grazing
 * cap seams; they approximate, rather than differentiate, the blended height. */
static void blend_ellipsoids_band(const ksn_proc_blend_ellipsoids *p,const ksn_proc_blend_region_mask *region,
        uint16_t *pixels,int y,int height){
    int left,top,right,bottom;if(!ksn_proc_blend_bounds(p,&left,&top,&right,&bottom))return;
    if(top<y)top=y;
    if(bottom>=y+height)bottom=y+height-1;
    if(left>right||top>bottom)return;
    const ksn_proc_blend_header *h=&p->header;
    const float inverse_feather=region&&region->feather>0?1/region->feather:0;
    const float *m=h->material;
    float light[3]={h->light[0],h->light[1],h->light[2]},view[3]={h->light[3],h->light[4],h->light[5]};
    (void)ell_unit(light);(void)ell_unit(view);
    float half[3]={light[0]+view[0],light[1]+view[1],light[2]+view[2]};const bool has_half=ell_unit(half);
    blend_projection projection[KSN_PROC_BLEND_LOBES];
    for(unsigned i=0;i<h->count;i++){
        const float *a=p->lobes[i].screen,*b=a+4,*d=p->lobes[i].depth;
        blend_projection *s=&projection[i];
        s->aa=a[0]*a[0]+a[1]*a[1]+a[2]*a[2];s->bb=b[0]*b[0]+b[1]*b[1]+b[2]*b[2];
        s->ab=a[0]*b[0]+a[1]*b[1]+a[2]*b[2];s->inv=1/(s->aa*s->bb-s->ab*s->ab);
        s->axis[0]=a[1]*b[2]-a[2]*b[1];s->axis[1]=a[2]*b[0]-a[0]*b[2];s->axis[2]=a[0]*b[1]-a[1]*b[0];
        (void)ell_unit(s->axis);
        if(d[0]*s->axis[0]+d[1]*s->axis[1]+d[2]*s->axis[2]<0)
            for(unsigned k=0;k<3;k++)s->axis[k]=-s->axis[k];
        const float rx=sqrtf(s->aa),ry=sqrtf(s->bb);
        s->left=(int)ceilf(a[3]-rx);s->right=(int)floorf(a[3]+rx);
        s->top=(int)ceilf(b[3]-ry);s->bottom=(int)floorf(b[3]+ry);
        s->edge=fmaxf(h->controls[1],2/fminf(rx,ry));
    }
    for(int yy=top;yy<=bottom;yy++)for(int x=left;x<=right;x++){
        bool found=false;float depth=0,normal[3]={0},chord=0,mass=0,coverage=0;
        for(unsigned i=0;i<h->count;i++){
            const blend_projection *s=&projection[i];
            if(x<s->left||x>s->right||yy<s->top||yy>s->bottom)continue;
            const ksn_proc_blend_lobe *l=&p->lobes[i];const float *a=l->screen,*b=a+4;
            const float dx=x-a[3],dy=yy-b[3],u=(s->bb*dx-s->ab*dy)*s->inv,w=(s->aa*dy-s->ab*dx)*s->inv;
            float q[3]={a[0]*u+b[0]*w,a[1]*u+b[1]*w,a[2]*u+b[2]*w};
            const float residual=1-q[0]*q[0]-q[1]*q[1]-q[2]*q[2];
            if(!(residual>0))continue;
            const float support=h->controls[0]>0&&h->count>1?residual:1;
            const float z=sqrtf(residual);
            for(unsigned k=0;k<3;k++)q[k]+=s->axis[k]*z;
            float n[3];
            for(unsigned k=0;k<3;k++)n[k]=l->normal[3*k]*q[0]+l->normal[3*k+1]*q[1]+l->normal[3*k+2]*q[2];
            /* A grazing cap has an infinite height slope. Bound that slope
             * before interpolation so malformed/extreme inputs stay finite. */
            const float scale=fmaxf(fabsf(n[0]),fmaxf(fabsf(n[1]),fabsf(n[2])));
            if(!(scale>0))continue;
            for(unsigned k=0;k<3;k++)n[k]/=scale;
            float nv=n[0]*view[0]+n[1]*view[1]+n[2]*view[2];
            if(nv<0){nv=-nv;for(unsigned k=0;k<3;k++)n[k]=-n[k];}
            nv=fmaxf(nv,1e-4f);
            for(unsigned k=0;k<3;k++)n[k]/=nv;
            /* A cap enters with finite height weight but unbounded slope.
             * Vanishing support removes its rim from interior shading without
             * changing coverage or depth. Hard/single surfaces retain normals. */
            if(support!=1)for(unsigned k=0;k<3;k++)n[k]*=support;
            const float d=l->depth[0]*q[0]+l->depth[1]*q[1]+l->depth[2]*q[2]+l->depth[3];
            coverage=fmaxf(coverage,fminf(1,residual/s->edge));
            if(!found){depth=d;memcpy(normal,n,sizeof normal);chord=z*support;mass=support;found=true;continue;}
            const float blend=h->controls[0];
            const float weight=blend>0?fmaxf(0,fminf(1,0.5f+0.5f*(d-depth)/blend)):(d>depth?1:0);
            depth=depth*(1-weight)+d*weight+blend*weight*(1-weight);
            for(unsigned k=0;k<3;k++)normal[k]=normal[k]*(1-weight)+n[k]*weight;
            chord=chord*(1-weight)+z*support*weight;
            mass=mass*(1-weight)+support*weight;
        }
        if(!found||!ell_unit(normal))continue;
        chord/=mass;
        const float dot=normal[0]*light[0]+normal[1]*light[1]+normal[2]*light[2],front=fmaxf(0,dot),back=fmaxf(0,-dot),
            nh=has_half?fmaxf(0,normal[0]*half[0]+normal[1]*half[1]+normal[2]*half[2]):0,
            spec=front>0&&nh>0&&m[2]>0?powf(nh,m[3]):0,thin=1-h->surface[2]*(1.0f/255)*chord;
        const uint16_t base=h->surface[0];unsigned color[3];
        float channels[3]={(base>>11)&31,(base>>5)&63,base&31};
        const unsigned max[3]={31,63,31};
        if(region&&base!=region->color){
            const float *r=region->plane,distance=r[0]*x+r[1]*yy+r[2]*depth+r[3],
                mix=inverse_feather>0?fmaxf(0,fminf(1,.5f+distance*inverse_feather)):(distance>=0?1:0);
            const uint16_t other=region->color;
            const unsigned b[3]={(other>>11)&31,(other>>5)&63,other&31};
            for(unsigned k=0;k<3;k++){
                if(mix>=1)channels[k]=(float)b[k];
                else if(mix>0)channels[k]+=((float)b[k]-channels[k])*mix;
            }
        }
        for(unsigned k=0;k<3;k++){
            const float c=channels[k]*(1.0f/max[k])*(m[0]*m[8+k]+m[1]*front*h->light[6+k])+
                h->light[6+k]*(m[2]*spec+m[4]*thin*back*m[5+k]);
            color[k]=(unsigned)lroundf(fmaxf(0,fminf(1,c))*max[k]);
        }
        const unsigned alpha=(unsigned)lroundf(h->surface[1]*coverage),ia=255-alpha;
        uint16_t *dest=pixels+(yy-y)*KSN_PROC_W+x;
        color[0]=(color[0]*alpha+((*dest>>11)&31)*ia+127)/255;
        color[1]=(color[1]*alpha+((*dest>>5)&63)*ia+127)/255;
        color[2]=(color[2]*alpha+(*dest&31)*ia+127)/255;
        *dest=(uint16_t)((color[0]<<11)|(color[1]<<5)|color[2]);
    }
}
/* A frame with pattern lines: the same order, the same plain lines. A
 * parameter entry's negative rows fail the range test; the bias is looked at
 * only for an entry that reached the band. Out of line, so a frame without
 * pattern lines runs the loop below as it always did. */
static __attribute__((noinline)) void render_band_ext(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height){
    for(unsigned j=0;j<frame->count;j++){
        const ksn_proc_segment *s=&frame->segments[j];
        if(s->x0>=KSN_PROC_EXT_BLEND_REGION-480){
            ksn_proc_blend_ellipsoids p;ksn_proc_blend_region_mask mask;unsigned used;
            if(!ksn_proc_blend_region_read(s+1,frame->count-j-1,&p,&mask,&used))return;
            int l,t,r,b;if(!ksn_proc_blend_region_bounds(&p,&mask,&l,&t,&r,&b))return;
            if(s->y1>=y&&s->y0<y+height)blend_ellipsoids_band(&p,&mask,pixels,y,height);
            j+=used;continue;
        }
        if(s->x0>=KSN_PROC_EXT_CLIPPED_ELLIPSOID-480){
            ksn_proc_clipped_ellipsoid p;int winding;
            if(!ksn_proc_clipped_ellipsoid_read(s+1,frame->count-j-1,&p)||!clip_prepare(&p.clip,&winding))return;
            if(s->y1>=y&&s->y0<y+height)ellipsoid_band(&p.patch,&p.clip,winding,pixels,y,height);
            j+=(unsigned)KSN_PROC_CLIPPED_WORDS;continue;
        }
        if(s->x0>=KSN_PROC_EXT_BLEND_ELLIPSOIDS-480){
            ksn_proc_blend_ellipsoids p;unsigned used;
            if(!ksn_proc_blend_read(s+1,frame->count-j-1,&p,&used))return;
            if(s->y1>=y&&s->y0<y+height)blend_ellipsoids_band(&p,NULL,pixels,y,height);
            j+=used;continue;
        }
        if(s->x0>=KSN_PROC_EXT_ELLIPSOID-480){
            if(KSN_PROC_ELLIPSOID_WORDS>frame->count-j-1)return;
            ksn_proc_ellipsoid p;memcpy(&p,s+1,sizeof p);
            ellipsoid_band(&p,NULL,0,pixels,y,height);
            j+=(unsigned)KSN_PROC_ELLIPSOID_WORDS;continue;
        }
        int ymin=s->y0<s->y1?s->y0:s->y1;
        int ymax=s->y0>s->y1?s->y0:s->y1;
        if(s->x0>=KSN_PROC_EXT_MIN){
            /* Consume the pair even outside this band. Its parameter x0 is
             * pattern bits, which may otherwise look like a packet marker. */
            if(j+1<frame->count&&ymax>=y&&ymin<y+height)
                pattern_band(s,s+1,pixels,y,height);
            j++;
        }else if(ymax>=y&&ymin<y+height)plain_band(s,pixels,y,height);
    }
}
bool ksn_proc_render_band(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height){
    if(!frame||!frame->ready||!pixels||y<0||height<0||y>KSN_PROC_H||height>KSN_PROC_H-y)return false;
    if(frame->ext){render_band_ext(frame,pixels,y,height);return true;}
    for(unsigned j=0;j<frame->count;j++){
        const ksn_proc_segment *s=&frame->segments[j];
        int ymin=s->y0<s->y1?s->y0:s->y1;
        int ymax=s->y0>s->y1?s->y0:s->y1;
        if(ymax<y||ymin>=y+height)continue;
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
            if(x>=0&&x<KSN_PROC_W&&yy>=y&&yy<y+height)pixels[(yy-y)*KSN_PROC_W+x]=s->color;
            if(x==s->x1&&yy==s->y1)break;
            int twice=2*err;
            if(twice>-dy){err-=dy;x+=sx;}
            if(twice<dx){err+=dx;yy+=sy;}
        }
    }
    return true;
}
#ifdef KASANE_BGCOST_TRACE
bool ksn_proc_render_band_base(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height){
    if(!frame||!frame->ready||!pixels||y<0||height<0||y>KSN_PROC_H||height>KSN_PROC_H-y)return false;
    for(unsigned j=0;j<frame->count;j++){
        const ksn_proc_segment *s=&frame->segments[j];
        int ymin=s->y0<s->y1?s->y0:s->y1;
        int ymax=s->y0>s->y1?s->y0:s->y1;
        if(ymax<y||ymin>=y+height)continue;
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
            if(x>=0&&x<KSN_PROC_W&&yy>=y&&yy<y+height)pixels[(yy-y)*KSN_PROC_W+x]=s->color;
            if(x==s->x1&&yy==s->y1)break;
            int twice=2*err;
            if(twice>-dy){err-=dy;x+=sx;}
            if(twice<dx){err+=dx;yy+=sy;}
        }
    }
    return true;
}
#endif
