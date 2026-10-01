"""Tables for docs/apps/derby-wide2-profile.md from run_derby_prof.py's output.

    python3 tools/vmtest/opprof/analyze.py [--prices-only]

Counts are host measurements (the profiler's dispatches, entries, native
calls, array literals, allocations; the harness's draws, VM steps and
segments). Microseconds are ESTIMATES: counts times the device prices in
PRICE below, which come from the device bench (derby-pan-camera-cost.md
section 1) divided by the host count of the same bench bodies' bytecode
(bench_ops.js; calibrate() prints the derivation). The model is compared
with the device's measured JS turn (DEVICE_MS) per configuration.
"""
from __future__ import annotations

import argparse
import collections
import csv
import re
import statistics
from pathlib import Path

import opprof_read as R

ROOT = Path(__file__).resolve().parents[3]
CACHE = ROOT / ".cache/opprof"

# Device JS turn (median ms, MDT js=), derby-pan-memory.md section 13 (P24, 44fce25's parent build).
DEVICE_MS = {"straight_side_mid": 14.51, "straight_w2_heavy": 21.40, "oval_w2_mid_w100": 44.25,
             "oval_w2_heavy_w100": 43.95}

# Device bench, us per loop turn (derby-pan-camera-cost.md section 1; loop is
# the raw empty loop, the others have it subtracted).
BENCH_US = {"loop": 2.86, "mul": 2.50, "add": 2.27, "div": 4.87, "sqrt": 7.42, "atan2": 15.9, "sin": 12.1,
            "cos": 12.0, "msin": 13.1, "arr": 3.77, "arrr": 2.75, "call": 4.44, "prop": 2.18, "lit8": 22.7,
            "projp": 41.8}

# Stack shuffles and constants: no lookup, no type dispatch. Split from the
# rest by the loop and arr benches (calibrate()).
SHUFFLE = {"drop", "dup", "dup1", "dup2", "dup3", "swap", "swap2", "nip", "nip1", "insert2", "insert3", "insert4",
           "perm3", "perm4", "perm5", "rot3l", "rot3r", "rot4l", "rot5l", "to_propkey", "to_propkey2", "push_i8",
           "push_i16", "push_i32", "push_minus1", "push_0", "push_1", "push_2", "push_3", "push_4", "push_5",
           "push_6", "push_7", "push_const", "push_const8", "undefined", "null", "push_true", "push_false",
           "push_atom_value", "is_undefined_or_null", "nop", "push_empty_string"}
B, S = 0.33, 0.12                       # base op, shuffle (us); loop: 8B + 2S = 2.88 (bench 2.86)
CALL_JS = 4.44 - (6 * B + 2 * S)        # a JS call + return beyond their two dispatches at B (bench: call)
PRICE = {
    # (price of the dispatch with a float64 operand, source); int operands cost B.
    "fmul": (2.50 - (3 * B + 2 * S), "bench mul"),
    "fadd": (2.27 - (3 * B + 2 * S), "bench add (sub: same, assumed)"),
    "fdiv": (4.87 - (3 * B + 2 * S), "bench div"),
    "fcmp": (0.55, "ESTIMATE (soft-float compare; not benched)"),
    "fint": (0.60, "ESTIMATE (ToInt32 of a double for | & ^ <<; not benched)"),
    "get_array_el": (2.75 - (4 * B + 3 * S), "bench arrr"),
    "put_array_el": (3.77 - (5 * B + 11 * S), "bench arr (shuffles at S)"),
    "get_field": (2.18 - (2 * B + 2 * S), "bench prop (get_field2, get_length, put_field: same, assumed)"),
    "get_var": (2.18 - (2 * B + 2 * S), "ESTIMATE = get_field (a global's lookup by atom; not benched)"),
    "array_from": (None, "bench lit8: 19.5 us for 8 elements; 15.5 + 0.5/element ESTIMATE"),
    "fclosure": (10.0, "ESTIMATE (a closure object)"),
    "other_alloc": (4.0, "ESTIMATE per malloc not in an array literal"),
}
NATIVE_US = {   # the call dispatch included (bench: e.g. sqrt = call1 + sqrt)
    "sin": 12.1 - (3 * B + 2 * S), "cos": 12.0 - (3 * B + 2 * S), "sqrt": 7.42 - (3 * B + 2 * S),
    "atan2": 15.9 - (4 * B + 2 * S),
}
NATIVE_EST = 5.0           # ESTIMATE: Math.min/max/round/floor/ceil/abs, imul, push, pressed, ...
NATIVE_EST_BY = {"apply": 14.0, "call": 6.0, "sort": 15.0, "concat": 12.0, "slice": 12.0, "indexOf": 6.0,
                 "beginFrame": 20.0, "commit": 50.0, "patch": 50.0, "setVisible": 10.0, "setColor": 10.0,
                 "setText": 15.0, "setRect": 10.0, "toFixed": 10.0, "join": 10.0}
DRAW_FIXED = 37.3 - (8 * B + 2 * S + 2 * 1.85)   # nopd: one draw, the loop and the H/P lookups out (ESTIMATE split)
DRAW_STEP = 0.46                                  # bench newton: native 8.7 us / 19 steps
DRAW_SEG = (48.8 - 37.3 - 11 * DRAW_STEP) / 3     # bench pd0 over nopd: 11 more steps, 3 segments

FCMP = {"lt", "lte", "gt", "gte", "eq", "neq", "strict_eq", "strict_neq"}
FINT = {"or", "and", "xor", "shl", "sar", "shr"}
FADD = {"add", "sub", "inc", "dec", "post_inc", "post_dec"}
FIELD = {"get_field", "get_field2", "get_length", "put_field", "get_private_field", "define_field"}
GVAR = {"get_var", "get_var_undef", "put_var", "put_var_init", "check_define_var"}
CALLS = {"call", "call0", "call1", "call2", "call3", "call_method", "tail_call", "tail_call_method", "array_from_call"}


def op_class(op: str) -> str:
    if op in SHUFFLE: return "shuffle"
    if op in FIELD: return "field"
    if op in GVAR: return "global"
    if op in CALLS: return "call"
    if op in ("get_array_el", "get_array_el2", "get_ref_value"): return "get_array_el"
    if op in ("put_array_el", "put_ref_value"): return "put_array_el"
    if op == "array_from": return "array_from"
    if op in ("fclosure", "fclosure8"): return "fclosure"
    if op in FADD or op in ("mul", "div", "mod", "neg", "plus") or op in FCMP or op in FINT: return "arith"
    return "base"


def price_ops(ops: collections.Counter, fop: collections.Counter, arr: collections.Counter) -> dict:
    """Estimated us by class for a bag of dispatches."""
    us = collections.Counter()
    for op, n in ops.items():
        c, f = op_class(op), fop.get(op, 0)
        if c == "shuffle": us["shuffle"] += n * S
        elif c == "field": us["field"] += n * PRICE["get_field"][0]
        elif c == "global": us["global"] += n * PRICE["get_var"][0]
        elif c == "call": us["call"] += n * B
        elif c == "get_array_el": us["array_el"] += n * PRICE["get_array_el"][0]
        elif c == "put_array_el": us["array_el"] += n * PRICE["put_array_el"][0]
        elif c == "array_from": pass
        elif c == "fclosure": us["alloc"] += n * PRICE["fclosure"][0]
        elif c == "arith":
            fp = (PRICE["fmul"][0] if op == "mul" else PRICE["fdiv"][0] if op in ("div", "mod") else
                  PRICE["fadd"][0] if op in FADD else PRICE["fcmp"][0] if op in FCMP else
                  PRICE["fint"][0] if op in FINT else B)
            us["float"] += f * fp
            us["base"] += (n - f) * B
        else: us["base"] += n * B
    for n, k in arr.items():
        us["array_lit"] += k * (15.5 + 0.5 * n)
    return us


def native_us(name: str) -> float:
    n = name.removeprefix("native:")
    return NATIVE_US.get(n, NATIVE_EST_BY.get(n, NATIVE_EST))


def calibrate(bench_path: Path) -> list[str]:
    """The bench bodies' bytecode per turn and the model's price for each,
    against the device's measured us (the fit's residual)."""
    ws = {w.tag: w for w in R.read(bench_path)}
    out = ["| ベンチ | 1 周の命令（host 計数） | 実機 µs（実測） | モデル µs（推定） |", "| --- | --- | ---: | ---: |"]
    for k, dev in BENCH_US.items():
        n = 256 if k == "projp" else 1000
        a, b = collections.Counter(), collections.Counter()
        fa, ba = collections.Counter(), collections.Counter()
        arr = collections.Counter()
        nat = collections.Counter()
        for f in ws[f"{k} {n}"].fns.values():
            if not f.native:
                a.update(f.ops); fa.update(f.sfop); arr.update(f.sarr); nat.update(f.snat)
        for f in ws[f"{k} 0"].fns.values():
            if not f.native:
                b.update(f.ops); ba.update(f.sfop)
        d = collections.Counter({o: (a[o] - b[o]) / n for o in a if a[o] - b[o]})
        fd = collections.Counter({o: (fa[o] - ba[o]) / n for o in fa if fa[o] - ba[o]})
        loop = k == "loop"
        if not loop:   # the device figure has the empty loop subtracted
            lw = collections.Counter({"get_loc_check": 2, "drop": 1, "dup": 1, "put_loc_check": 1, "inc": 1, "lt": 1,
                                      "get_arg0": 1, "if_false8": 1, "goto8": 1})
            if k == "projp":
                lw = collections.Counter()
            d.subtract(lw)
            d = +d
        us = price_ops(d, fd, collections.Counter({m: c / n for m, c in arr.items()}))
        natives = sum(c / n * native_us(nm) for nm, c in nat.items())
        js_calls = (CALL_JS if k == "call" else 0)
        model = sum(us.values()) + natives + js_calls - sum(d[o] * B for o in d if op_class(o) == "call" and natives)
        if k == "projp":
            model -= 0  # per point; the function call is once per 256
        body = " ".join(f"{o} {v:g}" for o, v in sorted(d.items(), key=lambda x: -x[1]))
        out.append(f"| {k} | {sum(d.values()):.1f}: {body} | {dev} | {model:.2f} |")
    return out


class Agg:
    """Per-frame means over a set of windows."""

    def __init__(self, ws: list[R.Window], csvrows: dict):
        self.n = len(ws)
        self.fn = {}
        self.edges = collections.Counter()
        self.hidden = sum(w.hidden_ops for w in ws) / self.n
        self.gc = sum(w.gc for w in ws)
        for w in ws:
            for name, f in w.fns.items():
                g = self.fn.setdefault(name, dict(native=f.native, calls=0, self=0, incl=0, allocs=0, iallocs=0,
                                                  ops=collections.Counter(), sfop=collections.Counter(),
                                                  iops=collections.Counter(), ifop=collections.Counter(),
                                                  sarr=collections.Counter(), iarr=collections.Counter(),
                                                  snat=collections.Counter(), inat=collections.Counter(),
                                                  lines=collections.Counter()))
                g["calls"] += f.calls; g["self"] += f.self_ops; g["incl"] += f.incl_ops
                g["allocs"] += f.allocs; g["iallocs"] += f.incl_allocs
                for key in ("ops", "sfop", "iops", "ifop", "sarr", "iarr", "snat", "inat", "lines"):
                    g[key].update(getattr(f, key))
            self.edges.update(w.edges)
        for g in self.fn.values():
            for key, v in list(g.items()):
                if isinstance(v, collections.Counter):
                    g[key] = collections.Counter({k: x / self.n for k, x in v.items()})
                elif isinstance(v, int) and key != "native":
                    g[key] = v / self.n
        self.edges = collections.Counter({k: v / self.n for k, v in self.edges.items()})
        ticks = [int(re.search(r"tick=(\d+)", w.tag).group(1)) for w in ws]
        rows = [csvrows[t] for t in ticks if t in csvrows]
        self.draws = statistics.fmean(int(r["draws"]) for r in rows)
        self.steps = statistics.fmean(int(r["frame_steps"]) for r in rows)
        self.segs = statistics.fmean(int(r["segments"]) for r in rows)

    def root(self):
        return next(n for n in self.fn if n.startswith("<null>@derby_play.js:31"))

    def cost(self, name: str, incl: bool) -> collections.Counter:
        g = self.fn[name]
        ops, fop, arr, nat = (g["iops"], g["ifop"], g["iarr"], g["inat"]) if incl else (g["ops"], g["sfop"], g["sarr"], g["snat"])
        us = price_ops(ops, fop, arr)
        nat_calls = sum(nat.values())
        # A call dispatch into a native is priced with the native (bench).
        us["call"] -= nat_calls * B
        for nm, c in nat.items():
            key = "draw" if nm == "native:draw" else "math" if nm.removeprefix("native:") in (
                "sin", "cos", "sqrt", "min", "max", "round", "floor", "ceil", "abs", "imul", "atan2") else "native"
            us[key] += c * native_us(nm)
        # JS entries made from here (self) or below (inclusive).
        callers = {name} if not incl else None
        entries = sum(v for (c, e), v in self.edges.items() if (c == name if not incl else c is not None and self.within(c, name)))
        us["call"] += entries * CALL_JS
        allocs = g["iallocs"] if incl else g["allocs"]
        arrays = sum(arr.values())
        us["alloc"] += max(0.0, allocs - 3 * arrays) * PRICE["other_alloc"][0]
        draws = nat.get("native:draw", 0)
        if draws:
            share = draws / max(self.draws, 1e-9)
            us["draw"] += draws * (DRAW_FIXED - native_us("native:draw")) + share * (self.steps * DRAW_STEP + self.segs * DRAW_SEG)
        return us

    _within_cache: dict = {}

    def within(self, f: str, top: str) -> bool:
        """f is top or is called (transitively) from top (by the edges)."""
        key = (f, top)
        if key in self._within_cache:
            return self._within_cache[key]
        seen, todo = {top}, [top]
        while todo:
            x = todo.pop()
            for (c, e) in self.edges:
                if c == x and e not in seen:
                    seen.add(e); todo.append(e)
        r = f in seen
        self._within_cache[key] = r
        return r


def load(name: str) -> tuple[list[R.Window], dict]:
    ws = R.read(CACHE / f"{name}.txt")
    rows = {int(r["tick"]): r for r in csv.DictReader(open(CACHE / f"{name}.csv"))}
    return ws, rows


def short(name: str) -> str:
    m = re.match(r"(.*)@derby_(\w+)\.js:(\d+):(\d+)", name)
    if not m: return name
    fn, file, line, col = m.groups()
    known = {("play", "31"): "frame", ("view", "41"): "dr", ("view", "211"): "rin", ("watch", "68", "15"): "order",
             ("watch", "68", "45"): "order の比較関数", ("play", "37"): "P（キー）", ("play", "111"): "up（patch）"}
    return known.get((file, line, col), known.get((file, line), fn)) + f" ({file}:{line})"


def fmt(x, d=0):
    return f"{x:,.{d}f}"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--prices-only", action="store_true")
    args = ap.parse_args()
    print("## 単価の較正（bench_ops.js）\n")
    print("\n".join(calibrate(CACHE / "bench.txt")))
    print(f"\nB={B} S={S} CALL_JS={CALL_JS:.2f} draw fixed={DRAW_FIXED:.1f} step={DRAW_STEP} seg={DRAW_SEG:.2f}")
    for k, (v, src) in PRICE.items():
        print(f"  {k}: {'' if v is None else f'{v:.2f}'} ({src})")
    if args.prices_only:
        return
    summary = []
    for name in ("oval_w2_mid_w100", "oval_w2_mid_w3", "oval_w2_heavy_w100", "straight_w2_heavy", "straight_side_mid"):
        ws, rows = load(name)
        a = Agg(ws, rows)
        root = a.root()
        tot = a.cost(root, True)
        g = a.fn[root]
        total = sum(tot.values()) / 1000
        dev = DEVICE_MS.get(name)
        summary.append((name, g["incl"], a.hidden, a.draws, a.steps, a.segs, total, dev, tot))
        at = {re.match(r"(at\d+)", w.tag).group(1): w for w in ws if w.tag.startswith("at")}
        per_at = []
        for m, w in sorted(at.items()):
            b = Agg([w], rows)
            per_at.append((m, b.fn[root]["incl"], sum(b.cost(root, True).values()) / 1000, b.draws, b.steps))
        print(f"\n## {name}: {a.n} frames\n")
        print("| 窓 | JS 命令（計数） | 推定 ms | draw | VM ステップ |\n| --- | ---: | ---: | ---: | ---: |")
        print(f"| 平均（{a.n} フレーム） | {fmt(g['incl'])} | {total:.1f} | {a.draws:.1f} | {fmt(a.steps)} |")
        for m, ops, ms, dr, st in per_at:
            print(f"| {m} | {fmt(ops)} | {ms:.1f} | {dr:.0f} | {fmt(st)} |")
        print("\n推定の内訳（ms）: " + ", ".join(f"{k} {v / 1000:.2f}" for k, v in tot.most_common()))
        # opcode table
        ops, fop = g["iops"], g["ifop"]
        T = sum(ops.values())
        print("\n| 命令 | 回数 | 割合 | うち倍精度 |\n| --- | ---: | ---: | ---: |")
        for op, c in ops.most_common(30):
            print(f"| {op} | {fmt(c)} | {100 * c / T:.1f}% | {fmt(fop.get(op, 0))} |")
        cls = collections.Counter()
        for op, c in ops.items():
            cls[op_class(op)] += c
        print("\n命令の種別: " + ", ".join(f"{k} {fmt(v)} ({100 * v / T:.1f}%)" for k, v in cls.most_common()))
        # functions
        print("\n| 関数 | 呼び出し | 自己 命令 | 包括 命令 | 包括 % | 自己 推定 ms | 包括 推定 ms | 包括 確保 |\n| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
        fns = [(n, f) for n, f in a.fn.items() if not f["native"] and f["incl"] > 0]
        for n, f in sorted(fns, key=lambda x: -x[1]["incl"])[:24]:
            print(f"| {short(n)} | {f['calls']:.1f} | {fmt(f['self'])} | {fmt(f['incl'])} | {100 * f['incl'] / g['incl']:.1f}% | "
                  f"{sum(a.cost(n, False).values()) / 1000:.2f} | {sum(a.cost(n, True).values()) / 1000:.2f} | {f['iallocs']:.0f} |")
        nat = g["inat"]
        print("\nネイティブ（1 フレーム）: " + ", ".join(f"{k.removeprefix('native:')} {v:.1f}" for k, v in nat.most_common(20)))
        print("配列リテラル（要素数: 個）: " + ", ".join(f"{k}: {v:.1f}" for k, v in sorted(g["iarr"].items())) +
              f"; 計 {sum(g['iarr'].values()):.1f}; 確保 {g['iallocs']:.1f}")
        print("\n| 関数 | 配列リテラル（要素数: 個） | Math などのネイティブ（自己、回） |\n| --- | --- | --- |")
        for n, f in sorted(fns, key=lambda x: -x[1]["incl"])[:16]:
            if f["sarr"] or f["snat"]:
                print(f"| {short(n)} | " + ", ".join(f"{k}: {v:.1f}" for k, v in sorted(f["sarr"].items())) + " | " +
                      ", ".join(f"{k.removeprefix('native:')} {v:.1f}" for k, v in f["snat"].most_common() if v >= .05) + " |")
        dr = next((n for n in a.fn if n.startswith("<null>@derby_view.js:41")), None)
        if dr:
            print(f"dr(): {a.fn[dr]['calls']:.1f} 回; H.draw {nat.get('native:draw', 0):.1f} 回")
        print("呼び出し辺（上位）: " + ", ".join(f"{short(c) if c else '-'}→{short(e)} {v:.1f}" for (c, e), v in a.edges.most_common(16)))
        if "_w2_" in name:
            p5(a, root)
    print("\n## 実機との比較\n")
    print("| 構成 | JS 命令（計数） | 包みの命令（計数、実機に無い） | draw | VM ステップ | 推定 ms | 実機 ms（実測） | 実測 / 推定 |\n| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for name, ops, hid, dr, st, sg, total, dev, tot in summary:
        print(f"| {name} | {fmt(ops)} | {fmt(hid)} | {dr:.1f} | {fmt(st)} | {total:.1f} | {dev if dev else '—'} | "
              f"{(dev / total):.2f}" + " |" if dev else f"| {name} | {fmt(ops)} | {fmt(hid)} | {dr:.1f} | {fmt(st)} | {total:.1f} | — | — |")
    # device = a * model + c, least squares over the measured configurations
    pts = [(total, dev) for name, ops, hid, dr, st, sg, total, dev, tot in summary if dev]
    mx_, my_ = statistics.fmean(x for x, _ in pts), statistics.fmean(y for _, y in pts)
    a = sum((x - mx_) * (y - my_) for x, y in pts) / sum((x - mx_) ** 2 for x, _ in pts)
    c = my_ - a * mx_
    print(f"\n一次式（最小二乗、{len(pts)} 構成）: 実機 ms = {a:.3f} × 推定 ms + {c:.2f}")
    for name, ops, hid, dr, st, sg, total, dev, tot in summary:
        print(f"  {name}: 式 {a * total + c:.2f} ms" + (f"、実測 {dev}（残差 {dev - a * total - c:+.2f}）" if dev else "（実測なし）"))


def p5(a: Agg, root: str) -> None:
    """What P5 (oval-pie-design.md 6.1) would move into native code, by the
    counts: every ser() (its pose/lim/inr/ou/dr), and pan()'s chord-end and
    chord-selection loops (lines 83-87); what stays: everything else."""
    pan = next(n for n in a.fn if n.startswith("pan@"))
    ser = next(n for n in a.fn if n.startswith("ser@"))
    pose = next(n for n in a.fn if n.startswith("pose@"))
    ou = next((n for n in a.fn if n.startswith("ou@")), None)   # none on the straight
    us = lambda n, incl=True: sum(a.cost(n, incl).values()) / 1000 if n else 0.0
    tot, pan_ms, ser_ms = us(root), us(pan), us(ser)
    # pan's own lines: VE (83-86) and VC (87) move; the rest stays.
    lines = a.fn[pan]["lines"]
    self_ops = sum(lines.values())
    pan_self_ms = us(pan, False)
    per_op = pan_self_ms / self_ops if self_ops else 0
    moved_lines = sum(v for l, v in lines.items() if 82 <= l <= 88)
    pose_from_pan = a.edges.get((pan, pose), 0)
    ve_pose = max(0.0, pose_from_pan - 14)            # 6 for the poles, 8 for the runners (trig-cull 2)
    pose_each = us(pose) / max(a.fn[pose]["calls"], 1e-9)
    ou_from_pan = a.edges.get((pan, ou), 0)
    ou_each = us(ou) / max(a.fn[ou]["calls"], 1e-9) if ou else 0.0
    moved = ser_ms + moved_lines * per_op + ve_pose * pose_each + ou_from_pan * ou_each
    print(f"\nP5 の区分（推定 ms）: フレーム {tot:.2f}、pan 包括 {pan_ms:.2f}（うち ser 包括 {ser_ms:.2f}、"
          f"弦の端と視野の選び出し {moved - ser_ms:.2f}：pan の 83〜87 行 {moved_lines:.0f} 命令、VE の pose {ve_pose:.1f} 回 × {pose_each * 1000:.0f} µs、"
          f"ou {ou_from_pan:.1f} 回 × {ou_each * 1000:.0f} µs）")
    print(f"  移せる分 {moved:.2f} ms（フレームの {100 * moved / tot:.0f}%）、pan に残る分 {pan_ms - moved:.2f} ms、pan の外 {tot - pan_ms:.2f} ms")
    rest = collections.Counter()
    groups = {"系列の呼び出し（89・96〜98・108 行）": (89, 96, 97, 98, 108), "大型画面（90〜95 行）": range(90, 96),
              "距離標（99〜103 行）": range(99, 104), "走者（104〜107 行）": range(104, 108), "先頭（80・81・109 行）": (79, 80, 81, 109)}
    for g, ls in groups.items():
        rest[g + " の自己命令"] = sum(v for l, v in lines.items() if l in ls) * per_op
    for callee, label in ((pose, "pose（距離標・走者）"), ):
        rest[label] = (pose_from_pan - ve_pose) * pose_each
    for n in a.fn:
        if a.fn[n]["native"] or n in (pose, ser, ou):
            continue
        c = a.edges.get((pan, n), 0)
        if c:
            rest[short(n)] += c * us(n) / max(a.fn[n]["calls"], 1e-9)
    print("  pan に残る分の内訳（推定 ms）: " + ", ".join(f"{k} {v:.2f}" for k, v in rest.most_common()) +
          f"; 計 {sum(rest.values()):.2f}（pan 包括 − 移せる分 = {pan_ms - moved:.2f}）")
    print(f"  ser 本体の数（ser の draw 前の判定を通った弦）: ser {a.fn[ser]['calls']:.1f} 回、lim {a.fn[next(n for n in a.fn if n.startswith('lim@'))]['calls']:.1f} 回 / 4 = 本体 {a.fn[next(n for n in a.fn if n.startswith('lim@'))]['calls'] / 4:.1f}")


if __name__ == "__main__":
    main()
