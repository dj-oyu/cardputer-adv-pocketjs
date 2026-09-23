"""The parts of the L2c input path only a human can press (backlog #11).

Everything else in that item was driven over USB, which enters the firmware at
usb_stroke() and so skips the keyboard scan, the held/repeat bookkeeping and
the debounce. This script watches the device's log and tells you what to press;
it sends nothing but the byte that starts the diagnostic, and it never waits on
the keyboard of the host -- each step simply watches until the device says the
thing happened, or the step times out.

Needs a build with CONFIG_POCKET_VM_SELFTEST=y (CONFIG_POCKET_VM_YIELD=y is the
shipping default since 2026-09-23) and the ESP-IDF Python environment:

    idf.py -B build_phys -D SDKCONFIG=build_phys/sdkconfig \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;<file with SELFTEST=y>" build
    idf.py -B build_phys -p COM3 flash
    python tools/device_physical_input.py --port COM3

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


def follow(port, want, marks, counts, timeout):
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
    a = p.parse_args()
    port = serial.Serial(a.port, 115200, timeout=0.2)
    time.sleep(1.5)
    port.reset_input_buffer()
    failures = []
    marks, counts = [], []

    print("Four things to press on the Cardputer. Each step waits for the "
          f"device to report it, up to {a.wait:.0f} s.", flush=True)

    # ---- 1. Back from the keyboard while the guest is parked --------------
    # The one case a USB byte cannot stand in for: the guest is suspended at an
    # opcode safepoint and the key has to travel scan -> keymap -> tick_app and
    # still produce the save, the stop hook and their promises in order
    # (docs/vm/vm-L2-results.md sec.4.15 did exactly this with a USB byte).
    print("\n== 1. Back while the guest is parked", flush=True)
    port.write(b"q")
    follow(port, "HOME_READY", [], [], 15)
    marks.clear()
    port.write(b"Y")
    if not follow(port, "VM_SAVE_MARK 10", marks, counts, 25):
        failures.append("the diagnostic never parked")
    else:
        print(f"\n>>> PRESS {ESC} once. The screen is waiting, parked.", flush=True)
        follow(port, "HOME_READY", marks, counts, a.wait)
        if marks == [1, 10, 11, 2, 3, 4, 5]:
            print(f"   OK marks {marks}", flush=True)
        else:
            failures.append(f"save/stop order {marks}, expected [1, 10, 11, 2, 3, 4, 5]")

    # ---- 2. ordinary presses in an app ------------------------------------
    print("\n== 2. ordinary presses in an app", flush=True)
    counts.clear()
    print(">>> OPEN 'HELLO WORLD' from the menu and PRESS Enter three times.",
          flush=True)
    end = time.monotonic() + a.wait
    while time.monotonic() < end and len(counts) < 3:
        follow(port, "HELLO_COUNT", [], counts, 2)
    if counts[:3] == [1, 2, 3]:
        print(f"   OK counts {counts[:3]}", flush=True)
    else:
        failures.append(f"HELLO_COUNT {counts}, expected 1, 2, 3")

    # ---- 3. a held key ----------------------------------------------------
    # Repeat is produced by the input service out of the scan, so no USB byte
    # ever reaches it. Recorded rather than judged: whether a held Enter should
    # repeat at all is a question for the input design, not for this guard.
    print("\n== 3. a held key (recorded, not judged)", flush=True)
    before = len(counts)
    print(">>> HOLD Enter for about two seconds, then let go.", flush=True)
    end = time.monotonic() + min(a.wait, 20)
    while time.monotonic() < end:
        follow(port, "HELLO_COUNT", [], counts, 2)
    added = counts[before:]
    print(f"   counts while held: {added}", flush=True)
    if len(added) < 2:
        print("   (one or none means repeat did not fire -- a fact to record)",
              flush=True)

    # ---- 4. leaving the app -----------------------------------------------
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
