"""Built-in (flash) plans registered by name, against the same plans as
arrays, on the real QuickJS (docs/kasane/flash-plan.md). WSL/Linux, with node
on PATH (run under bash -lc).

  python3 tools/kasane_contract/run_pocket_proc_rom_qjs.py [--cases CASES.json] [--mutate]

1. node tools/kasane_ir/emit_rom_plans.mjs writes the built-in table of
   apps/derby/derby_prog.js's @plan functions (the firmware's form), and
   lower_plans.mjs the same file as the firmware ships it (packed plans and
   their decoder): the test registers every plan both ways.
2. The cases are DERBY's captured draws and their perturbations
   (.cache/kasane_ir/cases.json, as tools/kasane_ir/check_equivalence.py
   writes it; captured here first when missing, or pass --cases).
3. test_pocket_proc_rom_qjs.c is built with ASan/UBSan and the allocation hook,
   twice (the host's scalar points and a fake PIE backend), and run.
--mutate then removes one bound at a time (the argument count, the entry's
instruction count, the plan's byte count) and requires ASan to stop the run:
the checks are what keeps those paths inside their memory.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
QJS=ROOT/'components/quickjs-ng/quickjs-ng'
OUT=Path(os.environ.get('ROM_QJS_OUT',ROOT/'.cache/kasane_pocket_proc_rom_qjs'))
DEFS=['-DQUICKJS_NG_BUILD','-D_GNU_SOURCE']
KASANE=ROOT/'main/ui/kasane'

MUTATIONS=[
    ('the argument count',ROOT/'main/pocket/pocket_proc.c',
     'if(given!=rom->params)','if(given>KSN_PROC_ROM_PARAMS)'),
    ("the entry's instruction count",KASANE/'ksn_proc_plan.c',
     'if(!rom||!rom->code||!rom->count||rom->count>KSN_PROC_CODE||',
     'if(!rom||!rom->code||!rom->count||'),
    ("the plan's byte count",KASANE/'ksn_proc_plan.c',
     'offsetof(ksn_proc_rom_plan,args)+params*sizeof(float);',
     'offsetof(ksn_proc_rom_plan,args)+(params?params-1:0)*sizeof(float);'),
]

def generate():
    OUT.mkdir(parents=True,exist_ok=True)
    app=ROOT/'apps/derby/derby_prog.js'
    table=OUT/'rom_derby.c';shipped=OUT/'derby_prog_lowered.js'
    subprocess.run(['node',str(ROOT/'tools/kasane_ir/emit_rom_plans.mjs'),str(table),f'derby={app}',
                    '--json',str(OUT/'rom_derby.json')],cwd=ROOT,check=True)
    # The array path's rows: the same @plan functions packed, from a copy
    # without the rom mark (DERBY ships them as the built-in table).
    packed=OUT/'derby_prog.js'
    packed.write_text(app.read_text(encoding='utf-8').replace('@planDecoder rom */','@planDecoder */'),
                      encoding='utf-8')
    subprocess.run(['node',str(ROOT/'tools/kasane_ir/lower_plans.mjs'),'--file',str(packed),str(shipped)],
                   cwd=ROOT,check=True)
    return table,shipped

def objects(cc):
    objs=[]
    for name in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):
        src=QJS/f'{name}.c';obj=OUT/f'{name}.o'
        if not obj.exists() or src.stat().st_mtime>obj.stat().st_mtime:
            subprocess.run([cc,'-std=gnu11','-c','-O1','-g','-w',*DEFS,'-I',str(QJS),
                            str(src),'-o',str(obj)],cwd=ROOT,check=True)
        objs.append(str(obj))
    return objs

def build_and_run(cc,objs,table,shipped,cases,label,flags,extra,plan_source,include_first):
    sources=[KASANE/'ksn_procedural.c',KASANE/'ksn_proc_analysis.c',plan_source,
             KASANE/'ksn_proc_plan_points.c',KASANE/'ksn_proc_points.c',
             KASANE/'ksn_proc_points_dispatch.c',table,
             ROOT/'tools/kasane_contract/test_pocket_proc_rom_qjs.c',*extra]
    binary=OUT/f'test_pocket_proc_rom_{label}'
    command=[cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined','-fno-omit-frame-pointer',*DEFS,*flags,
             '-DKSN_PROC_HOST_TEST','-include',str(ROOT/'tools/kasane_contract/proc_alloc_hook.h'),
             *(['-I',str(include_first)] if include_first else []),
             '-I',str(QJS),'-I',str(ROOT/'main'),'-I',str(ROOT/'main/pocket'),'-I',str(KASANE),
             '-I',str(ROOT/'tools/kasane_contract'),'-I',str(ROOT/'tools/hostshim'),
             *(str(s) for s in sources),*objs,'-Wl,--wrap=ksn_proc_run','-lm','-o',str(binary)]
    subprocess.run(command,cwd=ROOT,check=True)
    env=os.environ.copy()
    env.setdefault('ASAN_OPTIONS','detect_leaks=1:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS','halt_on_error=1:print_stacktrace=1')
    return subprocess.run([str(binary),str(shipped),str(cases)],cwd=ROOT,env=env,
                          capture_output=True,text=True)

def main():
    ap=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cases',type=Path,default=ROOT/'.cache/kasane_ir/cases.json')
    ap.add_argument('--mutate',action='store_true')
    a=ap.parse_args()
    if sys.platform=='win32':raise SystemExit('ASan/LeakSanitizer: run under WSL (bash -lc)')
    if not a.cases.exists():
        # check_equivalence.py's capture (the -m32 game at three tiers, about
        # three minutes once) and its case writer, without its hand-IR check.
        sys.path.insert(0,str(ROOT/'tools/kasane_ir'))
        import check_equivalence as ce
        apps=ROOT/'apps/derby'
        draws=ce.capture(apps)
        subprocess.run(['node',str(ROOT/'tools/kasane_ir/derby_plans.mjs'),'--prog',str(ce.text_prog(apps)),
                        '--js',str(apps/'derby_prog.js'),'--cases',str(draws),
                        str(a.cases.with_suffix('.txt'))],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
    cc=shutil.which('cc') or shutil.which('gcc')
    if not cc:raise SystemExit('No host C compiler found')
    table,shipped=generate()
    objs=objects(cc)
    arms=[('scalar',[],[KASANE/'ksn_proc_points_pie.c']),
          ('fake_pie',['-D__XTENSA__','-DCONFIG_IDF_TARGET_ESP32S3=1','-DKSN_PROC_HOST_FAKE_PIE'],[])]
    for label,flags,extra in arms:
        r=build_and_run(cc,objs,table,shipped,a.cases,label,flags,extra,KASANE/'ksn_proc_plan.c',None)
        print(r.stdout,end='')
        if r.returncode:raise SystemExit(f'{label}: failed\n{r.stderr[-4000:]}')
    if not a.mutate:return
    mut=OUT/'mutant';mut.mkdir(exist_ok=True)
    for what,source,old,new in MUTATIONS:
        text=source.read_text()
        if text.count(old)!=1:raise SystemExit(f'{source.name}: mutation site for {what} moved')
        copy=mut/source.name
        copy.write_text(text.replace(old,new))
        plan=copy if source.name=='ksn_proc_plan.c' else KASANE/'ksn_proc_plan.c'
        first=mut if source.name=='pocket_proc.c' else None
        r=build_and_run(cc,objs,table,shipped,a.cases,'mutant',[],[KASANE/'ksn_proc_points_pie.c'],plan,first)
        copy.unlink()
        caught='AddressSanitizer' in r.stderr
        kind=next((l.split('ERROR: AddressSanitizer: ')[1].split(' ')[0] for l in r.stderr.splitlines()
                   if 'ERROR: AddressSanitizer: ' in l),'-')
        print(f'mutation without {what}: {"stopped by ASan ("+kind+")" if caught else "NOT CAUGHT"}')
        if r.returncode==0 or not caught:raise SystemExit(f'mutation without {what} was not caught\n{r.stdout[-2000:]}')
    print('MUTATIONS CAUGHT: every bound removed alone ends the run in ASan')

if __name__=='__main__':main()
