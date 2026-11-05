"""Build and run the frame turn contract (test_pocket_proc_turn_qjs.c) on
real QuickJS.

QuickJS is compiled once without sanitizers and cached (vendor code, and its
objects are reused between runs). pocket_shiki_frame.c, the Shiki API, the Shiki
heap's fallback, the engine and the test are compiled with ASan/UBSan and
proc_alloc_hook.h force-included, so their malloc/calloc can be failed and
measured and LeakSanitizer decides whether reset() releases every frame and
plan exactly once. (Before Shiki v2.0 this ran beside the float VM's limits
contract as run_pocket_proc_limits_qjs.py.)
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
QJS=ROOT/'components/quickjs-ng/quickjs-ng'
OUT=ROOT/'.cache/kasane_pocket_proc_turn_qjs'
DEFS=['-DQUICKJS_NG_BUILD','-D_GNU_SOURCE']

def main():
    cc=shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not cc:raise SystemExit('No host C compiler found')
    OUT.mkdir(parents=True,exist_ok=True)
    sanitize=[] if sys.platform=='win32' else ['-fsanitize=address,undefined',
                                               '-fno-omit-frame-pointer']
    objects=[]
    for name in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):
        src=QJS/f'{name}.c';obj=OUT/f'{name}.o'
        if not obj.exists() or src.stat().st_mtime>obj.stat().st_mtime:
            subprocess.run([cc,'-std=gnu11','-c','-O1','-g','-w',*DEFS,'-I',str(QJS),
                            str(src),'-o',str(obj)],cwd=ROOT,check=True)
        objects.append(str(obj))
    kasane=ROOT/'main/ui/kasane'
    # No app table: the empty one the firmware links when no app has @shiki.
    rom=OUT/'ksn_shiki_rom_none.c'
    rom.write_text('#include "ksn_shiki.h"\n'
                   'const ksn_shiki_rom_entry *ksn_shiki_rom_table(unsigned *count){*count=0;return 0;}\n')
    sources=[kasane/'ksn_shiki_frame.c',kasane/'ksn_shiki.c',
             ROOT/'main/pocket/pocket_shiki_frame.c',ROOT/'main/pocket/shiki_heap.c',
             ROOT/'main/pocket/pocket_shiki_api.c',rom,
             ROOT/'tools/kasane_contract/test_pocket_proc_turn_qjs.c']
    binary=OUT/'test_pocket_proc_turn'
    subprocess.run([cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',*sanitize,*DEFS,
                    '-DKSN_PROC_HOST_TEST','-I',str(QJS),'-I',str(ROOT/'main'),
                    '-I',str(ROOT/'main/pocket'),'-I',str(kasane),
                    '-I',str(ROOT/'tools/kasane_contract'),'-I',str(ROOT/'tools/hostshim'),
                    '-include',str(ROOT/'tools/kasane_contract/proc_alloc_hook.h'),
                    *(str(s) for s in sources),*objects,'-lm','-o',str(binary)],cwd=ROOT,check=True)
    env=os.environ.copy()
    env.setdefault('ASAN_OPTIONS','detect_leaks=1:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS','halt_on_error=1:print_stacktrace=1')
    subprocess.run([str(binary)],cwd=ROOT,env=env,check=True)

if __name__=='__main__':main()
