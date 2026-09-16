"""Drive the optional USB L lifecycle or M Back diagnostic on the Cardputer."""
import argparse
import re
import time

import serial


def wait(port, marker, timeout=90):
    deadline = time.monotonic() + timeout
    recent = []
    while time.monotonic() < deadline:
        line = port.readline().decode(errors="replace").strip()
        if not line:
            continue
        recent.append(line)
        recent = recent[-20:]
        if "lifecycle" in line or "VM_LIFECYCLE" in line or "VM_BACK" in line:
            print(line, flush=True)
        if "VM_BACK_FAIL" in line:
            raise RuntimeError(line)
        if any(error in line for error in ("assert failed", "Guru Meditation", "CORRUPT HEAP")):
            raise RuntimeError(line)
        if marker in line:
            return line
    raise RuntimeError(f"Waiting for {marker}: {recent}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--back", action="store_true", help="test Back/stop-hook ordering")
    args = parser.parse_args()
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        time.sleep(1.5)
        port.reset_input_buffer()
        port.write(b"q")
        wait(port, "HOME_READY", 10)
        samples = []
        for _ in range(args.cycles):
            port.write(b"M" if args.back else b"L")
            line = wait(port, "VM_BACK_OK" if args.back else "VM_LIFECYCLE_OK")
            fields = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
            assert fields["free_after"] >= fields["free_before"] - 256, fields
            samples.append(fields)
        assert samples[-1]["free_after"] >= samples[0]["free_after"] - 256, samples
        print(f"DEVICE_LIFECYCLE_OK cycles={args.cycles}", flush=True)


if __name__ == "__main__":
    main()
