"""Build an ESP32-S3 grid kernel selection table from same-binary probe logs.

Only a candidate that beats gather in every run and by at least five percent
at the median becomes the selected route. Profile rows never grant legality;
the bound execution checks that again before use.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
from pathlib import Path
import re
from statistics import median


BACKEND_REV = 1
MIN_GAIN = 0.05
NAMES = {"gather": "KSN_GRID_PIE_LOAD_GATHER",
         "affine": "KSN_GRID_PIE_LOAD_AFFINE",
         "fused": "KSN_GRID_PIE_LOAD_FUSED"}
BITS = {"gather": 1 << 1, "affine": 1 << 2, "fused": 1 << 3}
LINE = re.compile(
    r"KSN_COMPILER: (GRID_TIME|GRID_DYNAMIC_TIME|GRID_BROADCAST_TIME) "
    r"rev=(\d+) key=([0-9a-fA-F]{16}) mask=(\d+) "
    r"width=(\d+) height=(\d+) taps_x=(\d+) taps_y=(\d+) "
    r"calls_per_backend=(\d+) scalar_us=(\d+) gather_us=(\d+) "
    r"affine_us=(\d+) fused_us=(-?\d+)"
)
ALIGNED = re.compile(
    r"KSN_COMPILER: GRID_ALIGNED_TIME outputs=(\d+) "
    r"calls_per_backend=(\d+) source_align=(\d+) dest_align=(\d+) "
    r"gather_us=(\d+) fused_us=(\d+) key=([0-9a-fA-F]{16})"
)
RUN = re.compile(
    r"GRID_PROFILE_RUN schema=(\d+) run=([0-9a-f]{32}) "
    r"binary=([0-9a-f]{64}) elf=([0-9a-fA-F]+) "
    r"cpu=(\d+) idf=(\S+) chip=(\S+) target=(\S+) opt=(\S+)"
)


def parse_logs(paths: list[Path], min_runs: int = 3):
    records = defaultdict(list)
    identities = set()
    run_ids = set()
    paths_seen = set()
    contents_seen = set()
    key_sets = []
    for path in paths:
        resolved = path.resolve()
        if resolved in paths_seen:
            raise ValueError(f"duplicate log path: {path}")
        paths_seen.add(resolved)
        data = path.read_text(encoding="utf-8", errors="replace")
        content_digest = hashlib.sha256(data.encode("utf-8")).digest()
        if content_digest in contents_seen:
            raise ValueError(f"duplicate log contents: {path}")
        contents_seen.add(content_digest)
        if "GRID PASS" not in data or "KSN_COMPILER: PASS" not in data:
            raise ValueError(f"unverified diagnostic log: {path}")
        run = RUN.findall(data)
        if (len(run) != 1 or run[0][0] != "1" or
            run[0][-2:] != ("esp32s3", "size")):
            raise ValueError(f"missing/incompatible profile run identity: {path}")
        _, run_id, *identity = run[0]
        if run_id in run_ids:
            raise ValueError(f"duplicate profile run ID: {path}")
        run_ids.add(run_id)
        identities.add(tuple(identity))
        seen = set()
        for match in LINE.finditer(data):
            label, rev, key, mask, width, height, tx, ty, calls, scalar, gather, affine, fused = match.groups()
            if int(rev) != BACKEND_REV or int(calls) != 1024:
                raise ValueError(f"incompatible probe revision/call count: {path}")
            key = int(key, 16)
            mask = int(mask)
            times = {"scalar": int(scalar), "gather": int(gather),
                     "affine": int(affine), "fused": int(fused)}
            if key in seen or key == 0 or any(times[name] <= 0 for name in ("scalar", "gather", "affine")):
                raise ValueError(f"duplicate/invalid profile key: {path}")
            seen.add(key)
            if (mask & (BITS["gather"] | BITS["affine"])) != (BITS["gather"] | BITS["affine"]):
                raise ValueError(f"missing baseline candidates: {path}")
            if bool(mask & BITS["fused"]) != (times["fused"] > 0):
                raise ValueError(f"fused legality/time mismatch: {path}")
            meta = (label, int(width), int(height), int(tx), int(ty), mask)
            records[key].append((meta, times))
        for match in ALIGNED.finditer(data):
            outputs, calls, source_align, dest_align, gather, fused, key = match.groups()
            key = int(key, 16)
            # This benchmark rebinds the fixed 2x2 probe to aligned input.
            # Recover its dimensions from the same run, not a guessed key.
            reference = [records[k][-1][0] for k in seen
                         if records[k][-1][0][0] == "GRID_TIME" and
                         records[k][-1][0][1] * records[k][-1][0][2] == int(outputs) and
                         records[k][-1][0][3:5] == (2, 2) and
                         records[k][-1][0][-1] & BITS["fused"]]
            if (len(reference) != 1 or int(calls) != 1024 or
                int(source_align) != 0 or int(dest_align) != 0 or
                key in seen or key == 0 or int(gather) <= 0 or int(fused) <= 0):
                raise ValueError(f"invalid aligned grid profile: {path}")
            base = reference[0]
            meta = ("GRID_ALIGNED_TIME", *base[1:5],
                    BITS["gather"] | BITS["fused"])
            records[key].append((meta, {"gather": int(gather),
                                        "fused": int(fused)}))
            seen.add(key)
        if not seen:
            raise ValueError(f"no grid profile measurements: {path}")
        key_sets.append(seen)
    if len(identities) != 1:
        raise ValueError("profile runs use different binary or device settings")
    if any(keys != key_sets[0] for keys in key_sets[1:]):
        raise ValueError("profile runs measured different case sets")
    provenance = next(iter(identities))
    result = []
    for key, samples in sorted(records.items()):
        if len(samples) < min_runs:
            raise ValueError(f"profile {key:016x} has only {len(samples)} runs")
        meta = samples[0][0]
        if any(sample[0] != meta for sample in samples):
            raise ValueError(f"profile {key:016x} changed shape or candidates")
        eligible = [name for name in NAMES if meta[-1] & BITS[name]]
        times = {name: [sample[1][name] for sample in samples] for name in eligible}
        stable = [name for name in eligible if name != "gather" and
                  1 - median(times[name]) / median(times["gather"]) >= MIN_GAIN and
                  all(candidate < baseline for candidate, baseline in
                      zip(times[name], times["gather"]))]
        winner = min(stable, key=lambda name: median(times[name])) if stable else "gather"
        gain = 1 - median(times[winner]) / median(times["gather"])
        result.append((key, meta, times, winner, gain, provenance))
    return result


def render_profile(rows) -> str:
    lines = [
        "#ifndef KSN_PROC_GRID_PROFILE_H",
        "#define KSN_PROC_GRID_PROFILE_H",
        "",
        "/* Generated from repeated same-binary ESP32-S3 measurements.",
        " * Entries rank only candidates already proved legal at bind. */",
        f"#define KSN_GRID_PROFILE_BACKEND_REV {BACKEND_REV}u",
        "static const ksn_grid_profile_entry ksn_grid_profile[] = {",
        "    {0, KSN_GRID_PIE_LOAD_AUTO},",
    ]
    if rows:
        binary, elf, cpu, idf, chip, target, opt = rows[0][5]
        version = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)(?:[-+].*)?", idf)
        if not version:
            raise ValueError(f"unsupported IDF version: {idf}")
        lines.insert(5, f"/* binary SHA256 {binary}; ELF {elf}; CPU {cpu} Hz; "
                        f"IDF {idf}; chip {chip}; target {target}; opt {opt} */")
        lines.insert(6, f"#define KSN_GRID_PROFILE_CPU_MHZ {int(cpu) // 1000000}u")
        lines.insert(7, f"#define KSN_GRID_PROFILE_IDF_MAJOR {version.group(1)}u")
        lines.insert(8, f"#define KSN_GRID_PROFILE_IDF_MINOR {version.group(2)}u")
        lines.insert(9, f"#define KSN_GRID_PROFILE_IDF_PATCH {version.group(3)}u")
        lines.insert(10, "#define KSN_GRID_PROFILE_OPT_SIZE 1u")
    for key, meta, times, winner, gain, _ in rows:
        label, width, height, tx, ty, _ = meta
        medians = ", ".join(f"{name}={median(values):.0f}us" for name, values in times.items())
        lines.append(f"    /* {label} {width}x{height} taps={tx}x{ty}: {medians}; gain={gain:.1%} */")
        lines.append(f"    {{UINT64_C(0x{key:016x}), {NAMES[winner]}}},")
    # The independently measured two-term routes share the table but have a
    # separate input record and generator. Keep them when refreshing one-term
    # probe measurements.
    lines += ['#include "ksn_proc_grid_dual_profile.inc"',
              "};", "", "#endif", ""]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--out", type=Path,
                        default=Path("main/ui/kasane/ksn_proc_grid_profile.h"))
    parser.add_argument("--min-runs", type=int, default=3)
    args = parser.parse_args()
    if args.min_runs < 2:
        parser.error("at least two independent probe runs are required")
    rows = parse_logs(args.logs, args.min_runs)
    args.out.write_text(render_profile(rows), encoding="utf-8")
    for key, meta, times, winner, gain, _ in rows:
        print(f"{meta[0]} {meta[1]}x{meta[2]} key={key:016x} "
              f"selected={winner} gain={gain:.1%} runs={len(times['gather'])}")


if __name__ == "__main__":
    main()
