"""Instruction-level check of pocket_shiki_frame.c's damage_pie_min/max
(frame_damage's PIE bounding box, docs/vm/qjs-observe.md sec.19): each lane
of the 40 halfwords out is the min (max) of that halfword over the groups."""

from pathlib import Path
import random

from piesim import Sim, extract_asm, load16, store16

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/pocket/pocket_shiki_frame.c"
LO, HI, SEG, OUT = 0x100, 0x110, 0x200, 0x2000


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def run(func, groups, halves):
    memory = bytearray(0x3000)
    store16(memory, LO, [0x7FFF] * 8)
    store16(memory, HI, [0x8000] * 8)
    store16(memory, SEG, halves)
    sim = Sim(memory)
    sim.run(extract_asm(SOURCE, func + "(const ksn_proc_segment"),
            {"seg": SEG, "out": OUT, "groups": groups, "init": LO if func.endswith("min") else HI})
    assert sim.ar["seg"] == SEG + 80 * groups, "segment pointer did not advance a group at a time"
    return [s16(v) for v in load16(memory, OUT, 40)]


def main():
    rng = random.Random(7)
    for case in range(300):
        groups = rng.choice([1, 2, 3, 7, 16, 90])
        halves = [rng.randint(-480, 720) if (h % 5) != 4 else rng.randint(0, 65535)
                  for h in range(40 * groups)]
        if case % 10 == 0:                       # extremes
            halves = [rng.choice([-32768, 32767, -480, 720]) for _ in range(40 * groups)]
        signed = [s16(v & 0xFFFF) for v in halves]
        want_lo = [min(signed[g * 40 + h] for g in range(groups)) for h in range(40)]
        want_hi = [max(signed[g * 40 + h] for g in range(groups)) for h in range(40)]
        assert run("damage_pie_min", groups, halves) == want_lo, (case, "min")
        assert run("damage_pie_max", groups, halves) == want_hi, (case, "max")
    print("PROC_DAMAGE_PIE PASS: 300 cases, min and max, every lane")


if __name__ == "__main__":
    main()
