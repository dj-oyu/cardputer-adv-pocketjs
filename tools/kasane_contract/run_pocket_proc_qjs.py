"""Compile and run the real QuickJS frame adapter (pocket_proc.c through
pocket.kasane.shiki) on the host: test_pocket_proc_qjs.c, under ASan/UBSan.

The float procedural VM and its PIE point batches went in Shiki v2.0, so the
fake-PIE arm, the MEGADEMO chunks and the 48-frame --out manifest went with
them; the frames are now Shiki programs (see the test's header)."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[2]
QJS=ROOT/'components/quickjs-ng/quickjs-ng'
OUT=ROOT/'.cache/kasane_pocket_proc_qjs'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--opt',choices=('0','1','2'),default='1',help='host compiler optimization level')
    args=parser.parse_args()
    cc=shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not cc:raise SystemExit('No host C compiler found')
    OUT.mkdir(parents=True,exist_ok=True)
    binary=OUT/('test_pocket_proc_qjs_O'+args.opt)
    # No app table: the empty one the firmware links when no app has @shiki.
    rom=OUT/'ksn_shiki_rom_none.c'
    rom.write_text('#include "ksn_shiki.h"\n'
                   'const ksn_shiki_rom_entry *ksn_shiki_rom_table(unsigned *count){*count=0;return 0;}\n')
    sources=[*(QJS/name for name in ('dtoa.c','libregexp.c','libunicode.c','quickjs.c','quickjs-vm.c')),
             *(ROOT/'main/ui/kasane'/name for name in ('ksn_procedural.c','ksn_core.c',
               'ksn_render.c','ksn_blend_pie.c','ksn_shiki.c')),
             ROOT/'main/pocket/pocket_proc.c',ROOT/'main/pocket/shiki_heap.c',
             ROOT/'main/pocket/pocket_shiki_api.c',
             # pocket.shapes' native line API is pocket_proc.c's; nothing
             # here draws through it, the weak stand-ins only satisfy the link
             # for a pocket_proc.c that lacks it.
             ROOT/'tools/kasane_contract/proc_native_weak.c',rom,
             ROOT/'tools/kasane_contract/test_pocket_proc_qjs.c']
    command=[cc,'-std=gnu11','-O'+args.opt,'-g','-fsanitize=address,undefined','-fno-sanitize-recover',
             '-DQUICKJS_NG_BUILD','-D_GNU_SOURCE','-DKSN_PROC_HOST_TEST',
             '-I',str(QJS),'-I',str(ROOT/'main'),'-I',str(ROOT/'main/pocket'),
             '-I',str(ROOT/'main/ui/kasane'),'-I',str(ROOT/'tools/kasane_contract'),
             '-I',str(ROOT/'tools/hostshim'),
             *(str(s) for s in sources),'-lm','-o',str(binary)]
    subprocess.run(command,cwd=ROOT,check=True)
    env=os.environ.copy();env.setdefault('ASAN_OPTIONS','detect_leaks=1')
    subprocess.run([str(binary)],cwd=ROOT,env=env,check=True)

if __name__=='__main__':main()
