"""Trigger the optional Kasane megademo probe and save its serial trace."""

import argparse
from pathlib import Path
import time

import serial


def collect(port: serial.Serial, log: bytearray, marker: bytes, seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while marker not in log and time.monotonic() < deadline:
        log.extend(port.read(32768))
    return marker in log


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    log = bytearray()
    try:
        with port:
            if not collect(port, log, b"HOME_READY", 12):
                port.write(b"q")
                if not collect(port, log, b"HOME_READY", 12):
                    print("HOME_READY missing")
                    return 2
            time.sleep(1)
            start = len(log)
            for _ in range(3):
                port.write(b"`")
                if collect(port, log, b"KSN_MEGA: START", 3):
                    break
            if b"KSN_MEGA: START" not in log[start:]:
                print("KSN_MEGA START missing")
                return 2
            if not collect(port, log, b"KSN_MEGA: END", 90):
                print("KSN_MEGA END missing")
                return 2
            for line in bytes(log[start:]).splitlines():
                if b"KSN_MEGA:" in line:
                    print(line.decode("utf-8", "replace"))
            return 0 if b"KSN_MEGA: PASS" in log[start:] and b"KSN_MEGA: FAIL" not in log[start:] else 2
    finally:
        args.out.write_bytes(log)
        print(f"Saved {len(log)} bytes to {args.out}")


if __name__ == "__main__":
    raise SystemExit(main())
