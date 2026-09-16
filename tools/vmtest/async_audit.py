#!/usr/bin/env python3
"""Compare async-related Test262 cases across two VM paths, without blessing.

Requires the entire pinned checkout (git sparse-checkout disable). Selection
is deliberately broad: async/await text or async feature metadata, plus any
recursion/stack-limit text even outside async directories. This is evidence
about this revision, not a proof that arbitrary code never depends on depth.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

import test262 as harness


ASYNC = re.compile(r"\b(?:async|await)\b|asyncFunctions|asyncIteration")
DEPTH = re.compile(r"recurs|stack.{0,30}(?:overflow|limit|depth)|maximum.{0,20}stack", re.I)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--variants", nargs=2, default=["o2-recur", "o2"])
    ap.add_argument("-j", type=int, default=8)
    ap.add_argument("--timeout", type=float, default=30)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    if args.j < 1 or args.timeout <= 0:
        ap.error("worker count and timeout must be positive")
    if args.variants[0] == args.variants[1]:
        ap.error("choose two distinct variants")
    for variant in args.variants:
        if not (Path(harness.OUT) / f"vmrun-{variant}").is_file():
            ap.error(f"build missing variant first: {variant}")
    checkout = Path(harness.T262)
    git = ["git", "-C", str(checkout)]
    # Native Git avoids 53k cross-filesystem stat calls for the clean check.
    if str(checkout).startswith("/mnt/") and shutil.which("git.exe"):
        native_path = subprocess.check_output(["wslpath", "-w", str(checkout)], text=True).strip()
        git = [shutil.which("git.exe"), "-C", native_path]
    head = subprocess.check_output(git + ["rev-parse", "HEAD"], text=True).strip()
    if head != harness.PINNED:
        raise SystemExit("unexpected Test262 revision")
    changed = subprocess.check_output(
        git + ["status", "--porcelain", "--untracked-files=no"], text=True)
    if changed.strip():
        raise SystemExit("Test262 tracked files differ from pinned revision")
    tracked = subprocess.check_output(
        git + ["ls-tree", "-r", "--name-only", "HEAD", "test"], text=True).splitlines()
    files = [p for p in tracked if p.endswith(".js") and "_FIXTURE" not in p]
    sparse = subprocess.run(
        git + ["config", "--bool", "core.sparseCheckout"],
        capture_output=True, text=True, check=False).stdout.strip()
    if sparse == "true":
        raise SystemExit("disable sparse checkout before the full audit")
    selected, depth = [], []
    pending = set(files)
    # Reading 53k tiny files over WSL's /mnt/c costs minutes in metadata
    # round trips. Stream the same verified, clean revision from Git instead.
    # No archive member is extracted to disk; only one source is held at once.
    archive = subprocess.Popen(git + ["archive", "HEAD", "test"],
                               stdout=subprocess.PIPE)
    try:
        with tarfile.open(fileobj=archive.stdout, mode="r|") as entries:
            for entry in entries:
                if entry.name not in pending:
                    continue
                source = entries.extractfile(entry).read().decode("utf-8")
                pending.remove(entry.name)
                relative = entry.name.removeprefix("test/")
                if DEPTH.search(source):
                    depth.append(relative)
                if ASYNC.search(source) or DEPTH.search(source):
                    selected.append(relative)
    finally:
        archive.stdout.close()
        archive_code = archive.wait()
    if archive_code or pending:
        raise SystemExit(f"incomplete source archive: exit={archive_code} missing={len(pending)}")
    print(f"scanned={len(files)} selected={len(selected)} depth_candidates={len(depth)}", flush=True)
    report = dict(revision=head, scanned=len(files), selected=selected,
                  depth_candidates=depth, binaries={}, results={}, differences=[])
    with tempfile.TemporaryDirectory(prefix="vm-async-audit-") as temporary:
        for variant in args.variants:
            original = Path(harness.OUT) / f"vmrun-{variant}"
            report["binaries"][variant] = hashlib.sha256(original.read_bytes()).hexdigest()
            runner = Path(temporary) / variant
            shutil.copy2(original, runner)
            results = {}
            with cf.ThreadPoolExecutor(max_workers=args.j) as pool:
                futures = [pool.submit(harness.run_one, str(runner), name, args.timeout) for name in selected]
                for count, future in enumerate(cf.as_completed(futures), 1):
                    for key, verdict, detail in future.result():
                        results[key] = [verdict, detail]
                    if count % 1000 == 0:
                        print(f"{variant}: {count}/{len(selected)}", flush=True)
            report["results"][variant] = results
            print(variant, {v: sum(r[0] == v for r in results.values()) for v in ["PASS", "FAIL", "SKIP"]}, flush=True)
    left, right = [report["results"][v] for v in args.variants]
    for key in sorted(left.keys() | right.keys()):
        if left.get(key) != right.get(key):
            report["differences"].append(dict(test=key, before=left.get(key), after=right.get(key)))
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"differences={len(report['differences'])}; report={output}", flush=True)
    return int(bool(report["differences"]))


if __name__ == "__main__":
    raise SystemExit(main())
