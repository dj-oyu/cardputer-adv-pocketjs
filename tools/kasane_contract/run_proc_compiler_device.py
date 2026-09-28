"""Trigger the optional Kasane procedural/compiler diagnostic over USB serial."""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

import serial


def d4_status(result: bytes) -> tuple[bool, bool, bool, bool, bool, bool]:
    def marker(name: bytes, require_budget: bool = False) -> bool:
        pattern = rb"KSN_PROC: " + re.escape(name) + rb" PASS [^\r\n]*"
        if require_budget:
            pattern += rb"budget_30fps=1(?:[ \r\n]|$)"
        return re.search(pattern, result) is not None

    return (marker(b"PIXEL_WORK_TIME", True),
            marker(b"PIXEL_WORK_SPAN", True),
            marker(b"PIXEL_SCOPE_64", True),
            marker(b"PIXEL_SCOPE_112", True),
            marker(b"PIXEL_WORK_MEM"),
            marker(b"PIXEL_WORK_PSRAM"))


def d4_scene_status(result: bytes) -> tuple[bool, bool, bool, bool, bool, bool]:
    lines = [line.strip() for line in result.splitlines() if b"KSN_PROC:" in line]

    def one(prefix: bytes) -> bool:
        return sum(prefix in line for line in lines) == 1

    scene_lines = [line for line in lines if b"KSN_PROC: PIXEL_SPAN_SCENE " in line]
    repair_lines = [line for line in lines if b"KSN_PROC: PIXEL_SPAN_REPAIR " in line]
    compare_lines = [line for line in lines if b"KSN_PROC: PIXEL_SPAN_COMPARE " in line]
    all_lines = [line for line in lines if b"KSN_PROC: ALL " in line]
    all_ok = (len(all_lines) == 1 and lines[-1] == all_lines[0] and
              re.search(rb"KSN_PROC: ALL (?:PASS|FAIL) [^\r\n]*\bpixel_scene=1(?:\s|$)",
                        all_lines[0]) is not None)
    return (len(scene_lines) == 2 and one(b"KSN_PROC: PIXEL_SPAN_SCENE arm=original frames=120"),
            len(scene_lines) == 2 and one(b"KSN_PROC: PIXEL_SPAN_SCENE arm=span frames=120"),
            len(repair_lines) == 2 and one(b"KSN_PROC: PIXEL_SPAN_REPAIR PASS arm=original "),
            len(repair_lines) == 2 and one(b"KSN_PROC: PIXEL_SPAN_REPAIR PASS arm=span "),
            len(compare_lines) == 1 and one(b"KSN_PROC: PIXEL_SPAN_COMPARE PASS frames=120 "),
            all_ok)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--binary", type=Path,
                        help="Verified flashed diagnostic image for profile provenance")
    parser.add_argument("--d4-scene", action="store_true",
                        help="Judge the 120-frame D4 compositor comparison independently of known full-screen/memory failures")
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)

    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    log = bytearray()
    with port:
        deadline = time.monotonic() + 12
        while b"HOME_READY" not in log and time.monotonic() < deadline:
            log.extend(port.read(32768))
        if b"HOME_READY" not in log:
            port.write(b"q")
            deadline = time.monotonic() + 12
            while b"HOME_READY" not in log and time.monotonic() < deadline:
                log.extend(port.read(32768))
        if b"HOME_READY" not in log:
            args.out.write_bytes(log)
            print(f"HOME_READY missing; saved {args.out}")
            return 2

        time.sleep(1)
        start = len(log)
        for _ in range(3):
            port.write(b"|")
            trigger_deadline = time.monotonic() + 3
            while b"KSN_PROC: START" not in log[start:] and time.monotonic() < trigger_deadline:
                log.extend(port.read(32768))
            if b"KSN_PROC: START" in log[start:]:
                break
        if b"KSN_PROC: START" not in log[start:]:
            args.out.write_bytes(log)
            print(f"KSN_PROC START missing; saved {args.out}")
            return 2
        deadline = time.monotonic() + (90 if args.d4_scene else 45)
        while time.monotonic() < deadline:
            log.extend(port.read(32768))
            if re.search(rb"KSN_PROC: ALL (?:PASS|FAIL)[^\r\n]*[\r\n]", log[start:]):
                quiet_deadline = time.monotonic() + 0.3
                while time.monotonic() < quiet_deadline:
                    log.extend(port.read(32768))
                break
        if args.binary:
            args.out.write_bytes(log)
            result_text = bytes(log[start:]).decode("utf-8", "replace")
            device = re.findall(
                r"GRID_PROFILE_DEVICE elf=([0-9a-fA-F]{64}) "
                r"cpu=(\d+) idf=(\S+) chip=(\S+) target=(esp32s3) opt=(\S+)",
                result_text)
            if len(device) != 1:
                raise RuntimeError("profile device identity missing or repeated")
            elf, cpu, idf, chip, target, opt = device[0]
            image_info = subprocess.run(
                [sys.executable, "-m", "esptool", "image-info", str(args.binary)],
                check=True, capture_output=True, text=True).stdout
            image_elf = re.search(r"ELF file SHA256:\s+([0-9a-fA-F]{64})",
                                  image_info)
            image_idf = re.search(r"ESP-IDF:\s+(\S+)", image_info)
            if (not image_elf or not image_idf or
                image_elf.group(1).lower() != elf.lower() or
                image_idf.group(1) != idf):
                raise RuntimeError("flashed image and boot identity differ")
            digest = hashlib.sha256(args.binary.read_bytes()).hexdigest()
            identity = (f"\nGRID_PROFILE_RUN schema=1 run={uuid.uuid4().hex} "
                        f"binary={digest} elf={elf.lower()} "
                        f"cpu={cpu} idf={idf} "
                        f"chip={chip} target={target} opt={opt}\n")
            log.extend(identity.encode("ascii"))
        args.out.write_bytes(log)
    result = bytes(log[start:])
    for line in result.splitlines():
        if (b"KSN_PROC:" in line or b"KSN_COMPILER:" in line or
                b"KSN_GRID_SCAN:" in line):
            print(line.decode("utf-8", "replace"))
    (pixel_time_ok, pixel_span_ok, scope_64_ok, scope_112_ok,
     pixel_memory_ok, pixel_psram_ok) = d4_status(result)
    pixel_work_ok = (b"KSN_PROC: PIXEL_WORK PASS time=1 span=1 scopes=1 memory=1"
                     in result)
    (scene_original_ok, scene_span_ok, scene_repair_original_ok,
     scene_repair_span_ok, scene_compare_ok, scene_all_ok) = d4_scene_status(result)
    print("D4_PIXEL_WORK "
          f"time={'PASS' if pixel_time_ok else 'FAIL'} "
          f"span={'PASS' if pixel_span_ok else 'FAIL'} "
          f"scope64={'PASS' if scope_64_ok else 'FAIL'} "
          f"scope112={'PASS' if scope_112_ok else 'FAIL'} "
          f"memory={'PASS' if pixel_memory_ok else 'FAIL'} "
          f"psram={'PASS' if pixel_psram_ok else 'FAIL'} "
          f"combined={'PASS' if pixel_work_ok else 'FAIL'}")
    print("D4_SCENE "
          f"original={'PASS' if scene_original_ok else 'FAIL'} "
          f"span={'PASS' if scene_span_ok else 'FAIL'} "
          f"retry_repair_original={'PASS' if scene_repair_original_ok else 'FAIL'} "
          f"retry_repair_span={'PASS' if scene_repair_span_ok else 'FAIL'} "
          f"compare={'PASS' if scene_compare_ok else 'FAIL'} "
          f"all={'PASS' if scene_all_ok else 'FAIL'}")
    ok = (b"KSN_PROC: DISPLAY PASS" in result and
          b"KSN_COMPILER: GRID_JS PASS" in result and
          b"KSN_COMPILER: GRID PASS" in result and
          b"KSN_GRID_SCAN: GRID_SCAN PASS" in result and
          b"KSN_COMPILER: PASS" in result and
          pixel_time_ok and pixel_span_ok and scope_64_ok and scope_112_ok and
          pixel_memory_ok and pixel_work_ok and
          b"KSN_PROC: ALL PASS" in result)
    if args.d4_scene:
        ok = (scene_original_ok and scene_span_ok and
              scene_repair_original_ok and scene_repair_span_ok and
              scene_compare_ok and scene_all_ok)
    print(f"{'PASS' if ok else 'FAIL'}; log={args.out}")
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
