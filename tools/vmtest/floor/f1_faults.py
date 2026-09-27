"""Apply one deliberate F1 fault to a copy of quickjs.c (the negative
controls of docs/vm/builtin-floor-plan.md sec.8). Each fault removes one piece
of the ROM-atom handling; the corpus has to notice every one of them.
    python3 f1_faults.py FAULT path/to/copy/quickjs.c
"""
import sys

FAULTS = {
    # The name lookup no longer consults flash: a builtin name typed or built
    # at run time becomes a second, heap atom, and properties keyed on the
    # two never meet.
    "no-rom-find": ("if (atom_type == JS_ATOM_TYPE_STRING) {\n            i = js_rom_find(",
                    "if (0) {\n            i = js_rom_find("),
    # One of the 33 sites forgets its flash branch: atom -> string value
    # dereferences the NULL slot.
    "no-value-branch": ("            if (r) {\n                return js_rom_atom_value(ctx, atom, r);",
                        "            if (0) {\n                return js_rom_atom_value(ctx, atom, r);"),
    # The baked CanonicalNumericIndexString answer is ignored ("Infinity").
    "no-numeric-flag": ("if (!(r->hash_flags & JS_ROM_NUMERIC)) {", "if (1) {"),
}

name, path = sys.argv[1], sys.argv[2]
old, new = FAULTS[name]
src = open(path, encoding="utf-8").read()
assert src.count(old) == 1, f"{name}: anchor found {src.count(old)} times"
open(path, "w", encoding="utf-8").write(src.replace(old, new))
print(f"applied {name}")
