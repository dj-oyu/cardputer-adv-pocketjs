"""Summarize uncaptured news-zoom frame timing and transfer from a COM3 log."""

import argparse
from pathlib import Path
import re
from statistics import median


FRAME = re.compile(
    rb"I \((\d+)\) PROC_JS: frame=(\d+) scalar=(\d+) pie=(\d+) bytes=(\d+)"
)
PAINT = re.compile(rb"KASANE_PAINT [^\n]*")
NUMBER = re.compile(rb"\b([a-z_]+)=([\d.]+)")
PHASES = (("monitor", 1, 19), ("zoom-in", 20, 63),
          ("full", 64, 115), ("zoom-out", 116, 159),
          ("zoom-out-wide", 116, 139), ("zoom-out-narrow", 140, 159))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    data = args.log.read_bytes()
    capture = data.find(b"CAPTURE_BEGIN")
    if capture >= 0:
        data = data[:capture]
    frames = [tuple(map(int, match.groups())) for match in FRAME.finditer(data)]
    starts = [index for index, frame in enumerate(frames) if frame[1] == 0]
    if not starts:
        raise SystemExit("no frame=0 marker before capture")
    frames = frames[starts[-1]:]
    print(f"{args.log}: frames 0..{frames[-1][1]}")
    for name, low, high in PHASES:
        intervals = [frames[i][0] - frames[i - 1][0]
                     for i in range(1, len(frames))
                     if low <= frames[i][1] <= high
                     and frames[i][1] == frames[i - 1][1] + 1]
        selected = [frame for frame in frames if low <= frame[1] <= high]
        if not intervals:
            continue
        transfer = sorted({frame[4] for frame in selected})
        pie_step = sorted({frames[i][3] - frames[i - 1][3]
                           for i in range(1, len(frames))
                           if low <= frames[i][1] <= high
                           and frames[i][1] == frames[i - 1][1] + 1})
        print(f"{name:8} n={len(intervals):3} median={median(intervals):5.1f} ms"
              f" range={min(intervals)}..{max(intervals)}"
              f" transfer={transfer} PIE-step={pie_step}")
    for index, match in enumerate(PAINT.finditer(data)):
        values = {key.decode(): float(value) for key, value in NUMBER.findall(match[0])}
        if "image_cy" not in values:
            continue
        # KASANE_PAINT logs sums over 30 frames. ESP32-S3 runs at 240 MHz.
        scale = 1 / (240_000 * values["frames"])
        print(f"window {index + 1}: render={values['render_ms']:.2f} ms"
              f" image={values['image_cy'] * scale:.2f} ms"
              f" blend={values['blend_cy'] * scale:.2f} ms"
              f" span={values['span_cy'] * scale:.2f} ms")


if __name__ == "__main__":
    main()
