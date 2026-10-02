"""Bounded ordinary HELLO smoke, or offline GRID LAB equality-log checks.

The key protocol comes from tools/smoke_device.py and
kasane_contract/measure_grid_pipeline_device.py. No diagnostic keys are sent.
"""
import argparse
import json
from pathlib import Path
import re
import time

FAULT = re.compile(rb"Guru Meditation|panic'ed|START_FAILED|PRESENTER_STEP_FAILED|APP_ERROR|abort\(\)")


def check_grid_log(raw):
    """The existing runner validates four pipeline arms; also require equality."""
    raw = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', raw)
    if FAULT.search(raw):
        raise RuntimeError("Device fault marker in GRID log")
    for kind in (b"MEASURE", b"ROUTES"):
        for mode in (3, 4, 5):
            matches = re.findall(rb"GRID_APP " + kind + b" " + str(mode).encode()
                                 + rb" ([^\r\n]*)[\r\n]", raw)
            if not matches or any(not re.search(rb"(?:^| )equal=1(?: |$)", line)
                                  for line in matches):
                raise RuntimeError(f"Missing or unequal GRID {kind.decode()} mode {mode}")
    return {"grid_measure_and_route_equality": "passed", "physical_pixels": "not assessed"}


class Session:
    def __init__(self, port, logfile, clock=time.monotonic):
        self.port, self.logfile, self.clock = port, logfile, clock

    def command(self, key, marker, timeout=12):
        if key not in (b'q', b'a', b'u', b'e'):
            raise ValueError("Only ordinary HELLO navigation keys are allowed")
        self.port.write(key)
        raw = bytearray()
        deadline = self.clock() + timeout
        while self.clock() < deadline:
            incoming = self.port.read(32768)
            if incoming:
                self.logfile.write(incoming)
                self.logfile.flush()
                raw.extend(incoming)
                if FAULT.search(raw):
                    raise RuntimeError("Device fault; inspect serial.log")
                clean = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', raw)
                if re.search(marker, clean):
                    return clean
        raise RuntimeError(f"Timeout waiting for {marker!r}; inspect serial.log")


def run_smoke(port, logfile, cycles):
    session = Session(port, logfile)
    session.command(b'q', rb'HOME_READY\r?\n')
    session.command(b'a', rb'CATEGORY 0\r?\n')
    # There are 14 rows in these pinned refs. Clamp at row 0 even after BIG WAVE.
    for _ in range(14):
        row = session.command(b'u', rb'APP \d+\r?\n')
    if not re.search(rb'APP 0\r?\n', row):
        raise RuntimeError("Could not select HELLO row 0")
    memory = []
    for _ in range(cycles):
        started = session.command(b'e', rb'(?:HELLO|KASANE)_FRAME_PRESENTED[^\r\n]*\r?\n')
        if b'APP_ID local.hello' not in started:
            raise RuntimeError("HELLO identity missing or wrong app launched")
        session.command(b'e', rb'HELLO_COUNT 1\r?\n')
        stopped = session.command(b'q', rb'HOME_READY\r?\n')
        if b'APP_STOPPED' not in stopped:
            raise RuntimeError("HELLO stop marker missing")
        match = re.search(rb'MEM free=(\d+) largest=(\d+)', stopped)
        if not match:
            raise RuntimeError("Post-stop memory observation missing")
        memory.append({"free": int(match[1]), "largest": int(match[2])})
    return {"status": "passed", "cycles": cycles, "post_stop_memory": memory,
            "memory_note": "Observations only; Wi-Fi/autosync and device state affect these values",
            "physical_checks": "not assessed", "performance_claim": "none"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--allow-device', action='store_true')
    parser.add_argument('--port')
    parser.add_argument('--cycles', type=int, default=3)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--check-grid-log', type=Path)
    args = parser.parse_args()
    if args.check_grid_log:
        if args.allow_device or args.port or args.out:
            parser.error('Offline log checking cannot be combined with device options')
        print(json.dumps(check_grid_log(args.check_grid_log.read_bytes()), indent=2))
        return
    if not args.allow_device or not args.port or not args.out:
        parser.error('--allow-device, --port and --out are all required; no default port')
    if not 1 <= args.cycles <= 20:
        parser.error('--cycles must be between 1 and 20')
    # Importing this module for offline tests never imports or opens pyserial.
    import serial
    args.out.mkdir(parents=True, exist_ok=False)
    result = {"status": "failed", "error": "interrupted before completion"}
    try:
        port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
        port.dtr = False
        port.rts = False
        port.port = args.port
        with (args.out / 'serial.log').open('wb') as logfile, port:
            result = run_smoke(port, logfile, args.cycles)
        print(json.dumps(result, indent=2))
    except Exception as error:
        result = {"status": "failed", "error": str(error)}
        raise
    finally:
        (args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
