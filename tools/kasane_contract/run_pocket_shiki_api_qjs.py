"""pocket.kasane.shiki on the host: the device's own suite
(apps/shikitest/shiki_api_test.js) over real QuickJS and the real C files,
under ASan/UBSan. Prints SHIKI API PASS or fails."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
QJS=ROOT/'components/quickjs-ng/quickjs-ng'
OUT=ROOT/'.cache/kasane_pocket_shiki_api_qjs'

def main():
    cc=shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not cc:raise SystemExit('No host C compiler found')
    OUT.mkdir(parents=True,exist_ok=True)
    binary=OUT/'test_pocket_shiki_api_qjs'
    # No app table here: the empty one the firmware links when no app has @shiki.
    rom=OUT/'ksn_shiki_rom_none.c'
    rom.write_text('#include "ksn_shiki.h"\n'
                   'const ksn_shiki_rom_entry *ksn_shiki_rom_table(unsigned *count){*count=0;return 0;}\n')
    sources=[*(QJS/name for name in ('dtoa.c','libregexp.c','libunicode.c','quickjs.c','quickjs-vm.c')),
             *(ROOT/'main/ui/kasane'/name for name in ('ksn_procedural.c','ksn_proc_analysis.c','ksn_proc_plan.c',
               'ksn_proc_plan_points.c','ksn_proc_points.c','ksn_proc_points_pie.c','ksn_proc_points_dispatch.c',
               'ksn_core.c','ksn_render.c','ksn_blend_pie.c','ksn_shiki.c')),
             ROOT/'main/pocket/pocket_proc.c',ROOT/'main/pocket/shiki_heap.c',ROOT/'main/pocket/pocket_shiki_api.c',
             ROOT/'tools/kasane_contract/proc_native_weak.c',rom,
             ROOT/'tools/kasane_contract/test_pocket_shiki_api_qjs.c']
    command=[cc,'-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover',
             '-DQUICKJS_NG_BUILD','-D_GNU_SOURCE','-DKSN_PROC_HOST_TEST',
             '-I',str(QJS),'-I',str(ROOT/'main'),'-I',str(ROOT/'main/pocket'),
             '-I',str(ROOT/'main/ui/kasane'),'-I',str(ROOT/'tools/kasane_contract'),
             '-I',str(ROOT/'tools/hostshim'),
             *(str(s) for s in sources),'-lm','-o',str(binary)]
    subprocess.run(command,cwd=ROOT,check=True)
    env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1'
    r=subprocess.run([str(binary),str(ROOT/'apps/shikitest/shiki_api_test.js')],cwd=ROOT,env=env)
    print('SHIKI API PASS' if r.returncode==0 else 'SHIKI API FAIL')
    sys.exit(r.returncode)

if __name__=='__main__':main()
