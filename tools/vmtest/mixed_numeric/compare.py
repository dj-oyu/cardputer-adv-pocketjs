#!/usr/bin/env python3
"""Pinned A/B semantics and real BIG WAVE parity. No device or speed claims."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
BASE = 'e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267'
QJS = ROOT / 'components/quickjs-ng/quickjs-ng'
sys.path.insert(0, str(ROOT / 'tools/games'))
import run_big_wave as bw


def command(args, **kwargs):
    return subprocess.run(args, cwd=ROOT, check=True, **kwargs)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1048576), b''):
            h.update(chunk)
    return {'bytes': path.stat().st_size, 'sha256': h.hexdigest()}


def equal_files(a, b):
    with a.open('rb') as x, b.open('rb') as y:
        while True:
            left, right = x.read(1048576), y.read(1048576)
            assert left == right, (a, b)
            if not left:
                return digest(a)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--m32', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    assert not (args.m32 and args.sanitize)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    flags = ['-Os', '-g']
    if args.m32:
        flags += ['-m32', '-malign-double']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    result = {'base': BASE, 'flags': flags, 'heap_limit': 163840 if args.m32 else 524288,
              'host_model_only': True, 'commands': [], 'variants': {}}

    def compile_c(source, obj):
        cmd = ['gcc', '-std=gnu11', '-w', *flags, '-DQUICKJS_NG_BUILD', '-D_GNU_SOURCE',
               '-I' + str(QJS), '-Icomponents/pocketjs_guest/include',
               '-c', str(source), '-o', str(obj)]
        result['commands'].append(cmd)
        command(cmd)

    shared = []
    for name in ('dtoa', 'libregexp', 'libunicode', 'quickjs-vm'):
        obj = out / (name + '.o')
        compile_c(QJS / (name + '.c'), obj)
        shared.append(str(obj))
    command(['python3', 'tools/make_font.py', str(out)], stdout=subprocess.DEVNULL)
    pets = (ROOT / 'apps/pet/assets/pets-compact.bin').read_bytes()
    (out / 'kasane_pet_test_data.h').write_text(
        'static const uint8_t pet_test_data[] = {' + ','.join(map(str, pets)) + '};\n')
    api = (ROOT / 'main/pocket/pocket_api.c').read_text()
    begin = api.index('// ----------------------------------------------------------- subscriptions')
    end = api.index('// ------------------------------------------------------- async completions', begin)
    (out / 'pocket_sub_impl.inc').write_text(api[begin:end])
    harness = (ROOT / 'tools/games/test_big_wave_host.c').read_text()
    needle = '    else pocket_kasane_animations_presented(now);'
    assert harness.count(needle) == 1
    harness = harness.replace(needle, needle + '''
    /* Compatibility capture after every existing full-app present. */
    static FILE *numeric_panels;
    if (!numeric_panels) { numeric_panels=fopen(getenv("NUMERIC_PANELS"),"wb"); if(!numeric_panels)abort(); }
    if(fwrite(panel,sizeof panel,1,numeric_panels)!=1)abort();
''')
    (out / 'app.c').write_text(harness)
    baseline = subprocess.check_output(
        ['git', 'show', BASE + ':components/quickjs-ng/quickjs-ng/quickjs.c'], cwd=ROOT)
    sources = {'base': baseline, 'candidate': (QJS / 'quickjs.c').read_bytes()}
    for mode, source in sources.items():
        folder = out / mode
        folder.mkdir(exist_ok=True)
        (folder / 'quickjs.c').write_bytes(source)
        obj = folder / 'quickjs.o'
        compile_c(folder / 'quickjs.c', obj)
        objects = [str(obj), *shared]
        numeric = folder / 'numeric'
        cmd = ['gcc', '-std=gnu11', *flags, '-I' + str(QJS),
               str(Path(__file__).with_name('numeric.c')), *objects, '-lm', '-o', str(numeric)]
        result['commands'].append(cmd)
        command(cmd)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        with (folder / 'numeric.log').open('w') as log:
            command([str(numeric)], stdout=log, env=env, timeout=60)
        # Record actual ELF class: 1 = 32 bit, 2 = 64 bit.
        elf_class = numeric.read_bytes()[4]
        assert elf_class == (1 if args.m32 else 2)
        includes = ['-I' + str(x) for x in (QJS, ROOT / 'tools/hostshim', ROOT / 'main',
                    ROOT / 'main/pocket', ROOT / 'main/ui', ROOT / 'main/ui/kasane',
                    ROOT / 'main/text', ROOT / 'main/hal', out)]
        app = folder / 'app'
        cmd = ['gcc', '-std=gnu11', *flags, '-DKSN_PROC_POINTS_PIE_MODEL', '-Wall', '-Wextra',
               '-Werror', *includes, str(out / 'app.c'), *bw.kasane_sources(), *bw.EXTRA,
               *objects, '-Wl,--wrap=calloc', '-Wl,--wrap=free', '-lm', '-o', str(app)]
        result['commands'].append(cmd)
        command(cmd)
        env.update(ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                   BW_HEAP_LIMIT=str(result['heap_limit']), NUMERIC_PANELS=str(folder / 'panels.bin'))
        with (folder / 'app.log').open('w') as log:
            command([str(app), 'apps/bigwave/big_wave.js'], stdout=log, env=env, timeout=300)
        assert 'BIGWAVE_HOST PASS' in (folder / 'app.log').read_text()
        result['variants'][mode] = {'source_sha256': hashlib.sha256(source).hexdigest(),
                                    'elf_class': elf_class}
    for name in ('numeric.log', 'app.log', 'panels.bin'):
        result[name] = equal_files(out / 'base' / name, out / 'candidate' / name)
    result['panels'] = result['panels.bin']['bytes'] // (240 * 135 * 2)
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'commands'}, indent=2))


if __name__ == '__main__':
    main()
