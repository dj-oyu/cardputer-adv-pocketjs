"""Compare OFF and experimental batch with actual QuickJS/renderer, no oracle heap.

ASan/UBSan safety, all-frame pixel hash, fixed captured panels and race result.
Host times are deliberately not a device performance claim.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

import run_derby as host
from derby_runner_batch import generate


def main():
    out = host.CACHE / 'runner-comparison'
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    for enabled in (False, True):
        name = 'on' if enabled else 'off'
        cache = out / name
        app = host.lower(host.ROOT / 'apps/derby', cache / 'apps/derby')
        if enabled:
            generate(app)
        # Reuse only same-worktree ASan QuickJS objects. App/native source is
        # always rebuilt; these objects are identical for the two variants.
        cache.mkdir(parents=True, exist_ok=True)
        for obj in (host.CACHE / 'asan').glob('*.o'):
            shutil.copy2(obj, cache / obj.name)
        binary = host.build(['-fsanitize=address,undefined'], cache, runner_batch=enabled)
        panels = cache / 'panels'
        panels.mkdir(exist_ok=True)
        for old in panels.glob('*.ppm'):
            old.unlink()
        env = dict(os.environ, DERBY_APP_DIR=str(app), DERBY_HEAP_LIMIT='524288', DERBY_PPM=str(panels))
        for variable in ('DERBY_SER_ORACLE', 'DERBY_RUNNER_BATCH_ORACLE', 'DERBY_JS', 'DERBY_TIER', 'DERBY_HW'):
            env.pop(variable, None)
        env['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        log = cache / 'run.log'
        with log.open('w') as output:
            run = subprocess.run([str(binary)], env=env, cwd=host.ROOT, stdout=output,
                                 stderr=subprocess.STDOUT, timeout=600)
        text = log.read_text()
        if run.returncode or 'DERBY_HOST PASS' not in text:
            raise SystemExit(f'{name} failed: {log}')
        panel_hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(panels.glob('*.ppm'))}
        results[name] = {
            'pixel_hash': re.search(r'^pixels hash (\w+)$', text, re.M)[1],
            'finish': re.search(r'^finish: (.*)$', text, re.M)[1],
            'heap': re.search(r'^guest heap \(TLSF model,.*$', text, re.M)[0],
            'panels': panel_hashes,
        }
    assert results['off']['pixel_hash'] == results['on']['pixel_hash']
    assert results['off']['finish'] == results['on']['finish']
    assert results['off']['panels'] and results['off']['panels'] == results['on']['panels']
    result = {'comparison': results, 'pixels_equal': True, 'finish_equal': True,
              'scope': '64-bit ASan host MID fixed game; no instrumentation/oracle, no hardware timing'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
