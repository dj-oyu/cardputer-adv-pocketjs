"""What DERBY WATCH's plan texts and their assembler keep in the guest heap,
and what registering plans by id (IR in flash) would save (host, WSL only).

  python3 tools/kasane_ir/derby_heap.py [--stage3 DIR]

Builds tools/games/test_derby_host.c -m32 (tools/games/run_derby.py's build,
the device's 8 B JSValue, allocations charged as TLSF lengths) and evaluates
the game with DERBY_EVAL_ONLY=1 for each form of derby_prog.js:

  as is      the shipped file: 13 plan texts in T, OPS/FLD and prog();
  no-asm     T's texts kept, prog() and OPS/FLD removed (prog(A, B) -> [A, B]);
  ids        T's texts replaced by small integers, the T.map loop removed
             (its lines would be baked at build time), no prog();
  names      no T at all: a plan is named by the string the scene already
             holds (PAD/RUN), the upper bound of the guest-side saving.

Each form only has to evaluate: registration is not called during the
evaluation, so the forms that no longer assemble are measured, not run.
--stage3 DIR measures the panning stage too (DIR/apps/derby: a `git show`
export of vm/pan-camera 55956e9, docs/kasane/js-to-ir.md).

Then tools/kasane_ir/heap_probe.c evaluates derby_prog.js alone and reports
each plan's registration cost in the guest: what spec(name) holds until
register() returns (the array of 6-element rows) and prog()'s peak.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/games"))
import run_derby  # noqa: E402

CACHE = ROOT / ".cache/kasane_ir"


def m32_flags() -> list[str]:
    env = dict(os.environ, M32_SYSROOT=str(ROOT / ".cache/kasane_megademo_app/m32sys"))
    return subprocess.run(["bash", "tools/vmtest/m32_sysroot.sh"], cwd=ROOT, check=True, env=env,
                          capture_output=True, text=True).stdout.split()


def rewrite_calls(text: str, name: str, fn) -> str:
    """Replace every name(ARGS) with fn(list of top-level argument texts)."""
    out, i = [], 0
    while True:
        j = text.find(name + "(", i)
        if j < 0 or (j and (text[j - 1].isalnum() or text[j - 1] in "_$.")):
            if j < 0:
                return "".join(out) + text[i:]
            out.append(text[i:j + 1]); i = j + 1; continue
        k, depth, args, start = j + len(name) + 1, 1, [], j + len(name) + 1
        quote = None
        while depth:
            c = text[k]
            if quote:
                quote = None if c == quote else quote
            elif c in "'\"":
                quote = c
            elif c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == "," and depth == 1:
                args.append(text[start:k].strip()); start = k + 1
            k += 1
        args.append(text[start:k - 1].strip())
        out.append(text[i:j]); out.append(fn(args)); i = k


def no_asm(text: str) -> str:
    text = re.sub(r"^const OPS = .*\n", "", text, flags=re.M)
    text = re.sub(r"^function prog\(src, arg\) \{\n(?:.*\n)*?\}\n", "", text, flags=re.M)
    return rewrite_calls(text, "prog", lambda a: a[0] if len(a) == 1 else f"[{a[0]}, {a[1]}]")


def ids(text: str) -> str:
    n = [0]

    def num(m):
        n[0] += 1
        return f"{m.group(1)}: {n[0]}{m.group(2)}"
    text = re.sub(r"^(  \w+): '[^']*'(,?)$", num, text, flags=re.M)
    text = re.sub(r"^for \(let k = 0; k < 8; \+\+k\) T\.map \+= .*\n", "", text, flags=re.M)
    return no_asm(text).replace("'S0,0'", "0")


def names(text: str) -> str:
    text = ids(text)
    text = re.sub(r"^const T = \{\n(?:.*\n)*?\};\n", "", text, flags=re.M)
    text = re.sub(r"\bT\[(\w+)\]", r"\1", text)
    return re.sub(r"\bT\.(\w+)", r"'\1'", text)


FORMS = {"as is": lambda t: t, "no-asm": no_asm, "ids": ids, "names": names}


def eval_only(binary: Path, apps: Path, form) -> dict:
    with tempfile.TemporaryDirectory() as t:
        root = Path(t)
        shutil.copytree(apps, root / "apps/derby")
        p = root / "apps/derby/derby_prog.js"
        p.write_text(form(p.read_text(encoding="utf-8")), encoding="utf-8")
        r = subprocess.run([str(binary), "apps/derby/derby_watch.js"], cwd=root, capture_output=True, text=True,
                           env=dict(os.environ, DERBY_EVAL_ONLY="1"))
        if r.returncode:
            raise SystemExit(f"eval failed:\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
        m = re.search(r"after eval (\d+) \(\+(\d+)\)", r.stdout)
        pk = re.search(r"peak during eval (\d+)", r.stdout)
        a = re.search(r"^atoms\s+(\d+)\s+(\d+)", r.stdout, re.M)
        f = re.search(r"^bytecode functions\s+(\d+)\s+(\d+)", r.stdout, re.M)
        c = re.search(r"^  bytecode\s+(\d+)\s+(\d+)", r.stdout, re.M)
        return {"after": int(m.group(1)), "peak": int(pk.group(1)), "atoms": (int(a.group(1)), int(a.group(2))),
                "funcs": (int(f.group(1)), int(f.group(2))), "code": int(c.group(2))}


PRELUDE = """'use strict';
const M = Math, PI = M.PI, sin = M.sin, rnd = M.round, mx = M.max;
const SILK = [0xffff, 0x8c71, 0xf800, 0x237f, 0xffe0, 0x07e0, 0xfd20, 0xf81f];
"""
PROBE = """
F = {h: [0, 1, 2, 3, 4, 5, 6, 7].map(i => ({coat: 0xdd8c}))};
const report = [];
for (const n of Object.keys(T).concat(['r0'])) {
  if (n === 'runner') continue;
  __gc(); const b = __heap(); __peak_reset();
  let p = spec(n);
  const held = __heap() - b, peak = __peak() - b, rows = p[0].length;
  p = null; __gc();
  __print(`PLAN ${n} rows=${rows} held=${held} peak=${peak} after=${__heap() - b}`);
}
"""


def probe(apps: Path) -> str:
    out = CACHE / "m32"
    out.mkdir(parents=True, exist_ok=True)
    objs = [str(run_derby.CACHE / "m32" / f"{n}.o") for n in ("dtoa", "libregexp", "libunicode", "quickjs",
                                                              "quickjs-vm")]
    binary = out / "heap_probe"
    subprocess.run(["gcc", "-std=gnu11", "-O1", "-g", *m32_flags(), "-DQUICKJS_NG_BUILD", "-D_GNU_SOURCE",
                    "-I", run_derby.QJS, "-I", "components/pocketjs_guest/include",
                    "tools/kasane_ir/heap_probe.c", *objs, "-lm", "-lpthread", "-o", str(binary)],
                   cwd=ROOT, check=True)
    script = out / "probe.js"
    script.write_text(PRELUDE + (apps / "derby_prog.js").read_text(encoding="utf-8").replace("'use strict';", "")
                      + PROBE, encoding="utf-8")
    return subprocess.run([str(binary), str(script)], check=True, capture_output=True, text=True).stdout


def main() -> None:
    stage3 = None
    if sys.argv[1:2] == ["--stage3"]:
        stage3 = Path(sys.argv[2]) / "apps/derby"
    binary = run_derby.build(m32_flags(), run_derby.CACHE / "m32")
    sets = [("909decd", ROOT / "apps/derby")] + ([("stage3", stage3)] if stage3 else [])
    for label, apps in sets:
        base = peak0 = None
        print(f"==== {label}: guest heap after evaluation (host m32, TLSF charge)")
        for name, form in FORMS.items():
            r = eval_only(binary, apps, form)
            base, peak0 = base or r["after"], peak0 or r["peak"]
            print(f"  {name:<7} after {r['after']:7d}  diff {r['after'] - base:+6d}  "
                  f"eval peak {r['peak']:7d} ({r['peak'] - peak0:+6d})  atoms {r['atoms'][0]} "
                  f"({r['atoms'][1]} B)  functions {r['funcs'][0]} ({r['funcs'][1]} B)  bytecode {r['code']} B")
    for label, apps in sets:
        print(f"==== {label}: registration in the guest (spec(name) until register returns)")
        print(probe(apps), end="")


if __name__ == "__main__":
    main()
