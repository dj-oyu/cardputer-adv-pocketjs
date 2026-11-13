"""Run the same DQ oracle against the experiment or an explicit product checkout.

python3 run.py NEW_OUTPUT [--product-root CHECKOUT]
Product mode never adds the test-only support header to the compiler search path.
No checkout is modified; all generated files and binaries live in NEW_OUTPUT.
"""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=pathlib.Path)
parser.add_argument('--product-root', type=pathlib.Path)
args = parser.parse_args()
here = pathlib.Path(__file__).resolve().parent
test_root = here.parents[3]
root = args.product_root.resolve() if args.product_root else test_root
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
product = args.product_root is not None
if product and not (root / 'main/ayatsuri/ayatsuri_skin_dq.h').is_file():
    raise SystemExit('Product checkout must contain main/ayatsuri/ayatsuri_skin_dq.h')

subprocess.run(['node', str(root / 'tools/ayatsuri/skin/render_fixture.cjs'),
                str(out / 'fixture')], check=True)
sources = [root / p for p in [
    'main/ayatsuri/ayatsuri_model.c', 'main/ayatsuri/ayatsuri_visibility.c',
    'main/ayatsuri/compact_opaque.c', 'main/ui/kasane/ksn_shiki_frame.c',
    'main/ui/kasane/ksn_mesh_depth.c']]
test_inputs = list(here.glob('*.c')) + [here / 'run.py']
if not product:
    test_inputs += list((here / 'support').rglob('*.h'))
product_inputs = sources + list((root / 'main/ayatsuri').glob('*.h'))
product_inputs += list((root / 'main/ui/kasane').glob('*.h'))
product_inputs += list((root / 'tools/hostshim').glob('*.h'))
product_inputs += [root / p for p in [
    'tools/ayatsuri/skin/test_render.c', 'tools/ayatsuri/skin/render_fixture.cjs',
    'tools/ayatsuri/model/pack_model.cjs']]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hashes():
    return {**{'test/' + str(p.relative_to(test_root)): sha(p) for p in test_inputs},
            **{'source/' + str(p.relative_to(root)): sha(p) for p in product_inputs}}


def head(path):
    return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()


before = hashes()
commands, logs = [], {}
includes = [root / 'main', root / 'main/ayatsuri', root / 'tools/hostshim',
            root / 'tools/ayatsuri/skin']
if not product:
    includes = [here / 'support', here / 'support/ayatsuri'] + includes
for mode, flags in [('o2', ['-O2']),
                    ('san', ['-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
    for test in ['core', 'native']:
        exe = out / (test + '-' + mode)
        cmd = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffp-contract=off',
               '-fno-fast-math', *flags, *['-I' + str(p) for p in includes],
               str(here / ('test_' + test + '.c'))]
        if test == 'native':
            cmd += list(map(str, sources)) + ['-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free']
        cmd += ['-H', '-lm', '-o', str(exe)]
        commands.append(cmd)
        compiled = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / (exe.name + '.compile.log')).write_text(compiled.stdout)
        compiled.check_returncode()
        run_args = [str(exe)] + ([str(out / 'fixture')] if test == 'native' else [])
        log = subprocess.check_output(run_args, text=True,
                                      env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
        print(log, end='')
        logs[exe.name] = log
        (out / (exe.name + '.log')).write_text(log)
assert hashes() == before, 'Test or source files changed during the run'
(out / 'report.json').write_text(json.dumps({
    'mode': 'product-headers' if product else 'experimental-copy',
    'testHead': head(test_root), 'sourceHead': head(root),
    'sourceSha256': before,
    'fixtureSha256': {p.name: sha(p) for p in (out / 'fixture').glob('*.aynm')},
    'commands': commands, 'logs': logs,
    'scope': 'same oracle/core/native mesh only; shared renderer; no JS/admission/OOM/DMA/device; LSan disabled'
}, indent=2) + '\n')
