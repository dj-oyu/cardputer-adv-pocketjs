"""Record where the DRAM went, and say what moved since last time.

Flash has had a budget guard since the beginning (check_flash.py); DRAM never
did, and it is the scarcer of the two on a PSRAM-less S3. Over one day of adding
pocket.* capabilities the free heap at the home screen fell from 267,696 to
222,620 bytes and apps that had been running stopped evaluating -- every step
measured by its own author, and nobody adding them up.

Two numbers, because they answer different questions:

  static   what the link map says the firmware reserves, per object file.
           Available without a device, attributable to a file, and what a
           reviewer can act on.
  runtime  what the device reports free while a JS app is up. This is the
           number that decides whether an app runs at all, and it is not the
           static one minus anything simple: heap fragmentation, the guest's
           own allocations and the drivers' runtime buffers all live here.

    python tools/memlog.py --map build_api/cardputer_pocketjs.map
    python tools/memlog.py --map build_api/cardputer_pocketjs.map --port COM3
    python tools/memlog.py --map build_api/cardputer_pocketjs.map --check

The log is JSON lines under .cache/, which .gitignore already covers: it is a
record of this machine's builds, not of the project, and two people's logs would
interleave into nonsense.
"""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOG = ROOT / '.cache' / 'memlog' / 'memory.jsonl'

# What free heap has to survive for a JS app to start. 39 KiB was measured on a
# run that worked with the guest at its 144 KiB cap; below about 30 the font
# atlas rebuild and the layout allocator start failing, and the Rust core
# aborts rather than returning an error, so the symptom is a reboot.
RUNTIME_FLOOR = 32 * 1024
# DIRAM is 341,760 bytes on this part. Past this the guest cannot reach its cap.
STATIC_BUDGET = 190 * 1024


def sizes(map_file):
    """Static DIRAM and flash, whole image and per object file."""
    def run(*extra):
        out = subprocess.run(
            [sys.executable, '-m', 'esp_idf_size', '--format', 'json2',
             *extra, str(map_file)],
            capture_output=True, text=True, check=True).stdout
        return json.loads(out)

    whole = {area['name']: {'used': area['used'], 'total': area['total']}
             for area in run()['layout']}
    files = {}
    for name, entry in run('--files').items():
        diram = entry.get('memory_types', {}).get('DIRAM', {})
        sections = diram.get('sections', {})
        # .iram0.text counts: on the S3 that IRAM comes out of the same pool as
        # the guest heap, so leaving it out would under-report a Wi-Fi-sized
        # cost by a third.
        bss = sections.get('.dram0.bss', {}).get('size', 0)
        data = sections.get('.dram0.data', {}).get('size', 0)
        iram = sections.get('.iram0.text', {}).get('size', 0)
        if bss or data or iram:
            files[entry.get('abbrev_name', name)] = {
                'bss': bss, 'data': data, 'iram': iram,
                'diram': bss + data + iram,
            }
    return whole, files


def probe(port):
    """Free heap from the device: idle at home, and with the built-in app up.

    Drives the same keys tools/smoke_device.py does, and reads app_report()'s
    MEM line, which is logged on both start and stop.
    """
    import serial
    s = serial.Serial(port, 115200, timeout=0.2)
    time.sleep(1.5)
    s.reset_input_buffer()

    def until(marker, seconds=10):
        end = time.monotonic() + seconds
        seen = []
        while time.monotonic() < end:
            line = s.readline().decode(errors='replace').strip()
            if line:
                seen.append(line)
                if marker in line:
                    return seen
        raise SystemExit(f'memlog: never saw {marker}; last lines {seen[-6:]}')

    def mem(lines):
        for line in reversed(lines):
            m = re.search(r'MEM free=(\d+) largest=(\d+) js=(\d+)', line)
            if m:
                return dict(zip(('free', 'largest', 'js'), map(int, m.groups())))
        return None

    try:
        s.write(b'q'); until('HOME_READY')
        s.write(b'a'); until('CATEGORY 0')
        running = mem(s.write(b'e') and until('HELLO_FRAME_PRESENTED'))
        idle = mem(s.write(b'q') and until('APP_STOPPED'))
    finally:
        s.close()
    if not running or not idle:
        raise SystemExit('memlog: the device never reported a MEM line')
    return {'idle': idle, 'running': running}


def head():
    try:
        return subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT,
                              capture_output=True, text=True,
                              check=True).stdout.strip()
    except Exception:
        return None


def previous(log):
    if not log.exists():
        return None
    last = None
    for line in log.read_text(encoding='utf-8').splitlines():
        if line.strip():
            last = json.loads(line)
    return last


def report(now, before):
    print(f"MEMLOG commit={now['commit']} diram={now['static']['DIRAM']['used']} "
          f"flash={now['static']['Flash Code']['used']}")
    if now.get('runtime'):
        r = now['runtime']
        print(f"MEMLOG runtime idle_free={r['idle']['free']} "
              f"app_free={r['running']['free']} "
              f"app_largest={r['running']['largest']} js={r['running']['js']}")
    if not before:
        print('MEMLOG first record; nothing to compare against')
        return
    diram = (now['static']['DIRAM']['used'] - before['static']['DIRAM']['used'])
    print(f"MEMLOG since {before['commit']} ({before['when'][:10]}): "
          f"DIRAM {diram:+d}")
    if now.get('runtime') and before.get('runtime'):
        for where in ('idle', 'running'):
            delta = (now['runtime'][where]['free']
                     - before['runtime'][where]['free'])
            print(f"MEMLOG   {where} free {delta:+d}")
    # Per-file, because "DRAM went up 6 KiB" is not actionable and
    # "pocket_io.c.obj +1069" is. Only movers, biggest first.
    moved = []
    for name, entry in now['files'].items():
        was = before['files'].get(name, {}).get('diram', 0)
        if entry['diram'] != was:
            moved.append((entry['diram'] - was, name, was, entry['diram']))
    for delta, name, was, is_ in sorted(moved, key=lambda m: -abs(m[0]))[:12]:
        print(f"MEMLOG   {name:<28} {was:>7} -> {is_:>7}  {delta:+d}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--map', required=True)
    p.add_argument('--port', help='read the device too; without it, static only')
    p.add_argument('--note', default='')
    p.add_argument('--log', default=str(LOG),
                   help='where to append; one per build directory if you keep '
                        'more than one configuration')
    p.add_argument('--check', action='store_true',
                   help='exit non-zero when a budget is crossed')
    p.add_argument('--only-changes', action='store_true',
                   help='append nothing when the static totals have not moved; '
                        'what the build hook uses, so an incremental rebuild '
                        'that changed no memory leaves no record')
    a = p.parse_args()

    whole, files = sizes(Path(a.map))
    record = {
        'when': time.strftime('%Y-%m-%dT%H:%M:%S'),
        'commit': head(),
        'note': a.note,
        'static': whole,
        'files': files,
    }
    if a.port:
        record['runtime'] = probe(a.port)

    log = Path(a.log)
    before = previous(log)
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('a', encoding='utf-8') as f:
        f.write(json.dumps(record) + '\n')
    report(record, before)

    if a.check:
        failed = []
        if whole['DIRAM']['used'] > STATIC_BUDGET:
            failed.append(f"static DIRAM {whole['DIRAM']['used']} over "
                          f"{STATIC_BUDGET}")
        if record.get('runtime'):
            free = record['runtime']['running']['free']
            if free < RUNTIME_FLOOR:
                failed.append(f'free heap with an app up {free} under '
                              f'{RUNTIME_FLOOR}')
        for line in failed:
            print('MEMLOG_OVER ' + line)
        if failed:
            raise SystemExit(1)
        print('MEMLOG_OK within budget')


if __name__ == '__main__':
    main()
