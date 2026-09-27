"""Per-app guest heap at start on the device: the F gate's js= number.

Starts each JS app from the menu, keeps the first "MEM free= largest= js="
line after APP_ID (app_session.c prints it once the source is evaluated, with
frames=0), stops the app, and repeats. The line is the device's own
accounting (tlsf block lengths), so it is what the F line promises to lower
(docs/vm/builtin-floor-plan.md sec.8).
    python tools/vmtest/floor/device_js.py --port COM3 [--rounds 3]
"""
import argparse, json, re, sys, time
import serial

sys.path.insert(0, __file__.rsplit("floor", 1)[0])
from device_runaway import wait  # noqa: E402

APPS = {0: "hello", 4: "imucal", 5: "pet", 6: "companion"}
MEM = re.compile(r"MEM free=(\d+) largest=(\d+) js=(\d+) frames=0")


def start_row(port, row):
    port.write(b"q"); wait(port, "HOME_READY", timeout=10)
    port.write(b"a"); wait(port, "CATEGORY 0", timeout=5)
    for _ in range(8):
        port.write(b"u"); time.sleep(0.05)
    for _ in range(row):
        port.write(b"d"); time.sleep(0.05)
    if row:
        wait(port, f"APP {row}", timeout=5)
    port.write(b"e")


def one(port, row):
    start_row(port, row)
    text = wait(port, "APP_ID", timeout=15)
    deadline = time.monotonic() + 10
    m = MEM.search(text)
    while not m and time.monotonic() < deadline:
        line = port.readline().decode(errors="replace")
        m = MEM.search(line)
    port.write(b"q")
    wait(port, "APP_STOPPED", timeout=20)
    wait(port, "HOME_READY", timeout=10)
    if not m:
        raise RuntimeError(f"no MEM line for row {row}")
    return tuple(int(g) for g in m.groups())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--rounds", type=int, default=3)
    args = ap.parse_args()
    out = {}
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        time.sleep(1.5); port.reset_input_buffer()
        port.write(b"q"); wait(port, "HOME_READY")
        for r in range(args.rounds):
            for row, name in APPS.items():
                out.setdefault(name, []).append(one(port, row))
    for name, runs in out.items():
        js = [x[2] for x in runs]
        print(f"{name:10s} js={js} free={[x[0] for x in runs]} largest={[x[1] for x in runs]}")
    print(json.dumps({k: [x[2] for x in v] for k, v in out.items()}))


if __name__ == "__main__":
    main()
