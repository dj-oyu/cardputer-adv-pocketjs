"""The shipped apps that keep themselves asleep on Back (docs/vm/app-suspend-design.md).

For each of IMU CALIBRATION, POCKET PET and PET COMPANION: open it from the
menu, Back (it must go to sleep, not stop), open the same row (it must resume
the same session -- no second *_READY), Back again, then open HELLO WORLD,
which must end the sleeping app with stop("evict").

    python tools/test_app_resume.py --port COM3
"""
import argparse
import re
import time

import serial

APPS = [(4, 'local.imucal', 'IMUCAL_READY'),
        (5, 'local.pet', 'PET_READY'),
        (6, 'local.companion', 'COMPANION_READY')]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    a = ap.parse_args()
    port = serial.Serial(a.port, 115200, timeout=0.2)

    def send(key, marker, timeout=12):
        port.write(key.encode())
        deadline = time.monotonic() + timeout
        seen = []
        while time.monotonic() < deadline:
            line = port.readline().decode(errors='replace').strip()
            if not line:
                continue
            seen.append(line)
            if any(bad in line for bad in ('Guru Meditation', 'assert failed', 'CORRUPT HEAP')):
                raise RuntimeError(line)
            if re.search(marker, line):
                return seen
        raise RuntimeError(f'waiting for {marker!r} after {key!r}: {seen[-12:]}')

    def to_row(row, marker):
        send('a', 'CATEGORY 0')
        for _ in range(8):
            send('u', 'APP ')
        for n in range(1, row + 1):
            send('d', f'APP {n}')
        return send('e', marker, 20)

    try:
        time.sleep(1.5)
        port.reset_input_buffer()
        send('q', 'HOME_READY')
        for row, app_id, ready in APPS:
            to_row(row, ready)
            time.sleep(1.0)
            lines = send('q', 'HOME_READY')
            text = '\n'.join(lines)
            assert f'APP_SUSPENDED {app_id}' in text and 'APP_STOPPED' not in text, text
            lines = to_row(row, f'APP_RESUMED {app_id}')
            time.sleep(1.0)
            assert ready not in '\n'.join(lines), 'the app started again instead of resuming'
            lines = send('q', 'HOME_READY')
            assert f'APP_SUSPENDED {app_id}' in '\n'.join(lines), lines
            lines = to_row(0, 'FRAME_PRESENTED')
            text = '\n'.join(lines)
            assert f'APP_EVICT {app_id}' in text, text
            send('q', 'HOME_READY')
            print(f'APP_RESUME_OK {app_id}', flush=True)
        print('TEST_APP_RESUME_OK', flush=True)
    finally:
        port.close()


if __name__ == '__main__':
    main()
