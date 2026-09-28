"""Measure the D2 two-surface ordinary APP in an already flashed diagnostic image."""
import argparse
from pathlib import Path
import re
import time


FAULTS = (b"Guru Meditation", b"CORRUPT HEAP", b"START_FAILED")
PRESENT = re.compile(
    rb"D2_MULTI: PRESENT result=(\d+) bytes=(\d+) duration_us=(\d+) "
    rb"heap_free=(\d+) heap_largest=(\d+) stack_hwm_bytes=(\d+)"
)


def check_run(data: bytes, run: int) -> None:
    for marker in (b"APP_ID local.videolab", b"D2_MULTI START surfaces=2 image_nodes=2",
                   b"D2_MULTI COMMIT tick=0 slot=0", b"D2_MULTI COMMIT tick=1 slot=1",
                   b"D2_MULTI FRAME 60", b"APP_STOPPED", b"HOME_READY"):
        if marker not in data:
            raise RuntimeError(f"run {run}: missing {marker!r}")
    if any(fault in data for fault in FAULTS):
        raise RuntimeError(f"run {run}: firmware fault")
    rows = [tuple(map(int, match.groups())) for match in PRESENT.finditer(data)]
    if len(rows) < 50 or any(result != 0 for result, *_ in rows):
        raise RuntimeError(f"run {run}: too few successful presents: {len(rows)}")
    # The first two frames create their resources. Later one-point moves use
    # one 16-column by 8-row LCD window; other UI damage may widen a frame.
    narrow = sum(bytes_sent == 256 for _, bytes_sent, *_ in rows[3:])
    if narrow < 30:
        raise RuntimeError(f"run {run}: insufficient 256-byte rects: {narrow}")
    heap_min = min(row[3] for row in rows)
    largest_min = min(row[4] for row in rows)
    stack_min = min(row[5] for row in rows)
    if heap_min < 16000 or largest_min < 8192 or stack_min < 2048:
        raise RuntimeError(
            f"run {run}: heap/stack floor free={heap_min} largest={largest_min} "
            f"stack={stack_min}"
        )
    print(f"D2_MULTI_RUN_{run} PASS presents={len(rows)} rect256={narrow} "
          f"heap_free_min={heap_min} largest_min={largest_min} stack_min={stack_min}")


def live(port_name: str, out: Path) -> None:
    import serial

    log = bytearray()
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = port_name
    try:
        with port:
            time.sleep(0.4)
            port.reset_input_buffer()

            def collect(marker: bytes, seconds: float = 20) -> bytes:
                start = len(log)
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    log.extend(port.read(32768))
                    recent = bytes(log[start:])
                    if any(fault in recent for fault in FAULTS):
                        raise RuntimeError(recent[-1500:])
                    if marker in recent:
                        return recent
                raise RuntimeError(f"missing {marker!r}: {bytes(log[-1500:])!r}")

            def key(code: bytes, marker: bytes, seconds: float = 20) -> bytes:
                port.write(code)
                return collect(marker, seconds)

            for run in (1, 2):
                key(b"q", b"HOME_READY")
                key(b"a", b"CATEGORY 0")
                for _ in range(12):
                    key(b"u", b"APP ")
                for index in range(1, 11):
                    key(b"d", f"APP {index}".encode())
                start = len(log)
                key(b"e", b"APP_ID local.videolab")
                collect(b"D2_MULTI FRAME 60", 25)
                key(b"q", b"HOME_READY")
                check_run(bytes(log[start:]), run)
    finally:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(log)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    live(args.port, args.out)


if __name__ == "__main__":
    main()
