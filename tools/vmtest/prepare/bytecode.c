/* Host-only inspection, like tools/vmtest/floor: reuse QuickJS's parser and
 * opcode definitions in its own translation unit. Do not copy the JS grammar. */
#include "quickjs.c"
#include "bytecode.h"

typedef struct {prep_value stack[PREP_STACK];unsigned top;} prep_scratch;
static prep_value unknown(void){return (prep_value){0};}
static bool atom_is(JSContext *ctx,JSAtom atom,const char *word){
    const char *s=JS_AtomToCString(ctx,atom);
    bool equal=s&&!strcmp(s,word);JS_FreeCString(ctx,s);return equal;
}
static prep_value literal(JSContext *ctx,JSValueConst v){
    prep_value a=unknown();
    if(JS_IsNumber(v)){a.kind=P_NUMBER;JS_ToFloat64(ctx,&a.u.number,v);}
    else if(JS_IsString(v)){
        size_t n=0;const char *s=JS_ToCStringLen(ctx,&n,v);
        if(s&&n<sizeof a.u.text&&!memchr(s,0,n)){
            a.kind=P_STRING;a.length=(unsigned)n;memcpy(a.u.text,s,n+1);
        }
        JS_FreeCString(ctx,s);
    }
    return a;
}
static bool push(prep_scratch *w,prep_value v){
    if(w->top==PREP_STACK)return false;
    w->stack[w->top++]=v;return true;
}
static prep_value pop(prep_scratch *w){return w->top?w->stack[--w->top]:unknown();}
static bool control_flow(unsigned f){
    return f==OP_FMT_label||f==OP_FMT_label8||f==OP_FMT_label16||
           f==OP_FMT_atom_label_u8||f==OP_FMT_atom_label_u16||f==OP_FMT_label_u16;
}
static bool function(JSContext *ctx,JSFunctionBytecode *b,prep_report *r,prep_scratch *w){
    if(r->functions==PREP_FUNCTIONS)return false;
    unsigned id=r->functions++,pc=0;bool branching=false;
    while(pc<(unsigned)b->byte_code_len){
        unsigned op=b->byte_code_buf[pc];if(op>=OP_COUNT)return false;
        const JSOpCode *d=&short_opcode_info(op);
        if(!d->size||d->size>(unsigned)b->byte_code_len-pc)return false;
        branching|=control_flow(d->fmt);pc+=d->size;
    }
    w->top=0;
    for(pc=0;pc<(unsigned)b->byte_code_len;){
        const uint8_t *p=b->byte_code_buf+pc;
        unsigned op=*p;const JSOpCode *d=&short_opcode_info(op);
        prep_value a=unknown();bool put=false;
        if(op>=OP_push_minus1&&op<=OP_push_7){a.kind=P_NUMBER;a.u.number=(int)op-OP_push_0;put=true;}
        else switch(op){
        case OP_push_i8:a.kind=P_NUMBER;a.u.number=(int8_t)p[1];put=true;break;
        case OP_push_i16:a.kind=P_NUMBER;a.u.number=(int16_t)get_u16(p+1);put=true;break;
        case OP_push_i32:a.kind=P_NUMBER;a.u.number=(int32_t)get_u32(p+1);put=true;break;
        case OP_push_const:case OP_push_const8:{
            unsigned k=op==OP_push_const?get_u32(p+1):p[1];
            if(k>=(unsigned)b->cpool_count)return false;
            a=literal(ctx,b->cpool[k]);put=true;break;
        }
        case OP_push_atom_value:{
            JSValue v=JS_AtomToValue(ctx,get_u32(p+1));a=literal(ctx,v);JS_FreeValue(ctx,v);put=true;break;
        }
        case OP_get_var:case OP_get_var_undef:
            if(atom_is(ctx,get_u32(p+1),"pocket")){a.kind=P_PATH;a.u.path=1;}
            put=true;break;
        case OP_get_field:case OP_get_field2:{
            prep_value base=pop(w);
            static const char *const parts[]={"","pocket","kasane","procedural","register"};
            if(base.kind==P_PATH&&base.u.path<4&&atom_is(ctx,get_u32(p+1),parts[base.u.path+1])){
                a.kind=P_PATH;a.u.path=base.u.path+1;
            }
            if(op==OP_get_field2&&!push(w,base))return false;
            put=true;break;
        }
        case OP_null:case OP_undefined:a.kind=P_EMPTY;put=true;break;
        case OP_array_from:{
            unsigned n=get_u16(p+1);a.kind=P_VECTOR;a.length=n;
            if(n>PREP_NUMBERS||n>w->top){w->top=0;a=unknown();}
            else for(unsigned i=n;i-->0;){prep_value v=pop(w);if(v.kind!=P_NUMBER)a.kind=P_UNKNOWN;a.u.numbers[i]=v.u.number;}
            put=true;break;
        }
        case OP_neg:a=pop(w);if(a.kind==P_NUMBER)a.u.number=-a.u.number;else a=unknown();put=true;break;
        case OP_dup:if(!push(w,w->top?w->stack[w->top-1]:unknown()))return false;break;
        case OP_drop:(void)pop(w);break;
        case OP_call:case OP_tail_call:case OP_call_method:case OP_tail_call_method:
        case OP_call0:case OP_call1:case OP_call2:case OP_call3:{
            if(r->count==PREP_CALLS)return false;
            bool method=op==OP_call_method||op==OP_tail_call_method;
            unsigned n=op>=OP_call0&&op<=OP_call3?op-OP_call0:get_u16(p+1);
            prep_call *c=&r->calls[r->count++];*c=(prep_call){.function=id,.pc=pc,.argc=n};
            if(n+(method?2:1)<=w->top){
                unsigned first=w->top-n;
                prep_value callee=w->stack[first-1];
                c->path_candidate=!branching&&callee.kind==P_PATH&&callee.u.path==4;
                if(n<=3)for(unsigned i=0;i<n;i++)c->args[i]=w->stack[first+i];
                w->top=first-(method?2:1);
            }else w->top=0;
            /* A call result stays unknown. No return-type inference. */
            put=true;break;
        }
        default:
            /* Unsupported value transforms, aliasing and branch merges are
             * barriers. Rebuild facts only from subsequent known bytecodes. */
            w->top=0;break;
        }
        if(put&&!push(w,a))return false;
        pc+=d->size;
    }
    /* Reuse the same bounded scratch for every function, not a stack per
     * closure. A failed bound rejects all sites, never a partial artifact. */
    for(int i=0;i<b->cpool_count;i++)
        if(JS_VALUE_GET_TAG(b->cpool[i])==JS_TAG_FUNCTION_BYTECODE&&
           !function(ctx,JS_VALUE_GET_PTR(b->cpool[i]),r,w))return false;
    return true;
}
bool prep_analyze(JSContext *ctx,JSValueConst compiled,prep_report *r){
    prep_scratch scratch;*r=(prep_report){.scratch_bytes=sizeof scratch};
    if(JS_VALUE_GET_TAG(compiled)!=JS_TAG_FUNCTION_BYTECODE||
       !function(ctx,JS_VALUE_GET_PTR(compiled),r,&scratch)||JS_HasException(ctx)){
        if(JS_HasException(ctx))JS_FreeValue(ctx,JS_GetException(ctx));
        r->count=0;r->bounded_fallback=true;return false;
    }
    return true;
}
bool prep_native_identity(JSContext *ctx,JSValueConst value,JSCFunction *expected){
    if(JS_VALUE_GET_TAG(value)!=JS_TAG_OBJECT)return false;
    JSObject *o=JS_VALUE_GET_OBJ(value);
    return o->class_id==JS_CLASS_C_FUNCTION&&o->u.cfunc.realm==ctx&&
           o->u.cfunc.cproto==JS_CFUNC_generic&&o->u.cfunc.c_function.generic==expected;
}
