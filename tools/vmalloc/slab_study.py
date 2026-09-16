"""Slab-vs-TLSF comparison driver for docs/vm/vm-ledger/08-slab-study.md.

Same method as compare_fragmentation.py (one o2 binary, identical traces,
identical pool, every run under --verify, binary and trace SHA256 kept in the
JSON), widened to many allocator variants and three measurements per trace:

  fixed   --pool 163840 (main/app_session.c's JS heap limit), every op sampled:
          does it fit, and how small does the largest free extent get
  wide    --pool 16 MiB: capacity out of the way, so pool - min(largest free
          extent) is the address span the allocator needed, independent of
          whether 160 KiB was enough
  bisect  --bisect: the smallest pool (64 B resolution) the trace completes in

A variant is an allocator name, optionally with --cfg knobs:
  tlsf  estalloc  segment  slab  slab:carve=split:pick=low  slab:classes=16+32:var_max=0
(knobs are ':'-separated; '+' inside a value becomes ',')

  python3 tools/vmalloc/slab_study.py --output .cache/slab/study.json \\
      --variants tlsf,estalloc,segment,slab --jobs 8 .cache/vmtest/traces/*.trace

Everything here is a host replay (JSValue 16 B, pointer 8 B); none of it is a
device measurement.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import statistics
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXED_POOL = 163840
WIDE_POOL = 16 * 1024 * 1024
STEPS = (59296, 29648)  # taffy single-block steps (CLAUDE.md), 34+ and 17-33 nodes


def run(binary, variant, trace, mode, lines):
    name, *knobs = variant.split(':')
    cmd = [str(binary), '--allocator', name]
    for kv in knobs:
        cmd += ['--cfg', kv.replace('+', ',')]
    if mode == 'bisect':
        cmd += ['--bisect', '--sample-every', '0']
    else:
        pool = FIXED_POOL if mode == 'fixed' else WIDE_POOL
        # Big traces are sampled on a stride in the wide run: stats() walks
        # the whole heap (TLSF's multi_heap_walk), and every-op sampling of a
        # multi-megabyte trace would take hours. The stride is recorded.
        every = 1 if mode == 'fixed' else max(1, lines // 100000)
        cmd += ['--pool', str(pool), '--sample-every', str(every), '--verify']
        if every > 1:
            cmd += ['--verify-gap', str(every)]
    cmd.append(str(trace))
    p = subprocess.run(cmd, capture_output=True, text=True)
    fields = dict(part.split('=', 1) for part in p.stdout.split() if '=' in part)
    return {'variant': variant, 'trace': trace.name, 'mode': mode, 'rc': p.returncode,
            'cmd': ' '.join(cmd[1:]), 'fields': fields,
            'stderr': p.stderr[-2000:] if p.returncode else ''}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--variants', required=True, help='comma-separated variants, see above')
    ap.add_argument('--modes', default='fixed,wide,bisect')
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--bisect-limit', type=int, default=1_500_000,
                    help='skip --bisect for traces with more lines than this (it replays ~30 times)')
    ap.add_argument('--binary', default='o2', help="replay variant: o2 (numbers) or asan (sanitizer pass)")
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('traces', type=Path, nargs='+')
    a = ap.parse_args()
    binary = ROOT / f'.cache/vmalloc/vmalloc_replay-{a.binary}'
    variants = a.variants.split(',')
    traces = [t for t in a.traces if not t.name.startswith('bench_')]
    lines = {t: sum(1 for _ in open(t)) for t in traces}
    jobs = []
    for mode in a.modes.split(','):
        for t in traces:
            if mode == 'bisect' and lines[t] > a.bisect_limit:
                continue
            for v in variants:
                jobs.append((v, t, mode))
    rows = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(run, binary, v, t, m, lines[t]) for v, t, m in jobs]
        for k, f in enumerate(cf.as_completed(futs)):
            r = f.result()
            rows.append(r)
            if r['rc'] not in (0,):
                print('RC', r['rc'], r['variant'], r['trace'], r['mode'], r['stderr'][-300:], flush=True)
            if (k + 1) % 50 == 0:
                print(f'{k + 1}/{len(futs)}', flush=True)
    report = {
        'host_replay': True, 'fixed_pool': FIXED_POOL, 'wide_pool': WIDE_POOL,
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'traces': {t.name: {'sha256': hashlib.sha256(t.read_bytes()).hexdigest(), 'lines': lines[t]} for t in traces},
        'runs': sorted(rows, key=lambda r: (r['mode'], r['trace'], r['variant'])),
    }
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
    summarize(report, variants)
    bad = [r for r in rows if r['rc'] != 0 or r['fields'].get('verify', 'OK') != 'OK' or r['fields'].get('check') == '0']
    raise SystemExit(1 if bad else 0)


def summarize(report, variants):
    by = {}
    for r in report['runs']:
        by.setdefault((r['variant'], r['mode']), []).append(r)
    ntr = len(report['traces'])
    print(f'\n{ntr} traces, host replay. fixed={FIXED_POOL} wide={WIDE_POOL}')
    cols = ['variant', 'fit 160KiB', 'min largest extent (fit)', '>=59,296', '>=29,648', 'max ext.frag (fit)',
            'max inside slack (fit)', 'max usable waste (fit)', 'median wide deficit', 'sum min_pool (n)',
            'malloc steps mean', 'free steps mean', 'grow covered / grows']
    print('| ' + ' | '.join(cols) + ' |')
    print('|' + '---|' * len(cols))

    def ints(rows, key):
        return [int(f[key]) for f in rows if f.get(key, 'n/a') not in ('n/a', '')]

    def mx(x): return f'{max(x):,}' if x else '-'
    def mn(x): return f'{min(x):,}' if x else '-'
    for v in variants:
        fx = by.get((v, 'fixed'), [])
        fit = [r['fields'] for r in fx if r['fields'].get('result') == 'OK']
        wide = [r['fields'] for r in by.get((v, 'wide'), []) if r['fields'].get('result') == 'OK']
        lg = ints(fit, 'app_min_pool_largest')
        wd = [WIDE_POOL - x for x in ints(wide, 'app_min_pool_largest')]
        bs = [int(r['fields']['min_pool']) for r in by.get((v, 'bisect'), []) if 'min_pool' in r['fields']]
        ms, fs = ints(fit, 'malloc_steps_mean_x100'), ints(fit, 'free_steps_mean_x100')
        gc, gr = ints(wide, 'realloc_grow_covered'), ints(wide, 'realloc_grows')
        cells = [v, f'{len(fit)}/{len(fx)}', mn(lg), str(sum(x >= STEPS[0] for x in lg)), str(sum(x >= STEPS[1] for x in lg)),
                 mx(ints(fit, 'app_ext_frag')), mx(ints(fit, 'app_slack_inside')), mx(ints(fit, 'app_usable_waste')),
                 f'{int(statistics.median(wd)):,}' if wd else '-', f'{sum(bs):,} ({len(bs)})',
                 f'{statistics.mean(ms) / 100:.2f}' if ms else '-', f'{statistics.mean(fs) / 100:.2f}' if fs else '-',
                 f'{sum(gc)} / {sum(gr)}' if gc else f'n/a / {sum(gr)}']
        print('| ' + ' | '.join(cells) + ' |')


def tables(path, variants):
    """Per-trace tables for the doc, from a JSON this script wrote."""
    report = json.loads(Path(path).read_text(encoding='utf-8'))
    runs = {(r['variant'], r['trace'], r['mode']): r['fields'] for r in report['runs']}
    names = sorted(report['traces'])
    variants = variants or sorted({r['variant'] for r in report['runs']})
    def g(v, t, m, k):
        f = runs.get((v, t, m), {})
        return f.get(k)
    print('\n## app_min_pool_largest at 160 KiB (FAIL = did not fit)')
    print('| trace | ' + ' | '.join(variants) + ' |')
    print('|---|' + '---:|' * len(variants))
    for t in names:
        cells = []
        for v in variants:
            f = runs.get((v, t, 'fixed'), {})
            cells.append(f'{int(f["app_min_pool_largest"]):,}' if f.get('result') == 'OK' else 'FAIL')
        print(f'| {t[:-6]} | ' + ' | '.join(cells) + ' |')
    print('\n## min_pool (bisect) relative to the first variant')
    base = variants[0]
    print('| variant | fits 160KiB by bisect | median ratio | min ratio | max ratio | sum over traces with base min_pool < 400KiB |')
    print('|---|---:|---:|---:|---:|---:|')
    for v in variants:
        rat, small, fits = [], 0, 0
        for t in names:
            b, x = g(base, t, 'bisect', 'min_pool'), g(v, t, 'bisect', 'min_pool')
            if b is None or x is None:
                continue
            rat.append(int(x) / int(b))
            fits += int(x) <= FIXED_POOL
            if int(b) < 400 * 1024:
                small += int(x)
        print(f'| {v} | {fits}/{len(rat)} | {statistics.median(rat):.3f} | {min(rat):.3f} | {max(rat):.3f} | {small:,} |')
    print('\n## steps and reallocs (wide pool, all traces)')
    print('| variant | malloc steps mean | malloc steps max | free steps max | realloc in place / calls | grow covered / grows | max peak_used (fit) |')
    print('|---|---:|---:|---:|---:|---:|---:|')
    for v in variants:
        w = [runs[(v, t, 'wide')] for t in names if (v, t, 'wide') in runs]
        fx = [runs[(v, t, 'fixed')] for t in names if runs.get((v, t, 'fixed'), {}).get('result') == 'OK']
        ms = [int(f['malloc_steps_mean_x100']) for f in w]
        cov = [f.get('realloc_grow_covered', 'n/a') for f in w]
        cov = 'n/a' if 'n/a' in cov else f'{sum(map(int, cov)):,}'
        print(f'| {v} | {statistics.mean(ms) / 100:.2f} | {max(int(f["malloc_steps_max"]) for f in w):,} '
              f'| {max(int(f["free_steps_max"]) for f in w)} '
              f'| {sum(int(f["realloc_in_place"]) for f in w):,} / {sum(int(f["realloc_calls"]) for f in w):,} '
              f'| {cov} / {sum(int(f["realloc_grows"]) for f in w):,} | {max(int(f["peak_used"]) for f in fx):,} |')


if __name__ == '__main__':
    import sys
    if len(sys.argv) >= 3 and sys.argv[1] == '--tables':
        # python3 slab_study.py --tables study.json [variant ...]
        tables(sys.argv[2], sys.argv[3:])
    else:
        main()
