"""The DERBY WATCH background-cost study on the device (docs/kasane/derby-background-cost.md).

  run    open APPS, start row 12 (DERBY WATCH, or the DERBY_BGCOST_SOURCE copy
         that replaced it), log COM3 for --seconds, Back, save the raw log.
  race   summarise a log of tools/games/bgcost/race/derby_watch.js: per
         (tier, camera, background mode) the frame interval, JS, render,
         send, draw, band and the BGC geometry; then each mode minus mode 0.
  synth  summarise a log of tools/games/bgcost/synth/derby_watch.js and give the
         per-segment slopes of band, draw and JS time for each geometry.
  paint  KASANE_PAINT windows of a non-trace image, grouped by the BGX state
         (a window is kept only if no BGX line fell inside it).

The trace lines are KASANE_MEGADEMO_TRACE's MDT and KASANE_BGCOST_TRACE's BGC
and BGP. An MDT line describes the turn before it and is printed at the top of
the next turn, so the BGX/BGS line a turn's frame() logged comes before that
turn's MDT line: the state in force when an MDT line is read is its turn's.
"""
from __future__ import annotations

import argparse
import re
import statistics
import sys
import time
from pathlib import Path

MDT = re.compile(r"MDT (\d+) (\w) t=(\d+) js=(\d+) rn=(\d+) sd=(\d+) by=(\d+) bd=(\d+) pr=(\d+) "
                 r"reg=(\d+)/(\d+) prep=(\d+)/(\d+) un=(\d+) draw=(\d+)/(\d+)/(\d+) band=(\d+)/(\d+) "
                 r"free=(\d+) lg=(\d+) k=([0-9a-f]+)/([0-9a-f]+) mn=(\d+) gu=(\d+)")
BGC = re.compile(r"BGC (\d+) st=(\d+) seg=(\d+) ras=(\d+) hit=(\d+) walk=(\d+)(?: cm=(\d+) vw=(\d+))?")
BGP = re.compile(r"BGP((?: \d+=\d+/\d+/\d+/\d+)*)")
BGX = re.compile(r"DERBY BGX (\d) (\d) (\d)")
REG = re.compile(r"DERBY REG (\S+) (\d+)")
SCENE = re.compile(r"DERBY SCENE (\w+)")
BGS = re.compile(r"BGS (\w+) (\d+)")
PAINT = re.compile(r"\((\d+)\) kasane: KASANE_PAINT turn_ms=([0-9.]+) render_ms=([0-9.]+) send_ms=([0-9.]+)")
BAD = ("Guru Meditation", "START_FAILED", "RUNAWAY", "abort()", "Uncaught", "app: OOM",
       "FRAMEFAIL", "LOADFAIL", "DEGRADE")
TIERS = ("LIGHT", "MID", "HEAVY")
CAMS = {0: "WIDE", 1: "CLOSE", 2: "FIELD", 5: "VISION", 6: "HEAD ON"}
MODES = ("all", "no bg", "no crowd")
SETTLE = 3


def lines(path: Path):
    text = path.read_bytes().replace(b"\x00", b"").decode("utf-8", "replace")
    return text.splitlines()


def run(args) -> int:
    import serial
    port = serial.Serial(port=None, baudrate=115200, timeout=0.05)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    log = bytearray()

    def pump(seconds, stop=None, since=0):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            log.extend(port.read(65536))
            if stop and stop in log[since:]:
                return True
        return False

    status = 0
    try:
        time.sleep(0.3)
        port.reset_input_buffer()
        port.write(b"q")
        if not pump(10, b"HOME_READY"):
            raise RuntimeError("no HOME_READY")
        port.write(b"a")
        pump(5, b"CATEGORY 0")
        for _ in range(16):
            port.write(b"u")
            pump(0.05)
        for _ in range(args.row):
            port.write(b"d")
            pump(0.05)
        if not pump(5, b"APP %d" % args.row):
            raise RuntimeError("row not reached")
        start = len(log)
        port.write(b"e")
        pump(args.seconds)
        got = bytes(log[start:]).decode("utf-8", "replace")
        bad = [b for b in BAD if b in got]
        mark = len(log)
        port.write(b"q")
        pump(15, b"HOME_READY", mark)
        print("BGCOST_RUN", "FAIL " + ",".join(bad) if bad else "PASS")
        status = 2 if bad else 0
    except RuntimeError as error:
        print(f"BGCOST_RUN FAIL {error}")
        status = 2
    finally:
        port.close()
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(log)
    return status


def med(values):
    return statistics.median(values) if values else float("nan")


def turns(path: Path, state_of):
    """Yield (state, turn dict) for every F turn; state_of(line, state) -> state."""
    state, since, prev, out = None, 0, None, []
    for line in lines(path):
        new = state_of(line, state)
        if new != state:
            state, since = new, 0
            continue
        if m := MDT.search(line):
            v = dict(kind=m[2], t=int(m[3]), js=int(m[4]), rn=int(m[5]), sd=int(m[6]),
                     draw_n=int(m[15]), draw_us=int(m[16]), band_n=int(m[18]), band_us=int(m[19]),
                     free=int(m[20]), mn=int(m[24]), gu=int(m[25]))
            if prev is not None:
                prev[1]["iv"] = v["t"] - prev[1]["t"]
            prev = [state, v, since]
            out.append(prev)
            since += 1
        elif (m := BGC.search(line)) and prev is not None:
            prev[1].update(st=int(m[2]), seg=int(m[3]), ras=int(m[4]), hit=int(m[5]), walk=int(m[6]),
                           cm=int(m[7] or 0), vw=int(m[8] or 0))
    return [(s, v) for s, v, k in out if s is not None and k >= SETTLE and v["kind"] == "F" and "iv" in v]


KEYS = ("iv", "js", "draw_us", "draw_n", "rn", "band_us", "band_n", "sd", "st", "seg", "hit", "walk", "mn",
        "cm", "vw")


def table(groups):
    return {k: {f: med([v.get(f, 0) for v in vs]) for f in KEYS} | {"n": len(vs)} for k, vs in groups.items()}


def race(args) -> int:
    def state_of(line, state):
        if m := BGX.search(line):
            return (int(m[1]), int(m[2]), int(m[3]))
        if (m := SCENE.search(line)) and m[1] != "race":
            return None
        return state

    groups = {}
    handles = {}
    plans = {}
    for path in args.logs:
        for s, v in turns(path, state_of):
            groups.setdefault(s, []).append(v)
        for line in lines(path):
            if m := REG.search(line):
                handles[m[2]] = m[1]
            elif m := BGP.search(line):
                for item in m[1].split():
                    h, rest = item.split("=")
                    n, us, st, sg = map(int, rest.split("/"))
                    name = re.sub(r"^r\d$", "runner", handles.get(h, h))
                    name = re.sub(r"^g\d$", "gallop", name)
                    p = plans.setdefault(name, [0, 0, 0, 0])
                    for i, x in enumerate((n, us, st, sg)):
                        p[i] += x
    t = table(groups)
    print("tier cam mode | n | fps | iv ms | js ms | draw ms (n) | js-draw ms | render ms | band ms (n) | send ms"
          " | steps | seg | hits | walk | min free | commit ms | patch ms")
    for k in sorted(t):
        r = t[k]
        print(f"{TIERS[k[0]]} {CAMS.get(k[1], k[1])} {MODES[k[2]]} | {r['n']} | {1e6 / r['iv']:.1f} | "
              f"{r['iv'] / 1e3:.1f} | {r['js'] / 1e3:.2f} | {r['draw_us'] / 1e3:.2f} ({r['draw_n']:.0f}) | "
              f"{(r['js'] - r['draw_us']) / 1e3:.2f} | {r['rn'] / 1e3:.2f} | {r['band_us'] / 1e3:.2f} "
              f"({r['band_n']:.0f}) | {r['sd'] / 1e3:.2f} | {r['st']:.0f} | {r['seg']:.0f} | {r['hit']:.0f} | "
              f"{r['walk']:.0f} | {r['mn']:.0f} | {r['cm'] / 1e3:.2f} | {r['vw'] / 1e3:.2f}")
    print("\ndifference from mode 'all' (positive = saved by removing)")
    print("tier cam mode | js ms | draw ms | render ms | band ms | iv ms | seg | steps | walk")
    for k in sorted(t):
        if k[2] == 0 or (k[0], k[1], 0) not in t:
            continue
        a, b = t[(k[0], k[1], 0)], t[k]
        print(f"{TIERS[k[0]]} {CAMS.get(k[1], k[1])} {MODES[k[2]]} | {(a['js'] - b['js']) / 1e3:.2f} | "
              f"{(a['draw_us'] - b['draw_us']) / 1e3:.2f} | {(a['rn'] - b['rn']) / 1e3:.2f} | "
              f"{(a['band_us'] - b['band_us']) / 1e3:.2f} | {(a['iv'] - b['iv']) / 1e3:.2f} | "
              f"{a['seg'] - b['seg']:.0f} | {a['st'] - b['st']:.0f} | {a['walk'] - b['walk']:.0f}")
    print("\nper plan (all logs, BGP windows): draws, us/draw, steps/draw, segments/draw")
    for name, (n, us, st, sg) in sorted(plans.items(), key=lambda x: -x[1][1]):
        print(f"{name} | {n} | {us / n:.0f} | {st / n:.0f} | {sg / n:.1f}")
    return 0


def synth(args) -> int:
    def state_of(line, state):
        if m := BGS.search(line):
            return (m[1], int(m[2]))
        return state

    groups = {}
    for path in args.logs:
        for s, v in turns(path, state_of):
            groups.setdefault(s, []).append(v)
    t = table(groups)
    print("case n | turns | band ms (n) | draw ms (n) | us/draw | js ms | render ms | steps | seg | hits | walk")
    for k in sorted(t, key=lambda k: (k[0], k[1])):
        r = t[k]
        per = r["draw_us"] / r["draw_n"] if r["draw_n"] else 0
        print(f"{k[0]} {k[1]} | {r['n']} | {r['band_us'] / 1e3:.3f} ({r['band_n']:.0f}) | "
              f"{r['draw_us'] / 1e3:.3f} ({r['draw_n']:.0f}) | {per:.0f} | {r['js'] / 1e3:.2f} | "
              f"{r['rn'] / 1e3:.2f} | {r['st']:.0f} | {r['seg']:.0f} | {r['hit']:.0f} | {r['walk']:.0f}")
    # Per geometry, the least-squares slope over n = 100..1000 of the band,
    # draw and JS time per segment: the intercepts (17 band fills, the anchor
    # dot) drop out. The document splits them (scan = 'off', hit + 1 px =
    # 'dot' - 'off', and so on).
    print("\nslope per segment (us): band | draw | js | hits | walk")
    for kind in sorted({k[0] for k in t if k[0] not in ("alu", "plot", "line", "sin")}):
        pts = [(k[1], t[k]) for k in t if k[0] == kind and k[1] >= 100]
        if len(pts) < 2:
            continue

        def slope(f):
            xs, ys = [p[0] for p in pts], [p[1][f] for p in pts]
            mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
            return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
        print(f"{kind} | {slope('band_us'):.3f} | {slope('draw_us'):.3f} | {slope('js'):.3f} | "
              f"{slope('hit'):.2f} | {slope('walk'):.1f}")
    return 0


def paint(args) -> int:
    groups, state, dirty, last = {}, None, True, None
    for path in args.logs:
        for line in lines(path):
            if m := BGX.search(line):
                state, dirty = (int(m[1]), int(m[2]), int(m[3])), True
            elif (m := SCENE.search(line)) and m[1] != "race":
                state, dirty = None, True
            elif m := PAINT.search(line):
                # The log's millisecond clock: 30 painted frames since the
                # previous window line, which is this window only when clean.
                at = int(m[1])
                if state is not None and not dirty and last is not None:
                    groups.setdefault(state, []).append((30000 / (at - last),) +
                                                        tuple(float(x) for x in m.groups()[1:]))
                dirty, last = False, at
    print("tier cam mode | windows | fps | turn ms | render ms | send ms")
    for k in sorted(groups):
        w = groups[k]
        print(f"{TIERS[k[0]]} {CAMS.get(k[1], k[1])} {MODES[k[2]]} | {len(w)} | "
              f"{med([x[0] for x in w]):.1f} | {med([x[1] for x in w]):.2f} | {med([x[2] for x in w]):.2f} | "
              f"{med([x[3] for x in w]):.2f}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--port", default="COM3")
    r.add_argument("--row", type=int, default=12)
    r.add_argument("--seconds", type=float, default=300)
    r.add_argument("--out", type=Path, required=True)
    for name in ("race", "synth", "paint"):
        s = sub.add_parser(name)
        s.add_argument("logs", type=Path, nargs="+")
    args = ap.parse_args()
    return {"run": run, "race": race, "synth": synth, "paint": paint}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
