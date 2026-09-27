"""Resident suspension on the device (docs/vm/app-suspend-design.md).

Needs a CONFIG_POCKET_VM_SELFTEST build: USB 'S' starts a diagnostic app that
registers suspend/resume/stop hooks, holds a 20 s sleep, an IMU watch and a
file handle, and counts frames. Per cycle:

  S         start it (it runs as local.hello, the home screen's first row)
  q (Back)  it must go to sleep: suspend hook, the sleep CANCELLED,
            APP_SUSPENDED, HOME_READY -- not APP_STOPPED
  e         open the first row again: it must resume where it was
            (resume hook with the frame count it slept at, the file handle
            CLOSED, frames counting on from there)
  q         asleep again
  1         any other start evicts it: stop('evict'), APP_EVICT

    python tools/vmtest/device_suspend.py --port COM3 --cycles 5
"""
import argparse
import re
import time

import serial


def wait(port, marker, timeout=15, log=None):
    deadline = time.monotonic() + timeout
    seen = []
    while time.monotonic() < deadline:
        line = port.readline().decode(errors="replace").strip()
        if not line:
            continue
        seen.append(line)
        if log is not None:
            log.append(line)
        if any(bad in line for bad in ("Guru Meditation", "assert failed", "CORRUPT HEAP")):
            raise RuntimeError(line)
        if re.search(marker, line):
            return line, seen
    raise RuntimeError(f"waiting for {marker!r}: {seen[-15:]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--cycles", type=int, default=5)
    ap.add_argument("--sleep", type=float, default=3.0, help="seconds on the home screen while asleep")
    a = ap.parse_args()
    frees = []
    with serial.Serial(a.port, 115200, timeout=0.2) as port:
        time.sleep(1.5)
        port.reset_input_buffer()
        port.write(b"q")
        wait(port, "HOME_READY", 10)
        port.write(b"a")
        wait(port, "CATEGORY 0", 5)
        for cycle in range(a.cycles):
            port.write(b"S")
            wait(port, r"SUSP_TEST ready")
            line, _ = wait(port, r"SUSP_TEST frame n=\d+")
            # Back: asleep, not stopped.
            port.write(b"q")
            line, seen = wait(port, r"SUSP_TEST suspend n=(\d+)")
            slept_at = int(re.search(r"n=(\d+)", line).group(1))
            line, seen2 = wait(port, r"HOME_READY")
            text = "\n".join(seen + seen2)
            assert "SUSP_TEST sleep CANCELLED" in text, text
            assert "APP_SUSPENDED" in text, text
            assert "APP_STOPPED" not in text, text
            free = int(re.search(r"APP_SUSPENDED \S+ us=\d+ free=(\d+)", text).group(1))
            time.sleep(a.sleep)
            # The same row: it wakes where it slept.
            port.write(b"e")
            line, seen = wait(port, r"SUSP_TEST resume ms=(\d+) n=(\d+)")
            ms, n = map(int, re.search(r"ms=(\d+) n=(\d+)", line).groups())
            assert n == slept_at, (n, slept_at)
            assert ms >= a.sleep * 1000 - 500, ms
            line, seen3 = wait(port, r"SUSP_TEST read ")
            assert "read CLOSED" in line, line
            line, _ = wait(port, r"SUSP_TEST frame n=(\d+)")
            assert int(re.search(r"n=(\d+)", line).group(1)) > n, line
            # Asleep again, then evicted by another start.
            port.write(b"q")
            wait(port, r"APP_SUSPENDED")
            wait(port, r"HOME_READY")
            port.write(b"1")
            line, seen = wait(port, r"APP_STOPPED")
            text = "\n".join(seen)
            assert "APP_EVICT" in text and "SUSP_TEST stop evict" in text, text
            port.write(b"q")
            wait(port, "HOME_READY", 10)
            frees.append(free)
            print(f"SUSPEND_CYCLE {cycle + 1} slept_at={slept_at} ms={ms} free_asleep={free}", flush=True)
    assert frees[-1] >= frees[0] - 512, frees
    print(f"DEVICE_SUSPEND_OK cycles={a.cycles}", flush=True)


if __name__ == "__main__":
    main()
