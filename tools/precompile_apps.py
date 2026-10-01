"""Experimental build-time compilation of final, already-lowered app chunks.

Outputs are deliberately NOT firmware inputs. Host round trips do not prove
Xtensa compatibility. A source/engine/flags/ABI manifest makes that boundary
explicit and permits reproducibility and stale-artifact checks.
"""
import argparse
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
QJS = ROOT / 'components/quickjs-ng/quickjs-ng'
FLAGS = ['-std=gnu11', '-O2', '-DQUICKJS_NG_BUILD', '-D_GNU_SOURCE']
SOURCES = ['dtoa', 'libregexp', 'libunicode', 'quickjs', 'quickjs-vm']


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def engine_files():
    # Include generated opcode/atom/ROM tables and VM headers, not just
    # BC_VERSION. Even unchanged serialization versions can have new meaning.
    return sorted(p for p in QJS.iterdir() if p.suffix in ('.c', '.h')) + [
        ROOT / 'components/quickjs-ng/CMakeLists.txt',
        ROOT / 'tools/precompile_app.c', ROOT / 'tools/precompile_apps.py',
        *sorted((ROOT / 'components/pocketjs_guest/include').rglob('*.h')),
    ]


def fingerprint(flags=FLAGS):
    files = {p.relative_to(ROOT).as_posix(): digest(p) for p in engine_files()}
    value = {'files': files, 'flags': flags}
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def atomic_write(path, data):
    path = Path(path)
    if path.exists() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(dir=path.parent, prefix=path.name + '.')
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def build_compiler(out, cc):
    out.mkdir(parents=True, exist_ok=True)
    inc = out / 'include'
    inc.mkdir(exist_ok=True)
    atomic_write(inc / 'sdkconfig.h', b'/* Experimental host compiler; no target config. */\n')
    compiler = out / ('precompile_app.exe' if os.name == 'nt' else 'precompile_app')
    key = fingerprint()
    stamp = out / 'compiler.json'
    cc = shutil.which(cc) or cc
    env = compiler_env(cc)
    cc_id = subprocess.check_output([cc, '--version'], text=True, env=env).splitlines()[0]
    identity = {'engine': key, 'cc': str(Path(cc).resolve()), 'cc_version': cc_id}
    if compiler.exists() and stamp.exists() and json.loads(stamp.read_text()) == identity:
        return compiler, identity
    common = [*FLAGS, '-I', str(QJS), '-I', str(inc), '-I',
              str(ROOT / 'components/pocketjs_guest/include')]
    objects = []
    for name in SOURCES:
        obj = out / (name + '.o')
        # Existing vendor warnings are noisy. Keep incompatible pointer types
        # fatal; our own driver additionally uses Wall/Wextra/Werror.
        subprocess.run([cc, *common, '-w', '-Werror=incompatible-pointer-types',
                        '-c', str(QJS / (name + '.c')), '-o', str(obj)], check=True, env=env)
        objects.append(str(obj))
    subprocess.run([cc, *common, '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'tools/precompile_app.c'), *objects,
                    '-lm', '-o', str(compiler)], check=True, env=env)
    atomic_write(stamp, json.dumps(identity, sort_keys=True).encode())
    return compiler, identity


def compiler_env(cc):
    # GCC's cc1 may live below libexec while runtime DLLs live beside gcc.
    # Only child processes receive this PATH; no global settings change.
    env = os.environ.copy()
    env['PATH'] = str(Path(cc).resolve().parent) + os.pathsep + env.get('PATH', '')
    return env


def generate(source, out, compiler, identity):
    source, out = Path(source).resolve(), Path(out)
    if source.suffix == '.mjs':
        raise ValueError('module precompile is not enabled; retain source loader')
    env = compiler_env(identity.get('cc', compiler))
    abi = json.loads(subprocess.check_output([str(compiler), '--identity'], text=True, env=env))
    contents = source.read_bytes()
    out.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(dir=out.parent, prefix=out.name + '.')
    os.close(fd)
    src_fd, src_name = tempfile.mkstemp(dir=out.parent, prefix=source.name + '.source.')
    try:
        # Compile an immutable snapshot, while keeping the logical filename
        # for debug tables. A concurrent edit must not label old BC with a
        # new source hash or publish a stale build as a successful result.
        with os.fdopen(src_fd, 'wb') as f:
            f.write(contents)
        subprocess.run([str(compiler), src_name, source.name, name], check=True, env=env)
        if source.read_bytes() != contents:
            raise ValueError('source changed during compilation')
        data = Path(name).read_bytes()
        if not data:
            raise ValueError('compiler emitted empty bytecode')
        manifest = {
            'format': 1, 'experimental_only': True, 'kind': 'global',
            'filename': source.name, 'source_sha256': hashlib.sha256(contents).hexdigest(),
            'source_bytes': len(contents), 'bytecode_sha256': digest(name),
            'bytecode_bytes': len(data), 'compiler': identity, 'abi': abi,
        }
        # Manifest is published last. A partial interruption is rejected by
        # verify rather than silently passing stale bytecode to an engine.
        atomic_write(out, data)
        atomic_write(str(out) + '.json', (json.dumps(manifest, sort_keys=True, indent=2) + '\n').encode())
    finally:
        os.unlink(name)
        os.unlink(src_name)


def verify(source, out, expected_abi=None):
    source, out = Path(source), Path(out)
    m = json.loads(Path(str(out) + '.json').read_text())
    if m.get('format') != 1 or not m.get('experimental_only') or m.get('kind') != 'global':
        raise ValueError('unsupported artifact kind or format')
    if source.suffix == '.mjs' or m['filename'] != source.name:
        raise ValueError('source filename/kind mismatch')
    if m['source_sha256'] != digest(source) or m['source_bytes'] != source.stat().st_size:
        raise ValueError('stale source')
    if m['bytecode_sha256'] != digest(out) or m['bytecode_bytes'] != out.stat().st_size:
        raise ValueError('bytecode hash/length mismatch')
    if m['compiler']['engine'] != fingerprint():
        raise ValueError('engine/flags mismatch')
    if expected_abi is not None and m['abi'] != expected_abi:
        raise ValueError('ABI mismatch')
    return m


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--outdir', required=True, type=Path)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('sources', nargs='+', type=Path)
    args = parser.parse_args()
    if len({p.name for p in args.sources}) != len(args.sources):
        parser.error('source base names must be unique')
    if args.verify:
        for source in args.sources:
            verify(source, args.outdir / (source.name + '.bc'))
    else:
        compiler, identity = build_compiler(args.outdir / 'compiler', args.cc)
        for source in args.sources:
            out = args.outdir / (source.name + '.bc')
            generate(source, out, compiler, identity)
            verify(source, out)
    print('PRECOMPILE_EXPERIMENT_OK (firmware still uses source)')


if __name__ == '__main__':
    main()
