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
    *x+=sx*x_steps;*yy+=sy*y_steps;
    *err=(int)((int64_t)dx-dy-(int64_t)x_steps*dy+(int64_t)y_steps*dx);
    return major;
}
/* A plain line in a frame with pattern lines: ksn_proc_render_band's walk. */
static inline __attribute__((always_inline)) void plain_band(const ksn_proc_segment *s,uint16_t *pixels,int y,int height){
    int x=s->x0,yy=s->y0,dx=abs(s->x1-s->x0),dy=abs(s->y1-s->y0);
    int sx=x<s->x1?1:-1,sy=yy<s->y1?1:-1,err=dx-dy;
    band_entry(s,s->x0,y,height,&x,&yy,&err);
    for(;;){
        if((sy>0&&yy>=y+height)||(sy<0&&yy<y))break;
        if(x>=0&&x<KSN_PROC_W&&yy>=y&&yy<y+height)pixels[(yy-y)*KSN_PROC_W+x]=s->color;
        if(x==s->x1&&yy==s->y1)break;
        int twice=2*err;
        if(twice>-dy){err-=dy;x+=sx;}
        if(twice<dx){err+=dx;yy+=sy;}
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
/* A frame with pattern lines: the same order, the same plain lines. A
 * parameter entry's negative rows fail the range test; the bias is looked at
 * only for an entry that reached the band. Out of line, so a frame without
 * pattern lines runs the loop below as it always did. */
static __attribute__((noinline)) void render_band_ext(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height){
    for(unsigned j=0;j<frame->count;j++){
        const ksn_proc_segment *s=&frame->segments[j];
        int ymin=s->y0<s->y1?s->y0:s->y1;
        int ymax=s->y0>s->y1?s->y0:s->y1;
        if(ymax<y||ymin>=y+height)continue;
        if(s->x0>=KSN_PROC_EXT_MIN){
            if(j+1<frame->count)pattern_band(s,s+1,pixels,y,height);
            j++;
        }else plain_band(s,pixels,y,height);
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
