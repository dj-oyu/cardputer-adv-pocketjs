"""What built-in (flash) plans would save DERBY WATCH (host, WSL only; node on
PATH, so run it under bash -lc). docs/kasane/flash-plan.md section 6.

  python3 tools/kasane_ir/flash_plan_estimate.py

1. Native: the scripted game (tools/games/test_derby_host.c -m32) runs at
   LIGHT, MID and HEAVY on a lowered copy of apps/derby whose load() and
   drop() log each plan registered and dropped (name, instruction count).
   Replaying that log gives the live set at every moment: the bytes of the
   sized plans the firmware allocates today (40 + 12n on the device) and of
   the flash form (40 + 4 * declared arguments), each also rounded as the
   device's TLSF rounds a block (plan-sized-alloc.md sec.2) plus its 4 B
   header. The sizes are computed, the live sets measured.
2. Guest: the same game evaluated (DERBY_EVAL_ONLY) with derby_prog.js as the
   firmware ships it (packed plans and their decoder) and as lower_plans.mjs
   --ids derby writes it (each plan the string 'derby.name', no decoder): the
   guest heap after evaluation and the evaluation's peak (host m32, TLSF
   charge). Only derby_prog.js differs; derby_view.js is not migrated (its
   load() still calls prog(), which evaluation never reaches).
apps/ is not changed: both runs use temporary copies.
"""
from __future__ import annotations

import json
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


def tlsf(request: int) -> int:
    """The device's block for a request (plan-sized-alloc.md sec.2): 4 B steps
    below 128, then 8/16/32 B steps; plus the 4 B block header."""
    step = 4 if request < 128 else 8 if request < 256 else 16 if request < 512 else 32
    return (request + step - 1) // step * step + 4


def instrument(root: Path) -> None:
    d = root / "apps/derby"
    view = (d / "derby_view.js").read_text(encoding="utf-8")
    reg = "    live[n] = p[1] ? H.register(p[0], p[1]) : H.register(p[0]);\n"
    assert reg in view, "derby_view.js: registration line moved"
    view = view.replace(reg, reg + "    __log('PLAN+ ' + n + ' ' + p[0].length);\n")
    unreg = "    if (live[n]) H.unregister(live[n]);\n"
    assert unreg in view, "derby_view.js: unregister line moved"
    view = view.replace(unreg, "    if (live[n]) { __log('PLAN- ' + n); H.unregister(live[n]); }\n")
    (d / "derby_view.js").write_text(view, encoding="utf-8")


def params_by_count(apps: Path) -> dict[int, tuple[str, int]]:
    """Declared arguments per plan, keyed by instruction count (unique in
    DERBY): the log names 'r3' or 'g2', the table 'runner' or 'nil'."""
    with tempfile.TemporaryDirectory() as t:
        out = Path(t) / "rom.c"
        j = Path(t) / "rom.json"
        subprocess.run(["node", str(HERE / "emit_rom_plans.mjs"), str(out), f"derby={apps / 'derby_prog.js'}",
                        "--json", str(j)], check=True, capture_output=True)
        entries = json.loads(j.read_text())
    by = {}
    for e in entries:
        assert e["count"] not in by, "two plans of one length: key the log by name instead"
        by[e["count"]] = (e["name"], e["params"])
    return by


def native(binary: Path, apps: Path) -> None:
    by = params_by_count(apps)
    with tempfile.TemporaryDirectory() as t:
        root = Path(t)
        run_derby.lower(apps, root / "apps/derby")
        instrument(root)
        print("==== native: the live plans (host game, measured) and their bytes (computed for the device)")
        print("tier   live max   sized plans      flash plans      saved         at the sized peak: live, instructions")
        for tier, label in (("0", "LIGHT"), ("1", "MID"), ("2", "HEAVY")):
            r = subprocess.run([str(binary)], cwd=root, capture_output=True, text=True,
                               env=dict(os.environ, DERBY_TIER=tier))
            if "DERBY_HOST PASS" not in r.stdout:
                raise SystemExit(f"host game failed at tier {tier}:\n{r.stdout[-3000:]}\n{r.stderr[-2000:]}")
            live: dict[str, int] = {}
            peak = {"n": 0, "sized": (0, 0), "flash": (0, 0), "at": None}
            for line in r.stdout.splitlines():
                m = re.search(r"PLAN([+-]) (\S+)(?: (\d+))?", line)
                if not m:
                    continue
                if m[1] == "+":
                    live[m[2]] = int(m[3])
                else:
                    live.pop(m[2], None)
                n = [c for c in live.values()]
                sized = (sum(40 + 12 * c for c in n), sum(tlsf(40 + 12 * c) for c in n))
                flash = (sum(40 + 4 * by[c][1] for c in n), sum(tlsf(40 + 4 * by[c][1]) for c in n))
                peak["n"] = max(peak["n"], len(n))
                if sized[1] > peak["sized"][1]:
                    peak["sized"], peak["at"] = sized, (len(n), sum(n), flash)
                if flash[1] > peak["flash"][1]:
                    peak["flash"] = flash
            s, f = peak["sized"], peak["at"][2]
            print(f"{label:<6} {peak['n']:8d}   {s[0]:6d} / {s[1]:6d}  {f[0]:6d} / {f[1]:6d}  "
                  f"{s[0] - f[0]:6d} / {s[1] - f[1]:6d}   {peak['at'][0]} plans, {peak['at'][1]} instructions")
        print("  (request / with TLSF rounding and header; flash is the same live set at the sized peak)")


def guest(binary: Path, apps: Path) -> None:
    print("==== guest: evaluation (host m32, TLSF charge, measured)")
    base = None
    forms = (("shipped (packed + decoder)", None), ("ids ('derby.name', no decoder)", "ids"),
             ("no T (load() names 'derby.' + n)", "noT"))
    for label, ids in forms:
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            run_derby.lower(apps, root / "apps/derby")
            if ids:
                out = root / "ids"
                subprocess.run(["node", str(HERE / "lower_plans.mjs"), "--ids", "derby", str(apps), str(out)],
                               check=True, capture_output=True)
                text = (out / "derby_prog.js").read_text(encoding="utf-8")
                if ids == "noT":
                    # The upper bound: T itself gone, as if load() built the
                    # id from the plan's name (r0..r7 -> runner, g0..g5 and
                    # hd -> nil). Only T's own lines go; evaluation never
                    # reads T.
                    text, n = re.subn(r"^const T = \{\n(?:.*\n)*?\};\n", "", text, flags=re.M)
                    assert n == 1, "derby_prog.js: T moved"
                (root / "apps/derby/derby_prog.js").write_text(text, encoding="utf-8")
            text = (root / "apps/derby/derby_prog.js").read_text(encoding="utf-8")
            r = subprocess.run([str(binary), "apps/derby/derby_watch.js"], cwd=root, capture_output=True, text=True,
                               env=dict(os.environ, DERBY_EVAL_ONLY="1"))
            if r.returncode:
                raise SystemExit(f"eval failed:\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
            after = int(re.search(r"after eval (\d+)", r.stdout)[1])
            peak = int(re.search(r"peak during eval (\d+)", r.stdout)[1])
            base = base or (after, peak)
            print(f"  {label:<32} derby_prog.js {len(text.encode()):5d} B   after eval {after:7d} ({after - base[0]:+6d})"
                  f"   eval peak {peak:7d} ({peak - base[1]:+6d})")


def main() -> None:
    apps = ROOT / "apps/derby"
    binary = run_derby.build(derby_heap.m32_flags(), run_derby.CACHE / "m32")
    native(binary, apps)
    guest(binary, apps)


if __name__ == "__main__":
    main()
