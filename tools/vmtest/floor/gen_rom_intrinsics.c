// R2 (docs/vm/rom-intrinsics.md sec.9): write the hardened intrinsic graph
// as C const data, components/quickjs-ng/quickjs-ng/quickjs-rom-intrinsics.h.
//
// Built with quickjs.c included, so the heap is read from inside. The graph
// is the one a device context ends up with: JS_NewContext, JS_PocketHarden,
// then every lazy builtin built and every AUTOINIT slot resolved (the
// js_immortal_resolve that vmrun --immortal uses). What is emitted:
//   - every object reachable from the context's intrinsic fields and from the
//     global object's properties, except the global object and
//     global_var_obj (RAM, rom-intrinsics.md RD3);
//   - their shapes (unhashed, one per distinct shape), property arrays,
//     string values and C-function-data records;
//   - the wiring: the context fields, the global object's properties, the
//     well-known symbols.
// Fields are written by name (designated initializers), so the one file
// compiles for the 64-bit host and the 32-bit device alike; the JSValue
// encoding comes from macros quickjs.c defines per target (ROMV_*). C
// function pointers are written by name: the generator is linked without
// PIE and reads its own symbol table (nm output, argv[2]).
//
// Refused while generating, because the runtime relies on each:
//   - a value pointing into RAM that is not part of the graph (a realm, a
//     bytecode function other than the RD5 ones, an AUTOINIT slot left);
//   - an atom that is not a fixed number (>= JS_ROM_ATOM_END);
//   - a property kind other than plain data and getter/setter;
//   - an object of a class this file does not know how to write.
#include "quickjs.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXO 2048
#define MAXS 512
#define MAXSTR 2048
#define MAXSYM 64

static JSContext *G_ctx;
static JSObject *objs[MAXO];
static int nobjs;
static JSShape *shapes[MAXS];
static int nshapes;
static JSString *strs[MAXSTR];
static int nstrs;
static int fails;

#define FAIL(...) do { fprintf(stderr, "gen_rom_intrinsics: " __VA_ARGS__); fputc('\n', stderr); fails++; } while (0)

static int obj_index(JSObject *p)
{
    for (int i = 0; i < nobjs; i++)
        if (objs[i] == p)
            return i;
    return -1;
}

static int shape_index(JSShape *sh)
{
    for (int i = 0; i < nshapes; i++)
        if (shapes[i] == sh)
            return i;
    return -1;
}

static bool is_ram_root(JSObject *p)
{
    return p == JS_VALUE_GET_OBJ(G_ctx->global_obj) ||
           p == JS_VALUE_GET_OBJ(G_ctx->global_var_obj);
}

/* A function made of bytecode cannot be in flash (IC and quickening write
   to it). The three JS-defined builtins RD5 used to drop here are C now
   (docs/vm/c-builtins-zip-fromasync.md), so meeting one again is an error,
   not something to leave out quietly. */
static bool is_bytecode_function(JSValueConst v)
{
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return false;
    switch (JS_VALUE_GET_OBJ(v)->class_id) {
    case JS_CLASS_BYTECODE_FUNCTION:
    case JS_CLASS_GENERATOR_FUNCTION:
    case JS_CLASS_ASYNC_FUNCTION:
    case JS_CLASS_ASYNC_GENERATOR_FUNCTION:
        return true;
    default:
        return false;
    }
}

/* Nothing is left out any more; kept for the property walk's shape. */
static bool dropped_value(JSValueConst v)
{
    (void)v;
    return false;
}

static void add_obj(JSObject *p);

/* performance: its timeOrigin is the context's start time, so the ROM
   context makes it in RAM (js_rom_context_init) and ROM leaves it out. */
static bool is_ram_global(JSAtom atom)
{
    char buf[64];
    return !strcmp(JS_AtomGetStr(G_ctx, buf, sizeof buf, atom), "performance");
}

static void add_value(JSValueConst v, const char *where)
{
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_OBJECT: {
        JSObject *p = JS_VALUE_GET_OBJ(v);
        if (is_ram_root(p))
            FAIL("%s: points at the global object", where);
        else if (is_bytecode_function(v))
            FAIL("%s: a bytecode function cannot be in flash", where);
        else if (!dropped_value(v))
            add_obj(p);
        break;
    }
    case JS_TAG_STRING: {
        JSString *s = JS_VALUE_GET_STRING(v);
        for (int i = 0; i < nstrs; i++)
            if (strs[i] == s)
                return;
        if (nstrs == MAXSTR) { FAIL("too many strings"); return; }
        strs[nstrs++] = s;
        break;
    }
    case JS_TAG_SYMBOL:
    case JS_TAG_INT:
    case JS_TAG_BOOL:
    case JS_TAG_NULL:
    case JS_TAG_UNDEFINED:
    case JS_TAG_FLOAT64:
        break;
    default:
        FAIL("%s: value tag %d", where, JS_VALUE_GET_TAG(v));
    }
}

static void add_obj(JSObject *p)
{
    if (obj_index(p) >= 0)
        return;
    if (nobjs == MAXO) { FAIL("too many objects"); return; }
    objs[nobjs++] = p;
    JSShape *sh = p->shape;
    if (shape_index(sh) < 0) {
        if (nshapes == MAXS) { FAIL("too many shapes"); return; }
        shapes[nshapes++] = sh;
    }
    if (sh->proto)
        add_obj(sh->proto);
    for (int i = 0; i < sh->prop_count; i++) {
        JSShapeProperty *prs = &sh->prop[i];
        JSProperty *pr = &p->prop[i];
        if (prs->atom == JS_ATOM_NULL)
            continue;
        switch (prs->flags & JS_PROP_TMASK) {
        case JS_PROP_NORMAL:
            add_value(pr->u.value, "property");
            break;
        case JS_PROP_GETSET:
            if (pr->u.getset.getter) add_obj(pr->u.getset.getter);
            if (pr->u.getset.setter) add_obj(pr->u.getset.setter);
            break;
        default:
            FAIL("object %d property %d: kind %d", obj_index(p), i, prs->flags & JS_PROP_TMASK);
        }
    }
    switch (p->class_id) {
    case JS_CLASS_C_FUNCTION_DATA: {
        JSCFunctionDataRecord *r = p->u.c_function_data_record;
        for (int i = 0; i < r->data_len; i++)
            add_value(r->data[i], "c function data");
        break;
    }
    case JS_CLASS_NUMBER:
    case JS_CLASS_STRING:
    case JS_CLASS_BOOLEAN:
    case JS_CLASS_SYMBOL:
    case JS_CLASS_BIG_INT:
        add_value(p->u.object_data, "object data");
        break;
    default:
        break;
    }
}

/* The context's intrinsic fields, in struct order. */
#define CTX_FIELDS(X) \
    X(function_proto) X(function_ctor) X(array_ctor) X(regexp_ctor) \
    X(promise_ctor) X(error_ctor) X(iterator_ctor) X(iterator_ctor_getset) \
    X(iterator_proto) X(async_iterator_proto) X(array_proto_values) \
    X(throw_type_error) X(eval_obj) X(ta_base)

static void collect(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
#define ADD(f) add_value(ctx->f, "ctx." #f);
    CTX_FIELDS(ADD)
#undef ADD
    for (int i = 0; i < JS_NATIVE_ERROR_COUNT; i++)
        add_value(ctx->native_error_proto[i], "ctx.native_error_proto");
    for (int i = 0; i < rt->class_count; i++)
        add_value(ctx->class_proto[i], "ctx.class_proto");
    JSObject *g = JS_VALUE_GET_OBJ(ctx->global_obj);
    if (g->shape->proto)
        add_obj(g->shape->proto);
    for (int i = 0; i < g->shape->prop_count; i++) {
        JSShapeProperty *prs = &g->shape->prop[i];
        if (prs->atom == JS_ATOM_NULL)
            continue;
        if ((prs->flags & JS_PROP_TMASK) == JS_PROP_NORMAL) {
            /* globalThis: the global object itself, RAM like its owner */
            JSValueConst v = g->prop[i].u.value;
            if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(v) == g)
                continue;
            if (is_ram_global(prs->atom))
                continue;
            add_value(v, "global");
        }
        else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
            if (g->prop[i].u.getset.getter) add_obj(g->prop[i].u.getset.getter);
            if (g->prop[i].u.getset.setter) add_obj(g->prop[i].u.getset.setter);
        } else
            FAIL("global property %d: kind %d", i, prs->flags & JS_PROP_TMASK);
    }
    JSObject *gv = JS_VALUE_GET_OBJ(ctx->global_var_obj);
    if (gv->shape->prop_count)
        FAIL("global_var_obj has %d properties", gv->shape->prop_count);
}

static void stats(void)
{
    int by_class[JS_CLASS_INIT_COUNT] = {0};
    for (int i = 0; i < nobjs; i++)
        if (objs[i]->class_id < JS_CLASS_INIT_COUNT)
            by_class[objs[i]->class_id]++;
    fprintf(stderr, "objects %d, shapes %d, strings %d\n", nobjs, nshapes, nstrs);
    for (int c = 0; c < JS_CLASS_INIT_COUNT; c++)
        if (by_class[c])
            fprintf(stderr, "  class %d: %d\n", c, by_class[c]);
    int hashed = 0, big_atoms = 0, wide = 0, lazy = 0, cfunc_realm = 0;
    for (int i = 0; i < nshapes; i++) {
        hashed += shapes[i]->is_hashed;
        for (int k = 0; k < shapes[i]->prop_count; k++)
            if (shapes[i]->prop[k].atom >= JS_ROM_ATOM_END) {
                big_atoms++;
                char buf[64];
                fprintf(stderr, "  dynamic atom %u '%s'\n", shapes[i]->prop[k].atom,
                        JS_AtomGetStr(G_ctx, buf, sizeof buf, shapes[i]->prop[k].atom));
            }
    }
    for (int i = 0; i < nstrs; i++)
        wide += strs[i]->is_wide_char;
    for (int i = 0; i < nobjs; i++) {
        lazy += js_obj_lazy(objs[i]);
        cfunc_realm += objs[i]->class_id == JS_CLASS_C_FUNCTION && objs[i]->u.cfunc.realm;
    }
    fprintf(stderr, "hashed shapes %d, dynamic atoms %d, wide strings %d, lazy objects %d, realms %d\n",
            hashed, big_atoms, wide, lazy, cfunc_realm);
}

/* ---- emission ---- */

static JSString *syms[MAXSYM];
static int nsyms;

typedef struct { uintptr_t addr; char name[96]; } sym_t;
static sym_t *fsyms;
static int nfsyms;

static int by_addr(const void *a, const void *b)
{
    uintptr_t x = ((const sym_t *)a)->addr, y = ((const sym_t *)b)->addr;
    return x < y ? -1 : x > y;
}

static void load_syms(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[256];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long a; char t; char name[200];
        if (sscanf(line, "%lx %c %199s", &a, &t, name) != 3) continue;
        if (t != 't' && t != 'T') continue;
        if (strchr(name, '.')) continue;   /* .constprop clones are not address-taken */
        if (nfsyms == cap) { cap = cap ? cap * 2 : 4096; fsyms = realloc(fsyms, cap * sizeof *fsyms); }
        fsyms[nfsyms].addr = a;
        snprintf(fsyms[nfsyms].name, sizeof fsyms[nfsyms].name, "%s", name);
        nfsyms++;
    }
    fclose(f);
    qsort(fsyms, nfsyms, sizeof *fsyms, by_addr);
}

static const char *fn_name(const void *fp)
{
    uintptr_t a = (uintptr_t)fp;
    int lo = 0, hi = nfsyms - 1, hit = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (fsyms[m].addr == a) { hit = m; break; }
        if (fsyms[m].addr < a) lo = m + 1; else hi = m - 1;
    }
    if (hit < 0) { FAIL("no symbol for function %p", fp); return "NULL"; }
    /* two names at one address (an alias): which one the source took is
       unknown, so refuse rather than guess */
    if ((hit > 0 && fsyms[hit - 1].addr == a) || (hit + 1 < nfsyms && fsyms[hit + 1].addr == a))
        FAIL("two symbols at %p (%s)", fp, fsyms[hit].name);
    return fsyms[hit].name;
}

static int str_index(JSString *p)
{
    for (int i = 0; i < nstrs; i++)
        if (strs[i] == p)
            return i;
    return -1;
}

static int sym_index(JSString *p)
{
    for (int i = 0; i < nsyms; i++)
        if (syms[i] == p)
            return i;
    if (nsyms == MAXSYM) { FAIL("too many symbols"); return 0; }
    syms[nsyms] = p;
    return nsyms++;
}

static void put_cstr(FILE *f, const uint8_t *s, uint32_t len)
{
    fputc('"', f);
    for (uint32_t i = 0; i < len; i++) {
        unsigned c = s[i];
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 32 || c >= 127) fprintf(f, "\\%03o", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

/* A JSValue through one of the ROM_PV / ROM_JSV / ROM_OBJDATA macro sets
   (quickjs.c defines them per JSValue encoding). */
static void put_value(FILE *f, const char *kind, JSValueConst v)
{
    int tag = JS_VALUE_GET_TAG(v);
    switch (tag) {
    case JS_TAG_OBJECT: {
        int k = obj_index(JS_VALUE_GET_OBJ(v));
        if (k < 0) FAIL("value: object not in the graph");
        fprintf(f, "%s_PTR(JS_TAG_OBJECT, &js_rom_obj[%d])", kind, k);
        return;
    }
    case JS_TAG_STRING: {
        int k = str_index(JS_VALUE_GET_STRING(v));
        if (k < 0) FAIL("value: string not in the graph");
        fprintf(f, "%s_PTR(JS_TAG_STRING, &js_rom_str_%d.s)", kind, k);
        return;
    }
    case JS_TAG_SYMBOL:
        fprintf(f, "%s_PTR(JS_TAG_SYMBOL, &js_rom_sym_%d.s)", kind, sym_index(JS_VALUE_GET_PTR(v)));
        return;
    case JS_TAG_INT:
    case JS_TAG_BOOL:
    case JS_TAG_NULL:
    case JS_TAG_UNDEFINED:
        fprintf(f, "%s_INT(%d, %d)", kind, tag, JS_VALUE_GET_INT(v));
        return;
    case JS_TAG_FLOAT64: {
        double d = JS_VALUE_GET_FLOAT64(v);
        union { double d; uint64_t u; } u = { .d = d };
        if (d != d)
            fprintf(f, "%s_NAN()", kind);
        else if (d == __builtin_inf() || d == -__builtin_inf())
            fprintf(f, "%s_F64(0x%016llxull, %s__builtin_inf())", kind, (unsigned long long)u.u, d < 0 ? "-" : "");
        else
            fprintf(f, "%s_F64(0x%016llxull, %a)", kind, (unsigned long long)u.u, d);
        return;
    }
    default:
        FAIL("value tag %d", tag);
        fprintf(f, "%s_INT(0, 0)", kind);
    }
}

/* The properties an object keeps in ROM, in shape order: free slots and the
   RD5 bytecode builtins left out. */
static int kept_props(JSObject *p, int *idx)
{
    int n = 0;
    for (int i = 0; i < p->shape->prop_count; i++) {
        JSShapeProperty *prs = &p->shape->prop[i];
        if (prs->atom == JS_ATOM_NULL)
            continue;
        if ((prs->flags & JS_PROP_TMASK) == JS_PROP_NORMAL && dropped_value(p->prop[i].u.value))
            continue;
        idx[n++] = i;
    }
    return n;
}

static int shape_owner[MAXS];   /* an object using the shape, for kept_props */
static int prop_off[MAXO];

static void emit_shape(FILE *f, int si)
{
    JSShape *sh = shapes[si];
    JSObject *owner = objs[shape_owner[si]];
    int idx[1024];
    int n = kept_props(owner, idx);
    /* every user of a shared shape must keep the same properties */
    for (int k = 0; k < nobjs; k++)
        if (objs[k]->shape == sh) {
            int idx2[1024];
            if (kept_props(objs[k], idx2) != n || memcmp(idx, idx2, n * sizeof *idx))
                FAIL("shape %d: users keep different properties", si);
        }
    uint32_t H = sh->prop_hash_mask + 1;
    uint32_t *head = calloc(H, sizeof *head), *next = calloc(n + 1, sizeof *next);
    for (int j = 0; j < n; j++) {
        uint32_t h = sh->prop[idx[j]].atom & (H - 1);
        next[j] = head[h];
        head[h] = j + 1;
    }
    fprintf(f, "static const ROM_SHAPE_T(%u, %d) js_rom_sh_%d = {\n    .hash = {", H, n ? n : 1, si);
    for (uint32_t h = 0; h < H; h++)
        if (head[h])
            fprintf(f, " [ROM_HPAD(%u) - 1 - %u] = %u,", H, h, head[h]);
    fprintf(f, " },\n    .sh = { .header = { .ref_count = JS_IMMORTAL_REF_COUNT, .gc_obj_type = JS_GC_OBJ_TYPE_SHAPE },\n"
               "            .prop_hash_mask = %u, .prop_size = %d, .prop_count = %d,\n", H - 1, n, n);
    if (sh->proto)
        fprintf(f, "            .proto = (JSObject *)&js_rom_obj[%d] },\n", obj_index(sh->proto));
    else
        fprintf(f, "            .proto = NULL },\n");
    fprintf(f, "    .prop = {");
    for (int j = 0; j < n; j++) {
        JSShapeProperty *prs = &sh->prop[idx[j]];
        if (!__JS_AtomIsConst(prs->atom))
            FAIL("shape %d: atom %u has no fixed number", si, prs->atom);
        char buf[64];
        fprintf(f, "\n        { .hash_next = %u, .flags = 0x%02x, .atom = %u }, /* %s */",
                next[j], prs->flags, prs->atom, JS_AtomGetStr(G_ctx, buf, sizeof buf, prs->atom));
    }
    fprintf(f, "\n    },\n};\nROM_SHAPE_CHECK(js_rom_sh_%d, %u);\n\n", si, H);
    free(head);
    free(next);
}

static void emit_object(FILE *f, int k)
{
    JSObject *p = objs[k];
    /* the GC mark (bit 4) and the lazy bit (bit 5, dummy0 bit 0) clear; the
       harden lock bit (bit 6) kept */
    unsigned mark = p->__gc_mark & ~(1u << 4) & ~(1u << 5);
    char buf[64];
    fprintf(f, "    [%d] = { .__gc_ref_count = JS_IMMORTAL_REF_COUNT, .__gc_mark = 0x%02x, .is_prototype = 1,\n"
               "           .extensible = %u, .is_exotic = %u, .fast_array = %u, .is_constructor = %u,\n"
               "           .is_HTMLDDA = %u, .class_id = %u,\n"
               "           .shape = (JSShape *)&js_rom_sh_%d.sh, .prop = (JSProperty *)&js_rom_props[%d],\n",
            k, mark, p->extensible, p->is_exotic, p->fast_array, p->is_constructor, p->is_HTMLDDA,
            p->class_id, shape_index(p->shape), prop_off[k]);
    switch (p->class_id) {
    case JS_CLASS_OBJECT:
    case JS_CLASS_ERROR:
        break;
    case JS_CLASS_C_FUNCTION:
        fprintf(f, "           .u.cfunc = { .c_function.generic = (JSCFunction *)%s, .length = %u, .cproto = %u, .magic = ",
                fn_name((void *)p->u.cfunc.c_function.generic), p->u.cfunc.length, p->u.cfunc.cproto);
        /* These are private bytecode offsets, not ABI-independent integers.
           Let the consuming compiler resolve its own layout. */
        if (p->u.cfunc.c_function.generic == (JSCFunction *)js_function_proto_int32) {
            if (p->u.cfunc.magic == offsetof(JSFunctionBytecode, line_num))
                fprintf(f, "offsetof(JSFunctionBytecode, line_num)");
            else if (p->u.cfunc.magic == offsetof(JSFunctionBytecode, col_num))
                fprintf(f, "offsetof(JSFunctionBytecode, col_num)");
            else
                FAIL("unexpected function metadata offset %d", p->u.cfunc.magic);
        } else {
            fprintf(f, "%d", p->u.cfunc.magic);
        }
        fprintf(f, " },\n");
        break;
    case JS_CLASS_C_FUNCTION_DATA:
        fprintf(f, "           .u.c_function_data_record = (JSCFunctionDataRecord *)&js_rom_cfd_%d,\n", k);
        break;
    case JS_CLASS_ARRAY:
        /* Array.prototype: harden made it a plain (non-fast) array */
        if (p->u.array.count != 0 || p->u.array.u.values)
            FAIL("object %d: an array that is not empty", k);
        break;
    case JS_CLASS_NUMBER:
    case JS_CLASS_STRING:
    case JS_CLASS_BOOLEAN:
    case JS_CLASS_SYMBOL:
    case JS_CLASS_BIG_INT:
        fprintf(f, "           ");
        put_value(f, "ROM_OBJDATA", p->u.object_data);
        fprintf(f, ",\n");
        break;
    default:
        FAIL("object %d: class %d not handled", k, p->class_id);
    }
    /* a comment naming the object, from its own name property */
    const char *name = "";
    for (int i = 0; i < p->shape->prop_count; i++)
        if (p->shape->prop[i].atom == JS_ATOM_name &&
            (p->shape->prop[i].flags & JS_PROP_TMASK) == JS_PROP_NORMAL &&
            JS_VALUE_GET_TAG(p->prop[i].u.value) == JS_TAG_STRING) {
            JSString *s = JS_VALUE_GET_STRING(p->prop[i].u.value);
            snprintf(buf, sizeof buf, "%.*s", (int)s->len, (const char *)str8(s));
            name = buf;
        }
    fprintf(f, "    }, /* %s */\n", name);
}

static void emit_string(FILE *f, int k)
{
    JSString *s = strs[k];
    if (s->is_wide_char) { FAIL("string %d is wide", k); return; }
    if (s->kind != JS_STRING_KIND_NORMAL) { FAIL("string %d: kind %d", k, s->kind); return; }
    fprintf(f, "static const ROM_STR_T(%u) js_rom_str_%d = { .s = { .header = { JS_IMMORTAL_REF_COUNT }, .len = %u, .kind = JS_STRING_KIND_NORMAL }, .c = ",
            s->len + 1, k, s->len);
    put_cstr(f, str8(s), s->len);
    fprintf(f, " };\n");
}

static void emit_symbol(FILE *f, int k)
{
    JSString *s = syms[k];
    if (s->is_wide_char || s->atom_type != JS_ATOM_TYPE_SYMBOL || s->kind != JS_STRING_KIND_NORMAL) {
        FAIL("symbol %d: unexpected form", k);
        return;
    }
    if (s->hash_next >= JS_ATOM_END) FAIL("symbol %d: atom %u is not predefined", k, s->hash_next);
    fprintf(f, "static const ROM_STR_T(%u) js_rom_sym_%d = { .s = { .header = { JS_IMMORTAL_REF_COUNT }, .len = %u, .hash = %u,\n"
               "    .kind = JS_STRING_KIND_NORMAL, .atom_type = JS_ATOM_TYPE_SYMBOL, .hash_next = %u }, .c = ",
            s->len + 1, k, s->len, s->hash, s->hash_next);
    put_cstr(f, str8(s), s->len);
    fprintf(f, " };\n");
}

static void emit(const char *path, const char *defs_path)
{
    JSContext *ctx = G_ctx;
    JSRuntime *rt = ctx->rt;
    /* symbols numbered in the order the graph meets them, so it is stable */
    for (int k = 0; k < nobjs; k++) {
        JSObject *p = objs[k];
        for (int i = 0; i < p->shape->prop_count; i++)
            if (p->shape->prop[i].atom != JS_ATOM_NULL &&
                (p->shape->prop[i].flags & JS_PROP_TMASK) == JS_PROP_NORMAL &&
                JS_VALUE_GET_TAG(p->prop[i].u.value) == JS_TAG_SYMBOL)
                sym_index(JS_VALUE_GET_PTR(p->prop[i].u.value));
    }
    for (int si = 0; si < nshapes; si++)
        for (int k = 0; k < nobjs; k++)
            if (objs[k]->shape == shapes[si]) { shape_owner[si] = k; break; }
    int total = 0;
    for (int k = 0; k < nobjs; k++) {
        int idx[1024];
        prop_off[k] = total;
        total += kept_props(objs[k], idx);
    }

    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "/* Generated by tools/vmtest/floor/gen_rom_intrinsics.sh -- do not edit.\n"
               " * The hardened intrinsics as const data (docs/vm/rom-intrinsics.md R2).\n"
               " * Included at the end of quickjs.c; regenerate after any change to the\n"
               " * builtins, the ROM atom table or the structs it writes. */\n\n");
    fprintf(f, "_Static_assert(JS_ROM_ATOM_END == %u, \"quickjs-rom-intrinsics.h is stale (ROM atoms)\");\n", (unsigned)JS_ROM_ATOM_END);
    fprintf(f, "_Static_assert(JS_ATOM_END == %u, \"quickjs-rom-intrinsics.h is stale (atoms)\");\n", (unsigned)JS_ATOM_END);
    fprintf(f, "_Static_assert(JS_CLASS_INIT_COUNT == %u, \"quickjs-rom-intrinsics.h is stale (classes)\");\n\n", (unsigned)JS_CLASS_INIT_COUNT);

    fprintf(f, "/* strings: %d */\n", nstrs);
    for (int k = 0; k < nstrs; k++) emit_string(f, k);
    fprintf(f, "\n/* well-known symbols: %d */\n", nsyms);
    for (int k = 0; k < nsyms; k++) emit_symbol(f, k);
    fprintf(f, "\nstatic const JSRomSymbol js_rom_symbols[JS_ROM_SYMBOL_COUNT] = {\n");
    for (int k = 0; k < nsyms; k++)
        fprintf(f, "    { %u, (JSString *)&js_rom_sym_%d.s },\n", syms[k]->hash_next, k);
    fprintf(f, "};\n\n");

    /* C function data records */
    for (int k = 0; k < nobjs; k++) {
        if (objs[k]->class_id != JS_CLASS_C_FUNCTION_DATA) continue;
        JSCFunctionDataRecord *r = objs[k]->u.c_function_data_record;
        fprintf(f, "static const ROM_CFD_T(%u) js_rom_cfd_%d = { .func = %s, .length = %u, .data_len = %u, .magic = %u, .data = {",
                r->data_len ? r->data_len : 1, k, fn_name((void *)r->func), r->length, r->data_len, r->magic);
        for (int i = 0; i < r->data_len; i++) { fprintf(f, " "); put_value(f, "ROM_PV", r->data[i]); fprintf(f, ","); }
        fprintf(f, " } };\nROM_CFD_CHECK(js_rom_cfd_%d);\n", k);
    }
    fprintf(f, "\n/* shapes: %d */\n", nshapes);
    for (int si = 0; si < nshapes; si++) emit_shape(f, si);

    fprintf(f, "/* properties: %d */\nstatic const JSProperty js_rom_props[%d] = {\n", total, total ? total : 1);
    for (int k = 0; k < nobjs; k++) {
        JSObject *p = objs[k];
        int idx[1024], n = kept_props(p, idx);
        for (int j = 0; j < n; j++) {
            JSShapeProperty *prs = &p->shape->prop[idx[j]];
            JSProperty *pr = &p->prop[idx[j]];
            fprintf(f, "    ");
            if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
                fprintf(f, "{ .u.getset = { ");
                if (pr->u.getset.getter) fprintf(f, "(JSObject *)&js_rom_obj[%d], ", obj_index(pr->u.getset.getter)); else fprintf(f, "NULL, ");
                if (pr->u.getset.setter) fprintf(f, "(JSObject *)&js_rom_obj[%d] } }", obj_index(pr->u.getset.setter)); else fprintf(f, "NULL } }");
            } else {
                put_value(f, "ROM_PV", pr->u.value);
            }
            char buf[64];
            fprintf(f, ", /* %d.%s */\n", k, JS_AtomGetStr(ctx, buf, sizeof buf, prs->atom));
        }
    }
    fprintf(f, "};\n\n");

    fprintf(f, "/* objects: %d */\nstatic const JSObject js_rom_obj[JS_ROM_OBJ_COUNT] = {\n", nobjs);
    for (int k = 0; k < nobjs; k++) emit_object(f, k);
    fprintf(f, "};\n\n");

    /* wiring: the context's intrinsic fields */
    fprintf(f, "static void js_rom_wire(JSContext *ctx)\n{\n");
#define WIRE(fld) do { fprintf(f, "    ctx->" #fld " = "); put_value(f, "ROM_JSV", ctx->fld); fprintf(f, ";\n"); } while (0);
    CTX_FIELDS(WIRE)
#undef WIRE
    for (int i = 0; i < JS_NATIVE_ERROR_COUNT; i++) {
        fprintf(f, "    ctx->native_error_proto[%d] = ", i);
        put_value(f, "ROM_JSV", ctx->native_error_proto[i]);
        fprintf(f, ";\n");
    }
    for (int i = 0; i < rt->class_count; i++) {
        if (JS_IsNull(ctx->class_proto[i])) continue;
        fprintf(f, "    ctx->class_proto[%d] = ", i);
        put_value(f, "ROM_JSV", ctx->class_proto[i]);
        fprintf(f, ";\n");
    }
    fprintf(f, "}\n\n");

    /* the global object's properties, in order; globalThis is the object
       itself, which ROM cannot name */
    JSObject *g = JS_VALUE_GET_OBJ(ctx->global_obj);
    int ng = 0;
    fprintf(f, "static const JSRomGlobal js_rom_global[JS_ROM_GLOBAL_COUNT] = {\n");
    for (int i = 0; i < g->shape->prop_count; i++) {
        JSShapeProperty *prs = &g->shape->prop[i];
        if (prs->atom == JS_ATOM_NULL || is_ram_global(prs->atom)) continue;
        JSValueConst v = g->prop[i].u.value;
        char buf[64];
        if ((prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL) { FAIL("global %d: not plain data", i); continue; }
        bool self = JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(v) == g;
        if (!__JS_AtomIsConst(prs->atom)) FAIL("global %d: atom %u has no fixed number", i, prs->atom);
        fprintf(f, "    { %u, 0x%02x, %d, ", prs->atom, prs->flags, self);
        if (self) fprintf(f, "ROM_PV_INT(JS_TAG_UNDEFINED, 0)"); else put_value(f, "ROM_PV", v);
        fprintf(f, " }, /* %s */\n", JS_AtomGetStr(ctx, buf, sizeof buf, prs->atom));
        ng++;
    }
    fprintf(f, "};\n");
    fclose(f);

    FILE *d = fopen(defs_path, "w");
    if (!d) { perror(defs_path); exit(1); }
    fprintf(d, "/* Generated by tools/vmtest/floor/gen_rom_intrinsics.sh -- do not edit. */\n"
               "#define JS_ROM_OBJ_COUNT %du\n#define JS_ROM_SYMBOL_COUNT %du\n#define JS_ROM_GLOBAL_COUNT %du\n",
            nobjs, nsyms, ng);
    fclose(d);
    fprintf(stderr, "rom intrinsics: objects %d, shapes %d, properties %d, strings %d, symbols %d, globals %d\n",
            nobjs, nshapes, total, nstrs, nsyms, ng);
}

int main(int argc, char **argv)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    G_ctx = ctx;
    if (!ctx || JS_PocketHarden(ctx) < 0 || js_immortal_resolve(ctx) < 0) {
        fprintf(stderr, "gen_rom_intrinsics: context setup failed\n");
        return 1;
    }
    JS_RunGC(rt);
    collect(ctx);
    if (argc < 3) {
        stats();
        return fails ? 1 : 0;
    }
    load_syms(argv[2]);
    char defs[1024];
    snprintf(defs, sizeof defs, "%.*s-defs.h", (int)(strlen(argv[1]) - 2), argv[1]);
    emit(argv[1], defs);
    if (fails)
        fprintf(stderr, "gen_rom_intrinsics: %d problem(s); the output is not usable\n", fails);
    return fails ? 1 : 0;
}
