"""Apply one deliberate F2 fault to a copy of quickjs.c (the negative
controls of docs/vm/builtin-floor-plan.md sec.8, N2a-N2c). Each removes one
piece of the lazy-builtins handling; the corpus has to notice every one.
    python3 f2_faults.py FAULT path/to/copy/quickjs.c
"""
import sys

FAULTS = {
    # N2a: full materialization keeps touch order instead of definition order.
    "no-reorder": ("static int lazy_reorder(JSContext *ctx, JSObject *p)\n{\n",
                   "static int lazy_reorder(JSContext *ctx, JSObject *p)\n{\n    if (1) return 0;\n"),
    # delete of a pending entry is not remembered: it comes back.
    "no-delete-mark": ("static int js_lazy_delete(JSContext *ctx, JSObject *p, JSAtom atom)\n{\n",
                       "static int js_lazy_delete(JSContext *ctx, JSObject *p, JSAtom atom)\n{\n    if (1) return 2;\n"),
    # An assignment walking up the chain ignores pending setters/read-only.
    "no-proto-set-hook": ("        if (!prs && unlikely(js_obj_lazy(p1))) {\n            int r = js_lazy_touch(ctx, p1, prop);\n            if (r < 0)\n                goto fail;\n            if (r)\n                goto retry2;",
                          "        if (0) {\n            int r = js_lazy_touch(ctx, p1, prop);\n            if (r < 0)\n                goto fail;\n            if (r)\n                goto retry2;"),
    # The interpreter's inline get_field loop does not hand lazy objects over.
    "no-fastpath-check": ("                        if (unlikely(p->is_exotic || js_obj_lazy(p))) {\n                            /* XXX: should avoid the slow path for arrays\n                               and typed arrays by ensuring that 'prop' is\n                               not numeric */\n                            obj = JS_MKPTR(JS_TAG_OBJECT, p);\n                            goto get_field_slow_path;",
                          "                        if (unlikely(p->is_exotic)) {\n                            /* XXX: should avoid the slow path for arrays\n                               and typed arrays by ensuring that 'prop' is\n                               not numeric */\n                            obj = JS_MKPTR(JS_TAG_OBJECT, p);\n                            goto get_field_slow_path;"),
    # N2b: a touched entry is not remembered as done, so full materialization
    # defines it a second time.
    "no-done-mark": ("            /* Marked first: add_property below looks the name up again. */\n            lazy_set_done(l, k);\n",
                     "            /* Marked first: add_property below looks the name up again. */\n"),
}

name, path = sys.argv[1], sys.argv[2]
old, new = FAULTS[name]
src = open(path, encoding="utf-8").read()
assert src.count(old) == 1, f"{name}: anchor found {src.count(old)} times"
open(path, "w", encoding="utf-8").write(src.replace(old, new))
print(f"applied {name}")
