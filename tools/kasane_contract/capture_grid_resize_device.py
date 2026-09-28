"""Capture GRID LAB's 15:7 PIE image and match every displayed RGB565 pixel."""
import argparse
from pathlib import Path
import re
import time

import serial

PIX = re.compile(rb"^PIX (\d+) ([0-9a-f]{960})\r?$", re.M)
SOURCE_W, SOURCE_H, WIDTH, HEIGHT = 60, 30, 28, 14
LEFT, TOP = (240 - WIDTH * 2) // 2, 20


def axis(index, source, output):
    numerator = (2 * index + 1) * source - output
    denominator = 2 * output
    if numerator <= 0:
        return 0, 0
    if numerator >= (source - 1) * denominator:
        return source - 1, 0
    cell, remainder = divmod(numerator, denominator)
    phase = (remainder * 128 + denominator // 2) // denominator
    if phase == 128:
        return cell + 1, 0
    return cell, phase


def color(x, y, tick):
    v = (x * 5 + y * 9 + tick * 2 + ((x ^ y) & 15)) & 63
    return (((v >> 1) & 31) << 11) | (v << 5) | ((63 - v) >> 1)


def expected(x, y, tick):
    sx, fx = axis(x, SOURCE_W, WIDTH)
    sy, fy = axis(y, SOURCE_H, HEIGHT)
    samples = (color(sx, sy, tick), color(min(sx + 1, SOURCE_W - 1), sy, tick),
               color(sx, min(sy + 1, SOURCE_H - 1), tick),
               color(min(sx + 1, SOURCE_W - 1),
                     min(sy + 1, SOURCE_H - 1), tick))
    weights = ((128 - fx) * (128 - fy), fx * (128 - fy),
               (128 - fx) * fy, fx * fy)
    channels = []
    for shift, mask in ((11, 31), (5, 63), (0, 31)):
        channels.append(sum(((v >> shift) & mask) * weight
                            for v, weight in zip(samples, weights)) >> 14)
    return (channels[0] << 11) | (channels[1] << 5) | channels[2]


def verify(data):
    begin = data.find(b"CAPTURE_BEGIN 240 135")
    end = data.find(b"CAPTURE_END", begin)
    if begin < 0 or end < 0:
        raise RuntimeError("no completed capture")
    rows = {int(match[1]): match[2].decode("ascii")
            for match in PIX.finditer(data[begin:end])}
    if len(rows) != 135:
        raise RuntimeError(f"capture has {len(rows)} rows")
    actual = [[int(rows[TOP + 2 * y][4 * (LEFT + 2 * x):
                                       4 * (LEFT + 2 * x + 1)], 16)
               for x in range(WIDTH)] for y in range(HEIGHT)]
    scores = []
    for tick in range(120):
        mismatches = sum(actual[y][x] != expected(x, y, tick)
                         for y in range(HEIGHT) for x in range(WIDTH))
        scores.append((mismatches, tick))
    mismatches, tick = min(scores)
    print(f"15:7 captured pixels: {WIDTH * HEIGHT}; best tick={tick}; "
          f"mismatches={mismatches}")
    if mismatches:
        raise RuntimeError("device PIE pixels differ from host reference")


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

            key(b"q", b"HOME_READY")
            key(b"a", b"CATEGORY 0")
            for _ in range(12):
                key(b"u", b"APP ")
            for index in range(1, 10):
                key(b"d", f"APP {index}".encode())
            key(b"e", b"GRID_APP MODE 0 28x14 backend=PIE", 20)
            key(b"s", b"CAPTURE_END", 45)
            verify(bytes(log))
            key(b"q", b"HOME_READY")
    finally:
        args.out.write_bytes(log)


if __name__ == "__main__":
    main()
