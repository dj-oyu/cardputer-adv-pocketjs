#!/usr/bin/env python3
"""Host timing baseline for the corpus benchmarks (vmrun-o2).

    python3 tools/vmtest/timing.py                 # 30 runs each, print table
    python3 tools/vmtest/timing.py --write         # also write timing-baseline.txt
    python3 tools/vmtest/timing.py -n 50 bench_calls

Every number here is 実測(host): x86-64 under WSL, gcc -O2, not Xtensa at
-Os, and says nothing absolute about the device. It exists so a VM level can
be compared against L0 on the SAME host with the SAME binary flags; a change
smaller than the spread between runs is not a result. The time is vmrun's
own "#info time_ns" (eval + drains + frames), so process start and runtime
creation are excluded. Runs are sequential to avoid self-contention.
"""
import argparse
import glob
import hashlib
import os
import platform
import re
import shutil
import statistics
import subprocess
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.environ.get("VMTEST_OUT", os.path.join(ROOT, ".cache", "vmtest"))
TIME = re.compile(r"^#info time_ns=(\d+)$", re.M)


def flags_of(path):
    with open(path) as f:
        first = f.readline()
    extra = first.split("vmrun-flags:", 1)[1].split() if "vmrun-flags:" in first else []
    return ["--profile", "host"] + extra


def pct(xs, p):
    xs = sorted(xs)
    k = max(0, min(len(xs) - 1, int(round(p / 100 * len(xs) + 0.5)) - 1))
    return xs[k]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=30)
    ap.add_argument("--variant", default="o2")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    src = os.path.join(OUT, f"vmrun-{a.variant}")
    tmp = tempfile.mkdtemp(prefix="vmtiming-")
    vmrun = os.path.join(tmp, "vmrun")
    shutil.copy2(src, vmrun)
    names = a.names or sorted(os.path.basename(p)[:-3] for p in glob.glob(os.path.join(HERE, "corpus", "bench_*.js")))
    rows = []
    for name in names:
        path = os.path.join(HERE, "corpus", name + ".js")
        cmd = [vmrun] + flags_of(path) + ["--time", os.path.basename(path)]
        subprocess.run(cmd, cwd=os.path.dirname(path), capture_output=True)  # warm the page cache
        ts = []
        for _ in range(a.n):
            p = subprocess.run(cmd, cwd=os.path.dirname(path), capture_output=True, text=True)
            m = TIME.search(p.stderr)
            if p.returncode != 0 or not m:
                raise SystemExit(f"{name}: exit {p.returncode}\n{p.stdout}{p.stderr}")
            ts.append(int(m.group(1)) / 1e6)
        rows.append((name, statistics.median(ts), pct(ts, 95), max(ts), min(ts)))
    shutil.rmtree(tmp, ignore_errors=True)

    cpu = "unknown"
    try:
        with open("/proc/cpuinfo") as f:
            cpu = re.search(r"model name\s*:\s*(.*)", f.read()).group(1)
    except (OSError, AttributeError):
        pass
    gcc = subprocess.run(["gcc", "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    with open(os.path.join(ROOT, "components/quickjs-ng/quickjs-ng/quickjs.c"), "rb") as f:
        qsha = hashlib.sha1(f.read()).hexdigest()[:12]
    lines = [
        "# vmtest timing baseline -- 実測(host), NOT device",
        f"# date={time.strftime('%Y-%m-%d')} host={platform.node()} cpu={cpu}",
        f"# {gcc}; vmrun-{a.variant} (-O2 -g); quickjs.c sha1={qsha}; runs={a.n} each, sequential",
        "# time = vmrun '#info time_ns' (eval + drains + frames), ms",
        f"{'benchmark':<18}{'median':>10}{'p95':>10}{'max':>10}{'min':>10}",
    ]
    for name, med, p95, mx, mn in rows:
        lines.append(f"{name:<18}{med:>10.2f}{p95:>10.2f}{mx:>10.2f}{mn:>10.2f}")
    text = "\n".join(lines) + "\n"
    print(text, end="")
    if a.write:
        with open(os.path.join(HERE, "timing-baseline.txt"), "w", newline="\n") as f:
            f.write(text)
        print("wrote tools/vmtest/timing-baseline.txt")


if __name__ == "__main__":
    main()
