"""Check like-for-like host capture identity; timing fields are not device speed."""
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
    return dict(tier=tier, frames=frames, finish=one(r"^finish: (.*)$", data),
                pixel_hash=one(r"^pixels hash (\w+)$", data), measure=measure)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('candidate', type=Path)
    args = parser.parse_args()
    baseline, candidate = capture(args.baseline), capture(args.candidate)
    if baseline != candidate:
        raise ValueError(json.dumps(dict(baseline=baseline, candidate=candidate), indent=2))
    print('DERBY_MEASURED_HOST_EQUAL ' + json.dumps(candidate, sort_keys=True))


if __name__ == '__main__':
    main()
