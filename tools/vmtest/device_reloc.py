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

What this does NOT test is compaction, because L3a has none. A move allocates
blocks of the SAME sizes as the ones it replaces, so it holds both copies at
once and gives back exactly what it took; it cannot pack the heap. The heap
figures it prints are observations for L4 to start from, not a gate -- see
fragmentation() below.
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
    """Start an app over USB, let it finish or stop it, and return its log.

    An app may end before the window is up, and the interesting ones do: a
    workload that parks deeply enough to be worth measuring is usually one the
    frame guard stops. So APP_STOPPED is watched for during the run -- and
    checked in the startup text too, since a guard that fires in 350 ms puts
    it there before this loop reads a single line. Only an app still alive at
    the end gets a 'q'. Waiting for APP_STOPPED unconditionally after that 'q'
    waits for a line that already went past.
    """
    port.write(command)
    text = wait(port, "APP_ID", timeout=15)
    deadline = time.monotonic() + seconds
    lines = [text]
    stopped = "APP_STOPPED" in text
    while time.monotonic() < deadline and not stopped:
        line = port.readline().decode(errors="replace").strip()
        if not line:
            continue
        lines.append(line)
        print(line, flush=True)
        if any(s in line for s in ("Guru Meditation", "assert failed",
                                   "CORRUPT HEAP")):
            raise RuntimeError(line)
        if "APP_STOPPED" in line:
            stopped = True
    if not stopped:
        port.write(b"q")
        lines.append(wait(port, "APP_STOPPED", timeout=20))
    return "\n".join(lines)


def contract(text):
    return CONTRACT.findall(text)


def reloc_stats(text):
    m = re.search(
        r"VM_RELOC moves=(\d+) refused=(\d+) empty=(\d+) frames=(\d+) var_refs=(\d+) "
        r"bytes=(\d+) max_us=(\d+) total_us=(\d+) "
        r"largest_first=(\d+) largest_last=(\d+) largest_min=(\d+) "
        r"gap_max=(\d+) gap_segments=(\d+)", text)
    if not m:
        raise RuntimeError("no VM_RELOC line; was the build RELOC=y and armed?")
    keys = ("moves", "refused", "empty", "frames", "var_refs", "bytes", "max_us",
            "total_us", "largest_first", "largest_last", "largest_min",
            "gap_max", "gap_segments")
    return dict(zip(keys, (int(g) for g in m.groups())))


def mem_pairs(text):
    """The session's own MEM line, which brackets the run at start and stop."""
    return [tuple(int(g) for g in m)
            for m in re.findall(r"MEM free=(\d+) largest=(\d+) js=(\d+)", text)]


def fragmentation(baseline, moved, stats):
    """What the run says about the heap -- reported, never asserted.

    L3a does not compact. A move takes NEW blocks of the SAME sizes, copies,
    and frees the old ones, so it cannot pack anything; the most it can do is
    hand the allocator a different arrangement of the same total. These numbers
    exist so that "moving made the heap worse" would be visible if it were
    true, not because any particular value is required to pass. Deciding what
    ought to happen here is L4's job, and L4 does not exist yet.
    """
    return {
        "note": "L3a relocates, it does not compact; these are observations",
        "largest_before_first_move": stats["largest_first"],
        "largest_after_last_move": stats["largest_last"],
        "largest_worst_seen": stats["largest_min"],
        "largest_net": stats["largest_last"] - stats["largest_first"],
        # Both copies are live across a move, so this is the transient extra
        # the heap has to find -- exactly the chain's own size, summed here
        # over every move rather than per move.
        "bytes_copied_total": stats["bytes"],
        # The question this was added for: are the pieces of one stack spread
        # across the heap with other allocations between them? gap_max is how
        # many foreign bytes sat inside the chain's address range at its worst,
        # and gap_segments is how deep the chain was at that moment. On the
        # host the gap tracked DEPTH, not how often the app parked; the device
        # is where that could differ, because native work runs in this same
        # pool while a chain sits parked and vmrun's park runs nothing.
        "foreign_bytes_inside_chain": stats["gap_max"],
        "chain_segments_then": stats["gap_segments"],
        "session_mem_without_moving": mem_pairs(baseline),
        "session_mem_with_moving": mem_pairs(moved),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--app", default="<",
                        help="USB app letter: '<' pet, '>' companion (needs "
                             "CONFIG_POCKET_VM_PROBE). The digits are FAILURE "
                             "diagnostics -- '1' is a deliberate syntax error "
                             "and '3' an endless frame -- so none of them is a "
                             "default worth having here.")
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
    result = {"app": args.app, "contract_match": before == after, **stats,
              "heap": fragmentation(baseline, moved, stats)}

    if before != after:
        raise RuntimeError(f"markers changed under relocation:\n"
                           f"  without: {before}\n  with:    {after}")
    # A run that refused every park proves the guard works and proves nothing
    # about the move. Both numbers are reported either way; only this one is
    # fatal, because the point of the test is that a move happened.
    if stats["moves"] == 0:
        raise RuntimeError(
            f"no move ever happened (refused={stats['refused']}): the app "
            f"never parked anywhere a move was legal (empty="
            f"{stats['empty']}), so this run did not test relocation")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
