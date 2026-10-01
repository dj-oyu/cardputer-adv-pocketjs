"""Launch GRID LAB twice and verify resize plus symbolic fold modes via PIE."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time
import uuid

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--binary", type=Path,
                        help="normal app image for reproducible route profile records")
    parser.add_argument("--runs", type=int, default=2)
    args = parser.parse_args()
    if not 1 <= args.runs <= 8:
        parser.error("--runs must be 1..8")
    binary = args.binary.read_bytes() if args.binary else None
    args.out.parent.mkdir(parents=True, exist_ok=True)
    log = bytearray()
    profile_runs = []
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        with port:
            time.sleep(0.5)
            port.reset_input_buffer()

            def collect(marker, seconds=15):
                start = len(log)
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    log.extend(port.read(32768))
                    recent = bytes(log[start:])
                    if b"Guru Meditation" in recent or b"START_FAILED" in recent:
                        raise RuntimeError(recent[-1500:])
                    if marker in recent:
                        return recent
                raise RuntimeError(f"missing {marker!r}: {bytes(log[-1500:])!r}")

            def key(value, marker, seconds=15):
                port.write(value)
                return collect(marker, seconds)

            for run in range(1, args.runs + 1):
                key(b"q", b"HOME_READY")
                key(b"a", b"CATEGORY 0")
                for _ in range(12):
                    key(b"u", b"APP ")
                for index in range(1, 10):
                    key(b"d", f"APP {index}".encode())
                started = key(b"e", b"GRID_APP MODE 0 28x14 backend=PIE strategy=BILINEAR reason=EXPERIMENT", 20)
                if b"APP_ID local.gridlab" not in started:
                    raise RuntimeError("GRID LAB launched under wrong identity")
                registrations = re.findall(
                    rb"GRID_APP REGISTER ([345]) ir=(\d+) plan=(\d+) "
                    rb"analysis=(\d+) parse_us=(\d+) prepare_us=(\d+) "
                    rb"total_us=(\d+) heap_before=(\d+) heap_plan=(\d+) "
                    rb"heap_after=(\d+) largest_before=(\d+) largest_after=(\d+)",
                    started)
                if {int(fields[0]) for fields in registrations} != {3, 4, 5}:
                    raise RuntimeError("GRID LAB registration diagnostics missing")
                for fields in registrations:
                    mode, ir, plan, analysis, parse_us, prepare_us, total_us, before, heap_plan, after, largest_before, largest_after = map(int, fields)
                    if (not ir or plan < analysis or
                            total_us < max(parse_us, prepare_us) or
                            not before or not (before > heap_plan >= after) or
                            largest_after > largest_before):
                        raise RuntimeError(f"invalid registration sample: {fields!r}")
                    print(f"GRID_LAB_REGISTER_{run}_{mode} ir={ir} "
                          f"plan={plan} analysis={analysis} "
                          f"parse={parse_us}us prepare={prepare_us}us "
                          f"total={total_us}us heap={before}->{heap_plan}->{after} "
                          f"largest={largest_before}->{largest_after}")
                start = len(log)
                deadline = time.monotonic() + 35
                while bytes(log[start:]).count(b"KASANE_PAINT") < 23:
                    if time.monotonic() > deadline:
                        raise RuntimeError("GRID LAB did not paint 690 frames")
                    log.extend(port.read(32768))
                    recent = bytes(log[start:])
                    if any(marker in recent for marker in
                           (b"APP_STOPPED", b"START_FAILED", b"Guru Meditation")):
                        raise RuntimeError(recent[-1500:])
                if b"GRID_APP FRAME" not in bytes(log[start:]):
                    raise RuntimeError("GRID LAB did not keep selecting backend")
                recent = bytes(log[start:])
                for mode in (b"GRID_APP MODE 1 37x23 backend=PIE",
                             b"GRID_APP MODE 2 30x15 backend=PIE",
                             b"GRID_APP MODE 3 48x28 backend=PIE strategy=GATHER reason=PROFILE",
                             b"GRID_APP MODE 4 48x20 backend=PIE strategy=AFFINE reason=PROFILE",
                             b"GRID_APP MODE 5 40x20 backend=PIE strategy=AFFINE reason=PROFILE"):
                    if mode not in recent:
                        raise RuntimeError(f"GRID LAB did not reach {mode!r}")
                measured = re.search(
                    rb"GRID_APP MEASURE 3 repeats=8 scalar_us=(\d+) "
                    rb"pie_us=(\d+) equal=1", recent)
                if not measured:
                    raise RuntimeError("GRID LAB native comparison missing")
                scalar_us, pie_us = map(int, measured.groups())
                if scalar_us <= 0 or pie_us <= 0:
                    raise RuntimeError("GRID LAB native timing was zero")
                routes = re.findall(
                    rb"GRID_APP ROUTES ([345]) gather_us=(\d+) "
                    rb"affine_us=(\d+) equal=1", recent)
                if {int(mode) for mode, _, _ in routes} != {3, 4, 5}:
                    raise RuntimeError("GRID LAB route comparison missing")
                keys = re.findall(
                    rb"GRID_APP MODE ([345]) (\d+)x(\d+) backend=PIE "
                    rb"strategy=[A-Z]+ reason=[A-Z]+ key=([0-9a-f]{16})",
                    recent)
                if {int(mode) for mode, _, _, _ in keys} != {3, 4, 5}:
                    raise RuntimeError("GRID LAB profile keys missing")
                diagnostics = re.findall(
                    rb"GRID_APP MODE ([345]) [^\r\n]+ kernel=MAC "
                    rb"scalar_reason=NONE candidate_mask=(\d+)", recent)
                if ({int(mode) for mode, _ in diagnostics} != {3, 4, 5} or
                        any(int(mask) == 0 for _, mask in diagnostics)):
                    raise RuntimeError("GRID LAB route diagnostics missing")
                stopped = key(b"q", b"HOME_READY")
                if b"APP_STOPPED" not in stopped:
                    raise RuntimeError("GRID LAB did not stop")
                by_mode = {int(mode): (int(gather), int(affine))
                           for mode, gather, affine in routes}
                profile_runs.append({
                    "run_id": uuid.uuid4().hex,
                    "cases": [{"mode": int(mode), "width": int(width),
                               "height": int(height), "key": key.decode(),
                               "gather_us": by_mode[int(mode)][0],
                               "affine_us": by_mode[int(mode)][1]}
                              for mode, width, height, key in keys]
                })
                print(f"GRID_LAB_RUN_{run} PASS scalar={scalar_us}us "
                      f"pie={pie_us}us repeats=8")
                for mode, gather_us, affine_us in routes:
                    print(f"GRID_LAB_ROUTE_{run}_{mode.decode()} "
                          f"gather={int(gather_us)}us affine={int(affine_us)}us")
    finally:
        args.out.write_bytes(log)
    if binary is not None:
        sidecar = args.out.with_suffix(args.out.suffix + ".profile.json")
        sidecar.write_text(json.dumps({
            "schema": 1, "device": args.port.upper(),
            "binary_sha256": hashlib.sha256(binary).hexdigest(),
            "binary_bytes": len(binary), "runs": profile_runs
        }, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
