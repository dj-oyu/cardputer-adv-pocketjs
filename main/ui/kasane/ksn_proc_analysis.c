#include "ksn_proc_analysis.h"
#include <math.h>
#include <string.h>

/* 32-bit arithmetic before narrowing: 1u<<16 is defined, and each cast names
 * the mask type so no set can be truncated by an 8-bit leftover. */
#define ALL_REGS ((ksn_pa_regs)(((uint32_t)1 << KSN_PROC_REGS) - 1u))
#define BIT(r) ((ksn_pa_regs)((uint32_t)1 << (r)))
/* CUBIC reads exactly the eight control-point registers from dst (always 0).
 * Naming them keeps r8..r15 dead across a curve instead of the former
 * ALL_REGS, which was exact only while the file had eight registers. */
#define CUBIC_REGS(dst) ((ksn_pa_regs)((uint32_t)0xffu << (dst)))
_Static_assert(KSN_PROC_REGS >= 8, "CUBIC needs r0..r7");

static bool reg_ok(uint8_t r){ return r < KSN_PROC_REGS; }
static void edge(ksn_pa_inst *i, uint8_t to){
    if(i->successor_count < 2) i->successor[i->successor_count++] = to;
}
static bool pure_total(const ksn_pa_inst *i){
    return i->effects == KSN_PA_EFFECT_REG && i->failure == KSN_PA_FAIL_NONE;
}
static bool independent_pair(const ksn_pa_inst *a,const ksn_pa_inst *b){
    return !((a->writes & (b->reads | b->writes)) || (b->writes & a->reads));
}

bool ksn_proc_analyze(const ksn_proc_program *p, ksn_proc_analysis *out){
    uint8_t open[KSN_PROC_LOOP_DEPTH], matching[KSN_PROC_CODE];
    uint8_t enclosing[KSN_PROC_CODE], depth = 0;
    bool leader[KSN_PROC_CODE + 1] = {false};
    if(!out)return false;
    memset(out,0,sizeof *out);
    out->program_reasons=KSN_PA_INVALID;
    if(!p || !p->code || !p->count || p->count > KSN_PROC_CODE)return false;
    memset(matching,0xff,sizeof matching);
    memset(enclosing,0xff,sizeof enclosing);
    out->count=p->count;
    for(uint8_t pc=0;pc<p->count;pc++){
        const ksn_proc_inst *i=&p->code[pc];
        ksn_pa_inst *a=&out->inst[pc];
        a->loop_depth=depth;
        if(depth)enclosing[pc]=open[depth-1];
        a->failure=KSN_PA_FAIL_STEPS;
        switch(i->op){
        case KSN_PROC_SET:
            if(!reg_ok(i->dst)||!isfinite(i->value))return false;
            a->writes=BIT(i->dst);a->effects=KSN_PA_EFFECT_REG;break;
        case KSN_PROC_INPUT:
            if(!reg_ok(i->dst)||i->a>=KSN_PROC_INPUTS)return false;
            a->writes=BIT(i->dst);a->effects=KSN_PA_EFFECT_REG;break;
        case KSN_PROC_ADD:case KSN_PROC_MUL:
            if(!reg_ok(i->dst)||!reg_ok(i->a)||!reg_ok(i->b))return false;
            a->reads=BIT(i->a)|BIT(i->b);a->writes=BIT(i->dst);
            a->effects=KSN_PA_EFFECT_REG;a->failure|=KSN_PA_FAIL_FINITE;break;
        case KSN_PROC_SIN:
            if(!reg_ok(i->dst)||!reg_ok(i->a))return false;
            a->reads=BIT(i->a);a->writes=BIT(i->dst);
            a->effects=KSN_PA_EFFECT_REG;a->failure|=KSN_PA_FAIL_FINITE;break;
        case KSN_PROC_REPEAT:case KSN_PROC_REPEAT_REG:
            if(depth==KSN_PROC_LOOP_DEPTH ||
               (i->op==KSN_PROC_REPEAT && !i->a) ||
               (i->op==KSN_PROC_REPEAT_REG && !reg_ok(i->a)))return false;
            if(i->op==KSN_PROC_REPEAT_REG){a->reads=BIT(i->a);a->failure|=KSN_PA_FAIL_COUNT;}
            a->effects=KSN_PA_EFFECT_CONTROL;
            open[depth++]=pc;
            a->loop_depth=depth-1;
            break;
        case KSN_PROC_END:
            if(!depth)return false;
            matching[pc]=open[--depth];matching[open[depth]]=pc;
            a->effects=KSN_PA_EFFECT_CONTROL;
            break;
        case KSN_PROC_BREAK_IF_GT:
            if(!depth||!reg_ok(i->a)||!reg_ok(i->b))return false;
            a->reads=BIT(i->a)|BIT(i->b);a->effects=KSN_PA_EFFECT_CONTROL;
            break;
        case KSN_PROC_MOVE:case KSN_PROC_PLOT:case KSN_PROC_LINE:
        case KSN_PROC_PLOT_COLOR_REG:case KSN_PROC_LINE_COLOR_REG:
            if(!reg_ok(i->a)||!reg_ok(i->b))return false;
            a->reads=BIT(i->a)|BIT(i->b);a->effects=KSN_PA_EFFECT_PEN;
            a->failure|=KSN_PA_FAIL_COORD;
            if(i->op==KSN_PROC_PLOT_COLOR_REG||i->op==KSN_PROC_LINE_COLOR_REG){
                if(!reg_ok(i->dst))return false;
                a->reads|=BIT(i->dst);a->failure|=KSN_PA_FAIL_COLOR;
            }
            if(i->op!=KSN_PROC_MOVE){
                a->effects|=KSN_PA_EFFECT_DRAW;
                a->failure|=KSN_PA_FAIL_SEGMENTS|KSN_PA_FAIL_RASTER;
            }
            break;
        case KSN_PROC_CUBIC:
            if(i->dst!=0||i->a<1||i->a>64)return false;
            a->reads=CUBIC_REGS(i->dst);
            a->effects=KSN_PA_EFFECT_PEN|KSN_PA_EFFECT_DRAW;
            a->failure|=KSN_PA_FAIL_COORD|KSN_PA_FAIL_SEGMENTS|KSN_PA_FAIL_RASTER;
            break;
        case KSN_PROC_LINE_PATTERN:
            /* The pen's two registers and the block dst..dst+5; a block value
             * out of range fails as a colour register does. */
            if(!reg_ok(i->a)||!reg_ok(i->b)||i->dst>KSN_PROC_REGS-KSN_PROC_PATTERN_BLOCK||
               !(i->value>=1.0f&&i->value<=(float)KSN_PROC_PATTERN_BITS)||
               i->value!=(float)(unsigned)i->value)return false;
            a->reads=(ksn_pa_regs)(BIT(i->a)|BIT(i->b)|((((uint32_t)1<<KSN_PROC_PATTERN_BLOCK)-1u)<<i->dst));
            a->effects=KSN_PA_EFFECT_PEN|KSN_PA_EFFECT_DRAW;
            a->failure|=KSN_PA_FAIL_COORD|KSN_PA_FAIL_COLOR|
                        KSN_PA_FAIL_SEGMENTS|KSN_PA_FAIL_RASTER;
            break;
        default:return false;
        }
    }
    if(depth)return false;

    /* Static worst-case logical steps. A dynamic count is bounded by 255.
     * Breaks can only shorten an execution. Saturation means no proof. */
    uint32_t multiplier=1, bound=0;
    for(uint8_t pc=0;pc<p->count;pc++){
        const ksn_proc_inst *i=&p->code[pc];
        if(i->op==KSN_PROC_END){
            uint8_t start=matching[pc];
            unsigned n=p->code[start].op==KSN_PROC_REPEAT ? p->code[start].a : 255u;
            bound+=multiplier;
            multiplier/=n;
        }else{
            bound+=multiplier;
            if(i->op==KSN_PROC_REPEAT||i->op==KSN_PROC_REPEAT_REG){
                unsigned n=i->op==KSN_PROC_REPEAT ? i->a : 255u;
                multiplier*=n;
            }
        }
        if(bound>KSN_PROC_STEPS)bound=KSN_PROC_STEPS+1u;
        if(multiplier>KSN_PROC_STEPS)multiplier=KSN_PROC_STEPS+1u;
    }
    if(bound<=KSN_PROC_STEPS)out->max_steps=(uint16_t)bound;
    else out->program_reasons|=KSN_PA_STEP_LIMIT;
    out->program_reasons&=(uint16_t)~KSN_PA_INVALID;
    out->program_reasons|=KSN_PA_OBSERVABLE_REG;

    leader[0]=true;
    for(uint8_t pc=0;pc<p->count;pc++){
        const ksn_proc_inst *i=&p->code[pc];
        ksn_pa_inst *a=&out->inst[pc];
        if(!out->max_steps)a->failure|=KSN_PA_FAIL_STEPS;
        else a->failure&=(uint8_t)~KSN_PA_FAIL_STEPS;
        switch(i->op){
        case KSN_PROC_REPEAT:case KSN_PROC_REPEAT_REG:
            edge(a,(uint8_t)(pc+1));edge(a,(uint8_t)(matching[pc]+1));break;
        case KSN_PROC_END:
            edge(a,(uint8_t)(matching[pc]+1));edge(a,(uint8_t)(pc+1));break;
        case KSN_PROC_BREAK_IF_GT:
            edge(a,(uint8_t)(matching[enclosing[pc]]+1));edge(a,(uint8_t)(pc+1));break;
        default:edge(a,(uint8_t)(pc+1));break;
        }
        if(a->effects & KSN_PA_EFFECT_CONTROL){
            leader[pc]=true;
            if(pc+1<p->count)leader[pc+1]=true;
            for(uint8_t j=0;j<a->successor_count;j++)
                if(a->successor[j]<p->count)leader[a->successor[j]]=true;
        }
    }
    for(uint8_t pc=0;pc<p->count;pc++){
        if(leader[pc]){
            ksn_pa_block *b=&out->block[out->block_count++];
            b->first=pc;b->last=pc;
        }
        ksn_pa_block *b=&out->block[out->block_count-1];
        b->last=pc;out->inst[pc].block=(uint8_t)(out->block_count-1);
        b->use|=(ksn_pa_regs)(out->inst[pc].reads & ~b->def);
        b->def|=out->inst[pc].writes;
    }
    for(uint8_t bi=0;bi<out->block_count;bi++){
        ksn_pa_block *b=&out->block[bi];
        const ksn_pa_inst *tail=&out->inst[b->last];
        for(uint8_t j=0;j<tail->successor_count;j++){
            uint8_t pc=tail->successor[j];
            if(pc<p->count){
                uint8_t target=out->inst[pc].block;
                bool duplicate=false;
                for(uint8_t k=0;k<b->successor_count;k++)
                    duplicate|=b->successor[k]==target;
                if(!duplicate)b->successor[b->successor_count++]=target;
            }
        }
    }
    /* Every register in capture_state is observable at normal exit. */
    bool changed;
    do{
        changed=false;
        for(int bi=(int)out->block_count-1;bi>=0;bi--){
            ksn_pa_block *b=&out->block[bi];
            ksn_pa_regs live=0;
            const ksn_pa_inst *tail=&out->inst[b->last];
            for(uint8_t j=0;j<tail->successor_count;j++)
                if(tail->successor[j]==p->count)live|=ALL_REGS;
            for(uint8_t j=0;j<b->successor_count;j++)
                live|=out->block[b->successor[j]].live_in;
            ksn_pa_regs in=(ksn_pa_regs)(b->use | (live & ~b->def));
            if(live!=b->live_out||in!=b->live_in){
                b->live_out=live;b->live_in=in;changed=true;
            }
        }
    }while(changed);

    for(uint8_t pc=0;pc<p->count;pc++){
        if(p->code[pc].op!=KSN_PROC_REPEAT && p->code[pc].op!=KSN_PROC_REPEAT_REG)continue;
        ksn_pa_loop *l=&out->loop[out->loop_count++];
        l->begin=pc;l->end=matching[pc];
        /* A count check belongs to the loop even when its body is pure.
         * Do not let an independent body authorize eliding a failing header. */
        if(out->inst[pc].failure)l->reasons|=KSN_PA_MAY_FAIL;
        ksn_pa_regs must_def=0;
        for(uint8_t at=(uint8_t)(pc+1);at<l->end;at++){
            const ksn_pa_inst *a=&out->inst[at];
            l->reads|=a->reads;l->writes|=a->writes;
            l->use_before_def|=(ksn_pa_regs)(a->reads & ~must_def);
            /* A nested loop or break can skip subsequent definitions. */
            if(a->effects & KSN_PA_EFFECT_CONTROL)must_def=0;
            else must_def|=a->writes;
            if(a->effects & KSN_PA_EFFECT_DRAW)l->reasons|=KSN_PA_DRAW_ORDER;
            if(a->effects & KSN_PA_EFFECT_PEN)l->reasons|=KSN_PA_PEN;
            if(a->failure)l->reasons|=KSN_PA_MAY_FAIL;
            if(a->effects & KSN_PA_EFFECT_CONTROL)l->reasons|=KSN_PA_CONTROL;
        }
        l->carried=(ksn_pa_regs)(l->writes & l->use_before_def);
        if(l->carried)l->reasons|=KSN_PA_LOOP_CARRIED;
        if(!out->max_steps)l->reasons|=KSN_PA_STEP_LIMIT;
        l->independent=l->reasons==0;
    }
    for(uint8_t pc=0;pc<p->count;pc++){
        ksn_pa_inst *a=&out->inst[pc];
        if(a->effects & KSN_PA_EFFECT_CONTROL)a->relocation_reasons|=KSN_PA_CONTROL;
        if(a->effects & KSN_PA_EFFECT_PEN)a->relocation_reasons|=KSN_PA_PEN;
        if(a->effects & KSN_PA_EFFECT_DRAW)a->relocation_reasons|=KSN_PA_DRAW_ORDER;
        if(a->failure)a->relocation_reasons|=KSN_PA_MAY_FAIL;
        if(!out->max_steps)a->relocation_reasons|=KSN_PA_STEP_LIMIT;
        if(!a->relocation_reasons && pure_total(a)){
            const ksn_pa_block *b=&out->block[a->block];
            bool pair=false;
            if(pc>b->first && pure_total(&out->inst[pc-1]) &&
               independent_pair(&out->inst[pc-1],a))pair=true;
            if(pc<b->last && pure_total(&out->inst[pc+1]) &&
               independent_pair(a,&out->inst[pc+1]))pair=true;
            if(pair)a->relocatable=true;
            else a->relocation_reasons|=KSN_PA_REG_DEP;
        }
        if(!a->loop_depth){a->loop_reasons=KSN_PA_CONTROL;continue;}
        for(uint8_t li=0;li<out->loop_count;li++){
            const ksn_pa_loop *l=&out->loop[li];
            if(pc>l->begin&&pc<l->end)a->loop_reasons|=l->reasons;
        }
        if(a->effects & KSN_PA_EFFECT_CONTROL)a->loop_reasons|=KSN_PA_CONTROL;
        if(a->effects & KSN_PA_EFFECT_PEN)a->loop_reasons|=KSN_PA_PEN;
        if(a->effects & KSN_PA_EFFECT_DRAW)a->loop_reasons|=KSN_PA_DRAW_ORDER;
        if(a->failure)a->loop_reasons|=KSN_PA_MAY_FAIL;
        a->loop_independent=a->loop_reasons==0;
    }
    out->valid=true;
    return true;
}
