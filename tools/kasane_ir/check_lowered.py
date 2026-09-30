"""DERBY WATCH with its plans written as JS functions and lowered at build
time, against the shipped apps/derby (host, WSL only).

  python3 tools/kasane_ir/check_lowered.py

1. tools/kasane_ir/migrate_derby.mjs turns a copy of apps/derby into the
   source form with @plan functions (tools/kasane_ir/plans_js/derby_plans.js);
2. tools/kasane_ir/lower_plans.mjs lowers that copy as the firmware build
   would (plans nibble-packed, prog() the decoder);
3. tools/games/test_derby_host.c (-m32) runs the scripted game at LIGHT, MID
   and HEAVY on both apps/derby and the lowered copy. The per-frame
   statistics (segments, raster steps, draws, live plans...; not the VM
   steps and instruction counts, which compiling shortens) and the finish
   lines must be identical, both must pass the harness's own checks (every
   draw through the plan, debug-step and single-step VM oracle, every frame
   compared pixel for pixel), and the guest heap after evaluation is printed;
4. no garbage only the cycle collector frees (the device runs it only near
   the guest heap limit, so a cycle made per plan or frame stays resident:
   pack.mjs's first decoder called itself, ~4 KB a race, LOADSTALL): the
   harness's per-scene "freed only by the cycle collector" must be 0 in
   every scene of the lowered game (the line comes with vm/derby-trim's
   test_derby_host.c; without it the check fails), and 20 rounds of every
   plan's registration (spec() through the decoder, heap_probe.c) must leave
   0 B for the cycle collector.
apps/ is not changed.
"""
from __future__ import annotations

import csv
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import derby_heap  # noqa: E402

ROOT, CACHE, run_derby = derby_heap.ROOT, derby_heap.CACHE, derby_heap.run_derby


def game(binary: Path, apps: Path, tier: str, extra: dict | None = None) -> tuple[str, str]:
    with tempfile.TemporaryDirectory() as t:
        root = Path(t)
        shutil.copytree(apps, root / "apps/derby")
        csv = root / "frames.csv"
        r = subprocess.run([str(binary)], cwd=root, capture_output=True, text=True,
                           env=dict(os.environ, DERBY_TIER=tier, DERBY_CSV=str(csv), **(extra or {})))
        if extra:
            return r.stdout, ""
        if "DERBY_HOST PASS" not in r.stdout:
            raise SystemExit(f"{apps}: host game failed at tier {tier}:\n{r.stdout[-3000:]}\n{r.stderr[-2000:]}")
        return r.stdout, csv.read_text()


CYCLE = re.compile(r"^guest heap churn in frame\(\), (\w+): .*freed only by the cycle collector (\d+) B$", re.M)


def cycles(out: str) -> dict[str, int]:
    return {m.group(1): int(m.group(2)) for m in CYCLE.finditer(out)}


def registration_cycles(apps: Path) -> int:
    """Bytes the cycle collector alone frees over 20 rounds of spec() for
    every plan of apps/derby_prog.js (the decoder and what it builds)."""
    probe = derby_heap.PROBE.split("const report")[0] + """
let cyc = 0;
for (let r = 0; r < 20; ++r) for (const k of Object.keys(T).concat(['r0'])) {
  if (k === 'runner') continue;
  let p = spec(k); p = null; const a = __heap(); __gc(); cyc += a - __heap();
}
__print('CYCLES ' + cyc);
"""
    derby_heap.probe(apps)                 # builds heap_probe
    script = CACHE / "m32/cycles.js"
    script.write_text(derby_heap.PRELUDE + (apps / "derby_prog.js").read_text(encoding="utf-8")
                      .replace("'use strict';", "") + probe, encoding="utf-8")
    out = subprocess.run([str(CACHE / "m32/heap_probe"), str(script)], check=True, capture_output=True, text=True).stdout
    return int(re.search(r"CYCLES (\d+)", out).group(1))


# The compiled plans are shorter than the hand IR, so these may differ; every
# other column (segments, raster steps, draws, live plans, points) may not.
STEPS = ("frame_steps", "draw_steps_max", "instr_max")


def compare(ca: str, cb: str) -> tuple[bool, int, int]:
    a, b = list(csv.DictReader(io.StringIO(ca))), list(csv.DictReader(io.StringIO(cb)))
    same = len(a) == len(b) and all({k: v for k, v in x.items() if k not in STEPS} ==
                                    {k: v for k, v in y.items() if k not in STEPS} for x, y in zip(a, b))
    return same, sum(int(x["frame_steps"]) for x in a), sum(int(y["frame_steps"]) for y in b)


def main() -> None:
    work = CACHE / "lowered_check"
    shutil.rmtree(work, ignore_errors=True)
    future, lowered = work / "future", work / "lowered"
    subprocess.run(["node", str(HERE / "migrate_derby.mjs"), str(ROOT / "apps/derby"),
                    str(HERE / "plans_js/derby_plans.js"), str(future)], check=True)
    subprocess.run(["node", str(HERE / "lower_plans.mjs"), str(future), str(lowered)], check=True)
    binary = run_derby.build(derby_heap.m32_flags(), run_derby.CACHE / "m32")
    ok = True
    for tier in ("0", "1", "2"):
        a, ca = game(binary, ROOT / "apps/derby", tier)
        b, cb = game(binary, lowered, tier)
        fa = re.findall(r"^finish: .*$", a, re.M)
        fb = re.findall(r"^finish: .*$", b, re.M)
        same, sa, sb = compare(ca, cb)
        cyc = cycles(b)
        ok &= same and fa == fb and bool(cyc) and not any(cyc.values())
        print(f"tier {tier}: lowered game, freed only by the cycle collector: " +
              (", ".join(f"{k} {v} B" for k, v in cyc.items()) if cyc else
               "NOT REPORTED (test_derby_host.c without vm/derby-trim's cycle count)"))
        print(f"tier {tier}: {len(ca.splitlines()) - 1} frames, per-frame drawing statistics "
              f"{'identical' if same else 'DIFFER'}, finish {'identical' if fa == fb else 'DIFFERS'}, "
              f"VM steps {sa} -> {sb} ({100 * (sb - sa) / sa:+.1f}%)")
    for label, apps in (("apps/derby", ROOT / "apps/derby"), ("lowered", lowered)):
        out, _ = game(binary, apps, "1", {"DERBY_EVAL_ONLY": "1"})
        m = re.search(r"peak during eval (\d+).*after eval (\d+)", out)
        print(f"{label:<10} guest heap after evaluation {m.group(2)} B, evaluation peak {m.group(1)} B (host m32)")
    reg = registration_cycles(lowered)
    ok &= reg == 0
    print(f"registration x20 of every plan through the lowered decoder: freed only by the cycle collector {reg} B")
    print("LOWERED PASS" if ok else "LOWERED FAIL")
    if not ok:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
