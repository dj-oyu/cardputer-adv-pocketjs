"""G12: does fragmentation make allocations fail on the device?

The device half of the L3/L4 decision (docs/vm/backlog.md, L3/L4 item 1;
docs/vm/vm-L3-results.md). Needs a CONFIG_POCKET_VM_OOMPROBE=y build: without
it there is no "G12 SESSION" line and the run stops at the first app, which
is the honest failure -- a probe-off build cannot see a refusal, so a clean
report from it would say nothing.

Runs the negative control first ('$'), then the shipped JS apps from the
menu, the Kasane demo, the OOM workloads ('!', '@', '#', main/app_session.c)
and diagnostic '4', then the shipped apps again, so a heap the workloads left
behind is what the second round starts from. Every line is kept (--log), and
the summary counts:

  heap_fails   refusals the heap itself returned (G12 FAIL lines)
  literal      of those, the backlog's criterion as written: total free >=
               request > largest free block, summed over all regions
  candidates   of those, refusals where ONE heap region held enough free
               bytes but no single block did -- the only kind any compaction
               could have prevented (regions do not join)
  segfix       of those, refusals where treating the VM's frame segments as
               movable would have left a run long enough -- the only kind
               L3/L4, which move segments and nothing else, could prevent
  canary       rejections the guest's OOM canary reported; most are
               QuickJS's own malloc_limit and never reach the heap

L3/L4 can close when segfix == 0 outside the control, the control's segfix is
> 0 (the probe can see the case it is looking for), and the workloads did
produce refusals (a run with none tested nothing).
"""
import argparse
import json
import os
import re
import time

import serial
from device_runaway import wait

# Menu rows of the JS apps (main/ui/shell.c apps[]). The others open native
# screens (SKK, editor, tutorial) with no guest, so they are not G12's subject.
MENU_APPS = {0: "hello", 4: "imucal", 5: "pet", 6: "companion"}
WORKLOADS = {"K": "kasane-demo", "!": "holes+large", "@": "array-growth",
             "#": "json", "4": "diag4-typed-arrays"}
# Not a workload: it builds a heap where the frame segments ARE what splits
# the free space, so segfix has to fire there; if it does not, a zero
# everywhere else means a broken probe, not a healthy heap.
CONTROL = ("$", "control:segments-split")

FAIL = re.compile(
    r"G12 FAIL req=(\d+) caps=(0x[0-9a-f]+) free=(\d+) largest=(\d+) rfree=(\d+) "
    r"rlargest=(\d+) rsize=(\d+) regions=(\d+) cand=(\d) noseg=(\d+) segs=(\d+)/(\d+) "
    r"segfix=(\d) walk_us=(\d+) bt=(\S+)")
SESSION = re.compile(r"G12 SESSION (\w+) heap_fails=(\d+) candidates=(\d+) segfix=(\d+) "
                     r"canary_rejections=(\d+) canary_turns=(\d+)")
FATAL = ("Guru Meditation", "assert failed", "CORRUPT HEAP", "abort()")


def pump(port, seconds, log, stop_on=None):
    """Read for `seconds`, keeping every line; return early on `stop_on`."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = port.readline().decode(errors="replace").strip()
        if not line:
            continue
        log.append(line)
        if line.startswith("G12 SESSION"):
            print(line, flush=True)
        if any(s in line for s in FATAL):
            raise RuntimeError(line)
        if stop_on and stop_on in line:
            return True
    return False


def run(port, start, name, seconds, log):
    """Start one app, keep it up for `seconds` (or until it ends), stop it."""
    mark = len(log)
    log.append(f"### RUN {name}")
    print(f"### RUN {name}", flush=True)
    start()
    log.append(wait(port, "APP_ID", timeout=15))
    ended = "APP_STOPPED" in log[-1] or pump(port, seconds, log, "APP_STOPPED")
    if not ended:
        port.write(b"q")
        pump(port, 20, log, "APP_STOPPED")
    pump(port, 10, log, "HOME_READY")
    return "\n".join(log[mark:])


def menu_start(port, row):
    def start():
        port.write(b"q")
        wait(port, "HOME_READY", timeout=10)
        port.write(b"a")
        wait(port, "CATEGORY 0", timeout=5)
        for _ in range(8):          # to the top; shell.c clamps at row 0
            port.write(b"u")
            time.sleep(0.05)
        for _ in range(row):
            port.write(b"d")
            time.sleep(0.05)
        if row:
            wait(port, f"APP {row}", timeout=5)
        port.write(b"e")
    return start


def usb_start(port, key):
    return lambda: port.write(key.encode())


def summarize(name, text):
    fails = [m.groups() for m in FAIL.finditer(text)]
    sessions = [m.groups() for m in SESSION.finditer(text)]

    def total(i, kind=None):
        return sum(int(s[i]) for s in sessions if kind is None or s[0] == kind)

    def rec(f):
        return {"req": int(f[0]), "caps": f[1], "free": int(f[2]), "largest": int(f[3]),
                "region_free": int(f[4]), "region_largest": int(f[5]),
                "region_size": int(f[6]), "noseg": int(f[9]),
                "segs_seen_of": f"{f[10]}/{f[11]}", "bt": f[14]}

    return {
        "run": name,
        # Refusals between the previous app's stop and this one's start.
        "home_fails": total(1, "home"),
        "heap_fails": total(1, "app"),
        "literal": sum(1 for f in fails if int(f[2]) >= int(f[0]) > int(f[3])),
        # Home and app alike: a candidate is a candidate whoever asked.
        "candidates": total(2),
        "segfix": total(3),
        "canary": total(4, "app"),
        "printed_fails": len(fails),
        "req_range": [min((int(f[0]) for f in fails), default=0),
                      max((int(f[0]) for f in fails), default=0)],
        "segs_max": max((int(f[11]) for f in fails), default=0),
        "walk_us_max": max((int(f[13]) for f in fails), default=0),
        "segfix_examples": [rec(f) for f in fails if f[12] == "1"][:3],
        "candidate_examples": [rec(f) for f in fails if f[8] == "1" and f[12] == "0"][:3],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=20.0,
                        help="how long each app or workload is left running")
    parser.add_argument("--log", default=".cache/vm/g12/device_g12.log")
    args = parser.parse_args()

    log, results = [], []
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        time.sleep(1.5)
        port.reset_input_buffer()
        port.write(b"q")
        wait(port, "HOME_READY")

        plan = [(CONTROL[1], usb_start(port, CONTROL[0]))]
        plan += [(f"menu:{n}", menu_start(port, row)) for row, n in MENU_APPS.items()]
        plan += [(f"usb:{n}", usb_start(port, c)) for c, n in WORKLOADS.items()]
        plan += [(f"again:{n}", menu_start(port, row)) for row, n in MENU_APPS.items()]

        for name, start in plan:
            text = run(port, start, name, args.seconds, log)
            # "G12 PROBE on" is printed once, by whichever session starts
            # first -- usually the boot overlay, before this script listens --
            # so the per-session line is what proves the build.
            if not results and "G12 SESSION" not in text:
                raise RuntimeError("no 'G12 SESSION': not a CONFIG_POCKET_VM_OOMPROBE build")
            results.append(summarize(name, text))
            port.write(b"q")
            pump(port, 3, log, "HOME_READY")

    os.makedirs(os.path.dirname(args.log) or ".", exist_ok=True)
    with open(args.log, "w", encoding="utf-8") as f:
        f.write("\n".join(log) + "\n")

    control, rest = results[0], results[1:]
    keys = ("home_fails", "heap_fails", "literal", "candidates", "segfix", "canary")
    total = {k: sum(r[k] for r in rest) for k in keys}
    if control["segfix"] == 0:
        verdict = "CONTROL SILENT -- the probe cannot see the case; nothing below counts"
    elif total["heap_fails"] + total["canary"] == 0:
        verdict = "NO REFUSALS -- nothing was tested"
    elif total["segfix"]:
        verdict = "segment-fixable refusals found -- see segfix_examples"
    else:
        verdict = "no refusal that moving segments could have prevented"
    print(json.dumps({"control": control, "runs": rest, "total": total,
                      "verdict": verdict}, indent=2))


if __name__ == "__main__":
    main()
