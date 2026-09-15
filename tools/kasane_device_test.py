"""Run the time-varying pocket.kasane diagnostic over USB."""
import argparse
import re
import time
import serial
from pathlib import Path


parser = argparse.ArgumentParser()
parser.add_argument("--port", required=True)
parser.add_argument("--ticks", type=int, default=300)
parser.add_argument("--out", type=Path, help="directory for the full serial log")
args = parser.parse_args()
if args.out:
    args.out.mkdir(parents=True, exist_ok=True)
serial_log = []
port = serial.Serial(args.port, 115200, timeout=0.15)


def wait_for(marker, timeout):
    deadline = time.monotonic() + timeout
    seen = []
    while time.monotonic() < deadline:
        line = port.readline().decode(errors="replace").strip()
        if line:
            serial_log.append(line)
            seen.append(line)
            if any(word in line for word in ("KASANE", "START_FAILED", "panic")):
                print(line, flush=True)
        if marker in line:
            return seen
    raise RuntimeError(f"waiting for {marker}: {seen[-12:]}")


try:
    time.sleep(1.0)
    port.reset_input_buffer()
    port.write(b"q")
    wait_for("HOME_READY", 8)
    port.reset_input_buffer()
    port.write(b"K")
    log = wait_for(f"KASANE_TICK {args.ticks} ", max(15, args.ticks / 20))
    text = "\n".join(log)
    required = ["KASANE_READY active=true", "KASANE_FRAME_PRESENTED",
                "KASANE_TICK 120 scope=modal", "KASANE_TICK 180 scope=modal",
                "KASANE_TICK 240 scope=app", f"KASANE_TICK {args.ticks} scope=app"]
    missing = [marker for marker in required if marker not in text]
    if missing or "START_FAILED" in text or "panic" in text.lower():
        raise RuntimeError(f"Kasane diagnostic failed; missing={missing}")
    paints = [tuple(map(float,m)) for m in re.findall(
        r"KASANE_PAINT turn_ms=([\d.]+) render_ms=([\d.]+) send_ms=([\d.]+)", text)]
    if len(paints)<3:
        raise RuntimeError(f"only {len(paints)} timing windows")
    mean = tuple(sum(row[i] for row in paints)/len(paints) for i in range(3))
    print("KASANE_DEVICE PASS " + " ".join(
        f"{name}={value:.2f}" for name,value in zip(("turn_ms","render_ms","send_ms"),mean)))
    port.write(b"q")
    wait_for("HOME_READY", 8)
finally:
    port.close()
    if args.out:
        (args.out / "serial.log").write_text("\n".join(serial_log)+"\n", encoding="utf-8")
