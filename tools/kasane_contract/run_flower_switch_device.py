"""Cycle away from and back to FLOWER, restoring the saved background."""
import argparse
from pathlib import Path
import re
import sys
import time

import serial
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from home_modes import BACKGROUNDS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    original = None
    current = None
    port = serial.Serial(port=None, baudrate=115200, timeout=0.12)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        with port:
            time.sleep(0.4)
            port.reset_input_buffer()

            def read_until(marker, timeout=12):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    line = port.readline().decode(errors="replace").strip()
                    if not line:
                        continue
                    lines.append(line)
                    if "Guru Meditation" in line or "panic" in line.lower():
                        raise RuntimeError(line)
                    if marker in line:
                        return line
                raise RuntimeError(f"missing {marker}: {lines[-12:]}")

            def key(value, marker):
                port.write(value.encode())
                return read_until(marker)

            def open_background():
                key("q", "HOME_READY")
                time.sleep(0.15)
                key("b", "CATEGORY 1")
                for _ in range(8):
                    key("u", "SELECT")
                line = key("e", "OPEN 0 choice=")
                match = re.search(r"choice=(\d+)", line)
                if not match:
                    raise RuntimeError(line)
                return int(match.group(1))

            def select(target):
                nonlocal current
                selected = open_background()
                if current is not None and selected != current:
                    raise RuntimeError(f"choice drifted: {selected} != {current}")
                step = "d" if target > selected else "u"
                for choice in range(selected + (1 if target > selected else -1),
                                    target + (1 if target > selected else -1),
                                    1 if target > selected else -1):
                    key(step, f"CHOICE {choice}")
                key("e", f"background={target}")
                current = target
                key("q", "HOME_READY")
                if target == 3:
                    read_until("PERF mode=3", 6)

            try:
                original = open_background()
                current = original
                key("q", "HOME_READY")
                sequence = [0, 3, 1, 3, 0, 3] if original == 3 else [3, 0, 3]
                for target in sequence:
                    select(target)
                    print(f"BACKGROUND {target} {BACKGROUNDS[target]} PASS", flush=True)
            finally:
                if original is not None:
                    selected = open_background()
                    current = selected
                    key("q", "HOME_READY")
                    if current != original:
                        select(original)
            print(f"FLOWER_SWITCH PASS restored={original}", flush=True)
    finally:
        args.out.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
