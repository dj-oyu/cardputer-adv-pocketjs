"""Validate callbench logs; optionally capture USB N on an idle Cardputer."""
import argparse
import json
import re
import statistics
import time
from pathlib import Path

EXPECTED = {"loop": 199990000, "call": 200010000, "method": 200010000, "recursive": 17000}


def report(text):
    if text.count("CALLBENCH PASS") != 1 or "CALLBENCH FAIL" in text or "CALLBENCH ERROR" in text:
        raise ValueError("missing/duplicate success or failed benchmark")
    kinds = re.findall(r"CALLBENCH KIND (\w+)", text)
    if len(kinds) > 1 or (kinds and kinds[0] not in ("inputs", "dispatch")):
        raise ValueError("invalid benchmark kind")
    kind = kinds[0] if kinds else "dispatch"  # Original logs predate KIND.
    names = ("lazy", "eager") if kind == "inputs" else ("flat", "recur")
    paths = re.findall(r"CALLBENCH PATH mode=(\w+) span=(\d+) checks=(\d+)", text)
    if len(paths) != 2 or {p[0] for p in paths} != set(names):
        raise ValueError("missing/duplicate path proof")
    for mode, span, checks in paths:
        if int(checks) != 17 or (int(span) == 0) != (kind == "inputs" or mode == "flat"):
            raise ValueError("path proof failed")
    rows = re.findall(r"CALLBENCH SAMPLE case=(\w+) round=(\d+) slot=(\d+) mode=(\w+) ns=(\d+) value=(-?\d+)", text)
    samples = {}
    for name, r, slot, mode, ns, value in rows:
        r, slot, ns, value = map(int, (r, slot, ns, value))
        key = name, r, slot
        expected_mode = names[int((slot in (1, 2)) ^ bool(r & 1))]
        if (name not in EXPECTED or not 0 <= r < 8 or not 0 <= slot < 4 or
                key in samples or value != EXPECTED[name] or mode != expected_mode or ns <= 0):
            raise ValueError(f"invalid/duplicate sample: {key}")
        samples[key] = mode, ns
    if len(samples) != 128:
        raise ValueError(f"incomplete benchmark: {len(samples)}/128 samples")
    summary = {}
    for name in EXPECTED:
        modes = {m: [ns for (case, _, _), (mode, ns) in samples.items()
                     if case == name and mode == m] for m in names}
        ratios = []
        for r in range(8):
            times = {m: [samples[name, r, s][1] for s in range(4)
                         if samples[name, r, s][0] == m] for m in modes}
            ratios.append(statistics.mean(times[names[0]]) / statistics.mean(times[names[1]]))
        summary[name] = {"median_ns": {m: statistics.median(v) for m, v in modes.items()},
                         f"paired_{names[0]}_over_{names[1]}": {"median": statistics.median(ratios),
                                                     "min": min(ratios), "max": max(ratios)}}
    return {"kind": kind, "samples": len(samples), "paths": paths, "cases": summary}


def capture(port_name, inputs=False):
    import serial
    # Keep a complete raw log even if validation later rejects it.
    with serial.Serial(port_name, 115200, timeout=0.2) as port:
        time.sleep(1.5)
        port.reset_input_buffer()
        port.write(b"q")
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if b"HOME_READY" in port.readline():
                break
        else:
            raise RuntimeError("device did not reach HOME_READY")
        port.write(b"U" if inputs else b"N")
        lines = []
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            line = port.readline().decode(errors="replace").strip()
            if not line:
                continue
            lines.append(line)
            if any(s in line for s in ("CALLBENCH PASS", "CALLBENCH FAIL", "Guru Meditation", "assert failed")):
                break
        return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--port")
    parser.add_argument("--inputs", action="store_true", help="USB U: lazy/eager restoration")
    args = parser.parse_args()
    if args.port:
        args.log.write_text(capture(args.port, args.inputs), encoding="utf-8")
    result = report(args.log.read_text(encoding="utf-8"))
    if args.inputs and result["kind"] != "inputs":
        raise ValueError("expected lazy/eager benchmark")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
