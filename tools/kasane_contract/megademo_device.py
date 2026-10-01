"""Drive the built-in MEGADEMO on the device and summarise it per scene and tier.

run     launches MEGADEMO from the APPS menu (the same presses as
        tools/kasane_megademo_menu_device.py), selects a load tier with DOWN,
        sends an optional timed key schedule, runs for --seconds, leaves with
        Back and requires APP_STOPPED + HOME_READY. The raw serial log is
        written to --out. Fails on Guru Meditation, START_FAILED, RUNAWAY,
        OOM, PRESENTER_STEP_FAILED, an early return home or a JS exception.
analyze reads such logs. With a KASANE_MEGADEMO_TRACE image the MDT lines give
        per-turn JS / render / send / register / draw / heap numbers, grouped by
        the MEGADEMO SCENE line that precedes them. A normal image has only
        KASANE_PAINT (30-frame averages), which is summarised per window.

Keys (main.c usb_stroke): e Enter, q Back, u UP, d DOWN, a LEFT, b RIGHT.
The app maps UP/DOWN to tier +1/-1 and LEFT/RIGHT to the previous/next scene.
"""
from __future__ import annotations

import argparse
import re
import statistics
import sys
import time
from collections import defaultdict
from pathlib import Path

BAD = (b"Guru Meditation", b"START_FAILED", b"RUNAWAY", b"PRESENTER_STEP_FAILED",
       b"abort()", b"Uncaught", b"app: OOM")
SCENE = re.compile(r"MEGADEMO SCENE (\w+) tier=(\d) plans=(\d+) freed=(\d+) registered=(\d+)")
MDT = re.compile(r"MDT (\d+) (\w) t=(\d+) js=(\d+) rn=(\d+) sd=(\d+) by=(\d+) bd=(\d+) pr=(\d+) "
                 r"reg=(\d+)/(\d+) prep=(\d+)/(\d+) un=(\d+) draw=(\d+)/(\d+)/(\d+) band=(\d+)/(\d+) "
                 r"free=(\d+) lg=(\d+) k=([0-9a-f]+)/([0-9a-f]+)(?: mn=(\d+))?(?: gu=(\d+))?"
                 r"(?: cv=(\d+)/(\d+)/(\d+))?")
MDTJ = re.compile(r"MDTJ js=(\d+) cost_us=(\d+)")
PAINT = re.compile(r"\((\d+)\) kasane: KASANE_PAINT turn_ms=([0-9.]+) render_ms=([0-9.]+) send_ms=([0-9.]+) "
                   r"bytes=(\d+) bands=(\d+)")
VARIANT = re.compile(r"MDX (?:zv=\d+ )?(.*)$")
MEM = re.compile(r"MEM free=(\d+) largest=(\d+) js=(\d+)")
FIELDS = ("seq", "kind", "t", "js", "rn", "sd", "by", "bd", "pr", "reg_n", "reg_us", "prep_us",
          "prep_max", "un", "draw_n", "draw_us", "draw_max", "band_n", "band_us", "free", "lg",
          "btn", "fed", "mn", "gu", "cm", "vw", "sp")


def open_port(name: str):
    import serial
    port = serial.Serial(port=None, baudrate=115200, timeout=0.05)
    port.dtr = False
    port.rts = False
    port.port = name
    port.open()
    return port


def run(args) -> int:
    log = bytearray()
    port = open_port(args.port)
    status = 0

    def pump(seconds: float, stop: bytes | None = None, since: int = 0) -> bytes:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            log.extend(port.read(65536))
            got = bytes(log[since:])
            for marker in BAD:
                if marker in got:
                    raise RuntimeError(f"{marker!r}: {got[-1500:]!r}")
            if stop and stop in got:
                return got
        if stop:
            raise RuntimeError(f"missing {stop!r}: {bytes(log[-1500:])!r}")
        return bytes(log[since:])

    def command(key: bytes, marker: bytes, seconds: float = 12) -> bytes:
        start = len(log)
        port.write(key)
        return pump(seconds, marker, start)

    try:
        time.sleep(0.3)
        port.reset_input_buffer()
        command(b"q", b"HOME_READY")
        for key in args.pre:
            port.write(key.encode())
            pump(0.4)
        command(b"a", b"CATEGORY 0")
        for _ in range(12):
            command(b"u", b"APP ")
        for index in range(1, 9):
            command(b"d", f"APP {index}".encode())
        start = len(log)
        launched = command(b"e", b"MEGADEMO SCENE", 20)
        if b"APP_ID local.megademo" not in launched:
            raise RuntimeError("unexpected app identity")
        # Keys sent before the first presented frame are dropped (the input
        # scope is blocked until then), so wait for the set to be on screen.
        pump(10, b"MEGADEMO VIEW", start)
        for level in range(1, 3 - args.tier):
            command(b"d", f"tier={2 - level} ".encode())
        began = time.monotonic()
        # Captures: wait for the scene's next SCENE line, then delay, then 's'
        # (board_capture of the next presented frame, 135 PIX rows).
        for item in (x for x in args.shots.split(",") if x):
            name, delay = item.split(":")
            mark = len(log)
            pump(40, f"MEGADEMO SCENE {name} tier=".encode(), mark)
            pump(float(delay), None, mark)
            command(b"s", b"CAPTURE_END", 25)
        schedule = sorted((float(at), key) for at, key in
                          (item.split(":") for item in args.keys.split(",") if item))
        for at, key in schedule:
            wait = at - (time.monotonic() - began)
            if wait > 0:
                pump(wait, None, start)
            # Host-clock latency from the key's write to the app's answer
            # (USB both ways included): a scene/tier key logs SCENE.
            mark, sent = len(log), time.monotonic()
            port.write(key.encode())
            if key in "abud":
                pump(3, b"MEGADEMO SCENE", mark)
                print(f"KEY {key} {1000 * (time.monotonic() - sent):.0f} ms")
        rest = args.seconds - (time.monotonic() - began)
        if rest > 0:
            got = pump(rest, None, start)
        got = bytes(log[start:])
        if b"APP_STOPPED" in got or b"HOME_READY" in got:
            raise RuntimeError("MEGADEMO returned home before Back")
        mark, sent = len(log), time.monotonic()
        port.write(b"q")
        pump(15, b"APP_STOPPED", mark)
        print(f"BACK {1000 * (time.monotonic() - sent):.0f} ms to APP_STOPPED")
        pump(10, b"HOME_READY", mark)
        print("MEGADEMO_DEVICE_RUN PASS")
    except RuntimeError as error:
        print(f"MEGADEMO_DEVICE_RUN FAIL {error}"[:2000])
        status = 2
    finally:
        try:
            pump(0.5)
        except RuntimeError:
            pass
        port.close()
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(log)
    return status


def pct(values, p):
    values = sorted(values)
    if not values:
        return 0
    return values[min(len(values) - 1, int(round(p / 100 * (len(values) - 1))))]


def analyze(args) -> int:
    for path in args.logs:
        text = path.read_bytes().decode("utf-8", "replace")
        print(f"== {path.name}")
        for line in text.splitlines():
            if any(s in line for s in ("DEGRADE", "PocketError", "MEM free", "MDT_LIMIT",
                                       "APP_ID", "START_FAILED", "OOM", "RUNAWAY")):
                print("  " + line.strip()[:200])
        turns = []           # (scene key, dict)
        key = None
        visits = []          # [scene key, frames]
        jheap = []
        paints = []
        for line in text.splitlines():
            if m := SCENE.search(line):
                key = (int(m[2]), m[1])
                visits.append([key, 0, int(m[3])])
            elif (m := VARIANT.search(line)) and key:
                # An experiment image tags the visit it builds (MDX ...).
                key = (key[0], key[1].split("[")[0] + "[" + m[1] + "]")
                visits[-1][0] = key
            elif (m := MDT.search(line)) and key:
                row = dict(zip(FIELDS, m.groups()))
                row["mn"] = row["mn"] or row["free"]
                row["gu"] = row["gu"] or 0
                for name in ("cm", "vw", "sp"):
                    row[name] = row[name] or 0
                # Frames this visit had completed when the turn began, and
                # the visit before it: a split present at frame 0 pairs the
                # new scene's image with the previous scene's view.
                row["vf"] = visits[-1][1] if visits else 0
                row["prev"] = visits[-2][0][1] if len(visits) > 1 else "-"
                for name in FIELDS:
                    if name not in ("kind", "btn", "fed"):
                        row[name] = int(row[name])
                row["btn"] = int(row["btn"], 16)
                row["fed"] = int(row["fed"], 16)
                turns.append((key, row))
                if row["kind"] in "FC" and visits:
                    visits[-1][1] += 1
            elif m := MDTJ.search(line):
                jheap.append((key, int(m[1]), int(m[2])))
            elif m := PAINT.search(line):
                paints.append((key, *map(float, m.groups()[1:4]), int(m[5])))
        if turns:
            summarize_turns(turns, jheap, visits)
        elif paints:
            print("  KASANE_PAINT windows (30 painted frames each, averages)")
            for k, turn, render, send, nbytes in paints:
                print(f"  {k} turn={turn:.2f} render={render:.2f} send={send:.2f} bytes={nbytes}")
    return 0


def summarize_turns(turns, jheap, visits) -> None:
    groups = defaultdict(list)
    for index, (key, row) in enumerate(turns):
        groups[key].append((index, row))
    # Frame completion: a frame() that returned (F) or a continuation that
    # finished one (C). Interval between successive completions' present end.
    done = [(i, key, row) for i, (key, row) in enumerate(turns) if row["kind"] in "FC"]
    interval = {}
    for (i0, _, a), (i1, k1, b) in zip(done, done[1:]):
        interval[i1] = (b["t"] + b["js"] + b["rn"] + b["sd"] - a["t"] - a["js"] - a["rn"] - a["sd"]) / 1000
    print("  tier scene    frames  js_ms(med/p95/max)  render_ms(med/max)  send_ms(med/max)  "
          "draw_ms(med/max) band_ms(med/max) bands  int_ms(med/p95/max)  fps  parks  free_min lg_min mn_min  gu_max")
    order = {"NEWS": 0, "TWIST": 1, "ZENITH": 2, "LIMIT": 3}
    for key in sorted(groups, key=lambda k: (k[0], order.get(k[1].split("[")[0], 9), k[1])):
        rows = [r for _, r in groups[key]]
        frames = [r for r in rows if r["kind"] in "FC"]
        # A parked frame() spans a Q turn and its C/P continuations: sum them.
        js, cur = [], 0
        for r in rows:
            cur += r["js"]
            if r["kind"] in "FC":
                js.append(cur / 1000)
                cur = 0
        rn = [r["rn"] / 1000 for r in rows if r["pr"]]
        sd = [r["sd"] / 1000 for r in rows if r["pr"]]
        dr = [r["draw_us"] / 1000 for r in rows if r["draw_n"]]
        bd = [r["band_us"] / 1000 for r in rows if r["pr"]]
        bn = [r["band_n"] for r in rows if r["pr"]]
        iv = [interval[i] for i, r in groups[key] if i in interval]
        parks = sum(r["kind"] in "QP" for r in rows)
        fps = 1000 / statistics.mean(iv) if iv else 0
        print(f"  {key[0]}    {key[1]:<8} {len(frames):5d}  "
              f"{statistics.median(js):5.1f}/{pct(js, 95):5.1f}/{max(js):5.1f}   "
              f"{statistics.median(rn) if rn else 0:5.1f}/{max(rn, default=0):5.1f}    "
              f"{statistics.median(sd) if sd else 0:5.1f}/{max(sd, default=0):5.1f}   "
              f"{statistics.median(dr) if dr else 0:5.1f}/{max(dr, default=0):5.1f}  "
              f"{statistics.median(bd) if bd else 0:5.1f}/{max(bd, default=0):5.1f}  "
              f"{max(bn, default=0):3d}  "
              f"{statistics.median(iv) if iv else 0:5.1f}/{pct(iv, 95):5.1f}/{max(iv, default=0):6.1f}  "
              f"{fps:4.1f}  {parks:4d}  {min(r['free'] for r in rows):6d} {min(r['lg'] for r in rows):6d} {min(r['mn'] for r in rows):6d} {max(r['gu'] for r in rows):6d}")
    regs = [r for _, r in turns if r["reg_n"]]
    if regs:
        per = [r["prep_max"] for r in regs]
        print(f"  register: turns={len(regs)} plans={sum(r['reg_n'] for r in regs)} "
              f"per-turn reg_ms max={max(r['reg_us'] for r in regs) / 1000:.2f} "
              f"prep_us per plan max={max(per)} median={statistics.median(per)} "
              f"per-turn prep_ms max={max(r['prep_us'] for r in regs) / 1000:.2f}")
    switch = []
    for i, (key, row) in enumerate(turns):
        if row["reg_n"] and row["un"] and row["kind"] in "FCQ":
            switch.append(interval.get(i, 0))
    if switch:
        print(f"  switch turns (register+unregister): n={len(switch)} interval max={max(switch):.1f} ms "
              f"median={statistics.median(switch):.1f} ms")
    # Input: turns from the one a key was handed to the one frame() got it.
    lag = []
    pending = None
    for i, (_, row) in enumerate(turns):
        if row["btn"] and pending is None:
            pending = (i, row["btn"], row["t"])
        if pending and row["fed"] & pending[1]:
            lag.append((i - pending[0], (row["t"] - pending[2]) / 1000))
            pending = None
    if lag:
        print(f"  input: presses={len(lag)} turns-to-frame max={max(t for t, _ in lag)} "
              f"ms max={max(m for _, m in lag):.1f}")
    if jheap:
        print(f"  guest heap samples: n={len(jheap)} max={max(j for _, j, _ in jheap)} "
              f"cost_us max={max(c for _, _, c in jheap)}")
    split = [(key, row) for key, row in turns if row["sp"]]
    if any("cm" in r and r["cm"] for _, r in turns):
        first = [f"{row['prev']}->{key[1]}@{key[0]}({row['kind']})" for key, row in split if row["vf"] == 0]
        print(f"  split presents (commit shown before its replace/patch): turns={len(split)} "
              f"presents={sum(r['sp'] for _, r in split)} at a visit's first frame={len(first)}"
              + (": " + ", ".join(first) if first else ""))
    loops = defaultdict(int)
    for k, n, _ in visits[:-1]:
        loops[k] = max(loops[k], n)
    print("  frames per completed visit (max): " + ", ".join(f"{k[1]}@{k[0]}={n}" for k, n in loops.items()))


W, H = 240, 135
LENS = {"NEWS0": 16, "NEWS1": 16, "NEWS2": 16, "TWIST": 128, "ZENITH": 96, "LIMIT": 96}
TOTAL = {"NEWS0": 5, "NEWS1": 5, "NEWS2": 5, "TWIST": 7, "ZENITH": 16, "LIMIT": 14}
INDEX = {"NEWS0": 0, "NEWS1": 1, "NEWS2": 2, "TWIST": 3, "ZENITH": 4, "LIMIT": 5}
# HUD rectangles over the full-screen procedural image (x0, y0, x1, y1), with
# the rotated radar and thumbnail's swept extents; pixels inside are not
# compared. NEWS and TWIST (after its iris) are the image alone.
MASKS = {"NEWS0": [], "NEWS1": [], "NEWS2": [], "TWIST": [(4, 2, 236, 26)],
         "ZENITH": [(2, 0, 152, 15), (158, 2, 198, 13), (224, 1, 238, 13), (0, 72, 64, 135),
                    (168, 78, 240, 135), (170, 14, 238, 66)],
         "LIMIT": [(2, 0, 152, 15), (224, 1, 238, 13), (0, 72, 64, 135), (58, 116, 238, 135)]}


def png(path: Path, width: int, height: int, rgb: bytes) -> None:
    import struct
    import zlib

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    raw = b"".join(bytes(1) + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def rgb888(v: int) -> tuple:
    return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def host565(path: Path) -> list:
    data = path.read_bytes()
    header = b"P6\n240 135\n255\n"
    rgb = data[len(header):]
    return [((rgb[i] >> 3) << 11) | ((rgb[i + 1] >> 2) << 5) | (rgb[i + 2] >> 3)
            for i in range(0, len(rgb), 3)]


def shots(args) -> int:
    """Extract every capture, name it by scene/tier/frame, compare with host."""
    args.out.mkdir(parents=True, exist_ok=True)
    found = []
    for path in args.logs:
        lines = path.read_bytes().decode("utf-8", "replace").splitlines()
        scene, tier, plans, frames, rows, capturing, want = None, 0, 0, 0, {}, False, None
        news = -1
        for line in lines:
            if m := SCENE.search(line):
                # Act I is three NEWS scenes in a row (host scenes 0..2).
                # A tier key re-enters the same scene at the new tier.
                if m[1] == "NEWS" and scene == "NEWS":
                    news += int(m[2]) == tier
                else:
                    news = 0
                scene, tier, plans, frames = m[1], int(m[2]), int(m[3]), -1
            elif "CAPTURE_BEGIN" in line:
                capturing, rows = True, {}
            elif "CAPTURE_END" in line:
                capturing, want = False, dict(rows)
                if not any(MDT.search(x) for x in lines[:200]) and len(want) == H:
                    # A normal image has no MDT lines to count frames by:
                    # the frame is found by matching the host frames below.
                    found.append((scene if scene != "NEWS" else f"NEWS{news}", tier, None,
                                  plans, want, "?"))
                    want = None
            elif capturing and (m := re.search(r"PIX (\d+) ([0-9a-f]{960})", line)):
                rows[int(m[1])] = [int(m[2][x:x + 4], 16) for x in range(0, 960, 4)]
            elif (m := MDT.search(line)) and scene:
                if m[2] in "FQ":
                    frames += 1
                if want is not None:
                    if len(want) == H:
                        found.append((scene if scene != "NEWS" else f"NEWS{news}", tier, frames,
                                      plans, want, m[2]))
                    want = None
    print(f"captures: {len(found)}")
    cells = {}
    for scene, tier, t, plans, rows, kind in found:
        pixels = [v for y in range(H) for v in rows[y]]
        if t is None and args.host:
            t, best = 0, None
            for u in range(LENS[scene]):
                path = args.host / f"t{tier}_s{INDEX[scene]}_f{u:03}_0.ppm"
                if not path.exists():
                    continue
                ref = host565(path)
                n = sum(pixels[i] != ref[i] for i in range(0, W * H, 7))
                if best is None or n < best:
                    t, best = u, n
            plans = TOTAL[scene]  # matched frames are taken as fully loaded
        name = f"{scene.lower()}_t{tier}_f{t:03}"
        png(args.out / f"{name}.png", W, H, bytes(c for v in pixels for c in rgb888(v)))
        cells.setdefault((tier, scene), (name, pixels))
        full = plans + t >= TOTAL[scene]
        note = f"{scene} tier={tier} t={t} kind={kind} loaded={'all' if full else plans + t}"
        if args.host and scene in INDEX and full:
            s = INDEX[scene]
            surface0 = t - 1 if s in (4, 5) and (t & 3) == 3 else t
            ref = host565(args.host / f"t{tier}_s{s}_f{surface0:03}_0.ppm")
            masked = [(x, y) for y in range(H) for x in range(W)
                      if not any(a <= x < c and b <= y < d for a, b, c, d in MASKS[scene])]
            diff = [(x, y) for x, y in masked if pixels[y * W + x] != ref[y * W + x]]
            note += f" compared={len(masked)} differing={len(diff)}"
            if scene == "TWIST" and t < 20:
                note += " (iris still opening: outside the clip is the view)"
        print("  " + note)
    # One sheet: rows LIGHT, MID, HEAVY; columns TWIST, ZENITH, LIMIT.
    gap, cols = 2, ["TWIST", "ZENITH", "LIMIT"]
    width, height = 3 * W + 2 * gap, 3 * H + 2 * gap
    sheet = bytearray(bytes((48, 48, 48)) * width * height)
    for r, tier in enumerate((0, 1, 2)):
        for c, scene in enumerate(cols):
            if (tier, scene) not in cells:
                continue
            pixels = cells[(tier, scene)][1]
            for y in range(H):
                for x in range(W):
                    at = ((r * (H + gap) + y) * width + c * (W + gap) + x) * 3
                    sheet[at:at + 3] = bytes(rgb888(pixels[y * W + x]))
    png(args.out / "sheet.png", width, height, bytes(sheet))
    print(f"wrote {args.out / 'sheet.png'} and {len(found)} captures")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--port", default="COM3")
    r.add_argument("--out", type=Path, required=True)
    r.add_argument("--tier", type=int, default=2, choices=(0, 1, 2))
    r.add_argument("--seconds", type=float, default=30)
    r.add_argument("--keys", default="", help="comma list of seconds:key, e.g. 5:b,6:a")
    r.add_argument("--shots", default="", help="comma list of SCENE:seconds captures, e.g. TWIST:2.5")
    r.add_argument("--pre", default="", help="keys sent at HOME before launching (e.g. ~ in a trace image)")
    c = sub.add_parser("shots")
    c.add_argument("logs", type=Path, nargs="+")
    c.add_argument("--out", type=Path, required=True)
    c.add_argument("--host", type=Path, help="host frames from run_proc_megademo_scenes.py --out")
    a = sub.add_parser("analyze")
    a.add_argument("logs", type=Path, nargs="+")
    args = parser.parse_args()
    return {"run": run, "shots": shots, "analyze": analyze}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
