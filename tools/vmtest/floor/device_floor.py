"""F-line measurement on the device (docs/vm/builtin-floor-plan.md sec.14).

Needs a CONFIG_POCKET_VM_FLOORPROBE=y build. For each shipped JS app (from
the menu) and the Kasane demo (USB 'K'): the guest's js= after each build
step ("FLOOR stage="), and at stop the F1/F2 path counters ("FLOORPROBE").
Then the same-binary microbenchmark (USB '('). Prints one JSON document.
    python tools/vmtest/floor/device_floor.py --port COM3 [--seconds 20] [--bench-only]
"""
import argparse, json, os, re, sys, time
import serial

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from device_runaway import wait  # noqa: E402

APPS = {0: "hello", 4: "imucal", 5: "pet", 6: "companion"}
STAGE = re.compile(r"FLOOR stage=(\S+) js=(\d+)")
MEM = re.compile(r"MEM free=(\d+) largest=(\d+) js=(\d+) frames=0")
PROBE = re.compile(r"FLOORPROBE (.*)")
# One line per measurement: the closing FLOORBENCH document is longer than
# the console's line and arrives cut, so the steps are what gets parsed.
STEP = re.compile(r"FLOORSTEP (\S+) (\d+)")


def read_for(port, seconds, until=None):
    lines, end = [], time.monotonic() + seconds
    while time.monotonic() < end:
        line = port.readline().decode(errors="replace").strip()
        if not line:
            continue
        lines.append(line)
        if any(s in line for s in ("Guru Meditation", "assert failed", "CORRUPT HEAP")):
            raise RuntimeError(line)
        if until and until in line:
            break
    return lines


def to_home(port):
    port.write(b"q")
    read_for(port, 10, "HOME_READY")


def start_menu(port, row):
    to_home(port)
    port.write(b"a"); wait(port, "CATEGORY 0", timeout=5)
    for _ in range(8):
        port.write(b"u"); time.sleep(0.05)
    for _ in range(row):
        port.write(b"d"); time.sleep(0.05)
    if row:
        wait(port, f"APP {row}", timeout=5)
    port.write(b"e")


def session(port, start, seconds):
    start()
    lines = read_for(port, seconds)
    port.write(b"q")
    lines += read_for(port, 20, "APP_STOPPED")
    to_home(port)
    text = "\n".join(lines)
    out = {"stages": [(s, int(j)) for s, j in STAGE.findall(text)]}
    m = MEM.search(text)
    if m:
        out["js_after_source"] = int(m.group(3))
    p = PROBE.search(text)
    if p:
        out["probe"] = dict(kv.split("=", 1) for kv in p.group(1).split())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--bench-only", action="store_true")
    args = ap.parse_args()
    result = {}
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        time.sleep(1.5); port.reset_input_buffer()
        to_home(port)
        if not args.bench_only:
            for row, name in APPS.items():
                result[name] = session(port, lambda r=row: start_menu(port, r), args.seconds)
                print(name, json.dumps(result[name]), flush=True)
            result["kasane_demo"] = session(port, lambda: port.write(b"K"), args.seconds)
            print("kasane_demo", json.dumps(result["kasane_demo"]), flush=True)
        # The microbenchmark: each loop x 5 rounds, one per frame, then a line.
        port.write(b"(")
        lines = read_for(port, 90, "FLOORBENCH")
        bench = {}
        for name, ms in STEP.findall("\n".join(lines)):
            bench.setdefault(name, []).append(int(ms))
        result["bench_ms"] = bench
        print("bench", json.dumps(bench), flush=True)
        port.write(b"q")
        read_for(port, 20, "APP_STOPPED")
        to_home(port)
    print(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
