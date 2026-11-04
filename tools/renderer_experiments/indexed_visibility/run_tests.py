#!/usr/bin/env python3
"""Compile and run only deterministic synthetic visibility tests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(".test-output"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--disable-leak-sanitizer", action="store_true",
                        help="Use only when the host cannot run LeakSanitizer; recorded as not tested.")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tests = (("visibility", True), ("nearest", False), ("bruteforce", False), ("storage", True))
    modes = (("O0", False), ("O2", False), ("O0", True), ("O2", True))
    source_hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in sorted(root.iterdir()) if p.is_file()}
    report = {"status": "RUNNING", "tests": [], "source_sha256": source_hashes,
              "leak_sanitizer": "NOT_TESTED_HOST_LIMITATION" if args.disable_leak_sanitizer else "ENABLED",
              "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0]}
    report_path = output / "RESULTS.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    started = time.monotonic()
    try:
        for optimization, sanitized in modes:
            mode = optimization + ("-asan-ubsan" if sanitized else "")
            directory = output / mode
            directory.mkdir()
            flags = ["-std=c11", "-" + optimization, "-g", "-Wall", "-Wextra", "-Werror",
                     "-Wno-misleading-indentation", "-ffp-contract=off", "-fexcess-precision=standard"]
            if sanitized:
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            for name, white_box in tests:
                binary = directory / name
                sources = ["test_" + name + ".c"] + ([] if white_box else ["visibility.c"])
                command = [args.cc, *flags, *sources, "-lm", "-o", str(binary)]
                record = {"mode": mode, "test": name, "status": "RUNNING",
                          "compile_flags": flags, "sources": sources}
                report["tests"].append(record)
                report_path.write_text(json.dumps(report, indent=2) + "\n")
                compiled = subprocess.run(command, cwd=root, capture_output=True, text=True)
                (directory / (name + ".compile.log")).write_text(compiled.stdout + compiled.stderr)
                if compiled.returncode:
                    record["status"] = "COMPILE_FAILED"
                    record["returncode"] = compiled.returncode
                    raise RuntimeError(mode + "/" + name + " did not compile")
                env = dict(os.environ)
                if sanitized:
                    env["ASAN_OPTIONS"] = "halt_on_error=1:detect_leaks=" + ("0" if args.disable_leak_sanitizer else "1")
                    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
                ran = subprocess.run([str(binary)], cwd=directory, env=env, capture_output=True, text=True)
                log = ran.stdout + ran.stderr
                (directory / (name + ".log")).write_text(log)
                record.update(status="PASS" if ran.returncode == 0 else "FAILED", returncode=ran.returncode,
                              output=ran.stdout.strip(),
                              output_sha256=hashlib.sha256(log.encode()).hexdigest(),
                              binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
                report_path.write_text(json.dumps(report, indent=2) + "\n")
                if ran.returncode:
                    raise RuntimeError(mode + "/" + name + " failed")
                print(mode, name, "PASS", flush=True)
        report["status"] = "PASS"
    except Exception as error:
        report["status"] = "FAILED"
        report["error"] = str(error)
        raise
    finally:
        report["elapsed_seconds"] = time.monotonic() - started
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("All 16 synthetic test executions passed.")


if __name__ == "__main__":
    main()
