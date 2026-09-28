"""Replay the PIE assembly selected by real QuickJS folds on the host.

The C PIE model emits the helper calls and their actual lane inputs. This
runner executes the corresponding inline assembly with piesim, checks every
eight-lane result against the C model (already checked against JS expected
values), and analyses stalls on each selected instruction sequence.
"""

from pathlib import Path
import sys

from run_proc_grid_fold_qjs import OUT, ROOT, main as run_quickjs

sys.path.insert(0, str(ROOT / "tools/pie"))
from piesim import Sim, extract_asm, load16, store16  # noqa: E402
from stalls import analyse, parse  # noqa: E402


SOURCE = ROOT / "main/ui/kasane/ksn_proc_grid_pie.c"
COEFF, LANES, DEST = 0x100, 0x200, 0x400
NAMES = {
    "START_FIXED": "pie_start(", "START_DYNAMIC": "pie_zero(",
    "TAP": "pie_tap(", "PAIR": "pie_tap_unzip_pair(",
    "FUSED_PAIR": "pie_tap_fused_pair(",
    "STRIDE2": "pie_tap_stride2(",
    "BROADCAST_INPUT": "pie_tap_broadcast_input(",
    "BROADCAST_COEFFICIENT": "pie_tap_broadcast_coefficient(",
    "DYNAMIC": "pie_tap_dynamic(", "FINISH": "pie_finish(",
}
ASM = {event: extract_asm(SOURCE, function) for event, function in NAMES.items()}
ASM.update({name: extract_asm(SOURCE, name + "(") for name in
            ("pie_reload_coefficient", "pie_scan_step")})
LENGTHS = {
    "START_FIXED": (8, 0), "START_DYNAMIC": (0, 0),
    "TAP": (8, 0), "PAIR": (16, 0), "FUSED_PAIR": (16, 0),
    "STRIDE2": (16, 0),
    "BROADCAST_INPUT": (1, 0), "BROADCAST_COEFFICIENT": (8, 1),
    "DYNAMIC": (8, 8), "SCAN": (8, 8), "FINISH": (8, 0),
}


def bias(sim, value, append):
    # The native pie_add_bias decomposition, including its coefficient reload.
    quotient = abs(value) // 32768 * (-1 if value < 0 else 1)
    remaining = -quotient
    parts = [value - quotient * 32768]
    for _ in range(3):
        part = max(-32768, min(32767, remaining))
        parts.append(part)
        remaining -= part
    assert remaining == 0
    for part_number, part in enumerate(parts):
        store16(sim.mem, LANES, [part] * 8)
        store16(sim.mem, COEFF, [1 if part_number == 0 else -32768] * 8)
        sim.run(ASM["DYNAMIC"], {"lanes": LANES, "coefficient": COEFF})
        append(ASM["DYNAMIC"])


def check_trace(path, fused=False):
    example = None
    expected_kind = None
    sim = None
    pending_scan = None
    instructions = []
    blocks = 0
    counts = {}
    issues = stalls = 0
    seen_examples = set()
    for line_number, line in enumerate(path.read_text().splitlines(), 1):
        fields = line.split()
        if not fields:
            continue
        event = fields[0]
        if event == "EXAMPLE":
            assert example is None and len(fields) == 3, (line_number, line)
            example, expected_kind = fields[1:]
            assert example not in seen_examples
            seen_examples.add(example)
            sim = Sim(bytearray(0x1000))
            blocks = 0
            continue
        if event == "END":
            assert fields == ["END", example] and not instructions
            assert (blocks > 0) == (expected_kind != "SCALAR"), example
            example = None
            continue
        assert example is not None and event in LENGTHS, (line_number, line)
        na, nb = LENGTHS[event]
        values = [int(value) for value in fields[1:]]
        assert len(values) == 1 + na + nb, (line_number, line)
        argument = values[0]
        a, b = values[1:1 + na], values[1 + na:]
        counts[event] = counts.get(event, 0) + 1

        def append(asm):
            instructions.append(asm)

        if event == "START_FIXED":
            assert not instructions and expected_kind == "PIE"
            store16(sim.mem, COEFF, a)
            sim.run(ASM[event], {"coefficient": COEFF})
            append(ASM[event])
            if argument:
                bias(sim, argument, append)
                store16(sim.mem, COEFF, a)
                sim.run(ASM["pie_reload_coefficient"], {"coefficient": COEFF})
                append(ASM["pie_reload_coefficient"])
        elif event == "START_DYNAMIC":
            assert not instructions and expected_kind == "PIE"
            sim.run(ASM[event], {})
            append(ASM[event])
            if argument:
                bias(sim, argument, append)
        elif event == "SCAN":
            assert not instructions and pending_scan is None and expected_kind == "SCAN"
            pending_scan = (a, b, argument)
        elif event == "FINISH":
            if pending_scan:
                previous, samples, coefficient = pending_scan
                store16(sim.mem, LANES, previous)
                store16(sim.mem, COEFF, samples)
                if coefficient == 1 and argument == 0:
                    sim.run(ASM["pie_scan_step"],
                            {"previous": LANES, "samples": COEFF, "result": DEST})
                    append(ASM["pie_scan_step"])
                else:
                    store16(sim.mem, COEFF, [coefficient] * 8)
                    sim.run(ASM["START_FIXED"], {"coefficient": COEFF})
                    append(ASM["START_FIXED"])
                    sim.run(ASM["TAP"], {"lanes": LANES})
                    append(ASM["TAP"])
                    store16(sim.mem, COEFF, [1] * 8)
                    sim.run(ASM["pie_reload_coefficient"], {"coefficient": COEFF})
                    append(ASM["pie_reload_coefficient"])
                    store16(sim.mem, LANES, samples)
                    sim.run(ASM["TAP"], {"lanes": LANES})
                    append(ASM["TAP"])
                    sim.run(ASM["FINISH"], {"shift": argument, "dest": DEST})
                    append(ASM["FINISH"])
                pending_scan = None
            else:
                assert instructions and expected_kind == "PIE"
                sim.run(ASM[event], {"shift": argument, "dest": DEST})
                append(ASM[event])
            actual = load16(sim.mem, DEST, 8)
            assert actual == [value & 0xFFFF for value in a], (
                example, blocks, actual, a)
            body = parse("\n".join(instructions))
            issues += len(body)
            stalls += len(analyse(body))
            instructions.clear()
            blocks += 1
        else:
            assert instructions and pending_scan is None and expected_kind == "PIE"
            store16(sim.mem, LANES, a)
            arguments = {"lanes": LANES, "source": LANES, "value": LANES,
                         "first": LANES, "next": LANES + 16}
            if event in ("DYNAMIC", "BROADCAST_COEFFICIENT"):
                store16(sim.mem, COEFF, b)
                arguments["coefficient"] = COEFF
            sim.run(ASM[event], arguments)
            append(ASM[event])
    assert example is None and len(seen_examples) == 29
    required = {"FUSED_PAIR" if fused else "PAIR", "STRIDE2",
                "BROADCAST_INPUT", "BROADCAST_COEFFICIENT", "DYNAMIC",
                "TAP", "SCAN"}
    assert required <= counts.keys(), required - counts.keys()
    print("JS->IR->selected PIE assembly: 29 examples, "
          f"{counts['FINISH']} vector blocks passed")
    print("selected paths: " + ", ".join(f"{name}={counts[name]}" for name in sorted(required)))
    print(f"static QR schedule: {issues} issued instructions, {stalls} predicted "
          "interlocks across replayed blocks; excludes C and memory latency")


def main():
    trace = OUT / "grid-pie-asm-trace.txt"
    run_quickjs(trace)
    check_trace(trace)
    fused_trace = OUT / "grid-pie-fused-asm-trace.txt"
    run_quickjs(fused_trace, fused=True)
    check_trace(fused_trace, fused=True)


if __name__ == "__main__":
    main()
