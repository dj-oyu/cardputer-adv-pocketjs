"""Measure existing frame/job runaway diagnostics; never change their limits."""
import argparse
import json
import re
import time

import serial


def wait(port, marker, timeout=15):
    deadline = time.monotonic() + timeout
    lines = []
    while time.monotonic() < deadline:
        line = port.readline().decode(errors="replace").strip()
        if not line:
            continue
        lines.append(line)
        print(line, flush=True)
        if any(s in line for s in ("Guru Meditation", "assert failed", "CORRUPT HEAP")):
            raise RuntimeError(line)
        if marker in line:
            return "\n".join(lines)
    raise RuntimeError(f"Waiting for {marker}: {lines[-20:]}")


def measurement(text, case):
    kind = "frame" if case == "3" else "drain"
    matches = re.findall(r"RUNAWAY one " + kind + r" spent (\d+) us", text)
    if len(matches) != 1:
        raise RuntimeError(f"Expected one {kind} runaway report, found {matches}")
    return {"case": case, "kind": kind, "execution_us": int(matches[0])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--cycles", type=int, default=3)
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error("--cycles must be positive")
    samples = []
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        time.sleep(1.5)
        port.reset_input_buffer()
        port.write(b"q")
        wait(port, "HOME_READY")
        for cycle in range(args.cycles):
            for case in ("3", "6"):
                began = time.monotonic()
                port.write(case.encode())
                text = wait(port, "APP_STOPPED")
                elapsed = time.monotonic() - began
                sample = measurement(text, case)
                sample.update(cycle=cycle, host_elapsed_ms=round(elapsed * 1000, 3))
                samples.append(sample)
                port.write(b"q")
                wait(port, "HOME_READY")
    # Host elapsed includes transport/startup/teardown; it is NOT VM CPU time.
    print("DEVICE_RUNAWAY_SAMPLES " + json.dumps(samples), flush=True)


if __name__ == "__main__":
    main()
