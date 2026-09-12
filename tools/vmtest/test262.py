#!/usr/bin/env python3
"""Runs a Test262 subset through vmrun and compares against the baseline.

    python3 tools/vmtest/test262.py --fetch          # clone the pinned commit into .cache/test262
    python3 tools/vmtest/test262.py                  # run, diff against test262-baseline.txt
    python3 tools/vmtest/test262.py --write-baseline # (re)record the pass list -- L0 only, see README
    python3 tools/vmtest/test262.py --variant o2 -j 8 language/statements/try

The baseline is the set of (test, mode) pairs that PASS at L0. A later level
may pass more; it must not pass fewer. Failures and skips are recorded only as
counts: quickjs-ng 0.14.0 does not pass all of these and that is not this
harness's business -- losing a pass is.

Each test runs in a fresh vmrun (--profile host: 64 MiB heap, 7 MiB stack --
Test262 is a semantics check, the device limits are the corpus's job), with
--test262 so $262 exists and uncaught errors are reported by name.
"""
import argparse
import concurrent.futures as cf
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
T262 = os.path.join(ROOT, ".cache", "test262")
OUT = os.environ.get("VMTEST_OUT", os.path.join(ROOT, ".cache", "vmtest"))
BASELINE = os.path.join(HERE, "test262-baseline.txt")

# tc39/test262 main as of 2026-09-12. Pinned so the baseline stays comparable;
# moving it means re-recording the baseline in its own commit.
PINNED = "72faf8ec1445c55149615e8b35187830783aba1a"

# The subset the spec's L1/L2 touch: call paths, frames, generators/async,
# exceptions, Promise jobs, eval. Directories are relative to test/.
SUBSET = [
    "language/statements/async-function",
    "language/statements/async-generator",
    "language/statements/generators",
    "language/statements/try",
    "language/statements/for-of",
    "language/expressions/call",
    "language/expressions/new",
    "language/expressions/async-arrow-function",
    "language/expressions/async-function",
    "language/expressions/async-generator",
    "language/expressions/await",
    "language/expressions/yield",
    "language/expressions/arrow-function",
    "language/eval-code/direct",
    "built-ins/Promise",
]
# Proxy is large and mostly trap-by-trap object-model checks; the call/
# construct/get/set traps are what re-enter JS, so those are taken whole.
PROXY_SAMPLE = [
    "built-ins/Proxy/apply",
    "built-ins/Proxy/construct",
    "built-ins/Proxy/get",
    "built-ins/Proxy/set",
    "built-ins/Proxy/has",
    "built-ins/Proxy/revocable",
]
SPARSE = ["harness"] + ["test/" + d for d in SUBSET + PROXY_SAMPLE]

# Features the host cannot provide (no agent threads, no [[IsHTMLDDA]]).
# Tests needing them are SKIP, not FAIL, so the counts say what was judged.
SKIP_FEATURES = {"IsHTMLDDA", "Atomics.waitAsync", "SharedArrayBuffer", "Atomics"}
SKIP_FLAGS = {"CanBlockIsTrue"}
# Driver flags added to every run (--force-yield puts them here).
EXTRA_FLAGS = []


def fetch():
    if os.path.isdir(os.path.join(T262, ".git")):
        head = subprocess.run(["git", "-C", T262, "rev-parse", "HEAD"], capture_output=True,
                              text=True).stdout.strip()
        if head == PINNED:
            print(f"test262 already at {PINNED}")
            return
        shutil.rmtree(T262)
    os.makedirs(T262)
    git = lambda *a: subprocess.run(["git", "-C", T262, *a], check=True)
    git("init", "-q")
    git("remote", "add", "origin", "https://github.com/tc39/test262.git")
    git("config", "core.sparseCheckout", "true")
    git("sparse-checkout", "set", "--no-cone", *SPARSE)
    git("fetch", "--depth", "1", "--filter=blob:none", "origin", PINNED)
    git("checkout", "-q", "FETCH_HEAD")
    print(f"test262 {PINNED} -> {T262}")


META = re.compile(r"/\*---(.*?)---\*/", re.S)


def parse_meta(src):
    m = META.search(src)
    meta = {"includes": [], "flags": [], "features": [], "negative": None}
    if not m:
        return meta
    text = m.group(1)
    for key in ("includes", "flags", "features"):
        inline = re.search(rf"^\s*{key}:\s*\[(.*?)\]", text, re.M | re.S)
        if inline:
            meta[key] = [x.strip() for x in inline.group(1).split(",") if x.strip()]
            continue
        block = re.search(rf"^\s*{key}:\s*\n((?:\s+-\s*.+\n?)+)", text, re.M)
        if block:
            meta[key] = [x.strip()[1:].strip() for x in block.group(1).splitlines() if x.strip()]
    neg = re.search(r"^\s*negative:\s*\n\s+phase:\s*(\S+)\s*\n\s+type:\s*(\S+)", text, re.M)
    if not neg:
        neg = re.search(r"^\s*negative:\s*\n\s+type:\s*(\S+)\s*\n\s+phase:\s*(\S+)", text, re.M)
        if neg:
            meta["negative"] = {"phase": neg.group(2), "type": neg.group(1)}
    else:
        meta["negative"] = {"phase": neg.group(1), "type": neg.group(2)}
    return meta


def collect(filters):
    dirs = filters or (SUBSET + PROXY_SAMPLE)
    tests = []
    for d in dirs:
        base = os.path.join(T262, "test", d)
        if os.path.isfile(base):
            tests.append(os.path.relpath(base, os.path.join(T262, "test")))
            continue
        for dirpath, _, files in os.walk(base):
            for f in files:
                if f.endswith(".js") and "_FIXTURE" not in f:
                    tests.append(os.path.relpath(os.path.join(dirpath, f), os.path.join(T262, "test")))
    return sorted(set(t.replace(os.sep, "/") for t in tests))


def run_one(vmrun, rel, timeout):
    """Returns a list of (key, verdict, detail); key = "<rel> <mode>"."""
    path = os.path.join(T262, "test", rel)
    with open(path, encoding="utf-8") as f:
        src = f.read()
    meta = parse_meta(src)
    flags = set(meta["flags"])
    if flags & SKIP_FLAGS or set(meta["features"]) & SKIP_FEATURES:
        return [(f"{rel} default", "SKIP", "feature/flag")]
    if "module" in flags:
        modes = ["module"]
    elif "raw" in flags or "noStrict" in flags:
        modes = ["sloppy"]
    elif "onlyStrict" in flags:
        modes = ["strict"]
    else:
        modes = ["sloppy", "strict"]
    includes = [] if "raw" in flags else ["assert.js", "sta.js"]
    if "async" in flags:
        includes.append("doneprintHandle.js")
    includes += meta["includes"]
    results = []
    for mode in modes:
        cmd = [vmrun, "--profile", "host", "--test262"] + EXTRA_FLAGS
        for inc in includes:
            cmd += ["--include", os.path.join(T262, "harness", inc)]
        if mode == "module":
            cmd.append("--module")
        elif mode == "strict":
            cmd.append("--strict")
        cmd.append(path)
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                               errors="replace", cwd=os.path.dirname(path))
            out = p.stdout + p.stderr
            code = p.returncode
        except subprocess.TimeoutExpired:
            results.append((f"{rel} {mode}", "FAIL", "timeout"))
            continue
        results.append((f"{rel} {mode}",) + judge(meta, flags, code, out))
    return results


UNCAUGHT = re.compile(r"^vmrun: uncaught (\S+) (\S+)$", re.M)


def judge(meta, flags, code, out):
    # Driver annotations, not program output. "#info" is the harness-wide
    # marker for a measurement (run.sh strips the same two prefixes), and from
    # L1 vmrun prints "#info turns=..." on every run -- a sync test that
    # deliberately leaves a rejection behind is judged on its rejection reports
    # alone, so an annotation left in here would fail it.
    out = "\n".join(l for l in out.splitlines()
                    if not l.startswith(("vmrun: note:", "#info")))
    if "AddressSanitizer" in out or "runtime error:" in out or "LeakSanitizer" in out:
        return "FAIL", "sanitizer"
    if code < 0 or code >= 128:
        return "FAIL", f"signal/abort {code}"
    neg = meta["negative"]
    if neg:
        m = UNCAUGHT.search(out)
        unhandled = re.search(r"Unhandled Promise rejection: (\w+)", out)
        name = m.group(2) if m else (unhandled.group(1) if unhandled else None)
        if code != 0 and name == neg["type"]:
            if neg["phase"] == "parse" and m and m.group(1) != "parse":
                return "FAIL", f"expected parse-phase {neg['type']}, got {m.group(1)}"
            return "PASS", ""
        return "FAIL", f"expected {neg['phase']} {neg['type']}, got exit={code} {name}"
    if "async" in flags:
        if "Test262:AsyncTestFailure" in out:
            return "FAIL", "async failure"
        if "Test262:AsyncTestComplete" not in out:
            return "FAIL", "async test never completed"
        return ("PASS", "") if code in (0, 2) and "vmrun: uncaught" not in out else ("FAIL", f"exit={code}")
    # Exit 2 with nothing but guest rejection reports: several sync tests
    # leave a rejected promise behind on purpose (e.g. Promise.all with a
    # throwing resolve). Test262 judges a sync test by its synchronous run
    # only; the report timing itself is the corpus's job (rejections.js).
    if code == 2 and "vmrun: uncaught" not in out and all(
            l.startswith("E pocketjs_guest: Unhandled Promise rejection:")
            for l in out.strip().splitlines() if l.strip()):
        return "PASS", ""
    if code != 0:
        last = out.strip().splitlines()[-1] if out.strip() else ""
        return "FAIL", f"exit={code} {last[:120]}"
    return "PASS", ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fetch", action="store_true")
    ap.add_argument("--variant", default="asan")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--timeout", type=float, default=30)
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--force-yield", action="store_true",
                    help="L1+: yield at every VM checkpoint; the pass set must not change")
    ap.add_argument("filters", nargs="*")
    a = ap.parse_args()
    if a.force_yield:
        EXTRA_FLAGS.append("--force-yield")
    if a.fetch:
        fetch()
        return
    if not os.path.isdir(os.path.join(T262, "test")):
        raise SystemExit("no test262 checkout; run with --fetch first")
    vmrun_src = os.path.join(OUT, f"vmrun-{a.variant}")
    if not os.path.exists(vmrun_src):
        raise SystemExit(f"missing {vmrun_src}; run tools/vmtest/build.sh {a.variant}")
    # Copy to local disk: thousands of execs of a binary on /mnt/c are slow.
    tmpdir = tempfile.mkdtemp(prefix="vmtest262-")
    vmrun = os.path.join(tmpdir, "vmrun")
    shutil.copy2(vmrun_src, vmrun)
    os.environ.setdefault("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1")
    os.environ.setdefault("UBSAN_OPTIONS", "halt_on_error=1")

    tests = collect(a.filters)
    results = {}
    with cf.ThreadPoolExecutor(max_workers=a.j) as ex:
        futs = {ex.submit(run_one, vmrun, t, a.timeout): t for t in tests}
        done = 0
        for fut in cf.as_completed(futs):
            for key, verdict, detail in fut.result():
                results[key] = (verdict, detail)
            done += 1
            if done % 500 == 0:
                print(f"  {done}/{len(tests)} files", file=sys.stderr)
    shutil.rmtree(tmpdir, ignore_errors=True)

    passes = sorted(k for k, (v, _) in results.items() if v == "PASS")
    fails = sorted(k for k, (v, _) in results.items() if v == "FAIL")
    skips = sorted(k for k, (v, _) in results.items() if v == "SKIP")
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, f"test262-results-{a.variant}.txt"), "w") as f:
        for k in sorted(results):
            v, d = results[k]
            f.write(f"{v} {k} {d}\n")
    print(f"test262 [{a.variant}] {PINNED[:12]}: {len(passes)} pass, {len(fails)} fail, "
          f"{len(skips)} skip ({len(tests)} files)")
    if a.verbose:
        for k in fails:
            print("FAIL", k, results[k][1])

    if a.write_baseline:
        if a.filters:
            raise SystemExit("--write-baseline records the whole subset; drop the filters")
        with open(BASELINE, "w", newline="\n") as f:
            f.write(f"# test262 {PINNED} vmrun-{a.variant} quickjs-ng 0.14.0 (vendored)\n")
            f.write(f"# pass={len(passes)} fail={len(fails)} skip={len(skips)}\n")
            for k in passes:
                f.write(k + "\n")
        print(f"wrote {BASELINE}")
        return
    with open(BASELINE) as f:
        base = {l.strip() for l in f if l.strip() and not l.startswith("#")}
    judged = set(results)
    lost = sorted(k for k in base if k in judged and results[k][0] != "PASS")
    gained = sorted(k for k in passes if k not in base)
    for k in lost:
        print("REGRESSED", k, results[k][0], results[k][1])
    if gained:
        print(f"{len(gained)} newly passing (not in baseline); first: {gained[:5]}")
    print(f"regressions: {len(lost)}")
    sys.exit(1 if lost else 0)


if __name__ == "__main__":
    main()
