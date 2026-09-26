// What the startup floor would be with (1) builtin names in a flash table
// instead of heap JSStrings and (2) builtin objects keeping their list-derived
// properties (autoinit methods, getters/setters) in the flash function lists
// until first use. Computed on the device layout (-m32 -malign-double) with
// the device guest's accounting (tlsf block length: round up to 4, min 12).
//
// Compiled with quickjs.c included so the heap can be walked from inside.
#include "quickjs.c"
#include <stdio.h>

typedef struct { size_t usable; } mhdr_t;
static size_t total, blocks;
static size_t tl(size_t n) { n = (n + 3) & ~(size_t)3; return n < 12 ? 12 : n; }
static void *m_malloc(void *o, size_t n) {
    (void)o; if (!n) return NULL;
    size_t u = tl(n); mhdr_t *h = malloc(sizeof(mhdr_t) + u);
    if (!h) return NULL; h->usable = u; total += u; blocks++; return h + 1;
}
static void m_free(void *o, void *p) {
    (void)o; if (!p) return; mhdr_t *h = (mhdr_t *)p - 1;
    total -= h->usable; blocks--; free(h);
}
static void *m_calloc(void *o, size_t c, size_t n) {
    void *p = m_malloc(o, c * n); if (p) memset(p, 0, c * n); return p;
}
static size_t m_usable(const void *p) { return p ? ((const mhdr_t *)p - 1)->usable : 0; }
static void *m_realloc(void *o, void *p, size_t n) {
    if (!p) return m_malloc(o, n);
    if (!n) { m_free(o, p); return NULL; }
    void *q = m_malloc(o, n); if (!q) return NULL;
    size_t old = m_usable(p); memcpy(q, p, old < n ? old : n); m_free(o, p); return q;
}
static const JSMallocFunctions MF = {m_calloc, m_malloc, m_free, m_realloc, m_usable};

// Shape + prop array an object ends up with after `n` properties are added one
// by one from the initial shape: resize_properties' own growth rule.
static void grown(uint32_t n, size_t *shape_b, size_t *prop_b) {
    uint32_t size = JS_PROP_INITIAL_SIZE, hash = JS_PROP_INITIAL_HASH_SIZE;
    for (uint32_t c = 1; c <= n; c++) {
        if (c > size) {
            uint32_t ns = max_int(c, size * 3 / 2);
            while (hash < ns) hash *= 2;
            size = ns;
        }
    }
    *shape_b = tl(get_shape_size(hash, size));
    *prop_b = tl(sizeof(JSProperty) * size);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    size_t t0 = total;
    JSRuntime *rt = JS_NewRuntime2(&MF, NULL);
    JSContext *ctx = JS_NewContext(rt);
    size_t floor_js = total - t0;

    // --- atoms: every string atom that exists now is a builtin name.
    size_t atom_str = 0, atom_n = 0, sym_n = 0;
    for (int i = 1; i < rt->atom_size; i++) {
        JSAtomStruct *p = rt->atom_array[i];
        if (atom_is_free(p)) continue;
        if (p->atom_type == JS_ATOM_TYPE_STRING) {
            atom_str += tl(sizeof(JSString) + (p->len << p->is_wide_char) + 1 - p->is_wide_char);
            atom_n++;
        } else sym_n++;
    }
    size_t atom_tables = tl(sizeof(rt->atom_array[0]) * rt->atom_size) +
                         tl(sizeof(rt->atom_hash[0]) * rt->atom_hash_size);

    // --- objects and shapes.
    size_t obj_b = 0, prop_now = 0, shape_now = 0, prop_lazy = 0, shape_lazy = 0;
    size_t n_obj = 0, n_auto = 0, n_getset = 0, n_plain = 0, getter_objs = 0, other_gc = 0;
    struct list_head *el;
    list_for_each(el, &rt->gc_obj_list) {
        JSGCObjectHeader *g = list_entry(el, JSGCObjectHeader, link);
        if (g->gc_obj_type == JS_GC_OBJ_TYPE_SHAPE) continue;   // counted via objects
        if (g->gc_obj_type != JS_GC_OBJ_TYPE_JS_OBJECT) { other_gc++; continue; }
        JSObject *p = (JSObject *)g;
        JSShape *sh = p->shape;
        n_obj++;
        obj_b += tl(sizeof(JSObject));
        prop_now += tl(sizeof(JSProperty) * sh->prop_size);
        // A shared shape is paid once; charge it to the object only when it
        // is the shape's sole user (ref_count 1). Shared shapes are the plain
        // function/array shapes and are kept as they are.
        bool own_shape = sh->header.ref_count == 1;
        if (own_shape)
            shape_now += tl(get_shape_size(sh->prop_hash_mask + 1, sh->prop_size));
        uint32_t keep = 0;
        JSShapeProperty *pr = sh->prop;
        for (int i = 0; i < sh->prop_count; i++, pr++) {
            if (pr->atom == JS_ATOM_NULL) continue;
            int t = pr->flags & JS_PROP_TMASK;
            if (t == JS_PROP_AUTOINIT) n_auto++;
            else if (t == JS_PROP_GETSET) n_getset++;
            else { n_plain++; keep++; }
        }
        size_t sb, pb;
        grown(keep, &sb, &pb);
        prop_lazy += pb;
        if (own_shape) shape_lazy += sb;
    }
    // Getter/setter function objects exist only because of GETSET entries:
    // count objects that are C functions reachable solely as accessors is
    // hard from here, so estimate them from the accessor pairs themselves.
    list_for_each(el, &rt->gc_obj_list) {
        JSGCObjectHeader *g = list_entry(el, JSGCObjectHeader, link);
        if (g->gc_obj_type != JS_GC_OBJ_TYPE_JS_OBJECT) continue;
        JSObject *p = (JSObject *)g;
        JSShapeProperty *pr = p->shape->prop;
        for (int i = 0; i < p->shape->prop_count; i++, pr++) {
            if (pr->atom == JS_ATOM_NULL || (pr->flags & JS_PROP_TMASK) != JS_PROP_GETSET) continue;
            JSProperty *v = &p->prop[i];
            getter_objs += (v->u.getset.getter != NULL) + (v->u.getset.setter != NULL);
        }
    }
    // One accessor function: object + its own prop array (length, name).
    size_t fn_b = tl(sizeof(JSObject)) + tl(sizeof(JSProperty) * 2);

    size_t walked = atom_str + atom_tables + obj_b + prop_now + shape_now;
    printf("floor_js=%zu blocks=%zu\n", floor_js, blocks);
    printf("walked: atoms_str=%zu (n=%zu, symbols kept %zu) atom_tables=%zu objects=%zu (n=%zu)"
           " prop_arrays=%zu own_shapes=%zu -> sum=%zu (%.0f%% of floor; rest = shared shapes,"
           " other gc objects n=%zu, misc)\n",
           atom_str, atom_n, sym_n, atom_tables, obj_b, n_obj, prop_now, shape_now, walked,
           100.0 * walked / floor_js, other_gc);
    printf("props: autoinit=%zu getset=%zu plain=%zu; accessor fn objects=%zu (~%zu B each)\n",
           n_auto, n_getset, n_plain, getter_objs, fn_b);
    size_t save_atoms = atom_str;
    size_t save_slots = (prop_now - prop_lazy) + (shape_now - shape_lazy);
    size_t save_fn = getter_objs * fn_b;
    printf("SAVE flash_atoms=%zu lazy_slots=%zu lazy_accessor_fns=%zu total=%zu\n",
           save_atoms, save_slots, save_fn, save_atoms + save_slots + save_fn);
    printf("FLOOR now=%zu after=%zu\n", floor_js, floor_js - save_atoms - save_slots - save_fn);
    return 0;
}
