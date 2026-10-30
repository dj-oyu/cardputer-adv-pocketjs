"""Q16.16 against float32 on DERBY's plans, panning ones included (host,
WSL or Linux; node on PATH).

  python3 tools/kasane_ir/q16_error.py [--fuzz N] [--reuse]

1. Captures the panning draws. pocket.derby.ser() draws from C, so the
   captured game of check_equivalence.py never sees pt, prail or hl. The
   host game (tools/games/test_derby_host.c, 64-bit) runs with
   DERBY_SER_ORACLE=1, which evaluates the JS ser() of
   tools/games/derby_ser_oracle.js beside the C; a copy of that file logs
   every draw it makes (SDRAW), on the scripted game at each tier (game) and
   on N random cameras, courses and series (fuzz). The app is a lowered,
   instrumented copy (check_equivalence.instrument): its REG lines give the
   registration arguments, its DRAW lines the panning screen's hl draws.
   Nothing under apps/ or tools/games/ is changed.
2. node tools/kasane_ir/q16_error.mjs runs the plans of apps/derby/derby_prog.js
   on those and on .cache/kasane_ir/cases.json (check_equivalence.py's
   capture, if present) in double, float32 and Q16.16, and reports the
   coordinate errors (docs/kasane/shiki.md sec.5.5).
"""
from __future__ import annotations

import argparse
import json
import os
import random
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_equivalence  # noqa: E402

ROOT, CACHE, run_derby = check_equivalence.ROOT, check_equivalence.CACHE, check_equivalence.run_derby
PANNING = {"pt", "prail", "hl", "crowd"}
PER_SET = 4000  # vectors kept per plan, source and argument set


def patch_oracle(text: str) -> str:
    old = "const dr = (h, a) => { if (h) rec.push(h, ...a); }; //*"
    assert old in text, "derby_ser_oracle.js: dr() moved"
    # Fuzz handles are the oracle's own (S.ser(3|4|5|6, ...), view's 7 and 9);
    # the game's are the app's (derby.L, the view's live table).
    names = ("let __turf = false; const __nm = h => fuzz ? (h === 3 ? 'prail' : h === 4 || h === 5 ? 'pt' : "
             "h === 6 ? (__turf ? 'pt' : 'prail') : h === 7 ? 'hl' : h === 9 ? 'crowd' : '?') : "
             "(Object.keys(derby.L).find(k => derby.L[k] === h) || '?');\n  ")
    text = text.replace(old, names + "const dr = (h, a) => { if (h) { rec.push(h, ...a); "
                        "__log('SDRAW ' + (fuzz ? 'fuzz ' : 'game ') + __nm(h) + ' ' + a.join(',')); } }; //*")
    old = "    rec = [];\n    ser(h, b === undefined,"
    assert old in text, "derby_ser_oracle.js: S.ser moved"
    return text.replace(old, "    __turf = b === undefined;\n" + old)


def capture(fuzz: int) -> Path:
    binary = run_derby.build([], CACHE / "q16_host")
    out = CACHE / "q16_pan_cases.json"
    sets: dict[tuple, list] = defaultdict(list)
    with tempfile.TemporaryDirectory() as t:
        root = Path(t)
        run_derby.lower(ROOT / "apps/derby", root / "apps/derby")
        check_equivalence.instrument(root)
        (root / "tools/games").mkdir(parents=True)
        (root / "tools/games/derby_ser_oracle.js").write_text(
            patch_oracle((ROOT / "tools/games/derby_ser_oracle.js").read_text(encoding="utf-8")), encoding="utf-8")
        for tier in ("0", "1", "2"):
            r = subprocess.run([str(binary)], cwd=root, capture_output=True, text=True,
                               env=dict(os.environ, DERBY_TIER=tier, DERBY_NOCAM="1", DERBY_SER_ORACLE="1",
                                        DERBY_SER_FUZZ=str(fuzz)))
            if "DERBY_HOST PASS" not in r.stdout:
                raise SystemExit(f"host game failed at tier {tier}:\n{r.stdout[-3000:]}\n{r.stderr[-2000:]}")
            args: dict[str, list] = {}
            for line in r.stdout.splitlines():
                w = line.split()
                if "REG" in w:
                    i = w.index("REG")
                    args[w[i + 1]] = json.loads(" ".join(w[i + 2:]))
                elif "SDRAW" in w or ("DRAW" in w and w[w.index("DRAW") + 1] == "hl"):
                    i = w.index("SDRAW") if "SDRAW" in w else w.index("DRAW")
                    src, key = ("ser-" + w[i + 1], w[i + 2]) if w[i] == "SDRAW" else ("view", w[i + 1])
                    vec = [float(v) for v in w[-1].split(",")]
                    plan = "pt" if key in ("t0", "t1") else key
                    if plan not in PANNING or (plan == "crowd" and src == "ser-fuzz"):
                        continue
                    a = [512] if plan == "pt" and src == "ser-fuzz" else args.get(key, [])
                    sets[(plan, src, json.dumps(a))].append(vec)
    rng = random.Random(1)
    cases = []
    for (plan, src, a), vecs in sorted(sets.items()):
        if len(vecs) > PER_SET:
            vecs = rng.sample(vecs, PER_SET)
        cases.append({"plan": plan, "tag": src, "args": json.loads(a), "inputs": vecs})
    out.write_text(json.dumps(cases))
    print(f"captured: " + ", ".join(f"{c['plan']} {c['tag']} {len(c['inputs'])}" for c in cases))
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fuzz", type=int, default=1500, help="random ser() cases (DERBY_SER_FUZZ)")
    ap.add_argument("--reuse", action="store_true", help="reuse .cache/kasane_ir/q16_pan_cases.json")
    a = ap.parse_args()
    pan = CACHE / "q16_pan_cases.json"
    if not a.reuse or not pan.exists():
        pan = capture(a.fuzz)
    files = [str(pan)] + ([str(CACHE / "cases.json")] if (CACHE / "cases.json").exists() else [])
    subprocess.run(["node", str(HERE / "q16_error.mjs"), str(ROOT / "apps/derby/derby_prog.js"), *files],
                   cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
