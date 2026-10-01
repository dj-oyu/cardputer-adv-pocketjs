"""Reader for opprof output (opprof_impl.c's FRAME ... END blocks)."""
from __future__ import annotations

import collections
from dataclasses import dataclass, field

C = collections.Counter


@dataclass
class Fn:
    name: str
    native: bool
    hidden: bool
    calls: int
    self_ops: int
    incl_ops: int
    allocs: int
    alloc_bytes: int
    reallocs: int
    frees: int
    incl_allocs: int
    ops: C = field(default_factory=C)     # self, by opcode
    sfop: C = field(default_factory=C)    # self, float64 operand
    iops: C = field(default_factory=C)    # inclusive
    ifop: C = field(default_factory=C)
    sarr: C = field(default_factory=C)    # array literals by length
    iarr: C = field(default_factory=C)
    snat: C = field(default_factory=C)    # native name -> calls (self / inclusive)
    inat: C = field(default_factory=C)
    lines: C = field(default_factory=C)   # self dispatches by source line


@dataclass
class Window:
    tag: str
    gc: int = 0
    hidden_ops: int = 0
    hidden_nat: int = 0
    hidden_alloc: int = 0
    fns: dict = field(default_factory=dict)   # name -> Fn (natives too)
    edges: C = field(default_factory=C)       # (caller name or None, callee name) -> entries


def _kv(parts, key=str):
    for kv in parts:
        k, v = kv.split("=")
        yield key(k), v


def read(path) -> list[Window]:
    out, w, idx, natrows = [], None, {}, []
    for line in open(path, encoding="utf-8"):
        p = line.rstrip("\n").split(" ")
        t = p[0]
        if t == "FRAME":
            w, idx, natrows = Window(" ".join(p[1:])), {}, []
        elif t == "GC":
            w.gc = int(p[1])
        elif t == "HIDDEN":
            w.hidden_ops, w.hidden_nat, w.hidden_alloc = map(int, p[1:4])
        elif t == "FN":
            f = Fn(" ".join(p[12:]), p[2] == "1", p[3] == "1", *map(int, p[4:12]))
            idx[int(p[1])] = f
            w.fns[f.name] = f
        elif t in ("OPS", "SFOP", "IOPS", "IFOP"):
            c = getattr(idx[int(p[1])], {"OPS": "ops", "SFOP": "sfop", "IOPS": "iops", "IFOP": "ifop"}[t])
            for k, v in _kv(p[2:]):
                c[k] += int(v)
        elif t in ("SARR", "IARR"):
            c = getattr(idx[int(p[1])], t.lower())
            for k, v in _kv(p[2:], int):
                c[k] += int(v)
        elif t == "LINES":
            for k, v in _kv(p[2:], int):
                idx[int(p[1])].lines[k] += int(v)
        elif t == "EDGE":
            c, e = int(p[1]), int(p[2])
            w.edges[(idx[c].name if c in idx else None, idx[e].name)] += int(p[3])
        elif t == "NAT":
            natrows.append((int(p[1]), p[2:]))
        elif t == "END":
            for i, parts in natrows:
                for k, v in _kv(parts, int):
                    s, n = map(int, v.split("/"))
                    name = idx[k].name if k in idx else f"native#{k}"
                    if name == "native:__opprof":   # the profiler's own switch
                        continue
                    idx[i].snat[name] += s
                    idx[i].inat[name] += n
            w.fns.pop("native:__opprof", None)
            out.append(w)
    return out
