"""Actual CLOUD MEMORY A,C,A,C in one process; separate from full24 registry coverage."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from run_megademo_app_host import ROOT, build
sys.path.insert(0, str(ROOT / 'tools/ayatsuri/model'))
from run_model_study import compile_sources, compiler_identity, content_key


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs():
    # Documentation edits do not affect compiled code; retain all generator,
    # C/header/JS, data and build inputs in the conservative shared closure.
    found = {k: v for k, v in compile_sources(ROOT).items() if not k.endswith('.md')}
    folder = ROOT / 'apps/cloudmem'
    paths = {folder / 'cloud_memory.js', folder / 'chunks.txt'}
    for line in (folder / 'chunks.txt').read_text().splitlines():
        words = line.split('#', 1)[0].split()
        if len(words) == 2 and words[0] not in ('app', 'option'):
            path = (folder / words[1]).resolve()
            path.relative_to(ROOT.resolve())
            paths.add(path)
    found.update({str(p.resolve().relative_to(ROOT.resolve())).replace('\\', '/'): sha(p) for p in paths})
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--m32', action='store_true')
    args = parser.parse_args()
    if sys.platform == 'win32':
        parser.error('Run under Linux/WSL for ASan or the existing i386 sysroot')
    profile = 'm32' if args.m32 else 'asan64'
    cache = ROOT / '.cache/class-session-order' / profile
    cache.mkdir(parents=True, exist_ok=True)
    (cache / 'result.json').unlink(missing_ok=True)
    flags = ['-Itools/kasane_contract', '-ffp-contract=off', '-fno-fast-math']
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    if args.m32:
        env.setdefault('M32_SYSROOT', '/tmp/m32sys')
        flags += subprocess.check_output(['bash', 'tools/vmtest/m32_sysroot.sh'], cwd=ROOT, env=env, text=True).split()
        flags += ['-msse2', '-mfpmath=sse']
    else:
        flags += ['-fsanitize=address,undefined']
    before = inputs()
    toolchain = compiler_identity()
    qjs = {k: v for k, v in before.items()
           if k.startswith(('components/quickjs-ng/', 'components/pocketjs_guest/include/'))
           or k == 'tools/kasane_contract/run_megademo_app_host.py'}
    key = content_key(qjs, flags, 'class-session-order-qjs-v1', toolchain)
    binary = build(flags, cache / key[:16], 'tools/kasane_contract/test_class_session_order.c')
    binary_hash = sha(binary)
    artifacts = {p.name: sha(p) for p in binary.parent.iterdir() if p.is_file() and p.suffix in ('.c', '.h', '.o')}
    runs = []
    for attempt in range(2):
        result = subprocess.run([str(binary)], cwd=ROOT, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (cache / f'run-{attempt}.log').write_text(result.stdout)
        print(result.stdout, end='', flush=True)
        sessions = [dict(index=int(a), variant=b, result=int(c), frame_calls=int(d), released_guest=int(e), exceptions=int(f), present_failures=int(g))
                    for a, b, c, d, e, f, g in re.findall(r'CLASS_SESSION_END index=(\d+) variant=([AC]) result=(\d+) frame_calls=(\d+) released_guest=(\d+) exceptions=(\d+) present_failures=(\d+)', result.stdout)]
        draws = [dict(variant=a, run=int(b), draws=int(c)) for a, b, c in
                 re.findall(r'CLOUD_MEMORY_HOST variant=([AC]) profile=DEVICE_SHORT run=(\d+) draws=(\d+)', result.stdout)]
        match = re.search(r'CLASS_SESSION_ORDER_PASS sessions=(\d+) successful_draws=(\d+) frame_calls=(\d+) released_guest=(\d+) full_reference=(\d+) eager_gc=(\d+) pointer_bits=(\d+) cap=(\d+)', result.stdout)
        summary = dict(zip(('sessions', 'successful_draws', 'frame_calls', 'released_guest', 'full_reference', 'eager_gc', 'pointer_bits', 'cap'), map(int, match.groups()))) if match else None
        valid = (result.returncode == 0 and summary and summary['sessions'] == 4 and summary['successful_draws'] == 504
                 and summary['released_guest'] == 0 and summary['full_reference'] == 1 and summary['eager_gc'] == 0
                 and summary['pointer_bits'] == (32 if args.m32 else 64) and summary['cap'] == (163840 if args.m32 else 0)
                 and [s['variant'] for s in sessions] == ['A', 'C', 'A', 'C']
                 and all(not s['result'] and not s['released_guest'] and not s['exceptions'] and not s['present_failures'] for s in sessions)
                 and [d['variant'] for d in draws] == ['A', 'A', 'C', 'C', 'A', 'A', 'C', 'C']
                 and [d['run'] for d in draws] == [1, 2] * 4 and all(d['draws'] == 63 for d in draws)
                 and len(re.findall(r'CLOUD_MEMORY_HOST_PASS released_guest=0', result.stdout)) == 4)
        runs.append(dict(exit_code=result.returncode, passed=bool(valid), sessions=sessions, draws=draws, summary=summary,
                         first_exception=next((s for s in result.stdout.splitlines() if 'threw:' in s or 'Assertion' in s or 'CLOUD_MEMORY_HOST_OOM' in s), None)))
    unchanged = before == inputs() and sha(binary) == binary_hash and artifacts == {
        p.name: sha(p) for p in binary.parent.iterdir() if p.is_file() and p.suffix in ('.c', '.h', '.o')}
    repeated = runs[0]['sessions'] == runs[1]['sessions'] and runs[0]['draws'] == runs[1]['draws'] and runs[0]['summary'] == runs[1]['summary']
    proof = dict(kind='HOST ACTUAL APP SESSION ORDER', passed=all(r['passed'] for r in runs) and unchanged and repeated,
                 profile=profile, runs=runs, source_sha256=before, sources_and_artifacts_unchanged=unchanged,
                 binary_sha256=binary_hash, binary=str(binary), qjs_cache_key=key,
                 generated_and_qjs_sha256=artifacts, toolchain=toolchain, flags=flags, repeated_protocol_equal=repeated,
                 forced_frame_gc=False, full_reference_every_present=True, device_run=False,
                 scope='Actual cloudmem A,C,A,C with native renderer and teardown. This host installs API2/shape/Kasane and excludes other hardware namespaces; full24 owner coverage belongs to the separate registry contract.')
    (cache / 'result.json').write_text(json.dumps(proof, indent=2) + '\n')
    print('CLASS_SESSION_ORDER_RUNNER_' + ('PASS' if proof['passed'] else 'FAIL'), cache / 'result.json')
    if not proof['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
