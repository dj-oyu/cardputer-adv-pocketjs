"""Bounded FOX PATCH study through real QuickJS/native primitives. HOST ONLY."""
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
    result = compile_sources(ROOT)
    for name in ('apps/lib/ayatsuri_model.js', 'apps/lib/ayatsuri_clip.js', 'apps/lib/ayatsuri_visibility.js',
                 'tools/character/fox_patch_fixture.js', 'tools/character/fox_retopology_fixture.js',
                 'tools/kasane_contract/test_fox_patch_host.c',
                 'tools/kasane_contract/run_fox_patch_host.py'):
        result[name] = sha(ROOT / name)
    return result


def numeric_fields(line):
    out = {}
    for key, value in re.findall(r'(\w+)=([^ ]+)', line.strip()):
        try:
            out[key] = float(value) if '.' in value or 'e' in value.lower() else int(value)
        except ValueError:
            out[key] = value
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--m32', action='store_true')
    parser.add_argument('--capture-dir', type=Path)
    parser.add_argument('--variant',choices=('baseline','unlit','clay','dense-clay','dense','socket','socket-only','gouraud','gouraud-socket','retopology','retopology-clay'),default='baseline')
    args = parser.parse_args()
    if sys.platform == 'win32':
        parser.error('Use Linux/WSL for ASan or the existing m32 sysroot')
    profile = 'm32' if args.m32 else 'asan64'
    cache = ROOT / '.cache/fox-patch-host' / profile / args.variant
    cache.mkdir(parents=True, exist_ok=True)
    (cache / 'result.json').unlink(missing_ok=True)
    captures = (args.capture_dir or cache / 'captures').resolve()
    flags = ['-Itools/kasane_contract', '-ffp-contract=off', '-fno-fast-math']
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    env['FOX_PATCH_VARIANT']=args.variant
    if args.m32:
        env.setdefault('M32_SYSROOT', '/tmp/m32sys')
        flags += subprocess.check_output(['bash', 'tools/vmtest/m32_sysroot.sh'],
                                         cwd=ROOT, env=env, text=True).split()
        flags += ['-msse2', '-mfpmath=sse']
    else:
        flags += ['-fsanitize=address,undefined']
    before = inputs()
    toolchain = compiler_identity()
    # Existing build() always rebuilds product/test C. Only upstream QJS objects
    # are reused, keyed by their complete source/header and compiler/flag inputs.
    qjs = {k: v for k, v in before.items()
           if k.startswith(('components/quickjs-ng/', 'components/pocketjs_guest/include/'))
           or k == 'tools/kasane_contract/run_megademo_app_host.py'}
    key = content_key(qjs, flags, 'fox-patch-host-v1', toolchain)
    binary = build(flags, ROOT / '.cache/fox-patch-builds' / profile / key[:16], 'tools/kasane_contract/test_fox_patch_host.c')
    binary_before = sha(binary)
    generated = {p.name: sha(p) for p in binary.parent.iterdir()
                 if p.is_file() and p.suffix in ('.h', '.c', '.o')}
    runs = []
    for index in range(2):
        folder = captures / f'run-{index}'
        folder.mkdir(parents=True, exist_ok=True)
        for i in range(3):
            (folder / f'app_{i:04}.ppm').unlink(missing_ok=True)
        env['FOX_PATCH_CAPTURE_DIR'] = str(folder)
        command = [str(binary)]
        result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (cache / f'run-{index}.log').write_text(result.stdout)
        print(result.stdout, end='', flush=True)
        rows = result.stdout.splitlines()
        summaries = [numeric_fields(s) for s in rows if s.startswith('FOX_PATCH_HOST ')]
        metrics = summaries[0] if len(summaries) == 1 else {}
        phases = [numeric_fields(s) for s in rows if s.startswith('FOX_PATCH_PHASE ')]
        timings = [numeric_fields(s) for s in rows if s.startswith('FOX_PATCH_TIME ')]
        natives = [numeric_fields(s) for s in rows if s.startswith('FOX_PATCH_NATIVE ')]
        frames = [numeric_fields(s) for s in rows if s.startswith('FOX_PATCH_FRAME ')]
        # Hex hashes are strings; numeric_fields must not interpret all-digit
        # hashes as decimal for one frame and strings for another.
        frame_hashes = re.findall(r'FOX_PATCH_FRAME .*? hash=([0-9a-f]{8}) ', result.stdout)
        images, complete = [], True
        header = b'P6\n240 135\n255\n'
        for i in range(3):
            path = folder / f'app_{i:04}.ppm'
            data = path.read_bytes() if path.exists() else b''
            complete = complete and data.startswith(header) and len(data) == len(header) + 240 * 135 * 3
            images.append(hashlib.sha256(data).hexdigest())
        profile_ok = (metrics.get('pointer_bits') == (32 if args.m32 else 64)
                      and metrics.get('cap') == (163840 if args.m32 else 0)
                      and metrics.get('frames') == 78 and len(frames) == 78
                      and metrics.get('eager_gc') == 0 and metrics.get('full_reference') == 1
                      and metrics.get('accounting') == 1 and metrics.get('quota') == 0
                      and metrics.get('allocator') == 0
                      and (not args.m32 or metrics.get('guest_peak', 163841) <= 163840))
        released = [p for p in phases if p.get('name') == 'released']
        recreated = [p for p in phases if p.get('name') == 'recreated']
        # First-release lazy metadata may differ; subsequent releases and all
        # recreated live states must plateau, rather than merely count events.
        plateau = lambda rows: len({(p['requested_current'],p['guest_current']) for p in rows}) == 1
        lifecycle_ok = (len(released) == len(recreated) == 3
                        and plateau(released[1:]) and plateau(recreated))
        exception = next((s for s in rows if 'FOX_PATCH_EXCEPTION' in s or ' threw:' in s), None)
        runs.append(dict(command=command, exit_code=result.returncode, metrics=metrics,
                         phases=phases, native_payload=natives, host_timings=timings,
                         frames=frames, frame_hashes=frame_hashes,
                         image_sha256=images, captures_complete=complete,
                         first_exception=exception,
                         passed=result.returncode == 0 and 'FOX_PATCH_PASS released_guest=0 live_blocks=0' in result.stdout
                         and complete and profile_ok and lifecycle_ok and len(timings) == 4))
    unchanged = inputs() == before and sha(binary) == binary_before
    generated_after = {p.name: sha(p) for p in binary.parent.iterdir()
                       if p.is_file() and p.suffix in ('.h', '.c', '.o')}
    unchanged = unchanged and generated_after == generated
    repeated = runs[0]['image_sha256'] == runs[1]['image_sha256'] and runs[0]['frame_hashes'] == runs[1]['frame_hashes']
    proof = dict(kind='HOST SIMULATION', device_capture=False, device_fps_claim=False,
                 passed=all(r['passed'] for r in runs) and unchanged and repeated,
                 profile=profile, variant=args.variant, cap=163840 if args.m32 else None,
                 forced_frame_gc=False, captures=str(captures), runs=runs,
                 source_sha256=before, sources_and_artifacts_unchanged=unchanged,
                 binary=str(binary), binary_sha256=binary_before,
                 generated_and_qjs_artifact_sha256=generated, qjs_cache_key=key,
                 toolchain=toolchain, flags=flags, repeat_images_and_frame_hashes_equal=repeated,
                 memory_scope='TLSF-rounded guest requests plus per-block QuickJS overhead; native arena payload aliases guest accounting, not additional off-guest heap',
                 timing_scope='monotonic host JS call plus pending jobs/turn cleanup; C present separately; full-reference rerender and stats excluded')
    (cache / 'result.json').write_text(json.dumps(proof, indent=2) + '\n')
    print('FOX_PATCH_RUNNER_' + ('PASS' if proof['passed'] else 'FAIL'), cache / 'result.json')
    if not proof['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
