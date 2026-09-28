"""Measure ordinary GRID LAB JS, grid, Kasane and LCD stages on COM3."""

import argparse
from pathlib import Path
import re
import statistics
import time

import serial


ARM = re.compile(rb"GRID_APP ARM ([0-3]) layout=(WINDOW|FULL) route=(AUTO|SCALAR)")
PIPE = re.compile(
    rb"GRID_APP PIPE arm=([0-3]) layout=(WINDOW|FULL) route=(AUTO|SCALAR) "
    rb"frames=(\d+) source_us=(\d+) run_us=(\d+) native_copy_us=(\d+) "
    rb"bind_us=(\d+) kernel_us=(\d+) native_total_us=(\d+) "
    rb"native_runs=(\d+)")
MEM = re.compile(
    rb"GRID_APP MEM arm=([0-3]) native_max_us=(\d+) heap_free=(\d+) "
    rb"heap_largest=(\d+) heap_min_free=(\d+) max_gap_us=(\d+) "
    rb"gaps_over_50ms=(\d+) gaps_over_75ms=(\d+)")
PAINT = re.compile(
    rb"KASANE_PAINT turn_ms=([\d.]+) render_ms=([\d.]+) "
    rb"send_ms=([\d.]+) bytes=(\d+)")
MODE = re.compile(rb"GRID_APP MODE ([6-9]) 48x28 backend=(PIE|scalar) ")
FAULTS = (b"Guru Meditation", b"START_FAILED", b"PRESENTER_STEP_FAILED")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    raw = bytearray()
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        with port:
            time.sleep(0.5)
            port.reset_input_buffer()

            def collect(marker, seconds=15):
                start = len(raw)
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    raw.extend(port.read(32768))
                    recent = bytes(raw[start:])
                    if any(fault in recent for fault in FAULTS):
                        raise RuntimeError(recent[-2000:])
                    if marker in recent:
                        return recent
                raise RuntimeError(f"missing {marker!r}: {bytes(raw[-2000:])!r}")

            def key(value, marker, seconds=15):
                port.write(value)
                return collect(marker, seconds)

            key(b"q", b"HOME_READY")
            key(b"a", b"CATEGORY 0")
            for _ in range(12):
                key(b"u", b"APP ")
            for index in range(1, 10):
                key(b"d", f"APP {index}".encode())
            started = key(b"e", b"GRID_APP MODE 0", 20)
            if b"APP_ID local.gridlab" not in started:
                raise RuntimeError("GRID LAB launched under wrong identity")
            collect(b"GRID_APP MEM arm=3", 100)
            stopped = key(b"q", b"HOME_READY")
            if b"APP_STOPPED" not in stopped:
                raise RuntimeError("GRID LAB did not stop")
    finally:
        args.out.write_bytes(raw)

    data = bytes(raw)
    arms = list(ARM.finditer(data))
    pipes = {int(match.group(1)): match for match in PIPE.finditer(data)}
    memories = {int(match.group(1)): match for match in MEM.finditer(data)}
    modes = {int(match.group(1)): match.group(2) for match in MODE.finditer(data)}
    if ({int(match.group(1)) for match in arms} != set(range(4)) or
            set(pipes) != set(range(4)) or set(memories) != set(range(4)) or
            modes != {6: b"PIE", 7: b"scalar", 8: b"PIE", 9: b"scalar"}):
        raise RuntimeError("four complete pipeline arms missing")
    expected = ((b"WINDOW", b"AUTO"), (b"WINDOW", b"SCALAR"),
                (b"FULL", b"AUTO"), (b"FULL", b"SCALAR"))
    for arm, (layout, route) in enumerate(expected):
        marker = next(match for match in arms if int(match.group(1)) == arm)
        pipe = pipes[arm]
        memory = memories[arm]
        if (marker.start() >= pipe.start() or
                marker.group(2, 3) != (layout, route) or
                pipe.group(2, 3) != (layout, route)):
            raise RuntimeError(f"arm {arm} ordering or label mismatch")
        (frames, source_us, run_us, copy_us, bind_us, kernel_us,
         native_total_us, native_runs) = map(int, pipe.group(*range(4, 12)))
        (native_max_us, heap_free, heap_largest, heap_min_free,
         max_gap_us, gaps_over_50ms, gaps_over_75ms) = map(
             int, memory.group(*range(2, 9)))
        if frames != 180 or native_runs != frames or not heap_free or not heap_largest:
            raise RuntimeError(f"arm {arm} incomplete or low memory: {pipe.group()!r}")
        if copy_us + bind_us + kernel_us > native_total_us or native_total_us > run_us:
            raise RuntimeError(f"arm {arm} inconsistent timings")
        paints = [tuple(map(float, p.group(1, 2, 3))) + (int(p.group(4)),)
                  for p in PAINT.finditer(data[marker.end():pipe.start()])]
        if len(paints) < 5:
            raise RuntimeError(f"arm {arm} has only {len(paints)} paint windows")
        paints = paints[1:-1]
        median = statistics.median
        print(f"arm={arm} layout={layout.decode()} route={route.decode()} "
              f"frames={frames} paint_windows={len(paints)} "
              f"source_ms={source_us/frames/1000:.3f} "
              f"run_ms={run_us/frames/1000:.3f} "
              f"copy_ms={copy_us/frames/1000:.3f} "
              f"bind_ms={bind_us/frames/1000:.3f} "
              f"kernel_ms={kernel_us/frames/1000:.3f} "
              f"native_total_ms={native_total_us/frames/1000:.3f} "
              f"native_max_ms={native_max_us/1000:.3f} "
              f"turn_ms={median(p[0] for p in paints):.3f} "
              f"render_ms={median(p[1] for p in paints):.3f} "
              f"send_ms={median(p[2] for p in paints):.3f} "
              f"bytes={median(p[3] for p in paints):.0f} "
              f"heap_free={heap_free} heap_largest={heap_largest} "
              f"heap_min_free={heap_min_free} max_gap_ms={max_gap_us/1000:.3f} "
              f"gaps_over_50ms={gaps_over_50ms} "
              f"gaps_over_75ms={gaps_over_75ms}")


if __name__ == "__main__":
    main()
