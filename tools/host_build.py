"""Dependency-checked host objects shared by local builds and CI.

Every invocation rescans dependencies (including system and generated headers),
checks their contents, and links a fresh executable. Only compiler outputs are
reused. An installed ccache is used for -c invocations with its safe defaults.
"""
from __future__ import annotations

import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.cache/host-objects-v1'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def compile_object(compiler, source, flags):
    """Return an object for exactly this compiler, environment, flags and inputs."""
    compiler = str(Path(shutil.which(compiler) or compiler).resolve())
    source = str(Path(source).resolve())
    toolchain = []
    for tool in ('cc1', 'as'):
        path = subprocess.check_output([compiler, '-print-prog-name=' + tool], text=True).strip()
        resolved = Path(shutil.which(path) or path)
        toolchain.append((str(resolved), digest(resolved)))
    identity = [digest(__file__), compiler, digest(compiler), toolchain, source, flags, str(Path.cwd()),
                {k: os.environ.get(k) for k in (
                    'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'GCC_EXEC_PREFIX',
                    'COMPILER_PATH', 'LIBRARY_PATH', 'SOURCE_DATE_EPOCH', 'LANG', 'LC_ALL')}]
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    folder = CACHE / key
    folder.mkdir(parents=True, exist_ok=True)
    obj = folder / 'unit.o'
    meta = folder / 'inputs.json'
    # Different test lanes/processes may request the same translation unit.
    with (folder / 'lock').open('w') as lock, tempfile.TemporaryDirectory(dir=folder) as tmp:
        fcntl.flock(lock, fcntl.LOCK_EX)
        depfile = Path(tmp) / 'unit.d'
        # Rescan instead of trusting an old depfile: a newly added header earlier
        # on an -I path must invalidate an object too. -M includes system headers.
        subprocess.run([compiler, *flags, '-M', '-MF', str(depfile), '-MT', 'unit', source],
                       check=True)
        dependencies = shlex.split(depfile.read_text().split(':', 1)[1].replace('\\\n', ''))
        inputs = {str(Path(p).resolve()): digest(p) for p in dependencies}
        volatile = any(token in Path(p).read_bytes() for p in dependencies
                       for token in (b'__DATE__', b'__TIME__', b'__TIMESTAMP__'))
        if not volatile and obj.exists() and meta.exists():
            previous = json.loads(meta.read_text())
            if previous.get('inputs') == inputs and previous.get('object') == digest(obj):
                return obj
        temporary = Path(tmp) / 'unit.o'
        launcher = [shutil.which('ccache')] if shutil.which('ccache') else []
        env = {k: v for k, v in os.environ.items()
               if not k.startswith('CCACHE_') or k in ('CCACHE_DIR', 'CCACHE_TEMPDIR', 'CCACHE_MAXSIZE')}
        env.update(CCACHE_COMPILERCHECK='content', CCACHE_SLOPPINESS='',
                   CCACHE_NODIRECT='1', CCACHE_NODEPEND='1')
        subprocess.run([*launcher, compiler, *flags, '-MD', '-MF', str(depfile),
                        '-c', source, '-o', str(temporary)], check=True, env=env)
        # Record actual compile dependencies, rather than assuming the scan and
        # compilation saw identical inputs. All generators finish before callers.
        dependencies = shlex.split(depfile.read_text().split(':', 1)[1].replace('\\\n', ''))
        inputs = {str(Path(p).resolve()): digest(p) for p in dependencies}
        temporary.replace(obj)
        meta.write_text(json.dumps({'inputs': inputs, 'object': digest(obj)}, sort_keys=True))
    return obj


def build(command, *, cwd=None, **kwargs):
    """Compile each .c separately, then always execute the original link command.

    Accepts the gcc/cc compile-and-link argument lists used by the host harnesses.
    Compile-only invocations also refresh the requested output from a checked
    object; callers can alternatively use compile_object directly. Compiler/linker flags and object order survive.
    """
    if cwd is not None and Path(cwd).resolve() != Path.cwd().resolve():
        raise ValueError('host_build must run from the build working directory')
    compiler, *args = map(str, command)
    sources = [a for a in args if a.endswith('.c')]
    if not sources:
        return subprocess.run(command, cwd=cwd, check=True, **kwargs)
    flags = []
    skip = False
    for arg in args:
        if skip:
            skip = False
            continue
        if arg in ('-o', '-L'):
            skip = True
        elif (arg != '-c' and arg not in sources and not arg.endswith(('.o', '.a', '.so'))
              and not arg.startswith(('-Wl,', '-l', '-L'))):
            flags.append(arg)
    objects = {s: str(compile_object(compiler, s, flags)) for s in sources}
    if '-c' in args:
        if len(sources) != 1 or '-o' not in args:
            raise ValueError('compile-only cache requires one source and explicit -o')
        shutil.copyfile(objects[sources[0]], args[args.index('-o') + 1])
        return subprocess.CompletedProcess(command, 0)
    return subprocess.run([compiler, *(objects.get(a, a) for a in args)], cwd=cwd,
                          check=True, **kwargs)


if __name__ == '__main__':
    try:
        build(sys.argv[1:])
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
