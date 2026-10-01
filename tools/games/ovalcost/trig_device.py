"""DERBY WATCH on the device for docs/apps/derby-trig-cull-device.md.

  run  --bin X.bin --boot DIR --out log [--seconds 420] [--heavy] [--no-flash]
       flash (bootloader and partition table from --boot), wait 6 s, open
       DERBY (row 12), press tab once at the paddock if --heavy (USB key
       injection, POCKET_KEYTEST), let the demo run, Back, save the raw log.
  sum  log... : per (tier, course, camera) of the race turns, after 3 settling
       turns: frame rate, JS, VM draw, least mn, most gu; per tier the least
       mn (all turns and gate turns) and per race the guest maximum (S/O).

The image: -DKASANE_MEGADEMO_TRACE=ON -DKASANE_BGCOST_TRACE=ON -DPOCKET_KEYTEST=ON.
ESP-IDF Python (pyserial, esptool).
"""
import argparse, re, statistics, subprocess, sys, time
from pathlib import Path

MDT = re.compile(r"MDT (\d+) (\w) t=(\d+) js=(\d+) rn=(\d+) sd=(\d+) by=(\d+) bd=(\d+) pr=(\d+) "
                 r"reg=(\d+)/(\d+) prep=(\d+)/(\d+) un=(\d+) draw=(\d+)/(\d+)/(\d+) band=(\d+)/(\d+) "
                 r"free=(\d+) lg=(\d+) k=([0-9a-f]+)/([0-9a-f]+) mn=(\d+) gu=(\d+)")
BAD = ("Guru Meditation", "START_FAILED", "RUNAWAY", "abort()", "Uncaught", "app: OOM",
       "FRAMEFAIL", "LOADFAIL", "DEMOFAIL", "LOADSTALL", "DEGRADE")
TIERS = ("LIGHT", "MID", "HEAVY")
SETTLE = 3


def run(a):
    if not a.no_flash:
        b = Path(a.boot)
        subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", a.port, "-b", "460800",
                        "--before", "default-reset", "--after", "hard-reset", "write-flash",
                        "--flash-mode", "dio", "--flash-size", "8MB", "--flash-freq", "80m",
                        "0x0", str(b / "bootloader.bin"), "0x8000", str(b / "partition-table.bin"),
                        "0x10000", a.bin], check=True, stdout=subprocess.DEVNULL)
    import serial
    port = serial.Serial(port=None, baudrate=115200, timeout=0.05)
    port.dtr = False
    port.rts = False
    port.port = a.port
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
        pump(6)
        port.write(b"q")
        if not pump(15, b"HOME_READY"):
            raise RuntimeError("no HOME_READY")
        port.write(b"a")
        pump(5, b"CATEGORY 0")
        for _ in range(16):
            port.write(b"u"); pump(0.05)
        for _ in range(12):
            port.write(b"d"); pump(0.05)
        if not pump(5, b"APP 12"):
            raise RuntimeError("row not reached")
        start = len(log)
        port.write(b"e")
        if not pump(20, b"DERBY LOADED", start):
            raise RuntimeError("no LOADED")
        if a.heavy:
            pump(1.5)
            port.write(b"\x1fK+0e\n"); pump(0.15); port.write(b"\x1fK-0e\n")
        pump(a.seconds)
        got = bytes(log[start:]).decode("utf-8", "replace")
        bad = [x for x in BAD if x in got]
        mark = len(log)
        port.write(b"q")
        pump(15, b"HOME_READY", mark)
        print("RUN", "FAIL " + ",".join(bad) if bad else "PASS")
        status = 2 if bad else 0
    except RuntimeError as e:
        print("RUN FAIL", e)
        status = 2
    finally:
        port.close()
        Path(a.out).write_bytes(log)
    return status


def med(v):
    return statistics.median(v) if v else float("nan")


def load(path):
    text = Path(path).read_bytes().replace(b"\x00", b"").decode("utf-8", "replace")
    tier, oval_next, oval, scene, cam, race = 1, 0, 0, None, None, -1
    key, since, prev, out = None, 0, None, []
    races = []  # per race: (oval, max gu)
    for line in text.splitlines():
        if m := re.search(r"DERBY ODDS \S+( OVAL)?", line):
            oval_next = 1 if m[1] else 0
        elif m := re.search(r"DERBY TIER (\d)", line):
            tier = int(m[1])
        elif m := re.search(r"DERBY SCENE (\w+)", line):
            scene = m[1]
            if scene == "gate":
                oval = oval_next
                races.append([oval, 0, tier])
            cam = None
        elif m := re.search(r"DERBY CAM (.+?)\s*$", line):
            cam = m[1].strip()
        elif m := MDT.search(line):
            v = dict(kind=m[2], t=int(m[3]), js=int(m[4]), rn=int(m[5]), sd=int(m[6]),
                     draw_n=int(m[15]), draw_us=int(m[16]), mn=int(m[24]), gu=int(m[25]))
            st = (tier, oval, scene, cam if scene == "race" else None)
            if st != key:
                key, since = st, 0
            if prev is not None:
                prev[1]["iv"] = v["t"] - prev[1]["t"]
            prev = [st, v, since]
            out.append(prev)
            since += 1
            if races and scene in ("gate", "race", "photo"):
                races[-1][1] = max(races[-1][1], v["gu"])
    return out, races


def summ(a):
    groups, allturns, allraces = {}, [], []
    for p in a.logs:
        out, races = load(p)
        allturns += out
        allraces.append(races)
        for st, v, k in out:
            if v["kind"] == "F" and "iv" in v and k >= SETTLE and st[2] == "race":
                groups.setdefault((st[0], st[1], st[3]), []).append(v)
    print("tier course cam | n | fps | iv ms | js ms | draw ms (n) | min mn | max gu")
    for k in sorted(groups, key=lambda k: (k[0], k[1], str(k[2]))):
        g = groups[k]
        iv = med([x["iv"] for x in g])
        print(f"{TIERS[k[0]]} {'OVAL' if k[1] else 'STR'} {k[2]} | {len(g)} | {1e6 / iv:.1f} | {iv / 1e3:.1f} | "
              f"{med([x['js'] for x in g]) / 1e3:.2f} | {med([x['draw_us'] for x in g]) / 1e3:.2f} "
              f"({med([x['draw_n'] for x in g]):.0f}) | {min(x['mn'] for x in g)} | {max(x['gu'] for x in g)}")
    for tier in (1, 2):
        ts = [v for st, v, k in allturns if st[0] == tier]
        if not ts:
            continue
        gate = [v["mn"] for st, v, k in allturns if st[0] == tier and st[2] == "gate"]
        first = [v["mn"] for st, v, k in allturns if st[0] == tier and st[2] is not None]
        print(f"{TIERS[tier]}: turn min mn {min(v['mn'] for v in ts)} (excluding first turn of run: "
              f"{min(first[1:]) if len(first) > 1 else '-'}) | gate min mn {min(gate) if gate else '-'} | "
              f"max gu {max(v['gu'] for v in ts)}")
    for p, r in zip(a.logs, allraces):
        print(Path(p).name, "races:", " ".join(f"{x[1]}{'O' if x[0] else 'S'}" for x in r))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--port", default="COM3")
    r.add_argument("--bin", required=True)
    r.add_argument("--boot", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--seconds", type=float, default=420)
    r.add_argument("--heavy", action="store_true")
    r.add_argument("--no-flash", action="store_true")
    s = sub.add_parser("sum")
    s.add_argument("logs", nargs="+")
    a = ap.parse_args()
    return run(a) if a.cmd == "run" else summ(a)


if __name__ == "__main__":
    sys.exit(main() or 0)
