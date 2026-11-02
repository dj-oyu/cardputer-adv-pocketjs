"""Synthetic-only opt-in product backend contracts. No character assets/device."""
from pathlib import Path
import os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
def run(args):subprocess.run([str(a) for a in args],cwd=ROOT,check=True)
flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-ffp-contract=off','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',ROOT/'main']
os.environ.setdefault('ASAN_OPTIONS','detect_leaks=1:abort_on_error=1')
os.environ.setdefault('UBSAN_OPTIONS','halt_on_error=1:print_stacktrace=1')
with tempfile.TemporaryDirectory(prefix='mesh-depth-contract-') as out:
    binary=Path(out)/'compact'
    run([os.environ.get('CC','cc'),*flags,ROOT/'main/ayatsuri/compact_opaque.c',ROOT/'tools/kasane_contract/test_mesh_compact.c','-lm','-o',binary]);run([binary])
    run(['node','tools/ayatsuri/model/test_pack_mesh.cjs'])
    binary=Path(out)/'view'
    run([os.environ.get('CC','cc'),*flags,ROOT/'main/ayatsuri/ayatsuri_model.c',ROOT/'main/ayatsuri/ayatsuri_visibility.c',ROOT/'tools/ayatsuri/model/test_model_mesh.c','-lm','-o',binary]);run([binary,'.cache/ayatsuri-native/mesh.aynm'])
run(['python3','tools/kasane_contract/run_pocket_proc_qjs.py','--mesh-depth-only'])
