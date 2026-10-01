"""Measure the installed MEGADEMO through the normal APPS menu on COM3."""

import argparse
from pathlib import Path
import re
import statistics
import time

import serial


PAINT = re.compile(rb"KASANE_PAINT .*?turn_ms=([0-9.]+) render_ms=([0-9.]+) send_ms=([0-9.]+)")
SCENE = re.compile(rb"MEGADEMO SCENE (\w+) tier=(\d)")
BAD = (b"START_FAILED", b"Guru Meditation", b"PRESENTER_STEP_FAILED", b"APP_STOPPED")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=2)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    raw = bytearray()
    rows = []

    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        with port:
            time.sleep(0.4)
            port.reset_input_buffer()

            def read_until(marker: bytes, seconds: float = 12) -> bytes:
                start = len(raw)
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    raw.extend(port.read(32768))
                    got = bytes(raw[start:])
                    if b"Guru Meditation" in got or b"START_FAILED" in got:
                        raise RuntimeError(got[-1200:])
                    if marker in got:
                        return got
                raise RuntimeError(f"missing {marker!r}: {bytes(raw[-1200:])!r}")

            def command(key: bytes, marker: bytes, seconds: float = 12) -> bytes:
                port.write(key)
                return read_until(marker, seconds)

            def windows(run: int, phase: str, count: int) -> None:
                start = len(raw)
                deadline = time.monotonic() + count * 2.5 + 8
                found = 0
                while found < count and time.monotonic() < deadline:
                    raw.extend(port.read(32768))
                    got = bytes(raw[start:])
                    if any(marker in got for marker in BAD):
                        raise RuntimeError(f"app stopped during {phase}: {got[-1200:]!r}")
                    matches = list(PAINT.finditer(got))
                    while found < min(len(matches), count):
                        m = matches[found]
                        # The loop is 368 frames over four scenes now; a
                        # window is labelled with the scene it ended in.
                        scenes = SCENE.findall(bytes(raw[:start]) + got[:m.start()])
                        where = b"%s@%s" % scenes[-1] if scenes else b"?"
                        rows.append((run, phase, found + 1, *(float(v) for v in m.groups()),
                                     where.decode()))
                        found += 1
                if found != count:
                    raise RuntimeError(f"only {found}/{count} paint windows in {phase}")

            for run in range(1, args.runs + 1):
                command(b"q", b"HOME_READY")
                command(b"a", b"CATEGORY 0")
                for _ in range(12):
                    command(b"u", b"APP ")
                for index in range(1, 9):
                    command(b"d", f"APP {index}".encode())
                launch = command(b"e", b"KASANE_FRAME_PRESENTED", 20)
                if b"APP_ID local.megademo" not in launch:
                    raise RuntimeError("unexpected app identity")
                windows(run, "full_initial", 5)
                port.write(b"e")
                windows(run, "to_monitor", 6)
                port.write(b"e")
                windows(run, "to_full", 6)
                stopped = command(b"q", b"HOME_READY")
                if b"APP_STOPPED" not in stopped:
                    raise RuntimeError("MEGADEMO did not stop")
                print(f"MEGADEMO_RUN_{run} PASS")
    finally:
        args.out.write_bytes(raw)

    for phase, index in (("full_initial", range(2, 6)),
                         ("to_monitor", range(4, 7)),
                         ("to_full", range(4, 7)),
                         ("to_monitor_transition", range(1, 3)),
                         ("to_full_transition", range(1, 3))):
        base = phase.replace("_transition", "")
        selected = [row for row in rows if row[1] == base and row[2] in index]
        values = [[row[col] for row in selected] for col in (3, 4, 5)]
        where = sorted({row[6] for row in selected})
        print(f"{phase} n={len(selected)} scenes={','.join(where)} " + " ".join(
            f"{name}_median={statistics.median(v):.2f}ms range={min(v):.2f}-{max(v):.2f}"
            for name, v in zip(("turn", "render", "send"), values)))


if __name__ == "__main__":
    main()
