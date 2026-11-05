#!/usr/bin/env python3
"""Find stack accesses beyond the 1,020-byte reach of l32i/s32i (Xtensa).

l32i/s32i/l16ui/s16i/l8ui/s8i take an unsigned 8-bit offset scaled by the
access size, so a word reaches a1+0..1020 in one instruction. A function whose
stack frame (`entry a1, N`) is larger reaches its far end through an address
computed first:

    movi  a9, 0x424        addmi a9, a1, 0x400
    add.n a9, a9, a1       s32i.n a8, a9, 0
    l32i.n a8, a9, 0

two extra instructions (movi+add) or one (addmi) for every such access. On
this core instruction count is most of the cycle count (docs/perf/pie-simd.md
3.9), so a hot function whose frame grew past 1,020 B pays for it on every
spill. That is what made render_group's inlining slower (docs/perf/o2-machine-code.md 3.2).

Input: object files, directories of them (*.o, *.obj, searched recursively) or
an ELF. Output: one row per function with a frame over the limit or with far
accesses. `--baseline FILE --check` fails when a function gains far accesses
or a new function gets them; `--write-baseline FILE` records the current state.

The scan is linear inside a function and forgets every derived address at a
branch target, so a far address is only credited to loads/stores that use it
before the next label. GCC emits the address and the access back to back, so
this misses little; it never invents an access (counts are a lower bound).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from collections import namedtuple

REACH = 1020
OBJDUMP_NAMES = ("xtensa-esp32s3-elf-objdump", "xtensa-esp-elf-objdump")

FUNC_RE = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
INSN_RE = re.compile(r"^\s*([0-9a-f]+):\s+([a-z][\w.]*)\s*(.*)$")
TARGET_RE = re.compile(r"\b([0-9a-f]+) <[^>+]+(?:\+0x[0-9a-f]+)?>")

MEM_OPS = {"l32i", "l32i.n", "s32i", "s32i.n", "l16ui", "l16si", "s16i",
           "l8ui", "s8i", "lsi", "ssi", "l32e", "s32e"}
# Instructions whose first operand is not written.
NO_DEST = {"s32i", "s32i.n", "s16i", "s8i", "ssi", "ssip", "ssx", "ssxp",
           "s32e", "s32nb", "j", "jx", "call0", "call4", "call8", "call12",
           "callx0", "callx4", "callx8", "callx12", "ret", "ret.n", "retw",
           "retw.n", "loop", "loopnez", "loopgtz", "wsr", "wur", "memw",
           "nop", "nop.n", "entry", "isync", "rsync", "esync", "dsync",
           "extw", "break", "ill"}

Row = namedtuple("Row", "func src frame accesses computes extra worst")


def find_objdump(explicit):
    if explicit and os.path.exists(explicit):
        return explicit
    for name in OBJDUMP_NAMES:
        path = shutil.which(name)
        if path:
            return path
    return None


def imm(text):
    text = text.strip()
    return int(text, 16) if text.lower().startswith(("0x", "-0x")) else int(text)


def operands(rest):
    rest = rest.split("<", 1)[0].split("#", 1)[0]
    return [o.strip() for o in rest.split(",") if o.strip()]


def scan(lines, src, limit=REACH):
    """Yield a Row per function in objdump text `lines`."""
    funcs = []
    cur = None
    for line in lines:
        line = line.rstrip("\n")
        m = FUNC_RE.match(line)
        if m:
            cur = [m.group(2), []]
            funcs.append(cur)
            continue
        if cur is None:
            continue
        m = INSN_RE.match(line)
        if m:
            cur[1].append((int(m.group(1), 16), m.group(2), m.group(3)))
    for name, insns in funcs:
        targets = set()
        for _, op, rest in insns:
            if op.startswith(("b", "j", "loop")):
                for t in TARGET_RE.findall(rest):
                    targets.add(int(t, 16))
        frame = 0
        const, far = {}, {}
        accesses = computes = extra = worst = 0
        for addr, op, rest in insns:
            if addr in targets:
                const.clear(); far.clear()
            ops = operands(rest)
            if op == "entry" and len(ops) == 2 and ops[0] == "a1":
                frame = max(frame, imm(ops[1]))
                continue
            if op in MEM_OPS and len(ops) == 3 and ops[1] in far:
                accesses += 1
                worst = max(worst, far[ops[1]] + imm(ops[2]))
            dest = ops[0] if ops and op not in NO_DEST and not op.startswith("b") else None
            new_const = new_far = None
            if op in ("movi", "movi.n") and len(ops) == 2:
                new_const = imm(ops[1])
            elif op in ("add", "add.n") and len(ops) == 3 and "a1" in ops[1:]:
                other = ops[2] if ops[1] == "a1" else ops[1]
                k = const.get(other)
                if k is not None and k > limit:
                    new_far = k; computes += 1; extra += 2
            elif op == "addmi" and len(ops) == 3 and ops[1] == "a1":
                k = imm(ops[2])
                if k > limit:
                    new_far = k; computes += 1; extra += 1
            elif op in ("addi", "addi.n") and len(ops) == 3 and ops[1] in far:
                new_far = far[ops[1]] + imm(ops[2])
            if dest:
                const.pop(dest, None); far.pop(dest, None)
                if new_const is not None:
                    const[dest] = new_const
                if new_far is not None:
                    far[dest] = new_far
        if frame > limit or accesses:
            yield Row(name, src, frame, accesses, computes, extra, worst)


def inputs(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for f in sorted(files):
                    if f.endswith((".o", ".obj")):
                        yield os.path.join(root, f)
        else:
            yield p


def disassemble(objdump, path):
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", path],
                         capture_output=True, text=True)
    if out.returncode:
        sys.stderr.write(f"frame_lint: {path}: {out.stderr.strip()}\n")
        return []
    return out.stdout.splitlines()


def load_baseline(path):
    base = {}
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                func, count = line.rsplit(None, 1)
                base[func] = int(count)
    return base


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="+", help="object files, directories, or an ELF")
    ap.add_argument("--objdump", help="Xtensa objdump (default: found on PATH)")
    ap.add_argument("--filter", default="", help="only sources whose path contains this")
    ap.add_argument("--limit", type=int, default=REACH, help="reach in bytes (default 1020)")
    ap.add_argument("--top", type=int, default=0, help="print only the N worst rows")
    ap.add_argument("--far-only", action="store_true",
                    help="list only functions with far accesses (not large frames alone)")
    ap.add_argument("--baseline", help="FILE of 'function count' lines")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if a function has more far accesses than the baseline allows")
    ap.add_argument("--write-baseline", metavar="FILE")
    args = ap.parse_args()
    objdump = find_objdump(args.objdump)
    if objdump is None:
        print("FRAME_LINT SKIPPED: no Xtensa objdump on PATH (run the IDF export "
              "script or pass --objdump)")
        return 1 if args.check else 0
    rows = []
    for path in inputs(args.paths):
        if args.filter and args.filter not in path:
            continue
        rows.extend(scan(disassemble(objdump, path), path, args.limit))
    rows.sort(key=lambda r: (-r.accesses, -r.frame, r.func))
    shown = [r for r in rows if r.accesses] if args.far_only else rows
    shown = shown[:args.top] if args.top else shown
    base = load_baseline(args.baseline) if args.baseline else None
    print(f"{'far':>5} {'+insn':>6} {'frame':>6} {'deepest':>7}  function  (source)")
    for r in shown:
        mark = ""
        if base is not None:
            allowed = base.get(r.func, 0)
            mark = "  NEW" if r.accesses and r.func not in base else (
                f"  GREW from {allowed}" if r.accesses > allowed else "")
        print(f"{r.accesses:5d} {r.extra:6d} {r.frame:6d} {r.worst or '-':>7}  {r.func}  "
              f"({os.path.basename(r.src)}){mark}")
    print(f"FRAME_LINT functions={len(rows)} far_accesses={sum(r.accesses for r in rows)} "
          f"extra_insns={sum(r.extra for r in rows)} limit={args.limit}")
    if args.write_baseline:
        with open(args.write_baseline, "w") as f:
            f.write("# tools/frame_lint.py baseline: function, far accesses allowed\n")
            for r in sorted(rows, key=lambda r: r.func):
                if r.accesses:
                    f.write(f"{r.func} {r.accesses}\n")
    if args.check:
        base = base or {}
        bad = [r for r in rows if r.accesses > base.get(r.func, 0)]
        for r in bad:
            print(f"FRAME_LINT FAIL {r.func}: {r.accesses} far accesses "
                  f"(baseline {base.get(r.func, 0)}), frame {r.frame} B")
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
