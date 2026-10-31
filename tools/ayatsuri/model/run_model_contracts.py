"""Run the existing offline, pure-C and real-QuickJS AYNM contracts; host only.

Default: Linux ASan/UBSan, 64-bit. --m32: existing i386 sysroot, SSE2 binary64
intermediates, no sanitizers. Neither profile operates a device.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
QJS = ROOT / 'components/quickjs-ng/quickjs-ng'
QJS_UNITS = ('dtoa', 'libregexp', 'libunicode', 'quickjs', 'quickjs-vm')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--m32', action='store_true', help='i386/SSE2 profile without sanitizers')
    args = parser.parse_args()
    if sys.platform == 'win32':
        parser.error('Run under Linux/WSL: the default profile requires ASan/UBSan')
    if not args.m32 and struct.calcsize('P') != 8:
        parser.error('The default ASan/UBSan profile requires a 64-bit host')
    compiler = shutil.which(os.environ.get('CC', 'gcc'))
    node = shutil.which(os.environ.get('NODE', 'node'))
    if not compiler or not node:
        parser.error('A C compiler, Node.js and Git are required on PATH')
    env = os.environ.copy()
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    profile = 'm32' if args.m32 else 'asan64'
    cache = ROOT / '.cache/ayatsuri-model-contracts' / profile
    cache.mkdir(parents=True, exist_ok=True)
    # A failed rerun must never leave an older successful proof in this folder.
    (cache / 'result.json').unlink(missing_ok=True)
    commands = []

    def run(command, log=None):
        command = [str(part) for part in command]
        commands.append(command)
        print('+', shlex.join(command), flush=True)
        if log is None:
            subprocess.run(command, cwd=ROOT, env=env, check=True)
            return
        result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (cache / log).write_text(result.stdout)
        print(result.stdout, end='', flush=True)
        result.check_returncode()

    # The compiler's fixture export is pinned to its recorded source commit.
    run([node, 'tools/ayatsuri/model/test_pack_model.cjs'], 'pack.log')
    run([node, 'tools/ayatsuri/model/pack_model.cjs', '.cache/ayatsuri-native/cloud.aynm'], 'export.log')
    fixtures = [ROOT / '.cache/ayatsuri-native' / name for name in
                ('tiny.aynm', 'cloud.aynm', 'region.aynm', 'region-plain.aynm', 'blend-region.aynm', 'blend-region-plain.aynm', 'source-plane.aynm')]
    extra = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    if args.m32:
        # The helper and all executions share this process/call; /tmp can be
        # transient between separate WSL invocations. Never run helpers in parallel.
        env.setdefault('M32_SYSROOT', '/tmp/m32sys')
        command = ['bash', 'tools/vmtest/m32_sysroot.sh']
        commands.append(command)
        extra = subprocess.run(command, cwd=ROOT, env=env, check=True,
                               text=True, stdout=subprocess.PIPE).stdout.split()
        extra += ['-msse2', '-mfpmath=sse']
    flags = ['-std=gnu11', '-O1', '-g', '-fno-fast-math', '-ffp-contract=off', *extra]
    defines = ['-DQUICKJS_NG_BUILD', '-D_GNU_SOURCE']
    includes = ['-I' + str(path) for path in
                (QJS, ROOT / 'main', ROOT / 'main/pocket', ROOT / 'main/ui/kasane', ROOT / 'tools/hostshim')]

    # Cache only upstream QuickJS objects. The key covers compiler identity,
    # complete local C/header/include sources and exact flags/profile. Our core
    # and tests always rebuild, so working-tree fixes cannot use stale binaries.
    version = subprocess.run([compiler, '--version'], env=env, check=True,
                             text=True, stdout=subprocess.PIPE).stdout
    key = hashlib.sha256(json.dumps([compiler, version, flags, defines, includes]).encode())
    dependencies = sorted(p for p in QJS.rglob('*') if p.is_file() and p.suffix in ('.c', '.h', '.inc'))
    for path in dependencies:
        key.update(str(path.relative_to(QJS)).encode())
        key.update(path.read_bytes())
    object_cache = cache / 'qjs' / key.hexdigest()
    object_cache.mkdir(parents=True, exist_ok=True)
    objects = []
    for name in QJS_UNITS:
        obj = object_cache / (name + '.o')
        if not obj.exists():
            temporary = obj.with_suffix('.o.tmp')
            run([compiler, *flags, *defines, *includes, '-c', QJS / (name + '.c'), '-o', temporary])
            temporary.replace(obj)
        objects.append(obj)
    core = ROOT / 'main/ayatsuri/ayatsuri_model.c'
    core_test = ROOT / 'tools/ayatsuri/model/test_model_core.c'
    region_test = ROOT / 'tools/ayatsuri/model/test_model_region.c'
    blend_region_test = ROOT / 'tools/ayatsuri/model/test_model_blend_region.c'
    qjs_test = ROOT / 'tools/ayatsuri/model/test_model_qjs.c'
    core_binary, qjs_binary = cache / 'test-model-core', cache / 'test-model-qjs'
    strict = ['-Wall', '-Wextra', '-Werror']
    run([compiler, *flags, *strict, *includes, core, core_test, '-lm', '-o', core_binary])
    region_binary = cache / 'test-model-region'
    run([compiler, *flags, *strict, *includes, core, region_test, '-lm', '-o', region_binary])
    blend_region_binary = cache / 'test-model-blend-region'
    run([compiler, *flags, *strict, *includes, core, blend_region_test, '-lm', '-o', blend_region_binary])
    renderer = ROOT / 'main/ui/kasane/ksn_procedural.c'
    run([compiler, *flags, *strict, *defines, *includes, core, renderer, qjs_test, *objects, '-lm', '-o', qjs_binary])
    source_plane_test = ROOT / 'tools/ayatsuri/model/test_source_plane_qjs.c'
    source_plane_binary = cache / 'test-source-plane-qjs'
    run([compiler, *flags, *strict, *defines, *includes, core, renderer, source_plane_test, *objects, '-lm', '-o', source_plane_binary])
    run([core_binary, fixtures[1]], 'core.log')  # Core fixture asserts the 19-node cloud.
    run([region_binary, fixtures[2], fixtures[3]], 'region.log')
    run([blend_region_binary, fixtures[4], fixtures[5]], 'blend-region.log')
    run([qjs_binary, fixtures[0]], 'qjs-tiny.log')
    run([qjs_binary, fixtures[1]], 'qjs-cloud.log')
    run([source_plane_binary, fixtures[6]], 'source-plane-qjs.log')
    sources = [core, ROOT / 'main/ayatsuri/ayatsuri_model.h',
               ROOT / 'main/ayatsuri/ayatsuri_transform.h',
               ROOT / 'main/pocket/pocket_ayatsuri_model.inc', core_test, region_test, blend_region_test, qjs_test,
               renderer, ROOT / 'main/ui/kasane/ksn_procedural.h',
               ROOT / 'apps/lib/ayatsuri_model.js',
               ROOT / 'tools/ayatsuri/model/test_model_qjs.js', Path(__file__).resolve()]
    sources += [ROOT / 'tools/ayatsuri/model/pack_model.cjs', ROOT / 'tools/ayatsuri/model/test_pack_model.cjs']
    sources += [source_plane_test, ROOT / 'tools/ayatsuri/model/test_source_plane_qjs.js',
                ROOT / 'apps/character/character.js', ROOT / 'apps/character/character_blend.js',
                ROOT / 'apps/lib/ayatsuri_local_plane.js']
    proof = dict(kind='HOST SIMULATION', passed=True, profile=profile,
                 compiler=version.splitlines()[0], flags=flags, commands=commands,
                 qjs_cache_key=key.hexdigest(),
                 source_sha256={str(p.relative_to(ROOT)): digest(p) for p in sources},
                 fixture_sha256={p.name: digest(p) for p in fixtures})
    (cache / 'result.json').write_text(json.dumps(proof, indent=2) + '\n')
    print('AYNM_CONTRACTS_PASS', profile, cache / 'result.json', flush=True)


if __name__ == '__main__':
    main()
