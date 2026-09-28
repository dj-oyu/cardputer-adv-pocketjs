"""Build and run the procedural limits/unregister contract on real QuickJS.

QuickJS is compiled once without sanitizers and cached (vendor code, and its
objects are reused between runs). The adapter, the procedural VM and the test
are compiled with ASan/UBSan, so LeakSanitizer decides whether unregister()
and reset() release every plan and point allocation exactly once. Two builds
run: the host scalar backend and a fake PIE backend that takes the dispatch's
PIE arm for batches of eight or more points.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
QJS=ROOT/'components/quickjs-ng/quickjs-ng'
OUT=ROOT/'.cache/kasane_pocket_proc_limits_qjs'
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
    sources=[kasane/'ksn_procedural.c',kasane/'ksn_proc_analysis.c',
             kasane/'ksn_proc_plan.c',kasane/'ksn_proc_plan_points.c',
             kasane/'ksn_proc_points.c',kasane/'ksn_proc_points_dispatch.c',
             ROOT/'main/pocket/pocket_proc.c',
             ROOT/'tools/kasane_contract/test_pocket_proc_limits_qjs.c']
    base=[cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',*sanitize,*DEFS,
          '-DKSN_PROC_HOST_TEST','-I',str(QJS),'-I',str(ROOT/'main'),
          '-I',str(ROOT/'main/pocket'),'-I',str(kasane),
          '-I',str(ROOT/'tools/kasane_contract'),'-I',str(ROOT/'tools/hostshim')]
    arms=[('scalar',[],[kasane/'ksn_proc_points_pie.c']),
          ('fake_pie',['-D__XTENSA__','-DCONFIG_IDF_TARGET_ESP32S3=1',
                       '-DKSN_PROC_HOST_FAKE_PIE'],[])]
    env=os.environ.copy()
    env.setdefault('ASAN_OPTIONS','detect_leaks=1:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS','halt_on_error=1:print_stacktrace=1')
    # The turn contract (parked frame, limit before allocation, per-count
    # points) is its own binary: it force-includes an allocation hook that
    # must not reach the limits test's own leak accounting.
    turn=[*sources[:-1],ROOT/'tools/kasane_contract/test_pocket_proc_turn_qjs.c']
    hook=['-include',str(ROOT/'tools/kasane_contract/proc_alloc_hook.h')]
    for label,flags,extra in arms:
        for name,srcs,more in (('limits',sources,[]),('turn',turn,hook)):
            binary=OUT/f'test_pocket_proc_{name}_{label}'
            subprocess.run([*base,*flags,*more,*(str(s) for s in [*srcs,*extra]),*objects,
                            '-lm','-o',str(binary)],cwd=ROOT,check=True)
            subprocess.run([str(binary)],cwd=ROOT,env=env,check=True)

if __name__=='__main__':main()
