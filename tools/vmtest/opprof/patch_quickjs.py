"""Write a profiling copy of the vendored quickjs.c (opprof_impl.c).

    python3 patch_quickjs.py OUT_DIR

OUT_DIR/quickjs.c is components/quickjs-ng/quickjs-ng/quickjs.c with its
hooks, each behind `if (unlikely(opprof_on))`, and opprof_impl.c included at
the end. The vendored file is never edited: the hooks exist only in this
copy, which only tools/vmtest/opprof/ builds. Each anchor must match exactly
as often as expected, so a change upstream of an anchor stops the build instead of silently
dropping a hook.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
SRC = ROOT / "components/quickjs-ng/quickjs-ng/quickjs.c"
IMPL = pathlib.Path(__file__).resolve().parent / "opprof_impl.c"

DECL = """
/* opprof (tools/vmtest/opprof): hooks; the profiler is #included at the end. */
extern int opprof_on;
struct JSFunctionBytecode; struct JSStackFrame;
static void opprof_hit(JSRuntime *rt, struct JSFunctionBytecode *b, struct JSStackFrame *sf, const uint8_t *pc, JSValue *sp);
static void opprof_ccall(JSContext *ctx, JSValueConst func_obj);
static void opprof_alloc(int kind, size_t size);
static void opprof_gc_hit(void);
"""

# (anchor, replacement[, count]); each anchor must occur exactly count times (1).
EDITS = [
    ("static JSVMState *js_vm_armed;\n", "static JSVMState *js_vm_armed;\n" + DECL),
    # The dispatch (DIRECT_DISPATCH, gcc): every SWITCH(pc) and so every
    # BREAK passes here. Not DUMP_BYTECODE_OR_DONT: which of its two
    # definitions applies depends on NDEBUG (ENABLE_DUMPS).
    ("#define SWITCH(pc)      DUMP_BYTECODE_OR_DONT(pc) __extension__ ({",
     "#define SWITCH(pc)      DUMP_BYTECODE_OR_DONT(pc) if (unlikely(opprof_on)) opprof_hit(rt, b, sf, pc, sp); __extension__ ({"),
    ("    p = JS_VALUE_GET_OBJ(func_obj);\n    cproto = p->u.cfunc.cproto;\n",
     "    p = JS_VALUE_GET_OBJ(func_obj);\n    if (unlikely(opprof_on)) opprof_ccall(ctx, func_obj);\n    cproto = p->u.cfunc.cproto;\n"),
    ("    s = JS_GetOpaque(func_obj, JS_CLASS_C_FUNCTION_DATA);\n    if (!s) {\n        return JS_EXCEPTION;    // can't really happen\n    }\n",
     "    s = JS_GetOpaque(func_obj, JS_CLASS_C_FUNCTION_DATA);\n    if (!s) {\n        return JS_EXCEPTION;    // can't really happen\n    }\n"
     "    if (unlikely(opprof_on)) opprof_ccall(ctx, func_obj);\n"),
    ("    s->malloc_count++;\n    s->malloc_size += rt->mf.js_malloc_usable_size(ptr) + MALLOC_OVERHEAD;\n    return ptr;\n}\n",
     "    s->malloc_count++;\n    s->malloc_size += rt->mf.js_malloc_usable_size(ptr) + MALLOC_OVERHEAD;\n"
     "    if (unlikely(opprof_on)) opprof_alloc(0, rt->mf.js_malloc_usable_size(ptr));\n    return ptr;\n}\n", 2),  # calloc, malloc
    ("    s->malloc_count--;\n    s->malloc_size -= free_size;\n",
     "    s->malloc_count--;\n    s->malloc_size -= free_size;\n    if (unlikely(opprof_on)) opprof_alloc(2, 0);\n"),
    ("void *js_realloc_rt(JSRuntime *rt, void *ptr, size_t size)\n{\n",
     "void *js_realloc_rt(JSRuntime *rt, void *ptr, size_t size)\n{\n    if (unlikely(opprof_on) && ptr && size) opprof_alloc(1, size);\n"),
    ("#endif\n    if (force_gc) {\n", "#endif\n    if (force_gc) {\n        if (unlikely(opprof_on)) opprof_gc_hit();\n"),
]


def main() -> None:
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    text = SRC.read_text(encoding="utf-8")
    for anchor, repl, *want in EDITS:
        n = text.count(anchor)
        if n != (want[0] if want else 1):
            raise SystemExit(f"patch_quickjs: anchor found {n} times:\n{anchor}")
        text = text.replace(anchor, repl)
    text += f'\n#include "{IMPL.as_posix()}"\n'
    (out / "quickjs.c").write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
