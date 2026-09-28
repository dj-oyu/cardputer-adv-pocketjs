"""Stage KSV in a full app, measure the FLOWER overlay, then exact-clean it.

The stage reserves the filename with create mode. A pre-existing name is a
collision, and this runner will not invoke cleanup for it. Recovery of a
previous diagnostic file is a separate, deliberate operation.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import time

import serial


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', default='COM3')
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
owned = False


def app_run(case, prefix, outcome, path, seconds):
    lines = []
    port = serial.Serial(port=None, baudrate=115200, timeout=0.15)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        with port:
            time.sleep(0.4)
            port.reset_input_buffer()

            def wait_for(marker, timeout=15):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    line = port.readline().decode(errors='replace').strip()
                    if not line:
                        continue
                    lines.append(line)
                    if ('panic' in line.lower() or 'Guru Meditation' in line or
                            'RUNAWAY' in line or
                            'OVERLAY_STOPPED OVER BUDGET' in line or
                            f'{prefix} ERROR' in line or
                            f'{prefix} REFUSED' in line):
                        raise RuntimeError(f'{prefix} device failure: {line}')
                    if marker in line or (marker.startswith('D6_CLEANUP REMOVED') and
                                          'D6_CLEANUP ABSENT' in line):
                        return line
                raise RuntimeError(f'{prefix} missing {marker}: {lines[-20:]}')

            def key(value, marker, timeout=15):
                port.write(value)
                return wait_for(marker, timeout)

            key(b'q', 'HOME_READY')
            key(b'a', 'CATEGORY 0')
            for _ in range(12):
                key(b'u', 'APP ')
            for index in range(1, case + 1):
                key(b'd', f'APP {index}')
            app_id = 'local.videolab' if case == 10 else 'local.gridlab'
            key(b'e', f'APP_ID {app_id}', 20)
            pick = wait_for('PICK ', 20)
            if 'PICK 2 folders' not in pick:
                raise RuntimeError(f'{prefix} unexpected SD picker: {pick}')
            port.write(b'de')
            granted = wait_for('GRANTED ', 20)
            if 'GRANTED music' not in granted:
                raise RuntimeError(f'{prefix} unexpected grant: {granted}')
            wait_for(f'{prefix} GRANTED', 15)
            if prefix == 'D6_STAGE':
                wait_for('D6_STAGE OPENED', 15)
                global owned
                owned = True
            result = wait_for(outcome, seconds)
            print(result, flush=True)
            key(b'q', 'HOME_READY', 15)
            return lines
    finally:
        path.write_text('\n'.join(lines) + '\n', encoding='utf-8')


try:
    staged = app_run(10, 'D6_STAGE',
                     'D6_STAGE COMMITTED bytes=157216 writer=closed',
                     args.out / 'stage.log', 90)
    created_at = next(i for i, line in enumerate(staged)
                      if 'D6_STAGE CREATE_START' in line)
    committed_at = next(i for i, line in enumerate(staged)
                        if 'D6_STAGE COMMITTED' in line)
    write_perf = [line for line in staged[created_at:committed_at]
                  if 'PERF mode=3' in line]
    print(f'D6_STAGE_PASS write_perf={len(write_perf)}', flush=True)
    subprocess.run([sys.executable, 'tools/overlay_device_test.py',
                    '--port', args.port, '--out', str(args.out),
                    '--d6-sd-av-stream'], check=True)
finally:
    if owned:
        app_run(9, 'D6_CLEANUP', 'D6_CLEANUP REMOVED bytes=',
                args.out / 'cleanup.log', 45)
        print('D6_CLEANUP_PASS', flush=True)
    else:
        print('D6_CLEANUP_SKIPPED no stage ownership; collision preserved',
              flush=True)
