"""Measure alternating resize sampling in a diagnostic MEGADEMO image."""

import argparse
from pathlib import Path
import re
import statistics
import time

import serial


ARM = re.compile(rb"MEGADEMO SAMPLING arm=(\d) mode=(\w+) tick=(\d+)")
PAINT = re.compile(rb"KASANE_PAINT [^\r\n]*?turn_ms=([\d.]+) render_ms=([\d.]+) send_ms=([\d.]+) [^\r\n]*?bytes=(\d+)")
STREAM = re.compile(rb"GRID_RESIZE_STREAM source_reads=(\d+) source_cycles=(\d+) kernel_spans=(\d+) kernel_cycles=(\d+)")
BAD = (b"Guru Meditation", b"START_FAILED", b"PRESENTER_STEP_FAILED", b"APP_STOPPED")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--arms", type=int, default=4)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    raw = bytearray()
    serial_port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    serial_port.dtr = False
    serial_port.rts = False
    serial_port.port = args.port
    try:
        with serial_port:
            time.sleep(0.4)
            serial_port.reset_input_buffer()

            def until(marker: bytes, seconds: float = 15) -> bytes:
                start = len(raw)
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    raw.extend(serial_port.read(32768))
                    recent = bytes(raw[start:])
                    if any(b in recent for b in BAD[:3]):
                        raise RuntimeError(recent[-1200:])
                    if marker in recent:
                        return recent
                raise RuntimeError(f"missing {marker!r}: {bytes(raw[-1200:])!r}")

            def key(value: bytes, marker: bytes, seconds: float = 15) -> bytes:
                serial_port.write(value)
                return until(marker, seconds)

            key(b"q", b"HOME_READY")
            key(b"a", b"CATEGORY 0")
            for _ in range(12):
                key(b"u", b"APP ")
            for index in range(1, 9):
                key(b"d", f"APP {index}".encode())
            launch = key(b"e", b"KASANE_FRAME_PRESENTED", 20)
            if b"APP_ID local.megademo" not in launch:
                raise RuntimeError("unexpected app identity")
            serial_port.write(b"e")
            until(b"MEGADEMO SAMPLING arm=0", 15)
            for index in range(1, args.arms):
                until(f"MEGADEMO SAMPLING arm={index & 1}".encode(), 30)
            # Complete the final arm before stopping the app.
            until(f"MEGADEMO SAMPLING arm={args.arms & 1}".encode(), 30)
            stopped = key(b"q", b"HOME_READY")
            if b"APP_STOPPED" not in stopped:
                raise RuntimeError("MEGADEMO did not stop")
    finally:
        args.out.write_bytes(raw)

    data = bytes(raw)
    markers = list(ARM.finditer(data))
    if len(markers) < args.arms + 1:
        raise RuntimeError(f"only {len(markers)} sampling arm markers")
    for index in range(args.arms):
        arm = markers[index]
        segment = data[arm.end():markers[index + 1].start()]
        paints = [tuple(map(float, m.groups()[:3])) + (int(m.group(4)),)
                  for m in PAINT.finditer(segment)]
        streams = [tuple(map(int, m.groups())) for m in STREAM.finditer(segment)]
        if len(paints) < 8 or len(streams) < 8:
            raise RuntimeError(f"too few windows in arm {index}: {len(paints)}, {len(streams)}")
        # Sampling switches can straddle a 30-frame accounting window.
        paints = paints[1:-1]
        streams = streams[1:-1]
        med = lambda values: statistics.median(values)
        print(f"arm={index} mode={arm.group(2).decode()} tick={arm.group(3).decode()} "
              f"windows={len(paints)} turn_ms={med(p[0] for p in paints):.3f} "
              f"render_ms={med(p[1] for p in paints):.3f} "
              f"send_ms={med(p[2] for p in paints):.3f} "
              f"bytes={med(p[3] for p in paints):.0f} "
              f"source_reads={med(s[0] for s in streams):.0f} "
              f"source_cycles={med(s[1] for s in streams):.0f} "
              f"kernel_spans={med(s[2] for s in streams):.0f} "
              f"kernel_cycles={med(s[3] for s in streams):.0f}")


if __name__ == "__main__":
    main()
