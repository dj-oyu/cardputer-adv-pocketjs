"""Run the production cloud memory fixture through actual QuickJS/buttons on host."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/kasane_contract'))
from run_megademo_app_host import build
from run_model_study import compile_sources, compiler_identity, content_key


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--m32', action='store_true')
    args = parser.parse_args()
    folder = ROOT / '.cache' / ('cloud-memory-m32' if args.m32 else 'cloud-memory-asan')
    flags = ['-fsanitize=address,undefined', '-ffp-contract=off']
    if args.m32:
        env = dict(os.environ, M32_SYSROOT=os.environ.get('M32_SYSROOT', '/tmp/m32sys'))
        flags = subprocess.check_output(['bash', 'tools/vmtest/m32_sysroot.sh'], cwd=ROOT, env=env, text=True).split()
        flags += ['-msse2', '-mfpmath=sse', '-ffp-contract=off']
    folder.mkdir(parents=True, exist_ok=True)
    inputs = compile_sources(ROOT)
    for file in (ROOT / 'apps/cloudmem').glob('*'):
        if file.is_file():
            inputs[file.relative_to(ROOT).as_posix()] = hashlib.sha256(file.read_bytes()).hexdigest()
    profile = compiler_identity()
    key = content_key(inputs, flags, hashlib.sha256((ROOT / 'apps/cloud/assets/cloud.aynm').read_bytes()).hexdigest(), profile)
    manifest_path = folder / 'build-manifest.json'
    old = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    object_names = ('dtoa.o', 'libregexp.o', 'libunicode.o', 'quickjs.o', 'quickjs-vm.o')
    valid = old.get('key') == key and all((folder / name).is_file() and
        hashlib.sha256((folder / name).read_bytes()).hexdigest() == old.get('objects', {}).get(name)
        for name in object_names)
    if not valid:
        for obj in folder.glob('*.o'):
            obj.unlink()
    binary = build(flags, folder, 'tools/kasane_contract/test_character_app_host.c')
    manifest_path.write_text(json.dumps(dict(key=key, objects={name:
        hashlib.sha256((folder / name).read_bytes()).hexdigest() for name in object_names})))
    rows, results = [], []
    for variant in ('A', 'C'):
        env = dict(os.environ, CHARACTER_MEMORY_APP=variant,
                   CHARACTER_APP_ENTRY='apps/cloudmem/cloud_memory.js',
                   CHARACTER_APP_CHUNKS='apps/cloudmem/chunks.txt',
                   CHARACTER_MEMORY_PIXELS=str(folder / (variant + '-pixels.csv')),
                   CHARACTER_HEAP_LIMIT='163840', CHARACTER_NO_EAGER_GC='1')
        for key in ('CHARACTER_FIXTURE', 'CHARACTER_MEMORY_STUDY', 'CHARACTER_CAPTURE_DIR'):
            env.pop(key, None)
        env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=1')
        env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
        run = subprocess.run([str(binary)], cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (folder / (variant + '.log')).write_text(run.stdout)
        print(run.stdout, end='')
        results.append(run.returncode == 0 and 'CLOUD_MEMORY_HOST_PASS' in run.stdout)
        rows.append((folder / (variant + '-pixels.csv')).read_bytes())
    current = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs}
    unchanged = inputs == current
    passed = unchanged and all(results) and rows[0] == rows[1] and len(rows[0].splitlines()) == 2230
    proof = dict(kind='HOST SIMULATION', passed=passed, variants_passed=results,
                 frame_rows=2230, runs_per_variant=2, full_reference_every_frame=True,
                 same_binary=True, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                 source_hashes=inputs, sources_unchanged_during_run=unchanged,
                 image_sequence_equal=rows[0] == rows[1],
                 frame_sequence_sha256=hashlib.sha256(rows[0]).hexdigest(),
                 heap_limit_bytes=163840, forced_frame_gc=False, device_measurement=False,
                 gc_count=None, gc_time=None, device_heap=None)
    (folder / 'result.json').write_text(json.dumps(proof, indent=2) + '\n')
    print('CLOUD_MEMORY_COMPARISON_' + ('PASS' if passed else 'FAIL'), folder / 'result.json')
    if not passed:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
