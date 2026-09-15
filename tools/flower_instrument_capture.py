#!/usr/bin/env python3
"""Return to the home screen (back out of any overlay), then capture the flower scene.

    python3 tools/flower_instrument_capture.py --port /dev/ttyACM0 --seconds 100 --out <log>

The Cardputer's own keyboard is not reachable from a host, but the firmware reads
keystrokes from the USB-Serial-JTAG console as well (main.c's usb_stroke: 'q' = BACK,
'e' = ENTER, a/b/u/d = left/right/up/down), which is how the repo's own device scripts
drive the menus. An overlay ends the shell's menu, and shell_draw's PERF line only
prints while the home screen is up -- so one BACK is enough to start measuring.
"""
import argparse
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="/dev/ttyACM0")
ap.add_argument("--seconds", type=float, default=100.0)
ap.add_argument("--out", required=True)
ap.add_argument("--backs", type=int, default=3, help="BACK strokes to send first")
a = ap.parse_args()

lines: list[str] = []
with serial.Serial(a.port, 115200, timeout=0.3) as ser:
    time.sleep(0.5)
    ser.reset_input_buffer()
    for _ in range(a.backs):
        ser.write(b"q")
        ser.flush()
        time.sleep(0.4)
    deadline = time.monotonic() + a.seconds
    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").rstrip("\r\n")
        lines.append(line)
        print(line, flush=True)

with open(a.out, "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines) + "\n")
print(f"-> {a.out} ({len(lines)} lines)")
