"""Summarise a device log of tools/games/pancost/bench/derby_watch.js
(docs/apps/derby-pan-camera-cost.md).

Record the log with the bgcost runner (it starts the DERBY WATCH row, which
the DERBY_BGCOST_SOURCE image replaced with the bench):

  python tools/games/bgcost/bgcost_device.py run --seconds 300 --out <log>
  python tools/games/pancost/pancost_device.py <log> [<log> ...]

Per case, the median JS turn (MDT js=) of each n, then the least-squares
slope over n: us per iteration (JS cases) or per point (projection cases).
JS cases subtract the empty loop's slope, so they read as the body's cost.
"""
from __future__ import annotations

import argparse
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bgcost"))
import bgcost_device as bg  # noqa: E402

BGS = re.compile(r"BGS (\w+) (\d+)")
ERR = re.compile(r"BGS ERR (.*)")


def slope(points):
    n = len(points)
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    sxx = sum((x - mx) ** 2 for x, _ in points)
    s = sum((x - mx) * (y - my) for x, y in points) / sxx
    return s, my - mx * s


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+", type=Path)
    args = ap.parse_args()

    def state_of(line, state):
        if m := BGS.search(line):
            return (m[1], int(m[2])) if m[1] not in ("READY", "ERR") else state
        return state

    groups: dict = {}
    for path in args.logs:
        for s, v in bg.turns(path, state_of):
            groups.setdefault(s, []).append(v)
        for line in bg.lines(path):
            if m := ERR.search(line):
                print("ERROR", m[1])
    cases: dict = {}
    for (name, n), vs in groups.items():
        cases.setdefault(name, {})[n] = vs
    loop = None
    if "loop" in cases:
        loop = slope([(n, statistics.median([v["js"] for v in vs])) for n, vs in cases["loop"].items()])[0]
    print("case | n: js median us (turns) | draw us | slope us/unit | net of loop | draw slope | intercept us")
    for name, by_n in cases.items():
        pts = sorted((n, statistics.median([v["js"] for v in vs])) for n, vs in by_n.items())
        dr = sorted((n, statistics.median([v["draw_us"] for v in vs])) for n, vs in by_n.items())
        cells = " ".join(f"{n}:{js:.0f}({len(by_n[n])})" for n, js in pts)
        s, b = slope(pts)
        ds = slope(dr)[0]
        net = f"{s - loop:.3f}" if loop is not None and name in JS_CASES else "-"
        print(f"{name} | {cells} | {' '.join(f'{d:.0f}' for _, d in dr)} | {s:.3f} | {net} | {ds:.3f} | {b:.0f}")
    return 0


JS_CASES = {"loop", "mul", "add", "div", "sqrt", "atan2", "sin", "cos", "msin", "arr", "arrr", "f32", "i16", "call",
            "prop", "lit8"}

if __name__ == "__main__":
    sys.exit(main())
