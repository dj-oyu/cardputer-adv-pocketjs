"""The parts of the L2c input path only a human can press (backlog #11).

Everything else in that item was driven over USB, which enters the firmware at
usb_stroke() and so skips the keyboard scan, the held/repeat bookkeeping and
the debounce. This script watches the device's log and tells you what to press;
it sends nothing but the byte that starts the diagnostic, and it never waits on
the host's keyboard -- each step watches until the device reports the thing, or
times out.

Needs a build with CONFIG_POCKET_VM_SELFTEST=y (CONFIG_POCKET_VM_YIELD=y is the
shipping default since 2026-09-23) and the ESP-IDF Python environment:

    idf.py -B build_phys -D SDKCONFIG=build_phys/sdkconfig \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;<file with SELFTEST=y>" build
    idf.py -B build_phys -p COM3 flash
    python tools/device_physical_input.py --port COM3
    python tools/device_physical_input.py --port COM3 --step 3   # redo one

The device's own log is the evidence: VM_SAVE_MARK for the parked Back path
(app_session.c), HELLO_COUNT for ordinary input, APP_STOPPED and HOME_READY
for the screen changes.
"""
import argparse
import re
import sys
import time

import serial

ESC = "ESC (top-left corner key; the firmware's Back)"


def follow(port, want, marks, counts, timeout, stamps=None):
    """Reads until `want` shows up, collecting the two markers on the way."""
    end = time.monotonic() + timeout
    recent = []
    while time.monotonic() < end:
        line = port.readline().decode(errors="replace").strip()
        if not line:
            continue
        recent.append(line)
        m = re.search(r"VM_SAVE_MARK (\d+)", line)
        if m:
            marks.append(int(m.group(1)))
        m = re.search(r"HELLO_COUNT (\d+)", line)
        if m:
            counts.append(int(m.group(1)))
            if stamps is not None:
                stamps.append(time.monotonic())
        if want in line:
            return True
    print(f"   TIMEOUT waiting for {want}. Last lines:", flush=True)
    for line in recent[-6:]:
        print(f"     {line[:110]}", flush=True)
    return False


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", required=True)
    p.add_argument("--wait", type=float, default=60.0,
                   help="seconds allowed for each thing you are asked to press")
    p.add_argument("--step", type=int, choices=(1, 2, 3, 4),
                   help="run one step only, so a misread instruction can be "
                        "redone without repeating the other three")
    a = p.parse_args()
    port = serial.Serial(a.port, 115200, timeout=0.2)
    time.sleep(1.5)
    port.reset_input_buffer()
    failures = []
    marks, counts = [], []

    def step(n):
        return a.step in (None, n)

    print(f"Each step waits for the device to report it, up to {a.wait:.0f} s.",
          flush=True)

    # ---- 1. Back from the keyboard while the guest is parked --------------
    # The one case a USB byte cannot stand in for: the guest is suspended at an
    # opcode safepoint and the key has to travel scan -> keymap -> tick_app and
    # still produce the save, the stop hook and their promises in order
    # (docs/vm/vm-L2-results.md sec.4.15 did exactly this with a USB byte).
    if step(1):
        print("\n== 1. Back while the guest is parked", flush=True)
        port.write(b"q")
        follow(port, "HOME_READY", [], [], 15)
        marks.clear()
        port.write(b"Y")
        if not follow(port, "VM_SAVE_MARK 10", marks, counts, 25):
            failures.append("the diagnostic never parked")
        else:
            print(f"\n>>> PRESS {ESC} once. The screen is waiting, parked.",
                  flush=True)
            follow(port, "HOME_READY", marks, counts, a.wait)
            if marks == [1, 10, 11, 2, 3, 4, 5]:
                print(f"   OK marks {marks}", flush=True)
            else:
                failures.append(
                    f"save/stop order {marks}, expected [1, 10, 11, 2, 3, 4, 5]")

    # ---- 2. ordinary presses in an app ------------------------------------
    if step(2):
        print("\n== 2. ordinary presses in an app", flush=True)
        counts.clear()
        print(">>> OPEN 'HELLO WORLD' from the menu, then PRESS Enter three "
              "times (three separate taps).", flush=True)
        end = time.monotonic() + a.wait
        while time.monotonic() < end and len(counts) < 3:
            follow(port, "HELLO_COUNT", [], counts, 2)
        if counts[:3] == [1, 2, 3]:
            print(f"   OK counts {counts[:3]}", flush=True)
        else:
            failures.append(f"HELLO_COUNT {counts}, expected 1, 2, 3")

    # ---- 3. ONE long press ------------------------------------------------
    # Repeat is produced by the input service out of the scan, so no USB byte
    # ever reaches it. The first run of this step was read as "tap it again and
    # again" and measured eleven taps, which says nothing about repeat -- hence
    # the wording below and the gaps: repeat is regular, a finger is not.
    if step(3):
        print("\n== 3. ONE long press (recorded, not judged)", flush=True)
        print(">>> With HELLO WORLD open: press Enter ONCE and KEEP IT HELD "
              "DOWN for about two seconds, then let go.", flush=True)
        print("    Do not tap it repeatedly -- one press, held.", flush=True)
        before = len(counts)
        stamps = []
        end = time.monotonic() + min(a.wait, 25)
        while time.monotonic() < end:
            follow(port, "HELLO_COUNT", [], counts, 2, stamps)
        added = counts[before:]
        gaps = [round((y - x) * 1000) for x, y in zip(stamps, stamps[1:])]
        print(f"   counts during the press: {added}", flush=True)
        print(f"   gaps between them (ms): {gaps}", flush=True)
        if len(added) <= 1:
            print("   one or none: a held key does not repeat", flush=True)
        else:
            print("   several: regular gaps are repeat; uneven ones mean the "
                  "key was tapped rather than held, so the step did not run",
                  flush=True)

    # ---- 4. leaving the app -----------------------------------------------
    if step(4):
        print("\n== 4. leaving the app", flush=True)
        print(f">>> PRESS {ESC} to go back to the home screen.", flush=True)
        if not follow(port, "HOME_READY", [], [], a.wait):
            failures.append("the app did not return to the home screen")

    port.close()
    if failures:
        print("\nPHYSICAL_INPUT_FAIL", flush=True)
        for f in failures:
            print(" -", f, flush=True)
        return 1
    print("\nPHYSICAL_INPUT_OK", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
