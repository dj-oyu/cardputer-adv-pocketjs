"""Run the bounded pixel APP gate; requires a D4 diagnostic image already flashed."""
import argparse
from pathlib import Path
import re
import time


def check_run(data: bytes, run: int) -> None:
    if b"APP_ID local.videolab" not in data:
        raise RuntimeError(f"run {run}: wrong app identity")
    if b"D4_PIXEL_APP START 112x63x8 nodes=2" not in data:
        raise RuntimeError(f"run {run}: pixel APP did not start")
    if b"D4_PIXEL_APP FRAME 60" not in data:
        raise RuntimeError(f"run {run}: pixel APP did not advance 60 turns")
    if b"D4_PIXEL_APP STAGE " not in data:
        raise RuntimeError(f"run {run}: no second pixel generation")
    if b"APP_STOPPED" not in data or b"HOME_READY" not in data:
        raise RuntimeError(f"run {run}: APP did not stop cleanly")
    if any(x in data for x in (b"Guru Meditation", b"CORRUPT HEAP", b"START_FAILED")):
        raise RuntimeError(f"run {run}: firmware fault")


def check_first_failure(data: bytes) -> None:
    ordered = (b"D4_PIXEL: ARMED second_send", b"D4_PIXEL: FAIL second_send",
               b"LCD transfer failed; retaining display work for retry",
               b"D4_PIXEL: RETRY_OK")
    cursor = 0
    for marker in ordered:
        found = data.find(marker, cursor)
        if found < 0:
            raise RuntimeError(f"missing ordered failure/repair marker: {marker!r}")
        cursor = found + len(marker)
    marks = [int(x) for x in re.findall(rb"stack_hwm_bytes=(\d+)", data)]
    if not marks or min(marks) < 2048:
        raise RuntimeError(f"UI task stack high-water too low or absent: {marks}")
    print(f"D4_PIXEL_STACK_MIN_BYTES {min(marks)}")
    durations = [int(x) for x in re.findall(rb"D4_PIXEL: PRESENT result=0 duration_us=(\d+)", data)]
    if not durations:
        raise RuntimeError("no successful pixel candidate render duration")
    print(f"D4_PIXEL_PRESENT_MAX_US {max(durations)}")


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
                    if any(x in recent for x in (b"Guru Meditation", b"CORRUPT HEAP", b"START_FAILED")):
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
                collect(b"D4_PIXEL_APP FRAME 60", 20)
                key(b"q", b"HOME_READY")
                data = bytes(log[start:])
                check_run(data, run)
                if run == 1:
                    check_first_failure(data)
                print(f"D4_PIXEL_RUN_{run} PASS")
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
