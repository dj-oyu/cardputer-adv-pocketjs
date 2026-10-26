"""Apply one deliberate R1 fault to a copy of quickjs.c (the negative
controls of docs/vm/rom-intrinsics.md sec.9 R1, N-R1a). Each removes one
piece of the immortal handling; vmrun --harden --immortal has to notice.
    python3 r1_faults.py FAULT path/to/copy/quickjs.c

Not here: the guards whose only effect in RAM is a harmless store (js_dup,
gc_scan_incref_child*, is_prototype, extensible, tmp_mark, the weak list).
A RAM immortal survives those; only a read-only snapshot (R2) can show them.
"""
import sys

FAULTS = {
    # The upstream free: an immortal's count goes 0 and it is freed.
    "free-upstream": (
        "        int rc = p->ref_count - 1;\n        if (unlikely(rc <= 0)) {\n            if (rc == 0) {\n                p->ref_count = 0;\n                js_free_value_rt(rt, v);\n            }\n            return;\n        }\n        p->ref_count = rc;\n",
        "        if (--p->ref_count <= 0)\n            js_free_value_rt(rt, v);\n"),
    # The cycle collector counts an immortal child like any other.
    "gc-decref": (
        "static void gc_decref_child(JSRuntime *rt, JSGCObjectHeader *p)\n{\n    if (js_rc_immortal(p->ref_count))\n        return;\n",
        "static void gc_decref_child(JSRuntime *rt, JSGCObjectHeader *p)\n{\n"),
    # A realm-free C function runs with the NULL realm it holds.
    "no-realm-fallback": (
        "    return realm ? realm : caller;\n",
        "    return realm;\n"),
}

name, path = sys.argv[1], sys.argv[2]
old, new = FAULTS[name]
src = open(path, encoding="utf-8").read()
assert src.count(old) == 1, f"{name}: anchor found {src.count(old)} times"
open(path, "w", encoding="utf-8").write(src.replace(old, new))
print(f"applied {name}")
