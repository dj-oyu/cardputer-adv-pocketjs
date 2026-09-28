"""Launch GRID LAB twice and verify resize plus symbolic fold modes via PIE."""
import argparse
from pathlib import Path
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    log = bytearray()
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        with port:
            time.sleep(0.5)
            port.reset_input_buffer()

            def collect(marker, seconds=15):
                start = len(log)
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    log.extend(port.read(32768))
                    recent = bytes(log[start:])
                    if b"Guru Meditation" in recent or b"START_FAILED" in recent:
                        raise RuntimeError(recent[-1500:])
                    if marker in recent:
                        return recent
                raise RuntimeError(f"missing {marker!r}: {bytes(log[-1500:])!r}")

            def key(value, marker, seconds=15):
                port.write(value)
                return collect(marker, seconds)

            for run in (1, 2):
                key(b"q", b"HOME_READY")
                key(b"a", b"CATEGORY 0")
                for _ in range(12):
                    key(b"u", b"APP ")
                for index in range(1, 10):
                    key(b"d", f"APP {index}".encode())
                started = key(b"e", b"GRID_APP MODE 0 28x14 backend=PIE strategy=BILINEAR reason=EXPERIMENT", 20)
                if b"APP_ID local.gridlab" not in started:
                    raise RuntimeError("GRID LAB launched under wrong identity")
                start = len(log)
                deadline = time.monotonic() + 20
                while bytes(log[start:]).count(b"KASANE_PAINT") < 14:
                    if time.monotonic() > deadline:
                        raise RuntimeError("GRID LAB did not paint 420 frames")
                    log.extend(port.read(32768))
                    recent = bytes(log[start:])
                    if any(marker in recent for marker in
                           (b"APP_STOPPED", b"START_FAILED", b"Guru Meditation")):
                        raise RuntimeError(recent[-1500:])
                if b"GRID_APP FRAME" not in bytes(log[start:]):
                    raise RuntimeError("GRID LAB did not keep selecting backend")
                recent = bytes(log[start:])
                for mode in (b"GRID_APP MODE 1 37x23 backend=PIE",
                             b"GRID_APP MODE 2 30x15 backend=PIE",
                             b"GRID_APP MODE 3 48x28 backend=PIE"):
                    if mode not in recent:
                        raise RuntimeError(f"GRID LAB did not reach {mode!r}")
                measured = re.search(
                    rb"GRID_APP MEASURE 3 repeats=8 scalar_us=(\d+) "
                    rb"pie_us=(\d+) equal=1", recent)
                if not measured:
                    raise RuntimeError("GRID LAB native comparison missing")
                scalar_us, pie_us = map(int, measured.groups())
                if scalar_us <= 0 or pie_us <= 0:
                    raise RuntimeError("GRID LAB native timing was zero")
                stopped = key(b"q", b"HOME_READY")
                if b"APP_STOPPED" not in stopped:
                    raise RuntimeError("GRID LAB did not stop")
                print(f"GRID_LAB_RUN_{run} PASS scalar={scalar_us}us "
                      f"pie={pie_us}us repeats=8")
    finally:
        args.out.write_bytes(log)


if __name__ == "__main__":
    main()
