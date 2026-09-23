"""Move the frame segments on the device and check nothing else changed.

L3a's device gate (docs/vm/vm-L3-design.md sec.8, docs/vm/vm-L3-results.md
sec.3). Needs a CONFIG_POCKET_VM_RELOC=y build; a normal build has no '&' key
and the run below fails at VM_RELOC_ARMED, which is the honest failure -- the
firmware cannot relocate, so a green run would be a lie.

The shape of the check is the host gate's shape: run an app WITHOUT moving,
run the SAME app WITH moving, and require the two to agree. What "agree" means
here is narrower than on the host, where stdout is compared byte for byte: a
device run has timings and heap figures in it that differ between any two runs.
So the comparison is over the markers that are supposed to be deterministic
(the app's own LOADED/APP_ID/APP_STOPPED sequence), plus the requirement that
the moving run actually moved -- moves=0 would pass every check here while
proving nothing.
"""
import argparse
import json
import re
import time

import serial
from device_runaway import wait

# Lines whose presence and order are a contract (CLAUDE.md's uppercase
# markers). Timings, free-heap figures and PERF lines are deliberately NOT
# here: they differ run to run for reasons that have nothing to do with
# relocation, and including them would make this test fail for noise.
CONTRACT = re.compile(r"\b(APP_ID \S+|LOADED \S+|APP_STOPPED|APP_REFUSED \S+)")


def run_app(port, command, seconds):
    """Start an app over USB, let it run, stop it, and return its log."""
    port.write(command)
    text = wait(port, "APP_ID", timeout=15)
    deadline = time.monotonic() + seconds
    lines = [text]
    while time.monotonic() < deadline:
        line = port.readline().decode(errors="replace").strip()
        if not line:
            continue
        lines.append(line)
        print(line, flush=True)
        if any(s in line for s in ("Guru Meditation", "assert failed",
                                   "CORRUPT HEAP")):
            raise RuntimeError(line)
    port.write(b"q")
    lines.append(wait(port, "APP_STOPPED", timeout=20))
    return "\n".join(lines)


def contract(text):
    return CONTRACT.findall(text)


def reloc_stats(text):
    m = re.search(
        r"VM_RELOC moves=(\d+) refused=(\d+) frames=(\d+) var_refs=(\d+) "
        r"bytes=(\d+) max_us=(\d+) total_us=(\d+)", text)
    if not m:
        raise RuntimeError("no VM_RELOC line; was the build RELOC=y and armed?")
    keys = ("moves", "refused", "frames", "var_refs", "bytes", "max_us",
            "total_us")
    return dict(zip(keys, (int(g) for g in m.groups())))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--app", default="1",
                        help="USB diagnostic letter to run (default '1')")
    parser.add_argument("--seconds", type=float, default=6.0)
    args = parser.parse_args()
    app = args.app.encode()

    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        time.sleep(1.5)
        port.reset_input_buffer()
        port.write(b"q")
        wait(port, "HOME_READY")

        # Control first, while nothing is armed: arming is sticky and cannot
        # be taken back, so the unmoved run has to come first.
        baseline = run_app(port, app, args.seconds)
        if "VM_RELOC" in baseline:
            raise RuntimeError("relocation was already armed; reset the board")
        wait(port, "HOME_READY", timeout=20)

        port.write(b"&")
        wait(port, "VM_RELOC_ARMED", timeout=10)
        wait(port, "HOME_READY", timeout=10)
        moved = run_app(port, app, args.seconds)

    stats = reloc_stats(moved)
    before, after = contract(baseline), contract(moved)
    result = {"app": args.app, "contract_match": before == after, **stats}

    if before != after:
        raise RuntimeError(f"markers changed under relocation:\n"
                           f"  without: {before}\n  with:    {after}")
    # A run that refused every park proves the guard works and proves nothing
    # about the move. Both numbers are reported either way; only this one is
    # fatal, because the point of the test is that a move happened.
    if stats["moves"] == 0:
        raise RuntimeError(
            f"no move ever happened (refused={stats['refused']}): the app "
            f"never parked anywhere a move was legal, so this run did not "
            f"test relocation")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
