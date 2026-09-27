"""Trigger the optional Kasane procedural/compiler diagnostic over USB serial."""

import argparse
from pathlib import Path
import time

import serial


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
    with port:
        deadline = time.monotonic() + 12
        while b"HOME_READY" not in log and time.monotonic() < deadline:
            log.extend(port.read(32768))
        if b"HOME_READY" not in log:
            port.write(b"q")
            deadline = time.monotonic() + 12
            while b"HOME_READY" not in log and time.monotonic() < deadline:
                log.extend(port.read(32768))
        if b"HOME_READY" not in log:
            args.out.write_bytes(log)
            print(f"HOME_READY missing; saved {args.out}")
            return 2

        time.sleep(1)
        start = len(log)
        for _ in range(3):
            port.write(b"|")
            trigger_deadline = time.monotonic() + 3
            while b"KSN_PROC: START" not in log[start:] and time.monotonic() < trigger_deadline:
                log.extend(port.read(32768))
            if b"KSN_PROC: START" in log[start:]:
                break
        if b"KSN_PROC: START" not in log[start:]:
            args.out.write_bytes(log)
            print(f"KSN_PROC START missing; saved {args.out}")
            return 2
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            log.extend(port.read(32768))
            if b"KSN_PROC: ALL PASS" in log[start:] or b"KSN_PROC: ALL FAIL" in log[start:]:
                break
        args.out.write_bytes(log)
    result = bytes(log[start:])
    for line in result.splitlines():
        if b"KSN_PROC:" in line or b"KSN_COMPILER:" in line:
            print(line.decode("utf-8", "replace"))
    ok = (b"KSN_PROC: DISPLAY PASS" in result and
          b"KSN_COMPILER: PASS" in result and
          b"KSN_PROC: ALL PASS" in result)
    print(f"{'PASS' if ok else 'FAIL'}; log={args.out}")
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
