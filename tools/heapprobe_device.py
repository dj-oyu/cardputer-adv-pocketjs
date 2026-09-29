"""Evaluation heap probe on the device (docs/vm/spread-eval-oom.md).

Needs an image built with -DPOCKET_HEAPPROBE=ON. Each variant of
apps/heapprobe/heapprobe.js is started alone from the home screen with
'Q<index>[,<limit>]\\n' (main.c), through the path a menu app takes, and
its outcome is read from the log:

  eval   ok / OOM / EVAL_ERROR   (HEAPPROBE eval ..., OOM ..., EVAL_ERROR ...)
  frame  ok / fail / -           (HP_FRAME ok from the variant's first frame)
  used   guest heap before and after the evaluation (JS_GetMemoryCounters)

  python tools/heapprobe_device.py --port COM3                 # every variant
  python tools/heapprobe_device.py --port COM3 --only 1,29,30  # some
  python tools/heapprobe_device.py --port COM3 --bisect 29 --lo 100000 --hi 163840
      # the smallest heap limit the variant still evaluates under (the
      # evaluation's peak as the device charges it), to --step bytes

Run with the ESP-IDF Python environment (pyserial). Opening the port does not
reset the board (DTR/RTS held low).
"""
import argparse
import json
import re
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parent.parent
VARIANTS = ROOT / 'apps' / 'heapprobe' / 'heapprobe.js'


def variant_names():
    return [l[3:].strip() for l in VARIANTS.read_text(encoding='utf-8').splitlines()
            if l.startswith('//@')]


class Board:
    def __init__(self, port, log):
        self.s = serial.Serial(port=None, baudrate=115200, timeout=0.1)
        self.s.dtr = False
        self.s.rts = False
        self.s.port = port
        self.s.open()
        self.log = log
        self.pending = b''
        # Complete lines not yet consumed: a caller that stops at a marker
        # must not lose the lines that arrived in the same read after it.
        self.queue = []

    def lines(self, seconds):
        end = time.monotonic() + seconds
        while True:
            while self.queue:
                yield self.queue.pop(0)
            if time.monotonic() >= end:
                return
            data = self.s.read(4096)
            if not data:
                continue
            self.log.write(data)
            self.pending += data.replace(b'\x00', b'')
            *done, self.pending = self.pending.split(b'\n')
            self.queue += [l.decode(errors='replace').strip() for l in done]

    def until(self, marker, seconds, keep=None):
        for l in self.lines(seconds):
            if keep is not None:
                keep.append(l)
            if marker in l:
                return l
        return None

    def home(self):
        for _ in self.lines(0.2):   # a stale HOME_READY must not answer this one
            pass
        for _ in range(3):
            self.s.write(b'q')
            if self.until('HOME_READY', 8):
                time.sleep(0.3)
                return True
        return False


def run_variant(b, index, limit, frame_wait):
    r = {'index': index, 'limit': limit, 'eval': '?', 'frame': '-', 'before': None,
         'used': None, 'oom': None, 'error': None, 'mu': None, 'log': []}
    if not b.home():
        r['eval'] = 'NO_HOME'
        return r
    b.s.write(f'Q{index}{"," + str(limit) if limit else ""}\n'.encode())
    got_eval = False
    end = time.monotonic() + 20
    for l in b.lines(20):
        r['log'].append(l)
        m = re.search(r'HEAPPROBE before \d+ \S+ \S+ used=(\d+)', l)
        if m:
            r['before'] = int(m.group(1))
        m = re.search(r'HEAPPROBE eval \d+ (\S+) (\S+) used=(\d+)', l)
        if m:
            r['name'], r['eval'], r['used'] = m.group(1), m.group(2), int(m.group(3))
            got_eval = True
            break
        m = re.search(r'OOM n=(\d+) first_req=(\d+) used=(\d+)', l)
        if m:
            r['oom'] = tuple(map(int, m.groups()))
        m = re.search(r'EVAL_ERROR (.*)', l)
        if m:
            r['error'] = m.group(1)[:100]
        m = re.search(r'HEAPPROBE (bc|chunk \d+) .*', l)
        if m:
            r.setdefault('loads', []).append(m.group(0)[10:])
        if 'HEAPPROBE no variant' in l:
            r['eval'] = 'NO_VARIANT'
            return r
        if time.monotonic() > end:
            break
    if not got_eval:
        r['eval'] = 'NO_REPORT'
        return r
    # After the evaluation: the MU line, then the frames.
    for l in b.lines(frame_wait):
        r['log'].append(l)
        if 'HEAPPROBE_MU' in l:
            r['mu'] = dict((k, v) for k, v in re.findall(r'(\w+)=([\d/]+)', l))
        m = re.search(r'OOM n=(\d+) first_req=(\d+) used=(\d+)', l)
        if m:
            r['oom'] = tuple(map(int, m.groups()))
            r['frame'] = 'OOM'
        if 'HP_FRAME ok' in l:
            r['frame'] = 'ok'
            r['note'] = l.split('HP_FRAME ok', 1)[1].strip()
        if 'HP_FRAME fail' in l:
            r['frame'] = 'fail'
            r['error'] = l.split('HP_FRAME fail', 1)[1].strip()[:100]
        if 'HP_ALIVE' in l:
            r['alive'] = True
        m = re.search(r'HEAPPROBE (bc|chunk \d+) .*', l)
        if m:
            r.setdefault('loads', []).append(m.group(0)[10:])
        if re.search(r'E \(\d+\) js:|W \(\d+\) js:', l) and r['frame'] == '-':
            r['frame'] = 'error'
            r['error'] = l[:120]
    if r['eval'] == 'ESP_OK':
        r['eval'] = 'ok'
    elif r['oom']:
        r['eval'] = 'OOM'
    elif r['error']:
        r['eval'] = 'EVAL_ERROR'
    return r


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--port', required=True)
    p.add_argument('--only', help='comma separated variant indices')
    p.add_argument('--limit', type=int, default=0, help='guest heap limit for every run (0: 160 KiB)')
    p.add_argument('--repeat', type=int, default=1)
    p.add_argument('--frame-wait', type=float, default=2.0)
    p.add_argument('--bisect', type=int, help='variant index: find the smallest limit it evaluates under')
    p.add_argument('--lo', type=int, default=60000)
    p.add_argument('--hi', type=int, default=163840)
    p.add_argument('--step', type=int, default=256)
    p.add_argument('--out', default='.cache/heapprobe')
    a = p.parse_args()
    names = variant_names()
    out = ROOT / a.out
    out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    with open(out / f'serial-{stamp}.log', 'wb') as log:
        b = Board(a.port, log)
        results = []
        if a.bisect is not None:
            lo, hi = a.lo, a.hi   # lo fails (assumed), hi evaluates
            r = run_variant(b, a.bisect, hi, 0.5)
            print(f'limit {hi}: {r["eval"]}', flush=True)
            if r['eval'] != 'ok':
                sys.exit(f'variant {a.bisect} does not evaluate at --hi {hi}')
            while hi - lo > a.step:
                mid = (lo + hi) // 2
                r = run_variant(b, a.bisect, mid, 0.5)
                print(f'limit {mid}: {r["eval"]} used={r["used"]} oom={r["oom"]}', flush=True)
                if r['eval'] == 'ok':
                    hi = mid
                else:
                    lo = mid
                results.append(r)
            print(f'BISECT {a.bisect} {names[a.bisect]}: evaluates at {hi}, fails at {lo}')
        else:
            idx = [int(x) for x in a.only.split(',')] if a.only else range(len(names))
            for _ in range(a.repeat):
                for i in idx:
                    r = run_variant(b, i, a.limit, a.frame_wait)
                    results.append(r)
                    print(f'{i:2d} {names[i]:24s} eval={r["eval"]:10s} frame={r["frame"]:5s} '
                          f'before={r["before"]} used={r["used"]} oom={r["oom"]} '
                          f'{r["error"] or ""}{r.get("note", "")}'
                          f'{" alive" if r.get("alive") else ""}', flush=True)
                    for x in r.get('loads', []):
                        print('     ', x, flush=True)
        b.home()
    for r in results:
        r.pop('log', None)
    (out / f'results-{stamp}.json').write_text(json.dumps(results, indent=1))


if __name__ == '__main__':
    main()
