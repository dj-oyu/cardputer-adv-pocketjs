/* Included by quickjs.c: no engine layout changes or second parser. This
 * deliberately recognizes less than the host experiment: one top-level,
 * contiguous direct-path call with a literal name and literal number vector.
 * Everything else remains normal execution. No atom/string allocation. */
static bool pjs_prep_atom(JSContext *ctx,JSAtom atom,const char *text) {
    if (__JS_AtomIsTaggedInt(atom) || atom >= ctx->rt->atom_size) return false;
#ifdef POCKET_VM_ROM_ATOMS
    const JSRomAtom *r=js_rom_atom(atom);
    if (r) return r->len==strlen(text) && !memcmp(js_rom_text(r),text,r->len);
#endif
    JSAtomStruct *s=ctx->rt->atom_array[atom];
    return s && s->atom_type==JS_ATOM_TYPE_STRING && !s->is_wide_char &&
           s->len==strlen(text) && !memcmp(str8(s),text,s->len);
}
static bool pjs_prep_name(JSContext *ctx,JSAtom atom,char name[64]) {
    if (__JS_AtomIsTaggedInt(atom) || atom >= ctx->rt->atom_size) return false;
#ifdef POCKET_VM_ROM_ATOMS
    const JSRomAtom *r=js_rom_atom(atom);
    if (r) {
        if (r->len>=64 || memchr(js_rom_text(r),0,r->len)) return false;
        memcpy(name,js_rom_text(r),r->len);name[r->len]=0;return true;
    }
#endif
    JSAtomStruct *s=ctx->rt->atom_array[atom];
    if (!s || s->atom_type!=JS_ATOM_TYPE_STRING || s->is_wide_char ||
        s->len>=64 || memchr(str8(s),0,s->len)) return false;
    memcpy(name,str8(s),s->len);name[s->len]=0;return true;
}
static bool pjs_prep_number(JSFunctionBytecode *b,const uint8_t *p,double *n) {
    unsigned op=*p;
    if (op>=OP_push_minus1 && op<=OP_push_7) *n=(int)op-OP_push_0;
    else switch (op) {
    case OP_push_i8:*n=(int8_t)p[1];break;
    case OP_push_i16:*n=(int16_t)get_u16(p+1);break;
    case OP_push_i32:*n=(int32_t)get_u32(p+1);break;
    case OP_push_const:case OP_push_const8: {
        unsigned k=op==OP_push_const?get_u32(p+1):p[1];
        if (k>=(unsigned)b->cpool_count) return false;
        JSValue v=((JSValue *)(b+1))[k];
        if (JS_VALUE_GET_TAG(v)==JS_TAG_INT) *n=JS_VALUE_GET_INT(v);
        else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v))) *n=JS_VALUE_GET_FLOAT64(v);
        else return false;
        break;
    }
    default:return false;
    }
    return isfinite(*n) && isfinite((float)*n);
}
bool pjs_prepare_literal_call(JSContext *ctx,JSValueConst compiled,PJSPreparedCall *out) {
    *out=(PJSPreparedCall){0};
    if (JS_VALUE_GET_TAG(compiled)!=JS_TAG_FUNCTION_BYTECODE) return false;
    JSFunctionBytecode *b=JS_VALUE_GET_PTR(compiled);
    if (b->byte_code_len<=0 || b->byte_code_len>16384) return false;
    /* Validate instruction boundaries first and reject branch-bearing roots.
     * Nested function bodies are not inspected or authorized. */
    for (unsigned pc=0;pc<(unsigned)b->byte_code_len;) {
        unsigned op=b->byte_code_buf[pc];if (op>=OP_COUNT) return false;
        const JSOpCode *d=&short_opcode_info(op);
        if (!d->size || d->size>(unsigned)b->byte_code_len-pc) return false;
        unsigned f=d->fmt;
        if (f==OP_FMT_label || f==OP_FMT_label8 || f==OP_FMT_label16 ||
            f==OP_FMT_atom_label_u8 || f==OP_FMT_atom_label_u16 ||
            f==OP_FMT_label_u16) return false;
        pc+=d->size;
    }
    unsigned found=0;
    for (unsigned start=0;start<(unsigned)b->byte_code_len;) {
        const uint8_t *p=b->byte_code_buf+start;
        unsigned next=start+short_opcode_info(*p).size;
        PJSPreparedCall c={0};unsigned pc=start;
        const char *parts[]={"pocket","kasane","procedural","register"};
        bool ok=true;
        for (unsigned k=0;k<4;k++) {
            if (pc>=(unsigned)b->byte_code_len) {ok=false;break;}
            p=b->byte_code_buf+pc;
            unsigned want=k==0?OP_get_var:k==3?OP_get_field2:OP_get_field;
            if ((*p!=want && !(k==0 && *p==OP_get_var_undef)) ||
                !pjs_prep_atom(ctx,get_u32(p+1),parts[k])) {ok=false;break;}
            pc+=short_opcode_info(*p).size;
        }
        if (!ok || pc>=(unsigned)b->byte_code_len) goto skip;
        c.lookup_pc=pc;
        p=b->byte_code_buf+pc;
        if (*p!=OP_push_atom_value || !pjs_prep_name(ctx,get_u32(p+1),c.name)) goto skip;
        pc+=short_opcode_info(*p).size;
        while (pc<(unsigned)b->byte_code_len && c.params<PJS_PREPARED_PARAMS) {
            p=b->byte_code_buf+pc;double n;
            if (!pjs_prep_number(b,p,&n)) break;
            pc+=short_opcode_info(*p).size;
            if (pc<(unsigned)b->byte_code_len && b->byte_code_buf[pc]==OP_neg) {
                n=-n;pc+=short_opcode_info(OP_neg).size;
            }
            c.numbers[c.params]=n;c.args[c.params++]=(float)n;
        }
        if (pc>=(unsigned)b->byte_code_len) goto skip;
        p=b->byte_code_buf+pc;
        if (*p!=OP_array_from || get_u16(p+1)!=c.params) goto skip;
        pc+=short_opcode_info(*p).size;
        c.argc=2;
        if (pc<(unsigned)b->byte_code_len &&
            (b->byte_code_buf[pc]==OP_undefined || b->byte_code_buf[pc]==OP_null)) {
            pc+=short_opcode_info(b->byte_code_buf[pc]).size;c.argc=3;
        }
        if (pc>=(unsigned)b->byte_code_len) goto skip;
        p=b->byte_code_buf+pc;
        if (*p!=OP_call_method || get_u16(p+1)!=c.argc) goto skip;
        c.bytecode=b;c.next_pc=pc+short_opcode_info(*p).size;
        if (++found>1) {*out=(PJSPreparedCall){0};return false;}
        *out=c;
    skip:start=next;
    }
    return found==1;
}

__attribute__((weak,noinline)) void pjs_prepared_binding_created(JSContext *ctx,JSValueConst value,const void *bytecode,unsigned pc) {
    (void)ctx;(void)value;(void)bytecode;(void)pc;
}
__attribute__((weak,noinline)) bool pjs_prepared_dispatch(JSContext *ctx,JSValueConst callee,
        JSValueConst self,int argc,JSValueConst *argv,const void *bytecode,
        unsigned next_pc,JSValue *out) {
    (void)ctx;(void)callee;(void)self;(void)argc;(void)argv;
    (void)bytecode;(void)next_pc;(void)out;return false;
}
