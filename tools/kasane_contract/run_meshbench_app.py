"""Compile the real public mesh app on the host; no device or external inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def build_harness(test_source='tools/kasane_contract/test_meshbench_app.c', cache_name='meshbench-host', extra_sources=(), extra_flags=()):
    """Build a public app-level native harness using the same real compositor."""
    out = ROOT/'.cache'/cache_name
    out.mkdir(parents=True, exist_ok=True)
    qjs = ROOT/'components/quickjs-ng/quickjs-ng'
    flags = ['-std=gnu11', '-O2', '-g', '-ffp-contract=off', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-DQUICKJS_NG_BUILD', '-D_GNU_SOURCE', '-DKSN_PROC_HOST_TEST']
    flags += list(extra_flags)
    compiler = subprocess.check_output(['cc', '--version'], text=True)
    key = hashlib.sha256((' '.join(flags)+compiler+''.join(
        hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(qjs.glob('*'))
        if p.suffix in ('.c', '.h'))).encode()).hexdigest()
    includes = ['-I'+str(p) for p in [qjs, ROOT/'main', ROOT/'main/pocket', ROOT/'main/ui',
                ROOT/'main/ui/kasane', ROOT/'main/ayatsuri', ROOT/'main/text', ROOT/'main/hal',
                ROOT/'tools/hostshim', ROOT/'tools/kasane_contract', out]]
    commands = []

    def run(command, **kwargs):
        commands.append(list(map(str, command)))
        return subprocess.run(commands[-1], cwd=ROOT, check=True, **kwargs)

    objects = []
    for name in ['dtoa', 'libregexp', 'libunicode', 'quickjs', 'quickjs-vm']:
        obj = ROOT/'.cache/mesh-shiki-contract'/(key+'-'+name+'.o')
        if not obj.exists():
            obj.parent.mkdir(parents=True, exist_ok=True)
            run(['cc', *flags, *includes, '-w', '-c', qjs/(name+'.c'), '-o', obj])
        objects.append(obj)
    run(['python3', 'tools/make_font.py', out], stdout=subprocess.DEVNULL)
    data = (ROOT/'apps/pet/assets/pets-compact.bin').read_bytes()
    (out/'kasane_pet_test_data.h').write_text('static const uint8_t pet_test_data[] = {'+','.join(map(str, data))+'};\n')
    rom = out/'rom.c'
    rom.write_text('#include "ksn_shiki.h"\nconst ksn_shiki_rom_entry *ksn_shiki_rom_table(unsigned *n){*n=0;return 0;}\n')
    sources = list(dict.fromkeys(re.findall(r'(?:main|tools)/[\w/]+\.c', (ROOT/'tools/build_kasane_test.sh').read_text())))
    sources = [ROOT/p for p in sources if p!='tools/test_pocket_kasane.c']
    sources += [ROOT/'main/pocket/pocket_shape.c', ROOT/'main/pocket/pocket_shiki_api.c',
                ROOT/'main/pocket/app_registry.c', ROOT/test_source, rom]
    sources += [ROOT/p for p in extra_sources]
    binary = out/'meshbench'
    run(['cc', *flags, '-DKASANE_DMA_OVERLAP=1',
         # app_registry.c's local.meshbench row exists only with the Shiki diagnostics.
         '-DCONFIG_POCKET_SHIKI_DEBUG=1', '-Wall', '-Wextra', '-Werror', *includes, *sources, *objects,
         *['-Wl,--wrap='+n for n in ['malloc', 'calloc', 'realloc', 'free']], '-lm', '-o', binary])
    evidence = {'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                'compiler': compiler, 'commands': commands, 'quickjs_cache_key': key,
                'inputs': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources+objects}}
    (out/'commands.json').write_text(json.dumps(evidence, indent=2)+'\n')
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    return binary, out, env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-lsan', action='store_true', help='Disable LSan only for hosts where ptrace prevents it; ASan/UBSan and allocation ledgers remain enabled')
    parser.add_argument('--request-ceiling', type=int, default=16384, help='Maximum startup guest allocation request in bytes (0: unlimited); a host stressor, not target RAM')
    args = parser.parse_args()
    if args.request_ceiling < 0:
        parser.error('--request-ceiling must be nonnegative')
    subprocess.run(['node', 'apps/meshbench/generate.cjs', '--check'], cwd=ROOT, check=True)
    binary, out, env = build_harness()
    if args.no_lsan:
        env['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
        print('LSAN_DISABLED host restriction; requested allocation release is checked separately', flush=True)
    env['MESHBENCH_REQUEST_CEILING'] = str(args.request_ceiling)
    digests = []
    for rows, image_rows, delayed in [(r, i, d) for r in [4, 8] for i in [0, 1] for d in [0, 1]]:
        try:
            result = subprocess.run(list(map(str, [binary, rows, image_rows, delayed])), cwd=ROOT,
                                    check=True, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except subprocess.CalledProcessError as error:
            (out/f'rows{rows}-direct{image_rows}-delayed{delayed}.log').write_text(error.stdout or '')
            print(error.stdout or '', end='')
            raise
        (out/f'rows{rows}-direct{image_rows}-delayed{delayed}.log').write_text(result.stdout)
        print(result.stdout, end='')
        digests.append(re.search(r'digest=([0-9a-f]+)', result.stdout)[1])
    assert len(set(digests))==1, digests
    print('MESH_BENCH_ALL_ARMS_PASS exact panels; host counters are stubs, not target cycles')


if __name__ == '__main__':
    main()
