// Where the context's bytes are, by category, on the device layout and the
// device guest's accounting (F2 follow-up: which part did not shrink?).
// Built like floor32.c; quickjs.c is included so rt->lazy can be read too.
#include "quickjs.c"
#include <stdio.h>

typedef struct { size_t usable; } mh_t;
static size_t total;
static size_t tl(size_t n) { n = (n + 3) & ~(size_t)3; return n < 12 ? 12 : n; }
static void *m_malloc(void *o, size_t n) {
    (void)o; if (!n) return NULL;
    size_t u = tl(n); mh_t *h = malloc(sizeof(mh_t) + u);
    if (!h) return NULL; h->usable = u; total += u; return h + 1;
}
static void m_free(void *o, void *p) { (void)o; if (!p) return; mh_t *h = (mh_t *)p - 1; total -= h->usable; free(h); }
static void *m_calloc(void *o, size_t c, size_t n) { void *p = m_malloc(o, c * n); if (p) memset(p, 0, c * n); return p; }
static size_t m_usable(const void *p) { return p ? ((const mh_t *)p - 1)->usable : 0; }
static void *m_realloc(void *o, void *p, size_t n) {
    if (!p) return m_malloc(o, n);
    if (!n) { m_free(o, p); return NULL; }
    void *q = m_malloc(o, n); if (!q) return NULL;
    size_t old = m_usable(p); memcpy(q, p, old < n ? old : n); m_free(o, p); return q;
}
static const JSMallocFunctions MF = {m_calloc, m_malloc, m_free, m_realloc, m_usable};

int main(void) {
    JSRuntime *rt = JS_NewRuntime2(&MF, NULL);
    size_t t0 = total;
    JSContext *ctx = JS_NewContext(rt);
    size_t ctxb = total - t0;
    size_t objb = 0, propb = 0, shb = 0, shn = 0, shp = 0, shh = 0, other = 0;
    struct list_head *el;
    list_for_each(el, &rt->gc_obj_list) {
        JSGCObjectHeader *g = list_entry(el, JSGCObjectHeader, link);
        if (g->gc_obj_type == JS_GC_OBJ_TYPE_JS_OBJECT) {
            JSObject *p = (JSObject *)g;
            objb += tl(sizeof(JSObject));
            propb += tl(sizeof(JSProperty) * p->shape->prop_size);
        } else if (g->gc_obj_type == JS_GC_OBJ_TYPE_SHAPE) {
            JSShape *sh = (JSShape *)g;
            shn++;
            shb += tl(get_shape_size(sh->prop_hash_mask + 1, sh->prop_size));
            shp += sh->prop_size;
            shh += sh->prop_hash_mask + 1;
        } else other++;
    }
    size_t lazyb = 0;
#ifdef CONFIG_POCKET_VM_LAZY_BUILTINS
    lazyb = rt->lazy ? tl(sizeof(rt->lazy[0]) * rt->lazy_size) : 0;
    printf("lazy lists=%u (array %u slots, %zu B)\n", rt->lazy_count, rt->lazy_size, lazyb);
#endif
    printf("context js=%zu: objects %zu, prop arrays %zu, shapes %zu (n=%zu, prop slots %zu, hash slots %zu), lazy %zu, rest %zu (other gc %zu)\n",
           ctxb, objb, propb, shb, shn, shp, shh, lazyb, ctxb - objb - propb - shb - lazyb, other);
    // The biggest object shapes: whose are they, and are they still lazy?
    struct { size_t b; int cls, n, size, hash, lazy, refs; } top[16] = {0};
    list_for_each(el, &rt->gc_obj_list) {
        JSGCObjectHeader *g = list_entry(el, JSGCObjectHeader, link);
        if (g->gc_obj_type != JS_GC_OBJ_TYPE_JS_OBJECT) continue;
        JSObject *p = (JSObject *)g;
        JSShape *sh = p->shape;
        size_t b = tl(get_shape_size(sh->prop_hash_mask + 1, sh->prop_size)) +
                   tl(sizeof(JSProperty) * sh->prop_size);
        for (int i = 0; i < 16; i++) {
            if (b > top[i].b) {
                memmove(&top[i + 1], &top[i], sizeof(top[0]) * (15 - i));
                top[i].b = b; top[i].cls = p->class_id; top[i].n = sh->prop_count;
                top[i].size = sh->prop_size; top[i].hash = sh->prop_hash_mask + 1;
                top[i].lazy = js_obj_lazy(p); top[i].refs = sh->header.ref_count;
                break;
            }
        }
    }
    for (int i = 0; i < 16 && top[i].b; i++)
        printf("  %5zu B class=%d props=%d size=%d hash=%d lazy=%d shape_refs=%d\n",
               top[i].b, top[i].cls, top[i].n, top[i].size, top[i].hash, top[i].lazy, top[i].refs);
    return 0;
}
