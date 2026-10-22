"""Verify the host experiment and the synchronous product runtime connection.

One acceptance entry runs native O2 + ASan/UBSan; the full suite also invokes
--m32 for the device-width ABI. This does not select fast/full acceptance.
Only compiler objects use the common dependency-aware cache. Every fixture,
report and linked executable is regenerated/revalidated.
"""
from pathlib import Path
import argparse
import hashlib
import fcntl
import json
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = ROOT / 'tools/vmtest/prepare'
QJS = ROOT / 'components/quickjs-ng/quickjs-ng'
OUT = ROOT / '.cache/prepare-api'
KASANE = ROOT / 'main/ui/kasane'
SOURCES = [HERE/'bytecode.c', HERE/'probe.c',
           *(QJS/f'{n}.c' for n in ('dtoa','libregexp','libunicode','quickjs-vm')),
           *(KASANE/f'{n}.c' for n in ('ksn_procedural','ksn_proc_analysis',
             'ksn_proc_plan','ksn_proc_plan_points','ksn_proc_points',
             'ksn_proc_points_dispatch','ksn_proc_points_pie'))]
# Expected allocations/effects are observations of the unchanged generic
# C API. Candidate size is additionally compared inside the C harness.
CASES = {
    'known': (1, 0, False, 1),
    'zero': (1, 0, False, 1),
    'optional-null': (1, 0, False, 1),
    'points': (0, 0, False, 2),
    'unknown-name': (0, 0, True, 0),
    'nul-name': (0, 0, True, 0),
    'bigint': (0, 0, True, 1),
    'template': (0, 1, False, 1),
    'variable': (1, 0, False, 1),
    'unknown': (0, 1, False, 0),
    'replaced': (1, 1, False, 0),
    'arity': (0, 0, True, 0),
    'count': (0, 0, True, 0),
    'side-effect': (0, 1, False, 1),
    'dynamic': (0, 0, False, 1),
    'lookalike': (0, 1, False, 0),
    'branch': (0, 1, False, 0),
    'spread': (0, 0, False, 1),
    'closure': (1, 0, False, 1),
    'domain': (0, 0, True, 1),
    'getter': (1, 1, False, 1),
    'types': (0, 0, True, 1),
}

def run(binary, path, exercise=True, compare=False):
    r = subprocess.run([str(binary), str(path), *(['--exercise'] if exercise else []), *(['--compare-plan'] if compare else [])],
                       cwd=ROOT, text=True, capture_output=True)
    if r.returncode:
        raise AssertionError(f'{path.name}: exit {r.returncode}\n{r.stdout}\n{r.stderr}')
    return json.loads(r.stdout)

def verify(binary, pointer_bytes):
    results = {}
    for name, (candidates, effects, threw, allocations) in CASES.items():
        d = run(binary, HERE/'fixtures'/f'{name}.js', compare=name in ('known','zero','optional-null','variable','closure'))
        assert d['pointer_bytes'] == pointer_bytes
        assert d['jsvalue_bytes'] == 2*pointer_bytes
        assert 0 < d['abstract_scratch_bytes'] <= 8192
        assert d['runtime_connected'] is False and d['default_route'] == 'generic'
        assert d['preparation_effects'] == 0 and d['identity_tests']
        assert not d['bounded_fallback']
        planned = [c for c in d['calls'] if c['route'] == 'guarded-candidate']
        assert len(planned) == candidates, (name, d)
        assert d['runtime_effects'] == effects, (name, d)
        assert d['generic_threw'] == threw, (name, d)
        assert d['native_allocations'] == allocations, (name, d)
        for c in planned:
            assert c['minimum_bytes'] == max(c['sizeof_floor'], c['args_offset']+4*c['params'])
            assert c['element_bytes'] == 4
            assert c['element_alignment'] == 4
            assert len(c['literal_float32_bits']) == c['params']
            if name == 'known':
                assert c['literal_float32_bits'] == ['3f800000','c0200000']
            assert c['alignment'] in (4, 8)
            assert c['guard'] == 'captured-callee-and-registry-identity'
            if pointer_bytes == 4:
                assert c['args_offset'] == c['sizeof_floor'] == 40
                assert c['minimum_bytes'] == 40+4*c['params']
        results[name] = d
    r = subprocess.run([str(binary), str(HERE/'fixtures/syntax.js')],
                       cwd=ROOT, text=True, capture_output=True)
    assert r.returncode == 2 and json.loads(r.stdout)['compile_failed']
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)/'bounded.js'
        path.write_text('unknown();\n'*40)
        d = run(binary, path)
        assert d['bounded_fallback'] and not d['calls'] and d['runtime_effects'] == 40
        path.write_text('unknown('+','.join('1' for _ in range(70))+');')
        d = run(binary, path)
        assert d['bounded_fallback'] and not d['calls'] and d['runtime_effects'] == 1
        path.write_text('\n'.join(f'function f{i}(){{}}' for i in range(20)))
        d = run(binary, path)
        assert d['bounded_fallback'] and not d['calls']
        path.write_text('/*'+'x'*16384+'*/')
        d = run(binary, path, False)
        assert d['bounded_fallback'] and not d['calls']
    return results

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--m32', action='store_true')
    args = p.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    abi = []
    if args.m32:
        env = os.environ | {'M32_SYSROOT': str(ROOT/'.cache/kasane_megademo_app/m32sys')}
        lockpath = ROOT/'.cache/kasane_megademo_app/m32-prepare.lock'
        lockpath.parent.mkdir(parents=True, exist_ok=True)
        with lockpath.open('a+') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            r = subprocess.run(['bash','tools/vmtest/m32_sysroot.sh'], cwd=ROOT,
                               env=env, text=True, capture_output=True, check=True)
        import shlex
        abi = shlex.split(r.stdout)
    variants = [('m32-O2' if args.m32 else 'native-O2', ['-O2','-fstrict-aliasing'])]
    if not args.m32:
        variants.append(('native-asan', ['-O1','-g','-fsanitize=address,undefined',
                                         '-fno-omit-frame-pointer']))
    lock = (OUT/('m32.lock' if args.m32 else 'native.lock')).open('a+')
    fcntl.flock(lock, fcntl.LOCK_EX)
    report = {}
    runtime_supported = (QJS/'quickjs-prepared.h').exists()
    table = OUT/'runtime-rom.c'
    if runtime_supported:
        subprocess.run(['node', str(ROOT/'tools/kasane_ir/emit_rom_plans.mjs'),
                        str(table), f"derby={ROOT/'apps/derby/derby_prog.js'}"],
                       cwd=ROOT, check=True)
    for name, options in variants:
        binary = OUT/name
        flags = ['-std=gnu11', *abi, *options, '-DQUICKJS_NG_BUILD', '-D_GNU_SOURCE',
                 '-DKSN_PROC_HOST_TEST', '-DPOCKET_PREPARED_TEST', '-Wno-unused-parameter',
                 *[x for d in (QJS, ROOT/'main', ROOT/'main/pocket', KASANE,
                                ROOT/'tools/hostshim', HERE) for x in ('-I',str(d))]]
        command = [sys.executable, str(ROOT/'tools/host_build.py'), 'cc', *flags,
                   *map(str,SOURCES), '-lm', '-o', str(binary)]
        subprocess.run(command, cwd=ROOT, check=True)
        import struct
        report[name] = verify(binary, 4 if args.m32 else struct.calcsize('P'))
        print(f'PREPARE_CONTRACT {name} PASS: real parser, identity guards, layout/generic parity, side effects, bounded fallback', flush=True)
        if runtime_supported:
            runtime_binary = OUT/(name+'-runtime')
            runtime_sources = [s for s in SOURCES if s != HERE/'probe.c'] + [HERE/'runtime.c', table]
            subprocess.run([sys.executable, str(ROOT/'tools/host_build.py'), 'cc',
                            *flags, *map(str,runtime_sources), '-lm', '-o', str(runtime_binary)],
                           cwd=ROOT, check=True)
            subprocess.run([str(runtime_binary), str(HERE/'fixtures/runtime-startup.js')],
                           cwd=ROOT, check=True)
    metadata = {'source_head': subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                'runtime_connected': runtime_supported, 'cases': report,
                'inputs': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in [HERE/'run.py',HERE/'bytecode.c',HERE/'bytecode.h',HERE/'probe.c',
                                     ROOT/'main/pocket/pocket_proc.c',KASANE/'ksn_proc_plan.c',
                                     KASANE/'ksn_proc_plan.h',QJS/'quickjs.c',QJS/'quickjs-opcode.h',
                                     *([HERE/'runtime.c', HERE/'fixtures/runtime-startup.js',
                                        QJS/'quickjs-prepared.h', QJS/'quickjs-prepared.inc',
                                        ROOT/'tools/kasane_ir/emit_rom_plans.mjs']
                                       if runtime_supported else [])]}}
    results = Path(os.environ['CONTRACT_LOG_DIR'])/'prepare' if 'CONTRACT_LOG_DIR' in os.environ else OUT
    results.mkdir(parents=True, exist_ok=True)
    (results/('m32-results.json' if args.m32 else 'native-results.json')).write_text(json.dumps(metadata,indent=2)+'\n')

if __name__ == '__main__':
    main()
