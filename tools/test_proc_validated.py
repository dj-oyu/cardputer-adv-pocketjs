"""P1 contracts and separate host setup/execute probe; no device actions."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
BASE_TREE='9e80c14dfe141dcdec8bdf7ca2121c5b5e7bae66'

def run(command,**kwargs):
    result=subprocess.run(command,cwd=ROOT,text=True,capture_output=True,**kwargs)
    if result.returncode:raise RuntimeError(' '.join(map(str,command))+'\n'+result.stdout+result.stderr)
    return result.stdout

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,default=ROOT/'.cache/proc_validated_contract')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    headers=['components/quickjs-ng/quickjs-ng','components/pocketjs_guest/include','tools/hostshim',
             'main','main/pocket','main/ui','main/ui/kasane','main/text','main/hal']
    equality={}
    cmake=(ROOT/'main/CMakeLists.txt').read_text()
    fragment=cmake[cmake.index('option(KSN_PROC_VALIDATED_PLAN '):cmake.index('set(POCKET_RANDOM_SOURCES')]
    with tempfile.TemporaryDirectory() as tmp:
        source=Path(tmp)
        configurations=[({},True),({'KSN_PROC_VALIDATED_PLAN':'ON'},True),
            ({'KSN_PROC_VALIDATED_PLAN':'ON','KSN_PROC_VALIDATED_REFERENCE':'ON'},True),
            ({'KSN_PROC_VALIDATED_REFERENCE':'ON'},False)]+[
            ({'KSN_PROC_VALIDATED_PLAN':'ON',name:'ON'},False) for name in
            ('POCKET_DERBY_RUNNER_AFFINE','POCKET_DERBY_COURSE_CACHE','POCKET_DERBY_PROJECT_BATCH',
             'POCKET_DERBY_CLIP_NORM','POCKET_DERBY_RUNNER_BATCH')]
        for i,(values,accepted) in enumerate(configurations):
            (source/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.22)\nproject(guards NONE)\n'+
                ''.join(f'set({k} {v})\n' for k,v in values.items())+fragment)
            result=subprocess.run(['cmake','-S',str(source),'-B',str(source/f'build-{i}')],text=True,capture_output=True)
            assert (result.returncode==0)==accepted,(values,result.stdout,result.stderr)
            if not accepted:assert 'requires' in result.stderr
    with tempfile.TemporaryDirectory() as tmp:
        for name in ('main/ui/kasane/ksn_procedural.c','main/ui/kasane/ksn_proc_plan.c','main/pocket/pocket_proc.c'):
            baseline=Path(tmp)/Path(name).name
            baseline.write_bytes(subprocess.check_output(['git','show',BASE_TREE+':'+name],cwd=ROOT))
            objects=[]
            for i,source in enumerate((baseline,ROOT/name)):
                obj=out/(Path(name).stem+f'-{i}.o')
                run(['cc','-std=gnu11','-O2','-fno-ident','-ffp-contract=off','-DQUICKJS_NG_BUILD','-D_GNU_SOURCE',
                     *['-I'+str(ROOT/h) for h in headers],'-c',str(source),'-o',str(obj)])
                objects.append(obj.read_bytes())
            assert objects[0]==objects[1],name+' OFF object changed'
            equality[name]=dict(bytes=len(objects[0]),sha256=hashlib.sha256(objects[0]).hexdigest())
    sources=['main/ui/kasane/ksn_procedural.c','main/ui/kasane/ksn_proc_analysis.c','main/ui/kasane/ksn_proc_plan.c']
    results={}
    for mode,flags in [('optimized',['-O2','-fstrict-aliasing']),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        for enabled in (False,True):
            for name in ('procedural','proc_plan'):
                binary=out/f'{name}-{mode}-{enabled}'
                run(['cc','-std=c11','-Wall','-Wextra','-Werror',*flags,
                     *(['-DKSN_PROC_VALIDATED_PLAN'] if enabled else []),'-Imain/ui/kasane',
                     *(sources if name=='proc_plan' else sources[:1]),f'tools/kasane_contract/test_{name}.c','-lm','-o',str(binary)])
                results[binary.name]=run([str(binary)])
        binary=out/f'validated-{mode}'
        run(['cc','-std=c11','-Wall','-Wextra','-Werror',*flags,'-DKSN_PROC_VALIDATED_PLAN',
             '-Imain/ui/kasane',*sources,'tools/kasane_contract/test_proc_validated.c','-lm','-o',str(binary)])
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        if mode=='sanitized':env['PROC_SKIP_COSTS']='1'
        results[binary.name]=run([str(binary)],env=env)
        assert 'PROC_VALIDATED_PASS' in results[binary.name]
        print(results[binary.name],end='')
    costs=[json.loads(line.split(' ',1)[1]) for line in results['validated-optimized'].splitlines() if line.startswith('PROC_SETUP_COST ')]
    (out/'result.json').write_text(json.dumps(dict(base_tree=BASE_TREE,cmake_configurations=len(configurations),off_objects=equality,contracts=results,
        setup_execute_probe=costs,scope='Host-only functional/phase probe. No target speed, heap or adoption acceptance.'),indent=2)+'\n')
    print('PASS OFF objects / snapshot APIs OFF+ON / validated lifecycle, resume, failure snapshots and caps')

if __name__=='__main__':main()
