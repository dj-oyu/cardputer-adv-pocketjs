"""SELFTEST-only USB Back persistence test; reboot between write and read stages."""
import argparse
import re
import time

import serial


def collect(port, marker, marks, timeout=15):
    deadline = time.monotonic() + timeout
    recent = []
    while time.monotonic() < deadline:
        line = port.readline().decode(errors="replace").strip()
        if not line:
            continue
        recent = (recent + [line])[-20:]
        match = re.search(r"VM_SAVE_MARK (\d+)", line)
        if match:
            mark = int(match[1])
            marks.append(mark)
            print(line, flush=True)
            if mark == 9:
                raise RuntimeError("Storage diagnostic rejected the operation: " + line)
        if any(s in line for s in ("Guru Meditation", "assert failed", "CORRUPT HEAP")):
            raise RuntimeError(line)
        if marker in line:
            return
    raise RuntimeError(f"Waiting for {marker}: {recent}")


def run(port, stage):
    marks = []
    port.write(b"q")
    collect(port, "HOME_READY", marks)
    if stage == "write":
        port.write(b"Y")
        collect(port, "VM_SAVE_MARK 10", marks)
        # This traverses usb_stroke -> KEY_BACK -> tick_app, not app_tick directly.
        port.write(b"q")
        collect(port, "HOME_READY", marks)
        expected = [1, 10, 11, 2, 3, 4, 5]
    else:
        port.write(b"Z")
        collect(port, "VM_SAVE_MARK 8", marks)
        port.write(b"q")
        collect(port, "HOME_READY", marks)
        expected = [6, 7, 8]
    if marks != expected:
        raise RuntimeError(f"Unexpected save/stop order: {marks}, expected {expected}")
    print(f"DEVICE_BACK_STORAGE_OK stage={stage} marks={marks}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--stage", choices=("write", "read"), required=True)
    args = parser.parse_args()
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        time.sleep(1.5)
        port.reset_input_buffer()
        run(port, args.stage)


if __name__ == "__main__":
    main()
