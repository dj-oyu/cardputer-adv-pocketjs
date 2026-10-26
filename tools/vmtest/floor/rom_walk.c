// What a fresh context costs after F1-F3, what an app's touched builtins add
// on top, and what the whole intrinsic graph would weigh fully materialized
// (= the flash cost of a ROM snapshot). docs/vm/rom-intrinsics.md sec.2.
// Built like floor_parts.c: quickjs.c is included so rt->lazy, the shapes
// and the GC list can be walked; the allocator charges the device guest's
// tlsf block length (4 B rounding, 12 B minimum; js= on the device adds
// MALLOC_OVERHEAD 8 B per block on top, reported as `blocks`).
//
//   gcc -std=gnu11 -O2 $F -DQUICKJS_NG_BUILD -D_GNU_SOURCE -DQJS_DISABLE_DUMPS \
//     -I .cache/vmtest32/include -I components/pocketjs_guest/include -I $QJS \
//     tools/vmtest/floor/rom_walk.c components/pocketjs_guest/src/vm_sched.c \
//     components/pocketjs_guest/src/vm_clock.c $obj/libregexp.o $obj/libunicode.o \
//     $obj/dtoa.o $obj/quickjs-vm.o -lm -o .cache/vmtest32/rom_walk
//   .cache/vmtest32/rom_walk [touch-script.js]
#include "quickjs.c"
#include <stdio.h>

typedef struct { size_t usable; } mh_t;
static size_t total, nblocks;
static size_t tl(size_t n) { n = (n + 3) & ~(size_t)3; return n < 12 ? 12 : n; }
static void *m_malloc(void *o, size_t n) {
    (void)o; if (!n) return NULL;
    size_t u = tl(n); mh_t *h = malloc(sizeof(mh_t) + u);
    if (!h) return NULL; h->usable = u; total += u; nblocks++; return h + 1;
}
static void m_free(void *o, void *p) { (void)o; if (!p) return; mh_t *h = (mh_t *)p - 1; total -= h->usable; nblocks--; free(h); }
static void *m_calloc(void *o, size_t c, size_t n) { void *p = m_malloc(o, c * n); if (p) memset(p, 0, c * n); return p; }
static size_t m_usable(const void *p) { return p ? ((const mh_t *)p - 1)->usable : 0; }
static void *m_realloc(void *o, void *p, size_t n) {
    if (!p) return m_malloc(o, n);
    if (!n) { m_free(o, p); return NULL; }
    void *q = m_malloc(o, n); if (!q) return NULL;
    size_t old = m_usable(p); memcpy(q, p, old < n ? old : n); m_free(o, p); return q;
}
static const JSMallocFunctions MF = {m_calloc, m_malloc, m_free, m_realloc, m_usable};

/* One pass over the GC list. `seen` marks objects that existed at the
   previous pass so growth can be attributed. */
typedef struct {
    size_t obj_n, obj_b, prop_b, sh_own_n, sh_own_b, sh_shared_n, sh_shared_b;
    size_t cfunc_n, cfunc_b;      /* JS_CLASS_C_FUNCTION objects (header + props) */
    size_t cfunc_sh_b;            /* shapes only C functions use */
    size_t other_n;
    size_t raw_obj_b, raw_prop_b, raw_sh_b; /* without tlsf rounding: flash cost */
    size_t str_n, str_b;          /* distinct JSString values held by properties */
    JSString *seen[1024]; size_t seen_n;
} walk_t;

static size_t shape_bytes(JSShape *sh) { return get_shape_size(sh->prop_hash_mask + 1, sh->prop_size); }
/* Flash cost of a shape that would hold exactly prop_count entries with a
   hash table sized for them (the snapshot generator can right-size). */
static size_t shape_raw(JSShape *sh) {
    int n = sh->prop_count, hs = JS_PROP_INITIAL_HASH_SIZE;
    while (hs < n) hs <<= 1;
    return get_shape_size(hs, n);
}

static void walk(JSRuntime *rt, walk_t *w, JSObject *skip1, JSObject *skip2) {
    struct list_head *el;
    memset(w, 0, sizeof *w);
    /* count each shape once */
    list_for_each(el, &rt->gc_obj_list) {
        JSGCObjectHeader *g = list_entry(el, JSGCObjectHeader, link);
        if (g->gc_obj_type == JS_GC_OBJ_TYPE_SHAPE) {
            JSShape *sh = (JSShape *)g;
            if (sh->header.ref_count == 1) { w->sh_own_n++; w->sh_own_b += tl(shape_bytes(sh)); }
            else { w->sh_shared_n++; w->sh_shared_b += tl(shape_bytes(sh)); }
            w->raw_sh_b += shape_raw(sh);
        }
    }
    list_for_each(el, &rt->gc_obj_list) {
        JSGCObjectHeader *g = list_entry(el, JSGCObjectHeader, link);
        if (g->gc_obj_type != JS_GC_OBJ_TYPE_JS_OBJECT) { if (g->gc_obj_type != JS_GC_OBJ_TYPE_SHAPE && g->gc_obj_type != JS_GC_OBJ_TYPE_JS_CONTEXT) w->other_n++; continue; }
        JSObject *p = (JSObject *)g;
        if (p == skip1 || p == skip2) continue;
        w->obj_n++; w->obj_b += tl(sizeof(JSObject)); w->raw_obj_b += sizeof(JSObject);
        w->prop_b += tl(sizeof(JSProperty) * p->shape->prop_size);
        w->raw_prop_b += sizeof(JSProperty) * p->shape->prop_count;
        if (p->class_id == JS_CLASS_C_FUNCTION || p->class_id == JS_CLASS_C_FUNCTION_DATA) {
            w->cfunc_n++;
            w->cfunc_b += tl(sizeof(JSObject)) + tl(sizeof(JSProperty) * p->shape->prop_size);
        }
        JSShapeProperty *prs = p->shape->prop;
        for (int i = 0; i < p->shape->prop_count; i++, prs++) {
            if (prs->atom == JS_ATOM_NULL || (prs->flags & JS_PROP_TMASK)) continue;
            JSValue v = p->prop[i].u.value;
            if (JS_VALUE_GET_TAG(v) == JS_TAG_STRING) {
                JSString *s = JS_VALUE_GET_STRING(v);
                /* count a string once: function names escape through F1's
                   rom_cache, so many `name` slots share one JSString */
                int dup = 0;
                for (size_t k = 0; k < w->seen_n; k++) if (w->seen[k] == s) { dup = 1; break; }
                if (dup) continue;
                if (w->seen_n < countof(w->seen)) w->seen[w->seen_n++] = s;
                w->str_n++; w->str_b += tl(sizeof(JSString) + (s->len << s->is_wide_char) + 1 - s->is_wide_char);
            }
        }
    }
}

static void show(const char *name, walk_t *w) {
    printf("%-22s obj %3zu (%5zu B) props %5zu B | shapes own %3zu (%5zu B) shared %3zu (%5zu B) | cfunc %3zu (%5zu B) | strings %3zu (%4zu B) | raw obj+prop+shape %zu B\n",
           name, w->obj_n, w->obj_b, w->prop_b, w->sh_own_n, w->sh_own_b, w->sh_shared_n, w->sh_shared_b,
           w->cfunc_n, w->cfunc_b, w->str_n, w->str_b, w->raw_obj_b + w->raw_prop_b + w->raw_sh_b);
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1); fread(b, 1, n, f); b[n] = 0; fclose(f); *len = n; return b;
}

static int materialize_all(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    /* F3 groups: read every lazy global binding so the classes exist */
    for (size_t i = 0; i < countof(js_lazy_entries); i++) {
        JSValue v = JS_GetPropertyStr(ctx, ctx->global_obj, js_lazy_entries[i].name);
        if (JS_IsException(v)) return -1;
        JS_FreeValue(ctx, v);
    }
    /* F2 lists: expand until no lazy object remains (expanding one may
       register new lazy objects, e.g. iterator prototypes) */
    for (int round = 0; round < 8; round++) {
        uint32_t n = 0; JSObject *objs[512];
        for (uint32_t i = 0; i < rt->lazy_count && n < 512; i++)
            if (n == 0 || objs[n - 1] != rt->lazy[i].obj) objs[n++] = rt->lazy[i].obj;
        if (!n) break;
        for (uint32_t i = 0; i < n; i++)
            if (js_lazy_all(ctx, objs[i], false) < 0) return -1;
    }
    return 0;
}

/* Resolve every AUTOINIT slot (F2's full expansion only puts the entries
   in the shape as AUTOINIT; the function objects are made on first read) so
   the graph is what a ROM snapshot would hold. JS_GetOwnProperty resolves
   AUTOINIT without calling getters. Returns the number of bytecode-defined
   builtins made (Array.fromAsync, Iterator.zip...), which cannot be ROM. */
static int resolve_all(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    for (int round = 0; round < 4; round++) {
        JSObject *objs[1024]; int n = 0;
        struct list_head *el;
        list_for_each(el, &rt->gc_obj_list) {
            JSGCObjectHeader *g = list_entry(el, JSGCObjectHeader, link);
            if (g->gc_obj_type == JS_GC_OBJ_TYPE_JS_OBJECT && n < 1024) objs[n++] = (JSObject *)g;
        }
        for (int i = 0; i < n; i++) {
            JSValue o = JS_MKPTR(JS_TAG_OBJECT, objs[i]);
            JSPropertyEnum *tab; uint32_t len;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, o, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK)) return -1;
            for (uint32_t k = 0; k < len; k++) {
                JSPropertyDescriptor d;
                int r = JS_GetOwnProperty(ctx, &d, o, tab[k].atom);
                if (r < 0) return -1;
                if (r) js_free_desc(ctx, &d);
            }
            JS_FreePropertyEnum(ctx, tab, len);
        }
    }
    int bc = 0; struct list_head *el;
    list_for_each(el, &rt->gc_obj_list) {
        JSGCObjectHeader *g = list_entry(el, JSGCObjectHeader, link);
        if (g->gc_obj_type == JS_GC_OBJ_TYPE_JS_OBJECT && ((JSObject *)g)->class_id == JS_CLASS_BYTECODE_FUNCTION) bc++;
    }
    return bc;
}

int main(int argc, char **argv) {
    JSRuntime *rt = JS_NewRuntime2(&MF, NULL);
    size_t rt_b = total, rt_blocks = nblocks;
    JSContext *ctx = JS_NewContext(rt);
    size_t ctx_b = total - rt_b;
    printf("sizeof: JSValue=%zu ptr=%zu JSObject=%zu JSShape=%zu JSShapeProperty=%zu JSProperty=%zu JSString=%zu JSContext=%zu class_count=%d\n",
           sizeof(JSValue), sizeof(void *), sizeof(JSObject), sizeof(JSShape), sizeof(JSShapeProperty), sizeof(JSProperty), sizeof(JSString), sizeof(JSContext), rt->class_count);
    printf("runtime js=%zu blocks=%zu | context js=%zu blocks=%zu | total %zu\n", rt_b, rt_blocks, ctx_b, nblocks - rt_blocks, total);
    JSObject *g = JS_VALUE_GET_OBJ(ctx->global_obj), *gv = JS_VALUE_GET_OBJ(ctx->global_var_obj);
    walk_t w0, wg;
    walk(rt, &w0, g, gv);
    show("fresh (no global)", &w0);
    walk(rt, &wg, NULL, NULL);
    printf("  global_obj: shape %zu B props %zu B (%d props) | global_var_obj %zu B\n",
           tl(shape_bytes(g->shape)), tl(sizeof(JSProperty) * g->shape->prop_size), g->shape->prop_count,
           tl(sizeof(JSObject)) + tl(shape_bytes(gv->shape)) + tl(sizeof(JSProperty) * gv->shape->prop_size));
    printf("  lazy table %zu B (%u lists, %u slots) | class_proto %zu B | JSContext %zu B | shape_hash %zu B | atom_array %zu B atom_hash %zu B\n",
           rt->lazy ? tl(sizeof(rt->lazy[0]) * rt->lazy_size) : 0, rt->lazy_count, rt->lazy_size,
           tl(sizeof(JSValue) * rt->class_count), tl(sizeof(JSContext)), tl(sizeof(JSShape *) * rt->shape_hash_size),
           tl(sizeof(JSAtomStruct *) * rt->atom_size), tl(sizeof(uint32_t) * rt->atom_hash_size));
    size_t acc = w0.obj_b + w0.prop_b + w0.sh_own_b + w0.sh_shared_b + tl(sizeof(JSObject)) * 2 + tl(shape_bytes(g->shape)) + tl(sizeof(JSProperty) * g->shape->prop_size) + tl(shape_bytes(gv->shape)) + tl(sizeof(JSProperty) * gv->shape->prop_size) + (rt->lazy ? tl(sizeof(rt->lazy[0]) * rt->lazy_size) : 0) + tl(sizeof(JSValue) * rt->class_count) + tl(sizeof(JSContext)) + w0.str_b;
    printf("  accounted %zu of context %zu (rest %zu)\n", acc, ctx_b, ctx_b - acc);

    if (argc > 1) {
        size_t len; char *src = slurp(argv[1], &len);
        if (!src) { fprintf(stderr, "no %s\n", argv[1]); return 1; }
        size_t before = total, bb = nblocks;
        JSValue r = JS_Eval(ctx, src, len, argv[1], JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) { JSValue e = JS_GetException(ctx); const char *s = JS_ToCString(ctx, e); fprintf(stderr, "eval failed: %s\n", s); return 1; }
        JS_FreeValue(ctx, r);
        JS_RunGC(rt);
        walk_t w1; walk(rt, &w1, g, gv);
        printf("TOUCH %s: js +%zu B (+%zu blocks)\n", argv[1], total - before, nblocks - bb);
        show("after touch", &w1);
        printf("  delta: obj +%zu (+%zu B) props +%zu B shapes +%zu B cfunc +%zu (+%zu B) strings +%zu (+%zu B)\n",
               w1.obj_n - w0.obj_n, w1.obj_b - w0.obj_b, w1.prop_b - w0.prop_b,
               w1.sh_own_b + w1.sh_shared_b - w0.sh_own_b - w0.sh_shared_b,
               w1.cfunc_n - w0.cfunc_n, w1.cfunc_b - w0.cfunc_b, w1.str_n - w0.str_n, w1.str_b - w0.str_b);
        return 0;
    }

    /* Everything: the fully materialized intrinsic graph. */
    size_t before = total;
    if (materialize_all(ctx)) { fprintf(stderr, "materialize failed\n"); return 1; }
    JS_RunGC(rt);
    walk_t w2; walk(rt, &w2, g, gv);
    printf("FULL materialization: js +%zu B -> context %zu B; lazy lists left %u\n", total - before, total - rt_b, rt->lazy_count);
    show("full (no global)", &w2);
    /* class histogram */
    int hist[JS_CLASS_INIT_COUNT + 8] = {0};
    struct list_head *el;
    list_for_each(el, &rt->gc_obj_list) {
        JSGCObjectHeader *gh = list_entry(el, JSGCObjectHeader, link);
        if (gh->gc_obj_type == JS_GC_OBJ_TYPE_JS_OBJECT) { JSObject *p = (JSObject *)gh; if (p->class_id < countof(hist)) hist[p->class_id]++; }
    }
    size_t b2 = total;
    int bc = resolve_all(ctx);
    if (bc < 0) { fprintf(stderr, "resolve failed\n"); return 1; }
    JS_RunGC(rt);
    walk_t w3; walk(rt, &w3, g, gv);
    printf("FULL + AUTOINIT resolved: js +%zu B -> context %zu B (%zu blocks); bytecode builtins %d\n", total - b2, total - rt_b, nblocks - rt_blocks, bc);
    show("resolved (no global)", &w3);
    size_t autoinit = 0; { struct list_head *el2; list_for_each(el2, &rt->gc_obj_list) { JSGCObjectHeader *gh = list_entry(el2, JSGCObjectHeader, link); if (gh->gc_obj_type != JS_GC_OBJ_TYPE_JS_OBJECT) continue; JSObject *p = (JSObject *)gh; for (int i = 0; i < p->shape->prop_count; i++) if ((p->shape->prop[i].flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) autoinit++; } }
    printf("  AUTOINIT slots left %zu\n", autoinit);
    printf("  classes:");
    for (size_t i = 0; i < countof(hist); i++) if (hist[i]) printf(" %zu:%d", i, hist[i]);
    printf("\n");
    return 0;
}
