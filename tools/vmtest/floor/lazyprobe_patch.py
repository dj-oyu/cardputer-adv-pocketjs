"""Write a counting copy of quickjs.c (argv[1]) to argv[2] (the repo file is
not touched; lazyprobe.sh puts the copy under .cache/vmtest-floor). find_own_property() reports every own-property lookup; the
appended code keeps the set of objects that existed right after
JS_NewContext (the builtins) and counts, per builtin object:
  miss   -- the lookup failed there (the lazy scheme would search its table)
  first  -- first hit on a list-derived property (AUTOINIT/GETSET): the lazy
            scheme's one-time materialization
"""
import pathlib, re, sys
src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
decl = "static void lp_own(JSObject *p, JSAtom atom, JSShapeProperty *pr);\n"
anchor = "static inline JSShapeProperty *find_own_property(JSProperty **ppr,"
assert src.count(anchor) == 1
src = src.replace(anchor, decl + anchor)
body_old = """            *ppr = &p->prop[h - 1];
            /* the compiler should be able to assume that pr != NULL here */
            return pr;
        }
        h = pr->hash_next;
    }
    *ppr = NULL;
    return NULL;
}"""
body_new = """            *ppr = &p->prop[h - 1];
            lp_own(p, atom, pr);
            /* the compiler should be able to assume that pr != NULL here */
            return pr;
        }
        h = pr->hash_next;
    }
    *ppr = NULL;
    lp_own(p, atom, NULL);
    return NULL;
}"""
assert src.count(body_old) >= 1
src = src.replace(body_old, body_new, 1)
src += r'''
/* ---- lazyprobe ---- */
#define LP_CAP 8192
static JSObject *lp_obj[LP_CAP];
static int lp_on;
unsigned long lp_miss, lp_first, lp_hit;
static struct { JSObject *p; JSAtom a; } lp_seen[LP_CAP];
static int lp_seen_n;
static unsigned lp_h(const void *p) { return (unsigned)(((uintptr_t)p >> 4) * 2654435761u) % LP_CAP; }
static int lp_builtin(JSObject *p) {
    for (unsigned i = lp_h(p), n = 0; n < LP_CAP; i = (i + 1) % LP_CAP, n++) {
        if (lp_obj[i] == p) return 1;
        if (!lp_obj[i]) return 0;
    }
    return 0;
}
void lp_mark(JSRuntime *rt) {
    struct list_head *el;
    memset(lp_obj, 0, sizeof lp_obj);
    lp_seen_n = 0; lp_miss = lp_first = lp_hit = 0;
    list_for_each(el, &rt->gc_obj_list) {
        JSGCObjectHeader *g = list_entry(el, JSGCObjectHeader, link);
        if (g->gc_obj_type != JS_GC_OBJ_TYPE_JS_OBJECT) continue;
        unsigned i = lp_h(g);
        while (lp_obj[i]) i = (i + 1) % LP_CAP;
        lp_obj[i] = (JSObject *)g;
    }
}
void lp_enable(int on) { lp_on = on; }
static void lp_own(JSObject *p, JSAtom atom, JSShapeProperty *pr) {
    if (!lp_on || !lp_builtin(p)) return;
    if (!pr) { lp_miss++; return; }
    int t = pr->flags & JS_PROP_TMASK;
    if (t != JS_PROP_AUTOINIT && t != JS_PROP_GETSET) { lp_hit++; return; }
    for (int i = 0; i < lp_seen_n; i++)
        if (lp_seen[i].p == p && lp_seen[i].a == atom) { lp_hit++; return; }
    if (lp_seen_n < LP_CAP) { lp_seen[lp_seen_n].p = p; lp_seen[lp_seen_n].a = atom; lp_seen_n++; }
    lp_first++;
}
'''
out = pathlib.Path(sys.argv[2])
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(src, encoding="utf-8")
print("patched", out)
