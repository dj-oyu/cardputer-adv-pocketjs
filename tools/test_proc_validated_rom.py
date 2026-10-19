"""Real QuickJS adapter: OFF/reference/validated, synthetic boundary vectors."""
from pathlib import Path
import argparse
import json
import random
import sys
import shutil
sys.path.insert(0,str(Path(__file__).resolve().parent/'kasane_contract'))
import run_pocket_proc_rom_qjs as rom

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,default=rom.ROOT/'.cache/proc_validated_rom')
    args=parser.parse_args();rom.OUT=args.out.resolve();rom.OUT.mkdir(parents=True,exist_ok=True)
    table,shipped=rom.generate();rows=json.loads((rom.OUT/'rom_derby.json').read_text())
    rng=random.Random(4242);cases=[]
    arguments={'stands':[3,2,3,3],'crowd':[4,3,4,5,5,7,.25,2],
               'runner':[0xdd8c,0xffff,0x7bef],'pt':[0x36c0]}
    for row in rows:
        name=row['name'].split('.',1)[1]
        vectors=[[0]*8,[1]*8,[3,5,120,-4,5,1,2,80]]+[
            [rng.uniform(-1000,1000) if i%5==0 else rng.randint(-4,12) for _ in range(8)] for i in range(50)]
        cases.append(dict(plan=name,tag='synthetic-bounds',args=arguments.get(name,[]),inputs=vectors))
    path=rom.OUT/'synthetic-cases.json';path.write_text(json.dumps(cases)+'\n')
    cc=shutil.which('cc') or shutil.which('gcc');objects=rom.objects(cc);results={}
    for mode,defs in [('OFF',[]),('REFERENCE',['-DKSN_PROC_VALIDATED_PLAN','-DKSN_PROC_VALIDATED_REFERENCE']),('ON',['-DKSN_PROC_VALIDATED_PLAN'])]:
        for backend in ('scalar','fake_pie'):
            flags=defs+(['-D__XTENSA__','-DCONFIG_IDF_TARGET_ESP32S3=1','-DKSN_PROC_HOST_FAKE_PIE'] if backend=='fake_pie' else [])
            result=rom.build_and_run(cc,objects,table,shipped,path,mode+'-'+backend,flags,
                [] if backend=='fake_pie' else [rom.KASANE/'ksn_proc_points_pie.c'],rom.KASANE/'ksn_proc_plan.c',None)
            log=result.stdout+result.stderr;(rom.OUT/(mode+'-'+backend+'.log')).write_text(log)
            if result.returncode:raise RuntimeError(log[-6000:])
            assert 'PASS built-in plans by name' in log
            if mode!='OFF':assert 'VALIDATED_NATIVE_PASS' in log
            results[mode+'-'+backend]=log;print(mode,backend,'PASS',flush=True)
    (rom.OUT/'result.json').write_text(json.dumps(dict(scope='Synthetic native contract vectors, not game capture or device evidence',
        plans=len(cases),vectors=sum(len(c['inputs']) for c in cases),results=results),indent=2)+'\n')

if __name__=='__main__':main()
