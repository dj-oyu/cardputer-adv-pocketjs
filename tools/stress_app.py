"""Drive apps/stress (menu row 7) over USB and summarise it (apps/stress/README.md).

Starts STRESS TEST from the menu, runs each load level for --seconds (Enter
steps L1 -> L2 -> L3), stops it, and prints one JSON document plus a verdict:
    python tools/stress_app.py --port COM3 [--seconds 20]
PASS needs: the native Uint8Array check ok, no STRESS_FAIL (a caught OOM is
STRESS_OOM, not a failure), no RUNAWAY, no crash, L3 reached at least one
caught OOM and kept drawing after it, and the app stopped cleanly
(APP_STOPPED, HOME_READY).
"""
import argparse, json, re, statistics, time
import serial

ROW = 7
PAINT = re.compile(r"KASANE_PAINT turn_ms=([\d.]+) render_ms=([\d.]+) send_ms=([\d.]+)")
STAT = re.compile(r"\((\d+)\) js: STRESS f=(\d+) lvl=(\d) pool=(\d+) peak=(\d+) oom=(\d+) cyc=(\d+) nat=(\d+) "
                  r"natOk=(\S+) err=(\d+) cmds=(\d+) native=(\d+)")
FATAL = ("Guru Meditation", "assert failed", "CORRUPT HEAP", "abort()")


def read_for(port, seconds, until=None, log=None):
    lines, end = [], time.monotonic() + seconds
    while time.monotonic() < end:
        line = port.readline().decode(errors="replace").strip()
        if not line:
            continue
        lines.append(line)
        if log is not None:
            log.append(line)
        if any(s in line for s in FATAL):
            raise RuntimeError(line)
        if until and until in line:
            break
    return lines


def level(lines, n):
    paints = [tuple(map(float, m.groups())) for m in map(PAINT.search, lines) if m]
    stats = [m.groups() for m in map(STAT.search, lines) if m]
    out = {"level": n, "paint_samples": len(paints)}
    if paints:
        out["turn_ms_med"] = statistics.median(p[0] for p in paints)
        out["render_ms_med"] = statistics.median(p[1] for p in paints)
        out["send_ms_med"] = statistics.median(p[2] for p in paints)
    if len(stats) >= 2:   # between the first and last 60-frame report, by the device's clock
        frames = int(stats[-1][1]) - int(stats[0][1])
        ms = int(stats[-1][0]) - int(stats[0][0])
        out["fps"] = round(frames * 1000 / ms, 1) if ms else None
    if stats:
        s = stats[-1][1:]
        out.update(peak=int(s[3]), oom=int(s[4]), nat=int(s[6]), natOk=s[7], err=int(s[8]),
                   cmds=int(s[9]), kasane_native=int(s[10]))
    out["oom_lines"] = sum("STRESS_OOM" in l for l in lines)
    out["engine_oom_lines"] = sum(" OOM n=" in l for l in lines)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--log", help="also write every USB line here")
    a = ap.parse_args()
    log = []
    try:
        run(a, log)
    finally:
        if a.log:
            with open(a.log, "w", encoding="utf-8") as f:
                f.write("\n".join(log) + "\n")


def run(a, log):
    with serial.Serial(a.port, 115200, timeout=0.2) as port:
        time.sleep(1.5); port.reset_input_buffer()
        port.write(b"q"); read_for(port, 10, "HOME_READY", log)
        port.write(b"a"); read_for(port, 5, "CATEGORY 0", log)
        for _ in range(10):
            port.write(b"u"); time.sleep(0.05)
        for _ in range(ROW):
            port.write(b"d"); time.sleep(0.05)
        read_for(port, 5, f"APP {ROW}", log)
        port.write(b"e")
        start = read_for(port, 15, "STRESS_READY", log)
        levels = []
        for n in (1, 2, 3):
            if n > 1:
                port.write(b"e")
            t = time.monotonic()
            lines = read_for(port, a.seconds, None, log)
            lv = level(lines, n)
            lv["seconds"] = round(time.monotonic() - t, 1)
            levels.append(lv)
        port.write(b"q")
        stop = read_for(port, 20, "APP_STOPPED", log)
        home = read_for(port, 10, "HOME_READY", log)
    text = "\n".join(log)
    native = re.search(r"STRESS_NATIVE (\S+) len=(\d+)", text)
    mem = re.search(r"APP_ID local\.stress[\s\S]*?MEM free=(\d+) largest=(\d+) js=(\d+) frames=0", text)
    l3 = levels[2]
    result = {
        "native": native.group(1) if native else None,
        "js_after_source": int(mem.group(3)) if mem else None,
        "levels": levels,
        "fails": [l for l in log if "STRESS_FAIL" in l][:5],
        "runaway": [l for l in log if "RUNAWAY" in l][:5],
        "stopped": any("APP_STOPPED" in l for l in stop),
        "home": any("HOME_READY" in l for l in home),
    }
    ok = (result["native"] == "ok" and not result["fails"] and not result["runaway"]
          and l3.get("oom_lines", 0) > 0 and l3.get("paint_samples", 0) > 0
          and result["stopped"] and result["home"])
    print(json.dumps(result, indent=1))
    print("STRESS_APP_" + ("PASS" if ok else "FAIL"))
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
