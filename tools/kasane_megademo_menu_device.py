"""Verify that the built-in MEGADEMO launches, returns, and launches again."""

import argparse
from pathlib import Path
import time

import serial


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    log = bytearray()

    try:
        with serial.Serial(args.port, 115200, timeout=0.1) as port:
            time.sleep(1)
            port.reset_input_buffer()

            def command(key: bytes, marker: bytes, seconds: int = 12) -> bytes:
                start = len(log)
                port.write(key)
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    log.extend(port.read(32768))
                    recent = bytes(log[start:])
                    if b"START_FAILED" in recent or b"Guru Meditation" in recent:
                        raise RuntimeError(recent[-1200:])
                    if marker in recent:
                        return recent
                raise RuntimeError(f"missing {marker!r}: {bytes(log[-1200:])!r}")

            for run in range(2):
                returned = command(b"q", b"HOME_READY")
                if run and b"APP_STOPPED" not in returned:
                    raise RuntimeError("Back did not stop the previous session")
                command(b"a", b"CATEGORY 0")
                for _ in range(12):
                    command(b"u", b"APP ")
                for index in range(1, 9):
                    command(b"d", f"APP {index}".encode())
                started = command(b"e", b"KASANE_FRAME_PRESENTED", 20)
                if b"APP_ID local.megademo" not in started:
                    raise RuntimeError("MEGADEMO ran under an unexpected app identity")
                # Four 30-frame paint windows prove the normal app survived
                # more than two 48-frame cycles. Reject an unsolicited return
                # home before Back is sent.
                start = len(log)
                deadline = time.monotonic() + 15.0
                while bytes(log[start:]).count(b"KASANE_PAINT") < 4:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("MEGADEMO did not paint 120 frames")
                    log.extend(port.read(32768))
                    recent = bytes(log[start:])
                    if any(marker in recent for marker in
                           (b"APP_STOPPED", b"HOME_READY", b"START_FAILED",
                            b"Guru Meditation", b"PRESENTER_STEP_FAILED")):
                        raise RuntimeError(f"MEGADEMO stopped early: {recent[-1200:]!r}")
                print(f"MEGADEMO_RUN_{run + 1} PASS")
            stopped = command(b"q", b"HOME_READY")
            if b"APP_STOPPED" not in stopped:
                raise RuntimeError("Back did not stop the MEGADEMO session")
            print("MEGADEMO_MENU_RESTART PASS")
    finally:
        args.out.write_bytes(log)


if __name__ == "__main__":
    main()
