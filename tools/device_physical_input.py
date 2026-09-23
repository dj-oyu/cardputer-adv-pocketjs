"""The parts of the L2c input path only a human can press (backlog #11).

Everything else in that item was driven over USB, which enters the firmware at
usb_stroke() and skips the keyboard scan, the held/repeat bookkeeping and the
debounce. This script does the watching and tells you what to press; it never
sends a key itself except to start a diagnostic.

Needs a build with CONFIG_POCKET_VM_SELFTEST=y and CONFIG_POCKET_VM_YIELD=y
(the shipping default since 2026-09-23 for the second one) and the ESP-IDF
Python environment:

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

ESC = "ESC (the key at the top-left corner; the firmware's Back)"


def watch(port, want, marks, timeout, note=None):
    """Reads until `want` appears, collecting VM_SAVE_MARK numbers on the way."""
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
        if note and note in line:
            print(f"   {line.split('): ', 1)[-1]}", flush=True)
        if want in line:
            return True
    print(f"   TIMEOUT waiting for {want}; last lines:", flush=True)
    for line in recent[-6:]:
        print(f"     {line[:110]}", flush=True)
    return False


def ask(text):
    print(f"\n>>> {text}", flush=True)
    input("    press Enter here once you have done it... ")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", required=True)
    a = p.parse_args()
    port = serial.Serial(a.port, 115200, timeout=0.2)
    time.sleep(1.5)
    port.reset_input_buffer()
    failures = []

    # ---- 1. Back from the keyboard while the guest is parked --------------
    # The one case the USB path cannot stand in for: the guest is suspended at
    # an opcode safepoint, and the key has to travel the scan -> keymap ->
    # tick_app route and still produce the save, the stop hook and their
    # promises in order (results sec.4.15 did this with a USB byte).
    print("== 1. the parked Back", flush=True)
    marks = []
    port.write(b"q")
    watch(port, "HOME_READY", marks, 10)
    marks.clear()
    port.write(b"Y")
    if not watch(port, "VM_SAVE_MARK 10", marks, 20):
        failures.append("the diagnostic never parked")
    else:
        ask(f"press {ESC} on the Cardputer once")
        watch(port, "HOME_READY", marks, 20)
        if marks == [1, 10, 11, 2, 3, 4, 5]:
            print("   OK marks", marks, flush=True)
        else:
            failures.append(f"save/stop order {marks}, expected [1, 10, 11, 2, 3, 4, 5]")

    # ---- 2. ordinary input, pressed rather than injected ------------------
    print("\n== 2. ordinary input in an app", flush=True)
    port.write(b"q")
    watch(port, "HOME_READY", [], 10)
    ask("open HELLO WORLD from the menu, then press Enter three times")
    counts = []
    end = time.monotonic() + 25
    while time.monotonic() < end and len(counts) < 3:
        line = port.readline().decode(errors="replace").strip()
        m = re.search(r"HELLO_COUNT (\d+)", line)
        if m:
            counts.append(int(m.group(1)))
    if counts == [1, 2, 3]:
        print("   OK counts", counts, flush=True)
    else:
        failures.append(f"HELLO_COUNT {counts}, expected [1, 2, 3]")

    # ---- 3. a held key ----------------------------------------------------
    # Repeat is produced by the input service from the scan, so a USB byte
    # never reaches it: this is the only way to see it at all.
    print("\n== 3. a held key", flush=True)
    ask("hold Enter down for about two seconds, then let go")
    held = []
    end = time.monotonic() + 15
    while time.monotonic() < end:
        line = port.readline().decode(errors="replace").strip()
        m = re.search(r"HELLO_COUNT (\d+)", line)
        if m:
            held.append(int(m.group(1)))
        if held and time.monotonic() > end - 10 and not line:
            break
    print(f"   counts while held: {held}", flush=True)
    if len(held) < 2:
        print("   (one count means repeat did not fire -- record it, it is not "
              "a pass or a fail on its own)", flush=True)

    # ---- 4. back out ------------------------------------------------------
    print("\n== 4. leaving the app", flush=True)
    ask(f"press {ESC} to go back to the home screen")
    if not watch(port, "HOME_READY", [], 20, note="APP_STOPPED"):
        failures.append("the app did not stop and return home")

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
