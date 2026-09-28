"""Launch VIDEO LAB, cycle its patterns with arrows, and check frame progress."""
import argparse
from pathlib import Path
import time
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    log = bytearray()
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        with port:
            time.sleep(0.4)
            port.reset_input_buffer()

            def collect(marker: bytes, seconds: float = 15) -> bytes:
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

            def key(keycode: bytes, marker: bytes, seconds: float = 15) -> bytes:
                port.write(keycode)
                return collect(marker, seconds)

            for run in (1, 2):
                key(b"q", b"HOME_READY")
                key(b"a", b"CATEGORY 0")
                for _ in range(12):
                    key(b"u", b"APP ")
                for index in range(1, 11):
                    key(b"d", f"APP {index}".encode())
                started = key(b"e", b"APP_ID local.videolab", 20)
                if b"APP_ID local.videolab" not in started:
                    raise RuntimeError("wrong app identity")
                progress = collect(b"VIDEO_LAB FRAME 60", 20)
                if b"KASANE_PAINT" not in progress:
                    raise RuntimeError("no Kasane paint measurement")
                for direction, pattern in (
                    (b"b", b"VIDEO_LAB PATTERN 1 COLOR BARS"),
                    (b"d", b"VIDEO_LAB PATTERN 2 CHECKER"),
                    (b"b", b"VIDEO_LAB PATTERN 3 INTERFERENCE"),
                    (b"b", b"VIDEO_LAB PATTERN 0 SWEEP"),
                    (b"a", b"VIDEO_LAB PATTERN 3 INTERFERENCE"),
                    (b"u", b"VIDEO_LAB PATTERN 2 CHECKER"),
                ):
                    key(direction, pattern)
                stopped = key(b"q", b"HOME_READY")
                if b"APP_STOPPED" not in stopped:
                    raise RuntimeError("VIDEO LAB did not stop")
                print(f"VIDEO_LAB_RUN_{run} PASS")
    finally:
        args.out.write_bytes(log)


if __name__ == "__main__":
    main()
