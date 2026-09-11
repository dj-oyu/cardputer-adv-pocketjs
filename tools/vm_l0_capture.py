"""Runs each VM_PROBE L0 workload (apps/vmprobe/*.js, USB diagnostic letters
A-F -- see main/main.c's usb_stroke() and main/app_session.c's app_start_test())
for a fixed duration, parses the device's "VMPROBE STATIC ..." /
"VMPROBE WINDOW ..." log lines (main/pocket/vmprobe.c), prints a summary per
workload, and appends one JSONL record per ~1s window to .cache/vm/l0.jsonl
(one capture run's file; pass --out to change it, --append to keep a prior
run's records instead of starting a fresh file).

Requires a device flashed from a CONFIG_POCKET_VM_PROBE=y build --
build_vm_probe in this worktree, built with:

    idf.py -B build_vm_probe \
        -D SDKCONFIG=build_vm_probe/sdkconfig \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.vmprobe.defaults" build

(-D SDKCONFIG is not optional: without it the probe option lands in the
project-root sdkconfig that every other build_* directory reads -- see
sdkconfig.vmprobe.defaults.)

A normal build (CONFIG_POCKET_VM_PROBE off, the default) has none of the
'A'..'F' triggers this script sends and no VMPROBE lines to parse -- running
this against one just times out waiting for HOME_READY after every workload.

    python tools/vm_l0_capture.py --port COM3 [--seconds 15] [--workload A]

Run with the ESP-IDF Python environment (pyserial).

What "median" means here: the device itself reports min/median/max over each
~1s window using a capped 48-sample ring (main/pocket/vmprobe.c) -- exact for
a window with <=48 samples of a metric, the last 48 otherwise. This script's
own per-workload summary then takes the min of window mins, the max of
window maxes (both exact), and the MEDIAN OF THE WINDOWS' OWN MEDIANS for a
"med" column -- an estimate of the run's median, not an exact one over every
individual sample. The JSONL keeps every window's own line untouched, so a
reader wanting an exact figure can recompute from there.
"""
import argparse
import json
import pathlib
import re
import statistics
import time

import serial

WORKLOADS = {
    "A": "sync_loop",
    "B": "deep_recursion",
    "C": "closures",
    "D": "promise_chain",
    "E": "io_wait",
    "F": "async_generator",
}

# Field name -> int or float, matched against main/pocket/vmprobe.c's
# ESP_LOGI format string byte for byte. If that format string changes, this
# regex is the other half of the change (same rule CLAUDE.md gives the
# uppercase HOME_READY-family markers, extended to this new family).
WINDOW_RE = re.compile(
    r"VMPROBE WINDOW ms=(?P<ms>\d+) "
    r"frame_n=(?P<frame_n>\d+) frame_min=(?P<frame_min>[\d.]+) "
    r"frame_med=(?P<frame_med>[\d.]+) frame_max=(?P<frame_max>[\d.]+) "
    r"jobs_n=(?P<jobs_n>\d+) jobs_min=(?P<jobs_min>\d+) "
    r"jobs_med=(?P<jobs_med>[\d.]+) jobs_max=(?P<jobs_max>\d+) "
    r"qpeak_max=(?P<qpeak_max>\d+) "
    r"lat_n=(?P<lat_n>\d+) lat_min=(?P<lat_min>[\d.]+) "
    r"lat_med=(?P<lat_med>[\d.]+) lat_max=(?P<lat_max>[\d.]+) "
    r"heap_free=(?P<heap_free>\d+) heap_largest=(?P<heap_largest>\d+) "
    r"js_used=(?P<js_used>\d+) js_limit=(?P<js_limit>\d+) stack_hw=(?P<stack_hw>\d+)"
)
INT_FIELDS = {
    "ms", "frame_n", "jobs_n", "jobs_min", "jobs_max", "qpeak_max", "lat_n",
    "heap_free", "heap_largest", "js_used", "js_limit", "stack_hw",
}
STATIC_RE = re.compile(
    r"VMPROBE STATIC engine=(?P<engine>\S+) compiler=(?P<compiler>.+?) "
    r"opt=(?P<opt>\S+) sizeof_jsvalue=(?P<sizeof_jsvalue>\d+) "
    r"sizeof_stackframe=(?P<sizeof_stackframe>\d+) "
    r"sizeof_varref=(?P<sizeof_varref>\d+) fw=(?P<fw>\S+)"
)


def parse_window(line):
    m = WINDOW_RE.search(line)
    if not m:
        return None
    d = m.groupdict()
    return {k: (int(v) if k in INT_FIELDS else float(v)) for k, v in d.items()}


def wait(ser, marker, limit=8):
    end = time.monotonic() + limit
    seen = []
    while time.monotonic() < end:
        line = ser.readline().decode(errors="replace").strip()
        if line:
            seen.append(line)
        if marker in line:
            return line
    raise RuntimeError(f"waiting for {marker}: {seen[-8:]}")


def run_workload(ser, letter, seconds, jsonl_fh, run_id):
    ser.reset_input_buffer()
    ser.write(letter.encode())
    static = None
    windows = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            continue
        # The workload ended on its own (frame() threw, the guest ran out of
        # heap or stack): the shell is back and every later window would be
        # empty. Say so instead of reporting a short run as a clean one --
        # the first deep_recursion.js threw RangeError every frame.
        if "HOME_READY" in line:
            print(f"  (warning: {WORKLOADS[letter]} ended early after "
                  f"{len(windows)} windows -- see the log above)", flush=True)
            return static, windows
        if static is None and "VMPROBE STATIC" in line:
            m = STATIC_RE.search(line)
            if m:
                static = m.groupdict()
                print(line, flush=True)
            continue
        if "VMPROBE WINDOW" in line:
            w = parse_window(line)
            if w:
                windows.append(w)
                print(line, flush=True)
                if jsonl_fh:
                    record = dict(w)
                    record["run_id"] = run_id
                    record["workload"] = WORKLOADS[letter]
                    record["static"] = static
                    jsonl_fh.write(json.dumps(record) + "\n")
                    jsonl_fh.flush()
    # Back leaves a running test app the same way it leaves any app
    # (main.c's tick_run(): app_tick(0x2000) then app_request_stop()).
    ser.write(b"q")
    try:
        wait(ser, "HOME_READY", limit=5)
    except RuntimeError as e:
        print(f"  (warning: {e})", flush=True)
    return static, windows


def summarize(windows, prefix):
    n_key, min_key, med_key, max_key = (
        f"{prefix}_n", f"{prefix}_min", f"{prefix}_med", f"{prefix}_max",
    )
    have = [w for w in windows if w.get(n_key, 0)]
    if not have:
        return None
    return {
        "windows": len(have),
        "samples": sum(w[n_key] for w in have),
        "min": min(w[min_key] for w in have),
        # Median of the windows' own medians -- see the module docstring for
        # exactly what this is and is not an estimate of.
        "med~": statistics.median(w[med_key] for w in have),
        "max": max(w[max_key] for w in have),
    }


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", required=True)
    p.add_argument("--seconds", type=float, default=15.0,
                    help="capture duration per workload")
    p.add_argument("--workload", choices=sorted(WORKLOADS),
                    help="run only this one letter (default: all six, in order)")
    p.add_argument("--out", default=".cache/vm/l0.jsonl")
    p.add_argument("--append", action="store_true",
                    help="keep a prior run's records instead of truncating --out")
    a = p.parse_args()

    out_path = pathlib.Path(a.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    run_id = time.strftime("%Y%m%dT%H%M%S")

    ser = serial.Serial(a.port, 115200, timeout=0.2)
    time.sleep(1.5)
    ser.reset_input_buffer()

    letters = [a.workload] if a.workload else list(WORKLOADS)
    try:
        ser.write(b"q")
        wait(ser, "HOME_READY")
        with open(out_path, "a" if a.append else "w") as fh:
            for letter in letters:
                name = WORKLOADS[letter]
                print(f"=== {letter} {name} ({a.seconds:.0f}s) ===", flush=True)
                static, windows = run_workload(ser, letter, a.seconds, fh, run_id)
                if not windows:
                    print(f"  no VMPROBE WINDOW lines seen -- is this a "
                          f"CONFIG_POCKET_VM_PROBE build?", flush=True)
                    continue
                for prefix in ("frame", "jobs", "lat"):
                    s = summarize(windows, prefix)
                    if s:
                        print(f"  {prefix:5s} n={s['samples']:<6d} "
                              f"min={s['min']:<10.3f} med~={s['med~']:<10.3f} "
                              f"max={s['max']:<10.3f} ({s['windows']} windows)",
                              flush=True)
                qpeaks = [w["qpeak_max"] for w in windows]
                print(f"  qpeak max={max(qpeaks)}", flush=True)
                last = windows[-1]
                print(f"  last window: heap_free={last['heap_free']} "
                      f"heap_largest={last['heap_largest']} "
                      f"js_used={last['js_used']}/{last['js_limit']} "
                      f"stack_hw={last['stack_hw']}", flush=True)
        print(f"JSONL: {out_path}", flush=True)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
