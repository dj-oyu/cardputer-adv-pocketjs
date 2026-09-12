"""Runs the VM_PROBE L0 matrix on the board: six workloads (apps/vmprobe/*.js,
USB letters A-F) x fixed contention conditions (USB letters P-W) x repetitions,
and writes one JSONL record per ~1s measurement window to .cache/vm/l0.jsonl.

    python tools/vm_l0_capture.py --port COM3                    # the matrix
    python tools/vm_l0_capture.py --port COM3 --conditions base,wifi --reps 1
    python tools/vm_l0_capture.py --summarize .cache/vm/l0.jsonl  # no device

Requires a device flashed from a CONFIG_POCKET_VM_PROBE=y build, and the
ESP-IDF Python environment (pyserial):

    idf.py -B build_vm_l0 \
        -D SDKCONFIG=build_vm_l0/sdkconfig \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.vmprobe.defaults" build

(-D SDKCONFIG is not optional: without it the probe option lands in the
project-root sdkconfig that every other build_* directory reads.)

WHAT IS EXACT AND WHAT IS NOT
-----------------------------
The device does not summarize. Every frame contributes one sample of frame
time (whole pocketjs_ui_turn), frame() time, drain time and job count, and
every completion contributes one latency sample; main/pocket/vmprobe.c writes
them out raw, closing a window at 1 s OR at 64 samples, whichever comes first,
so nothing is averaged away and nothing is overwritten. This script therefore
computes median / p95 / max EXACTLY over every sample of a capture -- p95 by
the nearest-rank method, sorted[ceil(0.95*n)-1] -- rather than estimating from
per-window summaries, which is what the first version of this file did and
which cannot produce a p95 at all.

The one way exactness can be lost is a window reporting lat_drop>0 (more
completions inside one frame than the device's array holds); this script
reports any such window loudly. heap_free / heap_largest / js_used / stack_hw
are NOT per frame: the device samples them every 8th frame and reports the
window's extreme, so those columns are minima/maxima over a 4 Hz sample, which
is stated rather than hidden.

CONDITIONS
----------
Each condition is one USB byte the firmware turns into a bit mask, and the JS
side of it is apps/vmprobe/condition.js, evaluated on top of the workload:

  base  'P'  nothing but the workload.
  ui    'Q'  a pocket.ui screen (rect + text) whose text is set every frame,
             so the UI core's tick, layout and draw are inside every turn.
  audio 'R'  pocket.audio.tone, 440 Hz 1 s, re-armed from its own completion:
             the real synthesiser, the real I2S path, the audio task at
             priority 7 preempting the ui task at 5.
  wifi   'T' pocket.net.wifi.acquire({profileId:'default'}) and then a plain
             GET of the LAN gateway (derived from the board's own address)
             every 3 s, body read to EOF -- radio linked for the whole run,
             with a periodic request on top. Plain HTTP and not HTTPS for a
             measured reason: apps/vmprobe/README.md.
  all   'W'  all three at once.

The device echoes what actually happened as "VMCOND ..." lines (a Wi-Fi that
will not link logs its error code and the run continues); this script keeps
them in the JSONL so a condition that degraded is visible in the report rather
than assumed.
"""
import argparse
import json
import math
import pathlib
import re
import statistics
import sys
import time

WORKLOADS = {
    "A": "sync_loop",
    "B": "deep_recursion",
    "C": "closures",
    "D": "promise_chain",
    "E": "io_wait",
    "F": "async_generator",
}
# name -> mask, and the USB byte is chr(ord('P') + mask). Keep in step with
# main/pocket/vmprobe.h's VMPROBE_COND_* and main.c's usb_stroke().
CONDITIONS = {"base": 0, "ui": 1, "audio": 2, "wifi": 4, "all": 7}

# Matched against main/pocket/vmprobe.c's format strings byte for byte. If one
# of those changes, this is the other half of the change (the same rule
# CLAUDE.md gives the HOME_READY family of markers).
WINDOW_RE = re.compile(
    r"VMPROBE WINDOW seq=(?P<seq>\d+) cond=(?P<cond>\d+) ms=(?P<ms>\d+) "
    r"frames=(?P<frames>\d+) lat_n=(?P<lat_n>\d+) lat_drop=(?P<lat_drop>\d+) "
    r"qpeak_max=(?P<qpeak_max>\d+) heap_free_min=(?P<heap_free_min>\d+) "
    r"heap_largest_min=(?P<heap_largest_min>\d+) js_used_max=(?P<js_used_max>\d+) "
    r"js_limit=(?P<js_limit>\d+) stack_hw_min=(?P<stack_hw_min>\d+) "
    r"flush_us=(?P<flush_us>\d+)"
    # L1 added this field (vm-l1-tuning): optional, so a log captured before
    # it exists still parses instead of being counted as a corrupted line.
    r"(?: drainrun_drop=(?P<drainrun_drop>\d+))?"
)
SAMPLES_RE = re.compile(r"VMPROBE S (?P<seq>\d+) (?P<name>\w+) (?P<n>\d+) (?P<values>[\d,]+)")
STATIC_RE = re.compile(
    r"VMPROBE STATIC engine=(?P<engine>\S+) compiler=(?P<compiler>.+?) "
    r"opt=(?P<opt>\S+) sizeof_jsvalue=(?P<sizeof_jsvalue>\d+) "
    r"sizeof_stackframe=(?P<sizeof_stackframe>\d+) "
    r"sizeof_varref=(?P<sizeof_varref>\d+) fw=(?P<fw>\S+) cond=(?P<cond>\d+)"
)
# Any of these in the stream means the run did not survive, and a short run
# must never be reported as a clean one.
CRASH_RE = re.compile(r"Guru Meditation|abort\(\) was called|rst:0x|assert failed|"
                      r"StoreProhibited|LoadProhibited|Stack canary")
# "drainrun" is per drain CALL, not per frame -- see the note above
# VMPROBE_DRAINRUN_CAP in main/pocket/vmprobe.c for why the two differ.
METRICS = ("frame", "call", "drain", "jobs", "lat", "drainrun")


def p95(values):
    """Nearest-rank p95 over the whole sample set. Exact, not interpolated."""
    s = sorted(values)
    return s[min(len(s) - 1, math.ceil(0.95 * len(s)) - 1)]


def stats(values):
    if not values:
        return None
    return {"n": len(values), "min": min(values), "med": statistics.median(values),
            "p95": p95(values), "max": max(values)}


# ------------------------------------------------------------------- device

def hard_reset(ser):
    """Reboot the board over USB Serial/JTAG: RTS pulses the chip's reset while
    DTR stays low, so it starts the application rather than the ROM loader --
    esptool's HardReset for this transport. Every run starts from boot, which
    is the only way stack_hw (a high-water mark since boot) and the free heap
    mean anything per run.

    The dtr echo after each RTS write is not superstition: on Windows'
    usbser.sys a bare RTS change is not sent to the device, and without it
    only the very first reset of a session lands (measured -- resets 2 and 3
    silently did nothing, and the runs after them shared one boot).

    Nothing is flushed AFTER the pulse: HOME_READY is printed about 1.4 s into
    the boot, and a flush that late throws it away."""
    ser.reset_input_buffer()

    def rts(state):
        ser.rts = state
        ser.dtr = ser.dtr

    ser.dtr = False
    rts(True)
    time.sleep(0.25)
    rts(False)


def wait(ser, marker, limit=10):
    end = time.monotonic() + limit
    seen = []
    while time.monotonic() < end:
        line = ser.readline().decode(errors="replace").strip()
        if line:
            seen.append(line)
        if marker in line:
            return line
    raise RuntimeError(f"waiting for {marker}: {seen[-6:]}")


def reopen(ser):
    """Close and reopen the port. Windows' usbser.sys hands back a handle that
    can stop delivering anything after the device re-enumerates (a reset the
    host did not expect, a panic), and one capture stalled there for minutes
    with the board happily logging. Reopening is the only way back."""
    import serial
    port = ser.port
    try:
        ser.close()
    except Exception:
        pass
    time.sleep(0.5)
    return serial.Serial(port, 115200, timeout=0.2, write_timeout=3)


class Collector:
    """Turns the device's window blocks back into records. A block is one
    WINDOW line plus one S line per non-empty metric, all carrying the same
    seq; the S lines are emitted inside the same ESP_LOGI, so a seq that never
    gets its samples means the line was corrupted -- counted, not ignored."""

    def __init__(self):
        self.records = []
        self.current = None
        self.static = None
        self.notes = []      # VMCOND / console lines, kept verbatim
        self.bad = 0

    def close(self):
        if self.current:
            self.records.append(self.current)
            self.current = None

    def feed(self, line):
        if "VMPROBE STATIC" in line:
            m = STATIC_RE.search(line)
            if m:
                self.static = m.groupdict()
            return
        if "VMCOND" in line or "NETCHK" in line:
            self.notes.append(line)
            return
        m = WINDOW_RE.search(line)
        if m:
            self.close()
            d = {k: int(v) if v is not None else 0
                 for k, v in m.groupdict().items()}
            d["samples"] = {}
            self.current = d
            return
        m = SAMPLES_RE.search(line)
        if m and self.current is not None:
            if int(m.group("seq")) != self.current["seq"]:
                self.bad += 1
                return
            values = [int(v) for v in m.group("values").split(",")]
            if len(values) != int(m.group("n")):
                self.bad += 1
                return
            self.current["samples"][m.group("name")] = values


def run_one(ser, letter, cond_name, seconds, reset):
    """One (condition, workload) run: boot, set the condition, start the
    workload, listen for `seconds`, stop. Returns (collector, ended_early, ser)
    -- the port object because a stalled one is replaced rather than nursed."""
    mask = CONDITIONS[cond_name]
    try:
        if reset:
            hard_reset(ser)
            wait(ser, "HOME_READY", limit=15)
        else:
            ser.write(b"q")
            wait(ser, "HOME_READY")
    except Exception:
        ser = reopen(ser)
        hard_reset(ser)
        wait(ser, "HOME_READY", limit=15)
    ser.write(bytes([ord("P") + mask]))
    time.sleep(0.2)
    ser.write(letter.encode())
    c = Collector()
    early = False
    crash = None
    end = time.monotonic() + seconds
    # The board logs something (motion, the probe) at least every 2 s, so a
    # longer silence is the port having gone deaf, not a quiet device.
    quiet_until = time.monotonic() + 8
    while time.monotonic() < end:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            if time.monotonic() > quiet_until:
                raise RuntimeError("the port went silent for 8 s")
            continue
        quiet_until = time.monotonic() + 8
        if CRASH_RE.search(line):
            crash = line
            break
        if "HOME_READY" in line:
            early = True
            break
        c.feed(line)
    c.close()
    if crash:
        raise RuntimeError(f"device crashed during {WORKLOADS[letter]}/{cond_name}: {crash}")
    if not early:
        ser.write(b"q")
        try:
            wait(ser, "HOME_READY", limit=6)
        except RuntimeError as e:
            print(f"  (warning: {e})", flush=True)
    return c, early, ser


# ---------------------------------------------------------------- summaries

def load(path):
    return [json.loads(l) for l in open(path) if l.strip()]


def group(records, skip_first_window=True):
    """(workload, condition) -> pooled samples plus the resource extremes.
    Window 0 of a run is dropped by default: it starts mid-boot of the session
    and holds the first frames, which include the guest's own warm-up."""
    out = {}
    for r in records:
        if skip_first_window and r["seq"] == 0:
            continue
        key = (r["workload"], r["condition"])
        g = out.setdefault(key, {m: [] for m in METRICS})
        for m in METRICS:
            g[m].extend(r["samples"].get(m, []))
        g.setdefault("reps", set()).add(r["rep"])
        g.setdefault("windows", 0)
        g["windows"] += 1
        g["lat_drop"] = g.get("lat_drop", 0) + r["lat_drop"]
        g["drainrun_drop"] = g.get("drainrun_drop", 0) + r.get("drainrun_drop", 0)
        g["qpeak"] = max(g.get("qpeak", 0), r["qpeak_max"])
        for k, how in (("heap_free_min", min), ("heap_largest_min", min),
                       ("stack_hw_min", min), ("js_used_max", max)):
            if r[k]:
                g[k] = how(g.get(k, r[k]), r[k])
        g["flush_us"] = max(g.get("flush_us", 0), r["flush_us"])
    return out


def per_rep_medians(records, metric):
    """Median of `metric` within each repetition, so the report can state the
    spread between repetitions instead of claiming one number is stable."""
    by = {}
    for r in records:
        if r["seq"] == 0:
            continue
        by.setdefault((r["workload"], r["condition"], r["rep"]), []).extend(
            r["samples"].get(metric, []))
    out = {}
    for (w, c, rep), values in by.items():
        if values:
            out.setdefault((w, c), []).append(statistics.median(values))
    return out


def summarize(records, out=sys.stdout):
    g = group(records)
    spread = per_rep_medians(records, "frame")
    order = [w for w in WORKLOADS.values() if any(k[0] == w for k in g)]
    conds = [c for c in CONDITIONS if any(k[1] == c for k in g)]
    print("\n== matrix (exact median / p95 / max over every sample) ==", file=out)
    head = (f"{'workload':16s}{'cond':6s}{'win':>4s}"
            f"{'frame med':>10s}{'p95':>8s}{'max':>8s}"
            f"{'jobs med':>9s}{'p95':>5s}{'max':>5s}"
            f"{'lat med':>9s}{'p95':>7s}{'max':>7s}"
            f"{'heapfree':>9s}{'largest':>8s}{'js_used':>8s}{'stack':>7s}")
    print(head, file=out)
    for w in order:
        for c in conds:
            if (w, c) not in g:
                continue
            d = g[(w, c)]
            f_, j_, l_ = stats(d["frame"]), stats(d["jobs"]), stats(d["lat"])
            def ms(v):
                return f"{v/1000.0:.2f}"
            row = f"{w:16s}{c:6s}{d['windows']:>4d}"
            row += f"{ms(f_['med']):>10s}{ms(f_['p95']):>8s}{ms(f_['max']):>8s}"
            row += (f"{j_['med']:>9.0f}{j_['p95']:>5.0f}{j_['max']:>5.0f}"
                    if j_ else f"{'-':>9s}{'-':>5s}{'-':>5s}")
            row += (f"{ms(l_['med']):>9s}{ms(l_['p95']):>7s}{ms(l_['max']):>7s}"
                    if l_ else f"{'-':>9s}{'-':>7s}{'-':>7s}")
            row += (f"{d.get('heap_free_min',0):>9d}{d.get('heap_largest_min',0):>8d}"
                    f"{d.get('js_used_max',0):>8d}{d.get('stack_hw_min',0):>7d}")
            if d["lat_drop"]:
                row += f"  !lat_drop={d['lat_drop']}"
            if d.get("drainrun_drop"):
                row += f"  !drainrun_drop={d['drainrun_drop']}"
            print(row, file=out)
    print("\n== frame()/drain/UI split (exact median, ms) and rep spread ==", file=out)
    print(f"{'workload':16s}{'cond':6s}{'turn':>8s}{'frame()':>9s}{'drain':>8s}"
          f"{'ui':>8s}{'rep medians (turn)':>30s}", file=out)
    for w in order:
        for c in conds:
            if (w, c) not in g:
                continue
            d = g[(w, c)]
            turn, call, drain = (statistics.median(d[m]) for m in ("frame", "call", "drain"))
            ui = turn - call - drain
            reps = "/".join(f"{v/1000.0:.2f}" for v in sorted(spread.get((w, c), [])))
            print(f"{w:16s}{c:6s}{turn/1000:>8.2f}{call/1000:>9.2f}"
                  f"{drain/1000:>8.2f}{ui/1000:>8.2f}{reps:>30s}", file=out)


def markdown(records, out=sys.stdout):
    """The same numbers as summarize(), as the tables docs/vm-L0-report.md
    carries, so the report is pasted from the data rather than retyped."""
    g = group(records)
    spread = per_rep_medians(records, "frame")
    order = [w for w in WORKLOADS.values() if any(k[0] == w for k in g)]
    conds = [c for c in CONDITIONS if any(k[1] == c for k in g)]
    print("| ワークロード | 条件 | frame 中央値 / p95 / 最大 (ms) | jobs 中央値 / p95 / 最大 | "
          "lat 中央値 / p95 / 最大 (ms) | 空きヒープ最小 | 最大空きブロック最小 | "
          "js_used 最大 | stack_hw 最小 | 標本数 |", file=out)
    print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |", file=out)
    for w in order:
        for c in conds:
            if (w, c) not in g:
                continue
            d = g[(w, c)]
            f_, j_, l_ = stats(d["frame"]), stats(d["jobs"]), stats(d["lat"])
            fm = f"{f_['med']/1000:.2f} / {f_['p95']/1000:.2f} / {f_['max']/1000:.2f}"
            jm = f"{j_['med']:.0f} / {j_['p95']:.0f} / {j_['max']:.0f}" if j_ else "—"
            lm = (f"{l_['med']/1000:.2f} / {l_['p95']/1000:.2f} / {l_['max']/1000:.2f}"
                  if l_ else "—")
            print(f"| {w} | {c} | {fm} | {jm} | {lm} | {d.get('heap_free_min',0):,} | "
                  f"{d.get('heap_largest_min',0):,} | {d.get('js_used_max',0):,} | "
                  f"{d.get('stack_hw_min',0):,} | {f_['n']} |", file=out)
    print("", file=out)
    print("| ワークロード | 条件 | turn | frame() | drain | UI tick+draw | 反復ごとの中央値 |",
          file=out)
    print("| --- | --- | --- | --- | --- | --- | --- |", file=out)
    for w in order:
        for c in conds:
            if (w, c) not in g:
                continue
            d = g[(w, c)]
            turn, call, drain = (statistics.median(d[m]) for m in ("frame", "call", "drain"))
            reps = " / ".join(f"{v/1000:.2f}" for v in sorted(spread.get((w, c), [])))
            print(f"| {w} | {c} | {turn/1000:.2f} | {call/1000:.2f} | {drain/1000:.2f} | "
                  f"{(turn-call-drain)/1000:.2f} | {reps} |", file=out)


# --------------------------------------------------------------------- main

def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port")
    p.add_argument("--seconds", type=float, default=15.0,
                   help="capture duration per (workload, condition) run")
    p.add_argument("--workloads", default="ABCDEF",
                   help="letters to run, in order (default all six)")
    p.add_argument("--conditions", default="base,ui,audio,wifi,all")
    p.add_argument("--reps", type=int, default=1,
                   help="repetitions of the whole matrix in this invocation")
    p.add_argument("--rep-offset", type=int, default=0,
                   help="number the repetitions from here (for --append runs)")
    p.add_argument("--no-reset", action="store_true",
                   help="do not reboot the board between runs (stack_hw and the "
                        "free heap then carry the previous run's history)")
    p.add_argument("--out", default=".cache/vm/l0.jsonl")
    p.add_argument("--append", action="store_true")
    p.add_argument("--summarize", help="read this JSONL and print the matrix; no device")
    p.add_argument("--markdown", action="store_true",
                   help="with --summarize: print the report's tables instead")
    a = p.parse_args()

    if a.summarize:
        records = load(a.summarize)
        if a.markdown:
            markdown(records)
        else:
            summarize(records)
        return
    if not a.port:
        p.error("--port is required unless --summarize is given")

    import serial

    letters = list(a.workloads)
    conds = a.conditions.split(",")
    for letter in letters:
        if letter not in WORKLOADS:
            p.error(f"unknown workload {letter}")
    for c in conds:
        if c not in CONDITIONS:
            p.error(f"unknown condition {c}")

    out_path = pathlib.Path(a.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    run_id = time.strftime("%Y%m%dT%H%M%S")
    ser = serial.Serial(a.port, 115200, timeout=0.2, write_timeout=3)
    time.sleep(1.0)
    written = []
    failures = []
    try:
        with open(out_path, "a" if a.append else "w") as fh:
            for rep in range(a.rep_offset, a.rep_offset + a.reps):
                for cond in conds:
                    for letter in letters:
                        name = WORKLOADS[letter]
                        print(f"=== rep {rep} {cond:5s} {letter} {name} "
                              f"({a.seconds:.0f}s) ===", flush=True)
                        try:
                            c, early, ser = run_one(ser, letter, cond,
                                                    a.seconds, not a.no_reset)
                        except RuntimeError as e:
                            # One dead run must not take the other 89 with it:
                            # the failure is printed and the matrix goes on to
                            # the next cell, which starts from a fresh boot.
                            print(f"  FAILED: {e}", flush=True)
                            failures.append((rep, cond, letter, str(e)))
                            continue
                        for note in c.notes:
                            print("  " + note, flush=True)
                        if early:
                            print("  WARNING: the workload ended before the "
                                  "capture did -- see the log above", flush=True)
                        if c.bad:
                            print(f"  WARNING: {c.bad} malformed sample line(s)",
                                  flush=True)
                        if not c.records:
                            print("  WARNING: no VMPROBE WINDOW block seen -- is "
                                  "this a CONFIG_POCKET_VM_PROBE build?", flush=True)
                        for r in c.records:
                            r.update(run_id=run_id, rep=rep, condition=cond,
                                     workload=name, letter=letter,
                                     ended_early=early, static=c.static,
                                     notes=c.notes if r["seq"] == 0 else [])
                            fh.write(json.dumps(r) + "\n")
                            written.append(r)
                        fh.flush()
                        f = [v for r in c.records if r["seq"] for v in r["samples"].get("frame", [])]
                        if f:
                            s = stats(f)
                            print(f"  frame n={s['n']} med={s['med']/1000:.2f} "
                                  f"p95={s['p95']/1000:.2f} max={s['max']/1000:.2f} ms",
                                  flush=True)
        print(f"JSONL: {out_path}", flush=True)
        for f in failures:
            print(f"FAILED rep={f[0]} cond={f[1]} workload={f[2]}: {f[3]}", flush=True)
        if written:
            summarize(written)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
