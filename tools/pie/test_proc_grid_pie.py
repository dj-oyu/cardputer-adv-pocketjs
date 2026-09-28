"""Instruction-level check of the typed grid IR's PIE reduction block."""

from pathlib import Path
import random

from piesim import Sim, extract_asm, load16, store16
from stalls import analyse, parse


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/ui/kasane/ksn_proc_grid_pie.c"
COEFFICIENT = 0x100
LANES = 0x200
DEST = 0x300


def sat16(value):
    return max(-32768, min(32767, value)) & 0xFFFF


def run_case(weight, shift, taps):
    memory = bytearray(0x1000)
    store16(memory, COEFFICIENT, [weight] * 8)
    sim = Sim(memory)
    sim.run(extract_asm(SOURCE, "pie_start("), {"coefficient": COEFFICIENT})
    sum_by_lane = [0] * 8
    for values in taps:
        store16(memory, LANES, values)
        sim.run(extract_asm(SOURCE, "pie_tap("), {"lanes": LANES})
        for lane, value in enumerate(values):
            sum_by_lane[lane] += value * weight
    sim.run(extract_asm(SOURCE, "pie_finish("), {"shift": shift, "dest": DEST})
    expected = [sat16(value >> shift) for value in sum_by_lane]
    got = load16(memory, DEST, 8)
    assert got == expected, (weight, shift, taps, got, expected)


def run_unzip_case(weight, shift, values, pair):
    memory = bytearray(0x1000)
    store16(memory, COEFFICIENT, [weight] * 8)
    store16(memory, LANES, values)
    sim = Sim(memory)
    sim.run(extract_asm(SOURCE, "pie_start("), {"coefficient": COEFFICIENT})
    name = "pie_tap_unzip_pair(" if pair else "pie_tap_stride2("
    sim.run(extract_asm(SOURCE, name), {"source": LANES})
    sim.run(extract_asm(SOURCE, "pie_finish("), {"shift": shift, "dest": DEST})
    taps = 2 if pair else 1
    expected = [sat16(sum(values[2 * lane + tap] * weight
                          for tap in range(taps)) >> shift)
                for lane in range(8)]
    got = load16(memory, DEST, 8)
    assert got == expected, (weight, shift, values, pair, got, expected)


def run_dynamic_case(shift, taps):
    memory = bytearray(0x1000)
    sim = Sim(memory)
    sim.run(extract_asm(SOURCE, "pie_zero("), {})
    sum_by_lane = [0] * 8
    for values, weights in taps:
        store16(memory, LANES, values)
        store16(memory, COEFFICIENT, weights)
        sim.run(extract_asm(SOURCE, "pie_tap_dynamic("),
                {"lanes": LANES, "coefficient": COEFFICIENT})
        for lane, (value, weight) in enumerate(zip(values, weights)):
            sum_by_lane[lane] += value * weight
    sim.run(extract_asm(SOURCE, "pie_finish("), {"shift": shift, "dest": DEST})
    expected = [sat16(value >> shift) for value in sum_by_lane]
    got = load16(memory, DEST, 8)
    assert got == expected, (shift, taps, got, expected)


def run_broadcast_case(weight, shift, values, input_broadcast):
    memory = bytearray(0x1000)
    store16(memory, COEFFICIENT, [weight] * 8)
    store16(memory, LANES, values)
    sim = Sim(memory)
    if input_broadcast:
        sim.run(extract_asm(SOURCE, "pie_start("),
                {"coefficient": COEFFICIENT})
        sim.run(extract_asm(SOURCE, "pie_tap_broadcast_input("),
                {"value": LANES})
        expected = [sat16(values[0] * weight >> shift)] * 8
    else:
        sim.run(extract_asm(SOURCE, "pie_zero("), {})
        sim.run(extract_asm(SOURCE, "pie_tap_broadcast_coefficient("),
                {"lanes": LANES, "coefficient": COEFFICIENT})
        expected = [sat16(value * weight >> shift) for value in values]
    sim.run(extract_asm(SOURCE, "pie_finish("),
            {"shift": shift, "dest": DEST})
    assert load16(memory, DEST, 8) == expected


def check_access_template_stalls():
    names = {"pie_tap_unzip_pair(": (5, 1),
             "pie_tap_stride2(": (4, 1),
             "pie_tap_broadcast_input(": (2, 1),
             "pie_tap_broadcast_coefficient(": (3, 1)}
    for name, expected in names.items():
        body = parse(extract_asm(SOURCE, name))
        assert (len(body), len(analyse(body))) == expected, name
    pair = names["pie_tap_unzip_pair("]
    split = names["pie_tap_stride2("]
    assert sum(pair) < 2 * sum(split)


def run_scan_case(seed, steps):
    """Eight row recurrences: each store becomes the next column's input."""
    memory = bytearray(0x1000)
    previous = list(seed)
    sim = Sim(memory)
    for samples in steps:
        store16(memory, LANES, previous)
        store16(memory, COEFFICIENT, samples)
        sim.run(extract_asm(SOURCE, "pie_scan_step("),
                {"previous": LANES, "samples": COEFFICIENT, "result": DEST})
        expected = [sat16(((p + 32768) % 65536 - 32768) + s)
                    for p, s in zip(previous, samples)]
        previous = load16(memory, DEST, 8)
        assert previous == expected, (samples, previous, expected)


def run_scan_affine_case(seed, steps, coefficient, shift):
    memory = bytearray(0x1000)
    previous = list(seed)
    sim = Sim(memory)
    for samples in steps:
        store16(memory, COEFFICIENT, [coefficient] * 8)
        sim.run(extract_asm(SOURCE, "pie_start("),
                {"coefficient": COEFFICIENT})
        store16(memory, LANES, previous)
        sim.run(extract_asm(SOURCE, "pie_tap("), {"lanes": LANES})
        store16(memory, COEFFICIENT, [1] * 8)
        sim.run(extract_asm(SOURCE, "pie_reload_coefficient("),
                {"coefficient": COEFFICIENT})
        store16(memory, LANES, samples)
        sim.run(extract_asm(SOURCE, "pie_tap("), {"lanes": LANES})
        sim.run(extract_asm(SOURCE, "pie_finish("),
                {"shift": shift, "dest": DEST})
        expected = [sat16(((((p + 32768) % 65536 - 32768) * coefficient + s)
                          >> shift)) for p, s in zip(previous, samples)]
        previous = load16(memory, DEST, 8)
        assert previous == expected, (coefficient, shift, previous, expected)


def run_bias_case(bias, shift, taps, fixed=False):
    memory = bytearray(0x1000)
    sim = Sim(memory)
    if fixed:
        store16(memory, COEFFICIENT, taps[0][1])
        sim.run(extract_asm(SOURCE, "pie_start("),
                {"coefficient": COEFFICIENT})
    else:
        sim.run(extract_asm(SOURCE, "pie_zero("), {})
    # C's signed quotient/remainder, as used by the Q14 point PIE kernel.
    quotient = abs(bias) // 32768 * (-1 if bias < 0 else 1)
    remainder = bias - quotient * 32768
    q = -quotient
    parts = [remainder]
    for _ in range(3):
        part = max(-32768, min(32767, q))
        parts.append(part)
        q -= part
    assert q == 0 and parts[0] - 32768 * sum(parts[1:]) == bias
    for at, part in enumerate(parts):
        store16(memory, LANES, [part] * 8)
        store16(memory, COEFFICIENT, [1 if at == 0 else -32768] * 8)
        sim.run(extract_asm(SOURCE, "pie_tap_dynamic("),
                {"lanes": LANES, "coefficient": COEFFICIENT})
    total = [bias] * 8
    if fixed:
        store16(memory, COEFFICIENT, taps[0][1])
        sim.run(extract_asm(SOURCE, "pie_reload_coefficient("),
                {"coefficient": COEFFICIENT})
    for values, weights in taps:
        store16(memory, LANES, values)
        if fixed:
            sim.run(extract_asm(SOURCE, "pie_tap("), {"lanes": LANES})
        else:
            store16(memory, COEFFICIENT, weights)
            sim.run(extract_asm(SOURCE, "pie_tap_dynamic("),
                    {"lanes": LANES, "coefficient": COEFFICIENT})
        for lane, (value, weight) in enumerate(zip(values, weights)):
            total[lane] += value * weight
    sim.run(extract_asm(SOURCE, "pie_finish("), {"shift": shift, "dest": DEST})
    assert load16(memory, DEST, 8) == [sat16(value >> shift) for value in total]


def main():
    check_access_template_stalls()
    # No partial sum can saturate signed QACC for the bounded 256-tap IR.
    assert 256 * 32768 * 32768 <= (1 << 39) - 1
    rng = random.Random(0x51F0)
    cases = 0
    for count in (1, 2, 4, 7, 16, 64, 256):
        for weight in (-32768, -1, 0, 1, 16384, 32767):
            for shift in (0, 2, 14, 30):
                taps = [[rng.randrange(-32768, 32768) for _ in range(8)]
                        for _ in range(count)]
                taps[0][0] = -32768
                taps[-1][7] = 32767
                run_case(weight, shift, taps)
                cases += 1
    for weight in (-32768, -1, 1, 16384, 32767):
        for shift in (0, 2, 14, 30):
            for pair in (False, True):
                for _ in range(8):
                    values = [rng.randrange(-32768, 32768) for _ in range(16)]
                    run_unzip_case(weight, shift, values, pair)
                    cases += 1
                    run_broadcast_case(weight, shift, values[:8], pair)
                    cases += 1
    for count in (1, 2, 4, 7, 16, 64, 256):
        for shift in (0, 2, 14, 30):
            taps = [([rng.randrange(-32768, 32768) for _ in range(8)],
                     [rng.randrange(-32768, 32768) for _ in range(8)])
                    for _ in range(count)]
            taps[0][0][0] = taps[0][1][0] = -32768
            taps[-1][0][7] = taps[-1][1][7] = 32767
            run_dynamic_case(shift, taps)
            cases += 1
    for bias in (-2147483648, -327680, -1, 1, 225000, 2147483647):
        for shift in (0, 14, 30):
            taps = [([rng.randrange(-1000, 1000) for _ in range(8)],
                     [rng.randrange(-16000, 16000) for _ in range(8)])
                    for _ in range(2)]
            run_bias_case(bias, shift, taps)
            cases += 1
            fixed_weight = rng.randrange(-32768, 32768)
            run_bias_case(bias, shift,
                          [(values, [fixed_weight] * 8) for values, _ in taps],
                          fixed=True)
            cases += 1
    for length in (1, 2, 16, 64):
        for _ in range(20):
            seed = [rng.randrange(-32768, 32768) for _ in range(8)]
            steps = [[rng.randrange(-32768, 32768) for _ in range(8)]
                     for _ in range(length)]
            run_scan_case(seed, steps)
            cases += 1
    for coefficient in (-32768, -2, 0, 2, 32767):
        for shift in (0, 1, 14):
            for _ in range(3):
                seed = [rng.randrange(-32768, 32768) for _ in range(8)]
                steps = [[rng.randrange(-32768, 32768) for _ in range(8)]
                         for _ in range(12)]
                run_scan_affine_case(seed, steps, coefficient, shift)
                cases += 1
    print(f"proc-grid PIE instruction model: {cases} cases passed")


if __name__ == "__main__":
    main()
