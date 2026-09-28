"""Run the JS procedural megademo on COM3 and capture one presented frame."""

import argparse
from pathlib import Path
import re
import time

import serial


PIX = re.compile(rb"^PIX (\d+) ([0-9a-f]{960})\r?$", re.M)


def read_until(port: serial.Serial, log: bytearray, predicate, seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        log.extend(port.read(32768))
        if predicate(log):
            return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--trigger", choices=("J", "("), default="J")
    parser.add_argument("--capture-frame", type=int,
                        help="wait for this diagnostic PROC_JS frame before capturing")
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    log = bytearray()
    try:
        with port:
            if not read_until(port, log, lambda b: b"HOME_READY" in b, 12):
                port.write(b"q")
                if not read_until(port, log, lambda b: b"HOME_READY" in b, 12):
                    print("HOME_READY missing")
                    return 2
            start = len(log)
            port.write(args.trigger.encode("ascii"))
            if not read_until(port, log,
                              lambda b: b"KASANE_FRAME_PRESENTED" in b[start:], 20):
                print("JS app did not present a Kasane frame")
                return 2
            if args.capture_frame is None:
                time.sleep(0.25)
            else:
                marker = f"PROC_JS: frame={args.capture_frame} ".encode()
                if not read_until(port, log, lambda b: marker in b[start:], 20):
                    print(f"PROC_JS frame {args.capture_frame} missing")
                    return 2
            port.write(b"s")
            capture = len(log)
            if not read_until(port, log,
                              lambda b: b"CAPTURE_END" in b[capture:] and
                              len(PIX.findall(bytes(b[capture:]))) >= 135,
                              20):
                print("Complete RGB565 capture missing")
                return 2
            port.write(b"q")
            if not read_until(port, log,
                              lambda b: b"HOME_READY" in b[capture:], 15):
                print("HOME_READY missing after JS app")
                return 2
            print("JS Kasane presented, captured 135 rows, returned HOME_READY")
            return 0
    finally:
        args.out.write_bytes(log)
        print(f"Saved {len(log)} bytes to {args.out}")


if __name__ == "__main__":
    raise SystemExit(main())
