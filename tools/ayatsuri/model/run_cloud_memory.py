"""Run the production cloud memory fixture through actual QuickJS/buttons on host."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/kasane_contract'))
from run_megademo_app_host import build
from run_model_study import compile_sources, compiler_identity, content_key


def runtime_inputs(root):
    """Resolve every source the production chunk table can load, both routes."""
    folder = root / 'apps/cloudmem'
    files = {folder / 'cloud_memory.js', folder / 'chunks.txt'}
    for line in (folder / 'chunks.txt').read_text().splitlines():
        words = line.split('#', 1)[0].split()
        if not words:
            continue
        if len(words) != 2:
            raise ValueError('unsupported cloud memory chunk declaration')
        if words[0] in ('app', 'option'):
            continue
        source = (folder / words[1]).resolve()
        source.relative_to(root.resolve())
        files.add(source)
    return {file.resolve().relative_to(root.resolve()).as_posix():
            hashlib.sha256(file.read_bytes()).hexdigest() for file in sorted(files)}


def study_inputs(root):
    return dict(compile_sources(root), **runtime_inputs(root))


def selftest_inputs():
    with tempfile.TemporaryDirectory(prefix='cloud-memory-inputs-') as temporary:
        root = Path(temporary)
        folder = root / 'apps/cloudmem'
        folder.mkdir(parents=True)
        (folder / 'cloud_memory.js').write_text("pocket.app.load('runtime');")
        declarations = ['app local.cloudmem', 'option strip-debug']
        targets = ['memory_runtime.js', '../lib/ayatsuri_model.js', '../lib/ayatsuri_follow.js',
                   '../cloud/cloud_model.js', '../cloud/cloud_shared_update.js',
                   '../cloud/cloud_pose.js', '../cloud/cloud_dynamic.js',
                   '../character/character.js', '../character/character_blend.js']
        for index, name in enumerate(targets):
            file = (folder / name).resolve()
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(f'fixture {index}')
            declarations.append(f'chunk{index} {name}')
        manifest = folder / 'chunks.txt'
        manifest.write_text('\n'.join(declarations))
        expected = runtime_inputs(root)
        assert len(expected) == len(targets) + 2
        for name in expected:
            file = root / name
            original = file.read_bytes()
            file.write_bytes(original + b'\n# changed\n')
            assert runtime_inputs(root) != expected, name
            file.write_bytes(original)
            assert runtime_inputs(root) == expected
        extra = folder / 'late.js'
        extra.write_text('new source')
        manifest.write_text(manifest.read_text() + '\nlate late.js')
        assert 'apps/cloudmem/late.js' in runtime_inputs(root)
        assert runtime_inputs(root) != expected
    print(f'CLOUD_MEMORY_INPUTS_PASS resolved={len(expected)} stale_negative={len(expected)+1} shared_tree_writes=0')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--m32', action='store_true')
    parser.add_argument('--profile', choices=('DEVICE_SHORT', 'HOST_LONG'), default='DEVICE_SHORT')
    parser.add_argument('--self-test-inputs', action='store_true')
    args = parser.parse_args()
    if args.self_test_inputs:
        selftest_inputs()
        return
    draws = 1115 if args.profile == 'HOST_LONG' else 63
    folder = ROOT / '.cache' / f'cloud-memory-{args.profile.lower()}-{"m32" if args.m32 else "asan"}'
    flags = ['-fsanitize=address,undefined', '-ffp-contract=off']
    if args.m32:
        env = dict(os.environ, M32_SYSROOT=os.environ.get('M32_SYSROOT', '/tmp/m32sys'))
        flags = subprocess.check_output(['bash', 'tools/vmtest/m32_sysroot.sh'], cwd=ROOT, env=env, text=True).split()
        flags += ['-msse2', '-mfpmath=sse', '-ffp-contract=off']
    folder.mkdir(parents=True, exist_ok=True)
    inputs = study_inputs(ROOT)
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
                   CHARACTER_MEMORY_PROFILE=args.profile,
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
    current = study_inputs(ROOT)
    unchanged = inputs == current
    passed = unchanged and all(results) and rows[0] == rows[1] and len(rows[0].splitlines()) == draws*2
    proof = dict(kind='HOST SIMULATION', passed=passed, variants_passed=results,
                 profile=args.profile, frame_rows=draws*2, runs_per_variant=2, full_reference_every_frame=True,
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
