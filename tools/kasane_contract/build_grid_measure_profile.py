"""Select legal grid load routes from repeated same-image measurement records.

The records come from grid.measure's scalar-checked GATHER/AFFINE runs. A
profile ranks candidates only; bind and each fast vector load retain their
own safety checks. The 5% margin matches build_grid_profile.py.
"""

import argparse
import json
from pathlib import Path
import re
from statistics import median

from build_grid_profile import MIN_GAIN


def select_records(paths, min_runs=3):
    identity = None
    records = {}
    run_ids = set()
    expected = None
    for path in paths:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        current = (data.get("device"), data.get("binary_sha256"),
                   data.get("binary_bytes"))
        if (data.get("schema") != 1 or not isinstance(current[0], str) or
                not isinstance(current[1], str) or
                not re.fullmatch(r"[0-9a-f]{64}", current[1]) or
                not isinstance(current[2], int) or current[2] <= 0 or
                not isinstance(data.get("runs"), list)):
            raise ValueError(f"invalid measurement identity: {path}")
        if identity is None:
            identity = current
        elif identity != current:
            raise ValueError("measurements use different device or binary")
        for run in data["runs"]:
            if not isinstance(run, dict):
                raise ValueError("invalid measurement run")
            run_id = run.get("run_id")
            if (not isinstance(run_id, str) or
                    not re.fullmatch(r"[0-9a-f]{32}", run_id) or
                    run_id in run_ids or not isinstance(run.get("cases"), list)):
                raise ValueError("duplicate or invalid measurement run")
            run_ids.add(run_id)
            seen = set()
            for case in run["cases"]:
                if not isinstance(case, dict):
                    raise ValueError("invalid route measurement")
                key = case.get("key")
                meta = (case.get("mode"), case.get("width"),
                        case.get("height"))
                gather = case.get("gather_us")
                affine = case.get("affine_us")
                if (not isinstance(key, str) or
                        not re.fullmatch(r"[0-9a-f]{16}", key) or
                        int(key, 16) == 0 or key in seen or
                        not isinstance(meta[0], int) or meta[0] < 0 or
                        not all(isinstance(v, int) and v > 0 for v in meta[1:]) or
                        not isinstance(gather, int) or gather <= 0 or
                        not isinstance(affine, int) or affine <= 0):
                    raise ValueError("invalid route measurement")
                seen.add(key)
                records.setdefault(key, []).append((meta, gather, affine))
            if expected is None:
                expected = seen
            elif expected != seen:
                raise ValueError("measurement runs use different case sets")
    if not identity or len(run_ids) < min_runs or not expected:
        raise ValueError(f"need at least {min_runs} independent runs")
    result = []
    for key, samples in sorted(records.items()):
        if len(samples) != len(run_ids) or any(s[0] != samples[0][0]
                                             for s in samples):
            raise ValueError("profile shape changed between runs")
        gathers = [s[1] for s in samples]
        affines = [s[2] for s in samples]
        gain = 1 - median(affines) / median(gathers)
        selected = ("AFFINE" if gain >= MIN_GAIN and
                    all(a < g for a, g in zip(affines, gathers))
                    else "GATHER")
        result.append((key, samples[0][0], gathers, affines, gain, selected))
    return identity, len(run_ids), result


def render(identity, count, rows):
    device, digest, binary_bytes = identity
    lines = [f"/* Generated from {count} independent runs on {device}; "
             f"binary SHA256 {digest}; {binary_bytes} bytes; "
             f"minimum gain {MIN_GAIN:.0%}. */"]
    for key, (mode, width, height), gathers, affines, gain, selected in rows:
        lines.append(f"/* mode={mode} {width}x{height} "
                     f"gather={gathers} affine={affines} "
                     f"gain={gain:.1%} */")
        lines.append(f"{{UINT64_C(0x{key}), KSN_GRID_PIE_LOAD_{selected}}},")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("records", nargs="+", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    identity, count, rows = select_records(args.records)
    args.out.write_text(render(identity, count, rows), encoding="utf-8")
    for key, meta, _, _, gain, selected in rows:
        print(f"mode={meta[0]} key={key} selected={selected} "
              f"gain={gain:.1%} runs={count}")


if __name__ == "__main__":
    main()
