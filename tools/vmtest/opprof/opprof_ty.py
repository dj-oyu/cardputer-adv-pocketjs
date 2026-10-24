"""(op, type) pairs per turn, from the TY rows of an opprof dump.

    python tools/vmtest/opprof/opprof_ty.py .cache/opprof/oval_w2_mid_w100.txt [--top N]

A pair is one handler a quickening interpreter would need: the op, and the
type code opprof_impl.c read from its operands (opprof_type). One window is
one frame() call, i.e. one turn. Prints, over the windows:
  - distinct pairs per turn (min / median / max) and over the whole run;
  - how many pairs cover 90 / 99 / 99.9 % of the dispatches;
  - the polymorphic ops (more than one code) and their dominant share;
  - the pairs, heaviest first, with their code named.
The lower bound's h term (docs/vm/interpreter-stages.md) is pairs x the
bytes of one specialised handler; this script counts, it does not price.
"""
from __future__ import annotations

import argparse
import collections
import statistics
from pathlib import Path

ARITH = {"add", "sub", "mul", "div", "mod", "lt", "lte", "gt", "gte", "strict_eq", "strict_neq",
         "eq", "neq", "shl", "sar", "shr", "and", "or", "xor"}
UNARY = {"neg", "inc", "dec", "post_inc", "post_dec", "plus", "not", "lnot", "if_true", "if_false",
         "if_true8", "if_false8", "to_propkey", "typeof", "is_undefined_or_null"}
PROP = {"get_field", "get_field2", "put_field", "get_length"}
ELEM = {"get_array_el", "get_array_el2", "put_array_el"}
GVAR = {"get_var", "get_var_undef", "put_var"}
CALL = {"call0", "call1", "call2", "call3", "call", "tail_call", "call_method", "tail_call_method"}

T = ["int", "float", "bool", "nullish", "string", "object", "other", "?"]
P = ["own", "own-special", "proto1", "proto2", "proto3+", "missing", "own-big", "?"]
EK = ["array", "slow-array", "typed", "object", "string", "prim"]
EI = ["int", "float", "string", "other"]
GV = ["lexical", "global-data", "global-slow"]
CK = ["bytecode", "cfunc", "cdata/bound", "other-obj", "non-obj"]


def name(op: str, code: int) -> str:
    if op in ARITH:
        return f"{T[code // 8]},{T[code % 8]}"
    if op in UNARY:
        return T[code]
    if op in PROP:
        if 8 <= code < 16:
            return "prim:" + T[code - 8]
        return ("fast:" if code >= 16 else "") + P[code % 16 if code % 16 < 8 else 7]
    if op in ELEM:
        return f"{EK[code // 4]}[{EI[code % 4]}]"
    if op in GVAR:
        return GV[code] if code < 3 else str(code)
    if op in CALL:
        return CK[code] if code < 5 else str(code)
    return "-" if code == 0 else str(code)


def windows(path: Path):
    cur = None
    for line in path.read_text().splitlines():
        if line.startswith("FRAME "):
            cur = {}
        elif line.startswith("TY ") and cur is not None:
            f = line.split()
            for kv in f[2:]:
                k, v = kv.split("=")
                cur[(f[1], int(k))] = int(v)
        elif line == "END" and cur is not None:
            yield cur
            cur = None


def cover(counts: collections.Counter, frac: float) -> int:
    total, run = sum(counts.values()), 0
    for i, (_, n) in enumerate(counts.most_common(), 1):
        run += n
        if run >= frac * total:
            return i
    return len(counts)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path)
    ap.add_argument("--top", type=int, default=60)
    a = ap.parse_args()
    ws = list(windows(a.dump))
    if not ws:
        raise SystemExit("no TY rows: rebuild with the current opprof_impl.c")
    total = collections.Counter()
    per = []
    for w in ws:
        total.update(w)
        per.append(len(w))
    disp = sum(total.values())
    ops = {op for op, _ in total}
    print(f"windows {len(ws)}, dispatches {disp} ({disp / len(ws):.0f}/turn), ops {len(ops)}")
    print(f"distinct pairs per turn: min {min(per)} median {statistics.median(per):.0f} max {max(per)}; "
          f"whole run {len(total)}")
    print("pairs covering 90/99/99.9% of dispatches: " +
          " / ".join(str(cover(total, f)) for f in (0.90, 0.99, 0.999)))
    typed = [k for k in total if k[0] in ARITH | UNARY | PROP | ELEM | GVAR | CALL]
    print(f"pairs in typed families {len(typed)}, type-independent ops {len(total) - len(typed)}")

    by_op = collections.defaultdict(collections.Counter)
    for (op, code), n in total.items():
        by_op[op][code] = n
    print("\npolymorphic ops (codes, dominant share):")
    for op, c in sorted(by_op.items(), key=lambda kv: -sum(kv[1].values())):
        if len(c) > 1:
            s = sum(c.values())
            parts = ", ".join(f"{name(op, k)} {n / s:.1%}" for k, n in c.most_common(4))
            print(f"  {op:16} {s / len(ws):8.1f}/turn  {parts}")

    print(f"\ntop {a.top} pairs (/turn, cumulative share):")
    run = 0
    for (op, code), n in total.most_common(a.top):
        run += n
        print(f"  {op:16} {name(op, code):22} {n / len(ws):8.1f}  {run / disp:6.1%}")


if __name__ == "__main__":
    main()
