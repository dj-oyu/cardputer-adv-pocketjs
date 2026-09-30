"""DERBY WATCH's compiled plans against the hand IR on the real VM (host,
WSL only; node on PATH, so run it under bash -lc).

  python3 tools/kasane_ir/check_equivalence.py [--prog DIR] [--js PLANS.js] [--reuse]

1. Captures what the game registers and draws: tools/games/test_derby_host.c
   (-m32) runs the scripted game at LIGHT, MID and HEAVY on a lowered copy
   of apps/derby (or DIR/apps/derby; tools/games/run_derby.py lower())
   whose view logs each registration's arguments (REG) and each draw's
   inputs (DRAW, in dr()). The copy is temporary; apps/ is not changed.
2. node tools/kasane_ir/derby_plans.mjs compiles the plans (the @plan
   functions of apps/derby/derby_prog.js, or --js PLANS.js; without either,
   tools/kasane_ir/plans/*.kjs) and writes the cases: per plan and argument
   set, the captured inputs (up to 400) and 300 perturbed ones (every tenth
   wild, to reach failures). The hand IR is tools/kasane_ir/derby_hand_ir.js
   (DERBY's plans as text until e9b88bc), or DIR's derby_prog_text.js or
   derby_prog.js when those are text.
3. tools/kasane_ir/run_ir.c runs hand and compiled IR through
   main/ui/kasane/ksn_procedural.c (and the compiled one also through
   ksn_proc_plan.c) and compares status, segments and raster steps
   (EQUIVALENCE PASS).
4. With plans from JS functions, tools/kasane_ir/check_js.mjs also runs
   those functions on every vector (float32 per operation, and as written
   in double) against what the VM drew from their compiled IR (JS REFERENCE
   PASS).
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import derby_heap  # noqa: E402

ROOT, CACHE = derby_heap.ROOT, derby_heap.CACHE
run_derby = derby_heap.run_derby


def text_prog(d: Path) -> Path:
    """The hand IR: DIR's plans as text, else the frozen copy."""
    t = d / "derby_prog_text.js"
    if t.exists():
        return t
    p = d / "derby_prog.js"
    return HERE / "derby_hand_ir.js" if "@plan" in p.read_text(encoding="utf-8") else p


def js_plans(d: Path) -> Path | None:
    p = d / "derby_prog.js"
    return p if "@plan" in p.read_text(encoding="utf-8") else None


def instrument(root: Path) -> None:
    d = root / "apps/derby"
    view = (d / "derby_view.js").read_text(encoding="utf-8")
    # Built-in plans (the decoder marked rom): load() registers p[0] with
    # the arguments p[1]. Before those, prog() made the rows and is patched
    # below to record its arguments.
    rom = "H.register('derby.' + p[0], p[1], p[2]);\n"
    if rom in view:
        view = view.replace(rom, rom + "    __log('REG ' + n + ' ' + JSON.stringify(p[1] || []));\n")
    else:
        m = re.search(r"^    else p = \[prog\(T\[n\], k\.concat\(1 / k\[2\].*\n", view, flags=re.M)
        assert m, "derby_view.js: registration line moved"
        anchor = m[0]
        view = view.replace(anchor, anchor + "    __log('REG ' + n + ' ' + JSON.stringify(globalThis.__arg || []));\n")
    # Every draw goes through dr() (derby_view.js), as it is made.
    anchor = "const dr = (n, a) => { if (live[n]) H.draw(live[n], a); };"
    assert anchor in view, "derby_view.js: draw line moved"
    view = view.replace(anchor, "const dr = (n, a) => { if (live[n]) { __log('DRAW ' + n + ' ' + a.join(',')); "
                                "H.draw(live[n], a); } };")
    (d / "derby_view.js").write_text(view, encoding="utf-8")
    # prog() as the app runs it (the decoder, or the text assembler of an
    # older DIR) records its arguments for the REG line.
    if rom in view:
        return
    prog, n = re.subn(r"^function prog\((\w+), (\w+)\) \{\n", r"\g<0>  globalThis.__arg = \2;\n",
                      (d / "derby_prog.js").read_text(encoding="utf-8"), flags=re.M)
    assert n == 1, "derby_prog.js: prog() not found"
    (d / "derby_prog.js").write_text(prog, encoding="utf-8")


def capture(apps: Path) -> Path:
    binary = run_derby.build(derby_heap.m32_flags(), run_derby.CACHE / "m32")
    CACHE.mkdir(parents=True, exist_ok=True)
    out = CACHE / "draws.txt"
    with tempfile.TemporaryDirectory() as t, out.open("w") as log:
        root = Path(t)
        run_derby.lower(apps, root / "apps/derby")
        instrument(root)
        for tier in ("0", "1", "2"):
            r = subprocess.run([str(binary)], cwd=root, capture_output=True, text=True,
                               env=dict(os.environ, DERBY_TIER=tier))
            if "DERBY_HOST PASS" not in r.stdout:
                raise SystemExit(f"host game failed at tier {tier}:\n{r.stdout[-3000:]}\n{r.stderr[-2000:]}")
            log.write("\n".join(line for line in r.stdout.splitlines() if " REG " in line or " DRAW " in line) + "\n")
    return out


def main() -> None:
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--prog", help="DIR whose apps/derby is measured instead of this tree's")
    ap.add_argument("--js", help="plans as JS functions (plan_js.mjs): compiled from here, and the VM's "
                                 "output compared with the functions run by check_js.mjs")
    ap.add_argument("--reuse", action="store_true", help="skip the capture, reuse .cache/kasane_ir/draws.txt")
    a = ap.parse_args()
    apps = Path(a.prog) / "apps/derby" if a.prog else ROOT / "apps/derby"
    js = a.js or js_plans(apps)
    draws = CACHE / "draws.txt" if a.reuse else capture(apps)
    cases = CACHE / "cases.txt"
    subprocess.run(["node", str(HERE / "derby_plans.mjs"), "--prog", str(text_prog(apps)),
                    *(["--js", str(js)] if js else []), "--cases", str(draws), str(cases)], check=True)
    binary = CACHE / "run_ir"
    subprocess.run(["gcc", "-std=gnu11", "-O1", "-g", "-Wall", "-Imain/ui/kasane", "tools/kasane_ir/run_ir.c",
                    "main/ui/kasane/ksn_procedural.c", "main/ui/kasane/ksn_proc_plan.c",
                    "main/ui/kasane/ksn_proc_analysis.c",
                    "-lm", "-o", str(binary)], cwd=ROOT, check=True)
    dump = CACHE / "dump.txt"
    with cases.open() as f:
        r = subprocess.run([str(binary)], stdin=f, capture_output=True, text=True,
                           env=dict(os.environ, RUN_IR_DUMP=str(dump)))
    print(r.stdout, end="")
    if r.returncode:
        raise SystemExit(r.stderr or "equivalence failed")
    if js:
        subprocess.run(["node", str(HERE / "check_js.mjs"), str(js), str(CACHE / "cases.json"), str(dump)], check=True)


if __name__ == "__main__":
    main()
