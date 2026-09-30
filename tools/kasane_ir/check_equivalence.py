"""The compiled DERBY WATCH plans against the hand IR on the real VM (host,
WSL only).

  python3 tools/kasane_ir/check_equivalence.py [--prog DIR]

1. Captures what the game registers and draws: tools/games/test_derby_host.c
   (-m32) runs the scripted game at LIGHT, MID and HEAVY on a copy of
   apps/derby (or DIR/apps/derby) whose view logs each registration's
   arguments (REG) and whose scene logs each draw's inputs (DRAW). The copy is
   temporary; apps/ is not changed.
2. node tools/kasane_ir/derby_plans.mjs compiles tools/kasane_ir/plans/*.kjs
   and writes the cases: per plan and argument set, the captured inputs
   (up to 400) and 300 perturbed ones (every tenth wild, to reach failures).
3. tools/kasane_ir/run_ir.c runs hand and compiled IR through
   main/ui/kasane/ksn_procedural.c (and the compiled one also through
   ksn_proc_plan.c) and compares status, segments and raster steps.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import derby_heap  # noqa: E402

ROOT, CACHE = derby_heap.ROOT, derby_heap.CACHE
run_derby = derby_heap.run_derby


def instrument(root: Path) -> None:
    d = root / "apps/derby"
    view = (d / "derby_view.js").read_text(encoding="utf-8")
    anchor = "    else p = [prog(T[n], k.concat(1 / k[2]))];\n"
    assert anchor in view, "derby_view.js: registration line moved"
    view = view.replace(anchor, anchor + "    __log('REG ' + n + ' ' + JSON.stringify(globalThis.__arg || []));\n")
    (d / "derby_view.js").write_text(view, encoding="utf-8")
    prog = (d / "derby_prog.js").read_text(encoding="utf-8")
    anchor = "function prog(src, arg) {\n"
    assert anchor in prog
    (d / "derby_prog.js").write_text(prog.replace(anchor, anchor + "  globalThis.__arg = arg;\n"), encoding="utf-8")
    scene = (d / "derby_scene.js").read_text(encoding="utf-8")
    anchor = "if (live[e[0]]) H.draw(live[e[0]], e[1]);"
    assert anchor in scene, "derby_scene.js: draw line moved"
    scene = scene.replace(anchor, "if (live[e[0]]) { __log('DRAW ' + e[0] + ' ' + e[1].join(',')); H.draw(live[e[0]], e[1]); }")
    (d / "derby_scene.js").write_text(scene, encoding="utf-8")


def capture(apps: Path) -> Path:
    binary = run_derby.build(derby_heap.m32_flags(), run_derby.CACHE / "m32")
    CACHE.mkdir(parents=True, exist_ok=True)
    out = CACHE / "draws.txt"
    with tempfile.TemporaryDirectory() as t, out.open("w") as log:
        root = Path(t)
        shutil.copytree(apps, root / "apps/derby")
        instrument(root)
        for tier in ("0", "1", "2"):
            r = subprocess.run([str(binary)], cwd=root, capture_output=True, text=True,
                               env=dict(os.environ, DERBY_TIER=tier))
            if "DERBY_HOST PASS" not in r.stdout:
                raise SystemExit(f"host game failed at tier {tier}:\n{r.stdout[-3000:]}\n{r.stderr[-2000:]}")
            log.write("\n".join(line for line in r.stdout.splitlines() if " REG " in line or " DRAW " in line) + "\n")
    return out


def main() -> None:
    apps = Path(sys.argv[2]) / "apps/derby" if sys.argv[1:2] == ["--prog"] else ROOT / "apps/derby"
    draws = capture(apps)
    cases = CACHE / "cases.txt"
    subprocess.run(["node", str(HERE / "derby_plans.mjs"), "--prog", str(apps / "derby_prog.js"),
                    "--cases", str(draws), str(cases)], check=True)
    binary = CACHE / "run_ir"
    subprocess.run(["gcc", "-std=gnu11", "-O1", "-g", "-Wall", "-Imain/ui/kasane", "tools/kasane_ir/run_ir.c",
                    "main/ui/kasane/ksn_procedural.c", "main/ui/kasane/ksn_proc_plan.c",
                    "main/ui/kasane/ksn_proc_analysis.c",
                    "-lm", "-o", str(binary)], cwd=ROOT, check=True)
    with cases.open() as f:
        r = subprocess.run([str(binary)], stdin=f, capture_output=True, text=True)
    print(r.stdout, end="")
    if r.returncode:
        raise SystemExit(r.stderr or "equivalence failed")


if __name__ == "__main__":
    main()
