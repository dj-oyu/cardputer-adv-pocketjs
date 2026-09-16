#!/usr/bin/env python3
"""Check tail-frame reuse, constant live capacity and GC at every park."""
import argparse
from pathlib import Path
import re
import subprocess

import test262 as harness


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", default="asan-tco")
    args = ap.parse_args()
    runner = Path(harness.OUT) / ("vmrun-" + args.variant)
    here = Path(__file__).resolve().parent
    capacities = []
    for small, gc in [(True, False), (False, False), (True, True)]:
        cmd = [str(runner), "--stats", "--profile", "device"]
        if small:
            cmd += ["--include", str(here / "tco_small.js")]
        if gc:
            cmd += ["--force-yield", "--gc-on-yield"]
        cmd += [str(here / "tco_reuse.js")]
        for attempt in range(5):
            try:
                result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
            except subprocess.TimeoutExpired:
                raise SystemExit("TCO probe timed out")
            raw = result.stdout + result.stderr
            if "AddressSanitizer:DEADLYSIGNAL" not in raw:
                break
        (Path(harness.OUT) / f"tco-{args.variant}-{'small' if small else 'large'}-gc{int(gc)}.raw").write_text(raw)
        if result.returncode or "Sanitizer" in raw or "runtime error:" in raw:
            raise SystemExit(raw)
        n = 100 if small else 100000
        captures = "100" if small else ",".join(str(i) for i in range(0, n + 1, 10000))
        expected = [f"mutual {n} {captures}", f"method {n + 19}", "closure 31",
                    "throw bottom", "default 8", f"recovered {n // 10}"]
        observed = [line for line in result.stdout.splitlines() if not line.startswith("#info")]
        if observed != expected:
            raise SystemExit(f"output mismatch: {observed!r}")
        capacity = re.search(r"\blive_max=(\d+)", raw)
        if not capacity:
            raise SystemExit("missing live capacity measurement")
        capacities.append(int(capacity[1]))
        if gc:
            stops = re.search(r"\bstops=(\d+)", raw)
            resumes = re.search(r"\bresumes=(\d+)", raw)
            if not stops or not resumes or int(stops[1]) < 100 or stops[1] != resumes[1]:
                raise SystemExit("missing or unbalanced TCO suspension coverage")
        print(f"TCO n={n} gc={int(gc)} live_max={capacity[1]} OK", flush=True)
    if len(set(capacities)) != 1:
        raise SystemExit(f"TCO capacity changed with depth or suspension: {capacities}")
    print("TCO constant-capacity and ownership checks OK")
    for gc in [False, True]:
        cmd = [str(runner), "--stats"]
        if gc:
            cmd += ["--force-yield", "--gc-on-yield", "--include", str(here / "tco_small.js")]
        cmd.append(str(here / "tco_paths.js"))
        for attempt in range(5):
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
            raw = result.stdout + result.stderr
            if "AddressSanitizer:DEADLYSIGNAL" not in raw:
                break
        n = 100 if gc else 100000
        expected = ["conditional 42", "catch 42", "logical 0 42", f"comma 42 {n + 1}"]
        observed = [line for line in result.stdout.splitlines() if not line.startswith("#info")]
        if result.returncode or "Sanitizer" in raw or "runtime error:" in raw or observed != expected:
            raise SystemExit(raw)
        print(f"TCO return paths n={n} gc={int(gc)} OK", flush=True)


if __name__ == "__main__":
    main()
