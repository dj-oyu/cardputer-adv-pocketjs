"""Apply one deliberate F3b fault to a copy of quickjs.c (the negative
controls of docs/vm/builtin-floor-plan.md sec.17). Each removes one piece of
the lazy typed-array handling; the corpus has to notice every one.
    python3 f3_faults.py FAULT path/to/copy/quickjs.c
"""
import sys

FAULTS = {
    # A native path makes the class beside the pending global binding instead
    # of resolving it: the global and prototype.constructor become two objects.
    "no-resolve-binding": ("            return JS_AutoInitProperty(ctx, g, atom, pr, prs);\n",
                           "            ;\n"),
    # The native path reads class_proto without the guard: a null prototype.
    "no-ctor-guard": ("        if (LAZY_CLASS_MISSING(ctx, class_id)) { return JS_EXCEPTION; } proto = js_dup(ctx->class_proto[class_id]);\n",
                      "        proto = js_dup(ctx->class_proto[class_id]);\n"),
    # %TypedArray%.prototype.toString is not Array.prototype.toString.
    "no-tostring-alias": ("    ret = JS_DefinePropertyValue(ctx, proto, JS_ATOM_toString, obj,\n                                 JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);\n    JS_FreeValue(ctx, proto);\n    if (ret < 0)\n        return -1;\n    if (js_lazy_bind(ctx, LAZY_G_TA))",
                          "    JS_FreeValue(ctx, obj); ret = 0;\n    JS_FreeValue(ctx, proto);\n    if (ret < 0)\n        return -1;\n    if (js_lazy_bind(ctx, LAZY_G_TA))"),
    # F3c: JS_NewObjectClass reads class_proto unguarded, so a Map/Set
    # iterator gets a null prototype (they have no binding to resolve).
    "no-newobjectclass-guard": ("    if (LAZY_CLASS_MISSING(ctx, class_id)) { return JS_EXCEPTION; } return JS_NewObjectProtoClass(ctx, ctx->class_proto[class_id], class_id);\n",
                                "    return JS_NewObjectProtoClass(ctx, ctx->class_proto[class_id], class_id);\n"),
    # The pending bindings are enumerable, unlike the constructors they stand for.
    "binding-enumerable": ("#define LAZY_E(name, cid, group) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE,",
                           "#define LAZY_E(name, cid, group) { name, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE,"),
}

name, path = sys.argv[1], sys.argv[2]
old, new = FAULTS[name]
src = open(path, encoding="utf-8").read()
assert src.count(old) == 1, f"{name}: anchor found {src.count(old)} times"
open(path, "w", encoding="utf-8").write(src.replace(old, new))
print(f"applied {name}")
