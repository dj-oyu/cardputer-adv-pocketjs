"""Run the D5 SD video diagnostic twice from the existing VIDEO LAB menu row."""
import argparse
from pathlib import Path
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    port = serial.Serial(port=None, baudrate=115200, timeout=0.15)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        with port:
            time.sleep(0.4)
            port.reset_input_buffer()

            def wait_for(marker, seconds=15):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    line = port.readline().decode(errors="replace").strip()
                    if not line:
                        continue
                    lines.append(line)
                    if any(bad in line for bad in (
                        "Guru Meditation", "START_FAILED", "D5_STREAM ERROR",
                        "D5_STREAM CLEANUP_ERROR", "D5_STREAM RECOVER_REFUSED",
                    )) or "panic" in line.lower():
                        raise RuntimeError(f"device failure: {line}")
                    if marker in line:
                        return line
                raise RuntimeError(f"missing {marker!r}: {lines[-20:]}")

            def key(value, marker, seconds=15):
                port.write(value)
                return wait_for(marker, seconds)

            for run in (1, 2):
                key(b"q", "HOME_READY")
                key(b"a", "CATEGORY 0")
                for _ in range(12):
                    key(b"u", "APP ")
                for index in range(1, 11):
                    key(b"d", f"APP {index}")
                key(b"e", "APP_ID local.videolab", 20)
                pick = wait_for("PICK ", 20)
                if "PICK 2 folders" not in pick:
                    raise RuntimeError(f"unexpected SD grant screen: {pick}")
                port.write(b"de")  # Existing card: second root row is music.
                granted = wait_for("GRANTED ", 20)
                if "GRANTED music" not in granted:
                    raise RuntimeError(f"unexpected SD folder: {granted}")
                wait_for("D5_STREAM CREATED bytes=", 40)
                wait_for("D5_STREAM START", 10)
                progress_at = len(lines)
                wait_for("D5_STREAM EOF", 25)
                stop = wait_for("D5_STREAM STOP ack=", 10)
                if "ack=true" not in stop:
                    raise RuntimeError(f"worker stop lacked ACK: {stop}")
                wait_for("D5_STREAM REMOVED", 15)
                done = wait_for("D5_STREAM DONE reason=end", 10)
                selected = re.search(r"selected=(\d+)", done)
                if not selected or int(selected.group(1)) < 2:
                    raise RuntimeError(f"too few selected video frames: {done}")
                pts = [int(m.group(1)) for line in lines[progress_at:]
                       if (m := re.search(r"D5_STREAM: SELECT pts=(\d+)", line))]
                if len(pts) < 2 or pts[0] >= pts[-1] or any(
                    later <= earlier for earlier, later in zip(pts, pts[1:])
                ):
                    raise RuntimeError(f"selected PTS did not advance: {pts}")
                stopped = key(b"q", "HOME_READY", 15)
                if "HOME_READY" not in stopped:
                    raise RuntimeError("HOME did not return after probe")
                print(f"D5_SD_STREAM_RUN_{run} PASS selected={selected.group(1)} "
                      f"pts={pts[0]}..{pts[-1]}", flush=True)
    finally:
        args.out.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
