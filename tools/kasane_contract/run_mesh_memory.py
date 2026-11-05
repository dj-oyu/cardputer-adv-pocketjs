"""Public synthetic mesh output/lifetime probe; 64-bit host requested bytes only.

No device, download or private input. The counter wraps allocation calls in the
linked objects, not libc/sanitizer internals. It excludes allocator overhead,
static storage (including the probe ledger), stack and device layout changes.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    out = ROOT / '.cache/mesh-shiki-contract'
    out.mkdir(parents=True, exist_ok=True)
    qjs = ROOT / 'components/quickjs-ng/quickjs-ng'
    flags = ['-std=gnu11', '-O2', '-g', '-ffp-contract=off',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-DQUICKJS_NG_BUILD', '-D_GNU_SOURCE', '-DKSN_PROC_HOST_TEST']
    includes = ['-I'+str(p) for p in [qjs, ROOT/'main', ROOT/'main/pocket',
                ROOT/'main/ui/kasane', ROOT/'main/ayatsuri',
                ROOT/'tools/kasane_contract', ROOT/'tools/hostshim']]
    compiler = subprocess.check_output(['cc', '--version'], text=True)
    key = hashlib.sha256((' '.join(flags)+compiler+''.join(
        hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(qjs.glob('*'))
        if p.suffix in ('.c', '.h'))).encode()).hexdigest()
    commands = []

    def run(command):
        commands.append(list(map(str, command)))
        subprocess.run(commands[-1], cwd=ROOT, check=True)

    objects = []
    for name in ['dtoa', 'libregexp', 'libunicode', 'quickjs', 'quickjs-vm']:
        obj = out / (key+'-'+name+'.o')
        if not obj.exists():
            run(['cc', *flags, *includes, '-w', '-c', qjs/(name+'.c'), '-o', obj])
        objects.append(obj)
    rom = out/'memory-rom.c'
    rom.write_text('#include "ksn_shiki.h"\nconst ksn_shiki_rom_entry *ksn_shiki_rom_table(unsigned *n){*n=0;return 0;}\n')
    sources = [ROOT/'main/ui/kasane'/n for n in ['ksn_shiki_frame.c', 'ksn_core.c',
               'ksn_render.c', 'ksn_blend_pie.c', 'ksn_shiki.c', 'ksn_mesh_depth.c']]
    sources += [ROOT/'main/ayatsuri'/n for n in ['ayatsuri_model.c',
                'ayatsuri_visibility.c', 'compact_opaque.c']]
    sources += [ROOT/'main/pocket'/n for n in ['pocket_shiki_frame.c', 'pocket_shiki_api.c', 'shiki_heap.c']]
    sources += [ROOT/'tools/kasane_contract/proc_native_weak.c', rom,
                ROOT/'tools/kasane_contract/test_mesh_memory_qjs.c']
    binary = out/'mesh-memory'
    run(['cc', *flags, *includes, *sources, *objects,
         *['-Wl,--wrap='+n for n in ['malloc', 'calloc', 'realloc', 'free']],
         '-lm', '-o', binary])
    evidence = {'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                'compiler': compiler, 'commands': commands,
                'quickjs_cache_key': key,
                'inputs': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in sources+objects}}
    (out/'memory-command.json').write_text(json.dumps(evidence, indent=2)+'\n')
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    result = subprocess.run([str(binary)], cwd=ROOT, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out/'memory-result.log').write_text(result.stdout)
    print(result.stdout, end='')
    result.check_returncode()


if __name__ == '__main__':
    main()
