"""Actual FOX SHEET app/chunks and native renderer; host only, never device FPS."""
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


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs():
    found = compile_sources(ROOT)
    folder = ROOT / 'apps/foxsheet'
    paths = {p for p in folder.rglob('*') if p.is_file()}
    paths.update((Path(__file__).resolve(), ROOT / 'tools/kasane_contract/test_fox_sheet_app_host.c'))
    for line in (folder / 'chunks.txt').read_text().splitlines():
        words = line.split('#', 1)[0].split()
        if len(words) == 2 and words[0] not in ('app', 'option'):
            path = (folder / words[1]).resolve()
            path.relative_to(ROOT.resolve())
            paths.add(path)
    found.update({p.resolve().relative_to(ROOT.resolve()).as_posix(): digest(p) for p in paths})
    return found


def stable(phases):
    return all(len(rows := [p for p in phases if p['name'] == name]) == 6 and
               len({(p['rounded'], p['accounted'], p['live']) for p in rows}) == 1
               for name in ('released', 'recreated'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--m32', action='store_true')
    parser.add_argument('--capture-dir', type=Path)
    args = parser.parse_args()
    if sys.platform == 'win32':
        parser.error('Use Linux/WSL for ASAN64 or --m32')
    cache = ROOT / ('.cache/fox-sheet-app-m32' if args.m32 else '.cache/fox-sheet-app-asan')
    cache.mkdir(parents=True, exist_ok=True)
    (cache / 'result.json').unlink(missing_ok=True)
    captures = args.capture_dir or cache / 'captures'
    flags = ['-Itools/kasane_contract', '-ffp-contract=off']
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    if args.m32:
        env.setdefault('M32_SYSROOT', '/tmp/m32sys')
        # One helper in this process; coordinate externally with other m32 jobs.
        flags += subprocess.check_output(['bash', 'tools/vmtest/m32_sysroot.sh'], cwd=ROOT, env=env, text=True).split()
        flags += ['-msse2', '-mfpmath=sse']
    else:
        flags += ['-fsanitize=address,undefined']
    before = inputs()
    toolchain = compiler_identity()
    qjs = {k: v for k, v in before.items() if k.startswith(('components/quickjs-ng/', 'components/pocketjs_guest/include/'))
           or k == 'tools/kasane_contract/run_megademo_app_host.py'}
    key = content_key(qjs, flags, 'fox-sheet-qjs-objects-v1', toolchain)
    binary = build(flags, cache / key[:16], 'tools/kasane_contract/test_fox_sheet_app_host.c')
    artifacts = [binary, *sorted(binary.parent.glob('*.o')), *sorted(binary.parent.glob('*.h')),
                 *sorted(binary.parent.glob('*.c'))]
    artifact_before = {p.relative_to(ROOT).as_posix(): digest(p) for p in artifacts}
    runs = []
    for index in range(2):
        folder = captures / f'run-{index}'
        folder.mkdir(parents=True, exist_ok=True)
        for path in folder.glob('app_*.ppm'):
            path.unlink()
        env['FOX_SHEET_CAPTURE_DIR'] = str(folder.resolve())
        result = subprocess.run([str(binary)], cwd=ROOT, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (cache / f'run-{index}.log').write_text(result.stdout)
        print(result.stdout, end='', flush=True)
        match = re.search(r'SHEET_HOST frames=(\d+) pointer_bits=(\d+) peak_requested=(\d+) accounted_peak=(\d+) max_request=(\d+) cap=(\d+) eager_gc=0 full_reference=1 quota=(\d+) allocator=(\d+) formula=(\d+)', result.stdout)
        metrics = dict(zip(('frames', 'pointer_bits', 'peak_requested', 'accounted_peak', 'max_request', 'cap', 'quota', 'allocator', 'formula'),
                           map(int, match.groups()))) if match else None
        profile = bool(metrics and metrics['frames'] == 130 and metrics['pointer_bits'] == (32 if args.m32 else 64)
                       and not metrics['quota'] and not metrics['allocator'] and metrics['formula'] == 1
                       and (not args.m32 or (metrics['cap'] == 163840 and metrics['accounted_peak'] <= 163840)))
        phases = [dict(name=m.group(1), **dict(zip(('rounded', 'peak', 'max_request', 'accounted', 'cap', 'live'), map(int, m.groups()[1:]))))
                  for m in re.finditer(r'SHEET_PHASE name=(\S+) rounded=(\d+) peak=(\d+) max_request=(\d+) accounted=(\d+) cap=(\d+) live=(\d+)', result.stdout)]
        images = []
        complete = True
        header = b'P6\n240 135\n255\n'
        for number in range(9):
            path = folder / f'app_{number:04}.ppm'
            data = path.read_bytes() if path.exists() else b''
            complete = complete and data.startswith(header) and len(data) == len(header) + 240 * 135 * 3
            images.append(hashlib.sha256(data).hexdigest())
        clean = 'SHEET_HOST_PASS released_guest=0 live_blocks=0' in result.stdout
        runs.append(dict(exit_code=result.returncode, metrics=metrics, phases=phases, captures_complete=complete,
                         image_sha256=images, passed=result.returncode == 0 and clean and profile and stable(phases) and complete))
    unchanged = inputs() == before
    artifact_unchanged = artifact_before == {p.relative_to(ROOT).as_posix(): digest(p) for p in artifacts}
    same_images = runs[0]['image_sha256'] == runs[1]['image_sha256']
    same_metrics = runs[0]['metrics'] == runs[1]['metrics']
    proof = dict(kind='HOST SIMULATION', device_measurement=False,
                 passed=all(r['passed'] for r in runs) and unchanged and artifact_unchanged and same_images and same_metrics,
                 pointer_profile=32 if args.m32 else 64, guest_cap_bytes=163840 if args.m32 else None,
                 forced_frame_gc=False, runs=runs, source_sha256=before, sources_unchanged_during_run=unchanged,
                 binary_object_generated_sha256=artifact_before, artifacts_unchanged_during_run=artifact_unchanged,
                 repeat_images_equal=same_images, repeat_metrics_equal=same_metrics, flags=flags, toolchain=toolchain,
                 capture_dir=str(captures))
    (cache / 'result.json').write_text(json.dumps(proof, indent=2) + '\n')
    print('FOX_SHEET_APP_' + ('PASS' if proof['passed'] else 'FAIL'), cache / 'result.json', flush=True)
    if not proof['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
