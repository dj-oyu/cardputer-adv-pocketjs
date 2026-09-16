"""Check finite frame completion and termination with the real app watchdog."""
import argparse
import json
import re
import time

import serial
from device_runaway import wait


def outcome(text, n):
    found = re.findall(r"VM_FINITE_DONE n=(\d+) sum=(\d+)", text)
    guarded = re.findall(r"RUNAWAY one frame spent (\d+) us", text)
    if found:
        if found != [(str(n), str(n * (n - 1) // 2))] or guarded:
            raise RuntimeError(f"Invalid finite result: {found}, guard={guarded}")
        return {"n": n, "completed": True}
    if len(guarded) != 1 or "APP_STOPPED" not in text:
        raise RuntimeError("Missing finite completion or frame guard")
    return {"n": n, "completed": False, "execution_us": int(guarded[0])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    results = []
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        time.sleep(1.5)
        port.reset_input_buffer()
        port.write(b"q")
        wait(port, "HOME_READY")
        for command, n in ((b"[", 20000), (b"\\", 40000), (b"]", 100000)):
            port.write(command)
            deadline = time.monotonic() + 15
            lines = []
            while time.monotonic() < deadline:
                line = port.readline().decode(errors="replace").strip()
                if not line:
                    continue
                print(line, flush=True)
                lines.append(line)
                if any(s in line for s in ("Guru Meditation", "assert failed", "CORRUPT HEAP")):
                    raise RuntimeError(line)
                if "VM_FINITE_DONE" in line or "APP_STOPPED" in line:
                    break
            result = outcome("\n".join(lines), n)
            results.append(result)
            port.write(b"q")
            wait(port, "HOME_READY")
    print("DEVICE_FINITE_SAMPLES " + json.dumps(results), flush=True)
    # Pin these only after observing the actual firmware's finite workloads.
    # The tool records outcomes; it does not tune the watchdog to force a pass.


if __name__ == "__main__":
    main()
