"""Check like-for-like host identity and fixed MID runner scope attribution."""
import argparse
import json
from pathlib import Path
import re


def one(pattern, data):
    found = re.findall(pattern, data, re.M)
    if len(found) != 1:
        raise ValueError(f"expected one {pattern!r}, found {len(found)}")
    return found[0]


def capture(path):
    data = path.read_text()
    tier = one(r"^==== tier (\d+) \((\w+)\)\s*$", data)
    frames = one(r"^frames (\d+) \(procedural frames checked (\d+)\): exceptions=(\d+) bad_present=(\d+) failures=(\d+) tones=(\d+)$", data)
    measure = one(r"^MEASURE_HOST functional_only=(\d+) faults=(\d+) scalar=(\d+) pie=(\d+)$", data)
    if "DERBY_HOST PASS" not in data or any(int(n) for n in frames[2:5]) or measure[:2] != ('1', '0'):
        raise ValueError(f"host capture failed: {path}")
    stages = {name: int(one(rf"^MEASURE_HOST s{index}=\d+:\d+:(\d+)$", data))
              for index, name in enumerate(('guest', 'bridge', 'native'))}
    return dict(tier=tier, frames=frames, finish=one(r"^finish: (.*)$", data),
                pixel_hash=one(r"^pixels hash (\w+)$", data), measure=measure,
                stage_calls=stages)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('candidate', type=Path)
    args = parser.parse_args()
    baseline, candidate = capture(args.baseline), capture(args.candidate)
    base_identity = {k: v for k, v in baseline.items() if k != 'stage_calls'}
    candidate_identity = {k: v for k, v in candidate.items() if k != 'stage_calls'}
    if base_identity != candidate_identity:
        raise ValueError(json.dumps(dict(baseline=baseline, candidate=candidate), indent=2))
    # Fixed scripted MID run: 2,995 native batch crossings replace 23,960
    # individual JS draw crossings. Each native batch also owns one nested
    # native scope; the 23,960 draws now enter pocket_proc_draw_numbers.
    batch_calls, runner_draws = 2995, 23960
    expected = dict(guest=baseline['stage_calls']['guest'],
                    bridge=baseline['stage_calls']['bridge']-runner_draws+batch_calls,
                    native=baseline['stage_calls']['native']+runner_draws+batch_calls)
    if candidate['stage_calls'] != expected:
        raise ValueError(json.dumps(dict(expected_stage_calls=expected,
                                         actual_stage_calls=candidate['stage_calls']), indent=2))
    print('DERBY_MEASURED_HOST_EQUAL ' + json.dumps(candidate, sort_keys=True))


if __name__ == '__main__':
    main()
